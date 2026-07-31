#include "manifest.h"

#include <cpr/cpr.h>

#include <algorithm>
#include <format>
#include <nlohmann/json.hpp>

namespace xvc::update
{

namespace
{

constexpr int HTTP_OK = 200;
constexpr auto FETCH_TIMEOUT = std::chrono::seconds{10};

// Reads a required string. Missing or wrong-typed is a hard failure -- these are the fields
// without which an entry cannot be acted on at all.
std::expected<std::string, Error> require_string(
    const nlohmann::json &node, std::string_view field, std::string_view context
)
{
    auto it = node.find(field);
    if (it == node.end() || !it->is_string()) {
        return std::unexpected(Error{
            ErrorCode::MalformedResponse,
            std::format("{}: missing or non-string required field '{}'", context, field)
        });
    }
    return it->get<std::string>();
}

// Optional string: absent, null, or wrong-typed all yield the default rather than failing.
// `deprecation_reason` is absent from every entry in the reference file, and unknown future
// fields must not break a shipped client.
std::string optional_string(const nlohmann::json &node, std::string_view field)
{
    auto it = node.find(field);
    if (it == node.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

std::expected<Artifact, Error> parse_artifact(const nlohmann::json &node, std::string_view context)
{
    if (!node.is_object()) {
        return std::unexpected(Error{
            ErrorCode::MalformedResponse, std::format("{}: files[0] is not an object", context)
        });
    }

    auto name = require_string(node, "name", context);
    if (!name) return std::unexpected(name.error());

    auto sha256 = require_string(node, "sha256", context);
    if (!sha256) return std::unexpected(sha256.error());

    auto size_it = node.find("size");
    if (size_it == node.end() || !size_it->is_number_unsigned()) {
        return std::unexpected(Error{
            ErrorCode::MalformedResponse,
            std::format("{}: missing or non-numeric required field 'size'", context)
        });
    }

    return Artifact{
        .name = *name, .size = size_it->get<std::uint64_t>(), .sha256 = *sha256
    };
}

// Shared by both entry points. `require_download_base` is the ONLY difference between a
// matrix entry and a manifest (ADR 0008).
std::expected<VersionEntry, Error> parse_entry(
    const nlohmann::json &node, bool require_download_base, std::string_view context
)
{
    if (!node.is_object()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, std::format("{}: entry is not an object", context)}
        );
    }

    auto version_str = require_string(node, "version", context);
    if (!version_str) return std::unexpected(version_str.error());

    auto version = Version::from_string(*version_str);
    if (!version) {
        return std::unexpected(Error{
            ErrorCode::MalformedResponse,
            std::format("{}: unparseable version '{}'", context, *version_str)
        });
    }

    auto platform = require_string(node, "platform", context);
    if (!platform) return std::unexpected(platform.error());

    VersionEntry entry;
    entry.version = *version;
    entry.platform = *platform;
    entry.environment = optional_string(node, "environment");
    entry.status = optional_string(node, "status");
    entry.released_at = optional_string(node, "released_at");
    entry.changelog = optional_string(node, "changelog");
    entry.deprecation_reason = optional_string(node, "deprecation_reason");

    if (require_download_base) {
        auto download_base = require_string(node, "download_base", context);
        if (!download_base) return std::unexpected(download_base.error());
        entry.download_base = *download_base;
    } else if (auto it = node.find("download_base"); it != node.end() && it->is_string()) {
        // Harmless if a manifest happens to carry it.
        entry.download_base = it->get<std::string>();
    }

    auto files_it = node.find("files");
    if (files_it == node.end() || !files_it->is_array() || files_it->empty()) {
        return std::unexpected(Error{
            ErrorCode::MalformedResponse,
            std::format("{}: 'files' is missing, not an array, or empty", context)
        });
    }

    // Each platform version is expected to carry exactly one artifact. Parse them all rather
    // than only files[0], but only files[0] is required to be well-formed, since that is the
    // one every caller uses.
    auto artifact = parse_artifact(files_it->at(0), context);
    if (!artifact) return std::unexpected(artifact.error());
    entry.files.push_back(*artifact);

    for (std::size_t i = 1; i < files_it->size(); ++i) {
        if (auto extra = parse_artifact(files_it->at(i), context)) {
            entry.files.push_back(*extra);
        }
    }

    return entry;
}

}  // namespace

Result<VersionMatrix> parse_versions_json(std::string_view json_text)
{
    // parse() with exceptions disabled: a malformed document is an expected failure mode of
    // a network fetch, not something to throw across the API boundary (ADR 0005).
    auto json = nlohmann::json::parse(json_text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "versions.json is not a valid JSON object"}
        );
    }

    VersionMatrix matrix;
    matrix.updated_at = optional_string(json, "updated_at");

    // Parsed because it is part of the wire format; ADR 0007 policy reads nothing from it.
    // An unparseable entry here is skipped rather than fatal -- no decision depends on it.
    if (auto it = json.find("latest_release"); it != json.end() && it->is_object()) {
        for (const auto &[platform, value] : it->items()) {
            if (!value.is_string()) continue;
            if (auto version = Version::from_string(value.get<std::string>())) {
                matrix.latest_release.emplace(platform, *version);
            }
        }
    }

    auto versions_it = json.find("versions");
    if (versions_it == json.end() || !versions_it->is_array()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "versions.json: 'versions' is missing or not an array"}
        );
    }

    matrix.versions.reserve(versions_it->size());
    for (std::size_t i = 0; i < versions_it->size(); ++i) {
        auto entry = parse_entry(
            versions_it->at(i), /*require_download_base=*/true, std::format("versions[{}]", i)
        );
        if (!entry) return std::unexpected(entry.error());
        matrix.versions.push_back(std::move(*entry));
    }

    return matrix;
}

