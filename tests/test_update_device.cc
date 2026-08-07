// Phase 5 tests, split by what they require (ADR 0009).
//
// Tier 1 -- the pure response parsers. These run on any checkout with no hardware. The
// parsers were split out of the HTTP functions precisely so that the device protocol's
// decoding rules stay verifiable without a bench device.
//
// Tier 3 -- the handshake/prepare/transfer/log-stream sequence, tagged [.integration] so it
// never runs by default. Point it at a device with XVC_TEST_DEVICE_HOST.
//
// NOTE: the [.integration] cases below have NOT been run. They encode the protocol sequence
// so it is repeatable on the bench, but nothing here has been executed against real
// hardware. Treat them as unverified until someone runs them.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

#include "../xdaqvc/update/artifact.h"
#include "../xdaqvc/update/device.h"

using namespace xvc::update;

// ---------------------------------------------------------------------------------------
// Tier 1 -- pure parsers, no device required
// ---------------------------------------------------------------------------------------

TEST_CASE("parse_handshake_response accepts a ready response", "[update][device]")
{
    auto handshake = parse_handshake_response(
        R"({"status":"ready","token":"abc123","expires":1784900000})"
    );
    REQUIRE(handshake.has_value());
    CHECK(handshake->token == "abc123");
    CHECK(handshake->expires == std::chrono::system_clock::from_time_t(1784900000));
}

TEST_CASE("parse_handshake_response tolerates a missing expires", "[update][device]")
{
    // The token is still usable; the server remains the authority on expiry.
    auto handshake = parse_handshake_response(R"({"status":"ready","token":"abc123"})");
    REQUIRE(handshake.has_value());
    CHECK(handshake->token == "abc123");
}

