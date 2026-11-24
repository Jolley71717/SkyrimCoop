#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>

// What a game decides about the NPCs it made as it ran that nobody commands (docs/DESIGN.md, "Run-time NPCs"): a quest's
// created alias, a world encounter, a script's PlaceAtMe. Each is said to the server with what made it (kNpcMades) before
// it is shared, and the server's answer (common/MadeBook.h) says whether this game runs it as its spawn (the others
// place a copy), or it is the twin of one a friend's game made first (this game shows that one through it: its own NPC
// is the copy, never deleted, its quest's), or there is no room yet. And a copy this game placed of a friend's made NPC is
// kept as this game's own when the friend's game lets it go (their player walked away, or left the session) while it
// stands here: the dragon a friend was fighting stays in the fight. The server's side is common/MadeBook.h; the game's is
// src/game/NpcSync.cpp. No game here: tests/MadeTests.cpp.
namespace MadeRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// One said and not answered (the answer lost with a connection that came back) is said again after this.
	inline constexpr auto ASK_AGAIN = 5s;
	// One the server had no room for, or refused, is said again after this.
	inline constexpr auto FULL_AGAIN = 10s;
	// What made a friend's NPC is kept this long after nobody runs it and no copy of it stands here.
	inline constexpr auto THEIRS_KEPT = 30s;
	// Our made NPCs followed at once (those not loaded are forgotten past it).
	inline constexpr std::size_t MAX_OURS = 256;

	enum class Phase : std::uint8_t
	{
		kNew,      // not said yet (or to be said again: its twin's maker let it go, we let go of our spawn)
		kAsked,    // said, no answer yet
		kShared,   // ours to run, as our spawn
		kTwin,     // it shows a friend's (twin)
		kWaiting,  // no room for it yet: said again at retryAt
	};

	[[nodiscard]] constexpr const char* PhaseName(Phase a_phase)
	{
		switch (a_phase) {
		case Phase::kNew:
			return "new";
		case Phase::kAsked:
			return "asked";
		case Phase::kShared:
			return "shared";
		case Phase::kTwin:
			return "twin";
		case Phase::kWaiting:
			return "waiting";
		}
		return "?";
	}

	// One NPC our game made that nobody commands.
	struct Ours
	{
		Protocol::MadeSource source;  // as last said
		Phase                phase = Phase::kNew;
		Clock::time_point    askedAt{};
		Clock::time_point    retryAt{};
		Protocol::NpcKey     twin;             // kTwin: the friend's NPC it shows
		bool                 adopted = false;  // a copy we placed of a friend's, ours since its maker let it go
		bool                 goneSaid = false;  // its end said (Gone)
	};

	// What the scan does with it: shares it as our spawn, leaves it (a copy of a friend's shows through it), or leaves it
	// for now (not answered yet, or no room).
	enum class Seen : std::uint8_t
	{
		kShare,
		kTwin,
		kWait,
	};

	[[nodiscard]] constexpr Seen SeenAs(const Ours& a_ours)
	{
		switch (a_ours.phase) {
		case Phase::kShared:
			return Seen::kShare;
		case Phase::kTwin:
			return Seen::kTwin;
		default:
			return Seen::kWait;
		}
	}

	// Whether it is said now. Only a living one is: a body pairs with nothing (a dead twin would kill nothing it shows).
	[[nodiscard]] inline bool SayNow(const Ours& a_ours, bool a_alive, Clock::time_point a_now)
	{
		if (!a_alive) {
			return false;
		}
		switch (a_ours.phase) {
		case Phase::kNew:
			return true;
		case Phase::kAsked:
			return a_now - a_ours.askedAt >= ASK_AGAIN;
		case Phase::kWaiting:
			return a_now >= a_ours.retryAt;
		default:
			return false;
		}
	}

	inline void Said(Ours& a_ours, const Protocol::MadeSource& a_source, Clock::time_point a_now)
	{
		a_ours.source = a_source;
		a_ours.phase = Phase::kAsked;
		a_ours.askedAt = a_now;
	}

	// The server's answer. False when it is no answer to what we asked (an old one, after we said it again): left as it is.
	inline bool Answered(Ours& a_ours, const Protocol::NpcMadeAnswer& a_answer, Clock::time_point a_now)
	{
		switch (a_answer.verdict) {
		case Protocol::MadeVerdict::kShared:
			if (a_ours.phase != Phase::kAsked && a_ours.phase != Phase::kShared) {
				return false;
			}
			a_ours.phase = Phase::kShared;
			a_ours.twin = {};
			return true;
		case Protocol::MadeVerdict::kTwin:
			// to our ask, or a merge of one we share (the server let it go: MadeBook::Merges)
			if (a_ours.phase != Phase::kAsked && a_ours.phase != Phase::kShared) {
				return false;
			}
			a_ours.phase = Phase::kTwin;
			a_ours.twin = a_answer.twin;
			return true;
		case Protocol::MadeVerdict::kFull:
		case Protocol::MadeVerdict::kRefused:
			if (a_ours.phase != Phase::kAsked) {
				return false;
			}
			a_ours.phase = Phase::kWaiting;
			a_ours.retryAt = a_now + FULL_AGAIN;
			return true;
		}
		return false;
	}

	// Said again: the friend's NPC it showed is nobody's now (their game let it go), or we let go of our spawn (unloaded).
	inline void Again(Ours& a_ours)
	{
		a_ours.phase = Phase::kNew;
		a_ours.twin = {};
	}

	// Its end is said (a made NPC of ours gone from our world: deleted or disabled, its quest done with it) when it was
	// shared, once: the friends' copies go with it, not kept as theirs.
	[[nodiscard]] constexpr bool SayGone(const Ours& a_ours)
	{
		return a_ours.phase == Phase::kShared && !a_ours.goneSaid;
	}

	// A copy of a friend's spawn, when nobody runs it any more.
	struct LetGoFacts
	{
		bool placed = false;  // a copy this game placed (else our own NPC shows it: a twin, or no actor at all)
		bool twin = false;    // our own NPC shows it
		bool known = false;   // its maker said what made it (kNpcMades), and has not said it is gone: no summon or mount
		bool there = false;   // its actor is in our world with its 3D
	};

	enum class LetGo : std::uint8_t
	{
		kTakeAway,  // a copy we placed: gone (as before: a summon goes with its caster)
		kAdopt,     // a copy we placed of a made NPC its maker let go of while it stands here: ours from now
		kUnpair,    // our own NPC showed it: its own again, said as ours
	};

	[[nodiscard]] constexpr LetGo OnLetGo(const LetGoFacts& a_facts)
	{
		if (a_facts.twin) {
			return LetGo::kUnpair;
		}
		return a_facts.placed && a_facts.known && a_facts.there ? LetGo::kAdopt : LetGo::kTakeAway;
	}

	// What an adopted copy is said as: what its maker said made it, where it stands here now, made of the NPC it was
	// placed from here.
	[[nodiscard]] inline Protocol::MadeSource Adopted(Protocol::MadeSource a_theirs, const Protocol::PlayerState& a_here, std::uint32_t a_pick)
	{
		a_theirs.flags = static_cast<std::uint8_t>(a_theirs.flags & ~Protocol::kMadeGone);
		a_theirs.cell = a_here.cell;
		a_theirs.worldspace = a_here.worldspace;
		a_theirs.x = a_here.x;
		a_theirs.y = a_here.y;
		a_theirs.z = a_here.z;
		if (a_pick != 0) {
			a_theirs.pick = a_pick;
		}
		return a_theirs;
	}
}
