// Known-answer tests for calculate_sha256() and verify_file().
//
// ADR 0001 makes these MANDATORY rather than optional: SHA-256 has two independent
// implementations (BCrypt on Windows, CommonCrypto on macOS) behind one signature, and these
// vectors are the only thing that checks the two paths agree. ADR 0009 makes test runs local
// rather than CI-gated, so they must be run by hand on BOTH platforms before a release.

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <string>

#include "../xdaqvc/update/artifact.h"

namespace fs = std::filesystem;
using namespace xvc::update;

namespace
{

// Writes content to a uniquely-named file under the system temp dir and removes it on scope
// exit, so a failing assertion cannot leave litter behind.
class TempFile
{
public:
    explicit TempFile(std::string_view content, std::string_view name)
        : _path(fs::temp_directory_path() / name)
    {
        std::ofstream out(_path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    ~TempFile()
    {
        std::error_code ec;
        fs::remove(_path, ec);
    }

    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    [[nodiscard]] const fs::path &path() const { return _path; }

private:
    fs::path _path;
};

}  // namespace

TEST_CASE("calculate_sha256 matches known vectors", "[update][sha256]")
{
    SECTION("empty input")
    {
        // The canonical SHA-256 of zero bytes. This also pins the loop's behaviour on a file
        // that yields no read iterations.
        TempFile file("", "xvc_sha256_empty.bin");
        auto digest = calculate_sha256(file.path());
        REQUIRE(digest.has_value());
        CHECK(*digest == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    SECTION("\"abc\"")
    {
        // FIPS 180-2 sample vector.
        TempFile file("abc", "xvc_sha256_abc.bin");
        auto digest = calculate_sha256(file.path());
        REQUIRE(digest.has_value());
        CHECK(*digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    SECTION("multi-block input crossing the read-chunk boundary")
    {
        // The read buffer is 64 KiB, so 200000 bytes forces several full reads plus a short
        // final read -- the exact path where a gcount()/return-value mix-up would show up.
        // Verified with sha256sum against a 200000-byte file of 'a'.
        const std::string content(200000, 'a');
        TempFile file(content, "xvc_sha256_multiblock.bin");
        auto digest = calculate_sha256(file.path());
        REQUIRE(digest.has_value());
        CHECK(*digest == "2287d207f24a941ff3b56c04c8a25ad56b63e3023207b3bb5b4ac0c9869d74be");
    }
}

TEST_CASE("calculate_sha256 reports a missing file rather than throwing", "[update][sha256]")
{
    auto digest = calculate_sha256(fs::temp_directory_path() / "xvc_sha256_does_not_exist.bin");
    REQUIRE_FALSE(digest.has_value());
    CHECK(digest.error().code == ErrorCode::FilesystemError);
}

TEST_CASE("verify_file distinguishes size from hash mismatch", "[update][sha256]")
{
    // ADR 0005: these must be distinct codes. A size mismatch is a truncated transfer and is
    // retryable; a hash mismatch on a correctly-sized file is corruption or tampering and
    // must never be retried. The old implementation retried both identically.
    const std::string content = "abc";
    const std::string correct_hash =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    TempFile file(content, "xvc_verify_abc.bin");

    SECTION("matching size and hash passes")
    {
        Artifact artifact{.name = "abc.bin", .size = 3, .sha256 = correct_hash};
        CHECK(verify_file(file.path(), artifact).has_value());
    }

    SECTION("uppercase hex in the manifest still verifies")
    {
        std::string upper = correct_hash;
        for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        Artifact artifact{.name = "abc.bin", .size = 3, .sha256 = upper};
        CHECK(verify_file(file.path(), artifact).has_value());
    }

    SECTION("wrong size yields SizeMismatch, and never reaches the hash")
    {
        Artifact artifact{.name = "abc.bin", .size = 999, .sha256 = correct_hash};
        auto result = verify_file(file.path(), artifact);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::SizeMismatch);
    }

    SECTION("correct size but wrong hash yields HashMismatch")
    {
        Artifact artifact{.name = "abc.bin", .size = 3, .sha256 = std::string(64, '0')};
        auto result = verify_file(file.path(), artifact);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::HashMismatch);
    }
}

TEST_CASE("Version::from_string accepts an optional v prefix", "[update][version]")
{
    // Phase 1.2: versions.json carries bare "0.1.2", but the v spelling is accepted at public
    // API boundaries. Widening this is additive -- the existing spellings must still work.
    CHECK(xvc::Version::from_string("0.3.2") == xvc::Version{0, 3, 2});
    CHECK(xvc::Version::from_string("v0.3.2") == xvc::Version{0, 3, 2});
    CHECK(xvc::Version::from_string("10.20.30") == xvc::Version{10, 20, 30});

    CHECK_FALSE(xvc::Version::from_string("0.3").has_value());
    CHECK_FALSE(xvc::Version::from_string("vv0.3.2").has_value());
    CHECK_FALSE(xvc::Version::from_string("0.3.2-rc1").has_value());
    CHECK_FALSE(xvc::Version::from_string("").has_value());
}
