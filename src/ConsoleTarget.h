#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

// A console command's target, as the console writes it: "0001A2B3.kill" runs "kill" on the reference 0001A2B3.
// The game's compiler, run from the plugin with no selected reference, ignores such a prefix: the command "runs" and
// does nothing (the rig's locks, kills and fights were all lost that way). So the plugin splits it off, looks the
// reference up and passes it as the target. Game-free, so the unit tests cover it.
namespace ConsoleTarget
{
	struct Split
	{
		std::uint32_t    id = 0;  // 0: the command names no reference by its id
		std::string_view command;
	};

	// Splits a leading "<hex id>." off a command. The id is 1 to 8 hex digits with at least one decimal digit among
	// them, so an editor id made only of the letters a-f ("dead.kill") is left to the game's compiler, as is
	// "player.kill". Spaces before the id and after the dot are dropped.
	inline Split SplitTarget(std::string_view a_command)
	{
		std::size_t start = 0;
		while (start < a_command.size() && (a_command[start] == ' ' || a_command[start] == '\t')) {
			++start;
		}
		const std::string_view rest = a_command.substr(start);
		const std::size_t      dot = rest.find('.');
		if (dot == std::string_view::npos || dot == 0 || dot > 8) {
			return { 0, a_command };
		}
		std::uint32_t id = 0;
		bool          digit = false;
		for (std::size_t i = 0; i < dot; ++i) {
			const char c = rest[i];
			std::uint32_t v;
			if (c >= '0' && c <= '9') {
				v = static_cast<std::uint32_t>(c - '0');
				digit = true;
			} else if (c >= 'a' && c <= 'f') {
				v = static_cast<std::uint32_t>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				v = static_cast<std::uint32_t>(c - 'A' + 10);
			} else {
				return { 0, a_command };
			}
			id = (id << 4) | v;
		}
		std::string_view command = rest.substr(dot + 1);
		while (!command.empty() && (command.front() == ' ' || command.front() == '\t')) {
			command.remove_prefix(1);
		}
		if (!digit || id == 0 || command.empty()) {
			return { 0, a_command };
		}
		return { id, command };
	}
}
