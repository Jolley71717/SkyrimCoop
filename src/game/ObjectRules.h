#pragma once

#include "Protocol.h"
#include "TokenBucket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <vector>

// Loose objects, the rules (docs/DESIGN.md, "Loose objects"; the game's side is src/game/LooseObjects.cpp, the
// server's common/ObjectBook.h). The sister project's moved objects, carried to Skyrim's Havok bodies:
// - The class (Movable): a reference of the load order, not an actor, a projectile, an explosion or a hazard, enabled,
//   with its 3D, and one rigid body whose motion type is loose (dynamic, sphere, box or thin box inertia). Judged by its
//   physics, never by a list of forms: a mod's clutter is shared as the game's is; a door, a lever, a trap's moving part
//   (keyframed) or a static never are.
// - Frames: what a player holds is said in their frame (ToHolder) and drawn in front of their stand-in (FromHolder).
// - Causes: whose move it is. A grab, a hit, an explosion, a projectile's path, a body standing by it: ours (our player,
//   an NPC this game runs) or theirs (a friend's stand-in, a copy of an NPC another game runs, an explosion or a
//   projectile one of those set off). Ours to report when our strongest cause that reached it is at least theirs.
// - Settle: at rest once it moved under MOVE_EPSILON and turned under TURN_EPSILON for STILL_FOR, or GIVE_UP after its move
//   began.
// - Pacer: rests go out BURST at once, then PER_SECOND; one that waits past WAIT_MAX is dropped and counted.
// - Quiet: what a friend's move put somewhere is not ours to report for QUIET (our physics settling it); Echo: at most
//   ECHO_MAX of our rests of one object go back against a friend's in ECHO_WINDOW.
// - Waiting: the session's places for objects not loaded here yet (at most MAX_WAITING).
// All of it in game units (the game side converts to and from Havok's with bhkWorld's scale). No game here:
// tests/ObjectTests.cpp.
namespace ObjectRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;
	using Quat = Protocol::Quat;

	struct Vec
	{
		float x = 0, y = 0, z = 0;
		bool operator==(const Vec&) const = default;
	};
	[[nodiscard]] inline Vec   operator+(const Vec& a_a, const Vec& a_b) { return { a_a.x + a_b.x, a_a.y + a_b.y, a_a.z + a_b.z }; }
	[[nodiscard]] inline Vec   operator-(const Vec& a_a, const Vec& a_b) { return { a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z }; }
	[[nodiscard]] inline Vec   operator*(const Vec& a_v, float a_s) { return { a_v.x * a_s, a_v.y * a_s, a_v.z * a_s }; }
	[[nodiscard]] inline float Dot(const Vec& a_a, const Vec& a_b) { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }
	[[nodiscard]] inline float Length(const Vec& a_v) { return std::sqrt(Dot(a_v, a_v)); }
	[[nodiscard]] inline float Distance(const Vec& a_a, const Vec& a_b) { return Length(a_a - a_b); }
	[[nodiscard]] inline bool  Finite(const Vec& a_v) { return std::isfinite(a_v.x) && std::isfinite(a_v.y) && std::isfinite(a_v.z); }

	// ---- rotations ----

	[[nodiscard]] inline Quat Mul(const Quat& a_a, const Quat& a_b)
	{
		return {
			a_a.w * a_b.x + a_a.x * a_b.w + a_a.y * a_b.z - a_a.z * a_b.y,
			a_a.w * a_b.y - a_a.x * a_b.z + a_a.y * a_b.w + a_a.z * a_b.x,
			a_a.w * a_b.z + a_a.x * a_b.y - a_a.y * a_b.x + a_a.z * a_b.w,
			a_a.w * a_b.w - a_a.x * a_b.x - a_a.y * a_b.y - a_a.z * a_b.z,
		};
	}
	[[nodiscard]] inline Quat Conj(const Quat& a_q) { return { -a_q.x, -a_q.y, -a_q.z, a_q.w }; }
	[[nodiscard]] inline Quat Normalized(const Quat& a_q)
	{
		const auto length = std::sqrt(a_q.x * a_q.x + a_q.y * a_q.y + a_q.z * a_q.z + a_q.w * a_q.w);
		if (!std::isfinite(length) || length < 1e-6f) {
			return {};
		}
		return { a_q.x / length, a_q.y / length, a_q.z / length, a_q.w / length };
	}
	// a_v turned by a_q.
	[[nodiscard]] inline Vec Rotate(const Quat& a_q, const Vec& a_v)
	{
		const auto r = Mul(Mul(a_q, Quat{ a_v.x, a_v.y, a_v.z, 0.0f }), Conj(a_q));
		return { r.x, r.y, r.z };
	}
	// The turn of a heading: the game's heading is clockwise from north (+y) seen from above, so a heading h is a turn
	// of -h about the up axis (+z): it takes ahead (0, 1, 0) to (sin h, cos h, 0).
	[[nodiscard]] inline Quat Heading(float a_heading)
	{
		return { 0.0f, 0.0f, std::sin(-a_heading * 0.5f), std::cos(-a_heading * 0.5f) };
	}
	// The angle between two rotations, 0 to pi (a quaternion and its negative are the same rotation).
	[[nodiscard]] inline float Angle(const Quat& a_a, const Quat& a_b)
	{
		const auto dot = std::fabs(a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z + a_a.w * a_b.w);
		return 2.0f * std::acos((std::min)(1.0f, dot));
	}
	// From a_a towards a_b by a_t (0 to 1), the short way.
	[[nodiscard]] inline Quat Nlerp(const Quat& a_a, const Quat& a_b, float a_t)
	{
		const auto dot = a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z + a_a.w * a_b.w;
		const auto s = dot < 0.0f ? -1.0f : 1.0f;
		return Normalized({ a_a.x + (s * a_b.x - a_a.x) * a_t, a_a.y + (s * a_b.y - a_a.y) * a_t, a_a.z + (s * a_b.z - a_a.z) * a_t,
			a_a.w + (s * a_b.w - a_a.w) * a_t });
	}

	struct Pose
	{
		Vec  at;
		Quat rot;
		bool operator==(const Pose&) const = default;
	};

	// ---- the holder's frame ----

	// What our player (standing at a_holder, facing a_heading) holds at a_object, in their frame.
	[[nodiscard]] inline Protocol::ObjectHold ToHolder(const Protocol::ObjectKey& a_key, const Vec& a_holder, float a_heading, const Pose& a_object)
	{
		const auto d = a_object.at - a_holder;
		const Vec  right{ std::cos(a_heading), -std::sin(a_heading), 0.0f };
		const Vec  ahead{ std::sin(a_heading), std::cos(a_heading), 0.0f };
		Protocol::ObjectHold hold;
		hold.key = a_key;
		hold.right = Dot(d, right);
		hold.ahead = Dot(d, ahead);
		hold.up = d.z;
		hold.rot = Normalized(Mul(Conj(Heading(a_heading)), a_object.rot));
		return hold;
	}

	// Where a hold puts the object in front of the actor that shows its holder here (standing at a_holder, facing
	// a_heading).
	[[nodiscard]] inline Pose FromHolder(const Protocol::ObjectHold& a_hold, const Vec& a_holder, float a_heading)
	{
		const Vec right{ std::cos(a_heading), -std::sin(a_heading), 0.0f };
		const Vec ahead{ std::sin(a_heading), std::cos(a_heading), 0.0f };
		return { a_holder + right * a_hold.right + ahead * a_hold.ahead + Vec{ 0.0f, 0.0f, a_hold.up }, Normalized(Mul(Heading(a_heading), a_hold.rot)) };
	}

	// A drawn hold eased towards the newest (time constant EASE: it follows the stand-in's hands without jumping).
	inline constexpr float EASE_SECONDS = 0.06f;
	[[nodiscard]] inline Pose Ease(const Pose& a_drawn, const Pose& a_target, float a_dt)
	{
		if (!(a_dt > 0.0f)) {
			return a_drawn;
		}
		const auto t = 1.0f - std::exp(-a_dt / EASE_SECONDS);
		return { a_drawn.at + (a_target.at - a_drawn.at) * t, Nlerp(a_drawn.rot, a_target.rot, t) };
	}

	// ---- the class ----

	// hkpMotion::MotionType's numbers.
	enum class Motion : std::uint8_t
	{
		kInvalid = 0,
		kDynamic = 1,
		kSphereInertia = 2,
		kBoxInertia = 3,
		kKeyframed = 4,
		kFixed = 5,
		kThinBoxInertia = 6,
		kCharacter = 7,
	};

	// Whether physics moves a body of this motion type (a keyframed one is moved by its animation or a script, a fixed
	// one never, a character's by its controller).
	[[nodiscard]] constexpr bool Loose(Motion a_motion)
	{
		switch (a_motion) {
		case Motion::kDynamic:
		case Motion::kSphereInertia:
		case Motion::kBoxInertia:
		case Motion::kThinBoxInertia:
			return true;
		default:
			return false;
		}
	}

	// What the game says of a reference, for Movable.
	struct Facts
	{
		bool          runtime = false;   // made as the game ran (FF): no other game has it
		bool          actor = false;     // an actor (alive or dead: bodies are the ragdolls' business)
		bool          transient = false;  // a projectile, an explosion, a hazard
		bool          enabled = true;
		bool          has3D = false;
		bool          inAlias = false;   // a quest alias holds it
		std::uint32_t bodies = 0;        // rigid bodies in its 3D
		Motion        motion = Motion::kInvalid;  // the one body's (when there is one)
	};

	[[nodiscard]] constexpr bool Movable(const Facts& a_facts)
	{
		return !a_facts.runtime && !a_facts.actor && !a_facts.transient && a_facts.enabled && a_facts.has3D && !a_facts.inAlias && a_facts.bodies == 1 &&
		       Loose(a_facts.motion);
	}

	// ---- whose move ----

	enum class Strength : std::uint8_t
	{
		kNone = 0,
		kBody = 1,    // an actor stood by it (walked into it)
		kStrike = 2,  // a hit, an explosion, a projectile's path
		kGrab = 3,    // held
	};

	inline constexpr float PLAYER_REACH = 160.0f;      // a player's body
	inline constexpr float NPC_REACH = 200.0f;         // an NPC's (a bigger body, on average)
	inline constexpr float BLAST_MARGIN = 128.0f;      // past an explosion's own radius
	inline constexpr float SHOT_REACH = 256.0f;        // from a projectile's path
	inline constexpr float CONE_REACH = 512.0f;        // from a cone's (Unrelenting Force widens as it goes)
	inline constexpr auto  HIT_HOLD = 3s;              // a hit counts this long
	inline constexpr auto  BLAST_HOLD = 4s;            // an explosion's reach counts this long
	inline constexpr auto  SHOT_HOLD = 2s;             // a projectile's path counts this long
	inline constexpr auto  BODY_HOLD = 1s;             // where a body stood counts this long (between two looks)

	// Something that moves objects: where (a point, or a path from `from` to `to`), how far it reaches, how strong a claim
	// it makes, whose it is, until when it counts. `object` != 0: it reaches that object alone (a hit, a grab).
	struct Cause
	{
		Vec               from;
		Vec               to;
		float             reach = 0.0f;
		Strength          strength = Strength::kNone;
		bool              ours = false;
		Clock::time_point until{};
		std::uint32_t     object = 0;
	};

	// The distance from a_p to the path from a_a to a_b.
	[[nodiscard]] inline float DistanceToPath(const Vec& a_p, const Vec& a_a, const Vec& a_b)
	{
		const auto ab = a_b - a_a;
		const auto length2 = Dot(ab, ab);
		if (length2 < 1e-6f) {
			return Distance(a_p, a_a);
		}
		const auto t = std::clamp(Dot(a_p - a_a, ab) / length2, 0.0f, 1.0f);
		return Distance(a_p, a_a + ab * t);
	}

	struct Claim
	{
		Strength ours = Strength::kNone;
		Strength theirs = Strength::kNone;

		// Ours to report: a cause of ours reached it and none of a friend's stronger.
		[[nodiscard]] bool Ours() const { return ours != Strength::kNone && ours >= theirs; }
	};

	class Causes
	{
	public:
		void Add(const Cause& a_cause)
		{
			if (a_cause.strength != Strength::kNone && Finite(a_cause.from) && Finite(a_cause.to) && causes.size() < MAX_CAUSES) {
				causes.push_back(a_cause);
			}
		}
		void Prune(Clock::time_point a_now)
		{
			std::erase_if(causes, [&](const Cause& a_cause) { return a_cause.until < a_now; });
		}
		void Clear() { causes.clear(); }
		[[nodiscard]] std::size_t Size() const { return causes.size(); }

		// Who moved object a_object: the causes that reach where it lay before it moved (a_before) or where it is now
		// (a_now: a blast throws it past its reach between two looks), counting at a_time.
		[[nodiscard]] Claim Judge(std::uint32_t a_object, const Vec& a_before, const Vec& a_at, Clock::time_point a_time) const
		{
			Claim claim;
			for (const auto& cause : causes) {
				if (cause.until < a_time) {
					continue;
				}
				bool reaches = false;
				if (cause.object != 0) {
					reaches = cause.object == a_object;
				} else {
					reaches = DistanceToPath(a_before, cause.from, cause.to) <= cause.reach || DistanceToPath(a_at, cause.from, cause.to) <= cause.reach;
				}
				if (!reaches) {
					continue;
				}
				auto& side = cause.ours ? claim.ours : claim.theirs;
				side = (std::max)(side, cause.strength);
			}
			return claim;
		}

		static constexpr std::size_t MAX_CAUSES = 1024;

	private:
		std::vector<Cause> causes;
	};

	// ---- settling ----

	inline constexpr float MOVE_EPSILON = 1.0f;   // units: less is not a move
	inline constexpr float TURN_EPSILON = 0.02f;  // radians
	inline constexpr auto  STILL_FOR = 600ms;     // unmoved this long: at rest
	inline constexpr auto  MIN_FLIGHT = 300ms;    // never at rest sooner after its move began
	inline constexpr auto  GIVE_UP = 15s;         // at rest after this whatever it does (it swings for ever)

	// Whether a_b is a move from a_a.
	[[nodiscard]] inline bool Moved(const Pose& a_a, const Pose& a_b)
	{
		return Distance(a_a.at, a_b.at) > MOVE_EPSILON || Angle(a_a.rot, a_b.rot) > TURN_EPSILON;
	}

	// One object's move, from its start to its rest.
	class Settle
	{
	public:
		Settle() = default;
		Settle(const Pose& a_pose, Clock::time_point a_now) :
			began(a_now), lastMove(a_now), still(a_pose)
		{}

		// The newest look at it: true once it is at rest (and from then on).
		bool Look(const Pose& a_pose, Clock::time_point a_now)
		{
			if (rested) {
				return true;
			}
			if (Moved(still, a_pose)) {
				still = a_pose;
				lastMove = a_now;
			}
			const bool stillLongEnough = a_now - lastMove >= STILL_FOR && a_now - began >= MIN_FLIGHT;
			rested = stillLongEnough || a_now - began >= GIVE_UP;
			gaveUp = rested && !stillLongEnough;
			return rested;
		}

		[[nodiscard]] const Pose&       Where() const { return still; }
		[[nodiscard]] bool              Rested() const { return rested; }
		[[nodiscard]] bool              GaveUp() const { return gaveUp; }
		[[nodiscard]] Clock::time_point Began() const { return began; }

	private:
		Clock::time_point began{};
		Clock::time_point lastMove{};
		Pose              still;
		bool              rested = false;
		bool              gaveUp = false;
	};

	// Where each loose object near us last lay, so a look can tell one that started moving (at most MAX_SEEN; one not
	// seen for FORGET_AFTER is forgotten).
	class Seen
	{
	public:
		static constexpr std::size_t MAX_SEEN = 1024;
		static constexpr auto        FORGET_AFTER = 5s;

		// What a look at a_id found: started moving (from the pose returned), or not (none). The first look at an object
		// is its baseline: nothing moved.
		std::optional<Pose> Look(std::uint32_t a_id, const Pose& a_pose, Clock::time_point a_now)
		{
			auto it = seen.find(a_id);
			if (it == seen.end()) {
				if (seen.size() >= MAX_SEEN) {
					Prune(a_now);
					if (seen.size() >= MAX_SEEN) {
						return std::nullopt;
					}
				}
				seen.emplace(a_id, Entry{ a_pose, a_now });
				return std::nullopt;
			}
			it->second.lastSeen = a_now;
			if (!Moved(it->second.pose, a_pose)) {
				return std::nullopt;
			}
			const auto before = it->second.pose;
			it->second.pose = a_pose;
			return before;
		}
		// Its baseline is now a_pose (we put it there, or its move is over).
		void Set(std::uint32_t a_id, const Pose& a_pose, Clock::time_point a_now) { seen[a_id] = Entry{ a_pose, a_now }; }
		void Forget(std::uint32_t a_id) { seen.erase(a_id); }
		void Prune(Clock::time_point a_now)
		{
			std::erase_if(seen, [&](const auto& a_entry) { return a_now - a_entry.second.lastSeen > FORGET_AFTER; });
		}
		void                      Clear() { seen.clear(); }
		[[nodiscard]] std::size_t Size() const { return seen.size(); }

	private:
		struct Entry
		{
			Pose              pose;
			Clock::time_point lastSeen{};
		};
		std::map<std::uint32_t, Entry> seen;
	};

	// ---- what goes out ----

	inline constexpr std::size_t BURST = 8;
	inline constexpr double      PER_SECOND = 3.0;
	inline constexpr auto        WAIT_MAX = 24s;

	// Rests of ours, paced: BURST at once, then PER_SECOND; a rest of an object waiting already replaces it (the newest
	// place is the one that counts); one waiting past WAIT_MAX is dropped and counted.
	class Pacer
	{
	public:
		void Push(const Protocol::ObjectRest& a_rest, Clock::time_point a_now)
		{
			for (auto& waiting : queue) {
				if (waiting.rest.key == a_rest.key) {
					// A let-go is never replaced by its own rest: the flight goes first.
					if (waiting.rest.kind != Protocol::RestKind::kLetGo || a_rest.kind == Protocol::RestKind::kLetGo) {
						waiting.rest = a_rest;
						++replaced;
						return;
					}
				}
			}
			queue.push_back({ a_rest, a_now });
		}
		std::vector<Protocol::ObjectRest> Due(Clock::time_point a_now)
		{
			std::vector<Protocol::ObjectRest> out;
			while (!queue.empty() && a_now - queue.front().since > WAIT_MAX) {
				queue.pop_front();
				++dropped;
			}
			while (!queue.empty() && budget.Take(a_now)) {
				out.push_back(queue.front().rest);
				queue.pop_front();
			}
			return out;
		}
		void                      Clear() { queue.clear(); }
		[[nodiscard]] std::size_t Waiting() const { return queue.size(); }

		std::uint64_t dropped = 0, replaced = 0;

	private:
		struct Entry
		{
			Protocol::ObjectRest rest;
			Clock::time_point    since{};
		};
		std::deque<Entry> queue;
		TokenBucket       budget{ static_cast<double>(BURST), PER_SECOND };
	};

	// Holds of what our player holds: HOLD_RATE a second while it moves, STILL_RATE while it is still.
	inline constexpr auto HOLD_EVERY = std::chrono::milliseconds(1000 / 15);
	inline constexpr auto STILL_EVERY = 500ms;

	class HoldPace
	{
	public:
		// Whether a hold of a_pose goes out now.
		bool Due(const Pose& a_pose, Clock::time_point a_now)
		{
			if (last && a_now - *last < HOLD_EVERY) {
				return false;
			}
			const bool moving = !sent || Moved(*sent, a_pose);
			if (last && !moving && a_now - *last < STILL_EVERY) {
				return false;
			}
			last = a_now;
			sent = a_pose;
			return true;
		}
		void Reset()
		{
			last.reset();
			sent.reset();
		}

	private:
		std::optional<Clock::time_point> last;
		std::optional<Pose>              sent;
	};

	// A let-go's velocity: where it was as it left the hands and LET_GO_AFTER later.
	inline constexpr auto LET_GO_AFTER = 80ms;

	// The velocity of a move from a_a (at a_ta) to a_b (at a_tb), capped at MAX_OBJECT_SPEED; none when no time passed.
	[[nodiscard]] inline Vec Velocity(const Vec& a_a, Clock::time_point a_ta, const Vec& a_b, Clock::time_point a_tb)
	{
		const auto dt = std::chrono::duration<float>(a_tb - a_ta).count();
		if (!(dt > 0.0f)) {
			return {};
		}
		auto       v = (a_b - a_a) * (1.0f / dt);
		const auto speed = Length(v);
		if (!std::isfinite(speed)) {
			return {};
		}
		if (speed > Protocol::MAX_OBJECT_SPEED) {
			v = v * (Protocol::MAX_OBJECT_SPEED / speed);
		}
		return v;
	}

	// ---- what comes in ----

	inline constexpr auto  QUIET = 4s;            // after a friend's rest put it somewhere: our physics settling it is not ours
	inline constexpr auto  FRIEND_FLIGHT = 12s;   // after a friend's let-go: theirs until their rest
	inline constexpr float APPLY_RANGE = 8000.0f;  // a place is put only this near our player
	inline constexpr float WRITE_JUMP = 8000.0f;   // and never written this far from where the object lies here
	inline constexpr float PUT_EPSILON = 1.0f;    // a copy within this (and TURN_EPSILON) of a rest is left as it lies
	inline constexpr std::size_t ECHO_MAX = 3;
	inline constexpr auto        ECHO_WINDOW = 120s;

	// Whether a pose may be written to a body here: finite, in the world, a rotation, and within WRITE_JUMP of where it
	// lies (a report never pulls an object across the map).
	[[nodiscard]] inline bool Writable(const Pose& a_pose, const Vec& a_here)
	{
		return Protocol::InWorld(a_pose.at.x) && Protocol::InWorld(a_pose.at.y) && Protocol::InWorld(a_pose.at.z) && Protocol::QuatHolds(a_pose.rot) &&
		       Finite(a_here) && Distance(a_pose.at, a_here) <= WRITE_JUMP;
	}

	// Whether our copy, lying at a_here, is put at a friend's rest a_rest: unless it lies there already.
	[[nodiscard]] inline bool NeedsPut(const Pose& a_here, const Pose& a_rest)
	{
		return Distance(a_here.at, a_rest.at) > PUT_EPSILON || Angle(a_here.rot, a_rest.rot) > TURN_EPSILON;
	}

	// Objects a friend's move put somewhere: not ours to report until the time given.
	class Quiet
	{
	public:
		void Hush(std::uint32_t a_id, Clock::time_point a_until)
		{
			auto& until = hushed[a_id];
			until = (std::max)(until, a_until);
		}
		void End(std::uint32_t a_id) { hushed.erase(a_id); }
		[[nodiscard]] bool Hushed(std::uint32_t a_id, Clock::time_point a_now) const
		{
			const auto it = hushed.find(a_id);
			return it != hushed.end() && a_now < it->second;
		}
		void Prune(Clock::time_point a_now)
		{
			std::erase_if(hushed, [&](const auto& a_entry) { return a_entry.second <= a_now; });
		}
		void Clear() { hushed.clear(); }

	private:
		std::map<std::uint32_t, Clock::time_point> hushed;
	};

	// Our rests of objects a friend's rest put somewhere: at most ECHO_MAX per ECHO_WINDOW each, so two games whose
	// physics never agree on where it lies stop trading it.
	class Echo
	{
	public:
		// A friend's rest put a_id somewhere at a_now.
		void Theirs(std::uint32_t a_id, Clock::time_point a_now) { lastTheirs[a_id] = a_now; }
		// Whether our rest of a_id may go: always, unless a friend's rest of it came in the window and we sent ECHO_MAX
		// against it already.
		bool MaySay(std::uint32_t a_id, Clock::time_point a_now)
		{
			const auto theirs = lastTheirs.find(a_id);
			if (theirs == lastTheirs.end() || a_now - theirs->second > ECHO_WINDOW) {
				return true;
			}
			auto& times = ours[a_id];
			std::erase_if(times, [&](Clock::time_point a_t) { return a_now - a_t > ECHO_WINDOW; });
			if (times.size() >= ECHO_MAX) {
				++stopped;
				return false;
			}
			times.push_back(a_now);
			return true;
		}
		void Clear()
		{
			lastTheirs.clear();
			ours.clear();
		}
		std::uint64_t stopped = 0;

	private:
		std::map<std::uint32_t, Clock::time_point>              lastTheirs;
		std::map<std::uint32_t, std::vector<Clock::time_point>> ours;
	};

	// The session's places for objects not put yet (not loaded here, or too far): at most MAX_WAITING; past that the
	// newest is refused and counted (the catch-ups come first, a friend's live rest replaces its object's).
	class Waiting
	{
	public:
		static constexpr std::size_t MAX_WAITING = 4096;

		void Put(const Protocol::ObjectRest& a_rest)
		{
			if (a_rest.kind == Protocol::RestKind::kGone) {
				places.erase(a_rest.key);
				return;
			}
			if (places.size() >= MAX_WAITING && !places.contains(a_rest.key)) {
				++refused;
				return;
			}
			places[a_rest.key] = a_rest;
		}
		void Forget(const Protocol::ObjectKey& a_key) { places.erase(a_key); }
		void Clear() { places.clear(); }
		// Each place a_try put (true) is no longer waiting.
		std::size_t Try(const std::function<bool(const Protocol::ObjectRest&)>& a_try)
		{
			std::size_t put = 0;
			for (auto it = places.begin(); it != places.end();) {
				if (a_try(it->second)) {
					it = places.erase(it);
					++put;
				} else {
					++it;
				}
			}
			return put;
		}
		[[nodiscard]] std::size_t Size() const { return places.size(); }
		[[nodiscard]] bool        Has(const Protocol::ObjectKey& a_key) const { return places.contains(a_key); }

		std::uint64_t refused = 0;

	private:
		std::map<Protocol::ObjectKey, Protocol::ObjectRest> places;
	};

	// A rest of ours, built: our object at a_pose in a_cell or a_worldspace (a let-go with its velocity).
	[[nodiscard]] inline Protocol::ObjectRest RestOf(std::uint32_t a_id, Protocol::RestKind a_kind, std::uint32_t a_cell, std::uint32_t a_worldspace, const Pose& a_pose,
		const Vec& a_velocity = {})
	{
		Protocol::ObjectRest rest;
		rest.key = { Protocol::ObjectKind::kRef, a_id };
		rest.kind = a_kind;
		rest.cell = a_cell;
		rest.worldspace = a_worldspace;
		rest.x = a_pose.at.x;
		rest.y = a_pose.at.y;
		rest.z = a_pose.at.z;
		rest.rot = Normalized(a_pose.rot);
		if (a_kind == Protocol::RestKind::kLetGo) {
			rest.vx = a_velocity.x;
			rest.vy = a_velocity.y;
			rest.vz = a_velocity.z;
		}
		return rest;
	}
	[[nodiscard]] inline Pose PoseOf(const Protocol::ObjectRest& a_rest) { return { { a_rest.x, a_rest.y, a_rest.z }, a_rest.rot }; }
}
