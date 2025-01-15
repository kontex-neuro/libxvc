#include "xvc.h"

#include <fmt/chrono.h>
#include <cpr/cpr.h>
#include <curl/curl.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <glib-object.h>
#include <glib.h>
#include <glibconfig.h>
#include <gst/gst.h>
#include <gst/gstbin.h>
#include <gst/gstbuffer.h>
#include <gst/gstcaps.h>
#include <gst/gstelement.h>
#include <gst/gstelementfactory.h>
#include <gst/gstevent.h>
#include <gst/gstinfo.h>
#include <gst/gstmeta.h>
#include <gst/gstobject.h>
#include <gst/gstpad.h>
#include <gst/gstparse.h>
#include <gst/gstpipeline.h>
#include <gst/gststructure.h>
#include <gst/gstutils.h>
#include <gst/video/video-info.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include "xdaqmetadata/key_value_store.h"
#include "xdaqmetadata/xdaqmetadata.h"

using namespace std::chrono_literals;

namespace
{

GstElement *create_element(const gchar *factoryname, const gchar *name)
{
    auto element = gst_element_factory_make(factoryname, name);
    if (!element) {
        spdlog::error("Element {} could not be created.", factoryname);
    }
    return element;
}

static gchararray generate_filename(GstElement *, guint fragment_id, gpointer udata)
{
    auto base_filename = static_cast<std::string *>(udata);
    auto now = std::chrono::system_clock::now();

    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;

#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);  // Windows
#else
    localtime_r(&time_t_now, &tm_now);  // Linux/Unix
#endif

    auto timestamp = fmt::format("{:%Y-%m-%d_%H-%M-%S}", tm_now);
    auto filename = fmt::format("{}-{}-{}.mkv", *base_filename, fragment_id, timestamp);

    return g_strdup(filename.c_str());
}

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    return fwrite(ptr, size, nmemb, stream);
}

std::string bytes_to_hex(const unsigned char *bytes, size_t len)
{
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        ss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return ss.str();
}

bool handle_response(const cpr::Response &response)
{
    if (response.status_code == 200) {
        auto json_response = nlohmann::json::parse(response.text);
        return json_response["status"] == "success";
    }

    spdlog::error(
        "File transfer failed with status code: {} ({})", response.status_code, response.text
    );
    return false;
}
}  // namespace


namespace xvc
{

void setup_h265_srt_stream(GstPipeline *pipeline, const std::string &uri)
{
    spdlog::info("setup_h265_srt_stream");

    auto src = create_element("srtsrc", "src");
    auto parser = create_element("h265parse", "parser");
    auto cf_parser = create_element("capsfilter", "cf_parser");
    auto tee = create_element("tee", "t");
    auto queue_display = create_element("queue", "queue_display");
#ifdef _WIN32
    auto dec = create_element("d3d11h265dec", "dec");
#elif __APPLE__
    auto dec = create_element("vtdec", "dec");
#else
    auto dec = create_element("avdec_h265", "dec");
#endif
    auto cf_dec = create_element("capsfilter", "cf_dec");
    auto conv = create_element("videoconvert", "conv");
    auto cf_conv = create_element("capsfilter", "cf_conv");
    auto appsink = create_element("appsink", "appsink");

    // clang-format off
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_src_caps(
        gst_caps_new_simple(
        "application/x-rtp",
        "encoding-name", G_TYPE_STRING, "H265", 
        nullptr),
        gst_caps_unref
    );
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_parser_caps(
        gst_caps_new_simple(
        "video/x-h265",
        "stream-format", G_TYPE_STRING, "byte-stream", 
        "alignment", G_TYPE_STRING, "au", 
        nullptr),
        gst_caps_unref
    );
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_dec_caps(
        gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "NV12", 
        nullptr),
        gst_caps_unref
    );
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_conv_caps(
        gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "RGB", 
        nullptr),
        gst_caps_unref
    );
    // clang-format on

    g_object_set(G_OBJECT(src), "uri", fmt::format("srt://{}", uri).c_str(), nullptr);
    g_object_set(G_OBJECT(cf_parser), "caps", cf_parser_caps.get(), nullptr);
    g_object_set(G_OBJECT(cf_dec), "caps", cf_dec_caps.get(), nullptr);
    g_object_set(G_OBJECT(cf_conv), "caps", cf_conv_caps.get(), nullptr);

    gst_bin_add_many(
        GST_BIN(pipeline),
        src,
        parser,
        cf_parser,
        tee,
        queue_display,
        dec,
        cf_dec,
        conv,
        cf_conv,
        appsink,
        nullptr
    );

    if (!gst_element_link_many(src, parser, cf_parser, tee, nullptr) ||
        !gst_element_link_many(tee, queue_display, dec, cf_dec, conv, cf_conv, appsink, nullptr)) {
        spdlog::error("Elements could not be linked.");
        gst_object_unref(pipeline);
    }
}

