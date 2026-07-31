#include "artifact.h"

#include <cpr/cpr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <iterator>
#include <format>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>
#include <vector>

#include "manifest.h"

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

namespace
{

constexpr int HTTP_OK = 200;
constexpr int MAX_ATTEMPTS = 3;

// Emit at most this often, so a fast download cannot flood a GUI event loop with queued
// cross-thread invocations (ADR 0003).
constexpr auto PROGRESS_INTERVAL = std::chrono::milliseconds{100};

// Abort a transfer that averages under this rate for this long. Preferred over a wall-clock
// Timeout, which would kill a healthy but slow 46 MB download.
constexpr std::int32_t MIN_BYTES_PER_SEC = 1024;
constexpr auto LOW_SPEED_WINDOW = std::chrono::seconds{60};

// Only files matching this prefix and suffix are ever swept, and only past this age.
constexpr std::string_view TEMP_PREFIX = "xvc-update-";
constexpr std::string_view TEMP_SUFFIX = ".part";
constexpr auto STALE_AFTER = std::chrono::hours{24};

// Per-call progress state. Deliberately a struct captured by the lambda rather than
// lambda-local `static` storage: a `static` here is shared across every invocation in the
// process, so two concurrent transfers corrupt each other's throttle state and a second
// sequential transfer starts stale. That was a real bug in the ported implementation.
struct ProgressState {
    std::chrono::steady_clock::time_point last_emit{};
    bool cancelled = false;
};

// Removes only files this library created, matched by its own naming pattern and older than
// STALE_AFTER. Never touches user data: a file must match BOTH affixes to be considered.
void sweep_stale_temp_files(const fs::path &dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;

    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;

        const auto name = it->path().filename().string();
        if (!name.starts_with(TEMP_PREFIX) || !name.ends_with(TEMP_SUFFIX)) continue;

        std::error_code time_ec;
        const auto written = fs::last_write_time(it->path(), time_ec);
        if (time_ec) continue;
        if (now - written < STALE_AFTER) continue;

        std::error_code remove_ec;
        fs::remove(it->path(), remove_ec);  // best effort; a locked orphan is swept next run
    }
}

fs::path make_temp_path(const fs::path &dir)
{
    // Unique per attempt so concurrent downloads, in this process or another, cannot collide
    // on the same partial file.
    static std::atomic<std::uint64_t> counter{0};
    std::random_device rd;
    const auto token = (static_cast<std::uint64_t>(rd()) << 32) ^ counter.fetch_add(1);

    return dir / std::format(
                     "{}{}-{:016x}{}", TEMP_PREFIX,
                     static_cast<std::uint64_t>(
                         std::hash<std::thread::id>{}(std::this_thread::get_id())
                     ),
                     token, TEMP_SUFFIX
                 );
}

}  // namespace

std::string join_url(std::string_view base, std::string_view path, std::string_view filename)
{
    auto trim_trailing = [](std::string_view s) {
        while (!s.empty() && s.back() == '/') s.remove_suffix(1);
        return s;
    };
    auto trim_both = [&trim_trailing](std::string_view s) {
        while (!s.empty() && s.front() == '/') s.remove_prefix(1);
        return trim_trailing(s);
    };

    const auto clean_base = trim_trailing(base);
    const auto clean_path = trim_both(path);
    const auto clean_file = trim_both(filename);

    if (clean_path.empty()) return std::format("{}/{}", clean_base, clean_file);
    return std::format("{}/{}/{}", clean_base, clean_path, clean_file);
}

