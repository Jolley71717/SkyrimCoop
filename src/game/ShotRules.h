#pragma once

#include "Protocol.h"
#include "TokenBucket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Projectiles, the rules (docs/DESIGN.md, "Projectiles"; the game's side is src/game/Shots.cpp). Every projectile an
// actor launches in its own game (an arrow, a bolt, a spell's of any type, a shout's, a staff's) is said once as it
// leaves, and every other game launches the same from the actor that shows the shooter there:
// - Seen: a projectile is said at its first update (its first sight: never seen before, and younger than YOUNG), when
//   its shooter is our player or an NPC this game runs. A concentration spell's stream is said at most once per
//   HELD_GAP for each shooter and hand (Outbox), so a held Flames is a stream, not a flood.
// - Due: a friend's shot is launched when the actor that shows its shooter is drawn at the moment it left (the stand-in
//   is drawn a delay behind its states), and one that waited longer than STALE is dropped: its flight is over (Inbox).
// - Launched: from that actor only when it is near where the shot left (Launchable), at the shooter's own origin and
//   aim. The engine may aim a launch at whoever the actor looks at (the sister project saw a stand-in's missile go
//   straight at the local player): a launch flying more than AIM_FIX off the shooter's aim is turned back to it (AimError).
// - Natives: a projectile a stand-in's or a moved copy's own graph launches (its replayed release or cast) is ended
//   before it moves, so nothing flies twice; one this game launched for the wire is told apart even when its first
//   update runs before the launch returns (Pending).
// - Ended: the ones this game launched are ended once their flight is over (Live): an arrow DWELL after it stopped (an
//   arrow that sticks is real ammunition a player could pick up), a rune or other lobbed one after its record's life
//   (at most LOBBER_LIFE), any other after MAX_LIFE, and all before a save. At most MAX_LIVE fly at once.
// No game here: tests/ShotTests.cpp.
namespace ShotRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;
	using Kind = Protocol::ShotKind;

	inline constexpr float YOUNG = 0.1f;          // s: a projectile's living time at its first update
	inline constexpr auto  HELD_GAP = 80ms;       // a concentration stream's shots for one shooter and hand, at most one per
	inline constexpr auto  STALE = 1s;            // a friend's shot that waited longer is dropped
	inline constexpr float REACH = 300.0f;        // u: how near where a shot left the actor that shows its shooter must be
	inline constexpr float AIM_FIX = 1.0f;        // degrees: a launch further off the shooter's aim is turned back to it
	inline constexpr auto  DWELL = 2s;            // an arrow of ours that stopped is ended this long after
	inline constexpr auto  MAX_LIFE = 12s;        // any launch of ours is ended by then (a lobbed one: LOBBER_LIFE)
	inline constexpr auto  LOBBER_LIFE = 30s;     // a rune of ours lies at most this long (its record's life if shorter)
	inline constexpr auto  PENDING_FOR = 500ms;   // a launch of ours is told apart from a native this long
	inline constexpr float PENDING_REACH = 4.0f;  // u: its first update is this near the origin it was launched at
	inline constexpr std::size_t MAX_LIVE = 64;   // launches of ours flying at once; a 65th is not launched

	inline constexpr std::array<std::string_view, Protocol::SHOT_KIND_COUNT> KIND_NAMES{ "other", "arrow", "missile", "lobber", "beam", "flame", "cone", "barrier" };

	[[nodiscard]] constexpr std::string_view NameOf(Kind a_kind)
	{
		const auto i = static_cast<std::size_t>(a_kind);
		return i < KIND_NAMES.size() ? KIND_NAMES[i] : std::string_view{ "other" };
	}

	// The kind a name says ("arrow"), none for a name that is no kind.
	[[nodiscard]] constexpr std::optional<Kind> KindNamed(std::string_view a_name)
	{
		for (std::size_t i = 0; i < KIND_NAMES.size(); ++i) {
			if (KIND_NAMES[i] == a_name) {
				return static_cast<Kind>(i);
			}
		}
		return std::nullopt;
	}

	// The game's projectile types (BGSProjectileData::Type), as bits.
	inline constexpr std::uint16_t TYPE_MISSILE = 1 << 0;
	inline constexpr std::uint16_t TYPE_LOBBER = 1 << 1;  // the game's "grenade"
	inline constexpr std::uint16_t TYPE_BEAM = 1 << 2;
	inline constexpr std::uint16_t TYPE_FLAME = 1 << 3;
	inline constexpr std::uint16_t TYPE_CONE = 1 << 4;
	inline constexpr std::uint16_t TYPE_BARRIER = 1 << 5;
	inline constexpr std::uint16_t TYPE_ARROW = 1 << 6;

	// A record's kind from its type bits: one bit is set in every record the game has; of several, the first of arrow,
	// flame, beam, cone, barrier, lobber, missile.
	[[nodiscard]] constexpr Kind KindOf(std::uint16_t a_types)
	{
		constexpr std::array<std::pair<std::uint16_t, Kind>, 7> ORDER{ { { TYPE_ARROW, Kind::kArrow }, { TYPE_FLAME, Kind::kFlame }, { TYPE_BEAM, Kind::kBeam },
			{ TYPE_CONE, Kind::kCone }, { TYPE_BARRIER, Kind::kBarrier }, { TYPE_LOBBER, Kind::kLobber }, { TYPE_MISSILE, Kind::kMissile } } };
		for (const auto& [bit, kind] : ORDER) {
			if ((a_types & bit) != 0) {
				return kind;
			}
		}
		return Kind::kOther;
	}

	// Counts by kind, for the developer channel ("arrow:3,missile:1", or "none").
	struct KindCounts
	{
		std::array<std::uint64_t, Protocol::SHOT_KIND_COUNT> n{};

		void Add(Kind a_kind)
		{
			const auto i = static_cast<std::size_t>(a_kind);
			++n[i < n.size() ? i : 0];
		}
		[[nodiscard]] std::uint64_t Of(Kind a_kind) const
		{
			const auto i = static_cast<std::size_t>(a_kind);
			return i < n.size() ? n[i] : 0;
		}
		[[nodiscard]] std::uint64_t Total() const
		{
			std::uint64_t total = 0;
			for (const auto v : n) {
				total += v;
			}
			return total;
		}
		[[nodiscard]] std::string Line() const
		{
			std::string out;
			for (std::size_t i = 0; i < n.size(); ++i) {
				if (n[i] != 0) {
					out += (out.empty() ? "" : ",") + std::string{ KIND_NAMES[i] } + ":" + std::to_string(n[i]);
				}
			}
			return out.empty() ? "none" : out;
		}
	};

	// ---- aim ----

	// The way a projectile leaves at a_pitch and a_heading (the game's angle x and z, radians): heading 0 is +y and turns
	// clockwise to +x; a pitch above 0 looks down.
	[[nodiscard]] inline std::array<float, 3> Direction(float a_pitch, float a_heading)
	{
		const float level = std::cos(a_pitch);
		return { std::sin(a_heading) * level, std::cos(a_heading) * level, -std::sin(a_pitch) };
	}

	// The angles of a way (the inverse of Direction); none for no way at all.
	[[nodiscard]] inline std::optional<std::pair<float, float>> AnglesOf(float a_x, float a_y, float a_z)
	{
		const float length = std::sqrt(a_x * a_x + a_y * a_y + a_z * a_z);
		if (!std::isfinite(length) || length < 1e-3f) {
			return std::nullopt;
		}
		const float pitch = -std::asin(std::clamp(a_z / length, -1.0f, 1.0f));
		float       heading = std::atan2(a_x, a_y);
		if (heading < 0.0f) {
			heading += 2.0f * std::numbers::pi_v<float>;
		}
		return std::pair{ pitch, heading };
	}

	// Degrees between a projectile's velocity and the way the shooter aimed it; none while it has no velocity (a beam or a
	// cone may have none to read).
	[[nodiscard]] inline std::optional<float> AimError(float a_vx, float a_vy, float a_vz, float a_pitch, float a_heading)
	{
		const float length = std::sqrt(a_vx * a_vx + a_vy * a_vy + a_vz * a_vz);
		if (!std::isfinite(length) || length < 1e-3f) {
			return std::nullopt;
		}
		const auto  want = Direction(a_pitch, a_heading);
		const float dot = std::clamp((a_vx * want[0] + a_vy * want[1] + a_vz * want[2]) / length, -1.0f, 1.0f);
		return std::acos(dot) * 180.0f / std::numbers::pi_v<float>;
	}

	// The velocity a launch flying a_error degrees off is turned to: the same speed, the shooter's way. None when it
	// flies true enough (within AIM_FIX) or has no velocity.
	[[nodiscard]] inline std::optional<std::array<float, 3>> Reaimed(float a_vx, float a_vy, float a_vz, float a_pitch, float a_heading)
	{
		const auto error = AimError(a_vx, a_vy, a_vz, a_pitch, a_heading);
		if (!error || *error <= AIM_FIX) {
			return std::nullopt;
		}
		const float speed = std::sqrt(a_vx * a_vx + a_vy * a_vy + a_vz * a_vz);
		const auto  way = Direction(a_pitch, a_heading);
		return std::array<float, 3>{ way[0] * speed, way[1] * speed, way[2] * speed };
	}

	// Whether a friend's shot is launched from the actor that shows its shooter here: that actor is near where it left
	// (a dragon's breath leaves its mouth, far from its feet: a_size, the actor's height, widens the reach).
	[[nodiscard]] inline bool Launchable(float a_distance, float a_size)
	{
		return std::isfinite(a_distance) && a_distance <= REACH + 2.0f * (std::max)(0.0f, a_size);
	}

	// ---- ours, going out ----

	// Who launched a shot, and with which hand: a concentration stream is one per shooter and hand.
	struct Stream
	{
		std::uint32_t    player = 0;
		Protocol::NpcKey npc;
		std::uint8_t     source = 0;
		auto operator<=>(const Stream&) const = default;
	};

	[[nodiscard]] inline Stream StreamOf(const Protocol::Shot& a_shot)
	{
		return Stream{ a_shot.player, a_shot.player != 0 ? Protocol::NpcKey{} : a_shot.npc, a_shot.source };
	}

	// What this game says of its own shooters' launches: each fire-and-forget shot as it is seen, a concentration
	// stream's at most one per HELD_GAP for each shooter and hand (the rest coalesced), within a budget below the server's
	// (the rest dropped), at most Protocol::MAX_SHOTS in a message.
	class Outbox
	{
	public:
		static constexpr std::size_t MAX_WAITING = 128;
		static constexpr double      BURST = 64.0;
		static constexpr double      PER_SECOND = 40.0;

		// False when it was coalesced or dropped.
		bool Seen(const Protocol::Shot& a_shot, Clock::time_point a_now)
		{
			++seen;
			if ((a_shot.flags & Protocol::kShotHeld) != 0) {
				std::erase_if(streams, [&](const auto& a_entry) { return a_now - a_entry.second > 2s; });
				const auto stream = StreamOf(a_shot);
				if (const auto it = streams.find(stream); it != streams.end() && a_now - it->second < HELD_GAP) {
					++coalesced;
					return false;
				}
				streams[stream] = a_now;
			}
			if (queue.size() >= MAX_WAITING || !budget.Take(a_now)) {
				++dropped;
				return false;
			}
			byKind.Add(a_shot.kind);
			queue.push_back(a_shot);
			return true;
		}

		// The shots to send now, at most Protocol::MAX_SHOTS (the rest wait for the next).
		[[nodiscard]] std::vector<Protocol::Shot> Take()
		{
			const auto count = (std::min)(queue.size(), Protocol::MAX_SHOTS);
			std::vector<Protocol::Shot> out(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(count));
			queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(count));
			sent += out.size();
			return out;
		}

		void Clear()
		{
			queue.clear();
			streams.clear();
		}
		[[nodiscard]] std::size_t Waiting() const { return queue.size(); }

		std::uint64_t seen = 0;
		std::uint64_t sent = 0;
		std::uint64_t coalesced = 0;
		std::uint64_t dropped = 0;
		KindCounts    byKind;  // of those kept to send

	private:
		std::deque<Protocol::Shot>        queue;
		std::map<Stream, Clock::time_point> streams;
		TokenBucket                       budget{ BURST, PER_SECOND };
	};

	// Which projectiles the update hook has seen: a projectile is new at its first update only (never seen, and younger
	// than YOUNG: an arrow lying in a wall is not new again once it is forgotten). Keyed by the object and its form id, so
	// a new projectile in a freed one's memory is new.
	class FirstSight
	{
	public:
		static constexpr std::size_t MAX = 1024;
		static constexpr auto        FORGET_AFTER = 60s;

		bool New(std::uintptr_t a_object, std::uint32_t a_formId, float a_livingTime, Clock::time_point a_now)
		{
			const auto key = std::pair{ a_object, a_formId };
			if (seen.contains(key)) {
				return false;
			}
			if (seen.size() >= MAX) {
				std::erase_if(seen, [&](const auto& a_entry) { return a_now - a_entry.second > FORGET_AFTER; });
				while (seen.size() >= MAX) {
					seen.erase(std::ranges::min_element(seen, {}, [](const auto& a_entry) { return a_entry.second; }));
				}
			}
			seen.emplace(key, a_now);
			return std::isfinite(a_livingTime) && a_livingTime < YOUNG;
		}

		void Clear() { seen.clear(); }
		[[nodiscard]] std::size_t Size() const { return seen.size(); }

	private:
		std::map<std::pair<std::uintptr_t, std::uint32_t>, Clock::time_point> seen;
	};

	// What becomes of a projectile at its first update.
	enum class Fate : std::uint8_t
	{
		kLeave,   // not ours to say, nor to end: any other actor's, or one the world launched
		kSay,     // an actor's: said to the others when it is our player's or an NPC's this game runs (the main thread checks)
		kOurs,    // one this game launched for a friend's shot: left to fly
		kEnd,     // a stand-in's or a moved copy's own: ended before it moves (the wire's copy flies instead)
	};

	struct Shooter
	{
		bool actor = false;         // launched by an actor (not a trap, a script's, an explosion's)
		bool standIn = false;       // a friend's stand-in here
		bool movedCopy = false;     // a copy of an NPC another game runs, moved by its runner's states now
		bool launchedByUs = false;  // a launch of ours for a friend's shot (Pending, or its handle)
	};

	[[nodiscard]] constexpr Fate Judge(const Shooter& a_shooter)
	{
		if (a_shooter.launchedByUs) {
			return Fate::kOurs;
		}
		if (!a_shooter.actor) {
			return Fate::kLeave;  // each game's world launches its own (a trap, a script): not said
		}
		if (a_shooter.standIn || a_shooter.movedCopy) {
			return Fate::kEnd;
		}
		return Fate::kSay;  // our player, or another actor: the main thread says it only for our player and the NPCs we run
	}

	// ---- theirs, coming in ----

	// Launches of ours under way: the projectile's first update may run (on another thread) before the launch returns,
	// so it is told apart by its shooter, its record and where it is (still at the origin it was launched at). Once the
	// launch returns, its entry is taken back (Cancel): from then on the projectile's id tells it, and the entry must not
	// match a native of the same shooter.
	class Pending
	{
	public:
		// A token for Cancel.
		std::uint64_t Add(std::uintptr_t a_shooter, std::uint32_t a_base, float a_x, float a_y, float a_z, Clock::time_point a_now)
		{
			std::erase_if(entries, [&](const Entry& a_e) { return a_now - a_e.at > PENDING_FOR; });
			entries.push_back({ a_shooter, a_base, a_x, a_y, a_z, a_now, ++last });
			return last;
		}

		// The launch returned: its entry goes. True when it was still there (no first update matched it yet).
		bool Cancel(std::uint64_t a_token)
		{
			return std::erase_if(entries, [&](const Entry& a_e) { return a_e.token == a_token; }) > 0;
		}

		// Whether a projectile of a_shooter's of record a_base at x y z is a launch of ours (each launch matches once).
		bool Match(std::uintptr_t a_shooter, std::uint32_t a_base, float a_x, float a_y, float a_z, Clock::time_point a_now)
		{
			for (auto it = entries.begin(); it != entries.end(); ++it) {
				if (it->shooter != a_shooter || it->base != a_base || a_now - it->at > PENDING_FOR) {
					continue;
				}
				const float dx = it->x - a_x, dy = it->y - a_y, dz = it->z - a_z;
				if (dx * dx + dy * dy + dz * dz <= PENDING_REACH * PENDING_REACH) {
					entries.erase(it);
					return true;
				}
			}
			return false;
		}

		void Clear() { entries.clear(); }
		[[nodiscard]] std::size_t Size() const { return entries.size(); }

	private:
		struct Entry
		{
			std::uintptr_t    shooter;
			std::uint32_t     base;
			float             x, y, z;
			Clock::time_point at;
			std::uint64_t     token;
		};
		std::vector<Entry> entries;
		std::uint64_t      last = 0;
	};

	// A friend's shots, each launched when the actor that shows its shooter is drawn at the moment it left. One that says
	// no time, or comes after that point, goes at once; one that waited longer than STALE is dropped (late).
	class Inbox
	{
	public:
		static constexpr std::size_t MAX = 128;

		void Push(const Protocol::Shot& a_shot, std::optional<Clock::time_point> a_at, Clock::time_point a_now)
		{
			const auto at = (std::min)(a_at.value_or(a_now), a_now + STALE);  // a clock read wrong never holds the queue up
			const auto pos = std::upper_bound(queue.begin(), queue.end(), at, [](Clock::time_point a_t, const Entry& a_e) { return a_t < a_e.at; });
			queue.insert(pos, Entry{ at, a_shot });
			while (queue.size() > MAX) {
				queue.pop_front();
				++dropped;
			}
		}

		// The shots due when their shooters are drawn at a_drawn (now less the delay), in their order.
		[[nodiscard]] std::vector<Protocol::Shot> Due(Clock::time_point a_drawn)
		{
			std::vector<Protocol::Shot> out;
			while (!queue.empty() && queue.front().at <= a_drawn) {
				if (a_drawn - queue.front().at > STALE) {
					++late;
				} else {
					out.push_back(queue.front().shot);
				}
				queue.pop_front();
			}
			return out;
		}

		void Clear() { queue.clear(); }
		[[nodiscard]] std::size_t Size() const { return queue.size(); }

		std::uint64_t late = 0;
		std::uint64_t dropped = 0;

	private:
		struct Entry
		{
			Clock::time_point at;
			Protocol::Shot    shot;
		};
		std::deque<Entry> queue;
	};

	// The projectiles this game launched for friends' shots, and when each is ended.
	class Live
	{
	public:
		// Room for one more (MAX_LIVE).
		[[nodiscard]] bool Room() const { return entries.size() < MAX_LIVE; }

		// a_life: its record's own life in seconds (0: none said).
		void Add(std::uint32_t a_id, Kind a_kind, float a_life, Clock::time_point a_now)
		{
			auto limit = std::chrono::duration_cast<Clock::duration>(MAX_LIFE);
			if (a_kind == Kind::kLobber) {
				limit = std::chrono::duration_cast<Clock::duration>(LOBBER_LIFE);
				if (std::isfinite(a_life) && a_life > 0.0f && a_life < 30.0f) {
					limit = (std::max)(std::chrono::duration_cast<Clock::duration>(MAX_LIFE),
						std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(a_life)));
				}
			}
			entries[a_id] = Entry{ a_kind, a_now, a_now + limit, std::nullopt };
		}

		// What the game says of one now: gone (ended by the game itself: it hit, its life ran out), or stopped (it moves no
		// more: an arrow in a wall). True when it is to be ended now; a gone one is forgotten either way.
		bool Step(std::uint32_t a_id, bool a_gone, bool a_stopped, Clock::time_point a_now)
		{
			const auto it = entries.find(a_id);
			if (it == entries.end()) {
				return false;
			}
			if (a_gone) {
				entries.erase(it);
				++gone;
				return false;
			}
			auto& e = it->second;
			if (a_stopped && !e.stoppedAt) {
				e.stoppedAt = a_now;
			}
			const bool dwelt = e.kind == Kind::kArrow && e.stoppedAt && a_now - *e.stoppedAt >= DWELL;
			if (dwelt || a_now >= e.endAt) {
				++(dwelt ? endedStuck : endedOld);
				entries.erase(it);
				return true;
			}
			return false;
		}

		// Every one, to be ended now (a save, the session's end, a load): forgotten.
		[[nodiscard]] std::vector<std::uint32_t> TakeAll()
		{
			std::vector<std::uint32_t> out;
			out.reserve(entries.size());
			for (const auto& [id, entry] : entries) {
				out.push_back(id);
			}
			entries.clear();
			return out;
		}

		[[nodiscard]] std::vector<std::uint32_t> Ids() const
		{
			std::vector<std::uint32_t> out;
			out.reserve(entries.size());
			for (const auto& [id, entry] : entries) {
				out.push_back(id);
			}
			return out;
		}

		[[nodiscard]] std::size_t Count() const { return entries.size(); }
		[[nodiscard]] std::size_t Count(Kind a_kind) const
		{
			return static_cast<std::size_t>(std::ranges::count_if(entries, [&](const auto& a_entry) { return a_entry.second.kind == a_kind; }));
		}

		std::uint64_t gone = 0;
		std::uint64_t endedStuck = 0;
		std::uint64_t endedOld = 0;

	private:
		struct Entry
		{
			Kind                             kind;
			Clock::time_point                at;
			Clock::time_point                endAt;
			std::optional<Clock::time_point> stoppedAt;
		};
		std::map<std::uint32_t, Entry> entries;
	};
}
