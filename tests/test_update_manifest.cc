// Tier 1 (ADR 0009): pure parsing logic, no network and no device.
//
// The parser's strictness is one of the two places most of this API's risk lives (the other
// is the ADR 0007 policy table). It is tested against the REAL wire format in
// tests/data/versions.json rather than a hand-written approximation.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "../xdaqvc/update/manifest.h"

namespace fs = std::filesystem;
using namespace xvc::update;

namespace
{

// XVC_TEST_DATA_DIR is defined by CMake so the fixture resolves regardless of the working
// directory the test binary happens to be launched from.
std::string read_fixture(std::string_view name)
{
    const auto path = fs::path{XVC_TEST_DATA_DIR} / name;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Reparses the real fixture after removing one field, so the "required field" assertions
// test the actual document rather than a minimal stand-in.
std::string fixture_without(std::string_view pointer)
{
    auto json = nlohmann::json::parse(read_fixture("versions.json"));
    json.at(nlohmann::json::json_pointer{std::string{pointer}}.parent_pointer())
        .erase(nlohmann::json::json_pointer{std::string{pointer}}.back());
    return json.dump();
}

}  // namespace

TEST_CASE("parse_versions_json accepts the real versions.json", "[update][manifest]")
{
    auto matrix = parse_versions_json(read_fixture("versions.json"));
    REQUIRE(matrix.has_value());

    CHECK(matrix->updated_at == "2026-07-22T10:49:25Z");
    REQUIRE(matrix->versions.size() == 3);

    // latest_release is parsed because it is part of the wire format, even though ADR 0007
    // policy reads nothing from it.
    REQUIRE(matrix->latest_release.contains("linux-arm64"));
    CHECK(matrix->latest_release.at("linux-arm64") == Version{0, 1, 2});

    const auto &newest = matrix->versions.front();
    CHECK(newest.version == Version{0, 1, 2});
    CHECK(newest.platform == "linux-arm64");
    CHECK(newest.environment == "release");
    CHECK(newest.status == "stable");
    CHECK(newest.released_at == "2026-07-22T10:48:38Z");

    // download_base is present on a matrix entry and carries slashes on BOTH ends -- the
    // URL join in Phase 4 must not duplicate them.
    REQUIRE(newest.download_base.has_value());
    CHECK(*newest.download_base == "/dist/v0.1.2/linux-arm64/");

    REQUIRE(newest.files.size() == 1);
    CHECK(newest.files[0].name == "ThorVisionServer-0.1.2-linux-arm64.tar.xz");
    CHECK(newest.files[0].size == 46070024);
    CHECK(newest.files[0].sha256 ==
          "7819a0bd00b1aa859df4cd410fc2bb97fe6f47cd505a30e6b5ef7d4697a18ff4");

    // Absent from every real entry; must default rather than fail.
    CHECK(newest.deprecation_reason.empty());
    CHECK_FALSE(newest.changelog.empty());
}

TEST_CASE("parse_versions_json ignores unknown fields", "[update][manifest]")
{
    // Forward compatibility is a requirement: shipped clients cannot be updated in lockstep
    // with the cloud, so a field added server-side must not break an old client.
    auto json = nlohmann::json::parse(read_fixture("versions.json"));
    json["some_future_top_level_field"] = {{"nested", true}};
    json["versions"][0]["some_future_entry_field"] = 42;
    json["versions"][0]["files"][0]["some_future_file_field"] = "ignored";

    auto matrix = parse_versions_json(json.dump());
    REQUIRE(matrix.has_value());
    CHECK(matrix->versions.size() == 3);
    CHECK(matrix->versions.front().version == Version{0, 1, 2});
}

TEST_CASE("parse_versions_json rejects each missing required field", "[update][manifest]")
{
    // Every one of these is a hard MalformedResponse -- an entry missing any of them cannot
    // be acted on at all.
    auto expect_malformed = [](const std::string &text) {
        auto result = parse_versions_json(text);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::MalformedResponse);
    };

    SECTION("version") { expect_malformed(fixture_without("/versions/0/version")); }
    SECTION("platform") { expect_malformed(fixture_without("/versions/0/platform")); }
    SECTION("download_base") { expect_malformed(fixture_without("/versions/0/download_base")); }
    SECTION("files") { expect_malformed(fixture_without("/versions/0/files")); }
    SECTION("files[0].name") { expect_malformed(fixture_without("/versions/0/files/0/name")); }
    SECTION("files[0].size") { expect_malformed(fixture_without("/versions/0/files/0/size")); }
    SECTION("files[0].sha256") { expect_malformed(fixture_without("/versions/0/files/0/sha256")); }
    SECTION("versions array itself") { expect_malformed(fixture_without("/versions")); }
}

