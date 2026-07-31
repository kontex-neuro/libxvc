#pragma once

#include <string>

#include "types.h"

namespace xvc::update
{

// The device-server major.minor series the application is built against. Patch versions
// within a series are interchangeable from the application's perspective, which is why patch
// is deliberately absent here.
//
// The application owns this claim and passes it in; it is NOT derivable from libxvc's own
// version, which is an independent version line (libxvc 0.3.2 vs. device server 0.1.2).
struct VersionSeries {
    int major = 0;
    int minor = 0;
};

enum class UpdateAction {
    None,    // device is compatible and current enough; leave it alone
    Update,  // move the device to `target` -- check `UpdatePlan::required` for severity
};

struct UpdatePlan {
    UpdateAction action = UpdateAction::None;
    Version device_version;

    // Only meaningful when action == Update.
    VersionEntry target;

    // TRUE means the device is outside the application's supported series and the app cannot
    // run until the update completes -- the blocking dialog at flowchart node E. FALSE means
    // the device is already compatible and this is an optional improvement the application
    // may choose not to surface at all.
    //
    // Collapsing these two would make the app either block on optional updates or fail to
    // block on required ones.
    bool required = false;

    // Human-readable rationale for logs and dialogs. Never branch on it.
    std::string reason;
};

// Resolves the newest acceptable device-server release within the required series.
//
// ADR 0007: policy is COMPATIBILITY-based, not latest-based. The gate is "is the device
// compatible with this application", not "is something newer available". `latest_release` in
// versions.json drives no decision here. A device on 0.1.5 while latest is 0.2.0 is correct
// and must not be touched when the app requires 0.1.x; conversely a device on 0.2.0 with an
// app requiring 0.1.x must move DOWN to 0.1.5. Downgrades into the required series are
// normal, so there is no allow_downgrade parameter.
//
// Returns VersionNotFound when the cloud publishes no stable release in the required series
// -- a deployment error, surfaced loudly rather than silently treated as "up to date".
Result<UpdatePlan> plan_update(
    const Version &device_version, const VersionMatrix &matrix, const VersionSeries &required,
    const std::string &platform = std::string{DEVICE_PLATFORM}
);

// Operator-driven override targeting one exact version, bypassing series policy.
//
// This is the only place `allow_downgrade` is meaningful: under plan_update() a version
// decrease is an expected outcome, whereas here the operator is deliberately stepping
// outside policy and should have to say so.
Result<UpdatePlan> plan_forced_update(
    const Version &device_version, const VersionMatrix &matrix, const Version &target_version,
    bool allow_downgrade = false, bool allow_deprecated = false,
    const std::string &platform = std::string{DEVICE_PLATFORM}
);

}  // namespace xvc::update
