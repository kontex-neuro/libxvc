# libxvc Update API Design

> **Design record — do not build against this document.**
> The signatures below are superseded: the implementation returns
> `Result<T> = std::expected<T, Error>` rather than `std::optional`, reuses `xvc::Version`
> rather than defining its own, and returns an RAII `LogStream` rather than a bare
> `std::thread`. Three statements here are also actively wrong — update policy is
> compatibility-based rather than latest-based (ADR 0007), `platform` identifies the device
> being updated rather than the host (ADR 0004), and artifacts are transient rather than
> reused-if-verified (ADR 0008).
>
> **If you are integrating this API, read
> [`update_integration_guide.md`](update_integration_guide.md).**
> Precedence when documents disagree: the header, then `decisions/`, then
> `update_implementation_plan.md`, then this file.

## Abstract

libxvc needs a small updater API that lets C++ programs:

1. Parse and compare SemVer versions.
2. Fetch and parse Kontex Cloud `versions.json`.
3. Select the latest stable release for a platform, or select an exact version when forced.
4. Read the current device version from the device server.
5. Download the update artifact into a local temp/download directory.
6. Verify the downloaded file with SHA-256 and size from `versions.json`.
7. Transfer the verified package to the device update service.
8. Stream device update logs while the update is being applied.

The API should replace the old cloud flow that lists bucket directories and downloads YAML metadata. New clients must consume only `<cloud_base_url>/versions.json`.

## Required C++ API

### Version

```cpp
namespace xvc::update {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

std::optional<Version> parse_version(std::string_view text);
std::string to_string(const Version& version);
int compare_versions(const Version& lhs, const Version& rhs);
bool is_newer(const Version& candidate, const Version& current);
bool is_equal(const Version& candidate, const Version& current);

}
```

Rules:

- Accept `X.Y.Z`.
- Optionally accept `vX.Y.Z` at public API boundaries and normalize to `X.Y.Z`.
- Compare by `major`, then `minor`, then `patch`.

> **Implementation note.** No new `Version` type is defined. `xvc::update::Version` is an
> alias for the existing `xvc::Version` in `xdaqvc/common.h`, which already satisfies
> these rules; the free functions wrap its operators. See
> `docs/decisions/0002-reuse-existing-version-type.md`.

### Cloud Metadata

```cpp
namespace xvc::update {

struct Artifact {
    std::string name;
    std::uint64_t size = 0;
    std::string sha256;
};

struct VersionEntry {
    Version version;
    std::string platform;
    std::string environment;  // "release" or "dev"
    std::string status;       // "stable" or "deprecated"
    std::string released_at;
    std::string changelog;
    std::string deprecation_reason;
    std::string download_base;
    std::vector<Artifact> files;
};

struct VersionMatrix {
    std::string updated_at;
    std::map<std::string, Version> latest_release;
    std::vector<VersionEntry> versions;
};

std::optional<VersionMatrix> parse_versions_json(std::string_view json_text);
std::optional<VersionMatrix> fetch_versions_json(const std::string& cloud_base_url);

std::optional<VersionEntry> select_latest_release(
    const VersionMatrix& matrix,
    const std::string& platform
);

std::optional<VersionEntry> select_exact_release(
    const VersionMatrix& matrix,
    const std::string& platform,
    const Version& version,
    bool allow_deprecated = false
);

}
```

Rules:

- `fetch_versions_json()` performs `GET <cloud_base_url>/versions.json`.
- `latest_release[platform]` is the source of truth for automatic stable updates.
- If `latest_release` has no key for the current platform, no automatic update is available.
- Clients must not list `dist/` or `dev/` folders.
- Each platform version is expected to have exactly one artifact in `files`.
- `download_base` plus `files[0].name` creates the artifact URL.

### Reference document

`docs/examples/versions.json` is the authoritative example of the wire format. The parser
is written and tested against it. The notes below record what that file establishes and
where it differs from the declarations above.

**`platform` identifies the device being updated, not the host running libxvc.**
The observed value is `linux-arm64` (the Jetson-based device server). libxvc itself ships
on Windows and macOS, but those never appear as `platform` values. Callers must not pass
a host platform string to `select_latest_release()`.

**`download_base` is a path, not an absolute URL.** Observed:
`"/dist/v0.1.2/linux-arm64/"`. The artifact URL is therefore

```
<cloud_base_url> + <download_base> + <files[0].name>
```

which is why `download_artifact()` takes `cloud_base_url`. `download_base` carries both a
leading and a trailing slash, so joining must not duplicate separators.

