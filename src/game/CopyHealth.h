#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
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
// - Only the runner's death counts, and a copy dies here only on it (DiesHere). A blow that would kill a copy here (an
//   NPC's of this game, a friend's stand-in's shot, a fall, our player's) is held at one point of health instead; our
//   player's goes to the runner as a killing blow of all it stood at (Seen::heldByUs), and the copy dies when the
//   runner's death comes. A copy that died here first would run its own death scripts at once (a dragon's soul given,
//   its skin turned to bones; a kill counted; a quest's stage) for an NPC that may live on, and be raised after.
//   Our player's kill move is the one kill a copy takes here first (the paired move needs its victim to die): its
//   killing blow goes as at least all it stood at. A copy the runner's death reaches is killed here, after any kill move
//   it is in.
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
		bool  crashing = false;  // its runner's body still crashes (NpcRules::Crash): its death waits
		float floor = 0;
		float ourDrain = 0;
		bool  killedByUs = false;
		bool  heldByUs = false;    // our player's blow would have killed it here since the last look, and it was held (DiesHere)
		bool  heldScript = false;  // a kill of it here that no blow explains (a script's, the console's) is owed (HeldKills)
		bool  heldEssential = false;  // that kill was a KillEssential: it was essential here until the kill (HeldKills)
	};

	// A kill here no blow explains (a script's Kill, the console's kill) of a copy that is essential here is the engine's
	// to refuse: Skyrim alone only knocks an essential NPC down for it. Nothing is owed for it (the copy is held, as any
	// kill of it). A KillEssential takes the essential flag off first, so its kill reaches here on a copy not essential.
	[[nodiscard]] constexpr bool OwesScriptKill(bool a_essentialHere)
	{
		return !a_essentialHere;
	}

	// What the game that runs an NPC does with a friend's kill of its copy that no blow explains (kHitScript): one not
	// essential here dies of it (the kill that keeps to the essential flag); one essential here dies only of a
	// KillEssential (kHitEssential: the kill that ignores the flag), and refuses any other, as Skyrim alone would only
	// knock it down; refused, it is left as it is and the answer says it lives, so the friend sends it no more.
	enum class ScriptKill
	{
		kKill,
		kKillEssential,
		kRefuse,
	};

	[[nodiscard]] constexpr ScriptKill OnScriptKill(bool a_essentialHere, bool a_killEssential)
	{
		if (!a_essentialHere) {
			return ScriptKill::kKill;
		}
		return a_killEssential ? ScriptKill::kKillEssential : ScriptKill::kRefuse;
	}

	// Whether a copy of an NPC another game runs may die here: on its runner's word (or the session's: it is dead in the
	// session), or in our player's kill move with it (the paired move kills its victim; its killing blow is sent). Any
	// other kill (a blow, a fall, a script's) is held: the copy is left at one point of health.
	[[nodiscard]] constexpr bool DiesHere(bool a_onRunnersWord, bool a_inOurKillMove)
	{
		return a_onRunnersWord || a_inOurKillMove;
	}

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
		std::uint64_t heldKills = 0;  // our player's blows that would have killed a copy here, held: killing blows sent
		std::uint64_t scriptKills = 0;  // kills here no blow explains (a script's, the console's), held: sent as deaths
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
		// back (the next Look sets it). kYours: our game runs the NPC now, and what the copy took stands. True: it was a
		// held kill no blow explains (kHitScript) and its runner settled it (applied: it died there, or it lives there,
		// essential, and refused it), so it is sent no more (HeldKills::Answered).
		bool Answer(std::uint32_t a_seq, Protocol::HitVerdict a_verdict, float a_share, Counts& a_counts)
		{
			const auto it = std::ranges::find_if(inFlight, [&](const Flying& a_f) { return a_f.seq == a_seq; });
			if (it == inFlight.end()) {
				return false;  // given up on already, or not ours
			}
			const bool script = scriptKill == a_seq;
			if (script) {
				scriptKill.reset();
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
			return script && a_verdict == Protocol::HitVerdict::kApplied;
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
			if (heldKill && std::ranges::none_of(inFlight, [&](const Flying& a_f) { return a_f.seq == *heldKill; })) {
				heldKill.reset();  // answered or given up on: a later held blow of ours is sent again
				scriptKill.reset();
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
				// Its runner's NPC is dead: the copy dies too, once out of any kill move (the move kills it itself), and once its
				// runner's body, shot down in flight, lies still (it is killed where that body came to rest).
				if (a_seen.dead || a_seen.inKillMove || a_seen.crashing) {
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
			// Our player's blow would have killed it, and it was held at one point: a killing blow of all it stood at goes to
			// its runner, once while it is on its way; the copy stays at its floor until the answer (applied: the runner's death
			// kills it here; refused or lost: given back). A kill here no blow explains (a script's Kill or KillEssential, the
			// console's kill; HeldKills) goes the same way, as a death (kHitScript: it dies there, even with no health left,
			// unless it is essential there and the kill was no KillEssential: OnScriptKill), our player its killer only if the
			// script named them.
			if ((a_seen.heldByUs || a_seen.heldScript) && !heldKill) {
				const auto left = *Expected();
				if (left > EPSILON || a_seen.heldScript) {
					if (ours) {
						bookedHit = ours->at;  // its drop is this blow's: not booked again
					}
					const auto flags = a_seen.heldScript ? static_cast<std::uint8_t>(Protocol::kHitScript | (a_seen.heldEssential ? Protocol::kHitEssential : 0) |
					                                                                 (a_seen.heldByUs ? Protocol::kHitKilling : 0)) :
					                                       static_cast<std::uint8_t>((ours ? ours->flags : 0) | Protocol::kHitKilling);
					auto step = book(a_seen.heldScript ? (std::max)(left, a_seen.floor) : left, flags);
					heldKill = step.hit.seq;
					if (a_seen.heldScript) {
						scriptKill = step.hit.seq;
					}
					++(a_seen.heldScript ? a_counts.scriptKills : a_counts.heldKills);
					return step;
				}
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
		std::optional<std::uint32_t>     heldKill;  // alive here: our held killing blow on its way (its number)
		std::optional<std::uint32_t>     scriptKill;  // of them, a held kill no blow explains (kHitScript)
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

	// Kills of copies held here (DiesHere) and still owed, by the copy's form id (the kill hook adds them on any thread:
	// the caller locks).
	// - Our player's blow: sent as a killing blow at the copy's next look with its runner's word (Seen::heldByUs). Owed for
	//   OUR_BLOW_FRESH only, so a copy that unloads, or that this game comes to run, before such a look never sends a blow
	//   from long ago.
	// - A kill no blow explains (a script's Kill or KillEssential, a scene's execution, the console's kill: its health was
	//   not brought to nothing; Seen::heldScript). Its runner's game runs only its own quests and would never deal it, so
	//   it is owed until the NPC dies: sent to its runner as a death, again each SCRIPT_RESEND while it is a copy here that
	//   lives (a runner that could not apply it: not loaded there, handed on), and put on here at once if this game comes to
	//   run the NPC (Run). A quest here waiting on that death never waits for ever.
	//   A kill of a copy essential here is the engine's refusal and owes nothing (OwesScriptKill: the caller does not add
	//   it). One is a KillEssential when the copy was seen essential here a moment before (Look's a_essential; the
	//   script takes the flag off just before its kill): sent with kHitEssential, so its runner kills it essential or not.
	//   Its runner's answer settles it (Answered: it died there, or it lives there, essential, and refused it), and it is
	//   sent SCRIPT_SENDS_MOST times at most in any case; settled or not, it stays owed for a take-over here until the NPC
	//   dies, and a death of it said anywhere (Dead) ends it, so it never kills an NPC raised after.
	class HeldKills
	{
	public:
		static constexpr auto        OUR_BLOW_FRESH = 2s;
		static constexpr auto        SCRIPT_RESEND = 2s;
		static constexpr int         SCRIPT_SENDS_MOST = 10;
		static constexpr auto        ESSENTIAL_FRESH = 5s;  // seen essential this long before a kill: a KillEssential
		static constexpr std::size_t MOST = 256;

		void Blow(std::uint32_t a_id, Clock::time_point a_now)
		{
			if (auto* entry = Entry(a_id)) {
				entry->blowAt = a_now;
			}
		}

		// False: too many owed already, dropped.
		bool Script(std::uint32_t a_id, bool a_byUs, Clock::time_point a_now)
		{
			auto* entry = Entry(a_id);
			if (!entry) {
				return false;
			}
			entry->script = true;
			entry->scriptByUs = entry->scriptByUs || a_byUs;
			if (const auto it = essentialAt.find(a_id); it != essentialAt.end() && a_now - it->second <= ESSENTIAL_FRESH) {
				entry->essential = true;
			}
			return true;
		}

		struct Due
		{
			bool blow = false;       // our player's held blow: send a killing blow
			bool script = false;     // a held kill no blow explains: send it as a death
			bool essential = false;  // that kill was a KillEssential
			bool byUs = false;       // our player is its killer
		};

		// A look at a living copy. a_canSend: its runner's word is here (what is owed waits for it otherwise); a_essential:
		// it is essential here now. The blow is taken; the script's kill stays owed, due again SCRIPT_RESEND after until
		// its runner settles it or it went SCRIPT_SENDS_MOST times.
		Due Look(std::uint32_t a_id, bool a_canSend, bool a_essential, Clock::time_point a_now)
		{
			if (a_essential && (essentialAt.size() < MOST || essentialAt.contains(a_id))) {
				essentialAt[a_id] = a_now;
			}
			const auto it = entries.find(a_id);
			if (it == entries.end() || !a_canSend) {
				return {};
			}
			Due   due;
			auto& entry = it->second;
			if (entry.blowAt) {
				due.blow = a_now - *entry.blowAt < OUR_BLOW_FRESH;
				entry.blowAt.reset();
			}
			if (entry.script && !entry.settled && entry.sends < SCRIPT_SENDS_MOST && (!entry.sentAt || a_now - *entry.sentAt >= SCRIPT_RESEND)) {
				entry.sentAt = a_now;
				++entry.sends;
				due.script = true;
				due.essential = entry.essential;
			}
			due.byUs = due.blow || (due.script && entry.scriptByUs);
			if (!entry.script) {
				entries.erase(it);
			}
			return due;
		}

		// Its runner answered the kill no blow explains (applied: it died there; or it lives there, essential, and refused
		// it): it is sent no more, and stays owed for a take-over here.
		void Answered(std::uint32_t a_id)
		{
			if (const auto it = entries.find(a_id); it != entries.end()) {
				it->second.settled = true;
			}
		}

		// This game runs the NPC now: a kill no blow explains that is still owed is put on here (true: our player its
		// killer). Whatever else was held of it is owed no more.
		std::optional<bool> Run(std::uint32_t a_id)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return std::nullopt;
			}
			const auto owed = it->second.script ? std::optional<bool>{ it->second.scriptByUs } : std::nullopt;
			entries.erase(it);
			return owed;
		}

		// It died (here, or its runner or the session said so): nothing is owed, and it is raised, if ever, owing nothing.
		void Dead(std::uint32_t a_id)
		{
			entries.erase(a_id);
			essentialAt.erase(a_id);
		}

		// Our blows that were not looked at in time are owed no more; essential copies not seen so lately are forgotten.
		void Forget(Clock::time_point a_now)
		{
			std::erase_if(entries, [&](auto& a_entry) {
				auto& entry = a_entry.second;
				if (entry.blowAt && a_now - *entry.blowAt >= OUR_BLOW_FRESH) {
					entry.blowAt.reset();
				}
				return !entry.blowAt && !entry.script;
			});
			std::erase_if(essentialAt, [&](const auto& a_seen) { return a_now - a_seen.second > ESSENTIAL_FRESH; });
		}

		void Clear()
		{
			entries.clear();
			essentialAt.clear();
		}
		[[nodiscard]] bool        Owes(std::uint32_t a_id) const { return entries.contains(a_id); }
		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		struct Owed
		{
			std::optional<Clock::time_point> blowAt;
			bool                             script = false;
			bool                             scriptByUs = false;
			bool                             essential = false;  // a KillEssential
			bool                             settled = false;    // its runner answered it
			int                              sends = 0;
			std::optional<Clock::time_point> sentAt;
		};

		Owed* Entry(std::uint32_t a_id)
		{
			if (entries.size() >= MOST && !entries.contains(a_id)) {
				return nullptr;
			}
			return &entries[a_id];
		}

		std::unordered_map<std::uint32_t, Owed>              entries;
		std::unordered_map<std::uint32_t, Clock::time_point> essentialAt;  // copies seen essential here, when last
	};
}