void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri)
{
    spdlog::info("setup_jpeg_srt_stream");

    auto src = create_element("srtclientsrc", "src");
    auto parser = create_element("jpegparse", "parser");
    auto tee = create_element("tee", "t");
    auto queue_display = create_element("queue", "queue_display");
#ifdef _WIN32
    auto dec = create_element("jpegdec", "dec");
#elif __APPLE__
    auto dec = create_element("vtdec", "dec");
#else
    auto dec = create_element("jpegdec", "dec");
#endif
    auto conv = create_element("videoconvert", "conv");
    auto cf_conv = create_element("capsfilter", "cf_conv");
    auto appsink = create_element("appsink", "appsink");

    // clang-format off
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_conv_caps(
        gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "RGB", 
        nullptr),
        gst_caps_unref
    );
    // clang-format on

    g_object_set(G_OBJECT(src), "uri", fmt::format("srt://{}", uri).c_str(), nullptr);
    g_object_set(G_OBJECT(cf_conv), "caps", cf_conv_caps.get(), nullptr);

    gst_bin_add_many(
        GST_BIN(pipeline), src, parser, tee, queue_display, dec, conv, cf_conv, appsink, nullptr
    );

    if (!gst_element_link_many(src, parser, tee, nullptr) ||
        !gst_element_link_many(tee, queue_display, dec, conv, cf_conv, appsink, nullptr)) {
        spdlog::error("Elements could not be linked.");
        gst_object_unref(pipeline);
    }
}

void start_h265_recording(
    GstPipeline *pipeline, fs::path &filepath, bool continuous, int max_size_time, int max_files
)
{
    spdlog::info("start_h265_recording");

    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    auto src_pad = gst_element_request_pad_simple(tee, "src_1");

    auto queue_record = create_element("queue", "queue_record");
    auto parser = create_element("h265parse", "record_parser");
    auto cf_parser = create_element("capsfilter", "cf_record_parser");
    auto filesink = create_element("splitmuxsink", "filesink");

    filepath += continuous ? ".mkv" : "-%02d.mkv";
    auto _max_size_time = continuous ? 0 : max_size_time * GST_SECOND * 60;

    // clang-format off
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_parser_caps(
        gst_caps_new_simple(
        "video/x-h265",
        "stream-format", G_TYPE_STRING, "hvc1", 
        "alignment", G_TYPE_STRING, "au", 
        nullptr),
        gst_caps_unref
    );
    // clang-format on

    g_object_set(G_OBJECT(cf_parser), "caps", cf_parser_caps.get(), nullptr);
    g_object_set(G_OBJECT(filesink), "location", filepath.generic_string().c_str(), nullptr);
    g_object_set(
        G_OBJECT(filesink), "max-size-time", _max_size_time, nullptr
    );  // max-size-time=0 -> continuous
    g_object_set(G_OBJECT(filesink), "max-files", max_files, nullptr);
    g_object_set(
        G_OBJECT(filesink), "max-size-bytes", 0, nullptr
    );  // Set max-size-bytes to 0 in order to make send-keyframe-requests work.
    g_object_set(G_OBJECT(filesink), "send-keyframe-requests", true, nullptr);
    g_object_set(G_OBJECT(filesink), "muxer-factory", "matroskamux", nullptr);

    gst_bin_add_many(GST_BIN(pipeline), queue_record, parser, cf_parser, filesink, nullptr);

    if (!gst_element_link_many(queue_record, parser, cf_parser, filesink, nullptr)) {
        spdlog::error("Elements could not be linked.");
        gst_object_unref(pipeline);
        return;
    }

    gst_element_sync_state_with_parent(queue_record);
    gst_element_sync_state_with_parent(parser);
    gst_element_sync_state_with_parent(cf_parser);
    gst_element_sync_state_with_parent(filesink);

    std::unique_ptr<GstPad, decltype(&gst_object_unref)> sink_pad(
        gst_element_get_static_pad(queue_record, "sink"), gst_object_unref
    );

    auto ret = gst_pad_link(src_pad, sink_pad.get());
    if (GST_PAD_LINK_FAILED(ret)) {
        g_error("Failed to link tee src pad to queue sink pad: %d", ret);
    }
    GST_DEBUG_BIN_TO_DOT_FILE(
        GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "video-capture-after-link"
    );
}

void stop_h265_recording(GstPipeline *pipeline)
{
    spdlog::info("stop_h265_recording");

    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    auto src_pad = gst_element_get_static_pad(tee, "src_1");
    gst_pad_add_probe(
        src_pad,
        GST_PAD_PROBE_TYPE_IDLE,
        [](GstPad *src_pad, GstPadProbeInfo *, gpointer user_data) -> GstPadProbeReturn {
            spdlog::info("Unlinking");

            auto pipeline = GST_PIPELINE(user_data);
            auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
            std::unique_ptr<GstElement, decltype(&gst_object_unref)> queue_record(
                gst_bin_get_by_name(GST_BIN(pipeline), "queue_record"), gst_object_unref
            );
            std::unique_ptr<GstElement, decltype(&gst_object_unref)> parser(
                gst_bin_get_by_name(GST_BIN(pipeline), "record_parser"), gst_object_unref
            );
            std::unique_ptr<GstElement, decltype(&gst_object_unref)> cf_parser(
                gst_bin_get_by_name(GST_BIN(pipeline), "cf_record_parser"), gst_object_unref
            );
            std::unique_ptr<GstElement, decltype(&gst_object_unref)> filesink(
                gst_bin_get_by_name(GST_BIN(pipeline), "filesink"), gst_object_unref
            );
            std::unique_ptr<GstPad, decltype(&gst_object_unref)> sink_pad(
                gst_element_get_static_pad(queue_record.get(), "sink"), gst_object_unref
            );
            gst_pad_unlink(src_pad, sink_pad.get());
            gst_pad_send_event(sink_pad.get(), gst_event_new_eos());

            gst_bin_remove(GST_BIN(pipeline), queue_record.get());
            gst_bin_remove(GST_BIN(pipeline), parser.get());
            gst_bin_remove(GST_BIN(pipeline), cf_parser.get());
            gst_bin_remove(GST_BIN(pipeline), filesink.get());

            gst_element_set_state(queue_record.get(), GST_STATE_NULL);
            gst_element_set_state(cf_parser.get(), GST_STATE_NULL);
            gst_element_set_state(parser.get(), GST_STATE_NULL);
            gst_element_set_state(filesink.get(), GST_STATE_NULL);

            gst_element_release_request_pad(tee, src_pad);
            gst_object_unref(src_pad);

            return GST_PAD_PROBE_REMOVE;
        },
        pipeline,
        nullptr
    );
}

