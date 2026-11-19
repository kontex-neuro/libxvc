#include "device.h"
#include <algorithm>

#include <cpr/cpr.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <format>
#include <nlohmann/json.hpp>
#include <system_error>
#include <thread>
#include <utility>

#include "../server.h"

namespace fs = std::filesystem;

namespace xvc::update
{

namespace
{

constexpr int HTTP_OK = 200;

// Emit at most this often (ADR 0003); see PROGRESS_INTERVAL in artifact.cc for the rationale.
constexpr auto PROGRESS_INTERVAL = std::chrono::milliseconds{100};

std::string base_url(const DeviceEndpoint &device, int port)
{
    return std::format("http://{}:{}", device.host, port);
}

cpr::Header auth_header(const Handshake &handshake)
{
    return cpr::Header{{"Authorization", std::format("Bearer {}", handshake.token)}};
}

// Maps a cpr result to the transport/HTTP half of the error taxonomy. Response-body
// interpretation is the caller's business.
std::optional<Error> transport_error(const cpr::Response &response, std::string_view url)
{
    if (response.error.code != cpr::ErrorCode::OK) {
        return Error{
            ErrorCode::NetworkUnreachable,
            std::format("{} failed: {}", url, response.error.message)
        };
    }
    if (response.status_code != HTTP_OK) {
        return Error{
            .code = ErrorCode::HttpError,
            .message = std::format("{} returned HTTP {}: {}", url, response.status_code,
                                   response.text),
            .http_status = static_cast<int>(response.status_code)
        };
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Pure parsers
// ---------------------------------------------------------------------------------------

Result<Handshake> parse_handshake_response(std::string_view json_text)
{
    auto json = nlohmann::json::parse(json_text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "Handshake response is not a JSON object"}
        );
    }

    // The device signals refusal in-band with a 200, so status must be checked explicitly
    // rather than inferred from the HTTP code.
    const auto status = json.value("status", std::string{});
    if (status != "ready") {
        return std::unexpected(Error{
            ErrorCode::HandshakeRejected,
            std::format("Update service is not ready (status: '{}')", status)
        });
    }

    auto token_it = json.find("token");
    if (token_it == json.end() || !token_it->is_string() || token_it->get<std::string>().empty()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "Handshake response carries no token"}
        );
    }

    Handshake handshake;
    handshake.token = token_it->get<std::string>();

    // `expires` is a UTC epoch timestamp. Treat it as optional: a device that omits it still
    // yields a usable token, and the server is the authority on expiry regardless.
    if (auto it = json.find("expires"); it != json.end() && it->is_number_integer()) {
        handshake.expires =
            std::chrono::system_clock::from_time_t(static_cast<std::time_t>(it->get<std::int64_t>())
            );
    }

    return handshake;
}

Result<PreparedTransfer> parse_prepare_response(std::string_view json_text)
{
    auto json = nlohmann::json::parse(json_text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "prepare-transfer response is not a JSON object"}
        );
    }

    const auto status = json.value("status", std::string{});
    if (status != "ready") {
        return std::unexpected(Error{
            ErrorCode::TransferRejected,
            std::format(
                "Device refused the transfer (status: '{}'){}", status,
                json.contains("message")
                    ? std::format(": {}", json.value("message", std::string{}))
                    : std::string{}
            )
        });
    }

    auto id_it = json.find("transfer_id");
    if (id_it == json.end() || !id_it->is_string() || id_it->get<std::string>().empty()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "prepare-transfer response carries no transfer_id"}
        );
    }

    return PreparedTransfer{.transfer_id = id_it->get<std::string>()};
}

Result<LogEntry> parse_log_event(std::string_view json_text)
{
    auto json = nlohmann::json::parse(json_text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "Log event is not a JSON object"}
        );
    }

    // Every field is optional: a malformed log line must never abort an update that is
    // otherwise progressing. The old code defaulted these to "N/A"; empty is a better
    // default because it lets a UI decide how to render a missing field.
    return LogEntry{
        .timestamp = json.value("timestamp", std::string{}),
        .source = json.value("source", std::string{}),
        .level = json.value("level", std::string{}),
        .message = json.value("message", std::string{}),
    };
}

// ---------------------------------------------------------------------------------------
// Device operations
// ---------------------------------------------------------------------------------------

Result<Version> get_device_version(
    const DeviceEndpoint &device, std::chrono::milliseconds timeout
)
{
    const auto url = base_url(device,device.server_port) + "/build_version";
    auto response = cpr::Get(cpr::Url{url},cpr::Timeout{timeout},cpr::Redirect{false});
    if (auto error = transport_error(response,url)) return std::unexpected(*error);
    auto json = nlohmann::json::parse(response.text,nullptr,false);
    if (!json.is_object() || !json.contains("version") || !json["version"].is_string())
        return std::unexpected(Error{ErrorCode::MalformedResponse,"Missing application build version"});
    auto version = Version::from_string(json["version"].get<std::string>());
    if (!version) return std::unexpected(Error{ErrorCode::VersionNotFound,"Application build is unknown; explicit legacy migration is required"});
    return *version;
}

