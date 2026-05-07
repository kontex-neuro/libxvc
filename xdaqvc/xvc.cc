#include "xvc.h"

#include <gst/gst.h>
#include <spdlog/spdlog.h>

#include <climits>
#include <memory>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace
{

struct FileTracker {
    std::string base_filepath;
    std::vector<fs::path> file_paths;
    int max_files;
};

gchararray generate_filename(GstElement *, guint, gpointer udata)
{
    auto tracker = static_cast<FileTracker *>(udata);
    if (!tracker) {
        spdlog::error("FileTracker is null");
        return nullptr;
    }

    const auto &now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto &timestamp = std::format("{:%Y-%m-%d_%H-%M-%S}", now);
    const auto &file_path = std::format("{}-{}.mkv", tracker->base_filepath, timestamp);

    tracker->file_paths.emplace_back(file_path);

    if (tracker->file_paths.size() > static_cast<size_t>(tracker->max_files)) {
        auto _file_path = tracker->file_paths.front();
        fs::remove(_file_path);
        spdlog::debug("Remove file: {}", _file_path.generic_string());

        _file_path.replace_extension(".bin");
        fs::remove(_file_path);
        spdlog::debug("Remove file: {}", _file_path.generic_string());

        tracker->file_paths.erase(tracker->file_paths.begin());
    }

    return g_strdup(file_path.c_str());
}

}  // namespace

namespace xvc
{


void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri)
{
    if (!pipeline) return;
    spdlog::info("Setup GStreamer M-JPEG SRT stream pipeline with uri: {}", uri);

    auto src = gst_element_factory_make("srtclientsrc", "src");
    auto parser = gst_element_factory_make("jpegparse", "parser");
    auto tee = gst_element_factory_make("tee", "t");
    auto queue_display = gst_element_factory_make("queue", "queue_display");
#ifdef _WIN32
    auto dec = gst_element_factory_make("jpegdec", "dec");
#elif __APPLE__
    auto dec = gst_element_factory_make("vtdec", "dec");
#else
    auto dec = gst_element_factory_make("jpegdec", "dec");
#endif
    auto conv = gst_element_factory_make("videoconvert", "conv");
    auto cf_conv = gst_element_factory_make("capsfilter", "cf_conv");
    auto fpsdisplaysink = gst_element_factory_make("fpsdisplaysink", "fpsdisplaysink");
    auto appsink = gst_element_factory_make("appsink", "appsink");

    // clang-format off
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_conv_caps(
        gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "RGB",
        nullptr),
        gst_caps_unref
    );
    // clang-format on

    g_object_set(G_OBJECT(src), "uri", std::format("srt://{}", uri).c_str(), nullptr);
    g_object_set(G_OBJECT(cf_conv), "caps", cf_conv_caps.get(), nullptr);
    g_object_set(G_OBJECT(appsink), "sync", false, nullptr);
    // clang-format off
    g_object_set(
        G_OBJECT(fpsdisplaysink),
        "video-sink", appsink,
        "text-overlay", false,
        "sync", false,
        nullptr
    );
    // clang-format on

    gst_bin_add_many(
        GST_BIN(pipeline),
        src,
        parser,
        tee,
        queue_display,
        dec,
        conv,
        cf_conv,
        fpsdisplaysink,
        nullptr
    );

    if (!gst_element_link_many(src, parser, tee, nullptr) ||
        !gst_element_link_many(tee, queue_display, dec, conv, cf_conv, fpsdisplaysink, nullptr)) {
        spdlog::error("Elements could not be linked.");
        gst_object_unref(pipeline);
    }
}

