#include "analytics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <set>

namespace xvc::analytics
{
namespace
{
constexpr std::array<std::uint8_t, 16> sei_uuid = {0x54, 0x56, 0x46, 0x52, 0x2d, 0x9b, 0x4c, 0x1e,
                                                   0x8a, 0x52, 0x6f, 0x0d, 0x31, 0xc7, 0x5e, 0x01};
constexpr char jpeg_tag[] = "ThorVisionFrameRef";  // The NUL terminator is part of the tag.
constexpr std::uint8_t jpeg_marker = 0xEB;
constexpr int prefix_sei = 39;
constexpr int user_data_unregistered = 5;
constexpr std::size_t peek_bytes = 8192;

std::uint64_t little_endian(std::span<const std::uint8_t> data, std::size_t at)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = value << 8 | data[at + i];
    return value;
}

std::vector<std::uint8_t> unescape(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> out;
    out.reserve(data.size());
    int zeros = 0;
    for (auto byte : data) {
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

// SEI NAL unit (header included) -> embedded frame reference, matched by UUID and version.
std::optional<Embedded> from_sei(std::span<const std::uint8_t> nal)
{
    if (nal.size() < 3) return std::nullopt;
    auto data = unescape(nal.subspan(2));
    std::size_t position = 0;
    while (position < data.size() && !(data[position] == 0x80 && position + 1 == data.size())) {
        std::size_t kind = 0, size = 0;
        while (position < data.size() && data[position] == 0xFF) kind += 255, ++position;
        if (position >= data.size()) break;
        kind += data[position++];
        while (position < data.size() && data[position] == 0xFF) size += 255, ++position;
        if (position >= data.size()) break;
        size += data[position++];
        if (position + size > data.size()) break;
        std::span<const std::uint8_t> payload(data.data() + position, size);
        if (kind == user_data_unregistered && size > 16 &&
            std::equal(sei_uuid.begin(), sei_uuid.end(), payload.begin())) {
            if (auto found = decode_payload(payload.subspan(16))) return found;
        }
        position += size;
    }
    return std::nullopt;
}

// matroskamux stores MJPEG as V_MS/VFW/FOURCC with a BITMAPINFOHEADER whose compression is MJPG.
std::string codec_name(const std::string &codec_id, const std::vector<std::uint8_t> &codec_private)
{
    if (codec_id == "V_MPEGH/ISO/HEVC") return "h265";
    if (codec_id == "V_MJPEG") return "jpeg";
    if (codec_id == "V_MS/VFW/FOURCC" && codec_private.size() >= 20) {
        std::string fourcc(codec_private.begin() + 16, codec_private.begin() + 20);
        std::transform(fourcc.begin(), fourcc.end(), fourcc.begin(),
                       [](char c) { return char(std::toupper(static_cast<unsigned char>(c))); });
        if (fourcc == "MJPG") return "jpeg";
    }
    return codec_id;
}

struct Reader {
    std::ifstream file;
    std::uint64_t size = 0;

    std::uint64_t read_vint(bool keep_marker, bool *unknown = nullptr)
    {
        auto first = file.get();
        if (first == EOF) throw std::out_of_range("eof");
        int length = 1;
        std::uint8_t mask = 0x80;
        while (length <= 8 && !(first & mask)) mask >>= 1, ++length;
        if (length > 8) throw std::invalid_argument("invalid EBML variable-length integer");
        std::uint64_t value = keep_marker ? std::uint64_t(first) : std::uint64_t(first & (mask - 1));
        for (int i = 1; i < length; ++i) {
            auto byte = file.get();
            if (byte == EOF) throw std::out_of_range("eof");
            value = value << 8 | std::uint8_t(byte);
        }
        if (unknown) *unknown = !keep_marker && value == (std::uint64_t(1) << (7 * length)) - 1;
        return value;
    }

    struct Header {
        std::uint64_t id, size, body;
        bool unknown;
    };

    Header header()
    {
        Header h{};
        h.id = read_vint(true);
        h.size = read_vint(false, &h.unknown);
        h.body = std::uint64_t(file.tellg());
        return h;
    }

    std::vector<std::uint8_t> read(std::uint64_t count)
    {
        std::vector<std::uint8_t> data(count);
        if (!file.read(reinterpret_cast<char *>(data.data()), std::streamsize(count)))
            throw std::out_of_range("eof");
        return data;
    }

    std::uint64_t tell() { return std::uint64_t(file.tellg()); }
    void seek(std::uint64_t at) { file.seekg(std::streamoff(at)); }
};

std::uint64_t big_endian(const std::vector<std::uint8_t> &data)
{
    std::uint64_t value = 0;
    for (auto byte : data) value = value << 8 | byte;
    return value;
}

void write_atomic(const std::filesystem::path &path, const std::string &text)
{
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << text;
        file.flush();
        if (!file) throw std::runtime_error("cannot write " + temporary.string());
    }
    std::filesystem::rename(temporary, path);
}

std::string lines(const std::vector<nlohmann::json> &values)
{
    std::string text;
    for (auto &value : values) text += value.dump() + "\n";
    return text;
}
}  // namespace

nlohmann::json FrameRef::json() const
{
    return {{"camera", camera}, {"epoch", epoch}, {"sequence", std::to_string(sequence)}};
}

std::optional<FrameRef> FrameRef::from_json(const nlohmann::json &value)
{
    try {
        auto sequence = value.at("sequence").get<std::string>();
        if (sequence.empty() || !std::all_of(sequence.begin(), sequence.end(), ::isdigit))
            return std::nullopt;
        return FrameRef{value.at("camera"), value.at("epoch"), std::stoull(sequence)};
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::optional<Embedded> decode_payload(std::span<const std::uint8_t> payload)
{
    constexpr std::size_t fixed = 37;
    if (payload.size() < fixed || payload[0] != 1) return std::nullopt;
    std::size_t camera_size = payload[36];
    if (payload.size() < fixed + camera_size) return std::nullopt;
    std::string epoch;
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) epoch += '-';
        epoch += std::format("{:02x}", payload[20 + i]);
    }
    Embedded found;
    found.flags = payload[1];
    found.ref = {std::string(reinterpret_cast<const char *>(payload.data() + fixed), camera_size), epoch,
                 little_endian(payload, 4)};
    found.capture_time_ns = little_endian(payload, 12);
    return found;
}

std::optional<Embedded> h265_frame_ref(std::span<const std::uint8_t> frame, int nal_length_size)
{
    auto visit = [](std::span<const std::uint8_t> nal) -> std::pair<bool, std::optional<Embedded>> {
        if (nal.size() < 2) return {true, std::nullopt};
        int kind = (nal[0] >> 1) & 0x3F;
        if (kind <= 31) return {true, std::nullopt};  // Prefix SEI precedes the picture's slices.
        if (kind == prefix_sei)
            if (auto found = from_sei(nal)) return {true, found};
        return {false, std::nullopt};
    };
    if (nal_length_size > 0) {
        std::size_t position = 0;
        while (position + nal_length_size <= frame.size()) {
            std::size_t size = 0;
            for (int i = 0; i < nal_length_size; ++i) size = size << 8 | frame[position + i];
            position += nal_length_size;
            auto nal = frame.subspan(position, std::min(size, frame.size() - position));
            position += size;
            auto [stop, found] = visit(nal);
            if (stop) return found;
        }
        return std::nullopt;
    }
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 2 < frame.size(); ++i)
        if (frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1) starts.push_back(i + 3), i += 2;
    for (std::size_t n = 0; n < starts.size(); ++n) {
        auto end = n + 1 < starts.size() ? starts[n + 1] - 3 : frame.size();
        while (end > starts[n] && frame[end - 1] == 0) --end;
        auto [stop, found] = visit(frame.subspan(starts[n], end - starts[n]));
        if (stop) return found;
    }
    return std::nullopt;
}

