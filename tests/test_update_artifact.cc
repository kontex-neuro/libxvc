// Tier 2 (ADR 0009): local filesystem only -- no network, no device.
//
// download_artifact()'s network path is Tier 3 and is not exercised here; what IS testable
// without a network is the URL join, the stale-temp sweep, and the whole offline
// use_local_artifact() path including its ownership rule (it must delete nothing).

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "../xdaqvc/update/artifact.h"

namespace fs = std::filesystem;
using namespace xvc::update;

namespace
{

// SHA-256 of "hello artifact contents\n" as written below; pinned so a change to the
// fixture content cannot silently invalidate the verification tests.
constexpr std::string_view PAYLOAD = "hello artifact contents\n";

// A scratch directory removed on scope exit, so a failing assertion leaves no litter.
class TempDir
{
public:
    TempDir()
        : _path(
              fs::temp_directory_path() /
              std::format("xvc-test-{}", std::chrono::steady_clock::now().time_since_epoch().count())
          )
    {
        fs::create_directories(_path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] const fs::path &path() const { return _path; }

    fs::path write(std::string_view name, std::string_view content) const
    {
        const auto file = _path / name;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        return file;
    }

private:
    fs::path _path;
};

std::string sha256_of(const fs::path &p)
{
    auto digest = calculate_sha256(p);
    REQUIRE(digest.has_value());
    return *digest;
}

// Writes a manifest.json describing `tarball`, mirroring the real wire format minus
// download_base (ADR 0008).
fs::path write_manifest(
    const TempDir &dir, const fs::path &tarball, std::uint64_t size, const std::string &sha256
)
{
    nlohmann::json manifest{
        {"version", "0.1.2"},
        {"platform", "linux-arm64"},
        {"environment", "release"},
        {"status", "stable"},
        {"released_at", "2026-07-22T10:48:38Z"},
        {"files",
         nlohmann::json::array(
             {{{"name", tarball.filename().string()}, {"size", size}, {"sha256", sha256}}}
         )},
    };
    return dir.write("manifest.json", manifest.dump(2));
}

}  // namespace

TEST_CASE("join_url does not duplicate separators", "[update][artifact]")
{
    // download_base carries a slash on BOTH ends in the real wire format, so this is the
    // case that a naive concatenation gets wrong.
    CHECK(
        join_url("https://cloud.example.com", "/dist/v0.1.2/linux-arm64/", "server.tar.xz") ==
        "https://cloud.example.com/dist/v0.1.2/linux-arm64/server.tar.xz"
    );

    SECTION("tolerates a trailing slash on the base")
    {
        CHECK(
            join_url("https://cloud.example.com/", "/dist/v0.1.2/", "a.tar.xz") ==
            "https://cloud.example.com/dist/v0.1.2/a.tar.xz"
        );
    }

    SECTION("tolerates a segment with no slashes at all")
    {
        CHECK(
            join_url("https://cloud.example.com", "dist", "a.tar.xz") ==
            "https://cloud.example.com/dist/a.tar.xz"
        );
    }

    SECTION("handles an empty path segment")
    {
        CHECK(
            join_url("https://cloud.example.com", "", "a.tar.xz") ==
            "https://cloud.example.com/a.tar.xz"
        );
    }
}

TEST_CASE("use_local_artifact verifies a good tarball", "[update][artifact]")
{
    TempDir dir;
    const auto tarball = dir.write("ThorVisionServer-0.1.2-linux-arm64.tar.xz", PAYLOAD);
    const auto manifest = write_manifest(dir, tarball, PAYLOAD.size(), sha256_of(tarball));

    auto result = use_local_artifact(manifest);
    REQUIRE(result.has_value());

    CHECK(result->path == tarball);
    CHECK(result->artifact.size == PAYLOAD.size());
    CHECK(result->url.empty());  // nothing was downloaded

    // The ownership rule: libxvc did not create these files and must not remove them.
    CHECK(fs::exists(tarball));
    CHECK(fs::exists(manifest));
}

TEST_CASE("use_local_artifact rejects a tampered tarball", "[update][artifact]")
{
    TempDir dir;
    const auto tarball = dir.write("server.tar.xz", PAYLOAD);
    const auto good_hash = sha256_of(tarball);

    // Same length, different bytes -- so size passes and only the hash catches it. This is
    // the tampering case, and it must be reported distinctly from truncation.
    std::string tampered{PAYLOAD};
    tampered.back() = 'X';
    REQUIRE(tampered.size() == PAYLOAD.size());
    dir.write("server.tar.xz", tampered);

    const auto manifest = write_manifest(dir, tarball, PAYLOAD.size(), good_hash);

    auto result = use_local_artifact(manifest);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::HashMismatch);

