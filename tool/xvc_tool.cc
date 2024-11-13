#include <fmt/core.h>
#include <gst/codecparsers/gsth265parser.h>
#include <gst/gst.h>
#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <gst/gstpad.h>
#include <gst/gstpipeline.h>
#include <spdlog/spdlog.h>

#include <boost/program_options.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "libxvc.h"
#include "src/camera.h"

int main(int argc, char *argv[])
{
    namespace po = boost::program_options;
    po::options_description desc("Usage");

    // clang-format off
    desc.add_options()
        ("help,h", "Show help options")
        ("file,f", po::value<std::string>(), "file to write or verify")
        ("list,l", po::value<bool>(), "Print cameras and their settings")
        ("ip,i", po::value<std::string>(), "Input URL ex. 192.168.177.100:8000")
        // ("list,l", po::value<std::string>(), "Print cameras and their settings")
        ("open,o", "Open a new stream")
        ("save,s", "Save as video file")
        ("parse,p", "Parse a video file")
        // ("reboot", po::bool_switch()->default_value(false), "Reboot device to target mode and exit")
        // ("index,i", po::value<int>()->default_value(0), "index of device to open")
    ;
    // clang-format on

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        fmt::println("help");

        desc.print(std::cout);
        return 0;
    }

    if (vm.count("list")) {
        fmt::println("list");

        auto ip = vm["ip"].as<std::string>();
        auto cameras = xvc::list_cameras(ip);
        fmt::println("{}", cameras);
        return 0;
    }

    if (vm.count("open")) {
        fmt::println("open");
        gst_init(&argc, &argv);

        std::string pipeline_in_string =
            "srtsrc name=sink uri=srt://192.168.177.100:9000 ! h265parse name=parser ! "
            "video/x-h265,stream-format=byte-stream,alignment=au ! fakesink";

        std::fstream fout;
        fout.open("video_latency_check.csv", std::ios::out | std::ios::trunc);

        GError *err = NULL;
        GstElement *pipeline = gst_parse_launch(pipeline_in_string.c_str(), &err);
        if (pipeline == NULL) {
            printf("Could not create pipeline. Cause: %s\n", err->message);
            return 0;
        }

        std::unique_ptr<GstElement, decltype(&gst_object_unref)> parser(
            gst_bin_get_by_name(GST_BIN(pipeline), "parser"), gst_object_unref
        );
        if (parser.get() != nullptr) {
            std::unique_ptr<GstPad, decltype(&gst_object_unref)> pad(
                gst_element_get_static_pad(parser.get(), "src"), gst_object_unref
            );

            gst_pad_add_probe(
                pad.get(),
                GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *pad, GstPadProbeInfo *info, gpointer user_data) -> GstPadProbeReturn {
                    auto buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                    gpointer state = NULL;
                    GstMapInfo map_info;

                    GstH265Parser *nalu_parser = gst_h265_parser_new();
                    GstH265NalUnit nalu;
                    for (int k = 0; k < gst_buffer_n_memory(buffer); ++k) {
                        GstMemory *mem_in_buffer = gst_buffer_get_memory(buffer, k);
                        gst_memory_map(mem_in_buffer, &map_info, GST_MAP_READ);

                        GstH265ParserResult parse_result = gst_h265_parser_identify_nalu_unchecked(
                            nalu_parser, map_info.data, 0, map_info.size, &nalu
                        );
                        // printf("parse_result: %d\n", parse_result);
                        if (parse_result == GST_H265_PARSER_OK ||
                            parse_result == GST_H265_PARSER_NO_NAL_END ||
                            parse_result == GST_H265_PARSER_NO_NAL)  // GST_H265_PARSER_NO_NAL_END)
                        {
                            // printf("type: %d\n", nalu.type);
                            if (/*nalu.type == GST_H265_NAL_SLICE || true|| */ nalu.type ==
                                    GST_H265_NAL_PREFIX_SEI ||
                                nalu.type ==
                                    GST_H265_NAL_SUFFIX_SEI)  // 在I帧或P帧前面插SEI,也可以在其他位置插入，根据nalu.type判断
                            {
                                GArray *array =
                                    g_array_new(false, false, sizeof(GstH265SEIMessage));
                                // printf("before parse SEI\n");
                                gst_h265_parser_parse_sei(nalu_parser, &nalu, &array);
                                GstH265SEIMessage sei_msg =
                                    g_array_index(array, GstH265SEIMessage, 0);
                                GstH265RegisteredUserData register_user_data =
                                    sei_msg.payload.registered_user_data;
                                std::string str(
                                    (const char *) register_user_data.data, register_user_data.size
                                );


                                std::array<uint64_t, 4> timestamp;
                                std::memcpy(
                                    &timestamp, register_user_data.data, register_user_data.size
                                );
                                auto const time = std::chrono::current_zone()->to_local(
                                    std::chrono::system_clock::now()
                                );
                                if (timestamp[1] == 0) continue;
                                // auto current_time = std::chrono::high_resolution_clock::now();

                                fmt::println(
                                    "buffer pts = {}, timestamp[0]: {:08x}, timestamp[1]: {:08x}, "
                                    "timestamp[2]: "
                                    "{:08x}, timestamp[3]: {:08x}\r",
                                    buffer->pts,
                                    timestamp[0],
                                    timestamp[1],
                                    timestamp[2],
                                    timestamp[3]
                                );

                                auto fout = (std::fstream *) user_data;

                                *fout << time << "," << timestamp[1] << "\n";
                                // fmt::print("register_user_data.size: {}",
                                // register_user_data.size); std::cout << "\nSEI: " << str <<
                                // "\n\n";
                                g_array_free(array, true);
                                printf("\n");
                            }
                        }
                    }
                    gst_h265_parser_free(nalu_parser);

                    return GST_PAD_PROBE_OK;
                },
                &fout,
                NULL
            );
        }

        gst_element_set_state(pipeline, GST_STATE_PLAYING);

        // xvc::start_stream(0, "video/x-raw,format=YUY2,width=640,height=480,framerate=30/1",
        // 9000); xvc::Camera::start();

        fmt::println("set pipeline to playing");

        gst_deinit();
        return 0;
    }

    if (vm.count("save")) {
        std::string filename = vm["file"].as<std::string>();

        gst_init(&argc, &argv);
        auto pipeline = gst_pipeline_new(NULL);
        // xvc::save_video_file(GST_PIPELINE(pipeline), filename);
        gst_object_unref(pipeline);
        gst_deinit();
        return 0;
    }

    return 1;
}