std::optional<Embedded> jpeg_frame_ref(std::span<const std::uint8_t> image)
{
    if (image.size() < 4 || image[0] != 0xFF || image[1] != 0xD8) return std::nullopt;
    std::size_t position = 2;
    while (position + 4 <= image.size() && image[position] == 0xFF) {
        auto marker = image[position + 1];
        if (marker == 0xDA) break;
        std::size_t length = image[position + 2] << 8 | image[position + 3];
        if (length < 2 || position + 2 + length > image.size()) break;
        auto body = image.subspan(position + 4, length - 2);
        if (marker == jpeg_marker && body.size() > sizeof(jpeg_tag) &&
            std::memcmp(body.data(), jpeg_tag, sizeof(jpeg_tag)) == 0)
            return decode_payload(body.subspan(sizeof(jpeg_tag)));
        position += 2 + length;
    }
    return std::nullopt;
}

MkvIndex index_mkv(const std::filesystem::path &video)
{
    constexpr std::uint64_t segment = 0x18538067, cluster = 0x1F43B675, timestamp = 0xE7, info = 0x1549A966,
                            timestamp_scale = 0x2AD7B1, tracks = 0x1654AE6B, track_entry = 0xAE,
                            track_number = 0xD7, track_type = 0x83, codec_id = 0x86, codec_private = 0x63A2,
                            simple_block = 0xA3, block_group = 0xA0, block = 0xA1;
    const std::set<std::uint64_t> segment_level = {cluster, info, tracks, 0x1C53BB6B, 0x1254C367,
                                                   0x114D9B74, 0x1043A770, 0x1941A469};
    struct Block {
        std::uint64_t track;
        std::int64_t time;
        bool keyframe;
        std::vector<std::uint8_t> peek;
    };
    MkvIndex index;
    std::vector<Block> blocks;
    std::uint64_t scale = 1'000'000, video_track = 0;
    int nal_length = 4;
    Reader r;
    r.file.open(video, std::ios::binary);
    if (!r.file) {
        index.problems.push_back("cannot open " + video.string());
        return index;
    }
    r.file.seekg(0, std::ios::end);
    r.size = r.tell();
    r.seek(0);
    auto read_block = [&](std::uint64_t body, std::uint64_t size, std::int64_t cluster_time, bool simple) {
        auto track = r.read_vint(false);
        auto head = r.read(3);
        auto relative = std::int16_t(head[0] << 8 | head[1]);
        auto flags = head[2];
        if (body + size > r.size) throw std::out_of_range("eof");
        Block b{track, cluster_time + relative, simple && (flags & 0x80), {}};
        if (!(flags & 0x06)) b.peek = r.read(std::min<std::uint64_t>(peek_bytes, body + size - r.tell()));
        blocks.push_back(std::move(b));
    };
    try {
        auto ebml = r.header();
        if (ebml.id != 0x1A45DFA3) throw std::invalid_argument("not a Matroska/EBML file");
        r.seek(ebml.body + ebml.size);
        auto seg = r.header();
        if (seg.id != segment) throw std::invalid_argument("Matroska segment not found");
        auto segment_end = seg.unknown ? r.size : std::min(r.size, seg.body + seg.size);
        std::int64_t cluster_time = 0;
        while (r.tell() < segment_end) {
            auto element = r.header();
            auto end = element.unknown ? segment_end : element.body + element.size;
            if (element.id == info) {
                while (r.tell() < end) {
                    auto child = r.header();
                    if (child.id == timestamp_scale) scale = big_endian(r.read(child.size));
                    r.seek(child.body + child.size);
                }
            } else if (element.id == tracks) {
                while (r.tell() < end) {
                    auto entry = r.header();
                    if (entry.id == track_entry) {
                        std::map<std::uint64_t, std::vector<std::uint8_t>> fields;
                        while (r.tell() < entry.body + entry.size) {
                            auto field = r.header();
                            if (field.id == track_number || field.id == track_type || field.id == codec_id ||
                                field.id == codec_private)
                                fields[field.id] = r.read(field.size);
                            r.seek(field.body + field.size);
                        }
                        if (!video_track && big_endian(fields[track_type]) == 1) {
                            video_track = big_endian(fields[track_number]);
                            std::string id(fields[codec_id].begin(), fields[codec_id].end());
                            id.erase(id.find_last_not_of('\0') + 1);
                            auto &priv = fields[codec_private];
                            index.codec = codec_name(id, priv);
                            if (index.codec == "h265" && priv.size() > 21) nal_length = (priv[21] & 3) + 1;
                        }
                    }
                    r.seek(entry.body + entry.size);
                }
            } else if (element.id == cluster) {
                while (r.tell() < end) {
                    auto position = r.tell();
                    auto child = r.header();
                    if (element.unknown && segment_level.contains(child.id)) {
                        r.seek(position);  // An unknown-size cluster ends at the next top-level element.
                        break;
                    }
                    if (child.unknown) throw std::invalid_argument("unsupported unknown-size cluster child");
                    if (child.id == timestamp) cluster_time = std::int64_t(big_endian(r.read(child.size)));
                    else if (child.id == simple_block) read_block(child.body, child.size, cluster_time, true);
                    else if (child.id == block_group) {
                        while (r.tell() < child.body + child.size) {
                            auto field = r.header();
                            if (field.id == block) read_block(field.body, field.size, cluster_time, false);
                            r.seek(field.body + field.size);
                        }
                    }
                    r.seek(child.body + child.size);
                    if (r.tell() > r.size) throw std::out_of_range("eof");
                }
                continue;
            }
            if (element.unknown) break;
            r.seek(end);
        }
    } catch (const std::out_of_range &) {
        index.problems.push_back("file is truncated; frames after the last complete block are not indexed");
    } catch (const std::invalid_argument &error) {
        index.problems.push_back(error.what());
    }
    for (auto &b : blocks) {
        if (b.track != video_track || b.peek.empty()) continue;
        IndexedFrame frame;
        frame.pts_ns = b.time * std::int64_t(scale);
        frame.keyframe = b.keyframe;
        frame.embedded = index.codec == "jpeg" ? jpeg_frame_ref(b.peek) : h265_frame_ref(b.peek, nal_length);
        index.frames.push_back(std::move(frame));
    }
    std::stable_sort(index.frames.begin(), index.frames.end(),
                     [](const auto &a, const auto &b) { return a.pts_ns < b.pts_ns; });
    for (std::size_t i = 0; i < index.frames.size(); ++i) index.frames[i].media_index = i;
    return index;
}

