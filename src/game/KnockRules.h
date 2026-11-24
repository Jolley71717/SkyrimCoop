#pragma once

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Knockdowns: a living actor knocked down or ragdolled (docs/DESIGN.md, "Knockdowns"): the rules, free of the game
// (src/game/Knocks.h is the game's side; tests/KnockTests.cpp).
//
// The class is every way the engine throws a living actor to the ground and gets it up again (its knock state leaves
// normal): a push (Unrelenting Force, PushActorAway, an explosion that knocks down, a giant's or a mammoth's blow, a
// power bash's knockdown, a dragon's tail, a fall) and a paralysis (a spell, a poison, Ice Form, a paralysis
// enchantment), on every kind of actor (a player, a person, a creature, a dragon, a horse, a follower, a summon). Shown
// only in the game that runs it, a friend's game shows it standing (a copy is placed by its runner's states; the
// stand-in by its friend's), so:
// - Said: the game that runs an actor (its player's; an NPC's runner) watches its knock state (Watch). As it leaves normal
//   the knock begins; SAMPLE later it is said down: where it began, its launch (the way it moved since: the engine
//   moves an actor's reference with its ragdoll) and whether it is a paralysis (its Paralysis value up). As it gets up
//   (or stands again) it is said up, with where it rests.
// - Played: a friend's down, due when the actor that shows it here is drawn at that moment (Inbox), knocks that actor
//   when Judge says so: a push by the engine's own knock from just behind its launch, as hard as this game's own
//   replays say that speed needs (Scale, learnt from them); a paralysis by its Paralysis value. While it is knocked it is
//   held: not placed by the update hooks, its walk not told, the engine's own update run (HoldStep). Lying while its
//   friend's or runner's actor is still down, it is kept down by the same value (WantKeptDown), taken back at the up.
// - Asked: a knock this game gives a copy of an NPC another game runs (our player's explosion, a fall here) is held as
//   above and asked of the NPC's runner (a push only), which knocks its NPC the same way unless it is down already, dead
//   or busy (JudgeAsk), and then says it as its own: the asker's held copy joins it.
// The engine's details (whether its knock moves the reference with the ragdoll, how fast a magnitude throws, whether a
// Paralysis value alone keeps an actor down) are the rig's to confirm: the counts say each.
namespace KnockRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto  SAMPLE = 100ms;       // a knock is said this long after it began (its launch measured over it)
	inline constexpr auto  STALE = 2s;           // a friend's down that waited longer is dropped (an up never is)
	inline constexpr auto  TAKE_WITHIN = 1500ms;  // a knock played here that never took is let go after this
	inline constexpr auto  STAND_GRACE = 300ms;  // a held actor standing again this long is let go
	inline constexpr auto  UP_WAIT = 8s;         // ... and one still down this long after its friend's up
	inline constexpr auto  MAX_DOWN = 40s;       // ... and one with no up this long after its down (a paralysis lasts 30 s at most)
	inline constexpr auto  PARALYZE_CALL = 250ms;  // a paralysis by value not down by then gets the engine's paralysis knock
	inline constexpr float REACH = 1500.0f;      // u: the actor shown here is this near where the knock began
	inline constexpr float BEHIND = 100.0f;      // u: the knock comes from this far behind the launch
	inline constexpr float MIN_SPEED = 30.0f;    // u/s: a slower launch is a fall in place
	inline constexpr float MIN_MAGNITUDE = 1.0f;
	inline constexpr float MAX_MAGNITUDE = 60.0f;
	inline constexpr float DEFAULT_SPEED_PER_MAGNITUDE = 120.0f;  // u/s a magnitude throws, until this game's own replays say
	inline constexpr float MIN_SPEED_PER_MAGNITUDE = 10.0f;
	inline constexpr float MAX_SPEED_PER_MAGNITUDE = 2000.0f;
	inline constexpr std::size_t MAX_HELD = 32;  // actors held by knocks at once

	// ---- the knock state ----

	// The engine's knock state (Actor's ActorState1::knockState, KNOCK_STATE_ENUM): 0 normal; 1 explode, 2 explode lead-in,
	// 3 out, 4 out lead-in, 5 queued, 7 down: knocked; 6 get-up.
	enum class Phase : std::uint8_t
	{
		kStanding,
		kDown,
		kGettingUp,
	};

	[[nodiscard]] constexpr Phase PhaseOf(std::uint32_t a_knockState)
	{
		return a_knockState == 0 ? Phase::kStanding : a_knockState == 6 ? Phase::kGettingUp : Phase::kDown;
	}

	// Lying on the ground (out, down), not on its way there (thrown, a lead-in, queued).
	[[nodiscard]] constexpr bool Resting(std::uint32_t a_knockState)
	{
		return a_knockState == 3 || a_knockState == 7;
	}

	[[nodiscard]] constexpr std::string_view NameOf(Phase a_phase)
	{
		return a_phase == Phase::kStanding ? "standing" : a_phase == Phase::kDown ? "down" : "gettingUp";
	}

	[[nodiscard]] constexpr std::string_view NameOf(Protocol::KnockKind a_kind)
	{
		return a_kind == Protocol::KnockKind::kParalysis ? "paralysis" : "push";
	}

	struct Vec
	{
		float x = 0, y = 0, z = 0;
	};

	[[nodiscard]] inline float Length(const Vec& a_v)
	{
		return std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z);
	}

	[[nodiscard]] inline float Distance(const Vec& a_a, const Vec& a_b)
	{
		return Length({ a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z });
	}

	// ---- saying ours ----

	// What Watch says of one actor: a knock's down (where it began, its launch, its kind, the moment it began) or its up
	// (where it rests, the moment it got up).
	struct Said
	{
		Protocol::KnockPhase phase = Protocol::KnockPhase::kDown;
		Protocol::KnockKind  kind = Protocol::KnockKind::kPush;
		Vec                  at;
		Vec                  launch;
		Clock::time_point    when;
	};

	// The knocks of the actors this game runs (our player, the NPCs it runs), each by its id, from its knock state once a
	// tick. Also the knocks this game's world gives copies and stand-ins (the same rules: a native knock's down is asked of
	// its runner).
	class Watch
	{
	public:
		// One actor's tick: its phase, where it is, whether its Paralysis value is up. What is to be said of it now (a down,
		// an up, or a down and its up when a knock was over within SAMPLE).
		std::vector<Said> Step(std::uint32_t a_id, Phase a_phase, const Vec& a_at, bool a_paralysed, Clock::time_point a_now)
		{
			std::vector<Said> out;
			auto              it = actors.find(a_id);
			if (it == actors.end()) {
				if (a_phase == Phase::kDown) {
					actors.emplace(a_id, Knock{ .phase = Phase::kDown, .start = a_at, .began = a_now, .paralysed = a_paralysed });
				}
				return out;
			}
			auto& knock = it->second;
			if (knock.phase == Phase::kGettingUp) {
				if (a_phase == Phase::kDown) {
					knock = Knock{ .phase = Phase::kDown, .start = a_at, .began = a_now, .paralysed = a_paralysed };  // down again
				} else if (a_phase == Phase::kStanding) {
					actors.erase(it);
				}
				return out;
			}
			// Down.
			knock.paralysed = knock.paralysed || a_paralysed;
			if (!knock.said && (a_phase != Phase::kDown || a_now - knock.began >= SAMPLE)) {
				knock.said = true;
				out.push_back(DownOf(knock, a_at, a_now));
			}
			if (a_phase != Phase::kDown) {
				out.push_back(Said{ .phase = Protocol::KnockPhase::kUp, .kind = KindOf(knock), .at = a_at, .when = a_now });
				if (a_phase == Phase::kStanding) {
					actors.erase(it);
				} else {
					knock.phase = Phase::kGettingUp;
				}
			}
			return out;
		}

		// An actor that is gone (dead, unloaded, no longer ours): nothing more is said of it; its up never comes, and the
		// others let go of it by its death or their own time.
		void Forget(std::uint32_t a_id) { actors.erase(a_id); }

		template <class Gone>
		void ForgetIf(Gone a_gone)
		{
			std::erase_if(actors, [&](const auto& a_entry) { return a_gone(a_entry.first); });
		}

		void Clear() { actors.clear(); }
		[[nodiscard]] std::size_t Size() const { return actors.size(); }
		[[nodiscard]] bool        Down(std::uint32_t a_id) const
		{
			const auto it = actors.find(a_id);
			return it != actors.end() && it->second.phase == Phase::kDown;
		}

	private:
		struct Knock
		{
			Phase             phase = Phase::kDown;
			Vec               start;
			Clock::time_point began;
			bool              paralysed = false;
			bool              said = false;
		};

		static Protocol::KnockKind KindOf(const Knock& a_knock)
		{
			return a_knock.paralysed ? Protocol::KnockKind::kParalysis : Protocol::KnockKind::kPush;
		}

		// A down: its launch is the way it moved since it began (none for a paralysis: it falls where it stands), at most
		// Protocol::MAX_KNOCK_SPEED.
		static Said DownOf(const Knock& a_knock, const Vec& a_at, Clock::time_point a_now)
		{
			Said said{ .phase = Protocol::KnockPhase::kDown, .kind = KindOf(a_knock), .at = a_knock.start, .when = a_knock.began };
			const float seconds = std::chrono::duration<float>(a_now - a_knock.began).count();
			if (said.kind == Protocol::KnockKind::kPush && seconds > 0.0f) {
				Vec         v{ (a_at.x - a_knock.start.x) / seconds, (a_at.y - a_knock.start.y) / seconds, (a_at.z - a_knock.start.z) / seconds };
				const float speed = Length(v);
				if (!std::isfinite(speed)) {
					v = {};
				} else if (speed > Protocol::MAX_KNOCK_SPEED) {
					const float k = Protocol::MAX_KNOCK_SPEED / speed * 0.999f;
					v = { v.x * k, v.y * k, v.z * k };
				}
				said.launch = v;
			}
			return said;
		}

		std::unordered_map<std::uint32_t, Knock> actors;
	};

	// A said knock as the message carries it (sentMs: the moment it began or got up, on this game's clock).
	[[nodiscard]] inline Protocol::Knock ToWire(const Protocol::PairSide& a_actor, const Said& a_said, std::uint32_t a_sentMs, bool a_ask = false)
	{
		Protocol::Knock knock{ .actor = a_actor,
			.sentMs = a_sentMs,
			.phase = a_ask ? Protocol::KnockPhase::kAsk : a_said.phase,
			.kind = a_said.kind,
			.x = a_said.at.x,
			.y = a_said.at.y,
			.z = a_said.at.z };
		if (a_said.phase != Protocol::KnockPhase::kUp) {
			knock.vx = a_said.launch.x;
			knock.vy = a_said.launch.y;
			knock.vz = a_said.launch.z;
		}
		return knock;
	}

	// Knocks to send, in messages of at most MAX_KNOCKS.
	[[nodiscard]] inline std::vector<Protocol::Knocks> Batch(std::vector<Protocol::Knock> a_knocks)
	{
		std::vector<Protocol::Knocks> out;
		for (std::size_t i = 0; i < a_knocks.size(); i += Protocol::MAX_KNOCKS) {
			Protocol::Knocks msg;
			const auto       end = (std::min)(a_knocks.size(), i + Protocol::MAX_KNOCKS);
			msg.knocks.assign(a_knocks.begin() + static_cast<std::ptrdiff_t>(i), a_knocks.begin() + static_cast<std::ptrdiff_t>(end));
			out.push_back(std::move(msg));
		}
		return out;
	}

	// ---- knocking an actor here ----

	// How hard and from where the engine's knock (AIProcess::KnockExplosion: away from a place, by a magnitude) throws an
	// actor at a_at along a_launch. A launch slower than MIN_SPEED is a fall in place: from just below it, the least
	// magnitude.
	struct Push
	{
		Vec   from;
		float magnitude = MIN_MAGNITUDE;
		bool  inPlace = false;
	};

	[[nodiscard]] inline Push PushFor(const Vec& a_at, const Vec& a_launch, float a_speedPerMagnitude)
	{
		Push        push;
		const float speed = Length(a_launch);
		const float flat = std::sqrt(a_launch.x * a_launch.x + a_launch.y * a_launch.y);
		if (!std::isfinite(speed) || speed < MIN_SPEED || flat < 1.0f) {
			push.from = { a_at.x, a_at.y, a_at.z - BEHIND };
			push.inPlace = true;
			return push;
		}
		push.from = { a_at.x - a_launch.x / flat * BEHIND, a_at.y - a_launch.y / flat * BEHIND, a_at.z };
		const float perMagnitude = std::clamp(a_speedPerMagnitude, MIN_SPEED_PER_MAGNITUDE, MAX_SPEED_PER_MAGNITUDE);
		push.magnitude = std::clamp(speed / perMagnitude, MIN_MAGNITUDE, MAX_MAGNITUDE);
		return push;
	}

	// How fast a magnitude of the engine's knock throws an actor in this game, learnt from its own replays: the median of
	// the last few (speed it was thrown at / magnitude), DEFAULT_SPEED_PER_MAGNITUDE until there is one. A replay that hardly
	// moved (a fall in place, one against a wall) teaches nothing.
	class Scale
	{
	public:
		static constexpr std::size_t KEEP = 8;

		void Note(float a_magnitude, float a_speed)
		{
			if (!(a_magnitude > 0.0f) || !std::isfinite(a_speed) || a_speed < MIN_SPEED) {
				++ignored;
				return;
			}
			samples.push_back(std::clamp(a_speed / a_magnitude, MIN_SPEED_PER_MAGNITUDE, MAX_SPEED_PER_MAGNITUDE));
			if (samples.size() > KEEP) {
				samples.pop_front();
			}
			++learnt;
		}

		[[nodiscard]] float SpeedPerMagnitude() const
		{
			if (samples.empty()) {
				return DEFAULT_SPEED_PER_MAGNITUDE;
			}
			std::vector<float> sorted(samples.begin(), samples.end());
			std::ranges::sort(sorted);
			const auto n = sorted.size();
			return n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0f;
		}

		void Clear() { samples.clear(); }

		std::uint64_t learnt = 0;
		std::uint64_t ignored = 0;

	private:
		std::deque<float> samples;
	};

	// A friend's knocks, each due when the actor that shows it is drawn at the moment it was said of (the stand-ins' delay
	// behind), in order. A down that waited longer than STALE is dropped (the actor is up by now there); an up never is:
	// the actor held here waits for it.
	class Inbox
	{
	public:
		static constexpr std::size_t MAX = 64;

		void Push(const Protocol::Knock& a_knock, std::uint32_t a_sender, std::optional<Clock::time_point> a_at, Clock::time_point a_now)
		{
			const auto at = (std::min)(a_at.value_or(a_now), a_now + STALE);  // a clock read wrong never holds the queue up
			const auto pos = std::upper_bound(queue.begin(), queue.end(), at, [](Clock::time_point a_t, const Entry& a_e) { return a_t < a_e.at; });
			queue.insert(pos, Entry{ at, a_sender, a_knock });
			while (queue.size() > MAX) {
				// The oldest down goes first; ups are kept while there is a down to drop.
				auto drop = std::ranges::find_if(queue, [](const Entry& a_e) { return a_e.knock.phase != Protocol::KnockPhase::kUp; });
				queue.erase(drop != queue.end() ? drop : queue.begin());
				++dropped;
			}
		}

		struct Due
		{
			std::uint32_t   sender = 0;
			Protocol::Knock knock;
		};

		[[nodiscard]] std::vector<Due> Take(Clock::time_point a_drawn)
		{
			std::vector<Due> out;
			while (!queue.empty() && queue.front().at <= a_drawn) {
				if (queue.front().knock.phase != Protocol::KnockPhase::kUp && a_drawn - queue.front().at > STALE) {
					++late;
				} else {
					out.push_back({ queue.front().sender, queue.front().knock });
				}
				queue.pop_front();
			}
			return out;
		}

		void Forget(std::uint32_t a_sender)
		{
			std::erase_if(queue, [&](const Entry& a_e) { return a_e.sender == a_sender; });
		}

		void Clear() { queue.clear(); }
		[[nodiscard]] std::size_t Size() const { return queue.size(); }

		std::uint64_t late = 0;
		std::uint64_t dropped = 0;

	private:
		struct Entry
		{
			Clock::time_point at;
			std::uint32_t     sender = 0;
			Protocol::Knock   knock;
		};
		std::deque<Entry> queue;
	};

	// What shows a knocked actor here.
	enum class Shown : std::uint8_t
	{
		kNone,       // nothing here (not loaded, no stand-in, no copy)
		kOurPlayer,  // our player
		kStandIn,    // a friend's stand-in
		kCopy,       // a copy of an NPC another game runs
		kRunHere,    // an NPC this game runs
	};

	struct Facts
	{
		Shown shown = Shown::kNone;
		bool  alive = true;
		bool  busy = false;        // in a pair (a kill move, or held by one here), in furniture or on a mount
		bool  knockedHere = false;  // already knocked in this game (by its world, or a knock played before)
		float drift = 0.0f;        // u: the actor here from where the knock began
		bool  full = false;        // MAX_HELD actors held already
	};

	enum class Verdict : std::uint8_t
	{
		kPlay,
		kNoActor,
		kOurs,     // our player or an NPC we run: its knocks are this game's (the sender's book was behind)
		kDead,
		kAlready,  // knocked here already: held as the friend's from now (joined), not knocked again
		kBusy,
		kFar,
		kFull,
		kCount,
	};
	inline constexpr std::array<std::string_view, static_cast<std::size_t>(Verdict::kCount)> VERDICT_NAMES{ "play", "noActor", "ours", "dead", "already", "busy", "far",
		"full" };

	[[nodiscard]] constexpr std::string_view NameOf(Verdict a_verdict)
	{
		const auto i = static_cast<std::size_t>(a_verdict);
		return i < VERDICT_NAMES.size() ? VERDICT_NAMES[i] : "?";
	}

	// A friend's down, on the actor that shows it here.
	[[nodiscard]] inline Verdict Judge(const Facts& a_facts)
	{
		if (a_facts.shown == Shown::kNone) {
			return Verdict::kNoActor;
		}
		if (a_facts.shown == Shown::kOurPlayer || a_facts.shown == Shown::kRunHere) {
			return Verdict::kOurs;
		}
		if (!a_facts.alive) {
			return Verdict::kDead;
		}
		if (a_facts.knockedHere) {
			return Verdict::kAlready;
		}
		if (a_facts.busy) {
			return Verdict::kBusy;
		}
		if (!(a_facts.drift <= REACH)) {
			return Verdict::kFar;
		}
		if (a_facts.full) {
			return Verdict::kFull;
		}
		return Verdict::kPlay;
	}

	template <class E, std::size_t N>
	struct Counts
	{
		std::array<std::uint64_t, N> n{};
		const std::array<std::string_view, N>* names = nullptr;

		void Add(E a_e) { ++n[static_cast<std::size_t>(a_e)]; }
		[[nodiscard]] std::uint64_t Of(E a_e) const { return n[static_cast<std::size_t>(a_e)]; }
		// Every one but the first ("far:1,dead:2"); "none" when there were none.
		[[nodiscard]] std::string Others() const
		{
			std::string out;
			for (std::size_t i = 1; i < N; ++i) {
				if (n[i] != 0) {
					out += (out.empty() ? "" : ",") + std::string{ (*names)[i] } + ":" + std::to_string(n[i]);
				}
			}
			return out.empty() ? "none" : out;
		}
	};

	struct VerdictCounts : Counts<Verdict, static_cast<std::size_t>(Verdict::kCount)>
	{
		VerdictCounts() { names = &VERDICT_NAMES; }
	};

	// A knock asked of this game, for an NPC it runs (a friend's game knocked its copy of it).
	struct AskFacts
	{
		bool  ours = false;  // this game runs the NPC and has it here
		bool  alive = true;
		bool  down = false;  // knocked here already (then its own down is said, and the asker's copy joins it)
		bool  busy = false;
		float drift = 0.0f;  // u: our NPC from where the asker's copy was knocked
		Protocol::KnockKind kind = Protocol::KnockKind::kPush;
	};

	enum class AskVerdict : std::uint8_t
	{
		kApply,
		kNotOurs,
		kDead,
		kDown,
		kBusy,
		kFar,
		kParalysis,  // a paralysis is its effect's: the effect reaches the runner on its own, not as a knock
		kCount,
	};
	inline constexpr std::array<std::string_view, static_cast<std::size_t>(AskVerdict::kCount)> ASK_NAMES{ "apply", "notOurs", "dead", "down", "busy", "far",
		"paralysis" };

	[[nodiscard]] constexpr std::string_view NameOf(AskVerdict a_verdict)
	{
		const auto i = static_cast<std::size_t>(a_verdict);
		return i < ASK_NAMES.size() ? ASK_NAMES[i] : "?";
	}

	[[nodiscard]] inline AskVerdict JudgeAsk(const AskFacts& a_facts)
	{
		if (!a_facts.ours) {
			return AskVerdict::kNotOurs;
		}
		if (a_facts.kind == Protocol::KnockKind::kParalysis) {
			return AskVerdict::kParalysis;
		}
		if (!a_facts.alive) {
			return AskVerdict::kDead;
		}
		if (a_facts.down) {
			return AskVerdict::kDown;
		}
		if (a_facts.busy) {
			return AskVerdict::kBusy;
		}
		if (!(a_facts.drift <= REACH)) {
			return AskVerdict::kFar;
		}
		return AskVerdict::kApply;
	}

	struct AskCounts : Counts<AskVerdict, static_cast<std::size_t>(AskVerdict::kCount)>
	{
		AskCounts() { names = &ASK_NAMES; }
	};

	// ---- holding a knocked actor here ----

	enum class Hold : std::uint8_t
	{
		kHold,    // still knocked (or waiting for it to take): not placed, the engine's update
		kEnded,   // standing again STAND_GRACE after it was knocked: placed by its states again
		kNoTake,  // never knocked by TAKE_WITHIN: the knock did not take
		kGaveUp,  // still down UP_WAIT after its friend's up, or MAX_DOWN with no up
	};

	inline constexpr std::array<std::string_view, 4> HOLD_NAMES{ "held", "ended", "noTake", "gaveUp" };

	[[nodiscard]] constexpr std::string_view NameOf(Hold a_hold)
	{
		return HOLD_NAMES[static_cast<std::size_t>(a_hold)];
	}

	// One actor held here, from its hold (a_since): a_phase now, whether it was ever seen knocked, when its friend's up came
	// (none: not yet), since when it stands (none: it does not).
	[[nodiscard]] inline Hold HoldStep(Clock::time_point a_since, Clock::time_point a_now, Phase a_phase, bool a_everKnocked, std::optional<Clock::time_point> a_upAt,
		std::optional<Clock::time_point> a_standingSince)
	{
		if (a_phase != Phase::kStanding) {
			if (a_upAt ? a_now - *a_upAt >= UP_WAIT : a_now - a_since >= MAX_DOWN) {
				return Hold::kGaveUp;
			}
			return Hold::kHold;
		}
		if (!a_everKnocked) {
			return a_now - a_since >= TAKE_WITHIN ? Hold::kNoTake : Hold::kHold;
		}
		return a_standingSince && a_now - *a_standingSince >= STAND_GRACE ? Hold::kEnded : Hold::kHold;
	}

	// Whether a held actor is to be kept down by its Paralysis value now: its friend's (or runner's) actor is down (its down
	// heard, no up yet), and it lies on the ground here (a push's actor once it lands; a paralysis's at once, by
	// a_lyingOrParalysis), and it is not kept already.
	[[nodiscard]] inline bool WantKeptDown(bool a_heardDown, bool a_upHeard, bool a_kept, bool a_lyingOrParalysis, bool a_alive)
	{
		return a_heardDown && !a_upHeard && !a_kept && a_lyingOrParalysis && a_alive;
	}
}
