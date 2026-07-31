#include "plan.h"

#include <format>

#include "manifest.h"

namespace xvc::update
{

namespace
{

bool in_series(const Version &version, const VersionSeries &series)
{
    return version.major() == series.major && version.minor() == series.minor;
}

// Rule 1: platform matches, environment == "release", status == "stable", and the version is
// in the required series. Filtering to "stable" is what makes a bad release retractable --
// it is the recall mechanism, not merely metadata hygiene.
bool is_candidate(const VersionEntry &entry, const VersionSeries &required,
                  const std::string &platform)
{
    return entry.platform == platform && entry.environment == "release" &&
           entry.status == "stable" && in_series(entry.version, required);
}

// Looks up the device's EXACT version in the matrix. The device version alone does not
// reveal whether that build was later recalled -- only the matrix does.
const VersionEntry *find_exact(
    const VersionMatrix &matrix, const Version &version, const std::string &platform
)
{
    for (const auto &entry : matrix.versions) {
        if (entry.platform == platform && entry.version == version) return &entry;
    }
    return nullptr;
}

}  // namespace

Result<UpdatePlan> plan_update(
    const Version &device_version, const VersionMatrix &matrix, const VersionSeries &required,
    const std::string &platform
)
{
    // Rules 1 and 3: gather candidates in the required series, take the highest patch.
    const VersionEntry *target = nullptr;
    for (const auto &entry : matrix.versions) {
        if (!is_candidate(entry, required, platform)) continue;
        if (target == nullptr || entry.version > target->version) target = &entry;
    }

    // Rule 2. The application requires a series the cloud does not publish. This is a
    // deployment error and must be loud -- silently reporting "up to date" would strand the
    // device with no indication anything is wrong.
    if (target == nullptr) {
        return std::unexpected(Error{
            ErrorCode::VersionNotFound,
            std::format(
                "No stable release in required series {}.{} for platform '{}'", required.major,
                required.minor, platform
            )
        });
    }

    UpdatePlan plan;
    plan.device_version = device_version;
    plan.target = *target;

    // Rule 6: outside the required series -- blocking, regardless of direction. A device
    // ahead of the series (0.2.0 when the app requires 0.1.x) is just as incompatible as one
    // behind it, so this check precedes any patch comparison.
    if (!in_series(device_version, required)) {
        plan.action = UpdateAction::Update;
        plan.required = true;
        plan.reason = std::format(
            "Device {} is outside the required {}.{} series; update to {} is required to run",
            device_version.to_string(), required.major, required.minor,
            target->version.to_string()
        );
        return plan;
    }

    // Rule 4: in-series and at or ahead of the target.
    if (device_version >= target->version) {
        // Rule 4's exception -- the recall path. Without this, a device sitting on a patch
        // that was later deprecated would be stranded on a known-bad build, because only the
        // matrix knows its status. This is the one case where a patch-level DOWNGRADE is
        // correct, so it is deliberately checked before returning None.
        const auto *device_entry = find_exact(matrix, device_version, platform);
        if (device_entry != nullptr && device_entry->status == "deprecated") {
            plan.action = UpdateAction::Update;
            // The device is still inside the supported series, so this is not the blocking
            // node-E case; it is a strong recommendation the app decides how to surface.
            plan.required = false;
            plan.reason = std::format(
                "Device {} has been deprecated{}; moving to newest stable {}",
                device_version.to_string(),
                device_entry->deprecation_reason.empty()
                    ? std::string{}
                    : std::format(" ({})", device_entry->deprecation_reason),
                target->version.to_string()
            );
            return plan;
        }

        plan.action = UpdateAction::None;
        plan.required = false;
        plan.reason = std::format(
            "Device {} is current within the required {}.{} series", device_version.to_string(),
            required.major, required.minor
        );
        return plan;
    }

    // Rule 5: in-series but behind on patch. Already compatible, so this is optional.
    plan.action = UpdateAction::Update;
    plan.required = false;
    plan.reason = std::format(
        "Device {} is compatible; {} is available within the {}.{} series",
        device_version.to_string(), target->version.to_string(), required.major, required.minor
    );
    return plan;
}

Result<UpdatePlan> plan_forced_update(
    const Version &device_version, const VersionMatrix &matrix, const Version &target_version,
    bool allow_downgrade, bool allow_deprecated, const std::string &platform
)
{
    auto target = select_exact_release(matrix, platform, target_version, allow_deprecated);
    if (!target) return std::unexpected(target.error());

    // Unlike plan_update(), where a version decrease is an expected outcome of compatibility
    // policy, here the operator is bypassing policy deliberately and must opt in.
    if (target->version < device_version && !allow_downgrade) {
        return std::unexpected(Error{
            ErrorCode::DowngradeRejected,
            std::format(
                "Target {} is older than device {}; pass allow_downgrade to proceed",
                target->version.to_string(), device_version.to_string()
            )
        });
    }

    UpdatePlan plan;
    plan.device_version = device_version;
    plan.target = *target;
    plan.required = false;  // an operator override, not a compatibility gate

    if (target->version == device_version) {
        plan.action = UpdateAction::None;
        plan.reason =
            std::format("Device is already running the forced target {}", target_version.to_string());
        return plan;
    }

    plan.action = UpdateAction::Update;
    plan.reason = std::format(
        "Forced update from {} to {}", device_version.to_string(), target->version.to_string()
    );
    return plan;
}

}  // namespace xvc::update
