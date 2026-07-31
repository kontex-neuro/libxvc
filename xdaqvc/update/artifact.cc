#include "artifact.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

// ADR 0001 / ADR 0010: the platform split for SHA-256 is confined to this file. Nothing else
// in the codebase sees a crypto #ifdef.
#if defined(_WIN32)
#include <windows.h>
// bcrypt.h must follow windows.h.
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#error "xvc::update::calculate_sha256 has no implementation for this platform (see ADR 0001)"
#endif

namespace fs = std::filesystem;

namespace xvc::update
{

namespace
{

constexpr std::size_t SHA256_DIGEST_BYTES = 32;

// 64 KiB: large enough that syscall overhead is irrelevant on a 46 MB tarball, small enough
// to stay off the stack pressure of a worker thread.
constexpr std::size_t READ_CHUNK_BYTES = 64 * 1024;

std::string to_hex(const unsigned char *bytes, std::size_t len)
{
    std::string hex;
    hex.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        hex += std::format("{:02x}", bytes[i]);
    }
    return hex;
}

#if defined(_WIN32)

// Minimal RAII over the two CNG handles so that every early return unwinds correctly. The
// ported OpenSSL code freed its context by hand on each error path, which is exactly the
// shape that leaks when someone adds a branch later.
class Sha256Hasher
{
public:
    ~Sha256Hasher()
    {
        if (_hash) BCryptDestroyHash(_hash);
        if (_algorithm) BCryptCloseAlgorithmProvider(_algorithm, 0);
    }

    Sha256Hasher(const Sha256Hasher &) = delete;
    Sha256Hasher &operator=(const Sha256Hasher &) = delete;

    Sha256Hasher() = default;

    // Returns an error string on failure, std::nullopt on success.
    std::optional<std::string> init()
    {
        auto status = BCryptOpenAlgorithmProvider(&_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(status)) {
            return std::format("BCryptOpenAlgorithmProvider failed: 0x{:08x}", status);
        }
        status = BCryptCreateHash(_algorithm, &_hash, nullptr, 0, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(status)) {
            return std::format("BCryptCreateHash failed: 0x{:08x}", status);
        }
        return std::nullopt;
    }

    std::optional<std::string> update(const char *data, std::size_t len)
    {
        auto status = BCryptHashData(
            _hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data)), static_cast<ULONG>(len), 0
        );
        if (!BCRYPT_SUCCESS(status)) {
            return std::format("BCryptHashData failed: 0x{:08x}", status);
        }
        return std::nullopt;
    }

    std::expected<std::string, std::string> finish()
    {
        std::array<unsigned char, SHA256_DIGEST_BYTES> digest{};
        auto status = BCryptFinishHash(_hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (!BCRYPT_SUCCESS(status)) {
            return std::unexpected(std::format("BCryptFinishHash failed: 0x{:08x}", status));
        }
        return to_hex(digest.data(), digest.size());
    }

private:
    BCRYPT_ALG_HANDLE _algorithm = nullptr;
    BCRYPT_HASH_HANDLE _hash = nullptr;
};

#elif defined(__APPLE__)

// CC_SHA256 is soft-deprecated since macOS 10.15 but still functions and underpins the
// platform's own TLS stack. ADR 0001 accepts that and requires the warning be suppressed
// HERE, at the call site, rather than project-wide.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

class Sha256Hasher
{
public:
    Sha256Hasher() = default;

    std::optional<std::string> init()
    {
        if (CC_SHA256_Init(&_ctx) != 1) {
            return std::string{"CC_SHA256_Init failed"};
        }
        return std::nullopt;
    }

    std::optional<std::string> update(const char *data, std::size_t len)
    {
        if (CC_SHA256_Update(&_ctx, data, static_cast<CC_LONG>(len)) != 1) {
            return std::string{"CC_SHA256_Update failed"};
        }
        return std::nullopt;
    }

    std::expected<std::string, std::string> finish()
    {
        std::array<unsigned char, SHA256_DIGEST_BYTES> digest{};
        if (CC_SHA256_Final(digest.data(), &_ctx) != 1) {
            return std::unexpected(std::string{"CC_SHA256_Final failed"});
        }
        return to_hex(digest.data(), digest.size());
    }

private:
    CC_SHA256_CTX _ctx{};
};

#pragma clang diagnostic pop

#endif

}  // namespace

Result<std::string> calculate_sha256(const fs::path &path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError, std::format("Not a regular file: {}", path.string())
        });
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError, std::format("Failed to open file: {}", path.string())
        });
    }

    Sha256Hasher hasher;
    if (auto err = hasher.init()) {
        return std::unexpected(Error{ErrorCode::FilesystemError, *err});
    }

    std::vector<char> buffer(READ_CHUNK_BYTES);
    // read() sets failbit on a short final read, so gcount() -- not the return value -- is
    // what drives the loop. An empty file yields zero iterations and hashes to the
    // well-known digest of the empty string, which the known-answer tests pin.
    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           file.gcount() > 0) {
        if (auto err = hasher.update(buffer.data(), static_cast<std::size_t>(file.gcount()))) {
            return std::unexpected(Error{ErrorCode::FilesystemError, *err});
        }
        if (file.eof()) break;
    }

    if (file.bad()) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError, std::format("Read error while hashing: {}", path.string())
        });
    }

    auto digest = hasher.finish();
    if (!digest) {
        return std::unexpected(Error{ErrorCode::FilesystemError, digest.error()});
    }
    return *digest;
}

Result<void> verify_file(const fs::path &path, const Artifact &artifact)
{
    std::error_code ec;
    const auto actual_size = fs::file_size(path, ec);
    if (ec) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError,
            std::format("Cannot stat {}: {}", path.string(), ec.message())
        });
    }

    // Size first: it is far cheaper than hashing, and a truncated transfer is the common
    // case. Reporting it as SizeMismatch is what lets the caller retry (ADR 0005).
    if (actual_size != artifact.size) {
        return std::unexpected(Error{
            ErrorCode::SizeMismatch,
            std::format(
                "Size mismatch for {}: expected {}, got {}", path.string(), artifact.size,
                actual_size
            )
        });
    }

    auto digest = calculate_sha256(path);
    if (!digest) return std::unexpected(digest.error());

    // Compare case-insensitively so a manifest carrying uppercase hex still verifies.
    const auto equal_ignoring_case = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };

    if (!equal_ignoring_case(*digest, artifact.sha256)) {
        // A correctly-sized file with the wrong hash is corruption or tampering, not a
        // transport blip. The caller must not retry this (ADR 0005).
        return std::unexpected(Error{
            ErrorCode::HashMismatch,
            std::format(
                "Hash mismatch for {}: expected {}, got {}", path.string(), artifact.sha256, *digest
            )
        });
    }

    return {};
}

}  // namespace xvc::update
