#pragma once

#define LIBXVC_API_VER "0.2.0"

#include <gst/gstpipeline.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace xvc
{

enum class TimeUnit { Seconds = 0, Minutes, Hours, Days };

void setup_h265_srt_stream(GstPipeline *pipeline, const std::string &uri);
void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri);

void mock_camera(
    GstPipeline *pipeline, [[maybe_unused]] const std::string &uri, const std::string &current_cap
);

void start_h265_recording(
    GstPipeline *pipeline, fs::path &filepath, bool continuous, int max_size_time, int max_files
);
void stop_h265_recording(GstPipeline *pipeline);

void start_jpeg_recording(
    GstPipeline *pipeline, fs::path &filepath, bool continuous = true, int max_size_time = 10,
    TimeUnit unit = TimeUnit::Minutes, bool loop = false, int max_files = 10
);
void stop_jpeg_recording(GstPipeline *pipeline);

void parse_video_save_binary_h265(const std::string &filepath);
void parse_video_save_binary_jpeg(const std::string &filepath);

}  // namespace xvc