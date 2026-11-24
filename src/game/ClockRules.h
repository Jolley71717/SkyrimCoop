#pragma once

#include "Protocol.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_set>

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
// - Nothing moves while this game's own Sleep/Wait runs (WaitGate); its wait is undone once it ends.
// - Weather, only outside, under the owner's sky (the same worldspace, or one on its climate: a walled city and the land
//   round it are one sky, SkyWorld): forced at once when this game comes into the owner's sky (a join,
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
			Drop();
		}

		// This game passed time itself (its own sleep or wait, WaitGate): a move forward under way is dropped, and the next
		// report is followed from where the wait left us, by the day offset kept.
		void Drop()
		{
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

	// This game's own Sleep/Wait: the engine passes the hours itself while its menu stays open (a sleep in a bed, a wait
	// anywhere), and ends the wait by the clock it reads. Nothing of ours moves the clock under it: while the menu is open
	// the owner's clock is not followed (its last report is kept), and once it shuts any move under way is dropped
	// (Follower::Drop) and the owner's clock is lined up from where the wait left us: a wait of a friend's is undone after
	// it, never during it. The game's other ways of passing time (fast travel, a carriage, jail, training) jump the clock
	// at once with no menu running, and are followed as any other jump is.
	enum class WaitStep
	{
		kRun,   // no wait of ours: follow as usual
		kHold,  // a wait of ours runs: follow nothing
		kShut,  // a wait of ours just ended: drop any move, follow the owner's last report again
	};

	class WaitGate
	{
	public:
		// Once a tick, with whether the Sleep/Wait menu is open now.
		[[nodiscard]] WaitStep Next(bool a_menuOpen)
		{
			if (a_menuOpen) {
				if (!open) {
					open = true;
					++waits;
				}
				return WaitStep::kHold;
			}
			if (open) {
				open = false;
				return WaitStep::kShut;
			}
			return WaitStep::kRun;
		}

		[[nodiscard]] bool Open() const { return open; }

		// A load or a new session: no wait of ours runs.
		void Reset() { open = false; }

		std::uint64_t waits = 0;  // waits of ours the gate held through

	private:
		bool open = false;
	};

	// Whether the Helgen opening holds this game's clock (its own time and weather, nothing said): its quest (MQ101)
	// runs, is not done, and has begun in this game: a stage of it set, or the player's movement taken from them (the
	// cart ride). A game put in the world another way (the console's coc from the main menu, an alternate start) that
	// leaves the quest running untouched at its first stage, with the player free to move, is no opening.
	[[nodiscard]] constexpr bool OpeningHolds(bool a_running, bool a_completed, std::uint16_t a_stage, bool a_canMove)
	{
		return a_running && !a_completed && (a_stage > 0 || !a_canMove);
	}

	// Parents walked at most, for a worldspace whose parents loop (a broken mod).
	inline constexpr int MAX_SKY_PARENTS = 8;

	// The worldspace whose sky a worldspace has: a_world itself, or the parent whose climate it uses, walked up. The walled
	// cities (Whiterun, Solitude, Windhelm, Riften, Markarth) are child worldspaces of Tamriel on its climate, and so is
	// any mod's world made the same way: one sky with their parent, so a player inside the walls and one on the plains
	// outside share a weather. a_parentSky(world) gives the parent whose climate that world uses, or none (World{}). World
	// is a form ID (the tests) or a worldspace pointer (the game).
	template <class World, class ParentSky>
	[[nodiscard]] World SkyWorld(World a_world, ParentSky&& a_parentSky)
	{
		for (int i = 0; a_world != World{} && i < MAX_SKY_PARENTS; ++i) {
			const World parent = a_parentSky(a_world);
			if (parent == World{} || parent == a_world) {
				break;
			}
			a_world = parent;
		}
		return a_world;
	}

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

	// This game's sky, as WeatherFollower, MagicWatch and AskKeeper need it.
	struct Sky
	{
		bool          outside = false;     // in an exterior cell, under the full sky
		std::uint32_t worldspace = 0;      // the worldspace whose sky this is (SkyWorld: a walled city's is its parent's); 0 inside
		std::uint32_t weather = 0;         // the current weather (0 none)
		bool          overridden = false;  // a script of this game forced a weather (Sky::overrideWeather, and not our pin)
		bool          pinned = false;      // the override is our pin: the owner's weather, held against this game's climate
		std::uint32_t overrideWeather = 0; // the weather any override holds (ours or a script's), 0 none
	};

	// While following, this game holds the owner's weather as an override of its own (the pin), so its climate never
	// rolls a weather of its own between two of the owner's reports (an override is what the game's scripts use to hold
	// a weather: the climate leaves it alone). A script of this game that forces its own override replaces the pin (that
	// weather is held, as before); when it lets go, the owner's is pinned again. The pin is let go when this game is no
	// longer under the owner's sky, when the owner goes quiet or leaves, and when the session ends, so no game is left
	// holding a weather no script will ever release; it is taken off (quietly: no release, so the climate does not take
	// over) while a save is written, so no save holds it, and put back on the next tick.
	enum class WeatherMove
	{
		kNone,     // nothing to do: not under the owner's sky (and nothing pinned), already pinned, or a change under way
		kEase,     // change to the owner's weather as the game changes weathers (Sky::SetWeather), pinned
		kForce,    // the owner's weather, now (Sky::ForceWeather), pinned
		kHeld,     // different, but this game's own scripts forced theirs: left alone
		kPin,      // the owner's weather already, not held: pin it (the override set to the current weather, no transition)
		kRelease,  // our pin, and no longer under the owner's sky: let it go (the game's climate takes over)
	};

	class WeatherFollower
	{
	public:
		[[nodiscard]] WeatherMove Decide(const Protocol::ClockState& a_owner, const Sky& a_here, Clock::time_point a_now)
		{
			if (!a_here.outside || a_here.worldspace == 0 || a_owner.worldspace == 0 || a_owner.weather == 0 || a_here.worldspace != a_owner.worldspace) {
				underSky = false;  // the next time under the owner's sky is an arrival
				asked.reset();
				if (a_here.pinned) {
					++releases;
					return WeatherMove::kRelease;
				}
				return WeatherMove::kNone;
			}
			if (a_here.weather == a_owner.weather) {
				underSky = true;
				asked.reset();  // the next difference (the owner's turns, or ours does) eases in again
				if (!a_here.pinned && !a_here.overridden) {
					++pins;
					return WeatherMove::kPin;
				}
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
		std::uint64_t held = 0;      // decisions, not weathers: one a tick while held
		std::uint64_t pins = 0;      // the owner's weather pinned when it was already ours (after a save, a script's release)
		std::uint64_t releases = 0;  // our pin let go on leaving the owner's sky

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

	// ---- a friend's weather magic (Clear Skies, Storm Call, Auriel's sun, a mod's weather spell) ----
	//
	// The game's weather magic is a magic effect's script on its caster: Clear Skies' ClearSkiesEffectScript holds a clear
	// weather as an override and lets go when the effect ends; Storm Call's magicSetWeatherScript sets its storm, held as
	// an override until the effect ends or not (bHoldWeatherUntilEnd); Dawnguard's sun arrow lets any override go, then
	// sets its own the same way (a scan of every script of the base game for Weather.SetActive, ForceActive and
	// ReleaseOverride: the magic ones are these three, all on an ActiveMagicEffect of the caster). The class is told by
	// what it does, never by a name: a weather change in this game soon after our player's own magic took hold on our
	// player (MagicKinds), while we follow the owner's sky. Such a weather is asked of the owner, whose game sets it (and
	// lets it go when ours does), so it is every game's sky; meanwhile this game leaves its own sky to the magic.

	// Which magic opens the watch. A weather spell's script runs on an effect its caster cast on itself (Clear Skies, Storm
	// Call, the sun arrow, a mod's), so only an effect our player cast on our player opens it: never one on someone else
	// (an enchanted weapon's hit, a spell at an enemy: a fight would keep the window open and our sky unfollowed), and
	// never one that only potions, food, poisons and ingredients carry (a drink in a fight). An effect that a spell, a
	// shout, a power, a scroll or an enchantment also carries opens it, whatever its kind, so no weather spell of any mod
	// is missed.
	class MagicKinds
	{
	public:
		// While the game's data is read (once, before any event): the effects an alchemy item carries, and those anything
		// else carries.
		void Alchemy(std::uint32_t a_effect)
		{
			if (a_effect != 0 && !other.contains(a_effect)) {
				alchemy.insert(a_effect);
			}
		}
		void Other(std::uint32_t a_effect)
		{
			if (a_effect != 0) {
				other.insert(a_effect);
				alchemy.erase(a_effect);
			}
		}

		// A magic effect took hold (TESMagicEffectApplyEvent): whether it opens the watch.
		[[nodiscard]] bool Opens(bool a_casterIsUs, bool a_targetIsUs, std::uint32_t a_effect) const
		{
			return a_casterIsUs && a_targetIsUs && !alchemy.contains(a_effect);
		}

		[[nodiscard]] std::size_t AlchemyOnly() const { return alchemy.size(); }

	private:
		std::unordered_set<std::uint32_t> alchemy;  // carried by alchemy items only
		std::unordered_set<std::uint32_t> other;
	};

	// After our player's magic takes hold, a weather change here within this long is its (a script runs a few frames,
	// or under load a few seconds, after its effect starts).
	inline constexpr auto MAGIC_WINDOW = std::chrono::seconds{ 5 };
	// A weather magic not held (an override's end never comes): this game leaves its sky alone until the owner's report
	// carries it, or this long.
	inline constexpr auto MAGIC_ANSWER = std::chrono::seconds{ 10 };
	// A held weather magic never let go (a mod's effect that never ends): let go after this, here and at the owner.
	inline constexpr auto MAGIC_HOLD_MAX = std::chrono::minutes{ 30 };

	// What MagicWatch says to do this tick.
	struct MagicStep
	{
		enum class Kind
		{
			kNone,
			kAsk,      // ask the owner for this weather (Protocol::WeatherAsk)
			kRelease,  // our held weather magic ended: the owner lets its go
		};
		Kind          kind = Kind::kNone;
		std::uint32_t worldspace = 0;
		std::uint32_t weather = 0;
		bool          held = false;  // an override (it ends when the magic does), or a weather the climate takes over from
	};

	class MagicWatch
	{
	public:
		// Our player's magic took hold (TESMagicEffectApplyEvent, caster our player), seen this tick, with the worldspace of
		// the owner's sky (0 none heard). The sky before it is the one Seen last noted (the script may already have run).
		void Cast(Clock::time_point a_now, std::uint32_t a_ownerWorldspace)
		{
			if (!lastSeen || !lastSeen->outside || lastSeen->worldspace == 0 || lastSeen->worldspace != a_ownerWorldspace) {
				return;  // inside, not under the owner's sky, or nothing seen yet: our magic's weather is our own there
			}
			++windows;
			windowUntil = a_now + MAGIC_WINDOW;
			if (state == State::kIdle || state == State::kWindow) {
				state = State::kWindow;
				before = *lastSeen;
			}
		}

		// Each tick, with our sky now and the owner's weather heard (0 none): what to do. Seen is called after, once this
		// game's own changes of the tick are made.
		[[nodiscard]] MagicStep Look(Clock::time_point a_now, const Sky& a_here, std::uint32_t a_ownerWeather)
		{
			switch (state) {
			case State::kIdle:
				return {};
			case State::kWindow:
				if (a_here.outside && a_here.worldspace == before.worldspace && Changed(a_here)) {
					return Ask(a_now, a_here);
				}
				if (a_now >= windowUntil) {
					state = State::kIdle;
				}
				return {};
			case State::kAskedHeld:
				if (!a_here.outside || a_here.worldspace != asked.worldspace || a_here.overrideWeather == 0 || a_here.pinned ||
					a_now - askedAt >= MAGIC_HOLD_MAX) {
					return Release();  // let go (its effect ended), out from under that sky, or held too long
				}
				if (a_here.overrideWeather != asked.weather) {
					return Ask(a_now, a_here);  // another magic's (a second shout): it replaces ours at the owner
				}
				return {};
			case State::kAskedOnce:
				if (a_now < windowUntil && a_here.outside && a_here.worldspace == asked.worldspace && a_here.overridden && a_here.overrideWeather != asked.weather) {
					return Ask(a_now, a_here);  // the same magic held it after all (the sun arrow: release, then set)
				}
				if (a_ownerWeather == asked.weather || a_now - askedAt >= MAGIC_ANSWER || !a_here.outside || a_here.worldspace != asked.worldspace) {
					state = State::kIdle;
				}
				return {};
			}
			return {};
		}

		// The sky as this tick leaves it (after our own changes): what the next cast is compared with.
		void Seen(const Sky& a_here) { lastSeen = a_here; }

		// This game leaves its sky to our magic (follows nothing, pins nothing): a window open or an ask under way.
		[[nodiscard]] bool Holds() const { return state != State::kIdle; }

		// A held weather magic of ours asked and not yet let go (the session ending says so to the owner).
		[[nodiscard]] std::optional<std::uint32_t> Asked() const
		{
			return state == State::kAskedHeld ? std::optional<std::uint32_t>{ asked.weather } : std::nullopt;
		}

		// A load, a new session, the owner gone: nothing of ours is asked any more.
		void Reset()
		{
			state = State::kIdle;
			lastSeen.reset();
		}

		std::uint64_t windows = 0;   // casts seen while outside
		std::uint64_t asks = 0;      // weathers asked of the owner
		std::uint64_t releases = 0;  // held ones let go

	private:
		enum class State
		{
			kIdle,
			kWindow,     // our magic took hold: a change of sky now is its
			kAskedHeld,  // asked, held as an override here until the magic lets go
			kAskedOnce,  // asked, not held: left alone until the owner's sky has it
		};

		// The sky changed since the cast, by something not ours: the weather, or an override of a script's.
		[[nodiscard]] bool Changed(const Sky& a_here) const
		{
			if (a_here.overridden && a_here.overrideWeather != 0 && a_here.overrideWeather != before.overrideWeather) {
				return true;
			}
			return a_here.weather != 0 && a_here.weather != before.weather && !a_here.pinned;
		}

		MagicStep Ask(Clock::time_point a_now, const Sky& a_here)
		{
			const bool held = a_here.overridden && a_here.overrideWeather != 0;
			asked = MagicStep{ MagicStep::Kind::kAsk, a_here.worldspace, held ? a_here.overrideWeather : a_here.weather, held };
			askedAt = a_now;
			state = held ? State::kAskedHeld : State::kAskedOnce;
			++asks;
			return asked;
		}

		MagicStep Release()
		{
			state = State::kIdle;
			++releases;
			return MagicStep{ MagicStep::Kind::kRelease, asked.worldspace, asked.weather, true };
		}

		State                state = State::kIdle;
		std::optional<Sky>   lastSeen;
		Sky                  before;
		Clock::time_point    windowUntil{};
		MagicStep            asked;
		Clock::time_point    askedAt{};
	};

	// The owner's side: a friend's weather magic asked (kRemoteWeatherAsk). The owner's game sets it as the magic did
	// (held as an override, or not), under the same sky only (a friend's magic in a worldspace the owner is not under is
	// that friend's own: nothing is followed there). A held one is let go when its friend says the magic ended, when that
	// friend leaves, after MAGIC_HOLD_MAX, and when the session ends; it is never written into the owner's save (no
	// script of the owner's would ever let it go after a load): taken off quietly for the save, put back after it.
	struct AskMove
	{
		enum class Kind
		{
			kNone,
			kSet,      // set this weather (held: as an override), accelerated as a spell's is
			kRelease,  // let our held asked weather go (Sky::ReleaseWeatherOverride)
			kIgnore,   // not under that sky: nothing set
		};
		Kind          kind = Kind::kNone;
		std::uint32_t weather = 0;
		bool          held = false;
	};

	class AskKeeper
	{
	public:
		// A friend's ask, with the owner's sky now.
		[[nodiscard]] AskMove Heard(const Protocol::WeatherAsk& a_ask, const Sky& a_here, Clock::time_point a_now)
		{
			if (a_ask.release) {
				return Drop(a_ask.playerId, a_here);
			}
			if (!a_here.outside || a_here.worldspace == 0 || a_here.worldspace != a_ask.worldspace || a_ask.weather == 0) {
				++ignored;
				return AskMove{ AskMove::Kind::kIgnore };
			}
			++sets;
			if (a_ask.held) {
				kept = Kept{ a_ask.playerId, a_ask.weather, a_now };
			} else if (kept) {
				kept.reset();  // a weather not held replaces the held one: the climate goes on from it
			}
			return AskMove{ AskMove::Kind::kSet, a_ask.weather, a_ask.held };
		}

		// The friend who asked left.
		[[nodiscard]] AskMove Left(std::uint32_t a_playerId, const Sky& a_here) { return Drop(a_playerId, a_here); }

		// Each tick: a held ask given up past MAGIC_HOLD_MAX, or forgotten when a script of ours replaced or let go of it.
		[[nodiscard]] AskMove Tick(Clock::time_point a_now, const Sky& a_here)
		{
			if (!kept) {
				return {};
			}
			if (a_here.overrideWeather != kept->weather) {
				kept.reset();  // a script of this game's took the sky (or let it go): nothing of the friend's to let go
				return {};
			}
			if (a_now - kept->at >= MAGIC_HOLD_MAX) {
				return Drop(kept->playerId, a_here);
			}
			return {};
		}

		// The weather a friend's magic holds here (0 none), and whose.
		[[nodiscard]] std::uint32_t Weather() const { return kept ? kept->weather : 0; }
		[[nodiscard]] std::uint32_t By() const { return kept ? kept->playerId : 0; }

		// A load, a new game, the session's end (after letting any go): nothing asked is kept.
		void Forget() { kept.reset(); }

		std::uint64_t sets = 0;
		std::uint64_t ignored = 0;
		std::uint64_t releases = 0;

	private:
		struct Kept
		{
			std::uint32_t     playerId = 0;
			std::uint32_t     weather = 0;
			Clock::time_point at{};
		};

		AskMove Drop(std::uint32_t a_playerId, const Sky& a_here)
		{
			if (!kept || kept->playerId != a_playerId) {
				return {};
			}
			const auto weather = kept->weather;
			kept.reset();
			if (a_here.overrideWeather != weather) {
				return {};  // not ours any more (a script of this game's took the sky): nothing to let go
			}
			++releases;
			return AskMove{ AskMove::Kind::kRelease, weather, true };
		}

		std::optional<Kept> kept;
	};

	// ---- what a friend's save keeps ----
	//
	// A friend's save written in a session holds the session's hour and the day count it reached following the owner
	// (never its own timescale, nor the owner's weather pinned: both are taken off for the save). It is never put back on
	// leaving: following only ever moves a friend's day count forward (a line-up goes back within the day at most), as a
	// wait or a sleep would, and every timer the session started in that game (a cell's reset, a quest's wait, a bounty
	// letter, a vendor's restock) was started on that day count: putting the date back would run them all backwards. So
	// the save's date is where the session left it, as if that player had waited the difference; the log says, at each
	// save and when the session ends, the day count the game had when it joined and the one it keeps (SaveDays).
	struct SaveDays
	{
		std::optional<double> joined;  // the day count when this game joined the session (or loaded a save in it)

		// The game joined (or loaded) with this day count; a second call in the same session keeps the first.
		void Joined(double a_days)
		{
			if (!joined) {
				joined = a_days;
			}
		}

		// Days the save now holds beyond the count it joined with (none when it never joined).
		[[nodiscard]] std::optional<double> Since(double a_days) const
		{
			return joined ? std::optional<double>{ a_days - *joined } : std::nullopt;
		}

		void Reset() { joined.reset(); }
	};
}
