import subprocess
import re
import os
import time
import click

FPS_PATTERN = re.compile(r"fps(\d+):.*average:\s*([\d.]+)")

CODEC_MAP = {
    "jpeg": {"encoder": "jpegenc", "decoder": "jpegdec"},
    "h264": {
        "encoder": "x264enc speed-preset=ultrafast tune=zerolatency",
        "decoder": "avdec_h264",
    },
    "h265": {
        "encoder": "x265enc speed-preset=ultrafast tune=zerolatency",
        "decoder": "avdec_h265",
    },
}


def get_codec_elements(codec):
    if codec not in CODEC_MAP:
        raise ValueError(f"Unsupported codec: {codec}")
    return CODEC_MAP[codec]["encoder"], CODEC_MAP[codec]["decoder"]


def run_decoder_pipeline(
    resolution,
    instances,
    codec,
    log_path,
    mode,
    video_path=None,
):
    encoder, decoder = get_codec_elements(codec)

    decode_branches = [
        f"t. ! queue name=dec{i} ! {decoder} ! "
        f"fpsdisplaysink name=fps{i} text-overlay=false video-sink=fakesink sync=false"
        for i in range(instances)
    ]

    if mode == "file":
        pipeline = f"filesrc location={video_path} ! " f"tee name=t " + " ".join(
            decode_branches
        )
    else:
        width, height = resolution.lower().split("x")
        pipeline = (
            f"videotestsrc is-live=false pattern=1 ! "
            f"video/x-raw,width={width},height={height} ! {encoder} ! "
            f"tee name=t " + " ".join(decode_branches)
        )

    cmd = f"gst-launch-1.0 -v {pipeline}"

    with open(log_path, "w") as logfile:
        process = subprocess.Popen(
            cmd,
            shell=True,
            stdout=logfile,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            print("Pipeline timeout. Terminating...")
            process.terminate()
            process.wait()


def analyze_fps_log(log_path, instances, expected_fps):
    results = {}

    with open(log_path, "r") as log_file:
        for line in reversed(log_file.readlines()):
            match = FPS_PATTERN.search(line)
            if match:
                idx, fps = int(match.group(1)), float(match.group(2))
                if idx not in results:
                    results[idx] = fps
                if len(results) == instances:
                    break

    all_ok = all(results.get(i, 0) >= expected_fps for i in range(instances))
    return all_ok, results


def run_resolution_test(
    resolution,
    expected_fps,
    codec,
    max_instances,
    mode,
    video_path,
    run_dir,
    label="",
):
    for instances in range(1, max_instances + 1):
        log_name = (
            f"decode_{label}_{instances}.log"
            if label
            else f"decode_{resolution.replace('x', '_')}_{instances}.log"
        )
        log_path = os.path.join(run_dir, log_name)

        run_decoder_pipeline(resolution, instances, codec, log_path, mode, video_path)
        all_ok, fps_data = analyze_fps_log(log_path, instances, expected_fps)

        print(f"\n--- {resolution} | {instances} instance(s) ---")
        for i in range(instances):
            avg_fps = fps_data.get(i)
            if avg_fps is not None:
                status = "OK" if avg_fps >= expected_fps else "SLOW"
                print(
                    f"Pipeline {i}: average FPS = {avg_fps:.2f} >= {expected_fps:.2f} => {status}"
                )
            else:
                print(f"Pipeline {i}: No FPS data found")

        if not all_ok:
            print(
                f"\nMax sustainable instances for {label or resolution} at {expected_fps:.2f} FPS: {instances - 1}"
            )
            return instances - 1

        instances += 1


def auto_scale_test(
    resolutions,
    expected_fps=30,
    codec="jpeg",
    max_instances=64,
    mode="encode",
    video_path=None,
):

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    run_dir = f"test_logs_{timestamp}"
    os.makedirs(run_dir, exist_ok=True)

    if mode == "file":
        print(f"\n--- Testing file input: {video_path} ---")
        resolution = resolutions[0]
        run_resolution_test(
            resolution,
            expected_fps,
            codec,
            max_instances,
            mode,
            video_path,
            run_dir,
            label="file",
        )
    else:
        for resolution in resolutions:
            print(f"\n--- Testing resolution: {resolution} ---")
            run_resolution_test(
                resolution,
                expected_fps,
                codec,
                max_instances,
                mode,
                video_path,
                run_dir,
            )


@click.command()
@click.argument("mode", default="encode", type=click.Choice(["encode", "file"]))
@click.argument(
    "resolutions",
    nargs=-1,
    required=False,
)
@click.argument("fps", default=30, type=int)
@click.option(
    "-c",
    "--codec",
    default="jpeg",
    type=click.Choice(["jpeg", "h264", "h265"]),
    help="Codec to use.",
)
@click.argument("max-instances", default=64, type=int)
@click.option("-p", "--path", default=None, help="Path to video file (file mode only).")
def main(mode, resolutions, fps, codec, max_instances, path):
    """
    Benchmark max sustainable decode branches at a given FPS.

    \b
    MODE: 'encode' or 'file'
    RESOLUTIONS: e.g., 1920x1080 1280x720 (for 'encode' mode)
    FPS: Expected average FPS per pipeline
    INSTANCES: Maximum number of decode branches to test
    """

    if mode == "file":
        if not path:
            raise click.UsageError("In 'file' mode, --path is required.")
        resolution_list = ["dummy"]
    else:
        if not resolutions:
            raise click.UsageError("In 'encode' mode, --resolutions is required.")
        resolution_list = list(resolutions)

    auto_scale_test(
        resolution_list,
        fps,
        codec,
        max_instances,
        mode,
        path,
    )


if __name__ == "__main__":
    main()
