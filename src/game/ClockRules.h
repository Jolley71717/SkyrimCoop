#pragma once

#include "Protocol.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

// The rules of the session's clock and weather (docs/DESIGN.md, "Time and weather"), free of the game so they are unit
// tested (tests/ClockTests.cpp); src/game/WorldClock.cpp reads and sets the game by them. Carried from the sister
// project's WorldClock and ClockMath, with what Skyrim and a review of that code added: the day count moves with the hour
// we set, a clock far behind across midnight lines up again rather than waiting for ever, the timescale follows, and a
// weather eases in rather than snapping once the two games already stand under the same sky.
//
// One game's clock is the session's (the server picks it: the host's game, else the player there longest). Each other
// game follows it:
// - Time of day. The first report lines the hours up: forward like waiting, or back within the same day, never back
//   across midnight (the date, the day count and every timer of the save would run backwards). From then on the follower
//   keeps the difference of the two day counts (each save has its own) and moves by what the owner's moved: a sleep or a
//   wait there is a move forward here, through each midnight one at a time (the game rolls the date over itself).
// - A follower ahead of the owner (its own wait, sleep or fast travel) is brought back within the day; one that is ahead
//   across midnight goes forward to the owner's hour on its next day, as waiting would.
// - The timescale is the owner's while following; the follower's own is put back when the session ends and while a save
//   is written, so nothing of the session goes into its save.
// - Weather, only outside, in the owner's worldspace: forced at once when this game comes into the owner's sky (a join,
//   stepping outside), eased in when the owner's weather turns (forced if it has not taken after WEATHER_PATIENCE). A
//   weather this game's own scripts forced (an override: a quest's storm) is left alone.
namespace ClockRules
{
	using Clock = std::chrono::steady_clock;

	// Every game says its clock this often, and sooner when something in it changed (Reporter).
	inline constexpr auto REPORT_EVERY = std::chrono::seconds{ 5 };
	// How often a game looks for a change worth saying at once.
	inline constexpr auto LOOK_EVERY = std::chrono::seconds{ 1 };
	// Game hours two clocks may be apart before the follower moves (15 game minutes: about 45 real seconds at the
	// default timescale of 20, so a report a few seconds old never moves anything).
	inline constexpr double MAX_HOUR_DRIFT = 0.25;
	// The owner's day count moved further than this since the last report (either way): it loaded another save, so the
	// hours are lined up again rather than followed.
	inline constexpr double MAX_FOLLOW_HOURS = 72.0;
	// A move forward stops here and lets the game roll the date over itself.
	inline constexpr float BEFORE_MIDNIGHT = 23.99f;
	// A weather eased in that has not taken by then is forced.
	inline constexpr auto WEATHER_PATIENCE = std::chrono::seconds{ 30 };
	// The owner's clock is followed this long after its last report: past it (the owner at its main menu, loading, gone
	// quiet) this game keeps its own timescale and weather until the owner speaks again.
	inline constexpr auto HEARD_LASTS = std::chrono::seconds{ 20 };
	// Two timescales closer than this are the same.
	inline constexpr float SAME_SCALE = 0.01f;

	// The signed shortest distance in hours from a_from to a_to on the 24-hour clock, in (-12, 12].
	[[nodiscard]] inline double HourGap(double a_to, double a_from)
	{
		double gap = std::fmod(a_to - a_from + 36.0, 24.0) - 12.0;
		return gap <= -12.0 ? gap + 24.0 : gap;
	}

	// Hours to move so the local clock reads the owner's time of day: forward, or back if that stays within the same
	// day (never back across midnight).
	[[nodiscard]] inline double LineUp(double a_ownerHour, double a_localHour)
	{
		double delta = std::fmod(a_ownerHour - a_localHour + 24.0, 24.0);
		if (delta > 12.0 && a_localHour - (24.0 - delta) >= 0.0) {
			delta -= 24.0;
		}
		return delta;
	}

	// A change this game makes to its clock: the hour to set, and how many days its day count moves with it (the hours
	// moved / 24, so the two stay together as the game's own passing of time keeps them).
	struct Set
	{
		float  hour = 0.0f;
		double days = 0.0;
	};

