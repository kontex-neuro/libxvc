#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "types.h"

namespace xvc::update
{

struct Handshake {
    std::string token;
    std::chrono::system_clock::time_point expires;
};

struct PreparedTransfer {
    std::string transfer_id;
};

// ---------------------------------------------------------------------------------------
// Pure response parsers
//
// Split out from the HTTP calls deliberately. Every function below this section needs a
// device to exercise (Tier 3, ADR 0009), but the parsing they depend on is pure and gets
// Tier 1 coverage with no hardware. That keeps the device protocol's decoding rules
// verifiable on a developer checkout instead of only on the bench.
// ---------------------------------------------------------------------------------------

// Body of GET /handshake. Success requires status == "ready" plus a token; `expires` is a
// UTC epoch timestamp.
Result<Handshake> parse_handshake_response(std::string_view json_text);

// Body of POST /prepare-transfer. Success is status == "ready" with a transfer_id.
Result<PreparedTransfer> parse_prepare_response(std::string_view json_text);

// One SSE event's `data` payload from GET /stream-logs/<id>.
Result<LogEntry> parse_log_event(std::string_view json_text);

// The device signals end-of-stream with this sentinel as a log message rather than by
// closing the connection.
inline constexpr std::string_view STREAM_END_SENTINEL = "__STREAM_END__";

[[nodiscard]] inline bool is_stream_end(const LogEntry &entry)
{
    return entry.message == STREAM_END_SENTINEL;
}

// ---------------------------------------------------------------------------------------
// Device operations (Tier 3 -- each needs a reachable device)
// ---------------------------------------------------------------------------------------

// GET http://<host>:<server_port>/api_version
//
// Delegates to xvc::Server::api_version() rather than issuing its own request, so exactly
// one HTTP call site exists for that endpoint (ADR 0010).
Result<Version> get_device_version(
    const DeviceEndpoint &device, std::chrono::milliseconds timeout = std::chrono::seconds{2}
);

// GET http://<host>:<update_port>/handshake
Result<Handshake> perform_handshake(
    const DeviceEndpoint &device, std::chrono::milliseconds timeout = std::chrono::seconds{5}
);

// POST http://<host>:<update_port>/prepare-transfer
//
// Sends filename / file_hash / file_size so the device can validate the upload before it
// begins. Returns PreparedTransfer rather than using the old out-parameter.
Result<PreparedTransfer> prepare_transfer(
    const DeviceEndpoint &device, const Handshake &handshake, const DownloadedArtifact &artifact,
    std::chrono::milliseconds timeout = std::chrono::seconds{10}
);

// POST http://<host>:<update_port>/transfer/<transfer_id>
//
// Uploads the tarball as multipart field "file" with Bearer auth, and the device applies the
// update. Only the tarball is sent -- manifest.json is client-side metadata (ADR 0008), so
// no device-server protocol change is implied.
//
// `progress` follows the ProgressCallback contract in types.h: worker thread, throttled,
// returning false cancels. Progress state is per-call, never static.
Result<void> transfer_file_and_apply_update(
    const DeviceEndpoint &device, const Handshake &handshake, const PreparedTransfer &transfer,
    const std::filesystem::path &file, ProgressCallback progress = nullptr,
    std::chrono::milliseconds timeout = std::chrono::seconds{90}
);

// RAII handle for the background log stream.
//
// The spec returns a bare std::thread, which calls std::terminate if destroyed while
// joinable -- the old CLI happened to keep it alive, but a GUI that cancels an update will
// not. This handle stops and joins in its destructor and offers an explicit stop()
// (ADR 0003).
//
// Move-only: two owners joining the same thread is not meaningful.
class LogStream
{
public:
    LogStream() noexcept;
    ~LogStream();

    LogStream(LogStream &&) noexcept;
    LogStream &operator=(LogStream &&) noexcept;

    LogStream(const LogStream &) = delete;
    LogStream &operator=(const LogStream &) = delete;

    // Idempotent; safe to call from another thread and safe to call after the stream has
    // already ended on its own.
    void stop() noexcept;

    // False once the server has sent __STREAM_END__ or the connection has closed.
    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;

    friend LogStream stream_update_logs(
        const DeviceEndpoint &, const Handshake &, const PreparedTransfer &, LogCallback
    );
};

// GET http://<host>:<update_port>/stream-logs/<transfer_id>
//
// Streams device-side update logs while the update is applied, delivering each entry through
// `on_log` on the stream's worker thread. Terminates on the __STREAM_END__ sentinel.
//
// Log delivery goes through the callback rather than straight to spdlog, so the GUI can
// render the stream; the library does not assume a single sink.
[[nodiscard]] LogStream stream_update_logs(
    const DeviceEndpoint &device, const Handshake &handshake, const PreparedTransfer &transfer,
    LogCallback on_log
);

}  // namespace xvc::update
