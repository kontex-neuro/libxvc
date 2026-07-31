#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../common.h"

namespace xvc::update
{

// ADR 0002: there is exactly one Version type in this library. `xvc::update::Version` is an
// alias for `xvc::Version` so that spec-conformant spellings compile, but no second type
// exists and no conversion is ever needed at a boundary.
using Version = xvc::Version;

// ADR 0004: `platform` in versions.json identifies the *device being updated* -- the
// Jetson-based device server -- never the host running libxvc. libxvc ships on Windows and
// macOS, and neither ever appears as a platform value, so deriving this from `#ifdef _WIN32`
// would be actively wrong. Every connected device is arm64.
//
// This constant is the single place to change if a second device type ever appears.
inline constexpr std::string_view DEVICE_PLATFORM = "linux-arm64";

// ---------------------------------------------------------------------------------------
// Errors (ADR 0005)
// ---------------------------------------------------------------------------------------

// Callers branch on `code`, NEVER on `message`. The message exists so that a customer's log
// file supports root-cause analysis; its wording is not part of the API and will change.
//
// This taxonomy is provisional: it was derived from failure modes visible in the ported
// client code rather than from the update service's documented behaviour. Extend it when a
// real failure cannot be expressed.
enum class ErrorCode {
    // transport / protocol
    NetworkUnreachable,  // cpr transport error: DNS, TLS, connection refused, timeout
    HttpError,           // reached the server, non-2xx status; sets Error::http_status

    // metadata
    MalformedResponse,  // not valid JSON, or a required field missing / wrong type

    // selection
    PlatformNotFound,   // no entry for this platform -- distinct from "up to date" (ADR 0004)
    VersionNotFound,    // requested version or series not present in the matrix
    ReleaseDeprecated,  // found, but status == "deprecated"
    DowngradeRejected,  // target older than device and the caller did not allow it

    // artifact
    SizeMismatch,     // size != manifest -- usually a truncated transfer; retrying is correct
    HashMismatch,     // sha256 != manifest -- security-relevant, NEVER auto-retry
    FilesystemError,  // cannot write temp file, disk full, rename failed
    Cancelled,        // ProgressCallback returned false

    // device transfer
    HandshakeRejected,  // update service refused, or token expired
    TransferRejected,   // prepare-transfer / transfer refused
    DeviceUpdateFailed, // device accepted the package but failed to apply it
};

struct Error {
    ErrorCode code;
    std::string message;  // human-readable detail, for logs -- never branch on it
    int http_status = 0;  // populated when code == ErrorCode::HttpError
};

template <typename T>
using Result = std::expected<T, Error>;

// ---------------------------------------------------------------------------------------
// Cloud metadata
// ---------------------------------------------------------------------------------------

struct Artifact {
    std::string name;
    std::uint64_t size = 0;
    std::string sha256;
};

// One entry of `versions[]`. A `manifest.json` is exactly this minus `download_base`, which
// is why that field is optional and why one parser serves both (ADR 0008). It is always
// engaged when the entry came from a matrix.
struct VersionEntry {
    Version version;
    std::string platform;
    std::string environment;  // "release" or "dev"
    std::string status;       // "stable" or "deprecated"
    std::string released_at;
    std::string changelog;
    std::string deprecation_reason;  // absent from every real entry; defaults to empty
    std::optional<std::string> download_base;
    std::vector<Artifact> files;
};

struct VersionMatrix {
    std::string updated_at;
    // Parsed because it is part of the wire format, but ADR 0007 policy reads nothing from
    // it: selection filters `versions[]` directly. Kept exposed rather than dropped.
    std::map<std::string, Version> latest_release;
    std::vector<VersionEntry> versions;
};

// ---------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------

struct DeviceEndpoint {
    std::string host;
    int server_port = 8000;  // the device server: /api_version
    int update_port = 8001;  // the update service: /handshake, /prepare-transfer, ...
};

struct DownloadedArtifact {
    Artifact artifact;
    std::filesystem::path path;
    std::string url;  // empty for artifacts supplied via use_local_artifact()
};

struct LogEntry {
    std::string timestamp;
    std::string source;
    std::string level;
    std::string message;
};

// ---------------------------------------------------------------------------------------
// Callbacks (ADR 0003)
// ---------------------------------------------------------------------------------------

// THREADING CONTRACT -- load-bearing and not enforceable by the compiler.
//
// Both callbacks are invoked on a WORKER THREAD (cpr's transfer thread for progress, the
// log-stream thread for logs). They MUST NOT touch UI state directly. A Qt/QML caller
// bridges with QMetaObject::invokeMethod(..., Qt::QueuedConnection) or a queued signal.
// libxvc is Qt-free and has no event loop of its own to marshal onto.
//
// Progress emissions are throttled by the library to roughly one per 100 ms, plus an
// unconditional final emission so the last update is never lost.
//
// Returning `false` CANCELS the transfer; this is the cancellation channel, and the deletion
// of incomplete temp files on cancellation depends on it. Any flag the callback reads is
// written from another thread and so must be std::atomic<bool>.
//
// `total` may be 0 when the server sends no Content-Length -- do not divide by it (QML should
// bind ProgressBar.indeterminate to total === 0).
//
// Because delivery is queued, the final progress callback may arrive AFTER the download
// function has returned. Drive "finished" state from the return value, not the last tick.
using ProgressCallback = std::function<bool(std::uint64_t done, std::uint64_t total)>;

// Invoked on the log-stream worker thread; same contract as above. Do not assume spdlog.
using LogCallback = std::function<void(const LogEntry &)>;

}  // namespace xvc::update
