#pragma once

#include <filesystem>
#include <vector>
#include <cstdint>
#include <optional>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sonata::pkg {
	
enum class ReleaseType
{
    Stable,
    Unstable,
	AnyLatest
};

class Version {
public:
    constexpr Version(
        std::uint32_t major = 0,
        std::uint32_t minor = 0,
        std::uint32_t patch = 0,
        ReleaseType releaseType = ReleaseType::Stable
    ) noexcept;

	explicit Version(std::string_view str);

    [[nodiscard]] constexpr std::uint32_t major() const noexcept;
    [[nodiscard]] constexpr std::uint32_t minor() const noexcept;
    [[nodiscard]] constexpr std::uint32_t patch() const noexcept;
    [[nodiscard]] constexpr ReleaseType releaseType() const noexcept;

    [[nodiscard]] static Version parse(std::string_view str);
    [[nodiscard]] std::string toString() const;

    friend constexpr bool operator==(const Version& lhs, const Version& rhs) noexcept;
    friend constexpr bool operator!=(const Version& lhs, const Version& rhs) noexcept;
    friend constexpr bool operator<(const Version& lhs, const Version& rhs) noexcept;
    friend constexpr bool operator<=(const Version& lhs, const Version& rhs) noexcept;
    friend constexpr bool operator>(const Version& lhs, const Version& rhs) noexcept;
    friend constexpr bool operator>=(const Version& lhs, const Version& rhs) noexcept;

private:
    std::uint32_t major_;
    std::uint32_t minor_;
    std::uint32_t patch_;
    ReleaseType releaseType_;
};

struct Source {
	std::string codeName;
	std::string displayName;
	std::string url;
	
	std::function<void(const Version&)> handler;
}

} // namespace sonata::pkg