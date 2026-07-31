# Integrating the libxvc Update API into the Qt/QML Application

Audience: the developer wiring device updates into the desktop application.

This is the **usage** document. `docs/update_api_spec.md` and
`docs/update_utility_api_spec.md` are earlier design records whose signatures no longer
match the code — do not build against them. Where anything disagrees, the header is the
authority, then the ADRs in `docs/decisions/`.

```cpp
#include <xdaqvc/update/update.h>   // one include for the whole surface
using namespace xvc::update;
```

---

## Three things that surprise people

**1. Update policy is compatibility-based, not "install the newest."**
The application declares the device-server `major.minor` series it supports, and
`plan_update()` resolves the newest stable patch *within that series*. A device on `0.1.5`
must not be touched when the app requires `0.1.x`, even if `0.2.0` exists. A device on
`0.2.0` when the app requires `0.1.x` must move **down**. Downgrades are normal, not errors.

**2. `platform` is the device, never the host.**
It is always `linux-arm64` (the Jetson). libxvc runs on Windows and macOS, but those never
appear as platform values. Use the `DEVICE_PLATFORM` constant; never derive it from
`#ifdef _WIN32`.

**3. Downloaded artifacts are transient.**
There is no cache. libxvc deletes its temp file on success, failure, and cancellation, and
you own the verified file it returns. A repeated update re-downloads ~46 MB.

---

## Where this fits in the application

[`flowchart_mermaid.md`](flowchart_mermaid.md) is the **app-level** flow — launch, connect,
gate, run — and is the starting point for designing the update experience. This guide covers
only the API calls behind the update path (nodes `B`–`K`). Dialog wording, retry policy,
reconnect behaviour, and everything on the client-self-update branch are application
concerns that libxvc has no opinion about.

| Flowchart node | Your responsibility | API involved |
| --- | --- | --- |
| `B` connect / read server version | Reconnect loop, timeouts | `get_device_version()` |
| `C` device connected? | Retry policy when unreachable | returns `NetworkUnreachable` |
| `D` version compatible? | — | `plan_update()` → `action`, `required` |
| `E` REQUIRED update dialog | Blocking modal wording | `plan->required == true` |
| `F` user choice | Offering "Ignore" at all is your call | — |
| `G` package available? | — | `fetch_versions_json()` |
| `H` download | Progress UI, cancel button | `download_artifact()` |
| `H1` download OK? | Route failures to `J1` | error code from `download_artifact()` |
| `I` push to device | "Do not disconnect" warning | handshake → prepare → transfer |
| `J` succeeded? | — | return value of the transfer |
| `J1` failed, device restored | Retry / Quit dialog | firmware rolls back on its own |
| `K` restart app / reconnect | Reconnect sequencing | back to `B` |
| `M`–`P` client self-update | **Not implemented in libxvc.** Future work. | — |

Four things the flowchart requires that this guide's happy path does not show:

- **`C` — device not connected.** `get_device_version()` returns `NetworkUnreachable`; loop
  back and retry rather than treating it as an update failure.
- **`F` — "Ignore" on a required update.** The flowchart allows it (edge `F → O`). libxvc
  does not enforce blocking; `plan->required` only *tells* you it is required. If your
  product decides a required update cannot be dismissed, that is enforced in the app.
- **`J1` — the device restores itself.** On a failed apply, firmware rolls back. Your job is
  the Retry / Quit dialog, not recovery.
- **`K` — after success the device restarts.** Expect the connection to drop; reconnect and
  re-read the version rather than assuming the new version is live immediately.

---

## The call sequence

libxvc deliberately has no `update_device()` — the app owns sequencing so it can show the
target, get confirmation, and offer a working cancel button (ADR 0006).

```
fetch_versions_json ──► get_device_version ──► plan_update
                                                   │
                                    action == None ─┴─► done, device is compatible
                                                   │
                                    action == Update
                                                   ▼
                              [show changelog, ask the user]
                                                   ▼
   download_artifact ──► perform_handshake ──► prepare_transfer ──► stream_update_logs
                                                                          │
                                                          transfer_file_and_apply_update
```

### 1–3. Decide whether an update is needed

```cpp
// The series YOUR application supports. Hardcode it here -- it is not derivable from
// libxvc's own version, which is an independent version line.
constexpr VersionSeries REQUIRED_SERIES{0, 1};

const DeviceEndpoint device{.host = "192.168.177.100"};  // ports default to 8000/8001

auto matrix = fetch_versions_json("https://cloud.example.com");
if (!matrix) return show_error(matrix.error());

auto device_version = get_device_version(device);
if (!device_version) return show_error(device_version.error());

auto plan = plan_update(*device_version, *matrix, REQUIRED_SERIES);
if (!plan) return show_error(plan.error());   // VersionNotFound => deployment problem

if (plan->action == UpdateAction::None) return;   // compatible; nothing to do

// plan->required distinguishes the two dialogs:
//   true  -> BLOCKING. The app cannot run until this completes.
//   false -> optional. An offer you may choose not to surface at all.
show_update_dialog(plan->target.version.to_string(),
                   plan->target.changelog,   // markdown, ready to render
                   plan->required);
```

### 4. Download, after the user confirms