	// Follows the owner's time of day. Follow on each report heard while this game is in the world; Advance on each tick,
	// for a move forward that crosses midnight.
	class Follower
	{
	public:
		// The owner's report, and our hour and day count now: what to set at once, if anything. A move forward under way
		// finishes first (its report is taken again when the move is done).
		[[nodiscard]] std::optional<Set> Follow(const Protocol::ClockState& a_owner, float a_hour, double a_days)
		{
			if (advanceLeft > 0.0) {
				return std::nullopt;
			}
			double delta = 0.0;  // hours to move
			if (dayOffset && a_owner.playerId == owner) {
				delta = (a_owner.daysPassed + *dayOffset - a_days) * 24.0;
			}
			// The day offset keeps the date right across midnights, but goes stale when a clock is set by hand (a console
			// "set gamehour", a mod's script): then the hours themselves are what counts. A delta back further than our day
			// allows (the owner loaded an earlier save) would never be followed: it lines up again too.
			const bool stale = dayOffset && std::fabs(HourGap(a_owner.gameHour, a_hour + delta)) > MAX_HOUR_DRIFT;
			const bool unreachable = delta < -MAX_HOUR_DRIFT && a_hour + delta < 0.0;
			if (!dayOffset || a_owner.playerId != owner || std::fabs(delta) > MAX_FOLLOW_HOURS || stale || unreachable) {
				delta = LineUp(a_owner.gameHour, a_hour);
				dayOffset = a_days + delta / 24.0 - a_owner.daysPassed;
				owner = a_owner.playerId;
				++lineUps;
			}
			if (delta > MAX_HOUR_DRIFT) {
				advanceLeft = delta;
				return Advance(a_hour);
			}
			if (delta < -MAX_HOUR_DRIFT && a_hour + delta >= 0.0) {
				++backSets;
				return Set{ static_cast<float>(a_hour + delta), delta / 24.0 };
			}
			return std::nullopt;
		}

		// Each tick, with our hour now: the next part of a move forward, if one is under way. A move that reaches
		// midnight stops just before it (BEFORE_MIDNIGHT) and waits for the game to roll the date over (a game paused in a
		// menu rolls nothing), then goes on.
		[[nodiscard]] std::optional<Set> Advance(float a_hour)
		{
			if (advanceLeft <= 0.0) {
				return std::nullopt;
			}
			if (atMidnight) {
				if (a_hour >= BEFORE_MIDNIGHT) {
					return std::nullopt;  // not rolled over yet
				}
				atMidnight = false;
			}
			if (a_hour + advanceLeft < BEFORE_MIDNIGHT) {
				const auto step = advanceLeft;
				advanceLeft = 0.0;
				++hourSets;
				return Set{ static_cast<float>(a_hour + step), step / 24.0 };
			}
			// The rest of this day goes: the game passes the last moment to midnight itself.
			advanceLeft -= 24.0 - a_hour;
			atMidnight = true;
			++midnightSteps;
			if (a_hour >= BEFORE_MIDNIGHT) {
				return std::nullopt;
			}
			return Set{ BEFORE_MIDNIGHT, (BEFORE_MIDNIGHT - a_hour) / 24.0 };
		}

		// A save was loaded, or a new session: our day count no longer lines up with the owner's the way it did.
		void Rebaseline()
		{
			dayOffset.reset();
			advanceLeft = 0.0;
			atMidnight = false;
		}

		[[nodiscard]] bool                  Moving() const { return advanceLeft > 0.0; }
		[[nodiscard]] double                Left() const { return advanceLeft; }
		[[nodiscard]] std::optional<double> Offset() const { return dayOffset; }
		[[nodiscard]] std::uint32_t         Owner() const { return owner; }

		std::uint64_t lineUps = 0;        // the hours lined up (a first report, a new owner, a load, a clock set by hand)
		std::uint64_t hourSets = 0;       // moves forward within a day
		std::uint64_t backSets = 0;       // moves back within a day (we were ahead)
		std::uint64_t midnightSteps = 0;  // moves forward that stopped at midnight for the game to roll the date

	private:
		// Our day count minus the owner's, once lined up: each save has its own count.
		std::optional<double> dayOffset;
		std::uint32_t         owner = 0;
		double                advanceLeft = 0.0;  // hours still to move forward
		bool                  atMidnight = false;
	};

	// When a game says its clock: every REPORT_EVERY, and at once (looked at every LOOK_EVERY) when its weather,
	// worldspace or timescale changed or its clock jumped (a sleep, a wait, a console), so the others follow in a second
	// rather than five.
	class Reporter
	{
	public:
		// Whether to say a_ours now (call Sent when it is said). Looks for a change at most every LOOK_EVERY.
		[[nodiscard]] bool Due(Clock::time_point a_now, const Protocol::ClockState& a_ours)
		{
			if (!last) {
				return true;
			}
			if (a_now - lastAt >= REPORT_EVERY) {
				return true;
			}
			if (a_now - lastLook < LOOK_EVERY) {
				return false;
			}
			lastLook = a_now;
			if (a_ours.weather != last->weather || a_ours.worldspace != last->worldspace || std::fabs(a_ours.timeScale - last->timeScale) > SAME_SCALE) {
				return true;
			}
			// Where the clock would be had it only run since (game hours: real seconds * timescale / 3600). A game paused
			// in a menu runs behind it, and says so once it is out: harmless, at most one report a second.
			const double ran = std::chrono::duration<double>(a_now - lastAt).count() * last->timeScale / 3600.0;
			const double moved = (static_cast<double>(a_ours.daysPassed) - last->daysPassed) * 24.0;
			return std::fabs(HourGap(a_ours.gameHour, last->gameHour + ran)) > MAX_HOUR_DRIFT || std::fabs(moved - ran) > MAX_HOUR_DRIFT;
		}

