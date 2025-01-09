#pragma once

#include <gst/gstpipeline.h>

#include <filesystem>
#include <string>
#include <optional>
#include <chrono>
#include <functional>

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

/**
 * Downloads a file from a URL and verifies its SHA256 hash
 * @param url The URL to download the file from
 * @param expected_hash The expected SHA256 hash of the file
 * @param output_path The path where the downloaded file should be saved
 * @return DownloadResult containing success status and error message if any
 */
DownloadResult download_and_verify(
    const std::string& url,
    const std::string& expected_hash,
    const std::filesystem::path& output_path
);

/**
 * Calculates SHA256 hash of a file
 * @param filepath Path to the file
 * @return Optional string containing the hash, or nullopt if operation failed
 */
std::optional<std::string> calculate_sha256(const std::filesystem::path& filepath);

struct HandshakeResponse {
    bool success;
    std::string token;
    std::string error_message;
    std::chrono::system_clock::time_point expires;
};

HandshakeResponse perform_handshake(const std::string& server_address, int port);

struct FileTransferProgress {
    size_t bytes_transferred;
    size_t total_bytes;
    float progress_percentage;
};

using ProgressCallback = std::function<void(const FileTransferProgress&)>;

bool prepare_file_transfer(
    const std::string& server_address,
    int port,
    const std::string& token,
    const std::string& filename,
    const std::string& file_hash,
    size_t file_size,
    std::string& out_transfer_id
);

bool transfer_file(
    const std::string& server_address,
    int port,
    const std::string& token,
    const std::filesystem::path& file_path,
    const std::string& transfer_id,
    ProgressCallback progress_callback = nullptr
);
}  // namespace xvc
