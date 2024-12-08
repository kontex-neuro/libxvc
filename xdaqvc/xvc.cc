#include "xvc.h"

#include <fmt/core.h>
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
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>

#include "xdaqmetadata/key_value_store.h"
#include "xdaqmetadata/xdaqmetadata.h"


using namespace std::chrono_literals;

namespace
{
GstElement *create_element(const gchar *factoryname, const gchar *name)
{
    auto element = gst_element_factory_make(factoryname, name);
    if (!element) {
        g_error("Element %s could not be created.", factoryname);
    }
    return element;
}
}  // namespace


namespace xvc
{

void setup_h265_srt_stream(GstPipeline *pipeline, const std::string &uri)
{
    g_info("setup_h265_srt_stream");

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
        NULL),
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
        g_error("Elements could not be linked.\n");
        gst_object_unref(pipeline);
        return;
    }
}

void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri)
{
    g_info("setup_jpeg_srt_stream");

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
        g_error("Elements could not be linked.\n");
        gst_object_unref(pipeline);
        return;
    }
}

void start_h265_recording(GstPipeline *pipeline, fs::path &filepath)
{
    spdlog::info("start_h265_recording");

    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    auto src_pad = gst_element_request_pad_simple(tee, "src_1");

    auto queue_record = create_element("queue", "queue_record");
    auto parser = create_element("h265parse", "record_parser");
    auto cf_parser = create_element("capsfilter", "cf_record_parser");
    auto filesink = create_element("splitmuxsink", "filesink");

    filepath += "-%02d.mkv";

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
    g_object_set(G_OBJECT(filesink), "max-files", 10, nullptr);
    g_object_set(
        G_OBJECT(filesink), "max-size-bytes", 0, nullptr
    );  // Set max-size-bytes to 0 in order to make send-keyframe-requests work.
    g_object_set(G_OBJECT(filesink), "send-keyframe-requests", true, nullptr);
    g_object_set(G_OBJECT(filesink), "muxer-factory", "matroskamux", nullptr);

    gst_bin_add_many(GST_BIN(pipeline), queue_record, parser, cf_parser, filesink, nullptr);

    if (!gst_element_link_many(queue_record, parser, cf_parser, filesink, nullptr)) {
        g_error("Elements could not be linked.\n");
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
    g_info("stop_h265_recording");
    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    auto src_pad = gst_element_get_static_pad(tee, "src_1");
    gst_pad_add_probe(
        src_pad,
        GST_PAD_PROBE_TYPE_IDLE,
        [](GstPad *src_pad, GstPadProbeInfo *info, gpointer user_data) -> GstPadProbeReturn {
            g_info("Unlinking...");
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
            g_info("Unlinked");

            return GST_PAD_PROBE_REMOVE;
        },
        pipeline,
        NULL
    );
}

void start_jpeg_recording(GstPipeline *pipeline, fs::path &filepath)
{
    spdlog::info("start_jpeg_recording");

    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    auto src_pad = gst_element_request_pad_simple(tee, "src_1");

    auto queue_record = create_element("queue", "queue_record");
    auto parser = create_element("jpegparse", "record_parser");
    auto filesink = create_element("splitmuxsink", "filesink");

    filepath += "-%02d.mkv";

    g_object_set(G_OBJECT(filesink), "location", filepath.generic_string().c_str(), nullptr);
    g_object_set(G_OBJECT(filesink), "max-size-time", 0, nullptr);  // max-size-time=0 -> continuous
    g_object_set(G_OBJECT(filesink), "muxer-factory", "matroskamux", nullptr);

    gst_bin_add_many(GST_BIN(pipeline), queue_record, parser, filesink, nullptr);

    if (!gst_element_link_many(queue_record, parser, filesink, nullptr)) {
        g_error("Elements could not be linked.\n");
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
    g_info("stop_jpeg_recording");
    auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
    auto src_pad = gst_element_get_static_pad(tee, "src_1");
    gst_pad_add_probe(
        src_pad,
        GST_PAD_PROBE_TYPE_IDLE,
        [](GstPad *src_pad, GstPadProbeInfo *info, gpointer user_data) -> GstPadProbeReturn {
            g_info("unlink");
            auto pipeline = GST_PIPELINE(user_data);
            auto tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
            std::unique_ptr<GstElement, decltype(&gst_object_unref)> queue_record(
                gst_bin_get_by_name(GST_BIN(pipeline), "queue_record"), gst_object_unref
            );
            std::unique_ptr<GstElement, decltype(&gst_object_unref)> parser(
                gst_bin_get_by_name(GST_BIN(pipeline), "record_parser"), gst_object_unref
            );
            std::unique_ptr<GstElement, decltype(&gst_object_unref)> filesink(
                gst_bin_get_by_name(GST_BIN(pipeline), "filesink"), gst_object_unref
            );
            std::unique_ptr<GstPad, decltype(&gst_object_unref)> sink_pad(
                gst_element_get_static_pad(queue_record.get(), "sink"), gst_object_unref
            );
            gst_pad_send_event(sink_pad.get(), gst_event_new_eos());

            gst_pad_unlink(src_pad, sink_pad.get());

            gst_bin_remove(GST_BIN(pipeline), queue_record.get());
            gst_bin_remove(GST_BIN(pipeline), parser.get());
            gst_bin_remove(GST_BIN(pipeline), filesink.get());

            gst_element_set_state(queue_record.get(), GST_STATE_NULL);
            gst_element_set_state(parser.get(), GST_STATE_NULL);
            gst_element_set_state(filesink.get(), GST_STATE_NULL);

            gst_element_release_request_pad(tee, src_pad);
            gst_object_unref(src_pad);

            return GST_PAD_PROBE_REMOVE;
        },
        pipeline,
        NULL
    );
}

void mock_camera(GstPipeline *pipeline, const std::string &uri)
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
        g_error("Elements could not be linked.\n");
        gst_object_unref(pipeline);
        return;
    }
}

