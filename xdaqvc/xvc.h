#pragma once

#include <gst/gstpipeline.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>


namespace fs = std::filesystem;

namespace xvc
{

void setup_h265_srt_stream(GstPipeline *pipeline, const std::string &uri);
void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri);
void mock_camera(GstPipeline *pipeline, const std::string &);

void start_h265_recording(
    GstPipeline *pipeline, fs::path &filepath, bool continuous, int max_size_time, int max_files
);
void stop_h265_recording(GstPipeline *pipeline);

void start_jpeg_recording(
    GstPipeline *pipeline, fs::path &filepath, bool continuous, int max_size_time, int max_files
);
void stop_jpeg_recording(GstPipeline *pipeline);

void parse_video_save_binary_h265(const std::string &filepath);
void parse_video_save_binary_jpeg(const std::string &filepath);

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

DownloadResult download_and_verify(
    const std::string &url, const std::string &expected_hash,
    const std::filesystem::path &output_path
);

std::optional<std::string> calculate_sha256(const std::filesystem::path &filepath);

struct HandshakeResponse {
    bool success;
    std::string token;
    std::string error_message;
    std::chrono::system_clock::time_point expires;
};

HandshakeResponse perform_handshake(const std::string &server_address, int port);

struct FileTransferProgress {
    size_t bytes_transferred;
    size_t total_bytes;
    float progress_percentage;
};

using ProgressCallback = std::function<void(const FileTransferProgress &)>;

bool prepare_file_transfer(
    const std::string &server_address, int port, const std::string &token,
    const std::string &filename, const std::string &file_hash, size_t file_size,
    std::string &out_transfer_id
);

bool transfer_file(
    const std::string &server_address, int port, const std::string &token,
    const std::filesystem::path &file_path, const std::string &transfer_id,
    ProgressCallback progress_callback = nullptr
);

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

struct UpdateResult {
    bool success;
    std::string error_message;
    Version current_version;
    Version available_version;
    bool update_needed;
};

// Get server version
std::optional<Version> get_server_version(const std::string &server_address, int port);

// Get version table from CDN
std::optional<VersionTable> get_version_table(const std::string &table_url);

// Main update function
UpdateResult update_server(
    const std::string &server_address,
    int server_port,         // Port of the server to be updated
    int update_server_port,  // Port of the update server
    const std::string &table_url, const std::filesystem::path &update_dir,
    const Version &client_version, bool skip_version_check = false,
    const std::optional<Version> &force_version = std::nullopt
);
}  // namespace xvc