void start_jpeg_recording(
    GstPipeline *pipeline, fs::path &filepath, bool continuous, int max_size_time, int max_files
)
{
    spdlog::info("start_jpeg_recording");

    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    auto src_pad = gst_element_request_pad_simple(tee, "src_1");

    auto queue_record = create_element("queue", "queue_record");
    auto parser = create_element("jpegparse", "record_parser");
    auto filesink = create_element("splitmuxsink", "filesink");

    auto _max_size_time = continuous ? 0 : max_size_time * GST_SECOND * 60;
    auto path = std::make_unique<std::string>(filepath.generic_string());

    g_signal_connect(filesink, "format-location", G_CALLBACK(generate_filename), path.release());

    g_object_set(
        G_OBJECT(filesink), "max-size-time", _max_size_time, nullptr
    );  // max-size-time=0 -> continuous
    g_object_set(G_OBJECT(filesink), "max-files", max_files, nullptr);
    g_object_set(G_OBJECT(filesink), "muxer-factory", "matroskamux", nullptr);

    gst_bin_add_many(GST_BIN(pipeline), queue_record, parser, filesink, nullptr);

    if (!gst_element_link_many(queue_record, parser, filesink, nullptr)) {
        spdlog::error("Elements could not be linked.");
        gst_object_unref(pipeline);
        return;
    }

    gst_element_sync_state_with_parent(queue_record);
    gst_element_sync_state_with_parent(parser);
    gst_element_sync_state_with_parent(filesink);

    std::unique_ptr<GstPad, decltype(&gst_object_unref)> sink_pad(
        gst_element_get_static_pad(queue_record, "sink"), gst_object_unref
    );

    auto ret = gst_pad_link(src_pad, sink_pad.get());
    if (GST_PAD_LINK_FAILED(ret)) {
        g_error("Failed to link tee src pad to queue sink pad: %d", ret);
    }
    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "after-link");
}

void stop_jpeg_recording(GstPipeline *pipeline)
{
    spdlog::info("stop_jpeg_recording");

    // Get the tee element and the recording branch pad.
    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    if (!tee) {
        spdlog::error("Tee element not found in pipeline");
        return;
    }
    GstPad *src_pad = gst_element_get_static_pad(tee, "src_1");
    if (!src_pad) {
        spdlog::error("No src pad 'src_1' found on tee");
        gst_object_unref(tee);
        return;
    }

    // Retrieve the recording branch elements.
    GstElement *queue_record = gst_bin_get_by_name(GST_BIN(pipeline), "queue_record");
    GstElement *parser = gst_bin_get_by_name(GST_BIN(pipeline), "record_parser");
    GstElement *filesink = gst_bin_get_by_name(GST_BIN(pipeline), "filesink");
    if (!queue_record || !parser || !filesink) {
        spdlog::error("Recording branch elements missing");
        gst_object_unref(src_pad);
        gst_object_unref(tee);
        return;
    }

    // Send EOS event on the recording branch sink pad.
    GstPad *sink_pad = gst_element_get_static_pad(queue_record, "sink");
    if (sink_pad) {
        spdlog::info("Sending EOS event to recording branch");
        gst_pad_send_event(sink_pad, gst_event_new_eos());
        gst_object_unref(sink_pad);
    } else {
        spdlog::error("Sink pad on queue_record not found");
    }

    // Immediately release the tee's request pad so that the rendering branch is not blocked.
    gst_pad_add_probe(
        src_pad,
        GST_PAD_PROBE_TYPE_IDLE,
        [](GstPad *src_pad, GstPadProbeInfo *, gpointer user_data) -> GstPadProbeReturn {
            GstElement *tee = gst_bin_get_by_name(GST_BIN((GstPipeline *) user_data), "t");
            spdlog::info("Releasing tee's src pad to unblock rendering branch");
            gst_element_release_request_pad(tee, src_pad);
            gst_object_unref(src_pad);
            gst_object_unref(tee);
            return GST_PAD_PROBE_REMOVE;
        },
        pipeline,
        nullptr
    );

    // Structure to pass recording branch elements for delayed cleanup.
    struct CleanupData {
        GstPipeline *pipeline;
        GstElement *queue;
        GstElement *parser;
        GstElement *filesink;
    };

    CleanupData *cleanup_data = new CleanupData;
    cleanup_data->pipeline = pipeline;
    cleanup_data->queue = queue_record;  // holds a reference from gst_bin_get_by_name
    cleanup_data->parser = parser;
    cleanup_data->filesink = filesink;

    // Schedule delayed cleanup (after ~3500ms) so that EOS has time to propagate.
    g_timeout_add(
        3500,
        [](gpointer data) -> gboolean {
            CleanupData *cleanup = static_cast<CleanupData *>(data);
            spdlog::info("Performing delayed cleanup of JPEG recording branch");
            // Remove the branch elements from the pipeline.
            gst_bin_remove(GST_BIN(cleanup->pipeline), cleanup->queue);
            gst_bin_remove(GST_BIN(cleanup->pipeline), cleanup->parser);
            gst_bin_remove(GST_BIN(cleanup->pipeline), cleanup->filesink);

            // Set the states of the branch elements to NULL.
            gst_element_set_state(cleanup->queue, GST_STATE_NULL);
            gst_element_set_state(cleanup->parser, GST_STATE_NULL);
            gst_element_set_state(cleanup->filesink, GST_STATE_NULL);

            // Unref the elements.
            gst_object_unref(cleanup->queue);
            gst_object_unref(cleanup->parser);
            gst_object_unref(cleanup->filesink);
            delete cleanup;
            return FALSE;  // One-time callback.
        },
        cleanup_data
    );

    // Unref our local references.
    gst_object_unref(src_pad);
    gst_object_unref(tee);
}