Result<Handshake> perform_handshake(const DeviceEndpoint &device, std::chrono::milliseconds timeout)
{
    const auto url = std::format("{}/handshake", base_url(device, device.update_port));

    auto response = cpr::Get(
        cpr::Url{url}, cpr::Timeout{timeout}, cpr::Header{{"User-Agent", "XVC-Client"}}
    );

    if (auto error = transport_error(response, url)) return std::unexpected(*error);
    return parse_handshake_response(response.text);
}

Result<std::string> transfer_filename(const Artifact &artifact, std::string_view environment)
{
    auto invalid = [](std::string message) -> Result<std::string> {
        return std::unexpected(Error{ErrorCode::MalformedResponse, std::move(message)});
    };
    if (environment != "dev" && environment != "release")
        return invalid("Invalid transfer environment");
    auto digest = [](std::string_view value) {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    if (artifact.role.empty() && artifact.package_metadata_sha256.empty()) {
        if (artifact.name.empty() || artifact.name.size() > 255 || artifact.name == "." || artifact.name == ".."
            || artifact.name.find_first_of("/\\:\"") != std::string::npos
            || std::any_of(artifact.name.begin(), artifact.name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            return invalid("Transfer filename must be a plain basename");
        return artifact.name;
    }
    if ((artifact.role != "application" && artifact.role != "updater-migration")
        || !digest(artifact.sha256) || !digest(artifact.package_metadata_sha256))
        return invalid("Device package transfer requires the PC-verified archive and metadata hashes");
    return std::format("tv1-{}-{}-{}-{}.tar.xz", artifact.role, environment,
                       artifact.sha256, artifact.package_metadata_sha256);
}

Result<PreparedTransfer> prepare_transfer(
    const DeviceEndpoint &device, const Handshake &handshake, const DownloadedArtifact &artifact,
    std::chrono::milliseconds timeout, std::string_view environment
)
{
    const auto url = std::format("{}/prepare-transfer", base_url(device, device.update_port));

    // The device validates these against what actually arrives, so they must describe the
    // artifact as verified rather than as advertised.
    auto filename = transfer_filename(artifact.artifact, environment);
    if (!filename) return std::unexpected(filename.error());
    nlohmann::json body{
        {"filename", *filename},
        {"file_hash", artifact.artifact.sha256},
        {"file_size", artifact.artifact.size},
    };
    if (environment == "dev") body["environment"] = "dev";
    else if (environment != "release") return std::unexpected(Error{ErrorCode::MalformedResponse,"Invalid transfer environment"});

    auto headers = auth_header(handshake);
    headers.emplace("Content-Type", "application/json");

    auto response =
        cpr::Post(cpr::Url{url}, headers, cpr::Body{body.dump()}, cpr::Timeout{timeout});

    if (auto error = transport_error(response, url)) return std::unexpected(*error);
    auto transfer = parse_prepare_response(response.text);
    if (transfer) transfer->upload_filename = *filename;
    return transfer;
}

Result<void> transfer_file_and_apply_update(
    const DeviceEndpoint &device, const Handshake &handshake, const PreparedTransfer &transfer,
    const fs::path &file, ProgressCallback progress, std::chrono::milliseconds timeout
)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError, std::format("Not a regular file: {}", file.string())
        });
    }

    const auto file_size = fs::file_size(file, ec);
    if (ec || file_size == 0) {
        return std::unexpected(Error{
            ErrorCode::FilesystemError,
            std::format("Cannot determine size of, or file is empty: {}", file.string())
        });
    }

    const auto url =
        std::format("{}/transfer/{}", base_url(device, device.update_port), transfer.transfer_id);

    const auto filename = transfer.upload_filename.empty() ? file.filename().string() : transfer.upload_filename;
    cpr::Multipart multipart{{cpr::Part{"file", cpr::File{file.string(), filename}}}};

    // Per-call state (ADR 0003). The ported implementation used a lambda-local
    // `static size_t last_progress`, which is shared across every invocation in the process:
    // two concurrent transfers corrupt each other's progress and a second sequential
    // transfer starts stale.
    struct ProgressState {
        std::chrono::steady_clock::time_point last_emit{};
        bool cancelled = false;
    } state;

    auto on_progress = [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t upload_total,
                           cpr::cpr_pf_arg_t upload_now, intptr_t) -> bool {
        if (!progress) return true;

        // cpr_pf_arg_t is curl_off_t -- signed. Guard before widening.
        const auto done =
            upload_now > 0 ? static_cast<std::uint64_t>(upload_now) : std::uint64_t{0};
        const auto total =
            upload_total > 0 ? static_cast<std::uint64_t>(upload_total) : file_size;

        const auto now = std::chrono::steady_clock::now();
        const bool finished = total > 0 && done >= total;
        if (!finished && now - state.last_emit < PROGRESS_INTERVAL) return true;
        state.last_emit = now;

        if (!progress(done, total)) {
            state.cancelled = true;
            return false;
        }
        return true;
    };

    auto response = cpr::Post(
        cpr::Url{url}, auth_header(handshake), multipart, cpr::ProgressCallback{on_progress},
        cpr::Timeout{timeout}
    );

    if (state.cancelled) {
        return std::unexpected(
            Error{ErrorCode::Cancelled, "Transfer cancelled by progress callback"}
        );
    }

    if (auto error = transport_error(response, url)) return std::unexpected(*error);

    auto json = nlohmann::json::parse(response.text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "transfer response is not a JSON object"}
        );
    }

    // The device accepted the upload but may still have failed to apply it -- a distinct
    // outcome from a rejected transfer, and the one that triggers firmware rollback.
    const auto status = json.value("status", std::string{});
    if (status != "success") {
        return std::unexpected(Error{
            ErrorCode::DeviceUpdateFailed,
            std::format(
                "Device failed to apply the update (status: '{}'){}", status,
                json.contains("message")
                    ? std::format(": {}", json.value("message", std::string{}))
                    : std::string{}
            )
        });
    }

    return {};
}

