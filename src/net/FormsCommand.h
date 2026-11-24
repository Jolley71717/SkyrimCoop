#pragma once

#include "FormIds.h"
#include "Redact.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

// The developer channel's `forms` command, without the game: what it asks, and the answers that need only the load
// orders (common/FormIds.h). The rig's `forms` step uses it to name one form by its plugin's file name and its number
// within it, to find that form in each game, and to check that each game's place for a plugin is what the step made it.
// Session.cpp gives it the session's mapper and this game's load order; unit-tested in tests/FormsCommandTests.cpp.
//
//   forms                        the session's edge: identity=, table=, oursOnly=, theirsOnly= and what was skipped
//   forms id <hex id>            one of our form IDs: its plugin, the session's ID for it, and back
//   forms find <plugin> <hex>    the form ID a plugin's form <hex> (its number within the plugin) has in this game
//   forms place <plugin>         the plugin's place in this game's load order (full:N, light:N or none)
//   forms armor <plugin>         the first armor of the plugin a player can wear (the game looks; Session.cpp)
namespace FormsCommand
{
	enum class Kind
	{
		kStatus,
		kId,
		kFind,
		kPlace,
		kArmor,
		kBad,
	};

	struct Query
	{
		Kind          kind = Kind::kBad;
		std::string   plugin;  // a plugin's file name, which may hold spaces
		std::uint32_t number = 0;
	};

	inline constexpr std::string_view USAGE = "error: usage: forms [id <hex id> | find <plugin> <hex local id> | place <plugin> | armor <plugin>]";

	[[nodiscard]] inline std::optional<std::uint32_t> ParseHex(std::string_view a_text)
	{
		if (a_text.starts_with("0x") || a_text.starts_with("0X")) {
			a_text.remove_prefix(2);
		}
		std::uint32_t value = 0;
		const auto    end = a_text.data() + a_text.size();
		const auto [at, error] = std::from_chars(a_text.data(), end, value, 16);
		if (a_text.empty() || error != std::errc{} || at != end) {
			return std::nullopt;
		}
		return value;
	}

	[[nodiscard]] inline std::string_view Trim(std::string_view a_text)
	{
		while (!a_text.empty() && a_text.front() == ' ') {
			a_text.remove_prefix(1);
		}
		while (!a_text.empty() && a_text.back() == ' ') {
			a_text.remove_suffix(1);
		}
		return a_text;
	}

	[[nodiscard]] inline Query Parse(std::string_view a_args)
	{
		a_args = Trim(a_args);
		const auto space = a_args.find(' ');
		const auto verb = a_args.substr(0, space);
		const auto rest = space == std::string_view::npos ? std::string_view{} : Trim(a_args.substr(space + 1));
		if (verb.empty() || verb == "status") {
			return rest.empty() ? Query{ Kind::kStatus } : Query{};
		}
		if (verb == "id") {
			const auto id = ParseHex(rest);
			return id ? Query{ Kind::kId, {}, *id } : Query{};
		}
		if (verb == "place" || verb == "armor") {
			return rest.empty() ? Query{} : Query{ verb == "place" ? Kind::kPlace : Kind::kArmor, std::string(rest) };
		}
		if (verb == "find") {
			// The number is the last word; the plugin is everything before it (a file name may hold spaces).
			const auto last = rest.rfind(' ');
			if (last == std::string_view::npos) {
				return {};
			}
			const auto number = ParseHex(rest.substr(last + 1));
			const auto plugin = Trim(rest.substr(0, last));
			return number && !plugin.empty() ? Query{ Kind::kFind, std::string(plugin), *number } : Query{};
		}
		return {};
	}

	[[nodiscard]] inline std::string Hex(std::optional<std::uint32_t> a_id)
	{
		return a_id ? std::format("{:08X}", *a_id) : std::string{ "none" };
	}

	// A plugin's name as a reply shows it: one line, an account number in it masked (Redact).
	[[nodiscard]] inline std::string Shown(std::string_view a_plugin)
	{
		return a_plugin.empty() ? std::string{ "none" } : Redact::Scrub(Redact::OneLine(a_plugin));
	}

	// forms id: a_id as this game numbers it, through a_mapper to the session's numbering and back.
	[[nodiscard]] inline std::string DescribeId(const FormIds::Mapper& a_mapper, const FormIds::Table& a_ours, std::uint32_t a_id)
	{
		const auto place = FormIds::PlaceOf(a_id);
		const auto name = place ? a_ours.At(*place) : nullptr;
		const auto session = a_mapper.ToSession(a_id);
		const auto back = session ? a_mapper.ToLocal(*session) : std::nullopt;
		return std::format("id={:08X} plugin={} local={:X} session={} back={}", a_id, Shown(name ? *name : std::string_view{}), FormIds::LocalPart(a_id), Hex(session),
			Hex(back));
	}

	// forms find: the form ID of a plugin's form a_local in this game.
	[[nodiscard]] inline std::string DescribeFind(const FormIds::Table& a_ours, std::string_view a_plugin, std::uint32_t a_local)
	{
		return std::format("id={} plugin={} local={:X}", Hex(a_ours.FormOf(a_plugin, a_local)), Shown(a_plugin), a_local);
	}

	// forms place: where this game loads a plugin.
	[[nodiscard]] inline std::string DescribePlace(const FormIds::Table& a_ours, std::string_view a_plugin)
	{
		const auto place = a_ours.Find(a_plugin);
		return std::format("place={} plugin={}", place ? std::format("{}:{}", place->light ? "light" : "full", place->index) : std::string{ "none" }, Shown(a_plugin));
	}
}
