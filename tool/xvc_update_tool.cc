#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "updater.h"

namespace fs = std::filesystem;


static std::atomic<bool> g_terminate_flag(false);

void print_support_contact_info()
{
    spdlog::critical("-------------------- SUPPORT --------------------");
    spdlog::critical("If you encounter any difficulties, please contact Kontex support:");
    spdlog::critical("Email: support@kontexneuro.zohodesk.com");
    spdlog::critical("Website: https://help.kontex.io/portal/en/home");
    spdlog::critical("-------------------------------------------------");
}


void signal_handler(int signum)
{
    std::string signal_name_str;
    switch (signum) {
    case SIGINT: signal_name_str = "SIGINT (Ctrl+C)"; break;
    case SIGTERM: signal_name_str = "SIGTERM"; break;
    default: signal_name_str = "Unknown signal"; break;
    }

    if (g_terminate_flag.exchange(true)) {
        spdlog::critical(
            "Termination signal ({}, code {}) received again. Restoring default handler and "
            "re-raising signal for immediate exit.",
            signal_name_str,
            signum
        );
        std::signal(signum, SIG_DFL);
        std::raise(signum);
    } else {
        spdlog::warn(
            "Termination signal ({}, code {}) received. Requesting graceful shutdown.",
            signal_name_str,
            signum
        );
        spdlog::info(
            "Ongoing operations will attempt to complete. Temporary files will be cleaned up on "
            "exit."
        );
        spdlog::info(
            "If the program does not exit promptly, send the signal again to force termination "
            "(this might skip some cleanup)."
        );
    }
}


struct ThreadGuard {
    std::thread _thread;

public:
    explicit ThreadGuard(std::thread t) : _thread(std::move(t)) {}
    ~ThreadGuard()
    {
        if (_thread.joinable()) {
            spdlog::debug("ThreadGuard: Auto-joining thread on destruction.");
            _thread.join();
        }
    }
    ThreadGuard(const ThreadGuard &) = delete;
    ThreadGuard &operator=(const ThreadGuard &) = delete;
    ThreadGuard(ThreadGuard &&) = default;
    ThreadGuard &operator=(ThreadGuard &&) = default;

    void join()
    {
        if (_thread.joinable()) {
            _thread.join();
        }
    }
    bool joinable() const { return _thread.joinable(); }
};

