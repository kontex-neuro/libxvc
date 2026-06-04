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

    // TODO
    [[nodiscard]] static std::optional<Version> from_string(std::string_view version)
    {
        try {
            std::regex regex(R"((\d+)\.(\d+)\.(\d+))");
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
