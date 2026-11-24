#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

// Where a friend is shown beside their stand-in: on the compass, the world map and the local map, and the name over their
// head with their health when hurt (docs/DESIGN.md, "Friends on the compass and the maps"). The game's side is
// src/game/FriendMarkers.cpp and src/game/Party.cpp; this is the part with no game in it, unit-tested
// (tests/MarkerTests.cpp).
namespace MarkerRules
{
	// ---- how each friend looks --------------------------------------------------------------------------------------

	// Each friend has a shape and a colour of their own, by player id, the same on the compass and the maps, and the
	// shape tells them apart where colours don't (a player who sees no colour). The sister project's four.
	enum class Shape : std::uint8_t
	{
		kCircle,
		kSquare,
		kDiamond,
		kTriangle,
	};

	struct Look
	{
		Shape         shape = Shape::kCircle;
		std::uint32_t colour = 0;  // 0xRRGGBB
	};

	inline constexpr std::uint32_t COLOURS[] = { 0x3AA0FF, 0xFF9A1F, 0xFF5FC8, 0xA56CFF };  // blue, orange, pink, purple

	[[nodiscard]] inline Look LookOf(std::uint32_t a_playerId)
	{
		const auto slot = (a_playerId == 0 ? 0u : a_playerId - 1) % 4;
		return { static_cast<Shape>(slot), COLOURS[slot] };
	}

	struct Point
	{
		float x = 0, y = 0;
		bool operator==(const Point&) const = default;
	};

	// The outline of a shape a_size across, centred on 0,0, y down (Flash's): drawn with moveTo/lineTo, closed. A circle
	// is twelve sides (the drawing API's curves are not worth the calls at this size).
	[[nodiscard]] inline std::vector<Point> Outline(Shape a_shape, float a_size)
	{
		const float h = a_size / 2.0f;
		switch (a_shape) {
		case Shape::kSquare:
			return { { -h, -h }, { h, -h }, { h, h }, { -h, h } };
		case Shape::kDiamond:
			return { { 0, -h * 1.2f }, { h * 1.2f, 0 }, { 0, h * 1.2f }, { -h * 1.2f, 0 } };
		case Shape::kTriangle:
			return { { 0, -h * 1.2f }, { h * 1.1f, h }, { -h * 1.1f, h } };
		case Shape::kCircle:
		default:
			{
				std::vector<Point> out;
				for (int i = 0; i < 12; ++i) {
					const float a = static_cast<float>(i) * std::numbers::pi_v<float> / 6.0f;
					out.push_back({ h * std::sin(a), -h * std::cos(a) });
				}
				return out;
			}
		}
	}

	// ---- the compass --------------------------------------------------------------------------------------------------

	inline constexpr float PI = std::numbers::pi_v<float>;

	// A heading the game's way: 0 north (+y), clockwise, radians.
	[[nodiscard]] inline float Bearing(float a_dx, float a_dy)
	{
		return std::atan2(a_dx, a_dy);
	}

	// a_angle brought into (-pi, pi].
	[[nodiscard]] inline float Wrap(float a_angle)
	{
		if (!std::isfinite(a_angle)) {
			return 0.0f;
		}
		a_angle = std::fmod(a_angle, 2.0f * PI);
		if (a_angle <= -PI) {
			a_angle += 2.0f * PI;
		} else if (a_angle > PI) {
			a_angle -= 2.0f * PI;
		}
		return a_angle;
	}

	// How far round from where the camera looks (a_yaw, the game's heading) a friend at a_dx a_dy from our player is:
	// negative to the left. 0 when they stand on our spot.
	[[nodiscard]] inline float Relative(float a_dx, float a_dy, float a_yaw)
	{
		if (std::hypot(a_dx, a_dy) < 1.0f) {
			return 0.0f;
		}
		return Wrap(Bearing(a_dx, a_dy) - a_yaw);
	}

	// The compass shows half a turn: west at its left end and east at its right when looking north (COMPASS_HALF, the rig
	// measures it: docs/DESIGN.md). A friend past an end is pinned there, drawn fainter, so they are never lost.
	inline constexpr float COMPASS_HALF = PI / 2.0f;

	struct CompassSpot
	{
		float x = 0;          // from the compass's middle, in its own units
		bool  pinned = false;  // past an end: drawn at the end
	};

	// Where on a compass a_halfWidth either side of its middle a friend a_relative round from the camera is drawn.
	[[nodiscard]] inline CompassSpot CompassAt(float a_relative, float a_halfWidth, float a_half = COMPASS_HALF)
	{
		const float r = Wrap(a_relative);
		if (std::fabs(r) > a_half) {
			return { std::copysign(a_halfWidth, r), true };
		}
		return { r / a_half * a_halfWidth, false };
	}

	// How opaque (percent) a marker is: a friend in front of us full, one pinned to an end or shown at the way into where
	// they are (inside) fainter.
	[[nodiscard]] inline int Alpha(bool a_pinned, bool a_inside)
	{
		return a_pinned ? 45 : a_inside ? 65 : 100;
	}

