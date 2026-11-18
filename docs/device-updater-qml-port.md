# Device updater port from production qml

This branch starts directly at fetched production `origin/qml`
`79c19ec91c31e7c0c136d10dd5f40bbb06a1dbc5`. It adapts only the reviewed device updater
changes, without merging the experimental FrameLab/recording branch ancestry.
Existing branches, worktrees, dirty work and private FrameLab checkpoints remain
preserved. Nothing is pushed, tagged or published.

## Selected source

- `514cae798f97e6f3287967b7da2624bf22ebaf00`: device-aware catalog/artifact selection,
  application build identity, HTTPS and exact size/SHA-256 verification.
- `cb9f61f8616b7a0765e537070cf9d5607e71773e`: updater readiness and durable operation
  receipts bound to artifact identity, role and kind.
- `a25aaa8790670439e371aca1c2166bd9aca93e50`: reviewed extraction of those update
  files and standalone-compatible update test includes. No other modules from that
  checkpoint are imported.

The public additions are in `xdaqvc/update/modern.h`; existing update APIs remain.
Descriptor discovery separates API compatibility from installed application build.
Catalog entries are selected by AIO/NVR product, qualified profile and channel,
never generic ARM64 fallback. Unknown legacy descriptors retain empty product and
profile. The default plan refuses them; the existing explicit AIO migration
confirmation flag is the only opt-in, and the migration's hardware/fingerprint
gate remains mandatory. User approval is not inferred from legacy API version.

Readiness, package digest/role, receipt status and final build/device identity are
verified before reporting an update as complete. Capability/health fields read
from a device do not bundle FrameLab software or expose new application panels.
Ordinary migration/recovery qualification remains separate from parser fixtures.

## Production behavior preserved

No camera, stream/source endpoint, analytics/sidecar, recorder or GUI feature is
ported. `camera.cc/.h`, port allocation, `server.cc/.h`, websocket, `xvc.cc/.h` and
all existing camera/schema behavior remain production source. No new
capture/analytics/endpoint/recording modules or media fixtures are added.

The only non-update code change is the compiler portability spelling
`[] -> json_validator` → `[]() -> json_validator` in `validator.cc`. Untouched
production source fails with MSVC19.40 C2760 at this lambda; the parentheses change
neither schema nor runtime policy and already exists in the preserved original
dirty qml checkout. This exception is explicit rather than concealed in a broader
camera port.

Private tests exercise update parsing/planning, unknown legacy refusal and the
unchanged production camera/port fixtures. Hardware/update integration tests are
excluded. No desktop application, updater transfer, device or cloud operation is
executed. The separate FrameLab Windows/macOS setup distribution is not bundled
or modified by this work.

## Review corrections

Application verification now uses a pure health predicate: exact product/profile/
build and compatible installed API are mandatory. AIO always requires healthy
advertised FrameLab plus its runtime sandbox contract. NVR requires that contract
when its descriptor advertises FrameLab; otherwise identity and updater receipt
verification can succeed without requesting that endpoint. Missing or unhealthy
required capability data never becomes success. API incompatibility is a
VersionNotFound selection failure, not malformed cloud data.

Local production tags v0.1.0 through v0.1.3 all expose /version and /api_version
with the API version, and /build_version with the separate build marker. Legacy
device_type labels are ignored without assigning a product; modern conflicting
product labels still fail. Operation/verification failures do not prove rollback,
and fallback messages make no restoration claim.
