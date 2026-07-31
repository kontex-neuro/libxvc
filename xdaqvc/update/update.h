#pragma once

// Umbrella header for the libxvc device-update API (namespace xvc::update).
//
// The consuming application drives the update sequence itself -- fetch -> get device version
// -> plan -> download -> verify -> handshake -> prepare -> transfer -- because a single
// blocking update_device() cannot accommodate a confirm-then-download GUI with per-phase
// progress and a working cancel button (ADR 0006). This header exists so that driving the
// sequence still costs one include.
//
// Design decisions live in docs/decisions/. Three that commonly surprise readers:
//   * Update policy is compatibility-based, not latest-based: the app declares a required
//     major.minor series and plan_update() resolves the newest stable patch within it.
//     Downgrades into that series are normal. (ADR 0007)
//   * `platform` identifies the device being updated (linux-arm64), never the host running
//     libxvc. (ADR 0004)
//   * Downloaded artifacts are transient -- deleted on success, failure, and cancellation.
//     There is no cache; offline updates use use_local_artifact(). (ADR 0008)

#include "artifact.h"
#include "types.h"
