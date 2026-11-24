#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The session's time of day, timescale and weather (docs/DESIGN.md, "Time and weather"; the rules, free of the game, are
// src/game/ClockRules.h). Every game says its clock every 5 s and at once on a change (kClock); the server passes on only
// the clock owner's (the host's game, else the player there longest), and every other game follows it: its hour (a
// sleep or a wait of the owner's is one here too; its own are undone within the day), its timescale, and outside in
// the owner's sky (its worldspace, or one on its climate: a walled city) its weather. Nothing is followed while this
// game's own Sleep/Wait runs. A game in the Helgen opening (ClockRules::OpeningHolds) keeps its own and says nothing,
// and its log says when that starts and ends.
//
// From the tick only (GameThread), and from SKSE's messages on the main thread (BeforeSave, AfterLoad).
namespace WorldClock
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// The session let us in as a_playerId: the owner's clock is lined up afresh.
	void Welcomed(std::uint32_t a_playerId);

	// Once a tick while in a session and let in: follows the owner's clock heard, and says ours when it is due.
	void Frame(const SendFn& a_send);

	// The owner's clock, from the session (kRemoteClock).
	void Heard(const Protocol::ClockState& a_clock);

	// A friend's weather magic, asked of this game as the clock owner (kRemoteWeatherAsk): set under our sky (held until
	// that friend says it ended, leaves, or ClockRules::MAGIC_HOLD_MAX), or let go.
	void HeardAsk(const Protocol::WeatherAsk& a_ask);

	// Watches our player's magic (TESMagicEffectApplyEvent): a weather it makes while this game follows the owner's sky
	// is asked of the owner. Once, when the game's data is loaded; false when the event source is missing.
	bool Install();

	// A player left: if they kept the clock, this game stops following it until the next owner speaks.
	void Left(std::uint32_t a_playerId);

	// A save is about to be written: our own timescale goes into it, not the owner's (set again on the next tick).
	void BeforeSave();

	// A save was loaded or a new game started: our day count and timescale are the save's; lined up again.
	void AfterLoad();

	// The session ended: our own timescale back, nothing followed.
	void Clear();

	// Developer channel: `clock` (one line: the hour, the day count, the timescale, the sky, the counts) and `clock pick
	// <clear|cloudy|rain|snow> [n]` (the n-th weather of the base game of that kind that is not the current one, for the
	// rig: both games pick the same), `clock weather <hex>` (this game's own weather changed, not as an override: one the
	// session's weather may undo) and `clock release` (any weather override let go).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
