# libxvc Optional Update Utility API

> **Design record — do not build against this document.**
> The planning layer described here was implemented, but with different rules: `plan_update()`
> takes an explicit `VersionSeries` and resolves the newest stable patch within it, rather
> than following `select_latest_release()`. See the Superseded note in the Update Decision
> section below. `update_device()` was deliberately not implemented (ADR 0006).
>
> **If you are integrating this API, read
> [`update_integration_guide.md`](update_integration_guide.md).**

This document defines optional higher-level updater utilities built on top of the required C++ APIs in `update_api_spec.md`.

These APIs are not required for the core updater library. They are useful for tools, UI apps, and tests that want update policy or full workflow orchestration in one place.

## Update Decision

```cpp
namespace xvc::update {

enum class UpdateAction {
    None,
    Update,
    ForcedUpdate
};

struct UpdatePlan {
    UpdateAction action = UpdateAction::None;
    Version device_version;
    VersionEntry target;
    std::string reason;
};

std::optional<UpdatePlan> plan_update(
    const Version& device_version,
    const VersionMatrix& matrix,
    const std::string& platform
);

std::optional<UpdatePlan> plan_forced_update(
    const Version& device_version,
    const VersionMatrix& matrix,
    const std::string& platform,
    const Version& target_version,
    bool allow_downgrade = false,
    bool allow_deprecated = false
);

}
```

Reasons to keep this utility:

- UI/tools can show the selected target before downloading.
- Tests can verify update policy without network download or device transfer.
- Forced update, deprecated release, and downgrade rules live in one place if callers want that policy.

Rules:

- Normal updates use `select_latest_release()`.
- Forced updates use `select_exact_release()`.
- Deprecated releases are ignored unless explicitly allowed.
- Downgrades are rejected unless explicitly allowed.
- Compare the selected cloud release `version` with `device_version`.

> **Superseded.** The latest-based policy above is **not** what is implemented.
> `docs/flowchart_mermaid.md` gates on *compatibility*, not recency: the desktop
> application is matched to a device-server `major.minor` series, and `plan_update()` takes
> that required series and resolves the newest stable patch within it. `latest_release`
> drives no decision, downgrades into the required series are normal rather than rejected,
> and `deprecated` acts as a release-recall mechanism. `update_device()` is out of scope.
> See `docs/decisions/0007-update-policy-is-compatibility-based.md` and
> `docs/decisions/0006-scope-planning-layer-without-orchestrator.md`.

## Full Workflow

```cpp
namespace xvc::update {

struct UpdateOptions {
    std::string cloud_base_url;
    std::string platform;
    DeviceEndpoint device;
    std::filesystem::path download_dir;
    std::optional<Version> force_version;
    bool download_only = false;
    bool allow_downgrade = false;
    bool allow_deprecated = false;
};

struct UpdateResult {
    UpdateAction action = UpdateAction::None;
    Version device_version;
    VersionEntry target;
    DownloadedArtifact downloaded_artifact;
    std::string message;
};

std::optional<UpdateResult> update_device(
    const UpdateOptions& options,
    ProgressCallback download_progress = nullptr,
    ProgressCallback upload_progress = nullptr,
    LogCallback log_callback = nullptr
);

}
```

`update_device()` is a convenience API for applications that want the whole workflow:

1. Fetch `versions.json`.
2. Query the local device version from `/api_version`.
3. Build an update plan.
4. Download and verify the artifact.
5. If not `download_only`, handshake with the update service.
6. Prepare transfer.
7. Stream logs if requested.
8. Upload the verified package and let the device apply it.

Reasons to keep this utility:

- `xvc_update_tool.cc` can call one function for the common path.
- Applications that do not need custom policy can avoid wiring all steps manually.
- The update sequence stays consistent: fetch, select, compare, download, verify, handshake, prepare, transfer.