TEST_CASE("parse_handshake_response rejects a non-ready status", "[update][device]")
{
    // The device signals refusal in-band with HTTP 200, so status must be checked rather
    // than inferred from the transport.
    auto result = parse_handshake_response(R"({"status":"busy"})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::HandshakeRejected);
}

TEST_CASE("parse_handshake_response rejects a ready response with no token", "[update][device]")
{
    auto missing = parse_handshake_response(R"({"status":"ready"})");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::MalformedResponse);

    auto empty = parse_handshake_response(R"({"status":"ready","token":""})");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("parse_handshake_response rejects malformed JSON", "[update][device]")
{
    auto result = parse_handshake_response("not json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("parse_prepare_response accepts a ready response", "[update][device]")
{
    auto prepared = parse_prepare_response(R"({"status":"ready","transfer_id":"t-42"})");
    REQUIRE(prepared.has_value());
    CHECK(prepared->transfer_id == "t-42");
}

TEST_CASE("parse_prepare_response reports refusal as TransferRejected", "[update][device]")
{
    // Distinct from HandshakeRejected: the token was fine, the device declined the payload.
    auto result = parse_prepare_response(R"({"status":"error","message":"insufficient space"})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::TransferRejected);
    CHECK(result.error().message.find("insufficient space") != std::string::npos);
}

TEST_CASE("parse_prepare_response rejects a ready response with no id", "[update][device]")
{
    auto result = parse_prepare_response(R"({"status":"ready"})");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("parse_log_event maps all four fields", "[update][device]")
{
    auto entry = parse_log_event(
        R"({"timestamp":"2026-07-22T10:48:38Z","source":"installer","level":"INFO","message":"unpacking"})"
    );
    REQUIRE(entry.has_value());
    CHECK(entry->timestamp == "2026-07-22T10:48:38Z");
    CHECK(entry->source == "installer");
    CHECK(entry->level == "INFO");
    CHECK(entry->message == "unpacking");
}

TEST_CASE("parse_log_event defaults every missing field", "[update][device]")
{
    // A partial log line must not abort an update that is otherwise progressing.
    auto entry = parse_log_event(R"({"message":"only a message"})");
    REQUIRE(entry.has_value());
    CHECK(entry->message == "only a message");
    CHECK(entry->timestamp.empty());
    CHECK(entry->source.empty());
    CHECK(entry->level.empty());
}

TEST_CASE("parse_log_event rejects malformed JSON", "[update][device]")
{
    auto result = parse_log_event("{oops");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::MalformedResponse);
}

TEST_CASE("is_stream_end detects the sentinel only", "[update][device]")
{
    auto sentinel = parse_log_event(R"({"message":"__STREAM_END__"})");
    REQUIRE(sentinel.has_value());
    CHECK(is_stream_end(*sentinel));

    auto ordinary = parse_log_event(R"({"message":"update complete"})");
    REQUIRE(ordinary.has_value());
    CHECK_FALSE(is_stream_end(*ordinary));

    // Must be an exact match -- a log line merely mentioning the sentinel is not the end.
    auto mentions = parse_log_event(R"({"message":"waiting for __STREAM_END__"})");
    REQUIRE(mentions.has_value());
    CHECK_FALSE(is_stream_end(*mentions));
}

TEST_CASE("LogStream is safe to destroy and to stop repeatedly", "[update][device]")
{
    // The whole point of the RAII handle: a default-constructed or already-stopped stream
    // must never call std::terminate, which a bare std::thread would if dropped joinable.
    {
        LogStream stream;
        CHECK_FALSE(stream.running());
    }  // destructor runs on an empty handle

    LogStream stream;
    stream.stop();
    stream.stop();  // idempotent
    CHECK_FALSE(stream.running());

    LogStream moved = std::move(stream);
    CHECK_FALSE(moved.running());
}

// ---------------------------------------------------------------------------------------
// Tier 3 -- requires a real device. NOT RUN. Excluded by the leading dot in the tag.
//
//   test_update_device.exe "[.integration]"
//
// with XVC_TEST_DEVICE_HOST set to a BENCH device. Note that the transfer case below
// actually installs a package and will trigger a device restart, so it is gated behind a
// second opt-in variable.
// ---------------------------------------------------------------------------------------

namespace
{

// MSVC deprecates std::getenv in favour of _dupenv_s; wrap the difference once rather than
// disabling the warning for the whole file.
std::string env_or_empty(const char *name)
{
#ifdef _MSC_VER
    char *value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return {};
    std::string result{value};
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
#endif
}

std::string device_host() { return env_or_empty("XVC_TEST_DEVICE_HOST"); }

}  // namespace

TEST_CASE("device: read version and perform handshake", "[update][device][.integration]")
{
    const auto host = device_host();
    if (host.empty()) {
        SKIP("Set XVC_TEST_DEVICE_HOST to run device integration tests");
    }

    const DeviceEndpoint device{.host = host};

    auto version = get_device_version(device);
    REQUIRE(version.has_value());
    INFO("device version: " << version->to_string());

    auto handshake = perform_handshake(device);
    REQUIRE(handshake.has_value());
    CHECK_FALSE(handshake->token.empty());
}

TEST_CASE("device: full transfer applies an update", "[update][device][.integration]")
{
    // DESTRUCTIVE. This installs a package on the device and restarts it. Gated behind a
    // separate variable so that running the whole [.integration] tag cannot flash hardware
    // by accident.
    const auto host = device_host();
    if (host.empty()) SKIP("Set XVC_TEST_DEVICE_HOST to run device integration tests");

    const auto artifact = env_or_empty("XVC_TEST_DEVICE_ARTIFACT");
    if (artifact.empty()) {
        SKIP("Set XVC_TEST_DEVICE_ARTIFACT to a manifest.json path to run the destructive "
             "transfer test");
    }

    const DeviceEndpoint device{.host = host};

    auto local = use_local_artifact(artifact);
    REQUIRE(local.has_value());

    auto handshake = perform_handshake(device);
    REQUIRE(handshake.has_value());

    auto prepared = prepare_transfer(device, *handshake, *local);
    REQUIRE(prepared.has_value());

    // Start log streaming before the transfer so no output is missed; the handle joins on
    // scope exit even if an assertion below fails.
    auto stream = stream_update_logs(device, *handshake, *prepared, [](const LogEntry &entry) {
        UNSCOPED_INFO(entry.level << " " << entry.source << ": " << entry.message);
    });

    auto result = transfer_file_and_apply_update(
        device, *handshake, *prepared, local->path,
        [](std::uint64_t done, std::uint64_t total) {
            UNSCOPED_INFO("upload " << done << "/" << total);
            return true;
        }
    );
    CHECK(result.has_value());
}
