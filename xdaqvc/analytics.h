#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

// Analytics sidecars for recordings. The formats are shared with the device recorder
// (videoserver recording/analytics.py) and the Python SDK (thorvision_sdk.frameref, .mkv,
// .bundle), so a PC recording replays with the same tool as a device recording.
namespace xvc::analytics
{

// camera stable ID + source epoch UUID + per-epoch picture sequence. JSON encodes the
// sequence as a decimal string so 64-bit values survive JavaScript clients.
struct FrameRef {
    std::string camera;
    std::string epoch;
    std::uint64_t sequence = 0;

    auto key() const { return std::tie(camera, epoch, sequence); }
    bool operator<(const FrameRef &other) const { return key() < other.key(); }
    bool operator==(const FrameRef &other) const { return key() == other.key(); }
    nlohmann::json json() const;
    static std::optional<FrameRef> from_json(const nlohmann::json &value);
};

struct Embedded {
    FrameRef ref;
    std::uint64_t capture_time_ns = 0;
    std::uint8_t flags = 0;
};

// Version-1 payload carried in the H.265 user_data_unregistered SEI and the JPEG APP11 segment.
std::optional<Embedded> decode_payload(std::span<const std::uint8_t> payload);
// `nal_length_size` 1-4 for length-prefixed (hvc1/MKV) data, 0 for an Annex B byte stream.
std::optional<Embedded> h265_frame_ref(std::span<const std::uint8_t> frame, int nal_length_size);
std::optional<Embedded> jpeg_frame_ref(std::span<const std::uint8_t> image);

struct IndexedFrame {
    std::size_t media_index = 0;  // Presentation-order ordinal, as decoders emit frames.
    std::int64_t pts_ns = 0;
    bool keyframe = false;
    std::optional<Embedded> embedded;
};

struct MkvIndex {
    std::vector<IndexedFrame> frames;
    std::string codec;  // "h265", "jpeg" or the Matroska codec ID
    std::vector<std::string> problems;
};

// Frame index of the first video track, read from the recording itself. Truncated files
// return the complete frames before the damage and a problem note.
MkvIndex index_mkv(const std::filesystem::path &video);

struct SegmentReport {
    bool written = false;
    bool complete = false;
    nlohmann::json coverage;
};

// Writes <dir>/.analytics/<video>.analytics.jsonl, .index.jsonl and .bundle.json from SDK
// analytics records (results and gaps) whose frames are in `video`. Frames older than
// `covered_from[epoch]` were not covered by the live result cache: they make coverage
// incomplete instead of counting as "not sampled". Nothing is written if no record matches.
SegmentReport write_segment(
    const std::filesystem::path &video, const std::vector<nlohmann::json> &records,
    const std::map<std::string, std::uint64_t> &covered_from,
    const std::vector<std::string> &extra_problems = {}
);

std::filesystem::path analytics_directory(const std::filesystem::path &video);

}  // namespace xvc::analytics
