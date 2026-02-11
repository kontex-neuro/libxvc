#pragma once

#include <gst/gstpipeline.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace xvc
{

struct RecordConfig {
    std::filesystem::path _path;
    bool _split;
    std::chrono::seconds _max_size_time;

    RecordConfig(
        std::filesystem::path path = std::filesystem::current_path(), bool split = false,
        std::chrono::seconds max_size_time = std::chrono::seconds(10)
    )
        : _path(std::move(path)), _split(split), _max_size_time(max_size_time)
    {
    }
};

void setup_h265_srt_stream(GstPipeline *pipeline, const std::string &uri);
void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri);

void start_h265_recording(
    GstPipeline *pipeline, std::filesystem::path &filepath, bool continuous, int max_size_time,
    int max_files
);
void stop_h265_recording(GstPipeline *pipeline);

bool start_jpeg_recording(GstPipeline *, const RecordConfig &);
bool stop_jpeg_recording(GstPipeline *);

void parse_video_save_binary_h265(const std::string &filepath);
void parse_video_save_binary_jpeg(const std::string &filepath);

}  // namespace xvc