int main(int argc, char *argv[])
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string server_address = "192.168.177.100";
    int server_port = 8000;
    int update_service_port = 8001;

    std::string cloud_endpoint = "https://xvc001.sgp1.digitaloceanspaces.com";
    std::string update_dir_str = "updates";

    std::string force_api_version_str;
    std::string force_build_version_str;

    std::string calculate_hash_file;
    bool get_local_versions_flag = false;
    bool skip_local_version_check_flag = false;

    CLI::App app{"XVC Update Check and Download Tool"};
    app.add_option("-s,--server", server_address, "Device server address (IP or hostname)")
        ->default_val(server_address);
    app.add_option("-p,--port", server_port, "Device server port (for info, e.g. version checks)")
        ->default_val(server_port);
    app.add_option(
           "--update-port",
           update_service_port,
           "Device update service port (for handshake, file transfer)"
    )
        ->default_val(update_service_port);

    app.add_option("--cloud-endpoint", cloud_endpoint, "Cloud storage endpoint URL (bucket base)")
        ->default_val(cloud_endpoint);
    app.add_option("-d,--download-dir", update_dir_str, "Directory for downloaded updates")
        ->default_val(update_dir_str);

    app.add_option(
        "--force-api",
        force_api_version_str,
        "Force update check for this API version (e.g., 0.0.8)"
    );
    app.add_option(
        "--force-build",
        force_build_version_str,
        "Force update check for this Build version (e.g., 0.0.12)"
    );

    app.add_option(
        "-c,--calculate-hash", calculate_hash_file, "Calculate SHA256 hash for a file and exit"
    );
    app.add_flag(
        "-g,--get-local-versions",
        get_local_versions_flag,
        "Get and display local device API and Build versions and exit"
    );
    app.add_flag(
        "--skip-local-check",
        skip_local_version_check_flag,
        "Skip local version check; requires --force-api (and optionally --force-build) to directly "
        "download from cloud."
    );

    CLI11_PARSE(app, argc, argv);


    if (g_terminate_flag.load(std::memory_order_relaxed)) {
        spdlog::warn("Termination signal received during startup. Exiting.");
        print_support_contact_info();
        return EXIT_FAILURE;
    }

    try {
        // Handle calculate-hash option
        if (!calculate_hash_file.empty()) {
            if (g_terminate_flag.load(std::memory_order_relaxed)) {
                spdlog::warn("Termination signal received before hash calculation. Exiting.");
                print_support_contact_info();
                return EXIT_FAILURE;
            }
            auto hash = xvc::calculate_sha256(calculate_hash_file);
            if (hash) {
                // spdlog::info already prints the hash inside calculate_sha256
                // fmt::print("SHA256 Hash for {}: {}\n", calculate_hash_file, *hash); //
                // Alternative direct print
                return EXIT_SUCCESS;
            } else {
                spdlog::error("Failed to calculate hash for file: {}", calculate_hash_file);
                print_support_contact_info();
                return EXIT_FAILURE;
            }
        }

        // Handle get-local-versions option
        if (get_local_versions_flag) {
            if (g_terminate_flag.load(std::memory_order_relaxed)) {
                spdlog::warn(
                    "Termination signal received before fetching local versions. Exiting."
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }
            spdlog::info(
                "Fetching local device versions as requested by -g,--get-local-versions flag..."
            );
            std::string device_server_base_url =
                "http://" + server_address + ":" + std::to_string(server_port);
            auto local_versions_opt = xvc::get_local_device_versions(device_server_base_url);

            if (local_versions_opt) {
                if (local_versions_opt->api_version.major == 0 &&
                    local_versions_opt->api_version.minor == 0 &&
                    local_versions_opt->api_version.patch == 0 &&
                    local_versions_opt->build_version.major == 0 &&
                    local_versions_opt->build_version.minor == 0 &&
                    local_versions_opt->build_version.patch == 0) {
                    spdlog::warn(
                        "Fetched local versions appear to be default/uninitialized (0.0.0)."
                    );
                    spdlog::info(
                        "Local API Version: {}", local_versions_opt->api_version.to_string()
                    );
                    spdlog::info(
                        "Local Build Version: {}", local_versions_opt->build_version.to_string()
                    );
                    // Depending on requirements, you might still consider this a success or a
                    // specific type of failure/warning. For now, exiting success as the query was
                    // made.
                } else {
                    spdlog::info("Successfully fetched local device versions:");
                    spdlog::info(
                        "Local API Version: {}", local_versions_opt->api_version.to_string()
                    );
                    spdlog::info(
                        "Local Build Version: {}", local_versions_opt->build_version.to_string()
                    );
                }
                return EXIT_SUCCESS;
            } else {
                spdlog::error(
                    "Failed to retrieve local device versions from device server at {}:{}.",
                    server_address,
                    server_port
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }
        }

        // Parse forced versions if specified (used by both paths, but mandatory for one)
        std::optional<xvc::Version> force_api_v_opt;
        if (!force_api_version_str.empty()) {
            auto parsed_v = xvc::Version::from_string(force_api_version_str);
            if (!parsed_v) {
                spdlog::error("Invalid format for --force-api version: {}", force_api_version_str);
                print_support_contact_info();
                return EXIT_FAILURE;
            }
            force_api_v_opt = *parsed_v;
        }

        std::optional<xvc::Version> force_build_v_opt;
        if (!force_build_version_str.empty()) {
            auto parsed_v = xvc::Version::from_string(force_build_version_str);
            if (!parsed_v) {
                spdlog::error(
                    "Invalid format for --force-build version: {}", force_build_version_str
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }
            force_build_v_opt = *parsed_v;
        }


        // Use the user-specified download directory. This directory will not be cleaned up
        // automatically.
        fs::path download_directory(update_dir_str);

        // Check before creating directory
        if (g_terminate_flag.load(std::memory_order_relaxed)) {
            spdlog::warn(
                "Termination signal received before creating download directory. Exiting."
            );
            print_support_contact_info();
            return EXIT_FAILURE;
        }

        std::error_code ec;
        fs::create_directories(download_directory, ec);
        if (ec) {
            spdlog::error(
                "Failed to create download directory {}: {}",
                download_directory.string(),
                ec.message()
            );
            print_support_contact_info();
            return EXIT_FAILURE;
        }
        spdlog::info("Using download directory: {}", download_directory.string());

        xvc::UpdateOrchestrationResult result;

        if (g_terminate_flag.load(std::memory_order_relaxed)) {
            spdlog::warn("Termination signal received before update check/download. Exiting.");
            print_support_contact_info();
            return EXIT_FAILURE;
        }

        if (skip_local_version_check_flag) {
            spdlog::info("Mode: Skip Local Check & Force Download.");
            if (!force_api_v_opt) {
                spdlog::error("--skip-local-check mode requires --force-api to be specified.");


                print_support_contact_info();
                return EXIT_FAILURE;
            }



            spdlog::info(
                "Calling force_download_updates with API: {} and optional Build: {}",
                force_api_v_opt->to_string(),
                force_build_v_opt ? force_build_v_opt->to_string() : "latest for API"
            );

            result = xvc::force_download_updates(
                cloud_endpoint,
                download_directory,
                *force_api_v_opt,  // Dereference, as it's logically mandatory here
                force_build_v_opt
            );

        } else {
            // Standard mode: check local versions, then check for updates or act on force flags.
            spdlog::info(
                "Mode: Standard Update Check (with local version comparison or force flags)."
            );
            std::string device_server_base_url =
                "http://" + server_address + ":" + std::to_string(server_port);

            spdlog::info("Calling check_for_and_download_updates...");
            result = xvc::check_for_and_download_updates(
                device_server_base_url,
                cloud_endpoint,
                download_directory,
                force_api_v_opt,
                force_build_v_opt
            );
        }

        spdlog::info("-------------------- Operation Result --------------------");
        spdlog::info("Success: {}", result.success);
        if (!result.error_message.empty()) {
            spdlog::error("Error Message: {}", result.error_message);
        }

        if (result.local_api_version)
            spdlog::info("Local API Version: {}", result.local_api_version->to_string());
        if (result.local_build_version)
            spdlog::info("Local Build Version: {}", result.local_build_version->to_string());

        if (result.latest_remote_api_version)
            spdlog::info(
                "Latest Remote API Version Found: {}", result.latest_remote_api_version->to_string()
            );
        if (result.target_remote_api_version)
            spdlog::info(
                "Target Remote API Version Used for Check: {}",
                result.target_remote_api_version->to_string()
            );
        if (result.target_remote_build_version)
            spdlog::info(
                "Target Remote Build Version Identified: {}",
                result.target_remote_build_version->to_string()
            );

        spdlog::info("Update Available and Downloaded: {}", result.update_available_and_downloaded);

        if (result.update_available_and_downloaded) {
            spdlog::info("Downloaded Package Path: {}", result.downloaded_package_path.string());
            if (result.downloaded_package_metadata) {
                spdlog::info(
                    "Downloaded Package Hash (from metadata): {}",
                    result.downloaded_package_metadata->package_hash_sha256
                );
                if (!result.downloaded_package_metadata->release_notes.empty()) {
                    spdlog::info(
                        "Release Notes:\n{}", result.downloaded_package_metadata->release_notes
                    );
                }
            }
        }
        spdlog::info("-----------------------------------------------------------");


        if (!result.success) {
            // Error message already logged if present in result.error_message
            // If no specific error message was in the result, log a generic one.
            if (result.error_message.empty()) {
                spdlog::error("The update operation failed. See logs for details.");
            }
            print_support_contact_info();
            return EXIT_FAILURE;
        }

        // At this point, result.success is true.
        // Handle the transfer if an update was downloaded.
        if (result.update_available_and_downloaded) {
            spdlog::info(
                "Update package downloaded. Proceeding to transfer to device server: {} on update "
                "port {}",
                server_address,
                update_service_port
            );

            if (g_terminate_flag.load(std::memory_order_relaxed)) {
                spdlog::warn(
                    "Termination signal received before starting transfer process. Exiting."
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }

            // 1. Perform Handshake with the device's update service
            spdlog::info("Step (Transfer) 1: Performing handshake with update service...");
            xvc::HandshakeResponse handshake_response =
                xvc::perform_handshake(server_address, update_service_port);

            if (!handshake_response.success) {
                spdlog::error(
                    "Handshake with update service at {}:{} failed: {}",
                    server_address,
                    update_service_port,
                    handshake_response.error_message
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }
            // Consider redacting or partially logging token in production environments for
            // security.
            spdlog::info("Handshake with update service successful. Token acquired.");

            // 2. Prepare for File Transfer
            fs::path package_to_transfer = result.downloaded_package_path;
            std::string package_filename = package_to_transfer.filename().string();

            // Hash from metadata is already verified against the downloaded file.
            std::string package_hash = result.downloaded_package_metadata->package_hash_sha256;
            size_t package_size = 0;
            try {
                if (!fs::exists(package_to_transfer) || !fs::is_regular_file(package_to_transfer)) {
                    spdlog::error(
                        "Downloaded package {} does not exist or is not a regular file. Cannot "
                        "transfer.",
                        package_to_transfer.string()
                    );
                    print_support_contact_info();
                    return EXIT_FAILURE;
                }
                package_size = fs::file_size(package_to_transfer);
                if (package_size == 0) {
                    spdlog::error(
                        "Downloaded package {} is empty. Cannot transfer.",
                        package_to_transfer.string()
                    );
                    print_support_contact_info();
                    return EXIT_FAILURE;
                }
            } catch (const fs::filesystem_error &e) {
                spdlog::error(
                    "Failed to get file size for {}: {}", package_to_transfer.string(), e.what()
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }

            spdlog::info(
                "Step (Transfer) 2: Preparing file transfer for {} (size: {} bytes, hash: {})...",
                package_filename,
                package_size,
                package_hash
            );
            std::string transfer_id;
            bool prepare_success = xvc::prepare_file_transfer(
                server_address,
                update_service_port,
                handshake_response.token,
                package_filename,
                package_hash,
                package_size,
                transfer_id
            );

            if (!prepare_success) {
                spdlog::error(
                    "Failed to prepare file transfer for {}. Check server logs on {}:{} for "
                    "details.",
                    package_filename,
                    server_address,
                    update_service_port
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }
            spdlog::info("File transfer prepared successfully. Transfer ID: {}", transfer_id);

            // Start streaming logs from the server in a background thread using ThreadGuard
            spdlog::info(
                "Starting server-side log streaming in background for transfer ID: {}...",
                transfer_id
            );
            ThreadGuard log_stream_thread_guard{xvc::stream_server_logs(
                server_address, update_service_port, transfer_id, handshake_response.token
            )};

            if (g_terminate_flag.load(std::memory_order_relaxed)) {
                spdlog::warn(
                    "Termination signal received after starting log stream, before file transfer. "
                    "Exiting."
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }

            // 3. Transfer File
            spdlog::info(
                "Step (Transfer) 3: Transferring file {} to {}:{} (Transfer ID: {})...",
                package_filename,
                server_address,
                update_service_port,
                transfer_id
            );

            auto transfer_progress_callback = [](const xvc::FileTransferProgress &progress) {
                // This callback is invoked by updater.cc's wrapper only when progress actually
                // changes.
                spdlog::info(
                    "Upload progress: {:.1f}% ({}/{} bytes)",
                    progress.progress_percentage,
                    progress.bytes_transferred,
                    progress.total_bytes
                );
            };

            bool transfer_success = xvc::transfer_file_and_update_server(
                server_address,
                update_service_port,
                handshake_response.token,
                package_to_transfer,
                transfer_id,
                transfer_progress_callback
            );

            // Wait for the log streaming thread to complete before proceeding
            spdlog::info(
                "Waiting for server-side log streaming to complete for transfer ID: {}...",
                transfer_id
            );
            if (log_stream_thread_guard.joinable()) {
                log_stream_thread_guard.join();
            }
            spdlog::info("Server-side log streaming finished for transfer ID: {}.", transfer_id);

            if (!transfer_success) {
                spdlog::error(
                    "Server update failed for {}. Check server logs on {}:{} for details (logs "
                    "should be above).",
                    package_filename,
                    server_address,
                    update_service_port
                );
                print_support_contact_info();
                return EXIT_FAILURE;
            }
            spdlog::info(
                "File {} transferred successfully to device update service at {}:{}.",
                package_filename,
                server_address,
                update_service_port
            );

            spdlog::info(
                "Update package delivered. Server-side processing logs have been streamed."
            );

            return EXIT_SUCCESS;

        } else {
            // result.success is true, but !result.update_available_and_downloaded
            // This means the check was successful, but no update was downloaded (e.g., up-to-date
            // or no suitable package).
            if (result.error_message.empty()) {
                spdlog::info(
                    "Update check completed. No update was downloaded (either up-to-date or no "
                    "suitable package found). No transfer needed."
                );
            } else {
                // This case (result.success true, but error_message not empty and no download)
                // might indicate an unusual scenario.
                spdlog::warn(
                    "Update check reported success but also an error message and no download: {}. "
                    "No transfer attempted.",
                    result.error_message
                );
            }
            return EXIT_SUCCESS;
        }


    } catch (const CLI::ParseError &e) {
        int exit_code = app.exit(e);
        print_support_contact_info();
        return exit_code;
    } catch (const std::exception &e) {
        spdlog::error("Unhandled Standard Exception: {}", e.what());
        print_support_contact_info();
        return EXIT_FAILURE;
    } catch (const fs::filesystem_error &e) {
        spdlog::error(fmt::format("Unhandled Filesystem Error: {}", e.what()));
        print_support_contact_info();
        return EXIT_FAILURE;
    } catch (...) {
        spdlog::error("An unknown error occurred.");
        print_support_contact_info();
        return EXIT_FAILURE;
    }
}