#include "updater.h"

#include <cpr/cpr.h>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <iomanip>  // For std::put_time, std::get_time
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <regex>
#include <sstream>
#include <thread>
#include <tuple>



using namespace std::chrono_literals;


namespace
{
auto constexpr OK = 200;

// size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream)
// {
//     return fwrite(ptr, size, nmemb, stream);
// }

// std::string bytes_to_hex(const unsigned char *bytes, size_t len)
// {
//     std::stringstream ss;
//     ss << std::hex << std::setfill('0');
//     for (size_t i = 0; i < len; i++) {
//         ss << std::setw(2) << static_cast<int>(bytes[i]);
//     }
//     return ss.str();
// }

// bool handle_response(const cpr::Response &response)
// {
//     if (response.status_code == OK) {
//         auto json_response = nlohmann::json::parse(response.text);
//         return json_response["status"] == "success";
//     }

//     spdlog::error(
//         "File transfer failed with status code: {} ({})", response.status_code, response.text
//     );
//     return false;
// }
}  // namespace


namespace xvc
{
std::optional<std::string> calculate_sha256(const fs::path &filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        spdlog::error("Failed to open file for hashing: {}", filepath.string());
        return std::nullopt;
    }

    EVP_MD_CTX *mdctx = nullptr;
    const EVP_MD *md = nullptr;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    std::string result_str;

    try {
        md = EVP_get_digestbyname("SHA256");
        if (md == nullptr) {
            spdlog::error("Failed to get SHA256 digest");
            return std::nullopt;
        }

        mdctx = EVP_MD_CTX_new();
        if (mdctx == nullptr) {
            spdlog::error("Failed to create EVP_MD_CTX");
            return std::nullopt;
        }

        if (1 != EVP_DigestInit_ex(mdctx, md, nullptr)) {
            spdlog::error("Failed to initialize EVP digest");
            EVP_MD_CTX_free(mdctx);
            return std::nullopt;
        }

        char buffer[4096];
        while (file.read(buffer, sizeof(buffer))) {
            if (1 != EVP_DigestUpdate(mdctx, buffer, file.gcount())) {
                spdlog::error("Failed to update EVP digest");
                EVP_MD_CTX_free(mdctx);
                return std::nullopt;
            }
        }
        if (file.gcount() > 0) {
            if (1 != EVP_DigestUpdate(mdctx, buffer, file.gcount())) {
                spdlog::error("Failed to update EVP digest (final part)");
                EVP_MD_CTX_free(mdctx);
                return std::nullopt;
            }
        }

        if (1 != EVP_DigestFinal_ex(mdctx, hash, &hash_len)) {
            spdlog::error("Failed to finalize EVP digest");
            EVP_MD_CTX_free(mdctx);
            return std::nullopt;
        }

        EVP_MD_CTX_free(mdctx);  // mdctx is consumed by EVP_DigestFinal_ex on success, but free in
                                 // case of error path not taken

        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < hash_len; i++) {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }
        result_str = ss.str();

    } catch (const std::exception &e) {
        spdlog::error("Exception during SHA256 calculation: {}", e.what());
        if (mdctx != nullptr) EVP_MD_CTX_free(mdctx);
        return std::nullopt;
    }


    spdlog::info("Calculated hash: {}", result_str);
    return result_str;
}

DownloadResult download_and_verify(
    const std::string &url, const std::string &expected_hash, const fs::path &output_path
)
{
    DownloadResult result{false, ""};
    constexpr auto MAX_RETRIES = 3;
    constexpr std::chrono::seconds TIMEOUT{30};

    try {
        // First, make a HEAD request to get the expected file size
        auto head_response = cpr::Head(cpr::Url{url}, cpr::VerifySsl{false}, cpr::Timeout{2s});
        if (head_response.status_code != OK) {
            result.error_message =
                fmt::format("Failed to get file information: {}", head_response.status_code);
            return result;
        }

        // Get expected file size from header
        size_t expected_size = 0;
        if (head_response.header.count("Content-Length") > 0) {
            expected_size = std::stoull(head_response.header["Content-Length"]);
        }

        // Retry loop
        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
            std::ofstream file(output_path, std::ios::binary);
            if (!file) {
                result.error_message = "Failed to open output file for writing";
                return result;
            }

            // Track last reported progress
            size_t last_progress = 0;

            // Progress callback
            auto progress_callback = [&last_progress](
                                         size_t downloadTotal,
                                         size_t downloadNow,
                                         [[maybe_unused]] size_t uploadTotal,
                                         [[maybe_unused]] size_t uploadNow,
                                         [[maybe_unused]] intptr_t userdata
                                     ) -> bool {
                if (downloadTotal > 0) {
                    float progress_percentage =
                        static_cast<float>(downloadNow) / downloadTotal * 100.0f;
                    if (downloadNow != last_progress) {
                        last_progress = downloadNow;
                        spdlog::info(
                            "Download progress: {:.1f}% ({}/{} bytes)",
                            progress_percentage,
                            downloadNow,
                            downloadTotal
                        );
                    }
                }
                return true;  // Continue transfer
            };

            // Perform the download
            auto response = cpr::Download(
                file,
                cpr::Url{url},
                cpr::VerifySsl{false},
                cpr::Timeout{TIMEOUT},
                cpr::ProgressCallback(progress_callback)
            );

            file.close();

            if (response.status_code != OK) {
                spdlog::warn(
                    "Download attempt {} failed with status code: {}. Retrying...",
                    attempt,
                    response.status_code
                );
                std::this_thread::sleep_for(std::chrono::seconds(attempt));
                continue;
            }

            // Verify file size
            if (expected_size > 0) {
                auto actual_size = fs::file_size(output_path);
                if (actual_size != expected_size) {
                    spdlog::warn(
                        "File size mismatch. Expected: {}, Got: {}. Retrying...",
                        expected_size,
                        actual_size
                    );
                    fs::remove(output_path);
                    continue;
                }
            }

            // Calculate and verify hash
            auto calculated_hash = calculate_sha256(output_path);
            if (!calculated_hash) {
                spdlog::warn("Failed to calculate file hash. Retrying...");
                fs::remove(output_path);
                continue;
            }

            if (*calculated_hash != expected_hash) {
                spdlog::warn(
                    "Hash verification failed. Expected: {}, Got: {}. Retrying...",
                    expected_hash,
                    *calculated_hash
                );
                fs::remove(output_path);
                continue;
            }

            // If we get here, all verifications passed
            result.success = true;
            return result;
        }

        // If retries are exhausted
        result.error_message = "Failed to download file after multiple attempts.";
        return result;

    } catch (const std::exception &e) {
        result.error_message = fmt::format("Download failed: {}", e.what());
        if (fs::exists(output_path)) {
            fs::remove(output_path);
        }
        return result;
    }
}


