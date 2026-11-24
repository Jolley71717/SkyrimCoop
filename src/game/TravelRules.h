#pragma once

#include "Protocol.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Fast travel together (docs/DESIGN.md, "Fast travel together"): the rule and the parts of it with no game in them. The
// game's side is src/game/TravelTogether.cpp; tests/TravelTests.cpp.
//
// The rule: anyone fast travels (or takes a carriage) on their own, as in single player; nobody is pulled along and
// nobody waits for a vote. A friend who was near the traveller when they left is offered to follow: a notice names the
// place and the key, for a minute, and the key fast travels them to the same marker by the game's own fast travel (time
// passes for them as for anyone who travels). Not while they fight, carry too much, are in a menu or a scene that took
// the controls, or the game has fast travel turned off.
namespace TravelRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// A friend this near the traveller's spot when they left (same interior, or same worldspace) is offered to follow.
	inline constexpr float FOLLOW_NEAR = 4000.0f;
	// How long an offer stands.
	inline constexpr auto OFFER_FOR = 60s;
	// A queued move this far (or into another space) is a trip, not a step; a load door's is told apart by its door.
	inline constexpr float TRIP_LEAST = 4000.0f;
	// A carriage (or a script) that moves the player with no fast travel marker: the trip goes to the map marker nearest
	// where they land, within this.
	inline constexpr float MARKER_NEAR = 6000.0f;
	// F7 (the keyboard scan code): the follow key unless SkyrimCoop.ini says another (iFollowKey; 0: none).
	inline constexpr std::uint32_t DEFAULT_FOLLOW_KEY = 0x41;

	// Whether our player, at a_here in a_cell / a_world, was near a trip's start.
	[[nodiscard]] inline bool Near(const Protocol::Trip& a_trip, std::uint32_t a_cell, std::uint32_t a_world, float a_x, float a_y, float a_z)
	{
		if (a_trip.fromCell == 0 && a_trip.fromWorld == 0) {
			return false;  // a place this game lacks
		}
		const bool interior = a_trip.fromWorld == 0;
		const bool same = interior ? a_trip.fromCell == a_cell && a_world == 0 : a_trip.fromWorld == a_world;
		return same && std::hypot(a_trip.x - a_x, a_trip.y - a_y, a_trip.z - a_z) < FOLLOW_NEAR;
	}

	// What the player's queued move is, as read the frame it is first seen.
	struct Queued
	{
		bool  valid = false;
		bool  marker = false;     // a fast travel marker is named (the map's fast travel, Game.FastTravel)
		bool  door = false;       // a load door near the player goes where it does
		bool  otherSpace = false;  // another interior or worldspace
		float distance = 0.0f;
	};

	enum class Start : std::uint8_t
	{
		kNone,    // no trip
		kMarker,  // to the marker it names
		kNearest,  // to the map marker nearest where it lands
	};

	// Whether a queued move first seen is a trip of ours to say, and where it goes: not a load door's, not a step.
	[[nodiscard]] constexpr Start TripOf(const Queued& a_queued)
	{
		if (!a_queued.valid || a_queued.door) {
			return Start::kNone;
		}
		if (a_queued.marker) {
			return Start::kMarker;
		}
		return a_queued.otherSpace || a_queued.distance >= TRIP_LEAST ? Start::kNearest : Start::kNone;
	}

	enum class Refusal : std::uint8_t
	{
		kNone,
		kNoOffer,     // none stands
		kCombat,      // in combat
		kWeight,      // over-encumbered
		kBusy,        // a menu, dialogue, or controls a scene took
		kTravelling,  // already on a trip
	};

	struct Now
	{
		bool offer = false;
		bool combat = false;
		bool overWeight = false;
		bool busy = false;
		bool travelling = false;
	};

	// Whether following now is refused, and why (the first that holds, in this order).
	[[nodiscard]] constexpr Refusal Refuse(const Now& a_now)
	{
		if (!a_now.offer) {
			return Refusal::kNoOffer;
		}
		if (a_now.travelling) {
			return Refusal::kTravelling;
		}
		if (a_now.busy) {
			return Refusal::kBusy;
		}
		if (a_now.combat) {
			return Refusal::kCombat;
		}
		if (a_now.overWeight) {
			return Refusal::kWeight;
		}
		return Refusal::kNone;
	}

	[[nodiscard]] constexpr std::string_view RefusalName(Refusal a_refusal)
	{
		switch (a_refusal) {
		case Refusal::kNone:
			return "none";
		case Refusal::kNoOffer:
			return "no offer";
		case Refusal::kCombat:
			return "combat";
		case Refusal::kWeight:
			return "weight";
		case Refusal::kBusy:
			return "busy";
		case Refusal::kTravelling:
			return "travelling";
		}
		return "?";
	}

	// The text key a refusal shows (none for no offer, or already on the way: nothing to say).
	[[nodiscard]] constexpr std::string_view RefusalText(Refusal a_refusal)
	{
		switch (a_refusal) {
		case Refusal::kCombat:
			return "travel.refused_combat";
		case Refusal::kWeight:
			return "travel.refused_weight";
		case Refusal::kBusy:
			return "travel.refused_now";
		default:
			return {};
		}
	}

	// The newest offer, standing for OFFER_FOR; a newer one replaces it, and taking it ends it.
	class Offers
	{
	public:
		void Take(const Protocol::Trip& a_trip, Clock::time_point a_now)
		{
			offer = a_trip;
			until = a_now + OFFER_FOR;
		}
		[[nodiscard]] std::optional<Protocol::Trip> Live(Clock::time_point a_now) const
		{
			if (offer && a_now < until) {
				return offer;
			}
			return std::nullopt;
		}
		void End() { offer.reset(); }

	private:
		std::optional<Protocol::Trip> offer;
		Clock::time_point             until{};
	};

	// A keyboard scan code's name as the keyboard shows it (the function keys, letters and digits); others by their number.
	[[nodiscard]] inline std::string KeyName(std::uint32_t a_code)
	{
		if (a_code >= 0x3B && a_code <= 0x44) {
			return "F" + std::to_string(a_code - 0x3B + 1);
		}
		if (a_code == 0x57 || a_code == 0x58) {
			return "F" + std::to_string(a_code - 0x57 + 11);
		}
		constexpr std::string_view rows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
		constexpr std::uint32_t    starts[] = { 0x02, 0x10, 0x1E, 0x2C };
		for (std::size_t i = 0; i < 4; ++i) {
			if (a_code >= starts[i] && a_code < starts[i] + rows[i].size()) {
				return std::string(1, rows[i][a_code - starts[i]]);
			}
		}
		return std::to_string(a_code);
	}
}