void mock_camera(GstPipeline *pipeline, const std::string &)
{
    spdlog::info("mock_camera");

    auto src = create_element("videotestsrc", "src");
    auto cf_src = create_element("capsfilter", "cf_src");
    // auto src = create_element("srtsrc", "src");
    // auto tee = create_element("tee", "t");
    // #ifdef JPEG_CLIENT
    //     auto parser = create_element("jpegparse", "parser");
    //     auto dec = create_element("jpegdec", "dec");
    // #elif H265_CLIENT
    //     auto parser = create_element("h265parse", "parser");
    //     auto dec = create_element("d3d12h265device1dec", "dec");
    // #elif H264_CLIENT
    //     auto parser = create_element("h264parse", "parser");
    //     auto dec = create_element("d3d12h264device1dec", "dec");
    // #endif
    // auto conv = create_element("videoconvert", "conv");
    // auto cf_conv = create_element("capsfilter", "cf_conv");
    // auto queue = create_element("queue", "queue");
    auto appsink = create_element("appsink", "appsink");

    // clang-format off
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_src_caps(
        gst_caps_new_simple(
            "video/x-raw",
            "format", G_TYPE_STRING, "RGB", 
            // "width", G_TYPE_INT, 720,
            "width", G_TYPE_INT, 3840,
            // "height", G_TYPE_INT, 540,
            "height", G_TYPE_INT, 2160,
            "framerate", GST_TYPE_FRACTION, 500, 1,
            nullptr
        ),
        gst_caps_unref
    );
    std::unique_ptr<GstCaps, decltype(&gst_caps_unref)> cf_conv_caps(
        gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "RGB", 
        nullptr),
        gst_caps_unref
    );
    // clang-format on

    // g_object_set(G_OBJECT(src), "pattern", 18, nullptr);
    // g_object_set(G_OBJECT(src), "uri", fmt::format("srt://{}", uri).c_str(), nullptr);
    g_object_set(G_OBJECT(cf_src), "caps", cf_src_caps.get(), nullptr);
    // g_object_set(G_OBJECT(cf_conv), "caps", cf_conv_caps.get(), nullptr);
    g_object_set(G_OBJECT(appsink), "drop", true, nullptr);
    g_object_set(G_OBJECT(appsink), "sync", false, nullptr);

    gst_bin_add_many(
        GST_BIN(pipeline),
        src,
        cf_src,
        // parser,
        // // tee,
        // dec,
        // conv,
        // cf_conv,
        // queue,
        appsink,
        nullptr
    );

    // if (!gst_element_link_many(src, parser, dec, conv, cf_conv, queue, appsink, nullptr)) {
    if (!gst_element_link_many(src, cf_src, appsink, nullptr)) {
        // if (!gst_element_link_many(src, cf_src, parser, dec, conv, cf_conv, appsink, nullptr)) {
        spdlog::error("Elements could not be linked.");
        gst_object_unref(pipeline);
    }
}