bool start_jpeg_recording(GstPipeline *pipeline, const RecordConfig &config)
{
    if (!pipeline) {
        spdlog::debug("Pipeline is null");
        return false;
    }
    if (config._path.empty()) {
        spdlog::debug("Config path is empty");
        return false;
    }
    spdlog::info("Starting M-JPEG recording ...");

    const auto &location = config._path.generic_string();
    const auto split = config._split;
    const auto max_size_time = config._max_size_time;

    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    if (auto exist_tee_srcpad = gst_element_get_static_pad(tee, "src_1")) {
        spdlog::warn("tee 'src_1' pad already exists, releasing it...");
        gst_element_release_request_pad(tee, exist_tee_srcpad);
        gst_object_unref(exist_tee_srcpad);
    }
    auto tee_srcpad = gst_element_request_pad_simple(tee, "src_1");
    gst_object_unref(tee);

    auto queue = gst_element_factory_make("queue", "queue_record");
    auto parser = gst_element_factory_make("jpegparse", "record_parser");
    auto muxer = gst_element_factory_make("matroskamux", "muxer");
    auto filesink = gst_element_factory_make("splitmuxsink", "filesink");

    if (!queue || !parser || !muxer || !filesink) {
        spdlog::error("Failed to create elements for recording");
        if (queue) gst_object_unref(queue);
        if (parser) gst_object_unref(parser);
        if (muxer) gst_object_unref(muxer);
        if (filesink) gst_object_unref(filesink);
        return false;
    }

    auto tracker = new FileTracker(location, {}, INT_MAX);
    g_signal_connect_data(
        filesink,
        "format-location",
        G_CALLBACK(generate_filename),
        tracker,
        [](gpointer data, GClosure *) {
            delete static_cast<FileTracker *>(data);
            spdlog::debug("FileTracker deleted");
        },
        G_CONNECT_DEFAULT
    );

    // clang-format off
    g_object_set(
        G_OBJECT(muxer), 
        "timecodescale", 1, 
        "offset-to-zero", true, 
        nullptr
    );
    g_object_set(
        G_OBJECT(filesink),
        "max-size-time", split ? max_size_time.count() * GST_SECOND : 0,  // max-size-time=0 -> continuous
        "async-finalize", false,
        "muxer", muxer,
        nullptr
    );
    // clang-format on

    gst_bin_add_many(GST_BIN(pipeline), queue, parser, filesink, nullptr);

    if (!gst_element_link_many(queue, parser, filesink, nullptr)) {
        spdlog::error("Failed to link MJPEG recording elements");
        return false;
    }

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(parser);
    gst_element_sync_state_with_parent(filesink);

    auto queue_sinkpad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(tee_srcpad, queue_sinkpad) != GST_PAD_LINK_OK) {
        spdlog::error("Failed to link 'tee' srcpad to 'queue' sinkpad");
        return false;
    }
    gst_object_unref(queue_sinkpad);

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "after-link");
    return true;
}

bool stop_jpeg_recording(GstPipeline *pipeline)
{
    if (!pipeline) {
        spdlog::debug("Pipeline is null");
        return false;
    }
    spdlog::info("Stopping M-JPEG recording ...");

    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    if (!tee) return false;

    auto tee_srcpad = gst_element_get_static_pad(tee, "src_1");
    if (!tee_srcpad) {
        gst_object_unref(tee);
        return false;
    }

    gst_pad_add_probe(
        tee_srcpad,
        GST_PAD_PROBE_TYPE_IDLE,
        [](GstPad *tee_srcpad, GstPadProbeInfo *, gpointer user_data) -> GstPadProbeReturn {
            spdlog::debug("MJPEG recording unlinking");

            // auto pipeline = GST_PIPELINE(user_data);
            auto pipeline = static_cast<GstBin *>(user_data);

            auto queue = gst_bin_get_by_name(pipeline, "queue_record");
            if (!queue) return GST_PAD_PROBE_REMOVE;

            auto queue_sinkpad = gst_element_get_static_pad(queue, "sink");
            if (!queue_sinkpad) {
                gst_object_unref(queue);
                return GST_PAD_PROBE_REMOVE;
            }

            gst_pad_unlink(tee_srcpad, queue_sinkpad);
            gst_pad_send_event(queue_sinkpad, gst_event_new_eos());

            gst_object_unref(queue_sinkpad);
            gst_object_unref(queue);

            return GST_PAD_PROBE_REMOVE;
        },
        pipeline,
        nullptr
    );

    gst_object_unref(tee_srcpad);
    gst_object_unref(tee);
    return true;
}

}  // namespace xvc