		void Sent(Clock::time_point a_now, const Protocol::ClockState& a_ours)
		{
			last = a_ours;
			lastAt = lastLook = a_now;
		}

		void Reset() { last.reset(); }

	private:
		std::optional<Protocol::ClockState> last;
		Clock::time_point                   lastAt{};
		Clock::time_point                   lastLook{};
	};

	// This game's sky, as WeatherFollower needs it.
	struct Sky
	{
		bool          outside = false;     // in an exterior cell, under the full sky
		std::uint32_t worldspace = 0;      // 0 inside
		std::uint32_t weather = 0;         // the current weather (0 none)
		bool          overridden = false;  // a script of this game forced a weather (Sky::overrideWeather)
	};

	enum class WeatherMove
	{
		kNone,    // nothing to do: not under the owner's sky, already the same, or a change under way
		kEase,    // change to the owner's weather as the game changes weathers (Sky::SetWeather)
		kForce,   // the owner's weather, now (Sky::ForceWeather)
		kHeld,    // different, but this game's own scripts forced theirs: left alone
	};

	class WeatherFollower
	{
	public:
		[[nodiscard]] WeatherMove Decide(const Protocol::ClockState& a_owner, const Sky& a_here, Clock::time_point a_now)
		{
			if (!a_here.outside || a_here.worldspace == 0 || a_owner.worldspace == 0 || a_owner.weather == 0 || a_here.worldspace != a_owner.worldspace) {
				underSky = false;  // the next time under the owner's sky is an arrival
				asked.reset();
				return WeatherMove::kNone;
			}
			if (a_here.weather == a_owner.weather) {
				underSky = true;
				asked.reset();  // the next difference (the owner's turns, or ours does) eases in again
				return WeatherMove::kNone;
			}
			if (a_here.overridden) {
				underSky = true;
				++held;
				return WeatherMove::kHeld;
			}
			if (!underSky) {
				underSky = true;
				asked = Asked{ a_owner.weather, a_now };
				++forced;
				return WeatherMove::kForce;
			}
			if (!asked || asked->weather != a_owner.weather) {
				asked = Asked{ a_owner.weather, a_now };
				++eased;
				return WeatherMove::kEase;
			}
			if (a_now - asked->at >= WEATHER_PATIENCE) {
				asked = Asked{ a_owner.weather, a_now };
				++forced;
				return WeatherMove::kForce;
			}
			return WeatherMove::kNone;  // easing in
		}

		void Reset()
		{
			underSky = false;
			asked.reset();
		}

		std::uint64_t forced = 0;
		std::uint64_t eased = 0;
		std::uint64_t held = 0;  // decisions, not weathers: one a tick while held

	private:
		struct Asked
		{
			std::uint32_t     weather = 0;
			Clock::time_point at{};
		};
		bool                 underSky = false;
		std::optional<Asked> asked;
	};

	// The timescale while following: the owner's, with ours kept to put back.
	class ScaleKeeper
	{
	public:
		// The owner's timescale and ours now: the one to set, if any. The first call keeps ours as our own.
		[[nodiscard]] std::optional<float> Follow(float a_owner, float a_ours)
		{
			if (!own) {
				own = a_ours;
			}
			if (std::fabs(a_owner - a_ours) <= SAME_SCALE) {
				return std::nullopt;
			}
			++sets;
			return a_owner;
		}

		// Our own timescale, to set before a save is written (Follow sets the owner's again after it); none when we never
		// followed or it is what we have.
		[[nodiscard]] std::optional<float> Own(float a_ours) const
		{
			if (!own || std::fabs(*own - a_ours) <= SAME_SCALE) {
				return std::nullopt;
			}
			return own;
		}

		// The session ended: our own timescale to put back (Own), and nothing kept.
		[[nodiscard]] std::optional<float> Restore(float a_ours)
		{
			const auto back = Own(a_ours);
			own.reset();
			return back;
		}

		// A save was loaded: its timescale is our own from now on.
		void Forget() { own.reset(); }

		[[nodiscard]] std::optional<float> Kept() const { return own; }

		std::uint64_t sets = 0;

	private:
		std::optional<float> own;
	};
}