Result<DownloadedArtifact> download_artifact(
    const std::string &cloud_base_url, const VersionEntry &entry, ProgressCallback progress,
    const std::optional<fs::path> &download_dir
)
{
    if (entry.files.empty()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "Version entry carries no files[0] artifact"}
        );
    }
    if (!entry.download_base) {
        // A manifest entry reached the online path. Its artifact is already local, so the
        // caller wanted use_local_artifact().
        return std::unexpected(Error{
            ErrorCode::MalformedResponse,
            "Version entry has no download_base; use use_local_artifact() for local artifacts"
        });
    }

    const auto &artifact = entry.files.front();
    const auto url = join_url(cloud_base_url, *entry.download_base, artifact.name);

    const auto dir = download_dir.value_or(fs::temp_directory_path() / "xvc-update");

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir)) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError,
            std::format("Cannot create download directory {}: {}", dir.string(), ec.message())
        });
    }

    sweep_stale_temp_files(dir);

    const auto final_path = dir / artifact.name;
    Error last_error{ErrorCode::NetworkUnreachable, "download not attempted"};

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        const auto temp_path = make_temp_path(dir);

        // Removes the partial file on every exit from this iteration -- success, failure,
        // cancellation, or an exception. Artifacts are transient (ADR 0008).
        struct TempGuard {
            fs::path path;
            bool armed = true;
            ~TempGuard()
            {
                if (!armed) return;
                std::error_code ec;
                fs::remove(path, ec);
            }
        } guard{temp_path};

        {
            // Truncate-create; partial downloads are never resumed, because resumption is a
            // subtle corruption source the hash would only catch after wasting the transfer.
            std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
            if (!file) {
                return std::unexpected(Error{
                    ErrorCode::FilesystemError,
                    std::format("Cannot open temp file {}", temp_path.string())
                });
            }

            ProgressState state;
            auto on_progress = [&](cpr::cpr_pf_arg_t download_total, cpr::cpr_pf_arg_t download_now,
                                   cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
                if (!progress) return true;

                // cpr_pf_arg_t is curl_off_t -- SIGNED. Guard before widening to uint64.
                const auto done =
                    download_now > 0 ? static_cast<std::uint64_t>(download_now) : std::uint64_t{0};
                const auto total = download_total > 0
                                       ? static_cast<std::uint64_t>(download_total)
                                       : std::uint64_t{0};

                const auto now = std::chrono::steady_clock::now();
                const bool finished = total > 0 && done >= total;

                // Throttle, but never drop the final tick.
                if (!finished && now - state.last_emit < PROGRESS_INTERVAL) return true;
                state.last_emit = now;

                if (!progress(done, total)) {
                    state.cancelled = true;
                    return false;  // aborts the transfer
                }
                return true;
            };

            auto response = cpr::Download(
                file, cpr::Url{url}, cpr::ProgressCallback{on_progress},
                cpr::LowSpeed{MIN_BYTES_PER_SEC, LOW_SPEED_WINDOW}
            );

            file.close();  // must be closed before verify/rename, especially on Windows

            if (state.cancelled) {
                return std::unexpected(
                    Error{ErrorCode::Cancelled, "Download cancelled by progress callback"}
                );
            }

            if (response.error.code != cpr::ErrorCode::OK) {
                last_error = Error{
                    ErrorCode::NetworkUnreachable,
                    std::format("GET {} failed: {}", url, response.error.message)
                };
                std::this_thread::sleep_for(std::chrono::seconds{attempt});
                continue;
            }

            if (response.status_code != HTTP_OK) {
                last_error = Error{
                    .code = ErrorCode::HttpError,
                    .message = std::format("GET {} returned HTTP {}", url, response.status_code),
                    .http_status = static_cast<int>(response.status_code)
                };
                std::this_thread::sleep_for(std::chrono::seconds{attempt});
                continue;
            }
        }

        auto verified = verify_file(temp_path, artifact);
        if (!verified) {
            // A hash mismatch on a correctly-sized file is corruption or tampering, not a
            // transport blip -- retrying it wastes time and treats a security signal as
            // noise. Only size mismatches (usually truncation) are worth another attempt.
            if (verified.error().code == ErrorCode::HashMismatch) {
                return std::unexpected(verified.error());
            }
            last_error = verified.error();
            continue;
        }

        std::error_code rename_ec;
        fs::remove(final_path, rename_ec);  // a previous run's verified file may linger
        fs::rename(temp_path, final_path, rename_ec);
        if (rename_ec) {
            return std::unexpected(Error{
                ErrorCode::FilesystemError,
                std::format(
                    "Cannot rename {} to {}: {}", temp_path.string(), final_path.string(),
                    rename_ec.message()
                )
            });
        }
        guard.armed = false;  // the file lives on under its real name now

        return DownloadedArtifact{.artifact = artifact, .path = final_path, .url = url};
    }

    return std::unexpected(last_error);
}

Result<DownloadedArtifact> use_local_artifact(const fs::path &manifest_json)
{
    std::ifstream in(manifest_json, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError,
            std::format("Cannot open manifest {}", manifest_json.string())
        });
    }

    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    auto entry = parse_manifest_json(text);
    if (!entry) return std::unexpected(entry.error());

    if (entry->files.empty()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "Manifest carries no files[0] artifact"}
        );
    }

    const auto &artifact = entry->files.front();

    // The tarball sits beside its manifest. Take only the filename, so a manifest cannot
    // point at an arbitrary path elsewhere on the filesystem.
    const auto parent = manifest_json.has_parent_path() ? manifest_json.parent_path() : fs::path{"."};
    const auto tarball = parent / fs::path{artifact.name}.filename();

    std::error_code ec;
    if (!fs::is_regular_file(tarball, ec)) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError,
            std::format(
                "Manifest names '{}' but no such file sits beside it at {}", artifact.name,
                tarball.string()
            )
        });
    }

    // Verifying against the manifest is what makes the offline path trustworthy; there is
    // deliberately no skip-verification escape hatch.
    auto verified = verify_file(tarball, artifact);
    if (!verified) return std::unexpected(verified.error());

    // Note the absence of any deletion, here or on failure: a developer supplied these files
    // and libxvc must never remove what it did not create (ADR 0008).
    return DownloadedArtifact{.artifact = artifact, .path = tarball, .url = {}};
}

}  // namespace xvc::update
