#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

// An NPC's health and death in every game (docs/DESIGN.md, "NPC health and deaths"), the parts that need no game: what
// a copy of an NPC another game runs shows, and when the game that runs one says its health. F4Multiplayer's design
// after five rounds of "heal on hit" (its docs/HEALTH-SYNC.md), carried over:
// - The runner's game is the only truth of an NPC's health. A copy shows the runner's word less every hit of ours that
//   is not answered yet, however long that takes: never a heal for a hit still on its way.
// - Our player's hit on a copy is a numbered request (Protocol::NpcHit) that ends in one answer: applied (the runner's
//   health after it comes with it), or refused, which gives the drop back at once. A hit never answered is given back
//   after LOST_AFTER, the one timer left.
// - Our player's hit is what its hit event explains (one drop, within HIT_REACH), and what our player's lasting effects
//   on the copy take as they tick (a burn, a poison, an enchantment's, a concentration spell's): only their share, at
//   their magnitude, never a window in which any drop counts.
// - What our player's hits did not do to a copy (a blow of an NPC's copy, a friend's stand-in, a trap, a fire, its own
//   regeneration) is set back once it has stood JUDGE_AFTER: those are the runner's game's to deal, to its NPC.
//   An NPC's blow is dealt in the game that runs the NPC it lands on, by the attacker's copy there, so a fight between
//   NPCs two games run (a follower and a bandit) is counted once, never twice.
// - Only the runner's death counts. A copy our player killed (its killer is our player, however late its hit event came)
//   stays dead here and its killing blow goes to the runner as at least all it stood at; a copy the runner's death
//   reaches is killed here, after any kill move it is in.
// No game here: tests/NpcCombatTests.cpp.
namespace CopyHealth
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr float EPSILON = 0.005f;      // half a percent: a change below it is left until it adds up
	inline constexpr auto  JUDGE_AFTER = 100ms;   // a change no hit of ours explains stands this long before it goes back
	inline constexpr auto  HIT_REACH = 1s;        // a drop this soon after our player's hit on the copy is that hit's
	inline constexpr float DRAIN_MARGIN = 1.25f;  // our lasting effects may take this much more than their magnitude says
	inline constexpr float DRAIN_HOLD = 0.25f;    // s: what our effects may take is held this long at most, unused (and EPSILON)
	inline constexpr auto  LOST_AFTER = 120s;     // a hit never answered: given back (the server answers for a runner that left)
	inline constexpr auto  REVIVE_AFTER = 5s;     // a copy dead here while its runner's NPC lives is raised after this
	inline constexpr auto  KILL_AGAIN = 2s;       // a kill the game did not take is tried again after this
	inline constexpr auto  REPORT_EVERY = 250ms;  // a runner says one NPC's health at most this often (its hits' answers: at once)
	inline constexpr std::size_t MAX_IN_FLIGHT = 64;

	// What a copy shows now: its health as a share of its most, dead or not, in a kill move or not; the least share a
	// living copy is ever set to (one point of its most): a runner's NPC down at none (an essential one) is no death here;
	// what our player's lasting effects on it take a second (a share of its most: a burn, a poison, an enchantment's,
	// a concentration spell's, each at its magnitude); and, dead, whether our player is its killer.
	struct Seen
	{
		float share = 1;
		bool  dead = false;
		bool  inKillMove = false;
		float floor = 0;
		float ourDrain = 0;
		bool  killedByUs = false;
	};

	// Our player's last hit on the copy (a hit event of theirs on it, or the kill move they are in with it, paired).
	struct OurHit
	{
		Clock::time_point at{};
		std::uint8_t      flags = 0;  // Protocol::NpcHitFlags (power, sneak, bash, spell, kill move)
	};

	enum class Do
	{
		kNothing,
		kSend,    // send `hit` to the runner (it is in flight now)
		kSet,     // set the copy's health to `share` of its most
		kKill,    // kill the copy: its runner's NPC died
		kRevive,  // raise the copy, at `share` of its most: its runner's NPC lives
	};

	struct Step
	{
		Do               what = Do::kNothing;
		Protocol::NpcHit hit;
		float            share = 0;
	};

	struct Counts
	{
		std::uint64_t sent = 0;       // hits of ours booked and sent
		std::uint64_t applied = 0;    // answered applied
		std::uint64_t refused = 0;    // answered refused: given back
		std::uint64_t lost = 0;       // never answered: given back
		std::uint64_t givenBack = 0;  // drops no hit of ours explains, set back
		std::uint64_t takenBack = 0;  // gains the runner never said, set back
		std::uint64_t kills = 0;      // copies killed for their runner's death
		std::uint64_t revived = 0;    // copies raised for their runner's NPC living
		std::uint64_t saveKills = 0;  // bodies of the host's save its runner had alive: killing blows sent
		bool operator==(const Counts&) const = default;
	};

	// One copy's health, as this game shows it.
	class Copy
	{
	public:
		// The runner's word: its NPC's health, as a share of its most (more than 0: it lives).
		void Word(float a_share)
		{
			word = std::clamp(a_share, 0.0f, 1.0f);
			runnerDead = false;
			setOwed = true;
		}

		// The runner's NPC died (its runner said so, or the session had it dead).
		void Died()
		{
			runnerDead = true;
			killTried.reset();
		}

		[[nodiscard]] bool RunnerDead() const { return runnerDead; }
		[[nodiscard]] std::optional<float> RunnersWord() const { return word; }
		[[nodiscard]] std::size_t InFlight() const { return inFlight.size(); }

		// What the copy should show: the runner's word less our hits not answered yet (none: no word yet).
		[[nodiscard]] std::optional<float> Expected() const
		{
			if (!word) {
				return std::nullopt;
			}
			float share = *word;
			for (const auto& hit : inFlight) {
				share -= hit.share;
			}
			return (std::max)(share, 0.0f);
		}

		// The answer to one of our hits on it. Applied: the runner's health after it is its word. Refused: the drop goes
		// back (the next Look sets it). kYours: our game runs the NPC now, and what the copy took stands.
		void Answer(std::uint32_t a_seq, Protocol::HitVerdict a_verdict, float a_share, Counts& a_counts)
		{
			const auto it = std::ranges::find_if(inFlight, [&](const Flying& a_f) { return a_f.seq == a_seq; });
			if (it == inFlight.end()) {
				return;  // given up on already, or not ours
			}
			const auto taken = it->share;
			inFlight.erase(it);
			switch (a_verdict) {
			case Protocol::HitVerdict::kApplied:
				++a_counts.applied;
				word = std::clamp(a_share, 0.0f, 1.0f);
				break;
			case Protocol::HitVerdict::kYours:
				word = (std::max)(word.value_or(1.0f) - taken, 0.0f);
				break;
			default:
				++a_counts.refused;
				break;
			}
			setOwed = true;
		}

		// Looked at once a frame or so: what to do about what the copy shows. a_ourHit: our player's last hit on it, if
		// any; a_nextSeq gives our next hit's number; a_mayRevive: the copy may be raised when it is dead here and its
		// runner's NPC lives (it was seen alive here this session, or this game is not the host's); a_hostsSave: this
		// game is the host's, whose save is the truth of who is dead: a body here never seen alive this session is killed in
		// its runner's game (a killing blow of no flags) rather than raised here.
		template <class NextSeq>
		Step Look(const Protocol::NpcKey& a_key, const Seen& a_seen, const std::optional<OurHit>& a_ourHit, NextSeq&& a_nextSeq, bool a_mayRevive, bool a_hostsSave,
			Clock::time_point a_now, Counts& a_counts)
		{
			// Hits never answered: given back.
			const auto before = inFlight.size();
			std::erase_if(inFlight, [&](const Flying& a_f) { return a_now - a_f.at >= LOST_AFTER; });
			if (inFlight.size() != before) {
				a_counts.lost += before - inFlight.size();
				setOwed = true;
			}
			std::optional<OurHit> ours;
			if (a_ourHit && a_now - a_ourHit->at <= HIT_REACH) {
				ours = a_ourHit;
			}
			// What our player's lasting effects on it may have taken since the last look: their share alone, held a moment.
			if (a_seen.ourDrain > 0.0f && !a_seen.dead && lastLook) {
				const auto rate = a_seen.ourDrain * DRAIN_MARGIN;
				const auto seconds = std::chrono::duration<float>(a_now - *lastLook).count();
				drainCredit = (std::min)(drainCredit + rate * (std::max)(seconds, 0.0f), rate * DRAIN_HOLD + EPSILON);
			} else if (a_seen.ourDrain <= 0.0f || a_seen.dead) {
				drainCredit = 0.0f;
			}
			lastLook = a_now;
			const auto book = [&](float a_share, std::uint8_t a_flags) {
				Step step{ .what = Do::kSend };
				step.hit = { .key = a_key, .seq = a_nextSeq(), .share = std::clamp(a_share, 0.0f, 1.0f), .flags = a_flags };
				if (inFlight.size() < MAX_IN_FLIGHT) {
					inFlight.push_back({ step.hit.seq, step.hit.share, a_now });
				}
				++a_counts.sent;
				changeSince.reset();
				return step;
			};

			if (runnerDead) {
				// Its runner's NPC is dead: the copy dies too, once out of any kill move (the move kills it itself).
				if (a_seen.dead || a_seen.inKillMove) {
					return {};
				}
				if (killTried && a_now - *killTried < KILL_AGAIN) {
					return {};
				}
				killTried = a_now;
				++a_counts.kills;
				return { .what = Do::kKill };
			}

			if (a_seen.dead) {
				deadSince = deadSince.value_or(a_now);
				// Killed by our player here (its killer is our player, or its hit's event, which may come a frame after the
				// death, is fresh): the killing blow goes as at least all it stood at, once, so it dies in its runner's game
				// too. Killed by anything else: its runner's game deals that, or the copy is raised.
				const auto left = Expected().value_or(a_seen.share);
				const bool saveSays = a_hostsSave && !seenAlive && word.has_value();
				const bool ourKill = ours || a_seen.killedByUs;
				if (!killBooked && (ourKill || saveSays) && left > 0.0f) {
					killBooked = true;
					const auto flags = ours ? ours->flags : std::uint8_t{ 0 };
					const auto killMove = ourKill && (a_seen.inKillMove || (flags & Protocol::kHitKillMove) != 0);
					a_counts.saveKills += ourKill ? 0 : 1;
					return book(left, static_cast<std::uint8_t>((flags & ~Protocol::kHitKillMove) | Protocol::kHitKilling | (killMove ? Protocol::kHitKillMove : 0)));
				}
				// Raised only to some health: a runner's NPC at none is down (an essential one), not up.
				if (word && *word > EPSILON && inFlight.empty() && a_mayRevive && a_now - *deadSince >= REVIVE_AFTER) {
					deadSince.reset();
					killBooked = false;
					++a_counts.revived;
					setOwed = false;
					return { .what = Do::kRevive, .share = *word };
				}
				return {};
			}
			deadSince.reset();
			killBooked = false;
			seenAlive = true;

			if (!word) {
				word = std::clamp(a_seen.share, 0.0f, 1.0f);  // no word yet: what it shows is where it starts
				return {};
			}
			const auto expected = (std::max)(*Expected(), a_seen.floor);
			const auto diff = a_seen.share - expected;
			// A drop our player's hit explains: booked and sent at once. A hit event explains one drop (a runner's word that
			// differs from what the copy took is set, never sent again as more hits).
			const bool fresh = ours && (!bookedHit || ours->at > *bookedHit);
			if (diff < -EPSILON && fresh) {
				bookedHit = ours->at;
				return book(-diff, static_cast<std::uint8_t>(ours->flags & ~Protocol::kHitKilling));
			}
			// A drop our player's lasting effects explain, as they tick: their share of it alone (what they may have taken
			// since, DRAIN_MARGIN over their magnitude), never what another's blow took in the same moment; that is judged
			// as anything else. Not in the frame a word or an answer is set (a refused one is not sent again).
			if (diff < -EPSILON && !setOwed && drainCredit >= EPSILON) {
				const auto take = (std::min)(-diff, drainCredit);
				drainCredit -= take;
				const auto since = changeSince;
				auto       step = book(take, Protocol::kHitSpell);
				if (-diff - take > EPSILON) {
					changeSince = since.value_or(a_now);  // the rest is none of ours: set back once it has stood
				}
				return step;
			}
			if (std::fabs(diff) <= EPSILON) {
				setOwed = false;
				changeSince.reset();
				return {};
			}
			// The runner's new word, or a refused hit: set at once.
			if (setOwed) {
				setOwed = false;
				changeSince.reset();
				return { .what = Do::kSet, .share = expected };
			}
			// Anything else: set back once it has stood a moment (our hit's event may come a frame after its drop).
			if (!changeSince) {
				changeSince = a_now;
				return {};
			}
			if (a_now - *changeSince < JUDGE_AFTER) {
				return {};
			}
			changeSince.reset();
			++(diff < 0.0f ? a_counts.givenBack : a_counts.takenBack);
			return { .what = Do::kSet, .share = expected };
		}

		[[nodiscard]] bool SeenAlive() const { return seenAlive; }

	private:
		struct Flying
		{
			std::uint32_t     seq = 0;
			float             share = 0;
			Clock::time_point at{};
		};

		std::optional<float>             word;
		std::vector<Flying>              inFlight;
		bool                             runnerDead = false;
		bool                             setOwed = false;
		bool                             seenAlive = false;
		bool                             killBooked = false;  // dead here: our killing blow went (once a death)
		std::optional<Clock::time_point> bookedHit;  // the hit event whose drop was booked last
		std::optional<Clock::time_point> lastLook;
		float                            drainCredit = 0;  // what our lasting effects on it may still take (a share)
		std::optional<Clock::time_point> changeSince;
		std::optional<Clock::time_point> deadSince;
		std::optional<Clock::time_point> killTried;
	};

	// When the game that runs an NPC says its health: the first time it runs it, after each hit of a friend's it applied
	// (with that hit's answer), and when it changed by more than EPSILON, at most every REPORT_EVERY.
	class Reporter
	{
	public:
		[[nodiscard]] bool Due(float a_share, bool a_hitApplied, Clock::time_point a_now) const
		{
			if (!sent || a_hitApplied) {
				return true;
			}
			return std::fabs(a_share - *sent) > EPSILON && a_now - sentAt >= REPORT_EVERY;
		}

		void Sent(float a_share, Clock::time_point a_now)
		{
			sent = a_share;
			sentAt = a_now;
		}

		void Reset() { sent.reset(); }

	private:
		std::optional<float> sent;
		Clock::time_point    sentAt{};
	};

	// A friend's hit on an NPC this game runs: the health it takes off, as the same share of this NPC's most as it took
	// off the friend's copy (so it takes as many hits to kill in every game, whatever each rolled), never below 0.
	[[nodiscard]] inline float DamageOf(float a_share, float a_max)
	{
		if (!std::isfinite(a_share) || !std::isfinite(a_max) || a_max <= 0.0f) {
			return 0.0f;
		}
		return std::clamp(a_share, 0.0f, 1.0f) * a_max;
	}

	// The health to set to show a_share of a_max, as a change from a_now (positive: restored, negative: damaged).
	[[nodiscard]] inline float ChangeTo(float a_share, float a_max, float a_now)
	{
		if (!std::isfinite(a_share) || !std::isfinite(a_max) || !std::isfinite(a_now) || a_max <= 0.0f) {
			return 0.0f;
		}
		return std::clamp(a_share, 0.0f, 1.0f) * a_max - a_now;
	}

	// A share of the most health (0 when the most is none).
	[[nodiscard]] inline float ShareOf(float a_health, float a_max)
	{
		if (!std::isfinite(a_health) || !std::isfinite(a_max) || a_max <= 0.0f) {
			return 0.0f;
		}
		return std::clamp(a_health / a_max, 0.0f, 1.0f);
	}

	// Whether a game kills an NPC alive in it that the session has dead (a death reported while it was not loaded here,
	// or before this game joined): every game but the host's does; the host's own save is the truth about its own kills
	// (a death its game reported, from an older save, lives), and a friend's kill is replayed to it as to anyone.
	[[nodiscard]] constexpr bool KillsOnLoad(bool a_weAreHost, bool a_weReportedIt)
	{
		return !(a_weAreHost && a_weReportedIt);
	}

	// The session's dead that live here (KillsOnLoad), each looked at as it is loaded here: killed; a kill the game did not
	// take tried again every KILL_AGAIN, up to KILL_TRIES (then given up, said once: this game keeps it from dying, an
	// essential flag the runner's game had off); and killed again whenever one seen dead here lives again (this game's own
	// respawn of its cell, which each game times from its own last visit, or a script's or a spell's raise in this game
	// alone). The session's word is that it is dead: a body risen in a friend's game and fought there would be run alive and
	// raise it in every game. (A raise by its runner is the session's: its health said again ends the session's death,
	// and Forget with it.)
	inline constexpr int  KILL_TRIES = 5;

	enum class DeadDo : std::uint8_t
	{
		kNothing,
		kKill,    // kill it (the first, or another try: Tries says which)
		kGiveUp,  // the last try did not take: say so, once
	};

	class DeadHere
	{
	public:
		// a_alive: it lives here now. Only for one the session has dead and this game kills (KillsOnLoad).
		[[nodiscard]] DeadDo Look(std::uint32_t a_id, bool a_alive, Clock::time_point a_now)
		{
			auto& s = states[a_id];
			if (!a_alive) {
				s.sawDead = true;
				s.tries = 0;
				s.toldGiveUp = false;
				return DeadDo::kNothing;
			}
			if (s.sawDead) {
				s.sawDead = false;
				++risen;
			}
			if (s.tries >= KILL_TRIES) {
				if (s.toldGiveUp) {
					return DeadDo::kNothing;
				}
				s.toldGiveUp = true;
				++givenUp;
				return DeadDo::kGiveUp;
			}
			if (s.tries == 0 || a_now - s.last >= KILL_AGAIN) {
				++s.tries;
				s.last = a_now;
				++kills;
				return DeadDo::kKill;
			}
			return DeadDo::kNothing;
		}

		// The tries so far on a_id since it was last seen dead (1 after the first kill).
		[[nodiscard]] int Tries(std::uint32_t a_id) const
		{
			const auto it = states.find(a_id);
			return it != states.end() ? it->second.tries : 0;
		}

		// Not dead for the session any more, or not this game's to kill.
		void Forget(std::uint32_t a_id) { states.erase(a_id); }
		// A load or the session's end: the world's actors are new ones.
		void Clear() { states.clear(); }
		[[nodiscard]] std::size_t Size() const { return states.size(); }

		std::uint64_t kills = 0;
		std::uint64_t risen = 0;
		std::uint64_t givenUp = 0;

	private:
		struct State
		{
			int               tries = 0;
			Clock::time_point last{};
			bool              sawDead = false;
			bool              toldGiveUp = false;
		};
		std::map<std::uint32_t, State> states;
	};
}
