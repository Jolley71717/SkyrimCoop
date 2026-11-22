#pragma once

#include <cstdint>

// Which versions of the game the plugin runs in. No game code here, so the rule is unit-tested; main.cpp asks it
// with the version SKSE reports.
//
// The project targets one version: the one a fresh Steam install has (1.7.104 on 2026-11-21, the maintainer's
// decision). The rest of the Anniversary Edition line loads with a warning; 1.5.97 and older are not supported for
// now and are refused with a clear line, rather than loaded into a game nobody tests.
namespace RuntimeSupport
{
	enum class Verdict
	{
		kRefused,   // not the Anniversary Edition line: 1.5.x and older, Skyrim VR (1.4.x)
		kOlder,     // an older build of the line: loads, with a warning in the log
		kNewer,     // a build newer than the tested one: loads, with a warning (SKSE and the Address Library decide first)
		kSupported, // the tested build
	};

	// 1.6.317: the first build of the Anniversary Edition line (the Address Library's "versionlib" files start there).
	inline constexpr std::uint16_t OLDEST[3] = { 1, 6, 317 };
	// 1.7.104: the build the project is tested on.
	inline constexpr std::uint16_t TESTED[3] = { 1, 7, 104 };

	namespace detail
	{
		constexpr int Compare(const std::uint16_t a_version[3], const std::uint16_t a_limit[3])
		{
			for (int i = 0; i < 3; ++i) {
				if (a_version[i] != a_limit[i]) {
					return a_version[i] < a_limit[i] ? -1 : 1;
				}
			}
			return 0;
		}
	}

	[[nodiscard]] constexpr Verdict Judge(std::uint16_t a_major, std::uint16_t a_minor, std::uint16_t a_patch)
	{
		const std::uint16_t version[3] = { a_major, a_minor, a_patch };
		if (detail::Compare(version, OLDEST) < 0) {
			return Verdict::kRefused;
		}
		const int tested = detail::Compare(version, TESTED);
		return tested < 0 ? Verdict::kOlder : tested > 0 ? Verdict::kNewer : Verdict::kSupported;
	}
}
