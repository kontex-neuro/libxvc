# Conan prebuilt publishing

libxvc keeps its implementation source private. Tagged releases publish installed headers and binaries to Cloudflare R2, then publish a source-free download recipe to `kontex-conan`.

## One-time configuration

Install Python 3 and the GitHub CLI, then authenticate with `gh auth login`.

Copy the template and fill in the values:

```text
cp .github/scripts/conan_publishing.env.example .github/scripts/conan_publishing.env
```

Most values are Kontex-wide defaults already present in the template. Only the Cloudflare account ID and the three secrets must be supplied.

Then validate and apply:

```text
python .github/scripts/configure_conan_publishing.py --repo kontex-neuro/libxvc --dry-run
python .github/scripts/configure_conan_publishing.py --repo kontex-neuro/libxvc
```

`--dry-run` parses the file, checks credential shape, probes whether the PAT can push to the recipe index, and prints what would be set without changing anything. Secret values appear only as a character count. The real run asks for confirmation; `--yes` skips it.

`conan_publishing.env` is gitignored and holds live credentials. Delete it once the repository is configured.

`--from-env` reads the same keys from the environment instead, for CI or password-manager workflows.

| Key | Kind | Source |
| --- | --- | --- |
| `KONTEX_R2_ACCESS_KEY_ID` | secret | Cloudflare R2 API token, Object Read & Write. Exactly 32 characters. |
| `KONTEX_R2_SECRET_ACCESS_KEY` | secret | Same token, shown once. Exactly 64 characters. |
| `KONTEX_CONAN_PAT` | secret | GitHub PAT with `Contents: Read and write` on `kontex-conan` |
| `KONTEX_R2_ACCOUNT_ID` | variable | Cloudflare account ID |
| `KONTEX_R2_BUCKET` | variable | `kontex-conan` |
| `KONTEX_R2_PUBLIC_BASE_URL` | variable | `https://dl-conan.kontex.io` |
| `KONTEX_R2_PACKAGE_PREFIX` | variable | `libxvc` |
| `KONTEX_CONAN_REPOSITORY` | variable | `kontex-neuro/kontex-conan` |

Public access must be enabled on the R2 bucket. The release workflow verifies every archive over an unauthenticated request, so a private bucket fails the release after uploading.

`KONTEX_R2_PUBLIC_BASE_URL` and `KONTEX_R2_PACKAGE_PREFIX` are also hardcoded in `public_conan.py` as `https://dl-conan.kontex.io/libxvc`. The recipe is source-free and cannot read repository variables, so changing either variable requires editing the recipe to match.

### Recipe index token

A fine-grained token needs all of:

- Resource owner: the organization
- Repository access: `kontex-conan`
- Repository permissions: **Contents: Read and write**
- Organization approval, where the org requires it

A token without write permission still reads successfully, so checkout succeeds and only the final push fails. The setup script probes for this during configuration.

## Release

1. Update the version in `conanfile.py` and `CMakeLists.txt` (`libxvc_VERSION`). Both must agree.
2. Commit and push the reviewed change.
3. Create and push the matching tag: `git tag vX.Y.Z` and `git push origin vX.Y.Z`.
4. Verify matrix builds, R2 publication, public URL checks, and `kontex-conan` publication.

Only strict `vX.Y.Z` tags publish. A tag differing from the internal recipe version fails in `validate` before anything is built.

Before the first release, or after changing the matrix, run **Build prebuilt Conan archives** manually via `workflow_dispatch`. It exercises every configuration without touching R2. This matters here in particular: CI has only ever built `Release`, so the dry run is the first real exercise of the `Debug` configuration.

## Supported configurations

| Runner | Conan profile | Conan os/arch | Build types | Archive names |
| --- | --- | --- | --- | --- |
| `windows-2022` | `windows-2022-x64` | `Windows` / `x86_64` | Release, Debug | `windows-x86_64-{release,debug}-<version>.zip` |
| `macos-15` | `macos-15-armv8` | `Macos` / `armv8` | Release, Debug | `macos-armv8-{release,debug}-<version>.zip` |

Removing a build type from the matrix requires making `public_conan.py` reject it in `validate()`, or consumers resolve a recipe whose download 404s.

libxvc builds static (`BUILD_SHARED_LIBS=OFF`, `Boost_USE_STATIC_LIBS=ON`); the recipe declares `package_type = "static-library"` to describe that, not to select it.

## Consumer prerequisite: GStreamer

**GStreamer is not a Conan dependency and cannot be one.** libxvc locates it with `pkg_search_module(gstreamer-1.0>=1.4)`, and it is part of the public API: `xdaqvc/xvc.h` includes `<gst/gstpipeline.h>` and takes `GstPipeline*` arguments.

Consumers must install the GStreamer **runtime and devel** packages (1.4+; CI builds against 1.26.8) and expose them via `PKG_CONFIG_PATH`:

- **Windows** — `choco install pkgconfiglite`, install the MSVC `gstreamer` and `gstreamer-devel` MSIs, then set
  `PKG_CONFIG_PATH=C:\Program Files\gstreamer\1.0\msvc_x86_64\lib\pkgconfig`
- **macOS** — install the universal `gstreamer` and `gstreamer-devel` PKGs, then set
  `PKG_CONFIG_PATH=/Library/Frameworks/GStreamer.framework/Versions/1.0/lib/pkgconfig`

`public_conan.py` warns during `system_requirements()` when pkg-config cannot find it, so a missing install reports this rather than an unexplained include or link error.

## Public package contract

CMake target `libxvc::xvc`, library `xvc`, headers under `include/xdaqvc/` (flat headers plus `include/xdaqvc/update/`).

Declared public Conan requirements:

| Dependency | Why it is public |
| --- | --- |
| `boost/1.81.0` | Linked `PUBLIC`; `xdaqvc/ws_client.h` includes Boost.Beast and Boost.Asio headers and exposes their types in the class definition. Declared with `transitive_headers=True`. |
| `cpr/1.14.2` | Linked `PRIVATE`, but the installed `libxvc-config.cmake` calls `find_dependency(cpr)`, which must resolve for consumers regardless of linkage. |

Deliberately excluded:

| Dependency | Why it is excluded |
| --- | --- |
| `nlohmann_json` | Linked `PUBLIC`, but no installed header includes it or names its types; the public API passes JSON as `std::string_view`. |
| `json-schema-validator`, `spdlog` | `PRIVATE`, absent from public headers and from `find_dependency()`. |
| `xdaqmetadata`, `cli11` | `BUILD_TOOLS` only; not built into the published package. |
| `catch2` | `test_requires` only. |
| `bcrypt` | Windows SDK import library, exposed as `cpp_info.system_libs`; nothing is redistributed (ADR 0001). |

## Reruns and recovery

**Published versions are permanent.** Once archives exist in R2 for a version, that version's content is fixed. Correct a released version by releasing the next patch version; never replace an archive or rewrite a recipe, and never move a tag that has already published.

The recipe is published only after all archives are publicly verified. If binaries publish but recipe publication fails, the release is inert — nothing resolves without a recipe, so there is no urgency to clean it up.

Choose recovery by what changed:

- **Credentials or settings only, nothing in the repo changed.** Re-run the failed job from the Actions UI. Uploads are idempotent and identical recipes are accepted, so the same tag completes safely.
- **Workflow, scripts, or sources changed.** The Actions UI re-runs the workflow as it existed at the tagged commit, so it will not pick up the fix. Bump the version, commit, and tag the next patch version.
