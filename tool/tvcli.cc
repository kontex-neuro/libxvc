#include <fmt/core.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <gst/gstpad.h>
#include <gst/gstpipeline.h>
#include <gst/video/video-info.h>
#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "camera.h"
#include "server.h"
#include "xdaqmetadata/metadata_handler.h"
#include "xvc.h"

using json = nlohmann::json;

namespace
{

GMainLoop *loop = nullptr;
GstElement *pipeline = nullptr;
MetadataHandler *handler = nullptr;
bool record = false;

Camera *stream_cam = nullptr;
std::vector<Camera *> cams;

GstFlowReturn draw_image(GstAppSink *sink, [[maybe_unused]] void *user_data)
{
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sample(
        gst_app_sink_pull_sample(sink), gst_sample_unref
    );
    if (!sample) return GST_FLOW_OK;

    auto buffer = gst_sample_get_buffer(sample.get());
    GstMapInfo info;
    if (gst_buffer_map(buffer, &info, GST_MAP_READ)) {
        std::unique_ptr<GstVideoInfo, decltype(&gst_video_info_free)> video_info(
            gst_video_info_new(), gst_video_info_free
        );
        if (!gst_video_info_from_caps(video_info.get(), gst_sample_get_caps(sample.get()))) {
            spdlog::critical("Failed to parse video info");
            gst_buffer_unmap(buffer, &info);
            return GST_FLOW_ERROR;
        }
        auto caps = gst_sample_get_caps(sample.get());
        auto structure = gst_caps_get_structure(caps, 0);
        auto width = static_cast<int>(g_value_get_int(gst_structure_get_value(structure, "width")));
        auto height =
            static_cast<int>(g_value_get_int(gst_structure_get_value(structure, "height")));
        auto buffer_pts = GST_BUFFER_PTS(buffer);

        auto xdaqmetadata = handler->safe_deque.check_pts_pop_timestamp(buffer_pts);
        auto metadata = xdaqmetadata.value_or(XDAQFrameData{0, 0, 0, 0, 0, 0});

        spdlog::info(
            "Received buffer: size={}, pts={}, width={}, height={}, "
            "fpga_timestamp={}, rhythm_timestamp={}, ttl_in={}, ttl_out={}, spi_perf_counter={}, "
            "reserved={}",
            gst_buffer_get_size(buffer),
            buffer_pts,
            width,
            height,
            metadata.fpga_timestamp,
            metadata.rhythm_timestamp,
            metadata.ttl_in,
            metadata.ttl_out,
            metadata.spi_perf_counter,
            metadata.reserved
        );
        gst_buffer_unmap(buffer, &info);
    }
    return GST_FLOW_OK;
}

void handle_sigint(int)
{
    spdlog::info("SIGINT received, stopping camera...");
    if (stream_cam) {
        stream_cam->stop();
    }
    if (record) {
        xvc::stop_jpeg_recording(GST_PIPELINE(pipeline));
    }
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    if (loop) {
        g_main_loop_quit(loop);
    }
    std::exit(EXIT_SUCCESS);
}

std::string cap_to_string(const Camera::Cap &cap)
{
    // Skip format for image/jpeg media type
    if (cap.format.empty()) {
        return fmt::format(
            "{},width={},height={},framerate={}/{}",
            cap.media_type,
            cap.width,
            cap.height,
            cap.fps_n,
            cap.fps_d
        );
    } else {
        return fmt::format(
            "{},format={},width={},height={},framerate={}/{}",
            cap.media_type,
            cap.format,
            cap.width,
            cap.height,
            cap.fps_n,
            cap.fps_d
        );
    }
}

std::vector<Camera *> cameras()
{
    auto const cameras_str = Camera::cameras();
    std::vector<Camera *> cams;

    if (cameras_str.empty()) {
        fmt::println("No camera found");
        return cams;
    }

    auto const cameras_json = json::parse(cameras_str);

    for (const auto &camera_json : cameras_json) {
        auto id = camera_json["id"].get<int>();
        auto name = camera_json["name"].get<std::string>();
        auto cam = new Camera(id, name);

        for (const auto &cap_json : camera_json["caps"]) {
            Camera::Cap cap;
            cap.media_type = cap_json["media_type"].get<std::string>();
            cap.format = cap_json["format"].get<std::string>();
            cap.width = cap_json["width"].get<int>();
            cap.height = cap_json["height"].get<int>();

            auto framerate_str = cap_json["framerate"].get<std::string>();
            auto delimiter_pos = framerate_str.find('/');
            if (delimiter_pos != std::string::npos) {
                cap.fps_n = std::stoi(framerate_str.substr(0, delimiter_pos));
                cap.fps_d = std::stoi(framerate_str.substr(delimiter_pos + 1));
            }
            cam->add_cap(cap);
        }
        cams.emplace_back(cam);
    }
    return cams;
}

}  // namespace

