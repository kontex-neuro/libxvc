#include "updater.h"

#include <cpr/cpr.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <thread>


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

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }

    auto calculated = ss.str();
    spdlog::info("Calculated hash: {}", calculated);
    return calculated;
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

bool transfer_file(
    const std::string &server_address, int port, const std::string &token,
    const fs::path &file_path, const std::string &transfer_id,
    std::function<void(const FileTransferProgress &)> progress_callback
)
{
    auto timeout = std::chrono::seconds{30};
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
                "File transfer failed with status code: {} ({})",
                response.status_code,
                response.text
            );
            return false;
        }

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

std::optional<VersionTable> get_version_table(const std::string &table_url)
{
    try {
        auto response = cpr::Get(
            cpr::Url{table_url}, cpr::Timeout{5s}, cpr::VerifySsl{false}
            // Add this if needed for self-signed certs
        );

        if (response.status_code == OK) {
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
    const std::string &table_url, const fs::path &update_dir,
    [[maybe_unused]] const Version &client_version, bool skip_version_check,
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
        fs::create_directories(update_dir);

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
        auto file_size = fs::file_size(update_file);

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