**Version strings carry no `v` prefix** in either `latest_release` or `version`
(`"0.1.2"`). The `v` prefix is still accepted on input per the Version rules, but is not
emitted by the cloud. Note that `download_base` *does* contain a `v` in its path segment
(`/dist/v0.1.2/`); that is part of the path, not a version string to parse.

**`deprecation_reason` is absent** from every entry in the reference file — no release is
currently deprecated. It remains part of the schema; parsers must treat it as optional and
default it to empty.

**`min_client_version` is gone.** The old format carried it as a guard against an
outdated client pulling an incompatible package. The new format has no equivalent, so
nothing in the metadata prevents an old client from selecting a package it cannot handle.

**Forward compatibility.** Shipped desktop clients cannot be updated in lockstep with the
cloud, so the parser ignores unknown fields and defaults missing optional ones. Only
`version`, `platform`, `download_base`, and `files[0].{name,size,sha256}` are required;
their absence is a hard parse failure.

**A release download also includes `manifest.json`**, which is exactly one `versions[]`
entry **minus `download_base`** — same fields, same types. One parser therefore serves both,
with `download_base` optional. The manifest is the trust anchor for offline/developer
updates, where no matrix is available to verify against; the online path does not need it,
since hash, size, and path are all derivable from `versions.json`. Only the tarball is
transferred to the device. See
`docs/decisions/0008-artifact-lifecycle-and-offline-update.md`.

### Device Query

```cpp
namespace xvc::update {

struct DeviceEndpoint {
    std::string host;
    int server_port = 8000;
    int update_port = 8001;
};

std::optional<Version> get_device_version(const DeviceEndpoint& device);

}
```

This wraps the existing device server endpoint:

- `GET http://<host>:<server_port>/api_version`

The API returns this as `device_version`. The HTTP endpoint name stays `/api_version` for compatibility, but updater callers should treat the returned value as the single source device version.

### Download and Verify

```cpp
namespace xvc::update {

struct DownloadedArtifact {
    Artifact artifact;
    std::filesystem::path path;
    std::string url;
};

using ProgressCallback = std::function<bool(std::uint64_t done, std::uint64_t total)>;

std::optional<std::string> calculate_sha256(const std::filesystem::path& path);

bool verify_file(
    const std::filesystem::path& path,
    const Artifact& artifact
);

std::optional<DownloadedArtifact> download_artifact(
    const std::string& cloud_base_url,
    const VersionEntry& entry,
    const Artifact& artifact,
    const std::filesystem::path& download_dir,
    ProgressCallback progress = nullptr
);

}
```

Rules:

- Each selected `VersionEntry` must contain exactly one artifact.
- Download to a temp file first, then rename after verification.
- Verify manifest `size` and `sha256`.
- Delete incomplete temp files on failure or cancellation.
- Reuse an existing local file only if verification succeeds.

### Device Transfer

```cpp
namespace xvc::update {

struct Handshake {
    std::string token;
    std::chrono::system_clock::time_point expires;
};

struct PreparedTransfer {
    std::string transfer_id;
};

struct LogEntry {
    std::string timestamp;
    std::string source;
    std::string level;
    std::string message;
};

using LogCallback = std::function<void(const LogEntry&)>;

std::optional<Handshake> perform_handshake(const DeviceEndpoint& device);

std::optional<PreparedTransfer> prepare_transfer(
    const DeviceEndpoint& device,
    const Handshake& handshake,
    const DownloadedArtifact& artifact
);

bool transfer_file_and_apply_update(
    const DeviceEndpoint& device,
    const Handshake& handshake,
    const PreparedTransfer& transfer,
    const std::filesystem::path& file,
    ProgressCallback progress = nullptr
);

std::thread stream_update_logs(
    const DeviceEndpoint& device,
    const Handshake& handshake,
    const PreparedTransfer& transfer,
    LogCallback on_log
);

}
```

This wraps the existing update service endpoints:

- `GET /handshake`
- `POST /prepare-transfer`
- `POST /transfer/<transfer_id>`
- `GET /stream-logs/<transfer_id>`

> **Implementation note.** `ProgressCallback` and `LogCallback` are invoked on a worker
> thread and must not touch UI state directly; the caller marshals to its own thread. The
> library throttles progress emissions. `stream_update_logs()` returns an RAII handle that
> stops and joins on destruction rather than a bare `std::thread`, which would call
> `std::terminate` if dropped while joinable. See
> `docs/decisions/0003-callback-threading-contract.md`.

## Migration Notes

- Replace `get_version_table()` with `fetch_versions_json()` and `parse_versions_json()`.
- Replace remote directory listing and YAML metadata parsing in the update path.
- Keep the existing SHA-256, device version, handshake, transfer, and log-streaming implementations as lower-level building blocks.