int func(int argc, char *argv[])
{
    CLI::App app("Thor Vision CLI", "tvcli");
    argv = app.ensure_utf8(argv);

    std::string host = "192.168.177.100";
    int id;
    std::string cap, codec;

    std::string location = ".";
    auto split = false;
    auto max_size_time = 5;
    auto max_files = 10;
    auto test = false;
    std::string log_file;
    std::string time_unit;

    auto stream = app.add_subcommand("stream", "Stream camera");
    stream->add_option("--host", host, "Host computer that connected cameras")
        ->default_val(host)
        ->group("Stream");
    stream->add_option("-i,--id", id, "Camera device ID")->required()->group("Stream");
    stream->add_option("--cap", cap, "Camera capability")->required()->group("Stream");
    stream->add_option("--codec", codec, "Camera codec")->required()->group("Stream");
    stream->add_flag("-t,--test", test, "Enable test mode")->default_val(test)->group("Stream");

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
            ->add_option("--max-size-time", max_size_time, "Max recording time per file (minutes)")
            ->default_val(5)
            ->group("Split");
    auto opt_time_unit =
        stream->add_option("--time-unit", time_unit, "Time unit for recording split size")
            ->check(CLI::IsMember({"seconds", "minutes", "hours", "days"}))
            ->default_val("minutes")
            ->group("Split");
    auto opt_max_files =
        stream->add_option("--max-files", max_files, "Maximum number of files to keep")
            ->default_val(10)
            ->group("Split");

    opt_location->needs(opt_record);
    opt_split->needs(opt_record);
    opt_max_size_time->needs(opt_split);
    opt_time_unit->needs(opt_split);
    opt_max_files->needs(opt_split);

    auto list = app.add_subcommand("list", "List cameras");
    list->add_option("--host", host, "Host computer that connected cameras")->default_val(host);

    auto logs = app.add_subcommand("logs", "Show server logs");
    logs->add_option("--host", host, "Host computer that connected cameras")->default_val(host);
    logs->add_option("-f,--file", log_file, "Log file to view");

    CLI11_PARSE(app, argc, argv);

    signal(SIGINT, handle_sigint);
    gst_init(&argc, &argv);

    xvc::TimeUnit unit;

    if (time_unit == "seconds")
        unit = xvc::TimeUnit::Seconds;
    else if (time_unit == "minutes")
        unit = xvc::TimeUnit::Minutes;
    else if (time_unit == "hours")
        unit = xvc::TimeUnit::Hours;
    else if (time_unit == "days")
        unit = xvc::TimeUnit::Days;
    else {
        fmt::println("Invalid time unit specified.");
        return EXIT_FAILURE;
    }

    if (*stream) {
        // TODO: support h264, h265
        auto valid_codecs = {"jpeg"};
        if (std::find(valid_codecs.begin(), valid_codecs.end(), codec) == valid_codecs.end()) {
            fmt::println("Invalid codec. Valid options is: jpeg.");
            return EXIT_FAILURE;
        }

        if (!test) {
            cams = cameras();
            for (auto cam : cams) {
                if (id == cam->id()) {
                    stream_cam = cam;
                    break;
                }
            }
            if (!stream_cam) {
                fmt::println("Error: no camera with id = {}", id);
                return EXIT_FAILURE;
            }

            auto caps = stream_cam->caps();
            auto it = std::find_if(caps.begin(), caps.end(), [cap](const Camera::Cap &_cap) {
                return cap_to_string(_cap) == cap;
            });
            if (it == caps.end()) {
                fmt::println("Error: Camera {} does not support cap '{}'", id, cap);
                return EXIT_FAILURE;
            }
        } else {
            stream_cam = new Camera(id, "test");
        }

        stream_cam->set_current_cap(cap);
        stream_cam->set_test(test);
        stream_cam->start();

        auto uri = fmt::format("{}:{}", host, stream_cam->port());
        auto record_path = std::filesystem::current_path();
        auto filepath = record_path / fmt::format("{}-{}", stream_cam->name(), stream_cam->id());

        if (location != ".") {
            record_path = fs::path(location);
            if (!fs::exists(record_path)) {
                fmt::println("Error: specified location path '{}' does not exist.", location);
                return EXIT_FAILURE;
            }
        }

        handler = new MetadataHandler();
        pipeline = gst_pipeline_new(codec.c_str());
        loop = g_main_loop_new(nullptr, false);

        if (codec == "jpeg") {
            xvc::setup_jpeg_srt_stream(GST_PIPELINE(pipeline), uri);
            if (record) {
                xvc::start_jpeg_recording(
                    GST_PIPELINE(pipeline), filepath, !split, max_size_time, unit, max_files
                );
            }
        }

        auto parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser");
        std::unique_ptr<GstPad, decltype(&gst_object_unref)> src_pad(
            gst_element_get_static_pad(parser, "src"), gst_object_unref
        );
        gst_pad_add_probe(
            src_pad.get(), GST_PAD_PROBE_TYPE_BUFFER, parse_jpeg_metadata, handler, nullptr
        );

        GstAppSinkCallbacks callbacks = {nullptr, nullptr, draw_image, nullptr, nullptr, {nullptr}};
        auto appsink = gst_bin_get_by_name(GST_BIN(pipeline), "appsink");
        gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, nullptr, nullptr);

        auto ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            spdlog::error("Unable to set the pipeline to the playing state");
            return EXIT_FAILURE;
        }

        auto _thread = std::jthread([]() {
            spdlog::debug("Run GStreamer stream thread");
            g_main_loop_run(loop);
            spdlog::debug("Quit GStreamer stream thread");
            delete stream_cam;
            delete handler;
            stream_cam = nullptr;
            handler = nullptr;
        });
    }

    if (*list) {
        cams = cameras();
        fmt::println("Discovered Cameras:");

        for (auto cam : cams) {
            fmt::println("");
            fmt::println("Camera ID    : {}", cam->id());
            fmt::println("Name         : {}", cam->name());
            fmt::println("Capabilities :");

            for (auto cap : cam->caps()) {
                fmt::println("  - {}", cap_to_string(cap));
            }
        }
    }

    if (*logs) {
        auto server = xvc::Server(host);
        auto logs = log_file.empty() ? server.logs() : server.logs(log_file);

        fmt::println("{}", logs);
    }

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
#if defined(__APPLE__) && TARGET_OS_MAC && !TARGET_OS_IPHONE
    return gst_macos_main((GstMainFunc) func, argc, argv, nullptr);
#else
    return func(argc, argv);
#endif
}