```cpp
auto artifact = download_artifact(
    "https://cloud.example.com", plan->target,
    [this](std::uint64_t done, std::uint64_t total) {
        // WORKER THREAD -- see the threading section below.
        QMetaObject::invokeMethod(this, [this, done, total] {
            setDownloadProgress(total > 0 ? double(done) / double(total) : 0.0);
        }, Qt::QueuedConnection);
        return !m_cancelled.load();     // returning false cancels the download
    });

if (!artifact) return show_error(artifact.error());
```

### 5–7. Push it to the device

```cpp
auto handshake = perform_handshake(device);
if (!handshake) return show_error(handshake.error());

auto prepared = prepare_transfer(device, *handshake, *artifact);
if (!prepared) return show_error(prepared.error());

// Start logs BEFORE the transfer so nothing is missed. The handle stops and joins when it
// goes out of scope, so a cancelled update cannot leave a thread running.
auto logs = stream_update_logs(device, *handshake, *prepared,
    [this](const LogEntry &entry) {
        QMetaObject::invokeMethod(this, [this, entry] { appendLog(entry); },
                                  Qt::QueuedConnection);
    });

auto result = transfer_file_and_apply_update(
    device, *handshake, *prepared, artifact->path,
    [this](std::uint64_t done, std::uint64_t total) {
        QMetaObject::invokeMethod(this, [this, done, total] {
            setUploadProgress(total > 0 ? double(done) / double(total) : 0.0);
        }, Qt::QueuedConnection);
        return !m_cancelled.load();
    });

// Artifacts are transient -- libxvc does not retain a cache, so clean up your copy.
std::error_code ec;
std::filesystem::remove(artifact->path, ec);

if (!result) return show_error(result.error());

// The device restarts. Reconnect and re-read its version.
```

---

## Threading — the part that bites

**Every callback fires on a worker thread.** Touching a `QObject` or a QML-visible property
from one is undefined behaviour: it usually appears to work under test and fails in the
field. Always marshal, as in the snippets above.

- **Cancellation flag must be `std::atomic<bool>`** — written by the GUI thread, read by a
  worker.
- **`total` may be `0`** when the server sends no `Content-Length`. Never divide by it; bind
  `ProgressBar.indeterminate: total === 0`.
- **The final progress callback may arrive *after* the function returns**, because delivery
  is queued. Drive "finished" state from the return value, never from the last tick.
- **libxvc already throttles progress to ~100 ms**, so you do not need to rate-limit again.

---

## Errors

Branch on `error.code`. **Never** parse `error.message` — it is for logs and its wording
will change.

| `ErrorCode` | Suggested user-facing handling |
| --- | --- |
| `NetworkUnreachable` | "Check your internet connection." Offer retry. |
| `HttpError` | Update server problem; `http_status` carries the code. Offer retry. |
| `MalformedResponse` | "The update service returned unreadable data." Not user-fixable. |
| `PlatformNotFound` | Deployment error — log loudly, do not present as "up to date". |
| `VersionNotFound` | The cloud publishes no build for the required series. Escalate. |
| `ReleaseDeprecated` | Only from forced updates; that build was recalled. |
| `DowngradeRejected` | Only from `plan_forced_update()` without `allow_downgrade`. |
| `SizeMismatch` | Transfer was truncated. **Retry is appropriate.** |
| `HashMismatch` | **Security-relevant.** Corruption or tampering — do NOT auto-retry. Advise retrying later; if it persists, escalate. |
| `FilesystemError` | Disk full or permissions. Show the path from `message`. |
| `Cancelled` | The user cancelled. **Suppress the error dialog** — they already know. |
| `HandshakeRejected` | Device busy or token expired. Retry from the handshake. |
| `TransferRejected` | Device refused the package (e.g. insufficient space). |
| `DeviceUpdateFailed` | Device could not apply it; firmware rolls back. Offer Retry / Quit. |

`SizeMismatch` and `HashMismatch` are deliberately distinct: the first is a transport blip
worth retrying, the second is not.

---

## Offline / bench updates

```cpp
// Requires BOTH the tarball and its manifest.json in the same directory.
auto artifact = use_local_artifact("/path/to/manifest.json");
```

Verifies against the manifest and **deletes nothing** — those are your files. From here the
handshake/prepare/transfer steps are identical. A tarball alone cannot be applied; the
manifest is the trust anchor.

---

## Reference

**Read these two, in this order:**

1. [`flowchart_mermaid.md`](flowchart_mermaid.md) — the app-level flow. Design the
   experience from this.
2. **This guide** — the API calls behind the update portion of that flow.

**Consult as needed:**

- **Headers** — `xdaqvc/update/{types,manifest,plan,artifact,device}.h`. Every contract is
  documented at its declaration; these are the authority if anything here disagrees.
- **Why it works this way** — [`decisions/`](decisions/). Most relevant: 0003 (threading),
  0005 (errors), 0007 (policy), 0008 (artifact lifecycle).

**Do NOT build against** `update_api_spec.md` or `update_utility_api_spec.md`. They are
superseded design records, retained for history and bannered as such.

**Not implemented:**

- `update_device()` one-call orchestration — deliberate (ADR 0006); you drive the sequence.
- Client self-update (flowchart nodes `M`–`P`) — future work; nothing in libxvc supports it.

**Unverified — treat as untested until you exercise it:**

- Every device HTTP path (handshake, prepare, transfer, log stream) has been compiled and
  its response parsing unit-tested, but **never run against real hardware**.
- `fetch_versions_json()` and `download_artifact()`'s network path have no test coverage.
- The macOS SHA-256 implementation has never been built or run.
