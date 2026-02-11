#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video-info.h>

#include <CLI/CLI.hpp>
#include <csignal>
#include <filesystem>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.h"
#include "server.h"
#include "xdaqmetadata/metadata_handler.h"
#include "xvc.h"

namespace fs = std::filesystem;

namespace
{

GMainLoop *loop = nullptr;
GstElement *pipeline = nullptr;
bool record = false;

std::unique_ptr<Camera> stream_cam = nullptr;
std::unique_ptr<MetadataHandler> handler = nullptr;
std::chrono::steady_clock::time_point stream_duration;

enum class Codec : int { MJPEG };
enum class TimeUnit : int { Seconds, Minutes, Hours, Days };

GstFlowReturn draw_image(GstAppSink *sink, [[maybe_unused]] void *user_data)
{
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sample(
        gst_app_sink_pull_sample(sink), gst_sample_unref
    );
    if (!sample) return GST_FLOW_OK;

    auto buffer = gst_sample_get_buffer(sample.get());
    std::unique_ptr<GstVideoInfo, decltype(&gst_video_info_free)> video_info(
        gst_video_info_new(), gst_video_info_free
    );
    if (!gst_video_info_from_caps(video_info.get(), gst_sample_get_caps(sample.get()))) {
        std::println("Failed to parse video info");
        return GST_FLOW_ERROR;
    }

    const auto caps = gst_sample_get_caps(sample.get());
    const auto structure = gst_caps_get_structure(caps, 0);
    const auto width =
        static_cast<int>(g_value_get_int(gst_structure_get_value(structure, "width")));
    const auto height =
        static_cast<int>(g_value_get_int(gst_structure_get_value(structure, "height")));
    const auto buffer_pts = GST_BUFFER_PTS(buffer);

    auto xdaqmetadata = handler->_safe_queue.dequeue(buffer_pts);
    if (!xdaqmetadata) {
        std::println("Failed to dequeue XDAQ metadata from buffer with PTS {}", buffer_pts);
        return GST_FLOW_OK;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stream_duration).count();

    auto hours = elapsed / 3600;
    auto minutes = (elapsed % 3600) / 60;
    auto seconds = elapsed % 60;

    std::println(
        "Received buffer: size={}, width={}, height={}, PTS={}, "
        "fpga_timestamp={}, time={:02}:{:02}:{:02}",
        gst_buffer_get_size(buffer),
        width,
        height,
        buffer_pts,
        xdaqmetadata->fpga_timestamp,
        hours,
        minutes,
        seconds
    );

    return GST_FLOW_OK;
}

void handle_sigint(int)
{
    std::println("SIGINT received, stopping stream...");
    if (record) {
        xvc::stop_jpeg_recording(GST_PIPELINE(pipeline));
    }
    if (stream_cam) {
        stream_cam->stop();
    }
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    if (loop) {
        g_main_loop_quit(loop);
    }
}

}  // namespace