// ---------------------------------------------------------------------------------------
// Log streaming
// ---------------------------------------------------------------------------------------

struct LogStream::Impl {
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> stream_end_received{false};
    std::mutex completion_mutex;
    std::condition_variable completion;
    std::thread worker;

    ~Impl()
    {
        stop_requested.store(true);
        if (worker.joinable()) worker.join();
    }
};

LogStream::LogStream() noexcept = default;

LogStream::~LogStream() = default;  // Impl's destructor stops and joins

LogStream::LogStream(LogStream &&) noexcept = default;

LogStream &LogStream::operator=(LogStream &&other) noexcept
{
    if (this != &other) {
        stop();  // wind down whatever this handle currently owns before taking the new one
        _impl = std::move(other._impl);
    }
    return *this;
}

void LogStream::stop() noexcept
{
    if (!_impl) return;
    _impl->stop_requested.store(true);
    if (_impl->worker.joinable()) _impl->worker.join();
}

bool LogStream::drain(std::chrono::milliseconds timeout) noexcept
{
    if (!_impl) return false;
    {
        std::unique_lock lock(_impl->completion_mutex);
        _impl->completion.wait_for(lock, timeout, [&] { return !_impl->running.load(); });
    }
    stop();
    return _impl->stream_end_received.load();
}

bool LogStream::running() const noexcept
{
    return _impl && _impl->running.load();
}

LogStream stream_update_logs(
    const DeviceEndpoint &device, const Handshake &handshake, const PreparedTransfer &transfer,
    LogCallback on_log
)
{
    LogStream stream;
    stream._impl = std::make_unique<LogStream::Impl>();
    auto *impl = stream._impl.get();
    impl->running.store(true);

    const auto url = std::format(
        "{}/stream-logs/{}", base_url(device, device.update_port), transfer.transfer_id
    );
    const auto token = handshake.token;

    impl->worker = std::thread([impl, url, token, on_log = std::move(on_log)]() {
        // cpr 1.14.2 ships a spec-compliant SSE parser, so the hand-rolled buffer-and-split
        // on "\n\n" from the 1.10.5-era implementation is no longer needed. It handles
        // multi-line data fields and event IDs that the old splitter would have mangled.
        cpr::ServerSentEventCallback sse{
            [impl, on_log](cpr::ServerSentEvent &&event, intptr_t) -> bool {
                if (impl->stop_requested.load()) return false;
                if (event.data.empty()) return true;

                auto entry = parse_log_event(event.data);
                if (!entry) return true;  // a malformed line must not abort a live update

                if (is_stream_end(*entry)) {
                    impl->stream_end_received.store(true);
                    return false;
                }

                if (on_log) on_log(*entry);
                return !impl->stop_requested.load();
            }
        };

        // The stream can outlive an upload; drain() allows its queued outcome to
        // arrive. Progress polling also interrupts silent connections on stop().
        cpr::Get(
            cpr::Url{url},
            cpr::Header{
                {"Authorization", std::format("Bearer {}", token)},
                {"Accept", "text/event-stream"}
            },
            sse, cpr::Timeout{0}, cpr::ConnectTimeout{std::chrono::seconds{2}},
            cpr::ProgressCallback{[impl](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                       cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
                return !impl->stop_requested.load();
            }}
        );

        {
            std::lock_guard lock(impl->completion_mutex);
            impl->running.store(false);
        }
        impl->completion.notify_all();
    });

    return stream;
}

}  // namespace xvc::update