Result<VersionEntry> parse_manifest_json(std::string_view json_text)
{
    auto json = nlohmann::json::parse(json_text, nullptr, false);
    if (json.is_discarded()) {
        return std::unexpected(
            Error{ErrorCode::MalformedResponse, "manifest.json is not valid JSON"}
        );
    }
    return parse_entry(json, /*require_download_base=*/false, "manifest.json");
}

Result<VersionMatrix> fetch_versions_json(const std::string &cloud_base_url)
{
    // Join without duplicating the separator; cloud_base_url may or may not carry a trailing
    // slash depending on how the caller configured it.
    auto base = cloud_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const auto url = std::format("{}/versions.json", base);

    auto response = cpr::Get(cpr::Url{url}, cpr::Timeout{FETCH_TIMEOUT});

    if (response.error.code != cpr::ErrorCode::OK) {
        return std::unexpected(Error{
            ErrorCode::NetworkUnreachable,
            std::format("GET {} failed: {}", url, response.error.message)
        });
    }

    if (response.status_code != HTTP_OK) {
        return std::unexpected(Error{
            .code = ErrorCode::HttpError,
            .message = std::format("GET {} returned HTTP {}", url, response.status_code),
            .http_status = static_cast<int>(response.status_code)
        });
    }

    return parse_versions_json(response.text);
}

Result<VersionEntry> select_latest_release(const VersionMatrix &matrix, const std::string &platform)
{
    const VersionEntry *best = nullptr;
    bool platform_seen = false;

    for (const auto &entry : matrix.versions) {
        if (entry.platform != platform) continue;
        platform_seen = true;
        if (entry.environment != "release" || entry.status != "stable") continue;
        if (best == nullptr || entry.version > best->version) best = &entry;
    }

    if (!platform_seen) {
        // Distinct from "nothing newer" (ADR 0004) -- this means the platform string itself
        // is wrong or unpublished, which must not masquerade as up-to-date.
        return std::unexpected(Error{
            ErrorCode::PlatformNotFound,
            std::format("No releases published for platform '{}'", platform)
        });
    }

    if (best == nullptr) {
        return std::unexpected(Error{
            ErrorCode::VersionNotFound,
            std::format("No stable release entries for platform '{}'", platform)
        });
    }

    return *best;
}

Result<VersionEntry> select_exact_release(
    const VersionMatrix &matrix, const std::string &platform, const Version &version,
    bool allow_deprecated
)
{
    bool platform_seen = false;

    for (const auto &entry : matrix.versions) {
        if (entry.platform != platform) continue;
        platform_seen = true;
        if (!(entry.version == version)) continue;

        if (entry.status == "deprecated" && !allow_deprecated) {
            return std::unexpected(Error{
                ErrorCode::ReleaseDeprecated,
                std::format(
                    "Release {} for '{}' is deprecated{}", version.to_string(), platform,
                    entry.deprecation_reason.empty()
                        ? std::string{}
                        : std::format(": {}", entry.deprecation_reason)
                )
            });
        }
        return entry;
    }

    if (!platform_seen) {
        return std::unexpected(Error{
            ErrorCode::PlatformNotFound,
            std::format("No releases published for platform '{}'", platform)
        });
    }

    return std::unexpected(Error{
        ErrorCode::VersionNotFound,
        std::format("Release {} not found for platform '{}'", version.to_string(), platform)
    });
}

}  // namespace xvc::update
