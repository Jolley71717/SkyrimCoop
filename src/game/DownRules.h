#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// A player's death in a session, and what follows it (docs/DESIGN.md, "Downed and helped up"), the parts that need no
// game. Chosen as the sister project (F4Multiplayer) does it, with the bugs it fixed over time designed out:
// - With a friend in the session, our player is made essential (by our own flag, never written into a save): a blow
//   that would kill them brings them down instead, kneeling in the engine's own bleedout.
// - Down, they wait for help for BLEED_OUT (real time: the friends' games keep running while a menu is open here). A
//   friend who stays within HELP_RANGE of them for HELP_TIME helps them up; they get up with HEALTH_ON_UP of their
//   health. No help in time: our flag comes off, they die, and the game loads their last save, as a death alone does.
// - The downed player's game decides: a friend's help is a request it takes once (a second does nothing), and only
//   while they wait. "Waits for help" is said apart from "on the ground": a player getting up is helped by nobody.
// - Nothing leaves a player stuck: a get-up the engine does not finish is finished by hand, and one that still has not
//   happened after GET_UP_MAX of play (menus not counted) ends in the death the bleed-out would have; a kill that does not
//   take is tried again with our flag kept off; a bleedout camera left behind by any path is ended; friends who leave
//   do not end a down (one may come back), and a down that another mod's or a quest's essential flag began is that
//   flag's, never ours to end.
// The game's side is src/game/Downed.cpp. No game here: tests/DownedTests.cpp.
namespace DownRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto  BLEED_OUT = 45s;           // down this long with no help: the death
	inline constexpr auto  HELP_TIME = 2s;            // a friend this long within HELP_RANGE helps us up
	inline constexpr float HELP_RANGE = 300.0f;       // game units (about four metres) from where the friend lies
	inline constexpr auto  HELP_AGAIN = 3s;           // a helper asks again no sooner (its first was lost, or came late)
	inline constexpr float HEALTH_ON_UP = 0.3f;       // of the most health, given on getting up
	inline constexpr auto  GET_UP_WAIT = 1500ms;      // the engine's own get-up is waited for this long (play time)
	inline constexpr auto  GET_UP_RETRY = 1s;         // then it is done by hand, again this often
	inline constexpr auto  GET_UP_MAX = 20s;          // of play: a get-up that never happens ends in the bleed-out's death
	inline constexpr auto  KILL_RETRY = 2s;           // a kill the game did not take is tried again
	inline constexpr auto  CAMERA_STRAY = 1s;         // a bleedout camera with nobody down this long is ended
	inline constexpr auto  CAMERA_RETRY = 2s;
	inline constexpr auto  SAY_EVERY = 5s;            // a down is said again (its seconds left) this often
	inline constexpr int   REMIND_AT[] = { 30, 10 };  // seconds left told to the downed player (when the bleed-out is longer)

	// What a player's down is, as the others are told (Protocol::Downed::state).
	enum class Said : std::uint8_t
	{
		kUp = 0,         // standing (or dead: their game loads their save)
		kWaiting = 1,    // on the ground, waiting for help
		kGettingUp = 2,  // on the ground, helped: nobody helps again
	};
	inline constexpr std::uint8_t SAID_COUNT = 3;

	[[nodiscard]] constexpr std::string_view SaidName(Said a_said)
	{
		switch (a_said) {
		case Said::kUp:
			return "up";
		case Said::kWaiting:
			return "waiting";
		case Said::kGettingUp:
			return "gettingUp";
		}
		return "?";
	}

	enum class Phase : std::uint8_t
	{
		kUp,         // standing (or down by an essential flag not ours: the engine's own bleedout)
		kDown,       // down by ours: waiting for help
		kGettingUp,  // helped: getting up
		kBledOut,    // no help in time (or a get-up that never came): our flag off, dying
	};

	[[nodiscard]] constexpr std::string_view PhaseName(Phase a_phase)
	{
		switch (a_phase) {
		case Phase::kUp:
			return "up";
		case Phase::kDown:
			return "down";
		case Phase::kGettingUp:
			return "gettingUp";
		case Phase::kBledOut:
			return "bledOut";
		}
		return "?";
	}

	// What our game shows of our player this frame.
	struct Facts
	{
		bool friends = false;        // another player is in the session
		bool ours = false;           // our essential flag is on (set by us)
		bool elsewise = false;       // essential (or protected) for another reason: a quest, a mod, the console
		bool down = false;           // the engine's bleedout (its life state: essential down or bleeding out)
		bool dead = false;           // dying or dead
		bool paused = false;         // a menu stops the game
		bool bleedCamera = false;    // the camera is in its bleedout state
	};

	// What to do, in this order: the flag, then the rest.
	struct Step
	{
		std::optional<bool> essential;  // set (true) or take off (false) our essential flag
		bool noRecovery = false;        // the engine must not stand them up on its own (went down)
		bool recovery = false;          // it may again (helped, or up)
		bool wentDown = false;
		bool health = false;            // give HEALTH_ON_UP of the most health (helped)
		bool standUp = false;           // end the bleedout by hand (the engine's own get-up did not come)
		bool kill = false;
		bool gotUp = false;
		bool fixCamera = false;         // end a bleedout camera nobody's down is in
		bool say = false;               // tell the others our down now (Said)
		int  remind = 0;                // tell our player this many seconds are left (0: nothing)
	};

	struct Counts
	{
		std::uint64_t downs = 0;
		std::uint64_t helped = 0;       // helps taken
		std::uint64_t helpsRefused = 0; // helps that came while not waiting
		std::uint64_t gotUp = 0;
		std::uint64_t upElsewise = 0;   // stood up by another path while down (a console, a script): taken as up
		std::uint64_t standUps = 0;     // get-ups done by hand
		std::uint64_t bledOut = 0;
		std::uint64_t neverUp = 0;      // get-ups that never happened: ended in the bleed-out's death
		std::uint64_t kills = 0;        // kills asked (retries included)
		std::uint64_t cameraFixes = 0;
	};

	class Self
	{
	public:
		Step Frame(const Facts& a_facts, Clock::time_point a_now)
		{
			Step step;
			const auto played = last && !a_facts.paused ? a_now - *last : Clock::duration::zero();
			last = a_now;
			switch (phase) {
			case Phase::kUp:
				if (a_facts.down && a_facts.ours && !a_facts.dead) {
					phase = Phase::kDown;
					downAt = a_now;
					reminded = 0;
					saidAt = a_now;
					++counts.downs;
					step.wentDown = step.noRecovery = step.say = true;
					break;
				}
				// Ours on while a friend is here and nothing else makes our player essential; never on a dead player.
				if (const bool want = a_facts.friends && !a_facts.elsewise && !a_facts.dead; want != a_facts.ours) {
					step.essential = want;
				}
				break;
			case Phase::kDown:
				if (a_facts.dead) {
					phase = Phase::kBledOut;  // a script's or a console's kill: their game loads the save
					step.say = true;
					break;
				}
				if (!a_facts.down) {
					phase = Phase::kUp;  // stood up by another path (a console's resurrect, a script): up it is
					++counts.upElsewise;
					step.gotUp = step.recovery = step.say = true;
					break;
				}
				keepOurs(a_facts, step);  // put back after a save took it off
				if (a_now - downAt >= BLEED_OUT) {
					phase = Phase::kBledOut;
					++counts.bledOut;
					bleedOut(a_now, step);
					break;
				}
				for (const int at : REMIND_AT) {
					if (BLEED_OUT > std::chrono::seconds(at) + 5s && SecondsLeft(a_now) <= at && (reminded == 0 || at < reminded)) {
						reminded = at;
						step.remind = at;
					}
				}
				if (a_now - saidAt >= SAY_EVERY) {
					saidAt = a_now;
					step.say = true;
				}
				break;
			case Phase::kGettingUp:
				if (a_facts.dead) {
					phase = Phase::kBledOut;
					step.say = true;
					break;
				}
				if (!a_facts.down) {
					phase = Phase::kUp;
					++counts.gotUp;
					step.gotUp = step.recovery = step.say = true;
					break;
				}
				keepOurs(a_facts, step);
				gettingUp += played;
				if (gettingUp >= GET_UP_MAX) {
					phase = Phase::kBledOut;
					++counts.neverUp;
					bleedOut(a_now, step);
					break;
				}
				if (gettingUp >= GET_UP_WAIT && !a_facts.paused && (!standUpAt || a_now - *standUpAt >= GET_UP_RETRY)) {
					standUpAt = a_now;
					++counts.standUps;
					step.standUp = step.health = step.recovery = true;
				}
				break;
			case Phase::kBledOut:
				if (a_facts.dead) {
					break;  // the game's death goes on to its load
				}
				if (!a_facts.down) {
					phase = Phase::kUp;  // raised (a console's resurrect): up it is
					step.gotUp = step.recovery = step.say = true;
					break;
				}
				if (a_facts.ours) {
					step.essential = false;
				}
				if (!killAt || a_now - *killAt >= KILL_RETRY) {
					killAt = a_now;
					++counts.kills;
					step.kill = true;
				}
				break;
			}
			// A bleedout camera with nobody down in it (a get-up by any path that left it, a knock-down's): ended.
			if (a_facts.bleedCamera && !a_facts.down && !a_facts.dead) {
				if (!strayCameraSince) {
					strayCameraSince = a_now;
				} else if (a_now - *strayCameraSince >= CAMERA_STRAY && (!cameraFixAt || a_now - *cameraFixAt >= CAMERA_RETRY)) {
					cameraFixAt = a_now;
					++counts.cameraFixes;
					step.fixCamera = true;
				}
			} else {
				strayCameraSince.reset();
			}
			return step;
		}

		// A friend's help. Taken only while we wait for it: the first one gets us up, a second does nothing. The next frame
		// gives the health and lets the engine stand us up.
		bool Help(Clock::time_point a_now)
		{
			if (phase != Phase::kDown) {
				++counts.helpsRefused;
				return false;
			}
			phase = Phase::kGettingUp;
			gettingUp = Clock::duration::zero();
			standUpAt.reset();
			helpedAt = a_now;
			last = a_now;  // the get-up's play time counts from the help
			++counts.helped;
			helpPending = true;
			return true;
		}

		// What the frame after a help does first: the health given and the engine let stand us up (its own get-up).
		[[nodiscard]] bool TakeHelpPending()
		{
			return std::exchange(helpPending, false);
		}

		// A load or a new game: whatever was before is another world's. Our flag is judged again next frame.
		void Forget()
		{
			phase = Phase::kUp;
			killAt.reset();
			standUpAt.reset();
			strayCameraSince.reset();
			cameraFixAt.reset();
			last.reset();
			helpPending = false;
		}

		[[nodiscard]] Phase Now() const { return phase; }
		[[nodiscard]] Said Says() const
		{
			return phase == Phase::kDown ? Said::kWaiting : phase == Phase::kGettingUp ? Said::kGettingUp : Said::kUp;
		}
		[[nodiscard]] bool WaitsForHelp() const { return phase == Phase::kDown; }
		[[nodiscard]] int SecondsLeft(Clock::time_point a_now) const
		{
			if (phase != Phase::kDown) {
				return 0;
			}
			const auto left = std::chrono::duration_cast<std::chrono::seconds>(BLEED_OUT - (a_now - downAt) + 999ms).count();
			return static_cast<int>(std::clamp<long long>(left, 0, 0xFFFF));
		}

		Counts counts;

	private:
		void keepOurs(const Facts& a_facts, Step& a_step) const
		{
			if (!a_facts.ours) {
				a_step.essential = true;
			}
		}

		void bleedOut(Clock::time_point a_now, Step& a_step)
		{
			a_step.essential = false;
			a_step.kill = true;
			a_step.say = true;
			killAt = a_now;
			++counts.kills;
		}

		Phase                            phase = Phase::kUp;
		Clock::time_point                downAt{};
		Clock::time_point                saidAt{};
		Clock::time_point                helpedAt{};
		Clock::duration                  gettingUp{};
		int                              reminded = 0;
		bool                             helpPending = false;
		std::optional<Clock::time_point> last;
		std::optional<Clock::time_point> killAt;
		std::optional<Clock::time_point> standUpAt;
		std::optional<Clock::time_point> strayCameraSince;
		std::optional<Clock::time_point> cameraFixAt;
	};

	// ---- helping a friend up ----

	// Each frame, for a friend who waits for help: whether our player is near enough to help them (and able: standing,
	// alive, in the world, not in a menu). Near for HELP_TIME in a row is a help, sent; not again for HELP_AGAIN while they
	// still wait (the first was lost, or is on its way). Away or unable, the time starts again.
	class Helper
	{
	public:
		// a_distance: from our player to where the friend's stand-in lies (none: no stand-in here, so too far).
		// True: send a help now.
		bool Frame(bool a_waits, bool a_able, std::optional<float> a_distance, Clock::time_point a_now)
		{
			if (!a_waits) {
				nearSince.reset();
				return false;
			}
			if (!a_able || !a_distance || *a_distance > HELP_RANGE) {
				nearSince.reset();
				return false;
			}
			if (!nearSince) {
				nearSince = a_now;
				++approaches;
			}
			if (a_now - *nearSince < HELP_TIME || (sentAt && a_now - *sentAt < HELP_AGAIN)) {
				return false;
			}
			sentAt = a_now;
			++sent;
			return true;
		}

		// Whether our player is within reach and the help is under way (for the notice).
		[[nodiscard]] bool Helping() const { return nearSince.has_value(); }

		std::uint64_t approaches = 0;
		std::uint64_t sent = 0;

	private:
		std::optional<Clock::time_point> nearSince;
		std::optional<Clock::time_point> sentAt;
	};

	// ---- a downed friend's stand-in ----

	// The stand-in of a friend on the ground kneels in the bleedout idle (its graph's own event: never its life state, which
	// would make it a stand-in that fell here, src/game/StandInRules.h) and gets up when they do. What was asked of the
	// stand-in now there is tracked by its form id: a new stand-in (placed again, a load) is asked again. An event the
	// graph turned away is asked again KNEEL_RETRY later, KNEEL_TRIES times at most per change.
	inline constexpr auto        KNEEL_RETRY = 1s;
	inline constexpr std::size_t KNEEL_TRIES = 5;
	inline constexpr std::string_view KNEEL_START = "BleedOutStart";
	inline constexpr std::string_view KNEEL_STOP = "BleedOutStop";

	enum class Kneel : std::uint8_t
	{
		kNothing,
		kStart,  // send KNEEL_START to its graph
		kStop,   // send KNEEL_STOP
	};

	class StandInKneel
	{
	public:
		// a_wanted: its friend is on the ground (waiting or getting up); a_standIn: the stand-in now there (0: none).
		Kneel Frame(bool a_wanted, std::uint32_t a_standIn, Clock::time_point a_now)
		{
			if (a_standIn == 0) {
				shownOn = 0;
				return Kneel::kNothing;
			}
			if (a_standIn != shownOn) {
				// A new stand-in stands: it kneels only if its friend is down.
				shownOn = a_standIn;
				kneeling = false;
				tries = 0;
				askedAt.reset();
				taken = true;
			}
			if (a_wanted == kneeling && taken) {
				return Kneel::kNothing;
			}
			if (a_wanted != kneeling) {
				kneeling = a_wanted;
				tries = 0;
				taken = false;
				askedAt.reset();
			}
			if (tries >= KNEEL_TRIES || (askedAt && a_now - *askedAt < KNEEL_RETRY)) {
				return Kneel::kNothing;
			}
			askedAt = a_now;
			++tries;
			return kneeling ? Kneel::kStart : Kneel::kStop;
		}

		// What the graph did with the last event asked.
		void Took(bool a_taken)
		{
			taken = a_taken;
			if (!a_taken) {
				++turnedAway;
			}
		}

		[[nodiscard]] bool Kneeling() const { return kneeling && shownOn != 0; }

		std::uint64_t turnedAway = 0;

	private:
		std::uint32_t                    shownOn = 0;
		bool                             kneeling = false;
		bool                             taken = true;
		std::size_t                      tries = 0;
		std::optional<Clock::time_point> askedAt;
	};
}
