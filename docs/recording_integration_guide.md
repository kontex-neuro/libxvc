# Device recording API integration (v1)

Use `#include <xdaqvc/recording.h>` from `libxvc/0.5.0-dev` with the matching videoserver feature backend. `RecordingClient` performs blocking HTTP requests; GUI consumers must call it on a worker thread.

```cpp
xvc::RecordingClient client; // optional explicit base URL constructor
const auto current = client.state();
if (!current) { /* show Unknown; inspect current.status and current.error */ }
else {
    auto config = nlohmann::json{
        {"revision", current.state.at("revision")}, {"segment_seconds", 60},
        {"confirmed_source_change", false}, {"cameras", nlohmann::json::array()}
    };
    auto updated = client.apply(config);
}
```

`RecordingReply` is true only for a successful version-1 state response. `status` preserves HTTP failures (409 for conflict/validation, 503 for source failure), and is zero on transport/parsing failures. `error` contains device detail. Poll after an ambiguous request failure before inferring whether a mutation applied. Snapshot success does not imply that storage or every recording destination is healthy; inspect their actual state.

Use the latest configuration revision. Camera changes contain stable_id, enabled and optional profile (`h265`/`jpeg`, width, height, fps_n, fps_d); omit profile for selection-only updates. No more than four cameras may be enabled. Source changes require explicit confirmation and affect the source shared by PC and local recording. The application must finalize its affected PC file before applying, adopt the returned actual profile/assigned SRT port, then resume in a new file when healthy. Failure requires refreshing the actual device state before resumption.

`storage(operation, token)` accepts eject, resume, check or repair. Token fields are uuid, generation and confirmed. Device operations continue independently of client HTTP lifetime; the immediate response can remain busy. Poll for completion. Repair additionally requires the device POC gate and explicit user approval; the endpoint cannot format or select arbitrary devices.

Camera discovery adopts the backend stable identity, active native profile and SRT port. Destruction of a client receiver must not stop device capture. Match snapshots by stable identity; numeric device IDs are ephemeral across reconnects.

Set `THORVISION_DEVICE_HOST`/`THORVISION_DEVICE_PORT` before constructing clients for a LAN endpoint, or pass an explicit base URL for this API. Defaults are 192.168.177.100:8000. The matching xvc source includes a compiled `recording-client-smoke` consumer exercising real state/config/revision/repair responses and an opt-in real UI source-change test. See that repository's `docs/device_loop_recording.md`.