void parse_video_save_binary_h265(const std::string &video_filepath)
{
    auto bin_file_name = video_filepath;
    bin_file_name.replace(bin_file_name.end() - 3, bin_file_name.end(), "bin");

    spdlog::info("parse_video_save_binary: {}", bin_file_name);

    KeyValueStore bin_store(bin_file_name);

    bin_store.openFile();

    auto pipeline_str = fmt::format(
        "filesrc location=\"{}\" ! matroskademux ! h265parse name=h265parse ! video/x-h265, "
        "stream-format=byte-stream, alignment=au ! fakesink",
        video_filepath
    );

    spdlog::info("pipeline_str = {}", pipeline_str);

    GError *error = nullptr;
    std::unique_ptr<GstElement, decltype(&gst_object_unref)> pipeline(
        gst_parse_launch(pipeline_str.c_str(), &error), gst_object_unref
    );

    if (!pipeline) {
        spdlog::error("Failed to create pipeline: {}", error->message);
        g_clear_error(&error);
        return;
    }

    std::unique_ptr<GstElement, decltype(&gst_object_unref)> h265parse(
        gst_bin_get_by_name(GST_BIN(pipeline.get()), "h265parse"), gst_object_unref
    );

    if (!h265parse) {
        spdlog::error("Failed to get h265parse element");
        return;
    }

    std::unique_ptr<GstPad, decltype(&gst_object_unref)> src_pad{
        gst_element_get_static_pad(h265parse.get(), "src"), gst_object_unref
    };

    if (!src_pad) {
        spdlog::error("Failed to get h265parse's src pad");
        return;
    }

    gst_pad_add_probe(
        src_pad.get(), GST_PAD_PROBE_TYPE_BUFFER, h265_parse_saving_metadata, &bin_store, nullptr
    );

    gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);

    // Event loop to keep the pipeline running
    std::unique_ptr<GstBus, decltype(&gst_object_unref)> bus = {
        gst_element_get_bus(pipeline.get()), gst_object_unref
    };
    bool terminate = false;

    while (!terminate) {
        // Wait for a message for up to 100 milliseconds
        std::unique_ptr<GstMessage, decltype(&gst_message_unref)> msg(
            gst_bus_timed_pop_filtered(
                bus.get(),
                100 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)
            ),
            gst_message_unref
        );

        // Handle errors and EOS messages
        if (msg) {
            GError *err;
            gchar *debug_info;

            switch (GST_MESSAGE_TYPE(msg.get())) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg.get(), &err, &debug_info);
                spdlog::error("Error from element {}:", GST_OBJECT_NAME(msg->src), err->message);
                g_clear_error(&err);
                g_free(debug_info);
                terminate = true;
                break;
            case GST_MESSAGE_EOS:
                spdlog::info("End-Of-Stream reached.");
                terminate = true;
                break;
            default: break;
            }
        }
    }

    gst_element_set_state(pipeline.get(), GST_STATE_NULL);

    bin_store.closeFile();
}

void parse_video_save_binary_jpeg(const std::string &video_filepath)
{
    auto bin_file_name = video_filepath;
    bin_file_name.replace(bin_file_name.end() - 3, bin_file_name.end(), "bin");

    spdlog::info("parse_video_save_binary: {}", bin_file_name);

    KeyValueStore bin_store(bin_file_name);

    bin_store.openFile();

    auto pipeline_str = fmt::format(
        "filesrc location=\"{}\" ! matroskademux ! jpegparse name=jpegparse ! fakesink",
        video_filepath
    );

    spdlog::info("pipeline_str = {}", pipeline_str);

    GError *error = nullptr;
    std::unique_ptr<GstElement, decltype(&gst_object_unref)> pipeline(
        gst_parse_launch(pipeline_str.c_str(), &error), gst_object_unref
    );

    if (!pipeline) {
        spdlog::error("Failed to create pipeline: {}", error->message);
        g_clear_error(&error);
        return;
    }

    std::unique_ptr<GstElement, decltype(&gst_object_unref)> jpegparse{
        gst_bin_get_by_name(GST_BIN(pipeline.get()), "jpegparse"), gst_object_unref
    };

    if (!jpegparse) {
        spdlog::error("Failed to get jpegparse element");
        return;
    }

    std::unique_ptr<GstPad, decltype(&gst_object_unref)> src_pad{
        gst_element_get_static_pad(jpegparse.get(), "src"), gst_object_unref
    };

    if (!src_pad) {
        spdlog::error("Failed to get jpegparse's src pad");
        return;
    }

    gst_pad_add_probe(
        src_pad.get(), GST_PAD_PROBE_TYPE_BUFFER, jpeg_parse_saving_metadata, &bin_store, nullptr
    );

    gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);

    // Event loop to keep the pipeline running
    std::unique_ptr<GstBus, decltype(&gst_object_unref)> bus = {
        gst_element_get_bus(pipeline.get()), gst_object_unref
    };
    bool terminate = false;

    while (!terminate) {
        // Wait for a message for up to 100 milliseconds
        std::unique_ptr<GstMessage, decltype(&gst_message_unref)> msg(
            gst_bus_timed_pop_filtered(
                bus.get(),
                100 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)
            ),
            gst_message_unref
        );

        // Handle errors and EOS messages
        if (msg) {
            GError *err;
            gchar *debug_info;

            switch (GST_MESSAGE_TYPE(msg.get())) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg.get(), &err, &debug_info);
                spdlog::error("Error from element {}:", GST_OBJECT_NAME(msg->src), err->message);
                g_clear_error(&err);
                g_free(debug_info);
                terminate = true;
                break;
            case GST_MESSAGE_EOS:
                spdlog::info("End-Of-Stream reached.");
                terminate = true;
                break;
            default: break;
            }
        }
    }

    gst_element_set_state(pipeline.get(), GST_STATE_NULL);

    bin_store.closeFile();
}

std::optional<std::string> calculate_sha256(const std::filesystem::path &filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        spdlog::error("Failed to open file for hashing: {}", filepath.string());
        return std::nullopt;
    }

    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    char buffer[4096];
    while (file.read(buffer, sizeof(buffer))) {
        SHA256_Update(&sha256, buffer, file.gcount());
    }
    if (file.gcount() > 0) {
        SHA256_Update(&sha256, buffer, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);

    std::string calculated = bytes_to_hex(hash, SHA256_DIGEST_LENGTH);
    spdlog::info("Calculated hash: {}", calculated);
    return calculated;
}

