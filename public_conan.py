from pathlib import Path

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, get


class libxvc(ConanFile):
    name = "libxvc"
    version = "0.0.0"
    settings = "os", "compiler", "build_type", "arch"
    license = "LGPL-3.0-or-later"
    url = "https://github.com/kontex-neuro/libxvc.git"
    description = "Thor Vision Video Capture library (prebuilt)"
    # libxvc builds with BUILD_SHARED_LIBS=OFF and Boost_USE_STATIC_LIBS=ON.
    # This states what the build actually produces; it is not a choice made here.
    package_type = "static-library"
    build_policy = "missing"

    # GStreamer is a SYSTEM PREREQUISITE, not a Conan requirement.
    #
    # libxvc finds it with pkg_search_module(gstreamer-1.0>=1.4), and the public
    # API exposes it directly: xdaqvc/xvc.h includes <gst/gstpipeline.h> and
    # takes GstPipeline* arguments. There is no Conan package behind it, so it
    # cannot become self.requires(). Consumers must install GStreamer 1.4+
    # (runtime AND devel packages) themselves and expose it via PKG_CONFIG_PATH:
    #
    #   Windows: choco install pkgconfiglite, then install the MSVC gstreamer and
    #            gstreamer-devel MSIs, and set
    #            PKG_CONFIG_PATH=C:\Program Files\gstreamer\1.0\msvc_x86_64\lib\pkgconfig
    #   macOS:   install the universal gstreamer and gstreamer-devel PKGs, then set
    #            PKG_CONFIG_PATH=/Library/Frameworks/GStreamer.framework/Versions/1.0/lib/pkgconfig
    #
    # validate() below checks for it so a missing install fails with this
    # explanation instead of an unexplained include or link error.
    _gstreamer_minimum = "1.4"

    _supported = {
        # (Conan os, Conan arch): archive prefix produced by build_prebuilt.yml
        ("Windows", "x86_64"): "windows-x86_64",
        ("Macos", "armv8"): "macos-armv8",
    }

    def requirements(self):
        # boost: linked PUBLIC and genuinely public. xdaqvc/ws_client.h includes
        # <boost/beast/...> and <boost/asio/...>, and Beast/Asio types appear in
        # the class definition, so consumers compile against these headers.
        self.requires("boost/1.81.0", transitive_headers=True)

        # cpr: linked PRIVATE in CMake, but the installed libxvc-config.cmake
        # calls find_dependency(cpr). That call must resolve on the consumer's
        # machine regardless of linkage, so it belongs here. Dropping it makes
        # every consumer's find_package(libxvc) fail at configure time.
        self.requires("cpr/1.14.2")

        # Deliberately NOT declared:
        #   nlohmann_json  - linked PUBLIC, but no installed header includes it or
        #                    names its types; the public API passes JSON as
        #                    std::string_view, never as nlohmann::json.
        #   json-schema-validator, spdlog - PRIVATE, absent from public headers
        #                    and from find_dependency().
        #   xdaqmetadata, cli11 - BUILD_TOOLS only; not built into this package.
        #   catch2         - test_requires only.
        #   bcrypt         - a Windows SDK import library; nothing is redistributed.

    def validate(self):
        key = (str(self.settings.os), str(self.settings.arch))
        if key not in self._supported:
            raise ConanInvalidConfiguration(
                f"libxvc has no prebuilt archive for {key[0]}/{key[1]}. "
                f"Supported: {sorted(self._supported)}"
            )
        if str(self.settings.build_type) not in ("Release", "Debug"):
            raise ConanInvalidConfiguration(
                "libxvc prebuilt archives exist only for Release and Debug."
            )

    def system_requirements(self):
        # Fail early and legibly when the GStreamer devel install is missing,
        # rather than at the consumer's compile or link step.
        import shutil
        import subprocess

        if not shutil.which("pkg-config"):
            self.output.warning(
                "pkg-config was not found. libxvc's public headers include "
                "<gst/gstpipeline.h>; you need GStreamer "
                f">={self._gstreamer_minimum} and pkg-config to build against it."
            )
            return
        probe = subprocess.run(
            ["pkg-config", "--atleast-version", self._gstreamer_minimum, "gstreamer-1.0"],
            capture_output=True,
        )
        if probe.returncode != 0:
            self.output.warning(
                f"GStreamer >={self._gstreamer_minimum} was not found via pkg-config. "
                "libxvc is a prebuilt binary and cannot install it for you: install the "
                "GStreamer runtime and devel packages, then set PKG_CONFIG_PATH to the "
                "directory holding gstreamer-1.0.pc."
            )

    def build(self):
        # Must stay identical to the KONTEX_R2_PUBLIC_BASE_URL and
        # KONTEX_R2_PACKAGE_PREFIX repository variables. This recipe is
        # source-free and cannot read them at consume time.
        base_url = "https://dl-conan.kontex.io/libxvc"
        archive = "{}-{}-{}-{}.zip".format(
            str(self.settings.os).lower(),
            str(self.settings.arch).lower(),
            str(self.settings.build_type).lower(),
            self.version,
        )
        get(self, f"{base_url}/{archive}", strip_root=True)

    def package(self):
        for folder in ("bin", "include", "lib"):
            copy(self, "*", Path(self.build_folder, folder), Path(self.package_folder, folder))

    def package_info(self):
        self.cpp_info.libs = ["xvc"]
        self.cpp_info.set_property("cmake_file_name", "libxvc")
        self.cpp_info.set_property("cmake_target_name", "libxvc::xvc")
        # SHA-256 comes from platform crypto (ADR 0001): bcrypt on Windows,
        # CommonCrypto inside libSystem on macOS. Consumers of the static
        # library must link bcrypt themselves; macOS needs no entry.
        if self.settings.os == "Windows":
            self.cpp_info.system_libs.append("bcrypt")
