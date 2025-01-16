#include <spdlog/spdlog.h>
#include <xdaqvc/xvc.h>

#include <boost/program_options.hpp>
#include <filesystem>
#include <iostream>


namespace po = boost::program_options;
namespace fs = std::filesystem;

int main(int argc, char *argv[])
{
    std::string server_address;
    int server_port;
    int update_server_port;
    std::string version_table_url;
    std::string update_dir;
    bool skip_version_check = false;
    std::string target_version;

    // Setup command line options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "Show help message")
        ("server,s", po::value<std::string>(&server_address)->default_value("192.168.177.100"), 
         "Server address (IP or hostname)")
        ("port,p", po::value<int>(&server_port)->default_value(8000), 
         "Server port to be updated")
        ("update-port,u", po::value<int>(&update_server_port)->default_value(8001), 
         "Update server port")
        ("version-table,t", 
         po::value<std::string>(&version_table_url)
         ->default_value("https://xvc001.sgp1.digitaloceanspaces.com/versions.json"),
         "Version table URL")
        ("update-dir,d", po::value<std::string>(&update_dir)->default_value("updates"),
         "Directory for downloaded updates")
        ("force,f", po::bool_switch(&skip_version_check),
         "Skip version check")
        ("version,v", po::value<std::string>(&target_version),
         "Target version to update to (optional)")
    ;

    try {
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 0;
        }

        po::notify(vm);

        // Set client version (using a dummy version that should work with most updates)
        xvc::Version client_version{999, 999, 999};

        // Parse target version if specified
        std::optional<xvc::Version> force_version;
        if (!target_version.empty()) {
            auto parsed_version = xvc::Version::from_string(target_version);
            if (!parsed_version) {
                spdlog::error("Invalid target version format: {}", target_version);
                return 1;
            }
            force_version = *parsed_version;
        }

        // Perform update
        auto result = xvc::update_server(
            server_address,
            server_port,
            update_server_port,
            version_table_url,
            update_dir,
            client_version,
            skip_version_check,
            force_version
        );

        if (!result.success) {
            spdlog::error("Update failed: {}", result.error_message);
            return 1;
        }

        if (!result.update_needed) {
            spdlog::info(
                "Server is already up to date (version {})", result.current_version.to_string()
            );
            return 0;
        }

        spdlog::info(
            "Successfully updated server from {} to {}",
            result.current_version.to_string(),
            result.available_version.to_string()
        );
        return 0;

    } catch (const po::error &e) {
        spdlog::error("Command line error: {}", e.what());
        std::cerr << desc << "\n";
        return 1;
    } catch (const std::exception &e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }
}