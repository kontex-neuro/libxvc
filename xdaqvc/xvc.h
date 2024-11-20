#pragma once

#include <gst/gstpipeline.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace xvc
{

void setup_h265_srt_stream(GstPipeline *pipeline, const std::string &uri);
void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri);
void mock_high_frame_rate(GstPipeline *pipeline, const std::string &uri);

void start_h265_recording(GstPipeline *pipeline, fs::path &filepath);
void stop_h265_recording(GstPipeline *pipeline);

void start_jpeg_recording(GstPipeline *pipeline, fs::path &filepath);
void stop_jpeg_recording(GstPipeline *pipeline);

void parse_video_save_binary_h265(std::string &filepath);
void parse_video_save_binary_jpeg(std::string &filepath);

}  // namespace xvc
