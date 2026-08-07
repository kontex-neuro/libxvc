#pragma once

#include <string>
#include <string_view>

#include "types.h"

namespace xvc::update
{

// Parses a full versions.json document.
//
// Forward compatibility is a REQUIREMENT, not a nicety: shipped desktop clients cannot be
// updated in lockstep with the cloud, so unknown fields are ignored and missing optional
// fields are defaulted. Only `version`, `platform`, `download_base`, and
// `files[0].{name,size,sha256}` are required; their absence is a hard MalformedResponse.
Result<VersionMatrix> parse_versions_json(std::string_view json_text);

// Parses a manifest.json -- exactly one versions[] entry MINUS download_base (ADR 0008).
// The schemas coincide, so this shares the entry parser with parse_versions_json(); the only
// difference is that download_base is not required here.
Result<VersionEntry> parse_manifest_json(std::string_view json_text);

// GET <cloud_base_url>/versions.json, then delegate to the parser.
//
// Deliberately thin: every piece of real logic lives in parse_versions_json(), which needs
// no network and is therefore fully unit-tested (ADR 0009). Transport failures map to
// NetworkUnreachable, non-2xx to HttpError with http_status set.
Result<VersionMatrix> fetch_versions_json(const std::string &cloud_base_url);

// Newest stable release entry for a platform.
//
// A platform miss reports PlatformNotFound, distinctly from "no update available" (ADR
// 0004). Collapsing the two is how a wrong or unknown platform string reaches production
// disguised as a healthy up-to-date result.
//
// Note that ADR 0007 policy does NOT use this -- plan_update() filters versions[] by the
// required major.minor series itself. This exists for callers that genuinely want "newest".
Result<VersionEntry> select_latest_release(const VersionMatrix &matrix, const std::string &platform);

// Exact-version lookup, used by the forced-update path.
// Deprecated releases are rejected with ReleaseDeprecated unless explicitly allowed.
Result<VersionEntry> select_exact_release(
    const VersionMatrix &matrix, const std::string &platform, const Version &version,
    bool allow_deprecated = false
);

}  // namespace xvc::update
