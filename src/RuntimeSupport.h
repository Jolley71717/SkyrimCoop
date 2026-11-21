#pragma once

#include <cstdint>

// Which versions of the game the plugin runs in. No game code here, so the rule is unit-tested; main.cpp asks it
// with the version SKSE reports.
namespace RuntimeSupport
{
	enum class Verdict
	{
		kRefused,   // not Skyrim Special Edition (Skyrim VR reports 1.4.x), or older than the oldest build we load in
		kUntested,  // loads, with a warning in the log: nothing is tested there
		kSupported,
	};

	// 1.5.97: the last build before the Anniversary Edition update, still widely played with mods.
	inline constexpr std::uint16_t OLDEST[3] = { 1, 5, 97 };
	// 1.6.1170: the oldest build of the current Steam line the project means to test on.
	inline constexpr std::uint16_t TESTED_FROM[3] = { 1, 6, 1170 };

	namespace detail
	{
		constexpr bool Before(const std::uint16_t a_version[3], const std::uint16_t a_limit[3])
		{
			for (int i = 0; i < 3; ++i) {
				if (a_version[i] != a_limit[i]) {
					return a_version[i] < a_limit[i];
				}
			}
			return false;
		}
	}

	[[nodiscard]] constexpr Verdict Judge(std::uint16_t a_major, std::uint16_t a_minor, std::uint16_t a_patch)
	{
		const std::uint16_t version[3] = { a_major, a_minor, a_patch };
		if (detail::Before(version, OLDEST)) {
			return Verdict::kRefused;
		}
		return detail::Before(version, TESTED_FROM) ? Verdict::kUntested : Verdict::kSupported;
	}
}