DownloadResult download_and_verify(
    const std::string &url, const std::string &expected_hash,
    const std::filesystem::path &output_path
)
{
    DownloadResult result{false, ""};

    try {
        // First, make a HEAD request to get the expected file size
        auto head_response = cpr::Head(cpr::Url{url}, cpr::VerifySsl{false}, cpr::Timeout{2s});

        if (head_response.status_code != 200) {
            result.error_message =
                fmt::format("Failed to get file information: {}", head_response.status_code);
            return result;
        }

        // Get expected file size from header
        size_t expected_size = 0;
        if (head_response.header.count("Content-Length") > 0) {
            expected_size = std::stoull(head_response.header["Content-Length"]);
        }

        // Download the file
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            result.error_message = "Failed to open output file for writing";
            return result;
        }

        auto response = cpr::Download(
            file, cpr::Url{url}, cpr::VerifySsl{false}, cpr::Timeout{30s}
            // Increased timeout for larger files
        );

        file.close();

        if (response.status_code != 200) {
            result.error_message =
                fmt::format("Download failed with status code: {}", response.status_code);
            std::filesystem::remove(output_path);
            return result;
        }

        // Verify file size
        if (expected_size > 0) {
            auto actual_size = std::filesystem::file_size(output_path);
            if (actual_size != expected_size) {
                result.error_message = fmt::format(
                    "File size mismatch. Expected: {}, Got: {}", expected_size, actual_size
                );
                std::filesystem::remove(output_path);
                return result;
            }
        }

        // Calculate and verify hash
        auto calculated_hash = calculate_sha256(output_path);
        if (!calculated_hash) {
            result.error_message = "Failed to calculate file hash";
            std::filesystem::remove(output_path);
            return result;
        }

        if (*calculated_hash != expected_hash) {
            result.error_message = fmt::format(
                "Hash verification failed. Expected: {}, Got: {}", expected_hash, *calculated_hash
            );
            std::filesystem::remove(output_path);
            return result;
        }

        result.success = true;
        return result;

    } catch (const std::exception &e) {
        result.error_message = fmt::format("Download failed: {}", e.what());
        if (std::filesystem::exists(output_path)) {
            std::filesystem::remove(output_path);
        }
        return result;
    }
}

HandshakeResponse perform_handshake(const std::string &server_address, int port)
{
    HandshakeResponse response{false, "", "", {}};

    try {
        std::string url = fmt::format("http://{}:{}/handshake", server_address, port);

        spdlog::info("Attempting handshake with server at {}", url);

        auto http_response =
            cpr::Get(cpr::Url{url}, cpr::Timeout{2s}, cpr::Header{{"User-Agent", "XVC-Client"}});

        if (http_response.status_code == 200) {
            try {
                auto json_response = nlohmann::json::parse(http_response.text);
                spdlog::debug("Raw server response: {}", http_response.text);

                if (json_response["status"] == "ready") {
                    response.success = true;
                    response.token = json_response["token"].get<std::string>();

                    // Get current UTC time
                    auto now = std::chrono::system_clock::now();
                    auto now_ts = std::chrono::system_clock::to_time_t(now);

                    // Get expiration time from server (as UTC timestamp)
                    int64_t expire_timestamp = json_response["expires"].get<int64_t>();
                    response.expires = std::chrono::system_clock::from_time_t(expire_timestamp);

                    // Format times in UTC
                    std::tm now_tm_utc{}, expire_tm_utc{};
#ifdef _WIN32
                    gmtime_s(&now_tm_utc, &now_ts);
                    gmtime_s(&expire_tm_utc, &expire_timestamp);
#else
                    gmtime_r(&now_ts, &now_tm_utc);
                    gmtime_r(&expire_timestamp, &expire_tm_utc);
#endif
                    char now_str[32], expire_str[32];
                    std::strftime(now_str, sizeof(now_str), "%Y-%m-%d %H:%M:%S UTC", &now_tm_utc);
                    std::strftime(
                        expire_str, sizeof(expire_str), "%Y-%m-%d %H:%M:%S UTC", &expire_tm_utc
                    );

                    spdlog::info("Handshake successful!");
                    spdlog::info("Current UTC time: {}", now_str);
                    spdlog::info("Current UTC timestamp: {}", now_ts);
                    spdlog::info("Session token: {}", response.token);
                    spdlog::info("Token expires (UTC): {}", expire_str);
                    spdlog::info("Expire UTC timestamp: {}", expire_timestamp);
                    spdlog::info("Time until expiration: {} seconds", expire_timestamp - now_ts);
                }
            } catch (const nlohmann::json::exception &e) {
                response.error_message =
                    fmt::format("Invalid handshake response format: {}", e.what());
                spdlog::error("JSON parse error: {}", e.what());
            }
        } else {
            response.error_message =
                fmt::format("Handshake failed with status code: {}", http_response.status_code);
            spdlog::error("HTTP error: {}", response.error_message);
        }

    } catch (const std::exception &e) {
        response.error_message = fmt::format("Handshake failed with exception: {}", e.what());
        spdlog::error("Exception: {}", e.what());
    }

    return response;
}