HandshakeResponse perform_handshake(const std::string &server_address, int port)
{
    HandshakeResponse response{false, "", "", {}};

    try {
        auto url = fmt::format("http://{}:{}/handshake", server_address, port);

        spdlog::info("Attempting handshake with server at {}", url);

        auto http_response =
            cpr::Get(cpr::Url{url}, cpr::Timeout{2s}, cpr::Header{{"User-Agent", "XVC-Client"}});

        if (http_response.status_code == OK) {
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
                    auto expire_timestamp = json_response["expires"].get<int64_t>();
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
        auto url = fmt::format("http://{}:{}/prepare-transfer", server_address, port);

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

        if (response.status_code == OK) {
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

bool transfer_file_and_update_server(
    const std::string &server_address, int port, const std::string &token,
    const fs::path &file_path, const std::string &transfer_id,
    std::function<void(const FileTransferProgress &)> progress_callback
)
{
    auto timeout = std::chrono::seconds{90};  // Increased timeout from 30 to 90 seconds
    try {
        if (!fs::exists(file_path)) {
            spdlog::error("File does not exist: {}", file_path.string());
            return false;
        }
        if (!fs::is_regular_file(file_path)) {
            spdlog::error("Invalid file type: {}", file_path.string());
            return false;
        }

        auto file_size = fs::file_size(file_path);
        if (file_size == 0) {
            spdlog::error("File is empty: {}", file_path.string());
            return false;
        }

        auto url = fmt::format("http://{}:{}/transfer/{}", server_address, port, transfer_id);

        cpr::Multipart multipart{};
        multipart.parts.emplace_back("file", cpr::File{file_path.string()});

        cpr::Header headers = {{"Authorization", fmt::format("Bearer {}", token)}};

        // Deduplication logic in progress callback
        auto progress_callback_wrapper = [&progress_callback, file_size](
                                             size_t, size_t, size_t, size_t ul_now, intptr_t
                                         ) -> bool {
            static size_t last_progress = 0;
            auto actual_progress = std::clamp(ul_now, size_t{0}, file_size);

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

        auto response = cpr::Post(
            cpr::Url{url},
            headers,
            multipart,
            progress_callback ? cpr::ProgressCallback(progress_callback_wrapper)
                              : cpr::ProgressCallback{},
            cpr::Timeout{timeout}
        );

        if (response.status_code == OK) {
            auto json_response = nlohmann::json::parse(response.text);
            return json_response["status"] == "success";
        } else {
            spdlog::error(
                "File transfer and server update failed with status code: {} ({})",
                response.status_code,
                response.text
            );
            return false;
        }

    } catch (const std::exception &e) {
        spdlog::error(
            "File transfer and server update failed for file '{}' (transfer_id: {}): {}",
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
            cpr::Url{fmt::format("http://{}:{}/api_version", server_address, port)},
            cpr::Timeout{5s}
        );

        if (response.status_code == OK) {
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

// Helper to parse filenames like "ThorVisionServer-0.0.8-0.0.12.tar.xz"
// Output: <api_version, build_version, file_suffix, base_name_prefix>
std::tuple<std::optional<Version>, std::optional<Version>, std::string, std::string>
parse_filename_details(const std::string &filename)
{
    // Regex for "ThorVisionServer-API_MAJ.API_MIN.API_PAT-BUILD_MAJ.BUILD_MIN.BUILD_PAT.SUFFIX"
    // Example: ThorVisionServer-0.0.8-0.0.12.tar.xz
    // Example: ThorVisionServer-0.0.9-0.1.0.yaml
    // Group 1: Base Name (ThorVisionServer)
    // Group 2: API Major
    // Group 3: API Minor
    // Group 4: API Patch
    // Group 5: Build Major
    // Group 6: Build Minor
    // Group 7: Build Patch
    // Group 8: Suffix (e.g., tar.xz, yaml)
    const std::regex filename_regex(R"(([^-\.]+)-(\d+)\.(\d+)\.(\d+)-(\d+)\.(\d+)\.(\d+)\.(.+))");
    std::smatch matches;

    std::optional<Version> api_version_opt;
    std::optional<Version> build_version_opt;
    std::string suffix;
    std::string base_name;

    if (std::regex_match(filename, matches, filename_regex) && matches.size() == 9) {
        try {
            base_name = matches[1].str();

            // Only proceed if base_name is what we expect, e.g. "ThorVisionServer"
            if (base_name != "ThorVisionServer") {
                spdlog::warn(
                    "Filename '{}' does not match expected base 'ThorVisionServer'.", filename
                );
                return {std::nullopt, std::nullopt, "", ""};
            }

            api_version_opt = Version{
                std::stoi(matches[2].str()),
                std::stoi(matches[3].str()),
                std::stoi(matches[4].str())
            };

            build_version_opt = Version{
                std::stoi(matches[5].str()),
                std::stoi(matches[6].str()),
                std::stoi(matches[7].str())
            };

            suffix = matches[8].str();

        } catch (const std::invalid_argument &ia) {
            spdlog::error(
                "Invalid number in version string for filename '{}': {}", filename, ia.what()
            );
            return {std::nullopt, std::nullopt, "", ""};
        } catch (const std::out_of_range &oor) {
            spdlog::error(
                "Number out of range in version string for filename '{}': {}", filename, oor.what()
            );
            return {std::nullopt, std::nullopt, "", ""};
        }
    } else {
        spdlog::debug("Filename '{}' does not match expected format.", filename);
    }

    return {api_version_opt, build_version_opt, suffix, base_name};
}

std::optional<VersionTable> get_version_table(const std::string &table_url)
{
    const int MAX_RETRIES = 3;
    const std::chrono::seconds RETRY_DELAY(2);

    // Create a temporary file path for the version table
    fs::path temp_file = fs::temp_directory_path() / "versions.json.tmp";

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        if (retry > 0) {
            spdlog::info("Retry attempt {} of {}", retry + 1, MAX_RETRIES);
            std::this_thread::sleep_for(RETRY_DELAY * retry);  // Exponential backoff
        }

        try {
            spdlog::info("Fetching version table from URL: {}", table_url);

            // Download file to disk first (bypasses content-encoding issues)
            std::ofstream file(temp_file, std::ios::binary);
            if (!file) {
                spdlog::error("Failed to open temporary file for writing");
                continue;
            }

            auto response =
                cpr::Download(file, cpr::Url{table_url}, cpr::VerifySsl{false}, cpr::Timeout{5s});

            file.close();

            if (response.status_code != OK) {
                spdlog::error("Failed to download version table: {}", response.status_code);
                continue;
            }

            // Now read the file and parse it
            std::ifstream json_file(temp_file);
            if (!json_file) {
                spdlog::error("Failed to open downloaded file for reading");
                continue;
            }

            std::string json_content(
                (std::istreambuf_iterator<char>(json_file)), std::istreambuf_iterator<char>()
            );
            json_file.close();

            // Remove temporary file
            fs::remove(temp_file);

            if (json_content.empty()) {
                spdlog::error("Downloaded file is empty");
                continue;
            }

            // Parse the JSON content
            auto json = nlohmann::json::parse(json_content);
            VersionTable table;

            // Parse latest version
            auto latest_ver = Version::from_string(json["latest_version"].get<std::string>());
            if (!latest_ver) {
                spdlog::error("Invalid latest version format");
                return std::nullopt;
            }
            table.latest_version = *latest_ver;

            // Parse version entries (rest of your existing parsing code)
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

        } catch (const std::exception &e) {
            spdlog::error("Error getting version table: {}", e.what());
            if (retry == MAX_RETRIES - 1) {
                return std::nullopt;
            }
        }
    }

    // Cleanup any lingering temporary file
    if (fs::exists(temp_file)) {
        fs::remove(temp_file);
    }

    spdlog::error("Failed to get version table after {} retries", MAX_RETRIES);
    return std::nullopt;
}

// Main orchestration function for checking and downloading updates
UpdateOrchestrationResult check_for_and_download_updates(
    const std::string &local_device_server_base_url,  // e.g., "http://192.168.177.100:8000"
    const std::string
        &cloud_storage_endpoint,  // e.g., "https://xvc001.sgp1.digitaloceanspaces.com"
    const fs::path &download_directory,
    const std::optional<Version> &force_api_v,  // Optionally force a specific API version
    const std::optional<Version> &force_build_v
)  // Optionally force a specific build version
{
    UpdateOrchestrationResult result;
    result.success = false;  // Default to failure

    spdlog::info(
        "Starting update check process. Device server: {}, Cloud endpoint: {}",
        local_device_server_base_url,
        cloud_storage_endpoint
    );
    if (force_api_v) spdlog::info("Forcing API version: {}", force_api_v->to_string());
    if (force_build_v) spdlog::info("Forcing Build version: {}", force_build_v->to_string());


    // 1. Get Local Versions
    spdlog::info("Step 1: Getting local device versions...");
    auto local_versions_opt = get_local_device_versions(local_device_server_base_url);
    if (!local_versions_opt) {
        result.error_message =
            "Failed to get local device versions from " + local_device_server_base_url;
        spdlog::error(result.error_message);
        return result;
    }
    result.local_api_version = local_versions_opt->api_version;
    result.local_build_version = local_versions_opt->build_version;
    spdlog::info(
        "Local device versions: API {}, Build {}",
        result.local_api_version->to_string(),
        result.local_build_version->to_string()
    );

    // 2. Determine Target Remote API Version for Update Check
    spdlog::info("Step 2: Determining target remote API version...");
    Version api_to_check_against;  // The API version on the remote we will try to find builds for.

    if (force_api_v) {
        api_to_check_against = *force_api_v;
        result.target_remote_api_version = *force_api_v;
        spdlog::info("Using forced API version for check: {}", api_to_check_against.to_string());
    } else {
        auto available_remote_apis_opt = get_available_remote_api_versions(cloud_storage_endpoint);
        if (!available_remote_apis_opt) {
            result.error_message =
                "Failed to get available remote API versions from " + cloud_storage_endpoint;
            spdlog::error(result.error_message);
            return result;
        }
        if (available_remote_apis_opt->empty()) {
            result.error_message = "No remote API versions found at " + cloud_storage_endpoint;
            spdlog::warn(
                result.error_message
            );  // Not necessarily a fatal error if local is fine.
                // For now, let's consider it something to stop the update.
            return result;
        }

        result.latest_remote_api_version =
            available_remote_apis_opt->front();  // Sorted latest first
        spdlog::info(
            "Latest remote API version available: {}", result.latest_remote_api_version->to_string()
        );

        // Decide which API version to target: the latest remote, or the current local one (to check
        // for build updates)
        if (*result.local_api_version < *result.latest_remote_api_version) {
            api_to_check_against = *result.latest_remote_api_version;
            spdlog::info(
                "Local API ({}) is older than latest remote ({}). Targeting remote API for update "
                "check.",
                result.local_api_version->to_string(),
                result.latest_remote_api_version->to_string()
            );
        } else {
            api_to_check_against = *result.local_api_version;
            spdlog::info(
                "Local API ({}) is same or newer than latest remote ({}). Targeting local API for "
                "build update check.",
                result.local_api_version->to_string(),
                result.latest_remote_api_version->to_string()
            );
        }
        result.target_remote_api_version = api_to_check_against;
    }
    spdlog::info(
        "Will check for updates against remote API version: {}", api_to_check_against.to_string()
    );


    // 3. Find Update Target (Latest or Specific Build for the chosen API version)
    spdlog::info("Step 3: Finding specific update target (package and metadata)...");
    std::optional<RemoteUpdateTarget> update_target_opt;
    if (force_build_v) {  // If forcing build, API must also be determined (either forced or derived
                          // above)
        update_target_opt = find_specific_build_version(
            cloud_storage_endpoint, api_to_check_against, *force_build_v
        );
    } else {
        update_target_opt =
            find_latest_build_for_api_version(cloud_storage_endpoint, api_to_check_against);
    }

    if (!update_target_opt) {
        result.error_message = fmt::format(
            "No suitable update package found on {} for API {} (Build {})",
            cloud_storage_endpoint,
            api_to_check_against.to_string(),
            force_build_v ? force_build_v->to_string() : "latest"
        );
        spdlog::warn(result.error_message);
        // If we were not forcing, and local version is fine, this is not an error.
        // If we were forcing, this is an error.
        if (force_api_v || force_build_v) return result;  // Forced update not found.

        // Check if an update was actually needed if not forcing
        if (*result.local_api_version == api_to_check_against) {
            spdlog::info(
                "No newer build found for current API version {}. Device is up-to-date for this "
                "API.",
                result.local_api_version->to_string()
            );
        } else if (*result.local_api_version > api_to_check_against) {
            spdlog::info(
                "Local API {} is newer than or same as targeted API {}. No downgrade/update needed "
                "unless forced.",
                result.local_api_version->to_string(),
                api_to_check_against.to_string()
            );
        } else {
            // This case means local_api < api_to_check_against, but find_latest_build failed for
            // api_to_check_against. This is an issue with remote.
            spdlog::error(
                "Could not find any builds for a newer remote API {}. Update cannot proceed.",
                api_to_check_against.to_string()
            );
            result.error_message =
                "Remote API " + api_to_check_against.to_string() + " has no available builds.";
            return result;
        }
        result.success = true;  // Successfully checked, no update found or needed (and not forced)
        return result;
    }

    RemoteUpdateTarget update_target = *update_target_opt;
    result.target_remote_api_version =
        update_target.api_version;  // Should be same as api_to_check_against
    result.target_remote_build_version = update_target.build_version;
    spdlog::info(
        "Identified update target: API {}, Build {}. Package: {}, Metadata: {}",
        update_target.api_version.to_string(),
        update_target.build_version.to_string(),
        update_target.package_filename,
        update_target.metadata_filename
    );

    // 4. Version Comparison Logic (is an update actually needed?)
    spdlog::info("Step 4: Comparing local version with target update version...");
    bool needs_update = false;
    if (force_api_v || force_build_v) {
        needs_update = true;
        spdlog::info("Update is forced.");
    } else {
        if (*result.local_api_version < update_target.api_version) {
            needs_update = true;
            spdlog::info(
                "Update needed: Local API {} < Target API {}",
                result.local_api_version->to_string(),
                update_target.api_version.to_string()
            );
        } else if (*result.local_api_version == update_target.api_version &&
                   *result.local_build_version < update_target.build_version) {
            needs_update = true;
            spdlog::info(
                "Update needed: Local API matches, but Local Build {} < Target Build {}",
                result.local_build_version->to_string(),
                update_target.build_version.to_string()
            );
        }
    }

    if (!needs_update) {
        spdlog::info(
            "No update needed. Local version: API {}, Build {}. Target version: API {}, Build {}.",
            result.local_api_version->to_string(),
            result.local_build_version->to_string(),
            update_target.api_version.to_string(),
            update_target.build_version.to_string()
        );
        result.success = true;  // Successfully checked, no update action required
        return result;
    }
    spdlog::info("Update is required or forced.");

    // 5. Download Metadata (.yaml file)
    spdlog::info(
        "Step 5: Checking for or downloading metadata from {} ...", update_target.metadata_url
    );
    fs::create_directories(download_directory);  // Ensure download directory exists
    fs::path metadata_path = download_directory / update_target.metadata_filename;
    std::optional<UpdateMetadata> metadata_opt;

    if (fs::exists(metadata_path)) {
        spdlog::info("Found local metadata file: {}", metadata_path.string());
        metadata_opt = xvc::parse_yaml_metadata_from_file(metadata_path);
    } else {
        spdlog::info("Local metadata file not found. Downloading...");
        // Define a temporary path for metadata. This could be made more robust.
        fs::path temp_metadata_dir = download_directory / "temp_metadata_files";
        fs::create_directories(temp_metadata_dir);  // Ensure temp directory exists
        metadata_opt = download_and_parse_yaml_metadata(
            update_target.metadata_url, temp_metadata_dir.string()
        );
        if (metadata_opt) {
            // Optional: save the downloaded metadata to the download_directory for future offline
            // use For now, we just use it from memory after download.
        }
    }

    if (!metadata_opt) {
        result.error_message = "Failed to get or parse metadata from " + update_target.metadata_url;
        spdlog::error(result.error_message);
        return result;
    }
    result.downloaded_package_metadata = *metadata_opt;
    spdlog::info(
        "Metadata parsed successfully. Expected Package Hash: {}",
        result.downloaded_package_metadata->package_hash_sha256
    );


    // 6. Download Package (.tar.xz file)
    spdlog::info(
        "Step 6: Checking for or downloading update package {} to {} ...",
        update_target.package_filename,
        download_directory.string()
    );
    fs::path package_output_path = download_directory / update_target.package_filename;

    if (fs::exists(package_output_path)) {
        spdlog::info("Found local package file: {}", package_output_path.string());
        spdlog::info("Verifying hash of local package file...");
        auto calculated_hash = calculate_sha256(package_output_path);
        if (!calculated_hash ||
            *calculated_hash != result.downloaded_package_metadata->package_hash_sha256) {
            result.error_message = fmt::format(
                "Local package file {} failed hash verification. Expected: {}, Got: {}. Please "
                "delete it to re-download.",
                package_output_path.string(),
                result.downloaded_package_metadata->package_hash_sha256,
                calculated_hash.value_or("calculation failed")
            );
            spdlog::error(result.error_message);
            return result;
        }
        spdlog::info("Local package file hash verified successfully.");
    } else {
        spdlog::info("Local package file not found. Downloading...");
        // Call the existing download_and_verify.
        // It uses its own HEAD request for Content-Length, which is an okay secondary check.
        // The primary check is the hash from our metadata.
        DownloadResult package_download_result = download_and_verify(
            update_target.package_url,
            result.downloaded_package_metadata->package_hash_sha256,  // Hash from YAML
            package_output_path
        );

        if (!package_download_result.success) {
            result.error_message = fmt::format(
                "Failed to download or verify package from {}: {}",
                update_target.package_url,
                package_download_result.error_message
            );
            spdlog::error(result.error_message);
            if (fs::exists(package_output_path))
                fs::remove(package_output_path);  // Clean up failed download
            return result;
        }
    }

    result.success = true;
    result.update_available_and_downloaded = true;
    result.downloaded_package_path = package_output_path;
    spdlog::info(
        "Update package {} (API {}, Build {}) downloaded and verified successfully to {}.",
        update_target.package_filename,
        update_target.api_version.to_string(),
        update_target.build_version.to_string(),
        result.downloaded_package_path.string()
    );

    return result;
}

// New function implementation
UpdateOrchestrationResult force_download_updates(
    const std::string
        &cloud_storage_endpoint,  // e.g., "https://xvc001.sgp1.digitaloceanspaces.com"
    const fs::path &download_directory,
    const Version &force_api_v,  // API version is mandatory
    const std::optional<Version> &force_build_v
)  // Optionally force a specific build version
{
    UpdateOrchestrationResult result;
    result.success = false;  // Default to failure

    spdlog::info("Starting forced download process. Cloud endpoint: {}", cloud_storage_endpoint);
    spdlog::info("Forcing API version: {}", force_api_v.to_string());
    if (force_build_v) {
        spdlog::info("Forcing Build version: {}", force_build_v->to_string());
    }

    result.target_remote_api_version = force_api_v;

    // Step 1: Find Update Target (Latest or Specific Build for the chosen API version)
    // This step is similar to Step 3 in check_for_and_download_updates, but without local version
    // context.
    spdlog::info(
        "Step 1 (Forced Download): Finding specific update target (package and metadata)..."
    );
    std::optional<RemoteUpdateTarget> update_target_opt;
    if (force_build_v) {
        update_target_opt =
            find_specific_build_version(cloud_storage_endpoint, force_api_v, *force_build_v);
    } else {
        update_target_opt = find_latest_build_for_api_version(cloud_storage_endpoint, force_api_v);
    }

    if (!update_target_opt) {
        result.error_message = fmt::format(
            "No suitable update package found on {} for API {} (Build {})",
            cloud_storage_endpoint,
            force_api_v.to_string(),
            force_build_v ? force_build_v->to_string() : "latest available"
        );
        spdlog::error(result.error_message);
        return result;  // Forced update not found.
    }

    RemoteUpdateTarget update_target = *update_target_opt;
    // result.target_remote_api_version is already set to force_api_v.
    // We should confirm that the found target matches this.
    if (update_target.api_version != force_api_v) {
        result.error_message = fmt::format(
            "Mismatch: Requested API {} but found package for API {}.",
            force_api_v.to_string(),
            update_target.api_version.to_string()
        );
        spdlog::error(result.error_message);
        return result;
    }
    result.target_remote_build_version = update_target.build_version;
    spdlog::info(
        "Identified update target: API {}, Build {}. Package: {}, Metadata: {}",
        update_target.api_version.to_string(),
        update_target.build_version.to_string(),
        update_target.package_filename,
        update_target.metadata_filename
    );

    // Step 2: Download Metadata (.yaml file)
    // This step is similar to Step 5 in check_for_and_download_updates.
    spdlog::info(
        "Step 2 (Forced Download): Checking for or downloading metadata from {} ...",
        update_target.metadata_url
    );
    fs::create_directories(download_directory);
    fs::path metadata_path = download_directory / update_target.metadata_filename;
    std::optional<UpdateMetadata> metadata_opt;

    if (fs::exists(metadata_path)) {
        spdlog::info("Found local metadata file: {}", metadata_path.string());
        metadata_opt = xvc::parse_yaml_metadata_from_file(metadata_path);
    } else {
        spdlog::info("Local metadata file not found. Downloading...");
        fs::path temp_metadata_dir = download_directory / "temp_metadata_files";
        fs::create_directories(temp_metadata_dir);
        metadata_opt = download_and_parse_yaml_metadata(
            update_target.metadata_url, temp_metadata_dir.string()
        );
    }

    if (!metadata_opt) {
        result.error_message = "Failed to get or parse metadata from " + update_target.metadata_url;
        spdlog::error(result.error_message);
        return result;
    }
    result.downloaded_package_metadata = *metadata_opt;
    spdlog::info(
        "Metadata parsed successfully. Expected Package Hash: {}",
        result.downloaded_package_metadata->package_hash_sha256
    );

    // Step 3: Download Package (.tar.xz file)
    // This step is similar to Step 6 in check_for_and_download_updates.
    spdlog::info(
        "Step 3 (Forced Download): Checking for or downloading update package {} to {} ...",
        update_target.package_filename,
        download_directory.string()
    );
    fs::path package_output_path = download_directory / update_target.package_filename;

    if (fs::exists(package_output_path)) {
        spdlog::info("Found local package file: {}", package_output_path.string());
        spdlog::info("Verifying hash of local package file...");
        auto calculated_hash = calculate_sha256(package_output_path);
        if (!calculated_hash ||
            *calculated_hash != result.downloaded_package_metadata->package_hash_sha256) {
            result.error_message = fmt::format(
                "Local package file {} failed hash verification. Expected: {}, Got: {}. Please "
                "delete it to re-download.",
                package_output_path.string(),
                result.downloaded_package_metadata->package_hash_sha256,
                calculated_hash.value_or("calculation failed")
            );
            spdlog::error(result.error_message);
            return result;
        }
        spdlog::info("Local package file hash verified successfully.");
    } else {
        spdlog::info("Local package file not found. Downloading...");
        DownloadResult package_download_result = download_and_verify(
            update_target.package_url,
            result.downloaded_package_metadata->package_hash_sha256,
            package_output_path
        );

        if (!package_download_result.success) {
            result.error_message = fmt::format(
                "Failed to download or verify package from {}: {}",
                update_target.package_url,
                package_download_result.error_message
            );
            spdlog::error(result.error_message);
            if (fs::exists(package_output_path)) fs::remove(package_output_path);
            return result;
        }
    }

    result.success = true;
    result.update_available_and_downloaded = true;
    result.downloaded_package_path = package_output_path;
    spdlog::info(
        "Update package {} (API {}, Build {}) force-downloaded and verified successfully to {}.",
        update_target.package_filename,
        update_target.api_version.to_string(),
        update_target.build_version.to_string(),
        result.downloaded_package_path.string()
    );

    return result;
}

// Old update_server function - to be refactored or replaced.
// For now, its body is commented out to prevent usage until fully refactored.
UpdateResult update_server(
    const std::string &server_address,
    int server_port,               // Port of the server to be updated
    int update_server_port,        // Port of the update server
    const std::string &table_url,  // This will be replaced by cloud_storage_endpoint
    const fs::path &update_dir,
    [[maybe_unused]] const Version &client_version,  // client_version might still be relevant
    bool skip_version_check,                         // Replaced by force_api_v/force_build_v logic
    const std::optional<Version> &force_version      // Replaced by force_api_v/force_build_v logic
)
{
    UpdateResult result;
    result.success = false;
    result.error_message =
        "Old update_server function called; this function is pending refactoring "
        "to use check_for_and_download_updates and then perform transfer.";
    spdlog::error(result.error_message);

    // The logic of this function is now largely handled by check_for_and_download_updates
    // followed by the actual server update process (handshake, transfer).
    //
    // Steps would be:
    // 1. Call check_for_and_download_updates(...).
    // 2. If successful and an update is downloaded:
    //    a. Get downloaded_package_path and metadata from the result.
    //    b. perform_handshake(server_address, update_server_port).
    //    c. prepare_file_transfer(...).
    //    d. transfer_file(...).
    //    e. The server-side would then handle applying the update.
    // 3. Populate and return UpdateResult.

    // Placeholder values if needed by legacy callers, though behavior is an error:
    if (auto current_ver_opt = get_server_version(server_address, server_port)) {
        result.current_version = *current_ver_opt;
    } else {
        // Use a default if cannot fetch, to avoid uninitialized Version issues
        result.current_version = {0, 0, 0};
    }
    result.available_version = {0, 0, 0};  // Unknown without full logic
    result.update_needed = false;          // Unknown

    return result;
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

// Fetches current API and Build versions from the device server
std::optional<LocalVersionInfo> get_local_device_versions(const std::string &device_server_base_url)
{
    LocalVersionInfo local_versions;
    bool api_version_fetched = false;
    bool build_version_fetched = false;

    // Helper lambda to fetch a single version
    auto fetch_version = [&](const std::string &endpoint_path, Version &out_version) -> bool {
        try {
            std::string full_url = device_server_base_url + endpoint_path;
            spdlog::debug("Fetching version from URL: {}", full_url);

            cpr::Response r = cpr::Get(cpr::Url{full_url}, cpr::Timeout{3s});  // 3 second timeout

            if (r.status_code == OK) {
                try {
                    auto json_response = nlohmann::json::parse(r.text);
                    if (json_response.contains("version") && json_response["version"].is_string()) {
                        std::string version_str = json_response["version"].get<std::string>();
                        auto version_opt = Version::from_string(version_str);
                        if (version_opt) {
                            out_version = *version_opt;
                            // spdlog::info("Fetched {} version: {}", endpoint_path, version_str);
                            return true;
                        } else {
                            spdlog::error(
                                "Failed to parse version string '{}' from {}", version_str, full_url
                            );
                        }
                    } else {
                        spdlog::error(
                            "JSON response from {} missing 'version' field or not a string. "
                            "Response: {}",
                            full_url,
                            r.text
                        );
                    }
                } catch (const nlohmann::json::parse_error &e) {
                    spdlog::error(
                        "Failed to parse JSON response from {}: {}. Response: {}",
                        full_url,
                        e.what(),
                        r.text
                    );
                }
            } else {
                spdlog::error(
                    "Failed to fetch version from {}. Status code: {}, Error: {}",
                    full_url,
                    r.status_code,
                    r.error.message
                );
            }
        } catch (const std::exception &e) {  // Catch potential cpr exceptions
            spdlog::error(
                "Exception while fetching version from {}{}: {}",
                device_server_base_url,
                endpoint_path,
                e.what()
            );
        }
        return false;
    };

    // Fetch API version
    if (fetch_version("/api_version", local_versions.api_version)) {
        api_version_fetched = true;
    } else {
        spdlog::error("Could not retrieve API version from device server.");
    }

    // Fetch Build version
    if (fetch_version("/build_version", local_versions.build_version)) {
        build_version_fetched = true;
    } else {
        spdlog::error("Could not retrieve Build version from device server.");
    }

    if (api_version_fetched && build_version_fetched) {
        return local_versions;
    }

    return std::nullopt;
}

// Lists files and subdirectories from a cloud storage URL via HTTPS GET.
// The directory_url is the full S3-style path including the prefix,
// e.g., https://xvc001.sgp1.digitaloceanspaces.com/ (for root listing)
// or https://xvc001.sgp1.digitaloceanspaces.com/0.0.8/ (for listing "0.0.8/" content)
std::optional<std::vector<std::string>> list_remote_directory_contents(
    const std::string &directory_url_with_prefix
)
{
    spdlog::debug(
        "Listing remote directory contents for S3-style path: {}", directory_url_with_prefix
    );
    std::vector<std::string> contents;

    std::string bucket_request_url_str;  // The URL to send the GET request to (bucket root)
    std::string path_prefix_to_list;     // The S3 prefix to list, e.g., "0.0.8/" or ""

    try {
        // Example 1: directory_url_with_prefix = "https://xvc001.sgp1.digitaloceanspaces.com/"
        // bucket_request_url_str = "https://xvc001.sgp1.digitaloceanspaces.com/"
        // path_prefix_to_list = ""

        // Example 2: directory_url_with_prefix =
        // "https://xvc001.sgp1.digitaloceanspaces.com/0.0.8/" bucket_request_url_str =
        // "https://xvc001.sgp1.digitaloceanspaces.com/" path_prefix_to_list = "0.0.8/"

        // Find the end of the protocol and host part
        size_t protocol_end_pos = directory_url_with_prefix.find("://");
        if (protocol_end_pos == std::string::npos) {
            spdlog::error("Invalid directory URL (missing '://'): {}", directory_url_with_prefix);
            return std::nullopt;
        }
        protocol_end_pos += 3;  // Move past "://"

        size_t first_slash_after_host_pos = directory_url_with_prefix.find('/', protocol_end_pos);

        if (first_slash_after_host_pos == std::string::npos) {
            // URL is just "https://host.com" (no trailing slash)
            bucket_request_url_str = directory_url_with_prefix + "/";
            path_prefix_to_list = "";
        } else {
            bucket_request_url_str =
                directory_url_with_prefix.substr(0, first_slash_after_host_pos + 1);
            path_prefix_to_list = directory_url_with_prefix.substr(first_slash_after_host_pos + 1);
        }

        // Ensure path_prefix_to_list ends with a slash if it's not empty,
        // as S3 prefixes for "directories" usually do.
        if (!path_prefix_to_list.empty() && path_prefix_to_list.back() != '/') {
            // This might happen if input was like ".../0.0.8" instead of ".../0.0.8/"
            // For robustness, we could add it, but it's better if the caller is consistent.
            spdlog::warn(
                "Path prefix '{}' derived from URL '{}' does not end with '/'. S3 listing might "
                "behave unexpectedly. Consider adding trailing slash to input URL for "
                "'directories'.",
                path_prefix_to_list,
                directory_url_with_prefix
            );
        }


        spdlog::debug(
            "S3 Listing Details: RequestURL='{}', PathPrefix='{}'",
            bucket_request_url_str,
            path_prefix_to_list
        );

    } catch (const std::out_of_range &oor) {
        spdlog::error(
            "Out of range error while parsing directory URL '{}': {}",
            directory_url_with_prefix,
            oor.what()
        );
        return std::nullopt;
    } catch (const std::exception &e) {
        spdlog::error(
            "Generic exception while parsing directory URL '{}': {}",
            directory_url_with_prefix,
            e.what()
        );
        return std::nullopt;
    }


    try {
        cpr::Parameters params;
        if (!path_prefix_to_list.empty()) {
            params.Add({"prefix", path_prefix_to_list});
        }
        params.Add({"delimiter", "/"});
        // MaxKeys defaults to 1000, which is usually sufficient for this use case.
        // If more items are expected, pagination would be needed.

        cpr::Url request_cpr_url{bucket_request_url_str};

        spdlog::info(
            "Requesting S3 listing from URL: {} with prefix: '{}', delimiter: '/'",
            bucket_request_url_str,
            path_prefix_to_list
        );

        // Use VerifySsl{false} for DigitalOcean Spaces if SSL issues arise, but true is preferred
        // for production
        cpr::Response r =
            cpr::Get(request_cpr_url, params, cpr::Timeout{15s}, cpr::VerifySsl{false});

        if (r.status_code == OK) {
            spdlog::trace(
                "Successfully fetched S3 listing. Response body (first 500 chars):\n{}",
                r.text.substr(0, 500)
            );
            pugi::xml_document doc;
            pugi::xml_parse_result result = doc.load_string(r.text.c_str());

            if (!result) {
                spdlog::error(
                    "Failed to parse XML response from S3 listing {}: {}. Offset: {}. XML: {}",
                    bucket_request_url_str,
                    result.description(),
                    result.offset,
                    r.text.substr(0, 500)
                );
                return std::nullopt;
            }

            // Extract items from <Contents><Key> (objects/files within the current prefix level)
            for (pugi::xpath_node content_xpath_node :
                 doc.select_nodes("/ListBucketResult/Contents/Key")) {
                pugi::xml_node content_node = content_xpath_node.node();
                std::string key = content_node.child_value();
                // The key is the full path from the bucket root, e.g., "0.0.8/file.txt"
                // We want the name relative to path_prefix_to_list
                if (key.rfind(path_prefix_to_list, 0) == 0) {  // starts_with
                    std::string relative_key = key.substr(path_prefix_to_list.length());
                    // Only add if it's not empty (i.e., key wasn't identical to prefix)
                    // and it's a file (does not contain '/' itself, as sub-sub-dirs are handled by
                    // CommonPrefixes)
                    if (!relative_key.empty() && relative_key.find('/') == std::string::npos) {
                        contents.push_back(relative_key);
                    }
                }
            }

            // Extract items from <CommonPrefixes><Prefix> (representing "subdirectories")
            for (pugi::xpath_node common_prefix_xpath_node :
                 doc.select_nodes("/ListBucketResult/CommonPrefixes/Prefix")) {
                pugi::xml_node common_prefix_node = common_prefix_xpath_node.node();
                std::string common_prefix_val =
                    common_prefix_node.child_value();  // e.g., "0.0.8/subdir/" or "0.0.9/"
                // We want the name relative to path_prefix_to_list
                if (common_prefix_val.rfind(path_prefix_to_list, 0) == 0) {  // starts_with
                    std::string relative_prefix =
                        common_prefix_val.substr(path_prefix_to_list.length());
                    if (!relative_prefix.empty()) {
                        contents.push_back(
                            relative_prefix
                        );  // e.g., "subdir/" or "0.0.9/" if path_prefix_to_list was ""
                    }
                }
            }

            // Filter out any potentially empty strings from vector, though logic above should
            // prevent them
            contents.erase(
                std::remove_if(
                    contents.begin(), contents.end(), [](const std::string &s) { return s.empty(); }
                ),
                contents.end()
            );

            // Log if results are truncated (more items exist than returned)
            pugi::xpath_node truncated_xpath_node =
                doc.select_node("/ListBucketResult/IsTruncated");
            if (truncated_xpath_node) {
                pugi::xml_node truncated_node = truncated_xpath_node.node();
                if (truncated_node && std::string(truncated_node.child_value()) == "true") {
                    spdlog::warn(
                        "S3 listing for prefix '{}' is truncated. Not all items may have been "
                        "listed. Pagination is not implemented.",
                        path_prefix_to_list
                    );
                }
            }

            spdlog::debug(
                "Found {} items in S3 path '{}'", contents.size(), directory_url_with_prefix
            );
            for (const auto &item : contents) {
                spdlog::trace(" - S3 item: {}", item);
            }
            return contents;

        } else {
            spdlog::error(
                "Failed to list S3 remote directory (URL: {}, prefix: {}). Status code: {}, Body: "
                "{}",
                bucket_request_url_str,
                path_prefix_to_list,
                r.status_code,
                r.text.substr(0, 500)
            );
            if (r.error.message != r.text &&
                !r.error.message.empty()) {  // Avoid duplicate logging if text is error msg
                spdlog::error("CPR error: {}", r.error.message);
            }
            return std::nullopt;
        }
    } catch (const std::exception &e) {  // Catch potential cpr exceptions or other std::exceptions
        spdlog::error(
            "Exception while listing S3 remote directory (URL: {}, prefix: {}): {}",
            bucket_request_url_str,
            path_prefix_to_list,
            e.what()
        );
        return std::nullopt;
    }
}

// Finds all available API version "folders" at the root of the storage endpoint
std::optional<std::vector<Version>> get_available_remote_api_versions(
    const std::string &storage_endpoint_url
)  // e.g., "https://xvc001.sgp1.digitaloceanspaces.com/"
{
    spdlog::info("Discovering available API versions from endpoint: {}", storage_endpoint_url);
    std::string root_listing_url = storage_endpoint_url;
    // Ensure the URL ends with a slash for S3 root listing behavior expected by
    // list_remote_directory_contents
    if (root_listing_url.empty() || root_listing_url.back() != '/') {
        root_listing_url += "/";
    }

    auto top_level_items_opt = list_remote_directory_contents(root_listing_url);

    if (!top_level_items_opt) {
        spdlog::error("Failed to list top-level items from {}", root_listing_url);
        return std::nullopt;
    }

    std::vector<Version> api_versions;
    spdlog::debug("Parsing top-level items for API versions:");
    for (const std::string &item_name : *top_level_items_opt) {
        spdlog::trace(" - Considering item: {}", item_name);
        // We are looking for "directories" which should end with a '/'
        // and whose names (without the slash) are valid versions.
        if (!item_name.empty() && item_name.back() == '/') {
            std::string potential_version_str = item_name.substr(0, item_name.length() - 1);
            auto version_opt = Version::from_string(potential_version_str);
            if (version_opt) {
                api_versions.push_back(*version_opt);
                spdlog::debug("  Found valid API version: {}", version_opt->to_string());
            } else {
                spdlog::trace(
                    "  Item '{}' (as '{}') is not a valid version string.",
                    item_name,
                    potential_version_str
                );
            }
        } else {
            spdlog::trace("  Item '{}' is not a directory (does not end with '/').", item_name);
        }
    }

    if (api_versions.empty()) {
        spdlog::warn(
            "No valid API version folders found at {}. Ensure they are named like 'X.Y.Z/'",
            root_listing_url
        );
        // Return empty vector, not nullopt, as the listing itself might have been successful but
        // found no versions
    }

    // Sort versions, latest first (descending order)
    std::sort(api_versions.begin(), api_versions.end(), std::greater<Version>());

    spdlog::info(
        "Found {} API versions. Latest available: {}",
        api_versions.size(),
        api_versions.empty() ? "N/A" : api_versions.front().to_string()
    );

    return api_versions;
}

// Finds the latest build (both .tar.xz and .yaml) for a specific API version
std::optional<RemoteUpdateTarget> find_latest_build_for_api_version(
    const std::string &storage_endpoint_url,  // e.g., "https://xvc001.sgp1.digitaloceanspaces.com"
    const Version &target_api_version
)
{
    spdlog::info(
        "Searching for latest build for API version: {} on endpoint: {}",
        target_api_version.to_string(),
        storage_endpoint_url
    );

    std::string api_version_folder_url = storage_endpoint_url;
    if (api_version_folder_url.empty() || api_version_folder_url.back() != '/') {
        api_version_folder_url += "/";
    }
    api_version_folder_url += target_api_version.to_string() + "/";

    auto files_in_folder_opt = list_remote_directory_contents(api_version_folder_url);

    if (!files_in_folder_opt) {
        spdlog::error("Failed to list contents of API version folder: {}", api_version_folder_url);
        return std::nullopt;
    }

    std::optional<RemoteUpdateTarget> latest_target_opt;
    std::map<Version, std::pair<std::string, std::string>>
        found_build_files;  // build_version -> {package_file, metadata_file}

    spdlog::debug(
        "Parsing files in folder {} for API {}:",
        api_version_folder_url,
        target_api_version.to_string()
    );
    for (const std::string &filename : *files_in_folder_opt) {
        spdlog::trace(" - Considering file: {}", filename);
        auto [file_api_ver_opt, file_build_ver_opt, suffix, base_name] =
            parse_filename_details(filename);

        if (base_name != "ThorVisionServer") {
            spdlog::trace(
                "   Skipping file '{}': base name mismatch (expected 'ThorVisionServer').", filename
            );
            continue;
        }

        if (file_api_ver_opt && file_build_ver_opt && *file_api_ver_opt == target_api_version) {
            spdlog::debug(
                "   File '{}' matches target API version {}. Build version: {}, Suffix: {}",
                filename,
                target_api_version.to_string(),
                file_build_ver_opt->to_string(),
                suffix
            );
            if (suffix == "tar.xz") {
                found_build_files[*file_build_ver_opt].first = filename;
            } else if (suffix == "yaml") {
                found_build_files[*file_build_ver_opt].second = filename;
            } else {
                spdlog::trace("   Skipping file '{}': unrecognized suffix '{}'.", filename, suffix);
            }
        } else {
            if (!file_api_ver_opt || !file_build_ver_opt) {
                spdlog::trace(
                    "   Skipping file '{}': could not parse API or build version.", filename
                );
            } else if (*file_api_ver_opt != target_api_version) {
                spdlog::trace(
                    "   Skipping file '{}': API version {} does not match target {}.",
                    filename,
                    file_api_ver_opt->to_string(),
                    target_api_version.to_string()
                );
            }
        }
    }

    // Find the highest build version that has both .tar.xz and .yaml files
    Version latest_valid_build_version = {0, 0, 0};  // Start with a very old version

    for (const auto &pair : found_build_files) {
        const Version &current_build_ver = pair.first;
        const std::string &package_file = pair.second.first;
        const std::string &metadata_file = pair.second.second;

        if (!package_file.empty() && !metadata_file.empty()) {
            if (!latest_target_opt || current_build_ver > latest_target_opt->build_version) {
                RemoteUpdateTarget current_target;
                current_target.api_version = target_api_version;
                current_target.build_version = current_build_ver;
                current_target.package_filename = package_file;
                current_target.metadata_filename = metadata_file;
                current_target.package_url = api_version_folder_url + package_file;
                current_target.metadata_url = api_version_folder_url + metadata_file;
                latest_target_opt = current_target;
                spdlog::debug(
                    "  Identified candidate build: API {}, Build {}, Package: {}, Metadata: {}",
                    target_api_version.to_string(),
                    current_build_ver.to_string(),
                    package_file,
                    metadata_file
                );
            }
        } else {
            spdlog::warn(
                "  Build version {} for API {} is incomplete. Package: '{}', Metadata: '{}'",
                current_build_ver.to_string(),
                target_api_version.to_string(),
                package_file.empty() ? "MISSING" : package_file,
                metadata_file.empty() ? "MISSING" : metadata_file
            );
        }
    }

    if (latest_target_opt) {
        spdlog::info(
            "Found latest build for API {}: Build {}, Package: {}, Metadata: {}",
            latest_target_opt->api_version.to_string(),
            latest_target_opt->build_version.to_string(),
            latest_target_opt->package_filename,
            latest_target_opt->metadata_filename
        );
    } else {
        spdlog::warn(
            "No complete (package + metadata) build found for API {} in folder {}",
            target_api_version.to_string(),
            api_version_folder_url
        );
    }

    return latest_target_opt;
}

// Finds a specific build (both .tar.xz and .yaml) for a given API and build version
std::optional<RemoteUpdateTarget> find_specific_build_version(
    const std::string &storage_endpoint_url,  // e.g., "https://xvc001.sgp1.digitaloceanspaces.com"
    const Version &target_api_version, const Version &target_build_version
)
{
    spdlog::info(
        "Searching for specific build: API {}, Build {} on endpoint: {}",
        target_api_version.to_string(),
        target_build_version.to_string(),
        storage_endpoint_url
    );

    std::string api_version_folder_url = storage_endpoint_url;
    if (api_version_folder_url.empty() || api_version_folder_url.back() != '/') {
        api_version_folder_url += "/";
    }
    api_version_folder_url += target_api_version.to_string() + "/";

    auto files_in_folder_opt = list_remote_directory_contents(api_version_folder_url);

    if (!files_in_folder_opt) {
        spdlog::error("Failed to list contents of API version folder: {}", api_version_folder_url);
        return std::nullopt;
    }

    std::string found_package_file;
    std::string found_metadata_file;

    spdlog::debug(
        "Parsing files in folder {} for API {}, Build {}:",
        api_version_folder_url,
        target_api_version.to_string(),
        target_build_version.to_string()
    );

    for (const std::string &filename : *files_in_folder_opt) {
        spdlog::trace(" - Considering file: {}", filename);
        auto [file_api_ver_opt, file_build_ver_opt, suffix, base_name] =
            parse_filename_details(filename);

        if (base_name != "ThorVisionServer") {
            spdlog::trace(
                "   Skipping file '{}': base name mismatch (expected 'ThorVisionServer').", filename
            );
            continue;
        }

        if (file_api_ver_opt && file_build_ver_opt && *file_api_ver_opt == target_api_version &&
            *file_build_ver_opt == target_build_version) {
            spdlog::debug(
                "   File '{}' matches target API {} and target Build {}. Suffix: {}",
                filename,
                target_api_version.to_string(),
                target_build_version.to_string(),
                suffix
            );

            if (suffix == "tar.xz") {
                if (!found_package_file.empty()) {
                    spdlog::warn(
                        "   Duplicate package file found for API {}, Build {}: '{}' and '{}'. "
                        "Using the latter.",
                        target_api_version.to_string(),
                        target_build_version.to_string(),
                        found_package_file,
                        filename
                    );
                }
                found_package_file = filename;
            } else if (suffix == "yaml") {
                if (!found_metadata_file.empty()) {
                    spdlog::warn(
                        "   Duplicate metadata file found for API {}, Build {}: '{}' and '{}'. "
                        "Using the latter.",
                        target_api_version.to_string(),
                        target_build_version.to_string(),
                        found_metadata_file,
                        filename
                    );
                }
                found_metadata_file = filename;
            } else {
                spdlog::trace("   Skipping file '{}': unrecognized suffix '{}'.", filename, suffix);
            }
        } else {
            // Log reasons for skipping if relevant (e.g. version mismatch)
            if (file_api_ver_opt &&
                file_build_ver_opt) {  // Only log if parse was successful but versions differ
                if (*file_api_ver_opt != target_api_version) {
                    spdlog::trace(
                        "   Skipping file '{}': API version {} does not match target {}.",
                        filename,
                        file_api_ver_opt->to_string(),
                        target_api_version.to_string()
                    );
                } else if (*file_build_ver_opt != target_build_version) {
                    spdlog::trace(
                        "   Skipping file '{}': Build version {} does not match target {}.",
                        filename,
                        file_build_ver_opt->to_string(),
                        target_build_version.to_string()
                    );
                }
            } else if (!filename.empty()) {  // Only log if filename is not empty and parsing failed
                spdlog::trace(
                    "   Skipping file '{}': could not parse API or build version, or other "
                    "mismatch.",
                    filename
                );
            }
        }
    }

    if (!found_package_file.empty() && !found_metadata_file.empty()) {
        RemoteUpdateTarget target;
        target.api_version = target_api_version;
        target.build_version = target_build_version;
        target.package_filename = found_package_file;
        target.metadata_filename = found_metadata_file;
        target.package_url = api_version_folder_url + found_package_file;
        target.metadata_url = api_version_folder_url + found_metadata_file;

        spdlog::info(
            "Found specific build: API {}, Build {}. Package: '{}', Metadata: '{}'",
            target.api_version.to_string(),
            target.build_version.to_string(),
            target.package_filename,
            target.metadata_filename
        );
        return target;
    } else {
        spdlog::warn(
            "Could not find complete package and metadata for specific build: API {}, Build {} in "
            "folder {}.",
            target_api_version.to_string(),
            target_build_version.to_string(),
            api_version_folder_url
        );
        if (found_package_file.empty()) {
            spdlog::warn(
                " - Package file (.tar.xz) for build {} is MISSING.",
                target_build_version.to_string()
            );
        }
        if (found_metadata_file.empty()) {
            spdlog::warn(
                " - Metadata file (.yaml) for build {} is MISSING.",
                target_build_version.to_string()
            );
        }
        return std::nullopt;
    }
}

// Downloads the .yaml metadata file and parses its content
std::optional<UpdateMetadata> download_and_parse_yaml_metadata(
    const std::string &metadata_url, const std::string &temp_metadata_dir_str
)
{
    fs::path temp_dir_path(temp_metadata_dir_str);
    if (temp_dir_path.empty()) {  // Fallback if an empty string was passed
        temp_dir_path = fs::temp_directory_path();
    }
    fs::create_directories(temp_dir_path);  // Ensure the directory exists

    // Create a unique temporary filename
    std::string temp_filename =
        "metadata_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
        ".yaml";
    fs::path temp_file_path = temp_dir_path / temp_filename;

    spdlog::info(
        "Downloading metadata from: {} to temporary file: {}", metadata_url, temp_file_path.string()
    );

    try {
        // Download to the temporary file
        std::ofstream temp_file_stream(temp_file_path, std::ios::binary);
        if (!temp_file_stream) {
            spdlog::error("Failed to open temporary file for writing: {}", temp_file_path.string());
            return std::nullopt;
        }

        cpr::Response r = cpr::Download(
            temp_file_stream, cpr::Url{metadata_url}, cpr::Timeout{10s}, cpr::VerifySsl{false}
        );
        temp_file_stream.close();

        if (r.status_code != OK) {
            spdlog::error(
                "Failed to download metadata file from {} to {}. Status code: {}",
                metadata_url,
                temp_file_path.string(),
                r.status_code
            );
            if (!r.error.message.empty()) {
                spdlog::error("CPR error: {}", r.error.message);
            }
            fs::remove(temp_file_path);  // Clean up
            return std::nullopt;
        }

        if (fs::is_empty(temp_file_path)) {
            spdlog::error(
                "Downloaded metadata file {} is empty, though status code was OK.",
                temp_file_path.string()
            );
            fs::remove(temp_file_path);  // Clean up
            return std::nullopt;
        }

        // Read content from the temporary file
        std::ifstream downloaded_file_stream(temp_file_path);
        if (!downloaded_file_stream) {
            spdlog::error(
                "Failed to open downloaded temporary metadata file for reading: {}",
                temp_file_path.string()
            );
            fs::remove(temp_file_path);  // Clean up
            return std::nullopt;
        }
        std::string yaml_content(
            (std::istreambuf_iterator<char>(downloaded_file_stream)),
            std::istreambuf_iterator<char>()
        );
        downloaded_file_stream.close();
        fs::remove(temp_file_path);  // Clean up temporary file

        if (yaml_content.empty()) {
            spdlog::error(
                "Content read from temporary metadata file {} is empty.", temp_file_path.string()
            );
            return std::nullopt;
        }

        spdlog::debug(
            "Successfully downloaded metadata to temporary file and read content. Parsing YAML...",
            metadata_url
        );
        spdlog::trace("YAML content:\n{}", yaml_content);

        UpdateMetadata metadata;
        YAML::Node yaml_doc;
        try {
            yaml_doc = YAML::Load(yaml_content);
        } catch (const YAML::ParserException &e) {
            spdlog::error("Failed to parse YAML from {}: {}", metadata_url, e.what());
            return std::nullopt;
        } catch (const YAML::Exception &e) {  // Catch other yaml-cpp exceptions
            spdlog::error("YAML processing error for {}: {}", metadata_url, e.what());
            return std::nullopt;
        }

        // Extract data from YAML
        if (yaml_doc["sha256"] && yaml_doc["sha256"].IsScalar()) {
            metadata.package_hash_sha256 = yaml_doc["sha256"].as<std::string>();
        } else {
            spdlog::error("YAML from {} is missing 'sha256' or it's not a scalar.", metadata_url);
            return std::nullopt;
        }

        if (metadata.package_hash_sha256.length() != 64 ||
            metadata.package_hash_sha256.find_first_not_of("0123456789abcdefABCDEF") !=
                std::string::npos) {
            spdlog::error(
                "Invalid SHA256 hash format '{}' in YAML from {}",
                metadata.package_hash_sha256,
                metadata_url
            );
            return std::nullopt;
        }

        // package_size_bytes is no longer read or validated

        if (yaml_doc["description"] && yaml_doc["description"].IsScalar()) {
            metadata.release_notes = yaml_doc["description"].as<std::string>();
        } else {
            spdlog::warn(
                "YAML from {} is missing 'description' or it's not a scalar. Release notes will be "
                "empty.",
                metadata_url
            );
            metadata.release_notes = "";
        }

        spdlog::info(
            "Successfully parsed metadata from {}. Hash: {}, Description: '{}'",
            metadata_url,
            metadata.package_hash_sha256,
            metadata.release_notes
        );
        return metadata;

    } catch (const std::exception &e) {
        spdlog::error(
            "Exception while downloading or parsing metadata from {}: {}", metadata_url, e.what()
        );
        if (fs::exists(temp_file_path))
            fs::remove(temp_file_path);  // Ensure cleanup on other exceptions
        return std::nullopt;
    }
}

// Parses a local .yaml metadata file
std::optional<UpdateMetadata> parse_yaml_metadata_from_file(const fs::path &metadata_filepath)
{
    spdlog::info("Parsing local metadata file: {}", metadata_filepath.string());

    try {
        if (!fs::exists(metadata_filepath) || fs::is_empty(metadata_filepath)) {
            spdlog::error(
                "Local metadata file does not exist or is empty: {}", metadata_filepath.string()
            );
            return std::nullopt;
        }

        std::ifstream file_stream(metadata_filepath);
        if (!file_stream) {
            spdlog::error(
                "Failed to open local metadata file for reading: {}", metadata_filepath.string()
            );
            return std::nullopt;
        }

        std::string yaml_content(
            (std::istreambuf_iterator<char>(file_stream)), std::istreambuf_iterator<char>()
        );
        file_stream.close();

        if (yaml_content.empty()) {
            spdlog::error(
                "Content of local metadata file {} is empty.", metadata_filepath.string()
            );
            return std::nullopt;
        }

        spdlog::trace("YAML content from local file:\n{}", yaml_content);

        UpdateMetadata metadata;
        YAML::Node yaml_doc;
        try {
            yaml_doc = YAML::Load(yaml_content);
        } catch (const YAML::ParserException &e) {
            spdlog::error("Failed to parse YAML from {}: {}", metadata_filepath.string(), e.what());
            return std::nullopt;
        } catch (const YAML::Exception &e) {  // Catch other yaml-cpp exceptions
            spdlog::error("YAML processing error for {}: {}", metadata_filepath.string(), e.what());
            return std::nullopt;
        }

        // Extract data from YAML
        if (yaml_doc["sha256"] && yaml_doc["sha256"].IsScalar()) {
            metadata.package_hash_sha256 = yaml_doc["sha256"].as<std::string>();
        } else {
            spdlog::error(
                "YAML from {} is missing 'sha256' or it's not a scalar.", metadata_filepath.string()
            );
            return std::nullopt;
        }

        if (metadata.package_hash_sha256.length() != 64 ||
            metadata.package_hash_sha256.find_first_not_of("0123456789abcdefABCDEF") !=
                std::string::npos) {
            spdlog::error(
                "Invalid SHA256 hash format '{}' in YAML from {}",
                metadata.package_hash_sha256,
                metadata_filepath.string()
            );
            return std::nullopt;
        }

        if (yaml_doc["description"] && yaml_doc["description"].IsScalar()) {
            metadata.release_notes = yaml_doc["description"].as<std::string>();
        } else {
            spdlog::warn(
                "YAML from {} is missing 'description' or it's not a scalar. Release notes will be "
                "empty.",
                metadata_filepath.string()
            );
            metadata.release_notes = "";
        }

        spdlog::info(
            "Successfully parsed metadata from {}. Hash: {}, Description: '{}'",
            metadata_filepath.string(),
            metadata.package_hash_sha256,
            metadata.release_notes
        );
        return metadata;

    } catch (const std::exception &e) {
        spdlog::error(
            "Exception while parsing local metadata file {}: {}",
            metadata_filepath.string(),
            e.what()
        );
        return std::nullopt;
    }
}

std::thread stream_server_logs(
    const std::string &server_address, int port, const std::string &transfer_id,
    const std::string &token
)
{
    // Launch the log streaming. The thread object will be returned.
    return std::thread([server_address,
                        port,
                        transfer_id,
                        token]() {  // Return the std::thread object
        std::string url =
            fmt::format("http://{}:{}/stream-logs/{}", server_address, port, transfer_id);
        spdlog::info("Connecting to log stream in background: {}", url);

        std::string buffer;

        auto write_callback = ([&buffer](std::string data, intptr_t /*userdata*/) -> bool {
            buffer += data;
            size_t pos;
            // Process complete "data: ...\n\n" blocks
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                std::string event_str = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);  // Erase the processed part including "\n\n"

                if (event_str.rfind("data: ", 0) == 0) {         // Check if it starts with "data: "
                    std::string json_str = event_str.substr(6);  // Skip "data: "
                    try {
                        auto json_log = nlohmann::json::parse(json_str);
                        LogEntry entry;
                        entry.timestamp = json_log.value("timestamp", "N/A");
                        entry.source = json_log.value("source", "N/A");
                        entry.level = json_log.value("level", "N/A");
                        entry.message = json_log.value("message", "");

                        // Attempt to parse and reformat timestamp for better readability
                        std::tm t{};
                        std::istringstream ss(entry.timestamp);

                        std::string parsable_timestamp = entry.timestamp;
                        if (!parsable_timestamp.empty() && parsable_timestamp.back() == 'Z') {
                            parsable_timestamp.pop_back();
                        }
                        size_t dot_pos = parsable_timestamp.find('.');
                        if (dot_pos != std::string::npos) {
                            parsable_timestamp = parsable_timestamp.substr(0, dot_pos);
                        }

                        ss.str(parsable_timestamp);
                        ss >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S");

                        std::string formatted_timestamp;
                        if (ss.fail()) {
                            formatted_timestamp =
                                entry.timestamp;  // Use original if parsing failed
                        } else {
                            std::ostringstream oss_time;
                            oss_time << std::put_time(&t, "%Y-%m-%d %H:%M:%S UTC");
                            formatted_timestamp = oss_time.str();
                        }

                        // Log using spdlog, adjust level based on parsed level
                        if (entry.level == "ERROR") {
                            spdlog::error(
                                "[{}] [{}] [{}]: {}",
                                formatted_timestamp,
                                entry.level,
                                entry.source,
                                entry.message
                            );
                        } else if (entry.level == "WARNING") {
                            spdlog::warn(
                                "[{}] [{}] [{}]: {}",
                                formatted_timestamp,
                                entry.level,
                                entry.source,
                                entry.message
                            );
                        } else if (entry.level == "DEBUG") {
                            spdlog::debug(
                                "[{}] [{}] [{}]: {}",
                                formatted_timestamp,
                                entry.level,
                                entry.source,
                                entry.message
                            );
                        } else {  // INFO, SYSTEM, or others
                            spdlog::info(
                                "[{}] [{}] [{}]: {}",
                                formatted_timestamp,
                                entry.level,
                                entry.source,
                                entry.message
                            );
                        }

                        if (entry.message == "__STREAM_END__") {
                            spdlog::info(
                                "Log stream ended by server (received __STREAM_END__). Detached "
                                "thread will now exit."
                            );
                            return false;  // Stop the stream
                        }
                    } catch (const nlohmann::json::parse_error &e) {
                        spdlog::error(
                            "Failed to parse log entry JSON in background thread: {}. Data: '{}'",
                            e.what(),
                            json_str
                        );
                    } catch (const std::exception &e) {
                        spdlog::error(
                            "Error processing log entry in background thread: {}. Data: '{}'",
                            e.what(),
                            json_str
                        );
                    }
                } else if (!event_str.empty() &&
                           event_str[0] != ':') {  // Ignore SSE comments (lines starting with ':')
                                                   // and empty lines from multiple \n
                    spdlog::warn(
                        "Received non-event data or malformed event on log stream in background "
                        "thread: '{}'",
                        event_str
                    );
                }
            }
            return true;  // Continue streaming
        });

        cpr::Response response = cpr::Get(
            cpr::Url{url},
            cpr::Header{
                {"Authorization", fmt::format("Bearer {}", token)}, {"Accept", "text/event-stream"}
                // Specify that we expect an event stream
            },
            cpr::WriteCallback{write_callback},
            cpr::Timeout{0}
            // No timeout for the connection itself, rely on stream ending or server closing
        );

        // Process any remaining data in the buffer after the stream closes
        if (!buffer.empty()) {
            // Simulate the \n\n to trigger processing of the last chunk if it was incomplete
            if (buffer.find("\n\n") == std::string::npos) {
                buffer += "\n\n";       // Add delimiter to process final partial data.
                write_callback("", 0);  // Call one last time to process remaining buffer.
            }
        }

        if (response.error.code != cpr::ErrorCode::OK &&
            response.error.code != cpr::ErrorCode::OPERATION_TIMEDOUT &&
            response.status_code != OK) {
            if (response.status_code != 0) {  // If status code is 0, it's likely callback returned
                                              // false or connection issue not yielding HTTP status.
                spdlog::error(
                    "Log stream (background) connection failed or ended with error. Status: {}, "
                    "Error: {}",
                    response.status_code,
                    response.error.message
                );
            } else if (response.error.code != cpr::ErrorCode::OK &&
                       response.error.message.find("User defined callback returned false") ==
                           std::string::npos) {
                // Log error if it's not the "callback returned false" message, which is expected
                // for __STREAM_END__.
                spdlog::warn(
                    "Log stream (background) ended. CPR Error: {} (Code: {})",
                    response.error.message,
                    static_cast<int>(response.error.code)
                );
            } else {
                // This case handles when callback returned false (__STREAM_END__)
                spdlog::info("Log stream (background) completed as expected.");
            }
        } else {
            spdlog::info("Log stream (background) connection closed. Thread exiting.");
        }
    });
}
}  // namespace xvc