#include <fmt/core.h>
#include <gst/gst.h>
#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <gst/gstpad.h>
#include <gst/gstpipeline.h>
#include <spdlog/spdlog.h>

#include <boost/program_options.hpp>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "camera.h"
#include "xdaqmetadata/xdaqmetadata.h"



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
        // auto ip = vm["ip"].as<std::string>();

        auto cameras = Camera::cameras();
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
        std::unique_ptr<GstPad, decltype(&gst_object_unref)> pad(
            gst_element_get_static_pad(parser.get(), "src"), gst_object_unref
        );

        gst_pad_add_probe(pad.get(), GST_PAD_PROBE_TYPE_BUFFER, parse_h265_metadata, NULL, NULL);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);

        fmt::println("set pipeline to playing");
        return 0;
    }

    if (vm.count("save")) {
        std::string filename = vm["file"].as<std::string>();

        gst_init(&argc, &argv);
        auto pipeline = gst_pipeline_new(NULL);
        gst_object_unref(pipeline);
        return 0;
    }

    return 1;
}