bool prepare_file_transfer(
    const std::string &server_address, int port, const std::string &token,
    const std::string &filename, const std::string &file_hash, size_t file_size,
    std::string &out_transfer_id
)
{
    try {
        std::string url = fmt::format("http://{}:{}/prepare-transfer", server_address, port);

        nlohmann::json request_body = {
            {"filename", filename}, {"file_hash", file_hash}, {"file_size", file_size}
        };

        auto response = cpr::Post(
            cpr::Url{url},
            cpr::Header{
                {"Authorization", fmt::format("Bearer {}", token)},
                {"Content-Type", "application/json"}
            },
            cpr::Body{request_body.dump()},
            cpr::Timeout{5s}
        );

        if (response.status_code == 200) {
            auto json_response = nlohmann::json::parse(response.text);
            if (json_response["status"] == "ready") {
                out_transfer_id = json_response["transfer_id"];
                return true;
            }
        }

        spdlog::error("Failed to prepare transfer: {} ({})", response.text, response.status_code);
        return false;

    } catch (const std::exception &e) {
        spdlog::error("Error preparing transfer: {}", e.what());
        return false;
    }
}

bool transfer_file(
    const std::string &server_address, int port, const std::string &token,
    const std::filesystem::path &file_path, const std::string &transfer_id,
    ProgressCallback progress_callback
)
{
    std::chrono::seconds timeout = std::chrono::seconds{30};
    try {
        if (!std::filesystem::exists(file_path)) {
            spdlog::error("File does not exist: {}", file_path.string());
            return false;
        }
        if (!std::filesystem::is_regular_file(file_path)) {
            spdlog::error("Invalid file type: {}", file_path.string());
            return false;
        }

        size_t file_size = std::filesystem::file_size(file_path);
        if (file_size == 0) {
            spdlog::error("File is empty: {}", file_path.string());
            return false;
        }

        std::string url =
            fmt::format("http://{}:{}/transfer/{}", server_address, port, transfer_id);

        cpr::Multipart multipart{};
        multipart.parts.emplace_back("file", cpr::File{file_path.string()});

        cpr::Header headers = {{"Authorization", fmt::format("Bearer {}", token)}};

        // Deduplication logic in progress callback
        auto progress_callback_wrapper = [&progress_callback, file_size](
                                             size_t, size_t, size_t, size_t ul_now, intptr_t
                                         ) -> bool {
            static size_t last_progress = 0;
            size_t actual_progress = std::clamp(ul_now, size_t{0}, file_size);

            if (actual_progress != last_progress) {  // Only log/report if progress changes
                last_progress = actual_progress;
                if (progress_callback) {
                    progress_callback(
                        {actual_progress,
                         file_size,
                         file_size > 0 ? static_cast<float>(actual_progress) / file_size * 100.0f
                                       : 0.0f}
                    );
                }
            }
            return true;  // Continue transfer
        };

        cpr::Response response = cpr::Post(
            cpr::Url{url},
            headers,
            multipart,
            progress_callback ? cpr::ProgressCallback(progress_callback_wrapper)
                              : cpr::ProgressCallback{},
            cpr::Timeout{timeout}
        );

        return handle_response(response);

    } catch (const std::exception &e) {
        spdlog::error(
            "File transfer failed for file '{}' (transfer_id: {}): {}",
            file_path.string(),
            transfer_id,
            e.what()
        );
        return false;
    }
}


std::optional<Version> get_server_version(const std::string &server_address, int port)
{
    try {
        auto response = cpr::Get(
            cpr::Url{fmt::format("http://{}:{}/server_version", server_address, port)},
            cpr::Timeout{5s}
        );

        if (response.status_code == 200) {
            // Parse JSON response
            auto json_response = nlohmann::json::parse(response.text);

            // Extract version string from JSON
            if (json_response.contains("version")) {
                return Version::from_string(json_response["version"].get<std::string>());
            }

            spdlog::error("Server response missing version field: {}", response.text);
            return std::nullopt;
        }

        spdlog::error("Failed to get server version: {} ({})", response.text, response.status_code);
        return std::nullopt;

    } catch (const std::exception &e) {
        spdlog::error("Error getting server version: {}", e.what());
        return std::nullopt;
    }
}

std::optional<VersionTable> get_version_table(const std::string &table_url)
{
    try {
        auto response = cpr::Get(
            cpr::Url{table_url}, cpr::Timeout{5s}, cpr::VerifySsl{false}
            // Add this if needed for self-signed certs
        );

        if (response.status_code == 200) {
            auto json = nlohmann::json::parse(response.text);
            VersionTable table;

            // Parse latest version
            auto latest_ver = Version::from_string(json["latest_version"].get<std::string>());
            if (!latest_ver) {
                spdlog::error("Invalid latest version format");
                return std::nullopt;
            }
            table.latest_version = *latest_ver;

            // Parse version entries
            for (const auto &version_data : json["versions"]) {
                UpdateInfo info;

                // Parse version
                auto ver = Version::from_string(version_data["version"].get<std::string>());
                if (!ver) {
                    spdlog::error("Invalid version format in version entry");
                    continue;
                }
                info.version = *ver;

                // Parse other fields
                info.release_date = version_data["release_date"].get<std::string>();
                info.update_url = version_data["update_url"].get<std::string>();
                info.hash = version_data["hash"].get<std::string>();

                auto min_ver =
                    Version::from_string(version_data["min_client_version"].get<std::string>());
                if (!min_ver) {
                    spdlog::error("Invalid min_client_version format");
                    continue;
                }
                info.min_client_version = *min_ver;

                info.description = version_data["description"].get<std::string>();

                table.versions.push_back(info);
            }

            return table;
        }

        spdlog::error("Failed to get version table: {} ({})", response.text, response.status_code);
        return std::nullopt;

    } catch (const std::exception &e) {
        spdlog::error("Error getting version table: {}", e.what());
        return std::nullopt;
    }
}

