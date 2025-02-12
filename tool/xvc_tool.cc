#include <fmt/core.h>
#include <gst/gst.h>
#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <gst/gstpad.h>
#include <gst/gstpipeline.h>
#include <spdlog/spdlog.h>

#include <boost/program_options.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include "server.h"


int main(int argc, char *argv[])
{
    namespace po = boost::program_options;
    po::options_description desc("Usage");

    // clang-format off
    desc.add_options()
        ("help,h", "Show help options")
        ("file,f", po::value<std::string>(), "file to write or verify")
        ("logs", "List server logs")
    ;
    // clang-format on

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        desc.print(std::cout);
        return EXIT_SUCCESS;
    }

    if (vm.count("logs")) {
        if (vm.count("file")) {
            auto filename = vm["file"].as<std::string>();
            auto content = xvc::server_logs(filename);
            fmt::print("{}", content);
        } else {
            auto logs = xvc::server_logs();
            fmt::print("{}", logs);
        }
        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}