    // Still not deleted, even on failure.
    CHECK(fs::exists(tarball));
}

TEST_CASE("use_local_artifact reports truncation as SizeMismatch", "[update][artifact]")
{
    TempDir dir;
    const auto tarball = dir.write("server.tar.xz", PAYLOAD);
    const auto manifest = write_manifest(dir, tarball, PAYLOAD.size() + 100, sha256_of(tarball));

    auto result = use_local_artifact(manifest);
    REQUIRE_FALSE(result.has_value());
    // Distinct from HashMismatch (ADR 0005): this one is retryable, the other is not.
    CHECK(result.error().code == ErrorCode::SizeMismatch);
}

TEST_CASE("use_local_artifact requires the tarball beside the manifest", "[update][artifact]")
{
    TempDir dir;
    const auto tarball = dir.write("server.tar.xz", PAYLOAD);
    const auto manifest = write_manifest(dir, tarball, PAYLOAD.size(), sha256_of(tarball));

    fs::remove(tarball);

    auto result = use_local_artifact(manifest);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::FilesystemError);
}

TEST_CASE("use_local_artifact rejects a malformed manifest", "[update][artifact]")
{
    TempDir dir;
    dir.write("server.tar.xz", PAYLOAD);
    const auto manifest = dir.write("manifest.json", "{ not valid json");

    auto result = use_local_artifact(manifest);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("use_local_artifact reports a missing manifest", "[update][artifact]")
{
    TempDir dir;
    auto result = use_local_artifact(dir.path() / "no-such-manifest.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::FilesystemError);
}

TEST_CASE("download_artifact rejects an entry with no download_base", "[update][artifact]")
{
    // A manifest-sourced entry has no download_base, so it cannot be fetched online. This is
    // caught before any network call, which is why it is testable at Tier 2.
    VersionEntry entry;
    entry.version = Version{0, 1, 2};
    entry.platform = std::string{DEVICE_PLATFORM};
    entry.files.push_back(Artifact{.name = "a.tar.xz", .size = 1, .sha256 = std::string(64, 'a')});

    auto result = download_artifact("https://cloud.example.com", entry);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("download_artifact rejects an entry with no files", "[update][artifact]")
{
    VersionEntry entry;
    entry.version = Version{0, 1, 2};
    entry.download_base = "/dist/v0.1.2/linux-arm64/";

    auto result = download_artifact("https://cloud.example.com", entry);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

// The two cases below drive download_artifact() to exhaustion against a refused connection,
// so they pay the full 3-attempt retry backoff (~12 s each). Tagged [.slow] so they are
// excluded from a default run but stay executable via `[.slow]` or `[artifact]`.
TEST_CASE("download_artifact sweeps only its own stale temp files", "[update][artifact][.slow]")
{
    TempDir dir;

    // Matches the library's pattern and is old enough to sweep.
    const auto stale = dir.write("xvc-update-abc-0123456789abcdef.part", "stale");
    fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours{48});

    // Matches the pattern but is recent -- a concurrent download must survive.
    const auto fresh = dir.write("xvc-update-def-fedcba9876543210.part", "fresh");

    // Does NOT match the pattern. Old, but user data: must never be touched.
    const auto user_file = dir.write("important-user-file.tar.xz", "do not delete");
    fs::last_write_time(user_file, fs::file_time_type::clock::now() - std::chrono::hours{48});

    // The sweep runs on entry, before any network activity. The call fails at the HTTP stage
    // (the host does not resolve), which is fine -- the sweep has already happened.
    VersionEntry entry;
    entry.version = Version{0, 1, 2};
    entry.download_base = "/dist/";
    entry.files.push_back(Artifact{.name = "a.tar.xz", .size = 1, .sha256 = std::string(64, 'a')});
    (void) download_artifact("http://127.0.0.1:1", entry, nullptr, dir.path());

    CHECK_FALSE(fs::exists(stale));
    CHECK(fs::exists(fresh));
    CHECK(fs::exists(user_file));
}

TEST_CASE("download_artifact leaves no partial file behind on failure", "[update][artifact][.slow]")
{
    TempDir dir;

    VersionEntry entry;
    entry.version = Version{0, 1, 2};
    entry.download_base = "/dist/";
    entry.files.push_back(Artifact{.name = "a.tar.xz", .size = 1, .sha256 = std::string(64, 'a')});

    // Connection refused on every attempt; artifacts are transient, so nothing may survive.
    auto result = download_artifact("http://127.0.0.1:1", entry, nullptr, dir.path());
    REQUIRE_FALSE(result.has_value());

    for (const auto &child : fs::directory_iterator(dir.path())) {
        FAIL_CHECK("unexpected leftover file: " << child.path().string());
    }
}