std::filesystem::path analytics_directory(const std::filesystem::path &video)
{
    return video.parent_path() / ".analytics";
}

SegmentReport write_segment(
    const std::filesystem::path &video, const std::vector<nlohmann::json> &records,
    const std::map<std::string, std::uint64_t> &covered_from, const std::vector<std::string> &extra_problems
)
{
    SegmentReport report;
    auto index = index_mkv(video);
    auto problems = index.problems;
    problems.insert(problems.end(), extra_problems.begin(), extra_problems.end());
    std::map<FrameRef, const IndexedFrame *> frames;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> range;
    for (auto &frame : index.frames) {
        if (!frame.embedded) continue;
        auto &ref = frame.embedded->ref;
        frames[ref] = &frame;
        auto [it, inserted] = range.try_emplace(ref.epoch, ref.sequence, ref.sequence);
        if (!inserted) {
            it->second.first = std::min(it->second.first, ref.sequence);
            it->second.second = std::max(it->second.second, ref.sequence);
        }
    }
    std::vector<nlohmann::json> matched, gaps;
    std::map<FrameRef, std::string> statuses;
    for (auto &record : records) {
        auto kind = record.value("kind", "");
        if (kind == "result") {
            auto ref = FrameRef::from_json(record.value("frame", nlohmann::json{}));
            if (ref && frames.contains(*ref) && !statuses.contains(*ref)) {
                statuses[*ref] = record.value("status", "");
                matched.push_back(record);
            }
        } else if (kind == "gap") {
            auto epoch = record.value("epoch", "");
            if (!range.contains(epoch)) continue;
            auto first = std::stoull(record.value("first_sequence", "0"));
            auto last = std::stoull(record.value("last_sequence", "0"));
            if (first <= range[epoch].second && last >= range[epoch].first) gaps.push_back(record);
        }
    }
    if (matched.empty() && gaps.empty()) return report;
    std::sort(matched.begin(), matched.end(), [](const auto &a, const auto &b) {
        return std::stoull(a["frame"]["sequence"].template get<std::string>()) <
               std::stoull(b["frame"]["sequence"].template get<std::string>());
    });
    nlohmann::json counts = {{"processed_objects", 0}, {"processed_empty", 0}, {"not_sampled", 0},
                             {"failed", 0}, {"gap", 0}};
    int unidentified = 0, uncovered = 0;
    std::vector<nlohmann::json> index_rows;
    for (auto &frame : index.frames) {
        nlohmann::json row = {{"schema", "thorvision.frame-index/1"}, {"media_index", frame.media_index},
                              {"pts_ns", std::to_string(frame.pts_ns)}, {"keyframe", frame.keyframe},
                              {"frame", nullptr}};
        if (!frame.embedded) {
            ++unidentified;
            counts["not_sampled"] = counts["not_sampled"].get<int>() + 1;
            index_rows.push_back(row);
            continue;
        }
        auto &ref = frame.embedded->ref;
        row["frame"] = ref.json();
        row["capture_time_ns"] = std::to_string(frame.embedded->capture_time_ns);
        index_rows.push_back(row);
        std::string status;
        if (auto it = statuses.find(ref); it != statuses.end()) status = it->second;
        else if (std::any_of(gaps.begin(), gaps.end(), [&](const auto &gap) {
                     return gap.value("epoch", "") == ref.epoch &&
                            std::stoull(gap.value("first_sequence", "0")) <= ref.sequence &&
                            ref.sequence <= std::stoull(gap.value("last_sequence", "0"));
                 }))
            status = "gap";
        else {
            auto covered = covered_from.find(ref.epoch);
            if (covered == covered_from.end() || ref.sequence < covered->second) ++uncovered;
            status = "not_sampled";
        }
        if (counts.contains(status)) counts[status] = counts[status].get<int>() + 1;
    }
    std::map<std::string, std::vector<std::uint64_t>> by_run;
    std::set<std::string> renderers, producers;
    double target_fps = 0;
    for (auto &record : matched) {
        by_run[record.value("run_id", "")].push_back(std::stoull(record.value("result_seq", "0")));
        renderers.insert(record.value("renderer_version", ""));
        auto producer = record.value("producer", nlohmann::json::object());
        producers.insert(producer.dump());
        if (producer.contains("target_fps") && producer["target_fps"].is_number())
            target_fps = producer["target_fps"].get<double>();
    }
    std::uint64_t missing = 0;
    for (auto &[run, sequences] : by_run) {
        std::sort(sequences.begin(), sequences.end());
        sequences.erase(std::unique(sequences.begin(), sequences.end()), sequences.end());
        missing += sequences.back() - sequences.front() + 1 - sequences.size();
    }
    report.complete = problems.empty() && gaps.empty() && !missing && !unidentified && !uncovered &&
                      counts["failed"].get<int>() == 0 && renderers.size() <= 1;
    report.coverage = {{"complete", report.complete}, {"counts", counts}, {"missing_results", missing},
                       {"unidentified_frames", unidentified}, {"uncovered_frames", uncovered},
                       {"problems", problems},
                       {"finalized_at", std::chrono::duration<double>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count()}};
    auto packages = nlohmann::json::array();
    for (auto &producer : producers) packages.push_back(nlohmann::json::parse(producer));
    auto directory = analytics_directory(video);
    std::filesystem::create_directories(directory);
    auto name = video.filename().string();
    std::vector<nlohmann::json> analytics = gaps;
    analytics.insert(analytics.end(), matched.begin(), matched.end());
    write_atomic(directory / (name + ".analytics.jsonl"), lines(analytics));
    write_atomic(directory / (name + ".index.jsonl"), lines(index_rows));
    nlohmann::json manifest = {
        {"schema", "thorvision.bundle/1"},
        {"renderer_version", renderers.empty() ? nlohmann::json(nullptr) : nlohmann::json(*renderers.begin())},
        {"package", packages},
        {"analytics", {{"target_fps", target_fps > 0 ? nlohmann::json(target_fps) : nlohmann::json(nullptr)}}},
        {"codec", index.codec},
        {"segments", nlohmann::json::array({{{"name", name}, {"video", "../" + name},
                                             {"index", name + ".index.jsonl"},
                                             {"analytics", nlohmann::json::array({name + ".analytics.jsonl"})}}})},
        {"coverage", report.coverage}};
    write_atomic(directory / (name + ".bundle.json"), manifest.dump(1) + "\n");
    report.written = true;
    return report;
}

}  // namespace xvc::analytics