UpdateResult update_server(
    const std::string &server_address,
    int server_port,         // Port of the server to be updated
    int update_server_port,  // Port of the update server
    const std::string &table_url, const std::filesystem::path &update_dir,
    const Version &client_version, bool skip_version_check,
    const std::optional<Version> &force_version
)
{
    UpdateResult result;
    result.success = false;

    try {
        // Step 1: Version check (unless skipped)

        auto current_version = get_server_version(server_address, server_port);
        if (!current_version) {
            result.error_message = "Failed to get current server version";
            return result;
        }
        result.current_version = *current_version;


        // Step 2: Get version table
        auto version_table = get_version_table(table_url);
        if (!version_table) {
            result.error_message = "Failed to get version table";
            return result;
        }

        // If force_version is specified, override the target version
        if (force_version) {
            auto it = std::find_if(
                version_table->versions.begin(),
                version_table->versions.end(),
                [&](const UpdateInfo &info) { return info.version == *force_version; }
            );

            if (it == version_table->versions.end()) {
                result.error_message = fmt::format(
                    "Forced version {} not found in version table", force_version->to_string()
                );
                return result;
            }
            version_table->latest_version = *force_version;
        }

        result.available_version = version_table->latest_version;

        // Check if update is needed (unless forced)
        if (!skip_version_check && result.current_version >= version_table->latest_version) {
            result.update_needed = false;
            result.success = true;
            return result;
        }

        result.update_needed = true;

        // Step 3: Download and verify update file
        auto target_version = std::find_if(
            version_table->versions.begin(),
            version_table->versions.end(),
            [&](const UpdateInfo &info) { return info.version == version_table->latest_version; }
        );

        if (target_version == version_table->versions.end()) {
            result.error_message = "Target version not found in version table";
            return result;
        }

        // Create update directory if it doesn't exist
        std::filesystem::create_directories(update_dir);

        auto update_file =
            update_dir / fmt::format("xvc-server-{}.tar.xz", target_version->version.to_string());

        spdlog::info("Downloading update file from {}", target_version->update_url);
        auto download_result =
            download_and_verify(target_version->update_url, target_version->hash, update_file);

        if (!download_result.success) {
            result.error_message =
                fmt::format("Failed to download update: {}", download_result.error_message);
            return result;
        }

        // Step 4: Perform handshake with update server
        spdlog::info("Performing handshake with update server");
        auto handshake_response = perform_handshake(server_address, update_server_port);
        if (!handshake_response.success) {
            result.error_message =
                fmt::format("Handshake failed: {}", handshake_response.error_message);
            return result;
        }

        // Step 5: Prepare file transfer
        std::string transfer_id;
        auto file_size = std::filesystem::file_size(update_file);

        spdlog::info("Preparing file transfer");
        bool prepared = prepare_file_transfer(
            server_address,
            update_server_port,
            handshake_response.token,
            update_file.filename().string(),
            target_version->hash,
            file_size,
            transfer_id
        );

        if (!prepared) {
            result.error_message = "Failed to prepare file transfer";
            return result;
        }

        // Step 6: Perform file transfer
        spdlog::info("Transferring update file");
        bool transfer_success = transfer_file(
            server_address,
            update_server_port,
            handshake_response.token,
            update_file,
            transfer_id,
            [](const FileTransferProgress &progress) {
                spdlog::info(
                    "Transfer progress: {:.1f}% ({}/{} bytes)",
                    progress.progress_percentage,
                    progress.bytes_transferred,
                    progress.total_bytes
                );
            }
        );

        if (!transfer_success) {
            result.error_message = "File transfer failed";
            return result;
        }

        result.success = true;
        return result;

    } catch (const std::exception &e) {
        result.error_message = fmt::format("Update failed: {}", e.what());
        return result;
    }
}

bool Version::operator==(const Version &other) const
{
    return major == other.major && minor == other.minor && patch == other.patch;
}

bool Version::operator>(const Version &other) const { return !(*this < other || *this == other); }

bool Version::operator<(const Version &other) const
{
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    return patch < other.patch;
}

bool Version::operator>=(const Version &other) const { return !(*this < other); }

bool Version::operator<=(const Version &other) const { return (*this < other) || (*this == other); }

std::optional<Version> Version::from_string(const std::string &version_str)
{
    try {
        std::regex version_regex(R"((\d+)\.(\d+)\.(\d+))");
        std::smatch matches;

        if (std::regex_match(version_str, matches, version_regex)) {
            return Version{
                std::stoi(matches[1].str()),
                std::stoi(matches[2].str()),
                std::stoi(matches[3].str())
            };
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::string Version::to_string() const { return fmt::format("{}.{}.{}", major, minor, patch); }

}  // namespace xvc