	// ---- where each friend is drawn --------------------------------------------------------------------------------

	enum class View : std::uint8_t
	{
		kCompass,
		kLocalMap,
		kWorldMap,
	};

	// Where we are (or, for the world map, which map is open): an interior cell, or the worldspace whose map it is
	// (a walled city's own worldspace is its parent's map: Whiterun's is Tamriel's, the same coordinates).
	struct Here
	{
		std::uint32_t cell = 0;      // inside: the interior; 0 outside
		std::uint32_t mapWorld = 0;  // outside (or the world map): the map's worldspace; 0 inside
	};

	// A friend as this game knows them: their newest state (its worldspace already the map's) and status (its map world
	// already the map's).
	struct Friend
	{
		std::optional<Protocol::PlayerState>  state;
		std::uint32_t                         stateMapWorld = 0;  // the map the state's worldspace is on; 0 inside or none
		std::optional<Protocol::PlayerStatus> status;
		std::uint32_t                         statusMapWorld = 0;  // the map the status's way in is on; 0 none
	};

	struct Spot
	{
		float x = 0, y = 0, z = 0;
		bool  inside = false;  // the way into where they are, not where they stand
		bool operator==(const Spot&) const = default;
	};

	// Where a friend is drawn in a_view, or nowhere: travelling (a door or fast travel is taking them away; they are drawn
	// again where they arrive), nowhere this game has, or somewhere else. In the same interior as ours: where they stand
	// (the compass and the local map; the world map shows no interior). Outside on the same map: where they stand. Inside
	// somewhere while we are outside on that map (or look at it): the way in, fainter.
	[[nodiscard]] inline std::optional<Spot> Where(View a_view, const Here& a_here, const Friend& a_friend)
	{
		if (a_friend.status && (a_friend.status->flags & Protocol::kStatusTravelling) != 0) {
			return std::nullopt;
		}
		const auto& state = a_friend.state;
		const bool  insideHere = a_view != View::kWorldMap && a_here.cell != 0;
		if (insideHere) {
			if (state && state->cell == a_here.cell) {
				return Spot{ state->x, state->y, state->z, false };
			}
			return std::nullopt;  // inside, only who shares the interior (its own coordinates)
		}
		if (a_here.mapWorld == 0) {
			return std::nullopt;
		}
		if (state && state->cell == 0 && a_friend.stateMapWorld == a_here.mapWorld) {
			return Spot{ state->x, state->y, state->z, false };
		}
		const bool inside = state ? state->cell != 0 : (a_friend.status && (a_friend.status->flags & Protocol::kStatusInside) != 0);
		if (inside && a_friend.status && (a_friend.status->flags & Protocol::kStatusInside) != 0 && a_friend.statusMapWorld == a_here.mapWorld) {
			return Spot{ a_friend.status->mapX, a_friend.status->mapY, a_friend.status->mapZ, true };
		}
		return std::nullopt;
	}

	// ---- the name over their head ------------------------------------------------------------------------------------

	// The health shown beside a friend's name: none at full health, else to the nearest ten (a scratch doesn't change the
	// name), 10 to 90 while alive and hurt (never 0% alive, never 100% hurt), 0 only at 0.
	[[nodiscard]] inline std::optional<int> HealthShown(std::uint8_t a_health)
	{
		if (a_health >= 100) {
			return std::nullopt;
		}
		if (a_health == 0) {
			return 0;
		}
		return std::clamp((static_cast<int>(a_health) + 5) / 10 * 10, 10, 90);
	}

	// The name over a friend's head is shown when our crosshair is on their stand-in (as the game names what we look
	// at) and within LABEL_RANGE of the camera, or for LABEL_LINGER after it leaves them, so a glance is enough.
	inline constexpr float LABEL_RANGE = 3000.0f;
	inline constexpr auto  LABEL_LINGER = std::chrono::milliseconds{ 1500 };
	inline constexpr float LABEL_ABOVE = 25.0f;  // units over the stand-in's height

	// Whether the name is shown: a_distance from the camera within LABEL_RANGE, and looked at now or a_sinceLooked ago (none:
	// never) within LABEL_LINGER.
	[[nodiscard]] inline bool LabelShown(bool a_lookedAt, std::optional<std::chrono::steady_clock::duration> a_sinceLooked, float a_distance)
	{
		if (!(a_distance <= LABEL_RANGE)) {
			return false;
		}
		return a_lookedAt || (a_sinceLooked && *a_sinceLooked <= LABEL_LINGER);
	}

	// ---- drawing on a menu ------------------------------------------------------------------------------------------

	// A rectangle in a movie's own units (Flash's: y down).
	struct Rect
	{
		float left = 0, top = 0, right = 0, bottom = 0;
	};

