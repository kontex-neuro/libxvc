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

// TODO
void setup_jpeg_srt_stream(GstPipeline *pipeline, const std::string &uri);

bool start_jpeg_recording(GstPipeline *, const RecordConfig &);
bool stop_jpeg_recording(GstPipeline *);

}  // namespace xvc