void parse_video_save_binary_h265(std::string &video_filepath)
{
    std::string bin_file_name = video_filepath;
    bin_file_name.replace(bin_file_name.end() - 3, bin_file_name.end(), "bin");

    spdlog::info("parse_video_save_binary: {}", bin_file_name);

    KeyValueStore bin_store(bin_file_name);

    std::cout << "bin_file_name: " << bin_file_name << std::endl;

    bin_store.openFile();

    std::string pipeline_str = " filesrc location=\"" + video_filepath +
                               "\" "
                               " ! matroskademux "
                               " ! h265parse name=h265parse "
                               " ! video/x-h265, stream-format=byte-stream, alignment=au "
                               " ! fakesink ";
    printf("pipeline_str: %s\n", pipeline_str.c_str());

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);

    if (!pipeline) {
        std::cerr << "Failed to create pipeline: " << error->message << std::endl;
        g_clear_error(&error);
        return;
    }

    // parse pts : h265parse src
    std::unique_ptr<GstElement, decltype(&gst_object_unref)> h265parse{
        gst_bin_get_by_name(GST_BIN(pipeline), "h265parse"), gst_object_unref
    };
    if (h265parse.get() == nullptr) {
        std::cerr << "Failed to get h265parse element" << std::endl;
        return;
    } else {
        std::unique_ptr<GstPad, decltype(&gst_object_unref)> pad{
            gst_element_get_static_pad(h265parse.get(), "src"), gst_object_unref
        };
        if (pad.get() != nullptr) {
            gst_pad_add_probe(
                pad.get(), GST_PAD_PROBE_TYPE_BUFFER, h265_parse_saving_metadata, &bin_store, NULL
            );
        }
    }

    // Start playing the pipeline
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Event loop to keep the pipeline running
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg;
    bool terminate = false;

    while (!terminate) {
        // Wait for a message for up to 100 milliseconds
        msg = gst_bus_timed_pop_filtered(
            bus, 100 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)
        );

        // Handle errors and EOS messages
        if (msg != NULL) {
            GError *err;
            gchar *debug_info;

            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg, &err, &debug_info);
                std::cerr << "Error from element " << GST_OBJECT_NAME(msg->src) << ": "
                          << err->message << std::endl;
                g_clear_error(&err);
                g_free(debug_info);
                terminate = true;
                break;
            case GST_MESSAGE_EOS:
                std::cout << "End-Of-Stream reached." << std::endl;
                terminate = true;
                break;
            default: break;
            }
            gst_message_unref(msg);
        }
    }

    // Clean up and shutdown the pipeline
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    bin_store.closeFile();
}

void parse_video_save_binary_jpeg(std::string &video_filepath)
{
    std::string bin_file_name = video_filepath;
    bin_file_name.replace(bin_file_name.end() - 3, bin_file_name.end(), "bin");

    spdlog::info("parse_video_save_binary: {}", bin_file_name);

    KeyValueStore bin_store(bin_file_name);

    std::cout << "bin_file_name: " << bin_file_name << std::endl;

    bin_store.openFile();

    std::string pipeline_str = " filesrc location=\"" + video_filepath +
                               "\" "
                               " ! matroskademux "
                               " ! jpegparse name=jpegparse "
                               " ! fakesink ";
    printf("pipeline_str: %s\n", pipeline_str.c_str());

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);

    if (!pipeline) {
        std::cerr << "Failed to create pipeline: " << error->message << std::endl;
        g_clear_error(&error);
        return;
    }

    // parse pts : h265parse src
    std::unique_ptr<GstElement, decltype(&gst_object_unref)> jpegparse{
        gst_bin_get_by_name(GST_BIN(pipeline), "jpegparse"), gst_object_unref
    };
    if (jpegparse.get() == nullptr) {
        std::cerr << "Failed to get h265parse element" << std::endl;
        return;
    } else {
        std::unique_ptr<GstPad, decltype(&gst_object_unref)> pad{
            gst_element_get_static_pad(jpegparse.get(), "src"), gst_object_unref
        };
        if (pad.get() != nullptr) {
            gst_pad_add_probe(
                pad.get(), GST_PAD_PROBE_TYPE_BUFFER, jpeg_parse_saving_metadata, &bin_store, NULL
            );
        }
    }

    // Start playing the pipeline
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Event loop to keep the pipeline running
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg;
    bool terminate = false;

    while (!terminate) {
        // Wait for a message for up to 100 milliseconds
        msg = gst_bus_timed_pop_filtered(
            bus, 100 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)
        );

        // Handle errors and EOS messages
        if (msg != NULL) {
            GError *err;
            gchar *debug_info;

            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg, &err, &debug_info);
                std::cerr << "Error from element " << GST_OBJECT_NAME(msg->src) << ": "
                          << err->message << std::endl;
                g_clear_error(&err);
                g_free(debug_info);
                terminate = true;
                break;
            case GST_MESSAGE_EOS:
                std::cout << "End-Of-Stream reached." << std::endl;
                terminate = true;
                break;
            default: break;
            }
            gst_message_unref(msg);
        }
    }

    // Clean up and shutdown the pipeline
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    bin_store.closeFile();
}

}  // namespace xvc