	// A point a camera projects to a_x a_y (0 to 1 across its view, y up: NiCamera::WorldPtToScreenPt3), in a movie's
	// units on a_rect, the part of the movie the view fills.
	[[nodiscard]] inline Point ToMovie(float a_x, float a_y, const Rect& a_rect)
	{
		return { a_rect.left + a_x * (a_rect.right - a_rect.left), a_rect.top + (1.0f - a_y) * (a_rect.bottom - a_rect.top) };
	}

	// Whether a projected point is in the view (in front of the camera and inside its edges, a_margin of the view
	// outside them allowed so a marker at the edge is not cut off).
	[[nodiscard]] inline bool InView(float a_x, float a_y, float a_margin = 0.0f)
	{
		return std::isfinite(a_x) && std::isfinite(a_y) && a_x >= -a_margin && a_x <= 1.0f + a_margin && a_y >= -a_margin && a_y <= 1.0f + a_margin;
	}

	// The heading (Bearing's) a camera looks along, from its forward axis in the world (an NiCamera looks along its own
	// x): straight up or down has none, 0.
	[[nodiscard]] inline float Yaw(float a_forwardX, float a_forwardY)
	{
		return std::hypot(a_forwardX, a_forwardY) < 1e-4f ? 0.0f : Bearing(a_forwardX, a_forwardY);
	}

	// ---- our status, sent ----------------------------------------------------------------------------------------------

	inline constexpr auto  STATUS_GAP = std::chrono::milliseconds{ 250 };  // no two sends closer (a burst of hits)
	inline constexpr int   HEALTH_STEP = 5;                                // a change of at least this many points is said
	inline constexpr float SPOT_MOVE = 512.0f;                             // a way in this far from the last said is said

	// Whether a_now is worth sending after a_sent (none: nothing sent yet this session): a change of travel, of being
	// inside, of place or map, the way in moved, or health by HEALTH_STEP or onto 0 or 100 (the name's ends), and no
	// sooner than STATUS_GAP after the last; travel is said at once, gap or not (a stand-in leaves its door as it goes).
	[[nodiscard]] inline bool Worth(const std::optional<Protocol::PlayerStatus>& a_sent, const Protocol::PlayerStatus& a_now, std::chrono::steady_clock::duration a_sinceSent)
	{
		if (!a_sent) {
			return true;
		}
		const auto& s = *a_sent;
		if (((s.flags ^ a_now.flags) & Protocol::kStatusTravelling) != 0) {
			return true;
		}
		if (a_sinceSent < STATUS_GAP) {
			return false;
		}
		const bool health = std::abs(static_cast<int>(s.health) - static_cast<int>(a_now.health)) >= HEALTH_STEP ||
		                    (s.health != a_now.health && (a_now.health == 0 || a_now.health == 100));
		const bool moved = std::hypot(s.mapX - a_now.mapX, s.mapY - a_now.mapY, s.mapZ - a_now.mapZ) >= SPOT_MOVE;
		return health || s.flags != a_now.flags || s.location != a_now.location || s.mapWorld != a_now.mapWorld || moved;
	}

	// Health in percent from the actor's value and its whole (the permanent value and what is added to it for now): 0 to
	// 100, rounded down but never 0 while there is any left (0 is dead).
	[[nodiscard]] inline std::uint8_t HealthPercent(float a_current, float a_whole)
	{
		if (!std::isfinite(a_current) || !std::isfinite(a_whole) || a_whole <= 0.0f) {
			return 100;
		}
		if (a_current <= 0.0f) {
			return 0;
		}
		const auto pct = static_cast<int>(std::floor(a_current / a_whole * 100.0f));
		return static_cast<std::uint8_t>(std::clamp(pct, 1, 100));
	}

	// The way in on the map, as our game keeps it: the last place our player stood outside, kept while they are inside
	// (they walked in near there) and dropped when the game loads (a save may be anywhere).
	class WayIn
	{
	public:
		void Outside(std::uint32_t a_world, float a_x, float a_y, float a_z)
		{
			if (a_world != 0 && Protocol::InWorld(a_x) && Protocol::InWorld(a_y) && Protocol::InWorld(a_z)) {
				last = Kept{ a_world, a_x, a_y, a_z };
			}
		}
		void Forget() { last.reset(); }

		// Inside: the status's map spot (none when nothing is kept; the game looks for a door out then, a_door).
		void Fill(Protocol::PlayerStatus& a_status, const std::optional<std::pair<std::uint32_t, Point>>& a_door = std::nullopt, float a_doorZ = 0.0f) const
		{
			a_status.flags |= Protocol::kStatusInside;
			if (last) {
				a_status.mapWorld = last->world;
				a_status.mapX = last->x;
				a_status.mapY = last->y;
				a_status.mapZ = last->z;
			} else if (a_door && a_door->first != 0) {
				a_status.mapWorld = a_door->first;
				a_status.mapX = a_door->second.x;
				a_status.mapY = a_door->second.y;
				a_status.mapZ = a_doorZ;
			}
		}
		[[nodiscard]] bool Known() const { return last.has_value(); }

	private:
		struct Kept
		{
			std::uint32_t world;
			float         x, y, z;
		};
		std::optional<Kept> last;
	};
}
