#pragma once

#define LIBXVC_API_VER "0.0.3"

#include <gst/gstpipeline.h>

#include <filesystem>
#include <string>


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

}  // namespace xvc
