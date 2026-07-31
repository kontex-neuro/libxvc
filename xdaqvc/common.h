#pragma once

#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

namespace xvc
{

class Version
{
public:
    constexpr Version() noexcept : _major(0), _minor(0), _patch(0) {}
    constexpr Version(int major, int minor, int patch) noexcept
        : _major(major), _minor(minor), _patch(patch)
    {
    }

    // Component access. Needed by the update API's compatibility policy, which gates on the
    // major.minor series and must compare components rather than whole versions (ADR 0007).
    // Additive; cannot break existing callers.
    [[nodiscard]] constexpr int major() const noexcept { return _major; }
    [[nodiscard]] constexpr int minor() const noexcept { return _minor; }
    [[nodiscard]] constexpr int patch() const noexcept { return _patch; }

    constexpr bool operator==(const Version &other) const noexcept
    {
        return _major == other._major && _minor == other._minor && _patch == other._patch;
    }
    constexpr bool operator>(const Version &other) const noexcept
    {
        return !(*this < other || *this == other);
    }
    constexpr bool operator<(const Version &other) const noexcept
    {
        if (_major != other._major) return _major < other._major;
        if (_minor != other._minor) return _minor < other._minor;
        return _patch < other._patch;
    }
    constexpr bool operator>=(const Version &other) const noexcept { return !(*this < other); }
    constexpr bool operator<=(const Version &other) const noexcept
    {
        return (*this < other) || (*this == other);
    };

    // Accepts "X.Y.Z" and "vX.Y.Z", normalizing the latter. The `v` prefix is not emitted by
    // the cloud (versions.json carries bare "0.1.2"), but is accepted at public API
    // boundaries per the update API spec. Widening what is accepted is additive and cannot
    // break existing callers.
    [[nodiscard]] static std::optional<Version> from_string(std::string_view version)
    {
        try {
            std::regex regex(R"(v?(\d+)\.(\d+)\.(\d+))");
            std::smatch matches;
            std::string version_str(version);

            if (std::regex_match(version_str, matches, regex)) {
                return Version{
                    std::stoi(matches[1].str()),
                    std::stoi(matches[2].str()),
                    std::stoi(matches[3].str())
                };
            }
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    };

    [[nodiscard]] std::string to_string() const
    {
        return std::format("{}.{}.{}", _major, _minor, _patch);
    };

private:
    int _major;
    int _minor;
    int _patch;
};

}  // namespace xvc