int func(int argc, char *argv[])
{
    CLI::App app("Thor Vision CLI", "tvcli");
    argv = app.ensure_utf8(argv);

    std::string host = "192.168.177.100";
    int id;
    std::string gst_cap;
    Codec codec{Codec::MJPEG};
    // TODO
    std::unordered_map<std::string, Codec> codec_map{{"mjpeg", Codec::MJPEG}};

    std::string location = "records";
    auto split = false;
    auto max_size_time = 10;
    std::string log_file;
    TimeUnit time_unit{TimeUnit::Seconds};
    std::unordered_map<std::string, TimeUnit> time_unit_map = {
        {"seconds", TimeUnit::Seconds},
        {"minutes", TimeUnit::Minutes},
        {"hours", TimeUnit::Hours},
        {"days", TimeUnit::Days}
    };

    auto stream = app.add_subcommand("stream", "Stream camera");
    stream->add_option("--host", host, "Host computer that connected cameras")
        ->default_val(host)
        ->group("Stream");
    stream->add_option("-i,--id", id, "Camera device ID")->required()->group("Stream");
    stream->add_option("--cap", gst_cap, "Camera capability")->required()->group("Stream");
    stream->add_option("--codec", codec, "Camera codec")
        ->required()
        ->transform(CLI::CheckedTransformer(codec_map, CLI::ignore_case))
        ->group("Stream");

    auto opt_record =
        stream->add_flag("-r,--record", record, "Whether to record stream")->group("Record");
    auto opt_location = stream->add_option("--location", location, "Location to save record")
                            ->default_val(location)
                            ->group("Record");
    auto opt_split = stream->add_flag("-s,--split", split, "Enable split recording")
                         ->default_val(split)
                         ->group("Record");
    auto opt_max_size_time =
        stream
            ->add_option("--max-size-time", max_size_time, "Max recording time per file (seconds)")
            ->default_val(10)
            ->group("Split");
    auto opt_time_unit =
        stream->add_option("--time-unit", time_unit, "Time unit for recording split size")
            ->transform(CLI::CheckedTransformer(time_unit_map, CLI::ignore_case))
            ->group("Split");

    opt_location->needs(opt_record);
    opt_split->needs(opt_record);
    opt_max_size_time->needs(opt_split);
    opt_time_unit->needs(opt_split);

    auto list = app.add_subcommand("list", "List cameras");
    list->add_option("--host", host, "Host computer that connected cameras")->default_val(host);

    auto logs = app.add_subcommand("logs", "Show server logs");
    logs->add_option("--host", host, "Host computer that connected cameras")->default_val(host);
    logs->add_option("-f,--file", log_file, "Log file to view");

    CLI11_PARSE(app, argc, argv);

    signal(SIGINT, handle_sigint);
    gst_init(&argc, &argv);

    if (*stream) {
        handler = std::make_unique<MetadataHandler>();
        pipeline = gst_pipeline_new(nullptr);
        loop = g_main_loop_new(nullptr, false);
        stream_duration = std::chrono::steady_clock::now();

        for (auto &camera : Camera::cameras()) {
            if (id == camera->id()) {
                stream_cam = std::move(camera);
                break;
            }
        }
        if (!stream_cam) {
            std::println("Error: no camera with id = {}", id);
            return -1;
        }

        auto caps = stream_cam->caps();
        auto it = std::find_if(caps.begin(), caps.end(), [gst_cap](const Camera::Cap &_cap) {
            return _cap.to_string() == gst_cap;
        });
        if (it == caps.end()) {
            std::println("Error: Camera {} does not support cap '{}'", id, gst_cap);
            return -1;
        }
        stream_cam->start(*it);

        std::chrono::seconds duration;
        switch (time_unit) {
        case TimeUnit::Seconds: duration = std::chrono::seconds(max_size_time); break;
        case TimeUnit::Minutes: duration = std::chrono::minutes(max_size_time); break;
        case TimeUnit::Hours: duration = std::chrono::hours(max_size_time); break;
        case TimeUnit::Days: duration = std::chrono::days(max_size_time); break;
        default: std::println("Invalid time unit specified."); return -1;
        }

        auto uri = std::format("{}:{}", host, stream_cam->port());

        if (codec == Codec::MJPEG) {
            xvc::setup_jpeg_srt_stream(GST_PIPELINE(pipeline), uri);
        }

        if (record) {
            const auto &base = (location == "records") ? fs::path("records") : fs::path(location);
            std::error_code ec;
            if (!fs::exists(base) && !fs::create_directories(base, ec)) {
                std::println(
                    "Error: cannot create directory '{}': {}", base.string(), ec.message()
                );
                return -1;
            }
            auto filepath = base / stream_cam->name();
            xvc::RecordConfig config(filepath, split, duration);
            xvc::start_jpeg_recording(GST_PIPELINE(pipeline), config);
        }

        auto parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser");
        std::unique_ptr<GstPad, decltype(&gst_object_unref)> src_pad(
            gst_element_get_static_pad(parser, "src"), gst_object_unref
        );
        gst_pad_add_probe(
            src_pad.get(), GST_PAD_PROBE_TYPE_BUFFER, parse_jpeg_metadata, handler.get(), nullptr
        );

        GstAppSinkCallbacks callbacks = {nullptr, nullptr, draw_image, nullptr, nullptr, {nullptr}};
        auto appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink");
        gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, nullptr, nullptr);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            std::println("Unable to set the pipeline to the playing state");
            return -1;
        }

        g_main_loop_run(loop);

        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
        stream_cam.reset();
        handler.reset();
    }

    if (*list) {
        std::println("Discovered Cameras:");
        for (const auto &camera : Camera::cameras()) {
            std::println("");
            std::println("Camera ID   : {}", camera->id());
            std::println("Device ID   : {}", camera->device_id());
            std::println("Name        : {}", camera->name());

            std::println("Capabilities:");
            for (const auto &cap : camera->caps()) {
                std::println("  - {}", cap.to_string());
            }
        }
    }

    if (*logs) {
        auto server = xvc::Server();
        if (auto logs = log_file.empty() ? server.logs() : server.logs(log_file)) {
            std::println("{}", logs.value());
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
#if defined(__APPLE__) && TARGET_OS_MAC && !TARGET_OS_IPHONE
    return gst_macos_main((GstMainFunc) func, argc, argv, nullptr);
#else
    return func(argc, argv);
#endif
}