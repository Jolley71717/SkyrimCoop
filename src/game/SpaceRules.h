#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

// Whether a friend's stand-in is in this game's world, and in the right cell of it (docs/DESIGN.md, "The other
// players": "Doors, interiors and worldspaces"): it leaves as its friend goes through a door, fast travels or loads, it
// arrives where they arrive, and none is left behind in a cell the game has let go or doubled. The game's side is
// src/game/RemotePlayers.cpp; this is the part with no game in it (tests/SpaceTests.cpp).
namespace SpaceRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// A stand-in is placed once its friend is this near (game units, about two exterior cells), and taken away past the
	// second: a stand-in in a cell this game has not loaded would stand on nothing.
	inline constexpr float SHOW_WITHIN = 8000.0f;
	inline constexpr float HIDE_BEYOND = 10000.0f;
	// A friend's status says a door, a fast travel or a loading screen is taking them away (Party.h): the stand-in goes
	// at once, not when the first state from the other side comes a load later. At most this long: a status that stays
	// travelling (a queued move the engine left set) hides no one for good; the states say where they are after it.
	inline constexpr auto TRAVEL_HIDES_FOR = 10s;
	// A stand-in that has had no 3D this long while it should be shown (the cell it is in was let go while its friend
	// walked on near us) is taken away and placed again where we are.
	inline constexpr auto NO_3D_FOR = 3s;
	// How often a stand-in outside is checked to be in the cell under it.
	inline constexpr auto CELL_CHECK_EVERY = 1s;

	enum class Why : std::uint8_t
	{
		kShown,
		kNotInWorld,  // this game is in a loading screen, or has no player in a cell
		kNoState,     // no state of the friend's for a while (their loading screen)
		kNoLooks,     // their looks have not come yet: no body to show
		kTravelling,  // their status says a door, a fast travel or a load is taking them away
		kElsewhere,   // another interior or worldspace
		kFar,         // past the range
	};

	[[nodiscard]] constexpr std::string_view WhyName(Why a_why)
	{
		switch (a_why) {
		case Why::kShown:
			return "shown";
		case Why::kNotInWorld:
			return "this game left the world";
		case Why::kNoState:
			return "no recent state";
		case Why::kNoLooks:
			return "no looks yet";
		case Why::kTravelling:
			return "travelling";
		case Why::kElsewhere:
			return "elsewhere";
		case Why::kFar:
			return "far";
		}
		return "?";
	}

	struct Seen
	{
		bool                           hereInWorld = false;
		bool                           haveState = false;
		bool                           haveLooks = false;
		bool                           sameSpace = false;
		float                          distance = 0.0f;
		bool                           placed = false;  // a stand-in is there now (the wider range keeps it)
		std::optional<Clock::duration> travellingFor;   // how long their status has said travelling (none: it doesn't)
	};

	// Whether a stand-in should be in the world now, and if not, why.
	[[nodiscard]] inline Why Judge(const Seen& a_seen)
	{
		if (!a_seen.hereInWorld) {
			return Why::kNotInWorld;
		}
		if (!a_seen.haveState) {
			return Why::kNoState;
		}
		if (!a_seen.haveLooks) {
			return Why::kNoLooks;
		}
		if (a_seen.travellingFor && *a_seen.travellingFor < TRAVEL_HIDES_FOR) {
			return Why::kTravelling;
		}
		if (!a_seen.sameSpace) {
			return Why::kElsewhere;
		}
		if (!(a_seen.distance < (a_seen.placed ? HIDE_BEYOND : SHOW_WITHIN))) {
			return Why::kFar;
		}
		return Why::kShown;
	}

	// Whether a stand-in outside is moved into the cell under it: it is placed in our player's cell and moved by its
	// position alone, which the engine does not take as a change of cell (an actor's AI does that, and a stand-in's never
	// runs), so its cell would be let go under it as we walk on. Only into a cell the game has loaded (attached): one
	// it has not would take its 3D with it.
	[[nodiscard]] constexpr bool Recell(bool a_interior, std::uint32_t a_parent, std::uint32_t a_under, bool a_underAttached)
	{
		return !a_interior && a_under != 0 && a_under != a_parent && a_underAttached;
	}

	// Watches a stand-in that should be drawn for its 3D: Tick says when it has had none for NO_3D_FOR.
	class Drawn
	{
	public:
		[[nodiscard]] bool Tick(bool a_has3D, Clock::time_point a_now)
		{
			if (a_has3D) {
				since.reset();
				return false;
			}
			if (!since) {
				since = a_now;
			}
			return a_now - *since >= NO_3D_FOR;
		}
		void Reset() { since.reset(); }

	private:
		std::optional<Clock::time_point> since;
	};
}