TEST_CASE("parse_versions_json rejects malformed documents", "[update][manifest]")
{
    auto expect_malformed = [](std::string_view text) {
        auto result = parse_versions_json(text);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::MalformedResponse);
    };

    expect_malformed("");
    expect_malformed("not json at all");
    expect_malformed("[1, 2, 3]");                    // valid JSON, wrong shape
    expect_malformed(R"({"versions": "not-array"})");
    expect_malformed(R"({"versions": [{"version": "not.a.version", "platform": "linux-arm64",
                        "download_base": "/d/", "files": [{"name":"a","size":1,"sha256":"b"}]}]})");
}

TEST_CASE("parse_versions_json rejects a non-numeric size", "[update][manifest]")
{
    // size arrives as a string from a sloppy generator: must fail rather than silently
    // yielding 0, which would make verify_file() compare against the wrong number.
    auto json = nlohmann::json::parse(read_fixture("versions.json"));
    json["versions"][0]["files"][0]["size"] = "46070024";

    auto result = parse_versions_json(json.dump());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("parse_manifest_json accepts an entry without download_base", "[update][manifest]")
{
    // ADR 0008: a manifest is exactly a versions[] entry minus download_base, which is why
    // one parser serves both.
    auto entry = parse_manifest_json(read_fixture("manifest.json"));
    REQUIRE(entry.has_value());

    CHECK(entry->version == Version{0, 1, 2});
    CHECK(entry->platform == "linux-arm64");
    CHECK(entry->status == "stable");
    CHECK_FALSE(entry->download_base.has_value());

    REQUIRE(entry->files.size() == 1);
    CHECK(entry->files[0].size == 46070024);
    CHECK(entry->files[0].sha256 ==
          "7819a0bd00b1aa859df4cd410fc2bb97fe6f47cd505a30e6b5ef7d4697a18ff4");
}

TEST_CASE("parse_manifest_json still requires the other fields", "[update][manifest]")
{
    auto json = nlohmann::json::parse(read_fixture("manifest.json"));
    json.erase("platform");

    auto result = parse_manifest_json(json.dump());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("select_latest_release picks the highest stable version", "[update][manifest]")
{
    auto matrix = parse_versions_json(read_fixture("versions.json"));
    REQUIRE(matrix.has_value());

    auto latest = select_latest_release(*matrix, "linux-arm64");
    REQUIRE(latest.has_value());
    CHECK(latest->version == Version{0, 1, 2});
}

TEST_CASE("select_latest_release reports a platform miss distinctly", "[update][manifest]")
{
    // ADR 0004: a wrong or unknown platform must NOT look like "up to date". This is
    // precisely how that class of bug reaches production.
    auto matrix = parse_versions_json(read_fixture("versions.json"));
    REQUIRE(matrix.has_value());

    auto result = select_latest_release(*matrix, "windows-x86_64");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::PlatformNotFound);
}

TEST_CASE("select_latest_release skips deprecated and non-release entries", "[update][manifest]")
{
    auto json = nlohmann::json::parse(read_fixture("versions.json"));
    json["versions"][0]["status"] = "deprecated";  // 0.1.2 recalled

    auto matrix = parse_versions_json(json.dump());
    REQUIRE(matrix.has_value());

    auto latest = select_latest_release(*matrix, "linux-arm64");
    REQUIRE(latest.has_value());
    CHECK(latest->version == Version{0, 1, 1});  // falls back to the newest still-stable
}

TEST_CASE("select_exact_release finds a specific version", "[update][manifest]")
{
    auto matrix = parse_versions_json(read_fixture("versions.json"));
    REQUIRE(matrix.has_value());

    auto entry = select_exact_release(*matrix, "linux-arm64", Version{0, 1, 1});
    REQUIRE(entry.has_value());
    CHECK(entry->version == Version{0, 1, 1});

    auto missing = select_exact_release(*matrix, "linux-arm64", Version{9, 9, 9});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::VersionNotFound);
}

TEST_CASE("select_exact_release gates deprecated releases", "[update][manifest]")
{
    auto json = nlohmann::json::parse(read_fixture("versions.json"));
    json["versions"][1]["status"] = "deprecated";
    json["versions"][1]["deprecation_reason"] = "corrupts the SPI log on rotation";

    auto matrix = parse_versions_json(json.dump());
    REQUIRE(matrix.has_value());

    auto rejected = select_exact_release(*matrix, "linux-arm64", Version{0, 1, 1});
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == ErrorCode::ReleaseDeprecated);
    // The reason is carried into the message for the log; callers still branch on the code.
    CHECK(rejected.error().message.find("corrupts the SPI log") != std::string::npos);

    auto allowed =
        select_exact_release(*matrix, "linux-arm64", Version{0, 1, 1}, /*allow_deprecated=*/true);
    REQUIRE(allowed.has_value());
    CHECK(allowed->version == Version{0, 1, 1});
}
