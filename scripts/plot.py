import os
import re
import click
import matplotlib.pyplot as plt
from collections import defaultdict


FPS_PATTERN = re.compile(r"fps(\d+):.*average:\s*([\d.]+)")


def parse_filename(filename):
    match = re.match(r"decode_(\d+)_(\d+)_(\d+)\.log", filename)
    if match:
        width, height, instances = match.groups()
        resolution = f"{width}x{height}"
        return resolution, int(instances)
    return None, None


def parse_fps_from_log(filepath):
    results = {}

    with open(filepath, "r") as f:
        for line in f:
            match = FPS_PATTERN.search(line)
            if match:
                idx, fps = int(match.group(1)), float(match.group(2))
                results[idx] = fps

    return sum(results.values()) / len(results) if results else None


def collect_log_data(folder_path):
    data = defaultdict(list)

    for filename in os.listdir(folder_path):
        if filename.startswith("decode_") and filename.endswith(".log"):
            resolution, instances = parse_filename(filename)
            if resolution:
                filepath = os.path.join(folder_path, filename)
                avg_fps = parse_fps_from_log(filepath)
                if avg_fps is not None:
                    data[resolution].append((instances, avg_fps))

    for resolution in data:
        data[resolution].sort()

    return data


def plot_fps_data(data, log_scale=True):
    plt.figure(figsize=(12, 7))

    for resolution, values in data.items():
        x = [inst for inst, _ in values]
        y = [fps for _, fps in values]
        plt.plot(x, y, marker="o", label=resolution)

        for inst, fps in values:
            plt.annotate(
                f"{inst}x\n{fps:.1f} FPS",
                (inst, fps),
                textcoords="offset points",
                xytext=(0, 5),
                ha="center",
                fontsize=8,
            )

    for line in [30, 60, 120]:
        plt.axhline(y=line, color="gray", linestyle="--", linewidth=1)
        plt.text(plt.xlim()[0], line * 1.05, f"{line} FPS", color="gray", fontsize=9)

    plt.xlabel("Decoder Instances")
    plt.ylabel("Average FPS (log scale)" if log_scale else "Average FPS")
    if log_scale:
        plt.yscale("log")
    plt.title("Decoder Performance vs Instance Count")
    plt.legend()
    plt.grid(True, which="both", linestyle="--", linewidth=0.5)
    plt.tight_layout()
    plt.show()


@click.command()
@click.argument("folder", type=click.Path(exists=True, file_okay=False))
@click.option("--linear", is_flag=True, help="Use linear scale for FPS (default: log).")
def main(folder, linear):
    """
    Visualize decoder performance logs in FOLDER.

    \b
    FOLDER: Path to the folder containing average FPS log files.
    """
    data = collect_log_data(folder)
    if not data:
        click.echo("No valid log files found.")
        return

    plot_fps_data(data, log_scale=not linear)


if __name__ == "__main__":
    main()
