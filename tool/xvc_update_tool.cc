#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <cstdlib>

#include "updater.h"

int main(int argc, char *argv[])
{
    // TODO: Make this updater CLI part of tvcli
    std::string server_address = "192.168.177.100";
    auto server_port = 8000;
    auto update_server_port = 8001;
    std::string version_table_url = "https://xvc001.sgp1.digitaloceanspaces.com/versions.json";
    std::string update_dir = "updates";
    auto skip_version_check = false;
    std::string target_version;
    std::string calculate_hash_file;
    bool get_server_version = false;

    CLI::App app{"Thor Vision Server Updater"};
    app.add_option("-s,--server", server_address, "Server address (IP or hostname)")
        ->default_val(server_address);
    app.add_option("-p,--port", server_port, "Server port to be updated")->default_val(server_port);
    app.add_option("-u,--update-port", update_server_port, "Update server port")
        ->default_val(update_server_port);
    app.add_option("-t,--version-table", version_table_url, "Version table URL")
        ->default_val(version_table_url);
    app.add_option("-d,--update-dir", update_dir, "Directory for downloaded updates")
        ->default_val(update_dir);
    app.add_flag("-f,--force", skip_version_check, "Skip version check");
    app.add_option("-v,--version", target_version, "Target version to update to (optional)");
    app.add_option("-c,--calculate-hash", calculate_hash_file, "Calculate SHA256 hash for a file");
    app.add_flag(
        "-g,--get-server-version", get_server_version, "Get and display the server version"
    );

    CLI11_PARSE(app, argc, argv);

    try {
        // Handle calculate-hash option
        if (!calculate_hash_file.empty()) {
            auto hash = xvc::calculate_sha256(calculate_hash_file);
            if (hash) {
                return EXIT_SUCCESS;
            } else {
                spdlog::error("Failed to calculate hash for file: {}", calculate_hash_file);
                return EXIT_FAILURE;
            }
        }

        // Handle get-server-version option
        if (get_server_version) {
            auto version = xvc::get_server_version(server_address, server_port);
            if (version) {
                spdlog::info("Server version: {}", version->to_string());
                return EXIT_SUCCESS;
            } else {
                spdlog::error("Failed to get server version");
                return EXIT_FAILURE;
            }
        }

        // Set client version (using a dummy version that should work with most updates)
        xvc::Version client_version{999, 999, 999};

        // Parse target version if specified
        std::optional<xvc::Version> force_version;
        if (!target_version.empty()) {
            auto parsed_version = xvc::Version::from_string(target_version);
            if (!parsed_version) {
                spdlog::error("Invalid target version format: {}", target_version);
                return EXIT_FAILURE;
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
            return EXIT_FAILURE;
        }

        if (!result.update_needed) {
            spdlog::info(
                "Server is already up to date (version {})", result.current_version.to_string()
            );
            return EXIT_SUCCESS;
        }

        spdlog::info(
            "Successfully updated server from {} to {}",
            result.current_version.to_string(),
            result.available_version.to_string()
        );
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        spdlog::error("Error: {}", e.what());
        return EXIT_FAILURE;
    }
}