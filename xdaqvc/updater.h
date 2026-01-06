#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>



namespace fs = std::filesystem;


namespace xvc
{

struct DownloadResult {
    bool success;
    std::string error_message;
};

class Version
{
public:
    int major;
    int minor;
    int patch;

    bool operator==(const Version &other) const;
    bool operator>(const Version &other) const;
    bool operator<(const Version &other) const;
    bool operator>=(const Version &other) const;
    bool operator<=(const Version &other) const;

    static std::optional<Version> from_string(const std::string &version_str);
    std::string to_string() const;
};

struct HandshakeResponse {
    bool success;
    std::string token;
    std::string error_message;
    std::chrono::system_clock::time_point expires;
};

struct FileTransferProgress {
    size_t bytes_transferred;
    size_t total_bytes;
    float progress_percentage;
};

struct UpdateInfo {
    Version version;
    std::string release_date;
    std::string update_url;
    std::string hash;
    Version min_client_version;
    std::string description;
};

struct VersionTable {
    Version latest_version;
    std::vector<UpdateInfo> versions;
};

struct LocalVersionInfo {
    Version api_version;
    Version build_version;
};

struct RemoteUpdateTarget {
    Version api_version;
    Version build_version;
    std::string package_url;
    std::string metadata_url;
    std::string package_filename;
    std::string metadata_filename;
};

struct UpdateMetadata {
    std::string package_hash_sha256;
    std::string release_notes;
};

struct UpdateOrchestrationResult {
    bool success = false;
    std::string error_message;
    bool update_available_and_downloaded = false;
    fs::path downloaded_package_path;
    std::optional<UpdateMetadata> downloaded_package_metadata;

    std::optional<Version> local_api_version;
    std::optional<Version> local_build_version;
    std::optional<Version> latest_remote_api_version;
    std::optional<Version> target_remote_api_version;
    std::optional<Version> target_remote_build_version;
};

struct LogEntry {
    std::string timestamp;
    std::string source;
    std::string level;
    std::string message;
};

struct UpdateResult {
    bool success;
    std::string error_message;
    Version current_version;
    Version available_version;
    bool update_needed;
};

DownloadResult download_and_verify(
    const std::string &url, const std::string &expected_hash,
    const std::filesystem::path &output_path
);

std::optional<std::string> calculate_sha256(const fs::path &filepath);

HandshakeResponse perform_handshake(const std::string &server_address, int port);

bool prepare_file_transfer(
    const std::string &server_address, int port, const std::string &token,
    const std::string &filename, const std::string &file_hash, size_t file_size,
    std::string &out_transfer_id
);

bool transfer_file_and_update_server(
    const std::string &server_address, int port, const std::string &token,
    const fs::path &file_path, const std::string &transfer_id,
    std::function<void(const FileTransferProgress &)> progress_callback = nullptr
);

std::optional<Version> get_server_version(const std::string &server_address, int port);

std::optional<VersionTable> get_version_table(const std::string &table_url);

std::tuple<std::optional<Version>, std::optional<Version>, std::string, std::string>
parse_filename_details(const std::string &filename);

std::optional<LocalVersionInfo> get_local_device_versions(const std::string &device_server_base_url
);

std::optional<std::vector<std::string>> list_remote_directory_contents(
    const std::string &directory_url
);

std::optional<std::vector<Version>> get_available_remote_api_versions(
    const std::string &storage_endpoint_url
);

std::optional<RemoteUpdateTarget> find_latest_build_for_api_version(
    const std::string &storage_endpoint_url, const Version &target_api_version
);

std::optional<RemoteUpdateTarget> find_specific_build_version(
    const std::string &storage_endpoint_url, const Version &target_api_version,
    const Version &target_build_version
);

std::optional<UpdateMetadata> download_and_parse_yaml_metadata(
    const std::string &metadata_url, const std::string &temp_download_dir
);

std::optional<UpdateMetadata> parse_yaml_metadata_from_file(const fs::path &metadata_filepath);

UpdateOrchestrationResult check_for_and_download_updates(
    const std::string &local_device_server_base_url, const std::string &cloud_storage_endpoint,
    const fs::path &download_directory, const std::optional<Version> &force_api_v = std::nullopt,
    const std::optional<Version> &force_build_v = std::nullopt
);

UpdateOrchestrationResult force_download_updates(
    const std::string &cloud_storage_endpoint, const fs::path &download_directory,
    const Version &force_api_v, const std::optional<Version> &force_build_v = std::nullopt
);

UpdateResult update_server(
    const std::string &server_address, int server_port, int update_server_port,
    const std::string &table_url, const fs::path &update_dir, const Version &client_version,
    bool skip_version_check = false, const std::optional<Version> &force_version = std::nullopt
);

std::thread stream_server_logs(
    const std::string &server_address, int port, const std::string &transfer_id,
    const std::string &token
);

}  // namespace xvc