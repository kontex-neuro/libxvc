#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "xdaqvc/analytics.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace xvc::analytics;

namespace
{
// Fixtures are device loop_recorder segments; *.index.json is thorvision_sdk.mkv output for them.
fs::path data(const std::string &name) { return fs::path(XVC_TEST_DATA_DIR) / "analytics" / name; }

json read_json(const fs::path &path)
{
    std::ifstream file(path);
    return json::parse(file);
}

std::vector<json> read_lines(const fs::path &path)
{
    std::vector<json> values;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
        if (!line.empty()) values.push_back(json::parse(line));
    return values;
}

json result(const FrameRef &ref, std::uint64_t result_seq, bool objects = true)
{
    return {{"schema", "thorvision.analytics.record/1"}, {"kind", "result"},
            {"status", objects ? "processed_objects" : "processed_empty"}, {"frame", ref.json()},
            {"run_id", "run-1"}, {"result_seq", std::to_string(result_seq)},
            {"producer", {{"package", "box"}, {"target_fps", 5.0}}}, {"renderer_version", "1"},
            {"result", json::object()}};
}

struct Scratch {
    fs::path root = fs::temp_directory_path() / ("xvc-analytics-" + std::to_string(std::rand()));
    Scratch() { fs::create_directories(root); }
    ~Scratch() { fs::remove_all(root); }
    fs::path copy(const std::string &name)
    {
        auto target = root / (name + ".mkv");
        fs::copy_file(data(name + ".mkv"), target);
        return target;
    }
};
}  // namespace

TEST_CASE("C++ frame index matches the Python SDK on real recorder output", "[analytics]")
{
    for (std::string codec : {"h265", "jpeg"}) {
        auto expected = read_json(data(codec + ".index.json"));
        auto index = index_mkv(data(codec + ".mkv"));
        INFO(codec);
        CHECK(index.codec == expected["codec"].get<std::string>());
        CHECK(index.problems.empty());
        REQUIRE(index.frames.size() == expected["frames"].size());
        for (std::size_t i = 0; i < index.frames.size(); ++i) {
            auto &frame = index.frames[i];
            auto &row = expected["frames"][i];
            CHECK(frame.media_index == row["media_index"].get<std::size_t>());
            CHECK(std::to_string(frame.pts_ns) == row["pts_ns"].get<std::string>());
            CHECK(frame.keyframe == row["keyframe"].get<bool>());
            REQUIRE(frame.embedded);
            CHECK(frame.embedded->ref.json() == row["frame"]);
            CHECK(std::to_string(frame.embedded->capture_time_ns) == row["capture_time_ns"].get<std::string>());
        }
    }
}

TEST_CASE("Truncated recordings keep complete frames and report the damage", "[analytics]")
{
    Scratch scratch;
    auto video = scratch.copy("h265");
    fs::resize_file(video, fs::file_size(video) / 2);
    auto index = index_mkv(video);
    CHECK(!index.frames.empty());
    CHECK(index.frames.size() < 15);
    REQUIRE(index.problems.size() == 1);
    CHECK(index.problems[0].find("truncated") != std::string::npos);
}

TEST_CASE("Segment sidecars match records to frames actually recorded", "[analytics]")
{
    Scratch scratch;
    auto video = scratch.copy("jpeg");
    auto frames = index_mkv(video).frames;
    REQUIRE(frames.size() == 15);
    auto ref = [&](std::size_t i) { return frames[i].embedded->ref; };
    std::map<std::string, std::uint64_t> covered{{ref(0).epoch, 0}};

    SECTION("complete coverage")
    {
        std::vector<json> records{result(ref(0), 1), result(ref(3), 2, false), result(ref(6), 3)};
        auto outside = ref(14);
        outside.sequence += 100;
        records.push_back(result(outside, 4));  // Not in this file: never attached.
        auto report = write_segment(video, records, covered);
        REQUIRE(report.written);
        CHECK(report.complete);
        CHECK(report.coverage["counts"]["processed_objects"] == 2);
        CHECK(report.coverage["counts"]["processed_empty"] == 1);
        CHECK(report.coverage["counts"]["not_sampled"] == 12);
        auto directory = analytics_directory(video);
        auto stored = read_lines(directory / "jpeg.mkv.analytics.jsonl");
        REQUIRE(stored.size() == 3);
        CHECK(read_lines(directory / "jpeg.mkv.index.jsonl").size() == 15);
        auto manifest = read_json(directory / "jpeg.mkv.bundle.json");
        CHECK(manifest["schema"] == "thorvision.bundle/1");
        CHECK(manifest["segments"][0]["video"] == "../jpeg.mkv");
        CHECK(manifest["analytics"]["target_fps"] == 5.0);
    }
    SECTION("missing results, gaps and cache coverage make it incomplete")
    {
        std::vector<json> records{result(ref(2), 1), result(ref(8), 3)};
        records.push_back({{"schema", "thorvision.analytics.record/1"}, {"kind", "gap"}, {"status", "gap"},
                           {"camera", ref(0).camera}, {"epoch", ref(0).epoch},
                           {"first_sequence", std::to_string(ref(10).sequence)},
                           {"last_sequence", std::to_string(ref(11).sequence)}, {"run_id", "run-1"},
                           {"reason", "delivery"}});
        auto report = write_segment(video, records, {{ref(0).epoch, ref(5).sequence}});
        REQUIRE(report.written);
        CHECK_FALSE(report.complete);
        CHECK(report.coverage["missing_results"] == 1);
        CHECK(report.coverage["counts"]["gap"] == 2);
        CHECK(report.coverage["uncovered_frames"] == 4);  // Frames 0, 1, 3, 4 predate the result cache.
    }
    SECTION("no matching records writes nothing")
    {
        auto report = write_segment(video, {}, covered);
        CHECK_FALSE(report.written);
        CHECK_FALSE(fs::exists(analytics_directory(video) / "jpeg.mkv.bundle.json"));
    }
}

TEST_CASE("Frame reference payloads are matched by version", "[analytics]")
{
    std::vector<std::uint8_t> payload{1, 1, 0, 0};
    for (int i = 0; i < 8; ++i) payload.push_back(i == 0 ? 42 : 0);
    for (int i = 0; i < 8; ++i) payload.push_back(0);
    for (int i = 0; i < 16; ++i) payload.push_back(std::uint8_t(i));
    payload.push_back(3);
    for (char c : std::string("cam")) payload.push_back(std::uint8_t(c));
    auto decoded = decode_payload(payload);
    REQUIRE(decoded);
    CHECK(decoded->ref.sequence == 42);
    CHECK(decoded->ref.camera == "cam");
    CHECK(decoded->ref.epoch == "00010203-0405-0607-0809-0a0b0c0d0e0f");
    payload[0] = 2;
    CHECK_FALSE(decode_payload(payload));
    CHECK_FALSE(FrameRef::from_json({{"camera", "c"}, {"epoch", "e"}, {"sequence", 5}}));
}
