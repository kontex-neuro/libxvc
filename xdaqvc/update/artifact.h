#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "types.h"

namespace xvc::update
{

// Joins a cloud base URL, a download_base path segment, and a filename without duplicating
// separators. download_base carries a slash on BOTH ends (observed:
// "/dist/v0.1.2/linux-arm64/"), so a naive concatenation produces doubled slashes.
//
// Exposed rather than kept internal purely so it can be unit-tested without a network.
[[nodiscard]] std::string join_url(
    std::string_view base, std::string_view path, std::string_view filename
);

// Streams the file in chunks; safe for the ~46 MB release tarballs (never loads it whole).
// Returns the digest as lowercase hex.
//
// ADR 0001: implemented against platform crypto -- BCrypt (CNG) on Windows, CommonCrypto on
// macOS -- so that libxvc ships no bundled crypto library and OS vendors own CVE patching.
// The two code paths are the reason the known-answer tests in test_update_sha256.cc are
// mandatory rather than optional; they are the only thing checking that the paths agree.
Result<std::string> calculate_sha256(const std::filesystem::path &path);

// Checks size first, then hash, and reports them as DISTINCT error codes.
//
// The distinction is load-bearing (ADR 0005): a size mismatch is almost always a truncated
// transfer and retrying it is correct, whereas a hash mismatch on a correctly-sized file
// indicates corruption or tampering. Callers must never retry HashMismatch -- the old
// implementation retried both identically three times, which wasted time and treated a
// security signal as noise.
Result<void> verify_file(const std::filesystem::path &path, const Artifact &artifact);

// Downloads the release tarball described by `entry` and verifies it before returning.
//
// Takes the whole VersionEntry rather than a separate Artifact parameter as the spec shows:
// entry.files[0] IS the artifact, and accepting both would let them disagree.
//
// `download_dir` overrides the default of temp_directory_path()/"xvc-update" (ADR 0008);
// tests and the CLI use it. `progress` is invoked on cpr's transfer thread -- see the
// threading contract on ProgressCallback in types.h -- and returning false cancels,
// yielding ErrorCode::Cancelled.
//
// Lifecycle (ADR 0008): each attempt writes a uniquely named temp file, truncate-created and
// NEVER resumed, renamed to its real name only after verification. The temp file is removed
// on success, failure, and cancellation. Artifacts are transient -- there is no cache, so
// the caller owns the returned file and should delete it once transferred.
//
// Retries transport failures and SizeMismatch only. A HashMismatch is never retried: on a
// correctly-sized file it indicates corruption or tampering rather than a transport blip.
Result<DownloadedArtifact> download_artifact(
    const std::string &cloud_base_url, const VersionEntry &entry,
    ProgressCallback progress = nullptr,
    const std::optional<std::filesystem::path> &download_dir = std::nullopt
);

// Offline / developer path. No network.
//
// Parses `manifest_json` (a versions.json entry minus download_base), locates the tarball
// named by files[0].name in the SAME directory, and verifies it against
// files[0].{size,sha256}. The manifest is the trust anchor: without it, offline verification
// would need either a fetched matrix or a pasted hash.
//
// DELETES NOTHING -- libxvc did not create these files. This is a separate function rather
// than a flag on download_artifact() so that the ownership rule lives in the type system
// rather than in a bool someone might read wrongly (ADR 0008).
Result<DownloadedArtifact> use_local_artifact(const std::filesystem::path &manifest_json);

}  // namespace xvc::update
