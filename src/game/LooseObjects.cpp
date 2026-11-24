#include "game/LooseObjects.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/NpcSync.h"
#include "game/ObjectRules.h"
#include "game/RemotePlayers.h"
#include "game/ShotRules.h"
#include "game/Shots.h"
#include "game/VtableHook.h"
#include "net/FormsCommand.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xmmintrin.h>

namespace LooseObjects
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		using ObjectRules::Pose;
		using ObjectRules::Vec;
		using Key = Protocol::ObjectKey;

		// Four looks a second at what lies within SCAN_RANGE of our player; at most MAX_LOOKED objects a look (the nearest).
		constexpr auto        SCAN_EVERY = 250ms;
		constexpr float       SCAN_RANGE = 1500.0f;
		constexpr std::size_t MAX_LOOKED = 256;
		// A move of ours is followed wherever it flies, past SCAN_RANGE (at most MAX_WATCHED at once); one whose object is
		// not found for LOST_AFTER (unloaded, picked up) is given up.
		constexpr std::size_t MAX_WATCHED = 64;
		constexpr auto        LOST_AFTER = 3s;
		// A rest of ours that moves on within FOLLOW_ON (a ball still rolling) is still ours, at most FOLLOW_ON_MAX times.
		constexpr auto          FOLLOW_ON = 10s;
		constexpr std::uint32_t FOLLOW_ON_MAX = 8;
		// A friend's hold not heard for this long is over: our copy falls where it is.
		constexpr auto HOLD_STALE = 2s;
		// A held copy's body is written every frame; its reference (its place for the save and a cell's reload, and its 3D
		// moved with it) this often: 20 times a second keeps the mesh with the body if the engine does not draw a keyframed
		// body's moves itself, at a third of the cost. Whether it does is for the rig (`objects`: refWrites, bodyOnly).
		constexpr auto HOLD_REF_EVERY = 50ms;
		// The session's places waiting here are tried this often.
		constexpr auto WAITING_EVERY = 1s;
		// A held copy whose stand-in is in another cell (carried over a cell border, or through a door with the friend) is
		// moved into the stand-in's cell at most this often.
		constexpr auto CARRY_EVERY = 1s;
		// Explosion::Update, the explosion's own update (SE and AE; this plugin is not built for VR).
		constexpr std::size_t EXPLOSION_UPDATE_SLOT = 0xA3;
		constexpr std::size_t MAX_RECORDED = 64;
		// Projectiles followed at once (each update of each, from the projectiles' own update), and stretches of their paths
		// waiting for the main thread.
		constexpr std::size_t MAX_FLIGHTS = 256;
		constexpr std::size_t MAX_STRETCHES = 512;

		std::atomic<bool> active{ false };
		std::uint32_t     ourId = 0;

		// ---- recorded off the main thread (the explosion hook, the event sinks) ----

		struct Blast
		{
			Vec               at;
			float             radius = 0.0f;
			RE::ActorHandle   owner;
			Clock::time_point when{};
		};
		struct Hit
		{
			RE::FormID          target = 0;
			RE::ObjectRefHandle cause;
			Clock::time_point   when{};
		};
		std::mutex                     recordLock;
		std::vector<Blast>             blasts;
		std::vector<Hit>               hits;
		std::vector<RE::FormID>        pickups;
		std::atomic<std::uint64_t>     blastsHeard{ 0 }, hitsHeard{ 0 }, pickupsHeard{ 0 };

		// A projectile's flight, from its updates (Flew), and the stretches of the paths of all of them.
		struct FlightOf
		{
			ObjectRules::Flight flight;
			RE::ObjectRefHandle shooter;
			float               reach = 0.0f;
		};
		struct ShotPath
		{
			ObjectRules::Stretch stretch;
			float                reach = 0.0f;
			RE::ObjectRefHandle  shooter;
			Clock::time_point    when{};
		};
		std::map<std::pair<std::uintptr_t, std::uint32_t>, FlightOf> flights;  // by the object and its form id
		std::vector<ShotPath>                                         shotPaths;
		std::atomic<std::uint64_t> flightsHeard{ 0 }, flightsDropped{ 0 }, stretchesHeard{ 0 }, stretchesDropped{ 0 }, conesHeard{ 0 };

		// Under recordLock.
		void PushPath(const ObjectRules::Stretch& a_stretch, float a_reach, RE::ObjectRefHandle a_shooter, Clock::time_point a_now)
		{
			if (shotPaths.size() >= MAX_STRETCHES) {
				stretchesDropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			shotPaths.push_back({ a_stretch, a_reach, a_shooter, a_now });
			stretchesHeard.fetch_add(1, std::memory_order_relaxed);
		}

		// After each update of a projectile that flies (Shots::WatchFlights), on the thread that updates it: its path, a
		// stretch each FLIGHT_EVERY and the last as it ends, so a fast arrow or a cone that launches and lands between two
		// looks is a cause of what it knocks over. A cone (Unrelenting Force) is its whole line at once, as far as its
		// record's range. Whose it is, is the main thread's (its shooter's).
		void Flew(RE::Projectile* a_projectile)
		{
			if (!a_projectile || !active.load(std::memory_order_relaxed)) {
				return;
			}
			const auto& rd = a_projectile->GetProjectileRuntimeData();
			const auto  p = a_projectile->GetPosition();
			const Vec   at{ p.x, p.y, p.z };
			if (!ObjectRules::Finite(at)) {
				return;
			}
			const auto key = std::pair{ reinterpret_cast<std::uintptr_t>(a_projectile), a_projectile->GetFormID() };
			const bool cone = a_projectile->GetFormType() == RE::FormType::ProjectileCone;
			const auto reach = cone ? ObjectRules::CONE_REACH : ObjectRules::SHOT_REACH;
			const auto now = Clock::now();
			std::scoped_lock l{ recordLock };
			auto it = flights.find(key);
			if (it != flights.end() && it->second.flight.Another(at, now)) {
				if (const auto rest = it->second.flight.Rest()) {
					PushPath(*rest, it->second.reach, it->second.shooter, now);
				}
				flights.erase(it);
				it = flights.end();
			}
			if (it == flights.end()) {
				if (flights.size() >= MAX_FLIGHTS) {
					flightsDropped.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				flights.emplace(key, FlightOf{ ObjectRules::Flight{ at, now }, rd.shooter, reach });
				flightsHeard.fetch_add(1, std::memory_order_relaxed);
				if (cone) {
					const auto base = a_projectile->GetProjectileBase();
					const auto way = ShotRules::Direction(a_projectile->data.angle.x, a_projectile->data.angle.z);
					if (const auto line = base ? ObjectRules::ConeLine(at, { way[0], way[1], way[2] }, base->data.range) : std::nullopt) {
						PushPath(*line, reach, rd.shooter, now);
						conesHeard.fetch_add(1, std::memory_order_relaxed);
					}
				}
				return;
			}
			if (const auto stretch = it->second.flight.Update(at, now)) {
				PushPath(*stretch, it->second.reach, it->second.shooter, now);
			}
		}

		using ExplosionUpdateFn = void (*)(RE::Explosion*, float);
		// The explosion classes whose update is hooked (a chain explosion has its own vtable), each with its original.
		ExplosionUpdateFn explosionUpdates[2] = {};
		std::size_t       explosionsHooked = 0;
		bool              explosionHooked = false;

		void Blasted(RE::Explosion* a_this, bool a_before);

		template <std::size_t I>
		void OnExplosionUpdate(RE::Explosion* a_this, float a_delta)
		{
			const bool before = a_this && a_this->GetExplosionRuntimeData().flags.any(RE::Explosion::Flags::kForcesApplied);
			explosionUpdates[I](a_this, a_delta);
			Blasted(a_this, before);
		}

		void Blasted(RE::Explosion* a_this, bool a_before)
		{
			const bool before = a_before;
			if (!a_this || before || !active.load(std::memory_order_relaxed)) {
				return;
			}
			auto& data = a_this->GetExplosionRuntimeData();
			if (!data.flags.any(RE::Explosion::Flags::kForcesApplied)) {
				return;
			}
			// The moment it pushed what it reached (the update whose start found its forces not applied yet, so once an
			// explosion: no address is kept, a later explosion often reuses a freed one's): where, how far, who set it off.
			const auto at = a_this->GetPosition();
			std::scoped_lock l{ recordLock };
			if (blasts.size() < MAX_RECORDED) {
				blasts.push_back({ { at.x, at.y, at.z }, data.radius, data.actorOwner, Clock::now() });
				blastsHeard.fetch_add(1, std::memory_order_relaxed);
			}
		}

		struct HitSink final : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !a_event->target || !a_event->cause || !active.load(std::memory_order_relaxed)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->target->Is(RE::FormType::ActorCharacter)) {
					return RE::BSEventNotifyControl::kContinue;  // an actor: the NPCs' business
				}
				std::scoped_lock l{ recordLock };
				if (hits.size() < MAX_RECORDED) {
					hits.push_back({ a_event->target->GetFormID(), a_event->cause->GetHandle(), Clock::now() });
					hitsHeard.fetch_add(1, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		HitSink hitSink;

		// Our player picked something up out of the world: the session keeps no place for it any more.
		struct ContainerSink final : RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!a_event || a_event->oldContainer != 0 || a_event->newContainer != 0x14 || !active.load(std::memory_order_relaxed)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto ref = a_event->reference.get();
				if (!ref || FormIds::IsRuntime(ref->GetFormID())) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::scoped_lock l{ recordLock };
				if (pickups.size() < MAX_RECORDED) {
					pickups.push_back(ref->GetFormID());
					pickupsHeard.fetch_add(1, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		ContainerSink containerSink;

		// ---- the main thread's ----

		// A move of ours, watched until it rests.
		struct Watch
		{
			ObjectRules::Settle                     settle;
			Clock::time_point                       lastFound{};
			bool                                    thrown = false;  // our player's let-go
			std::optional<ObjectRules::PartsSettle> parts;           // an object of several bodies': every loose one at rest
		};
		// A friend's hold of an object, drawn in front of their stand-in.
		struct FriendHold
		{
			std::uint32_t        holder = 0;
			Protocol::ObjectHold hold;
			Clock::time_point    heard{};
			std::optional<Pose>  drawn;
			bool                 keyframed = false;  // our copy is keyframed and out of collisions now
			std::optional<Clock::time_point> refWritten;  // its reference last written
			// What our copy was before the hold made it keyframed and took it out of collisions, read once and put back
			// when it falls (a 3D rebuilt by a move into another cell is set again, never read again).
			std::optional<RE::COL_LAYER>       layer;
			std::optional<ObjectRules::Motion> motion;
			std::optional<Clock::time_point>   carried;  // last moved into the stand-in's cell
			bool                               reapply = false;  // moved: keyframed and out of collisions again (its 3D may be new)
			bool                               parted = false;  // its stand-in is not here: it fell where it was
			// For the rig: how far the drawn copy trailed where the hold puts it, the longest wait between two holds.
			float                    trailMax = 0.0f;
			Clock::duration          gapMax{};
			std::uint64_t            heardCount = 0;
		};
		// Our player's let-go, said LET_GO_AFTER after it.
		struct LetGo
		{
			RE::FormID        id = 0;
			Pose              from;
			Clock::time_point at{};
			std::uint32_t     cell = 0;  // where it was held
			std::uint32_t     worldspace = 0;
		};

		struct FollowOn
		{
			Clock::time_point until{};
			std::uint32_t     count = 0;
		};

		ObjectRules::Causes                       causes;
		ObjectRules::Seen                         seen;
		ObjectRules::PartsSeen                    partsSeen;  // objects of several bodies' parts
		ObjectRules::Pacer                        pacer;
		ObjectRules::HoldPace                     holdPace;
		ObjectRules::Quiet                        quiet;
		ObjectRules::Echo                         echo;
		ObjectRules::Falls                        falls;  // dead bodies near us: their falls are causes
		ObjectRules::Waiting                      waiting;
		std::map<RE::FormID, Watch>               watches;
		std::map<RE::FormID, FollowOn>            followOns;
		std::map<RE::FormID, FriendHold>          friendHolds;
		std::vector<Protocol::ObjectRest>         inbox;    // rests heard, put on the next tick
		std::optional<RE::FormID>                 ourHold;  // what our player holds (said)
		std::optional<Pose>                       ourHoldPose;
		std::pair<std::uint32_t, std::uint32_t>   ourHoldSpace{};  // the cell or worldspace of ourHoldPose
		bool                                      ourHoldTelekinesis = false;
		ObjectRules::HoldEnds                     holdEnds;
		std::optional<RE::FormID>                 severalGrab;  // an object of several bodies our player holds (not said)
		std::optional<LetGo>                      letGo;
		std::optional<RE::FormID>                 refusedGrab;  // a friend holds what our player grabbed: dropped
		std::unordered_map<std::uint32_t, Vec>    shotsSeen;   // projectiles' last place, for their path
		std::optional<Clock::time_point>          nextScan;
		std::optional<Clock::time_point>          nextWaiting;
		std::optional<Clock::time_point>          lastFrame;
		bool                                      askPlaces = false;

		struct Counts
		{
			std::uint64_t holdsSent = 0, holdsHeard = 0, holdsDrawn = 0, holdsRefused = 0, grabsDropped = 0, holdsEnded = 0;
			std::uint64_t telekinesisHolds = 0, lateHolds = 0, parted = 0, carried = 0, carriedOver = 0, leftBehind = 0, notMovable = 0;
			std::uint64_t partRests = 0, partsWritten = 0, partsUnlike = 0, severalGrabs = 0, partMoves = 0, partsOnly = 0;
			std::uint64_t letGosSent = 0, letGosHeard = 0, letGosApplied = 0;
			std::uint64_t restsSaid = 0, restsSent = 0, restsHeard = 0, restsApplied = 0, restsLeft = 0, answers = 0, catchUps = 0;
			std::uint64_t placesApplied = 0, goneSent = 0, goneHeard = 0;
			std::uint64_t scans = 0, started = 0, notOurs = 0, hushed = 0, followOns = 0, lost = 0, gaveUp = 0;
			std::uint64_t bodyFalls = 0, ourPaths = 0, theirPaths = 0;
			std::uint64_t badPoses = 0, writes = 0, writeFails = 0, refWrites = 0, refUnwritten = 0, bodyOnly = 0, keyframes = 0, releases = 0;
		};
		Counts counts;

		// ---- the engine ----

		// The rigid bodies of a 3D (at most a few deep), and the first one.
		ObjectRules::Motion MotionOf(RE::bhkRigidBody* a_body)
		{
			const auto hk = a_body ? a_body->GetRigidBody() : nullptr;
			return hk ? static_cast<ObjectRules::Motion>(hk->motion.type.underlying()) : ObjectRules::Motion::kInvalid;
		}

		struct Bodies
		{
			struct One
			{
				RE::bhkRigidBody* body = nullptr;
				RE::NiAVObject*   node = nullptr;  // the node that holds its collision
			};
			RE::bhkRigidBody* first = nullptr;  // the lead: the first loose body, or the first body when none is loose
			RE::NiAVObject*   node = nullptr;   // the node that holds the lead's collision
			std::uint32_t     count = 0;        // counted to one past Protocol::MAX_OBJECT_PARTS
			std::array<One, Protocol::MAX_OBJECT_PARTS> all{};  // the first MAX_OBJECT_PARTS, in the order the 3D holds them
		};
		void FindBodies(RE::NiAVObject* a_node, Bodies& a_out, int a_depth)
		{
			if (!a_node || a_depth > 24 || a_out.count > Protocol::MAX_OBJECT_PARTS) {
				return;
			}
			if (const auto collision = a_node->collisionObject.get()) {
				if (const auto niCollision = netimmerse_cast<RE::bhkNiCollisionObject*>(collision)) {
					if (const auto body = niCollision->body ? netimmerse_cast<RE::bhkRigidBody*>(niCollision->body.get()) : nullptr) {
						if (a_out.count < Protocol::MAX_OBJECT_PARTS) {
							a_out.all[a_out.count] = { body, a_node };
						}
						++a_out.count;
					}
				}
			}
			if (const auto node = a_node->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					FindBodies(child.get(), a_out, a_depth + 1);
				}
			}
		}

		Bodies BodiesOf(RE::TESObjectREFR* a_ref)
		{
			Bodies bodies;
			FindBodies(a_ref ? a_ref->Get3D() : nullptr, bodies, 0);
			const auto known = (std::min)(bodies.count, static_cast<std::uint32_t>(Protocol::MAX_OBJECT_PARTS));
			for (std::uint32_t i = 0; i < known && !bodies.first; ++i) {
				if (ObjectRules::Loose(MotionOf(bodies.all[i].body))) {
					bodies.first = bodies.all[i].body;
					bodies.node = bodies.all[i].node;
				}
			}
			if (!bodies.first && known > 0) {
				bodies.first = bodies.all[0].body;
				bodies.node = bodies.all[0].node;
			}
			return bodies;
		}

		// An object of several bodies' loose ones, named (ObjectRules::PartNames), in the order its 3D holds them.
		struct Part
		{
			RE::bhkRigidBody* body = nullptr;
			std::uint32_t     name = 0;
		};
		std::vector<Part> LooseParts(const Bodies& a_bodies)
		{
			std::vector<Part>             out;
			std::vector<std::string_view> names;
			const auto                    known = (std::min)(a_bodies.count, static_cast<std::uint32_t>(Protocol::MAX_OBJECT_PARTS));
			for (std::uint32_t i = 0; i < known; ++i) {
				const auto& one = a_bodies.all[i];
				if (ObjectRules::Loose(MotionOf(one.body))) {
					out.push_back({ one.body, 0 });
					const auto name = one.node ? one.node->name.c_str() : nullptr;
					names.emplace_back(name ? name : "");
				}
			}
			const auto hashes = ObjectRules::PartNames(names);
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i].name = hashes[i];
			}
			return out;
		}
		std::vector<std::uint32_t> NamesOf(const std::vector<Part>& a_parts)
		{
			std::vector<std::uint32_t> out;
			for (const auto& part : a_parts) {
				out.push_back(part.name);
			}
			return out;
		}

		bool Transient(const RE::TESObjectREFR* a_ref)
		{
			switch (a_ref->GetFormType()) {
			case RE::FormType::ProjectileMissile:
			case RE::FormType::ProjectileArrow:
			case RE::FormType::ProjectileGrenade:
			case RE::FormType::ProjectileBeam:
			case RE::FormType::ProjectileFlame:
			case RE::FormType::ProjectileCone:
			case RE::FormType::ProjectileBarrier:
			case RE::FormType::Explosion:
			case RE::FormType::PlacedHazard:
				return true;
			default:
				return false;
			}
		}

		// A base that is never loose (a static, a tree, grass, flora, a door, furniture, a container): skipped before its
		// 3D is walked, so a city's buildings cost the four looks a second nothing.
		bool FixedKind(const RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref->GetBaseObject();
			if (!base) {
				return true;
			}
			switch (base->GetFormType()) {
			case RE::FormType::Static:
			case RE::FormType::Tree:
			case RE::FormType::Grass:
			case RE::FormType::Flora:
			case RE::FormType::Door:
			case RE::FormType::Furniture:
			case RE::FormType::Container:
				return true;
			default:
				return false;
			}
		}

		// The lead rigid body of a loose object of the class (ObjectRules::Movable: its only one, or its first loose one), or none.
		// a_bodies: what was found, for the caller (one walk of its 3D, not two).
		RE::bhkRigidBody* LooseBody(RE::TESObjectREFR* a_ref, Bodies* a_bodies = nullptr)
		{
			if (!a_ref) {
				return nullptr;
			}
			ObjectRules::Facts facts;
			facts.runtime = FormIds::IsRuntime(a_ref->GetFormID());
			facts.actor = a_ref->Is(RE::FormType::ActorCharacter);
			facts.transient = Transient(a_ref);
			facts.enabled = !a_ref->IsDisabled() && !a_ref->IsDeleted();
			facts.has3D = a_ref->Is3DLoaded();
			facts.fixedKind = !facts.actor && FixedKind(a_ref);
			if (facts.runtime || facts.actor || facts.transient || facts.fixedKind || !facts.enabled || !facts.has3D) {
				return nullptr;
			}
			facts.inAlias = a_ref->extraList.HasType(RE::ExtraDataType::kAliasInstanceArray);
			const auto bodies = BodiesOf(a_ref);
			facts.bodies = bodies.count;
			facts.motion = MotionOf(bodies.first);
			if (a_bodies) {
				*a_bodies = bodies;
			}
			return ObjectRules::Movable(facts) ? bodies.first : nullptr;
		}

		Vec FromHavok(const RE::hkVector4& a_v, float a_scale)
		{
			alignas(16) float f[4];
			_mm_store_ps(f, a_v.quad);
			return { f[0] * a_scale, f[1] * a_scale, f[2] * a_scale };
		}

		std::optional<Pose> PoseOf(RE::bhkRigidBody* a_body)
		{
			if (!a_body) {
				return std::nullopt;
			}
			RE::hkVector4    at;
			RE::hkQuaternion rot;
			a_body->GetPosition(at);
			a_body->GetRotation(rot);
			alignas(16) float q[4];
			_mm_store_ps(q, rot.vec.quad);
			Pose pose{ FromHavok(at, RE::bhkWorld::GetWorldScaleInverse()), ObjectRules::Normalized({ q[0], q[1], q[2], q[3] }) };
			if (!ObjectRules::Finite(pose.at) || !Protocol::QuatHolds(pose.rot)) {
				++counts.badPoses;
				return std::nullopt;  // a body gone NaN is never said (the sister project's Havok freeze)
			}
			return pose;
		}

		// Each loose part's pose by name (an object of several bodies).
		std::vector<std::pair<std::uint32_t, Pose>> PartPoses(const Bodies& a_bodies)
		{
			std::vector<std::pair<std::uint32_t, Pose>> out;
			for (const auto& part : LooseParts(a_bodies)) {
				if (const auto pose = PoseOf(part.body)) {
					out.emplace_back(part.name, *pose);
				}
			}
			return out;
		}

		// An object of several bodies' rest gets every loose part's pose; false when one has none (it is said whole or not
		// at all). An object of one body's is left as it is.
		bool AddParts(RE::TESObjectREFR* a_ref, Protocol::ObjectRest& a_rest)
		{
			const auto bodies = BodiesOf(a_ref);
			if (bodies.count <= 1) {
				return true;
			}
			const auto                       parts = LooseParts(bodies);
			std::vector<std::optional<Pose>> poses;
			for (const auto& part : parts) {
				poses.push_back(PoseOf(part.body));
			}
			auto made = ObjectRules::PartsOf(NamesOf(parts), poses);
			if (!made) {
				return false;
			}
			a_rest.parts = std::move(*made);
			++counts.partRests;
			return true;
		}

		ObjectRules::Mat3 MatFrom(const RE::NiMatrix3& a_m)
		{
			ObjectRules::Mat3 out;
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					out.m[r][c] = a_m.entry[r][c];
				}
			}
			return out;
		}

		// The engine's two conventions (ObjectRules, "the reference's own place"), learnt from the loose objects looked at.
		ObjectRules::SenseVotes bodySense;   // a node's world rotation against its body's quaternion
		ObjectRules::SenseVotes angleSense;  // a reference's angles against its 3D's world rotation

		void Learn(RE::TESObjectREFR* a_ref, const Bodies& a_bodies)
		{
			if (!bodySense.Done() && a_bodies.first && a_bodies.node) {
				RE::hkQuaternion rot;
				a_bodies.first->GetRotation(rot);
				alignas(16) float q[4];
				_mm_store_ps(q, rot.vec.quad);
				bodySense.Vote(ObjectRules::SenseOf(ObjectRules::MatOf({ q[0], q[1], q[2], q[3] }), MatFrom(a_bodies.node->world.rotate)));
			}
			const auto root = a_ref->Get3D();
			if (!angleSense.Done() && root) {
				// Only a reference whose place is its 3D's (it has not moved since its 3D did): its angles are then the 3D's too.
				const auto& t = root->world.translate;
				const auto  at = a_ref->GetPosition();
				if (ObjectRules::Distance({ t.x, t.y, t.z }, { at.x, at.y, at.z }) <= 1.0f) {
					const auto angle = a_ref->GetAngle();
					angleSense.Vote(ObjectRules::SenseOf(ObjectRules::FromAnglesXYZ({ angle.x, angle.y, angle.z }), MatFrom(root->world.rotate)));
				}
			}
		}

		const char* SenseName(ObjectRules::Sense a_sense)
		{
			switch (a_sense) {
			case ObjectRules::Sense::kSame:
				return "same";
			case ObjectRules::Sense::kTransposed:
				return "transposed";
			default:
				return "unknown";
			}
		}

		// Puts a loose object at a pose with a velocity (units a second; the pose is its body's): the reference first (its
		// place and angles, then its 3D and collision moved with them, as the engine moves a reference: what the save, a
		// cell's reload and the game's own drawing use), then its body, kept in step and given the velocity. A body moved
		// alone, asleep, would collide where its mesh is not, and the reference would go back to its old place on a reload.
		// a_moveRef false: the body alone (a held copy between its reference's writes).
		// An object of several bodies is put whole or not at all: at rest, with a_parts naming exactly its loose bodies
		// (ObjectRules::PartsAlike), its reference from its lead's part, then every part.
		bool Write(RE::TESObjectREFR* a_ref, const Pose& a_pose, const Vec& a_velocity, bool a_moveRef = true, const std::vector<Protocol::ObjectPart>* a_parts = nullptr)
		{
			const auto bodies = a_ref ? BodiesOf(a_ref) : Bodies{};
			if (!a_ref || bodies.count == 0 || bodies.count > Protocol::MAX_OBJECT_PARTS || !ObjectRules::Finite(a_velocity)) {
				++counts.writeFails;
				return false;
			}
			const bool several = bodies.count > 1;
			if (!several && a_parts) {
				++counts.partsUnlike;  // a rest of several bodies for one of one here: another mesh, left as it lies
				return false;
			}
			Pose lead = a_pose;
			bool moveRef = a_moveRef;
			if (several) {
				const auto parts = LooseParts(bodies);
				const bool still = a_velocity.x == 0.0f && a_velocity.y == 0.0f && a_velocity.z == 0.0f;
				if (!a_parts || !a_moveRef || !still || !ObjectRules::PartsAlike(NamesOf(parts), *a_parts)) {
					++counts.partsUnlike;
					return false;
				}
				// Its reference moves with its lead only when every body is loose (a heap of bones). One with a fixed or
				// keyframed body (a chain's anchor, a lantern's hook) keeps its reference where it is, and only its loose
				// bodies are put: moving the reference would take the anchor along by the lead's swing, and drift.
				if (parts.size() != bodies.count) {
					moveRef = false;
					++counts.partsOnly;
				}
				// The reference goes where our lead's part lies in the rest (its own place is its sender's lead's).
				for (const auto& part : parts) {
					if (part.body == bodies.first) {
						const auto it = std::find_if(a_parts->begin(), a_parts->end(), [&](const Protocol::ObjectPart& a_part) { return a_part.name == part.name; });
						if (it != a_parts->end()) {
							lead = ObjectRules::PoseOf(*it);
						}
					}
				}
			}
			const auto here = PoseOf(bodies.first);
			const auto root = a_ref->Get3D();
			const auto senseOfBody = bodySense.Settled();
			const auto senseOfAngles = angleSense.Settled();
			const auto refWas = a_ref->GetPosition();
			const auto anglesWas = a_ref->GetAngle();
			bool       refMoved = false;
			if (!moveRef) {
				++counts.bodyOnly;
			} else if (here && root && senseOfBody != ObjectRules::Sense::kUnknown && senseOfAngles != ObjectRules::Sense::kUnknown) {
				const auto& t = root->world.translate;
				const Pose  rootNow{ { t.x, t.y, t.z }, ObjectRules::RotOf(MatFrom(root->world.rotate), senseOfBody) };
				// Where the body lies in the reference's frame (its collision may sit off the root), and so where the
				// reference goes for the body to lie at a_pose.
				const auto refPose = ObjectRules::RefFor(lead, ObjectRules::OffsetOf(rootNow, *here));
				const auto angles = ObjectRules::AnglesFor(ObjectRules::WorldOf(refPose.rot, senseOfBody), senseOfAngles);
				if (ObjectRules::Writable(refPose, rootNow.at) && ObjectRules::Finite(angles)) {
					a_ref->SetPosition(RE::NiPoint3{ refPose.at.x, refPose.at.y, refPose.at.z });
					a_ref->SetAngle(RE::NiPoint3{ angles.x, angles.y, angles.z });
					a_ref->Update3DPosition(true);
					refMoved = true;
					++counts.refWrites;
				} else {
					++counts.badPoses;
				}
			} else {
				++counts.refUnwritten;  // the conventions are not learnt yet (no loose object looked at): the body alone
			}
			// The bodies, found again (the 3D's update may have warped them), written exactly and set moving, under their
			// world's lock.
			const auto                                      now = BodiesOf(a_ref);
			std::vector<std::pair<RE::bhkRigidBody*, Pose>> puts;
			if (several) {
				for (const auto& part : LooseParts(now)) {
					const auto it = std::find_if(a_parts->begin(), a_parts->end(), [&](const Protocol::ObjectPart& a_part) { return a_part.name == part.name; });
					if (it == a_parts->end()) {
						// Its 3D made anew otherwise: whole or none, so its reference goes back where it was.
						if (refMoved) {
							a_ref->SetPosition(refWas);
							a_ref->SetAngle(anglesWas);
							a_ref->Update3DPosition(true);
						}
						++counts.partsUnlike;
						return false;
					}
					puts.emplace_back(part.body, ObjectRules::PoseOf(*it));
				}
			} else if (now.first) {
				puts.emplace_back(now.first, a_pose);
			}
			if (puts.empty()) {
				++counts.writeFails;
				return false;
			}
			const auto          scale = RE::bhkWorld::GetWorldScale();
			const RE::hkVector4 velocity{ a_velocity.x * scale, a_velocity.y * scale, a_velocity.z * scale, 0.0f };
			const auto          putAll = [&]() {
				for (const auto& [body, pose] : puts) {
					RE::hkVector4    at{ pose.at.x * scale, pose.at.y * scale, pose.at.z * scale, 0.0f };
					RE::hkQuaternion rot;
					rot.vec = RE::hkVector4{ pose.rot.x, pose.rot.y, pose.rot.z, pose.rot.w };
					body->SetPositionAndRotation(at, rot);
					body->SetLinearVelocity(velocity);
					body->SetAngularVelocity(RE::hkVector4{ 0.0f });
				}
			};
			const auto cell = a_ref->GetParentCell();
			const auto world = cell ? cell->GetbhkWorld() : nullptr;
			if (world) {
				RE::BSWriteLockGuard guard{ world->worldLock };
				putAll();
			} else {
				putAll();
			}
			a_ref->AddChange(static_cast<std::uint32_t>(RE::TESObjectREFR::ChangeFlags::kHavokMoved));
			++counts.writes;
			counts.partsWritten += several ? puts.size() : 0;
			return true;
		}

		Vec At(const RE::TESObjectREFR* a_ref)
		{
			const auto p = a_ref->GetPosition();
			return { p.x, p.y, p.z };
		}

		// A reference's place: its interior cell, or its worldspace outside.
		std::pair<std::uint32_t, std::uint32_t> SpaceOf(const RE::TESObjectREFR* a_ref)
		{
			const auto cell = a_ref->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				return { cell->GetFormID(), 0 };
			}
			const auto world = a_ref->GetWorldspace();
			return { 0, world ? world->GetFormID() : 0 };
		}

		bool SameSpace(const RE::TESObjectREFR* a_ref, std::uint32_t a_cell, std::uint32_t a_worldspace)
		{
			const auto [cell, worldspace] = SpaceOf(a_ref);
			return (cell != 0 && cell == a_cell) || (worldspace != 0 && worldspace == a_worldspace);
		}

		RE::TESObjectREFR* LoadedRef(RE::FormID a_id)
		{
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			return ref && ref->Is3DLoaded() && !ref->IsDisabled() && !ref->IsDeleted() ? ref : nullptr;
		}

		enum class Whose
		{
			kNobody,
			kOurs,
			kTheirs,
		};

		// Whose an actor's doing is: our player's and an NPC's this game runs are ours; a friend's stand-in's and a copy
		// of an NPC another game runs are theirs; an NPC nobody runs (or not shared) is nobody's.
		Whose WhoseActor(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return Whose::kNobody;
			}
			if (a_actor->IsPlayerRef()) {
				return Whose::kOurs;
			}
			if (RemotePlayers::PlayerOf(a_actor) != 0) {
				return Whose::kTheirs;
			}
			if (const auto key = NpcSync::KeyOf(a_actor)) {
				const auto runner = NpcSync::RunnerOf(*key);
				if (runner == ourId) {
					return Whose::kOurs;
				}
				if (runner != 0) {
					return Whose::kTheirs;
				}
			}
			return Whose::kNobody;
		}

		Whose WhoseRef(RE::TESObjectREFR* a_ref)
		{
			return a_ref ? WhoseActor(a_ref->As<RE::Actor>()) : Whose::kNobody;
		}

		void AddCause(const Vec& a_from, const Vec& a_to, float a_reach, ObjectRules::Strength a_strength, Whose a_whose, Clock::time_point a_until,
			std::uint32_t a_object = 0)
		{
			if (a_whose == Whose::kNobody) {
				return;
			}
			causes.Add({ a_from, a_to, a_reach, a_strength, a_whose == Whose::kOurs, a_until, a_object });
		}

		// The motion type our copy had before a hold, to put back (dynamic when it was not one physics moves).
		RE::hkpMotion::MotionType MotionTypeOf(std::optional<ObjectRules::Motion> a_motion)
		{
			const auto motion = a_motion && ObjectRules::Loose(*a_motion) ? *a_motion : ObjectRules::Motion::kDynamic;
			return static_cast<RE::hkpMotion::MotionType>(static_cast<std::uint8_t>(motion));
		}

		// Our copy of a friend's held object falls where it is: its motion type and collision layer as they were before the
		// hold (so it is pushed, picked up and activated as before), its reference where its body is.
		void Release(RE::FormID a_id, FriendHold& a_hold)
		{
			if (!a_hold.keyframed) {
				return;
			}
			a_hold.keyframed = false;
			if (const auto ref = LoadedRef(a_id)) {
				if (a_hold.layer) {
					if (const auto node = BodiesOf(ref).node) {
						node->SetCollisionLayer(*a_hold.layer);
					}
				}
				// Its reference where its body is now (written at most HOLD_REF_EVERY ago): the save and a reload keep it there.
				if (const auto pose = PoseOf(BodiesOf(ref).first)) {
					Write(ref, *pose, {});
				}
				ref->SetMotionType(MotionTypeOf(a_hold.motion), true);
				++counts.releases;
			}
		}

		// What our player holds now: the grab (Z), or telekinesis (its effect's object).
		struct OurGrab
		{
			RE::TESObjectREFRPtr ref;
			bool                 telekinesis = false;
		};
		RE::TelekinesisEffect* TelekinesisOf(RE::PlayerCharacter* a_player)
		{
			const auto target = a_player->AsMagicTarget();
			const auto list = target ? target->GetActiveEffectList() : nullptr;
			if (!list) {
				return nullptr;
			}
			for (const auto effect : *list) {
				const auto base = effect ? effect->GetBaseObject() : nullptr;
				if (!base || base->GetArchetype() != RE::EffectSetting::Archetype::kTelekinesis ||
					effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
					continue;
				}
				return static_cast<RE::TelekinesisEffect*>(effect);
			}
			return nullptr;
		}
		OurGrab GrabOf(RE::PlayerCharacter* a_player)
		{
			if (auto grabbed = a_player->GetGrabbedRef()) {
				return { std::move(grabbed), false };
			}
			if (const auto effect = TelekinesisOf(a_player)) {
				if (auto ref = effect->grabbedObject.get()) {
					return { std::move(ref), true };
				}
			}
			return {};
		}

		// Our player lets go of a_id (a friend holds it): the grab's springs go, a telekinesis holding it ends.
		void DropOurGrab(RE::PlayerCharacter* a_player, RE::FormID a_id)
		{
			if (const auto grabbed = a_player->GetGrabbedRef(); grabbed && grabbed->GetFormID() == a_id) {
				a_player->DestroyMouseSprings();
			}
			if (const auto effect = TelekinesisOf(a_player)) {
				if (const auto ref = effect->grabbedObject.get(); ref && ref->GetFormID() == a_id) {
					effect->Dispel(true);
				}
			}
		}

		// Whether a reference may be moved into another cell for a hold or a rest (what LooseBody needs, but its 3D: it
		// may be in a cell not loaded here).
		bool MayCarry(RE::TESObjectREFR* a_ref)
		{
			return a_ref && !FormIds::IsRuntime(a_ref->GetFormID()) && !a_ref->Is(RE::FormType::ActorCharacter) && !Transient(a_ref) && !FixedKind(a_ref) &&
			       !a_ref->IsDisabled() && !a_ref->IsDeleted() && !a_ref->extraList.HasType(RE::ExtraDataType::kAliasInstanceArray);
		}

		void ReleaseAll()
		{
			for (auto& [id, hold] : friendHolds) {
				Release(id, hold);
			}
			friendHolds.clear();
		}

		// ---- ours ----

		void SayHold(const SendFn& a_send, RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			const auto grab = GrabOf(a_player);
			const auto grabbed = grab.ref;
			auto       body = grabbed ? LooseBody(grabbed.get()) : nullptr;
			if (body && BodiesOf(grabbed.get()).count > 1) {
				// An object of several bodies is not drawn in a friend's hands: its grab is our cause (so its move is ours),
				// and its rest is said whole once it settles.
				if (const auto pose = PoseOf(body)) {
					AddCause(pose->at, pose->at, 0.0f, ObjectRules::Strength::kGrab, Whose::kOurs, a_now + 1s, grabbed->GetFormID());
					if (!severalGrab || *severalGrab != grabbed->GetFormID()) {
						severalGrab = grabbed->GetFormID();
						++counts.severalGrabs;
					}
				}
				body = nullptr;
			} else {
				severalGrab.reset();
			}
			const auto id = body ? grabbed->GetFormID() : 0;
			if (ourHold && *ourHold != id) {
				// Let go of (or another grabbed): its flight is said LET_GO_AFTER later, from where it left the hands.
				if (ourHoldPose && !(refusedGrab && *refusedGrab == *ourHold)) {
					letGo = LetGo{ *ourHold, *ourHoldPose, a_now, ourHoldSpace.first, ourHoldSpace.second };
				}
				ourHold.reset();
				ourHoldPose.reset();
				holdPace.Reset();
			}
			if (refusedGrab && *refusedGrab != id) {
				refusedGrab.reset();
			}
			if (body && friendHolds.contains(id) && !(refusedGrab && *refusedGrab == id)) {
				// Our player grabbed what a friend holds: the server would refuse it, so it is never asked; our player lets go.
				DropOurGrab(a_player, id);
				refusedGrab = id;
				++counts.grabsDropped;
				logs::info("Objects: player {} holds {:08X}; our player's grab of it was let go", friendHolds[id].holder, id);
			}
			if (!body || (refusedGrab && *refusedGrab == id) || friendHolds.contains(id)) {
				return;
			}
			const auto pose = PoseOf(body);
			if (!pose) {
				return;
			}
			if (!ourHold || grab.telekinesis != ourHoldTelekinesis) {
				counts.telekinesisHolds += grab.telekinesis ? 1 : 0;
			}
			ourHold = id;
			ourHoldPose = pose;
			ourHoldSpace = SpaceOf(grabbed.get());
			ourHoldTelekinesis = grab.telekinesis;
			watches.erase(id);
			AddCause(pose->at, pose->at, 0.0f, ObjectRules::Strength::kGrab, Whose::kOurs, a_now + 1s, id);
			if (!holdPace.Due(*pose, a_now)) {
				return;
			}
			Protocol::ObjectHolds msg;
			msg.holds.push_back(ObjectRules::ToHolder({ Protocol::ObjectKind::kRef, id }, At(a_player), a_player->GetAngleZ(), *pose));
			if (Protocol::ObjectHoldHolds(msg.holds.front())) {
				a_send(Protocol::Encode(msg), false);
				++counts.holdsSent;
			}
		}

		void SayLetGo(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			if (!letGo || a_now - letGo->at < ObjectRules::LET_GO_AFTER) {
				return;
			}
			const auto done = *letGo;
			letGo.reset();
			const auto ref = LoadedRef(done.id);
			const auto body = ref ? LooseBody(ref) : nullptr;
			const auto pose = body ? PoseOf(body) : std::nullopt;
			if (!pose) {
				// Gone from what is loaded as the hands left it (our player went through a door, or out of the cell, and it
				// stayed behind): it lies where it was held last, in the cell it was held in.
				const auto form = RE::TESForm::LookupByID<RE::TESObjectREFR>(done.id);
				if (!ref && form && !form->IsDisabled() && !form->IsDeleted() && (done.cell != 0 || done.worldspace != 0)) {
					pacer.Push(ObjectRules::RestOf(done.id, Protocol::RestKind::kRest, done.cell, done.worldspace, done.from), a_now);
					++counts.leftBehind;
					++counts.restsSaid;
				}
				return;
			}
			const auto [cell, worldspace] = SpaceOf(ref);
			const auto velocity = ObjectRules::Velocity(done.from.at, done.at, pose->at, a_now);
			pacer.Push(ObjectRules::RestOf(done.id, Protocol::RestKind::kLetGo, cell, worldspace, done.from, velocity), a_now);
			++counts.letGosSent;
			// Ours until it rests.
			watches[done.id] = Watch{ ObjectRules::Settle{ *pose, a_now }, a_now, true };
			(void)a_player;
		}

		// What recorded causes say, now on the main thread.
		void TakeRecorded(Clock::time_point a_now)
		{
			std::vector<Blast>      takenBlasts;
			std::vector<Hit>        takenHits;
			std::vector<RE::FormID> takenPickups;
			std::vector<ShotPath>   takenPaths;
			{
				std::scoped_lock l{ recordLock };
				takenBlasts.swap(blasts);
				takenHits.swap(hits);
				takenPickups.swap(pickups);
				// Flights no update came for are over: their last stretch.
				for (auto it = flights.begin(); it != flights.end();) {
					if (it->second.flight.Over(a_now)) {
						if (const auto rest = it->second.flight.Rest()) {
							PushPath(*rest, it->second.reach, it->second.shooter, a_now);
						}
						it = flights.erase(it);
					} else {
						++it;
					}
				}
				takenPaths.swap(shotPaths);
			}
			for (const auto& path : takenPaths) {
				const auto shooter = path.shooter.get();
				AddCause(path.stretch.from, path.stretch.to, path.reach, ObjectRules::Strength::kStrike, WhoseRef(shooter.get()), path.when + ObjectRules::SHOT_HOLD);
			}
			for (const auto& blast : takenBlasts) {
				const auto owner = blast.owner.get();
				AddCause(blast.at, blast.at, blast.radius + ObjectRules::BLAST_MARGIN, ObjectRules::Strength::kStrike, WhoseActor(owner.get()), blast.when + ObjectRules::BLAST_HOLD);
			}
			for (const auto& hit : takenHits) {
				const auto cause = hit.cause.get();
				AddCause({}, {}, 0.0f, ObjectRules::Strength::kStrike, WhoseRef(cause.get()), hit.when + ObjectRules::HIT_HOLD, hit.target);
			}
			for (const auto id : takenPickups) {
				watches.erase(id);
				// Picked up as (or just after) it left the hands: no let-go and no hold of it is said any more.
				if (letGo && letGo->id == id) {
					letGo.reset();
				}
				if (ourHold && *ourHold == id) {
					ourHold.reset();
					ourHoldPose.reset();
					holdPace.Reset();
				}
				waiting.Forget({ Protocol::ObjectKind::kRef, id });
				Protocol::ObjectRest gone;
				gone.key = { Protocol::ObjectKind::kRef, id };
				gone.kind = Protocol::RestKind::kGone;
				pacer.Push(gone, a_now);
				++counts.goneSent;
			}
		}

		// One look at what lies near our player.
		void Scan(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			++counts.scans;
			causes.Prune(a_now);
			seen.Prune(a_now);
			partsSeen.Prune(a_now);
			quiet.Prune(a_now);
			falls.Prune(a_now);
			const auto me = At(a_player);
			AddCause(me, me, ObjectRules::PLAYER_REACH, ObjectRules::Strength::kBody, Whose::kOurs, a_now + ObjectRules::BODY_HOLD);
			for (const auto& [id, hold] : friendHolds) {
				AddCause({}, {}, 0.0f, ObjectRules::Strength::kGrab, Whose::kTheirs, a_now + 1s, id);
			}

			// One walk of what is near: actors and projectiles are causes, loose objects are looked at.
			struct Near
			{
				RE::TESObjectREFR* ref;
				RE::bhkRigidBody*  body;
				float              distance;
				bool               several;  // of several bodies
			};
			std::vector<Near>                      nearBy;
			std::unordered_map<std::uint32_t, Vec> shotsNow;
			const auto                             tes = RE::TES::GetSingleton();
			if (!tes) {
				return;
			}
			// Every projectile's path comes from its own updates (Flew) when every class is hooked; else from these looks.
			const bool sampleShots = !Shots::AllHooked();
			tes->ForEachReferenceInRange(a_player, SCAN_RANGE + ObjectRules::CONE_REACH, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref == a_player) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto at = At(a_ref);
				if (const auto actor = a_ref->As<RE::Actor>()) {
					if (!actor->IsDead()) {
						const auto whose = WhoseActor(actor);
						const auto reach = RemotePlayers::PlayerOf(actor) != 0 ? ObjectRules::PLAYER_REACH : ObjectRules::NPC_REACH;
						AddCause(at, at, reach, ObjectRules::Strength::kBody, whose, a_now + ObjectRules::BODY_HOLD);
					} else if (const auto root = actor->Get3D()) {
						// A dead body falling (a ragdoll knocks a table's clutter flying): its path since the last look, by the
						// middle of its bound (the reference stays where it died while the bones fly), its runner's.
						const auto& bound = root->worldBound;
						if (const auto fall = falls.Look(actor->GetFormID(), { bound.center.x, bound.center.y, bound.center.z }, a_now)) {
							const auto whose = WhoseActor(actor);
							if (whose != Whose::kNobody) {
								AddCause(fall->from, fall->to, ObjectRules::BodyReach(bound.radius), ObjectRules::Strength::kStrike, whose, a_now + ObjectRules::SHOT_HOLD);
								++counts.bodyFalls;
							}
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (a_ref->As<RE::Projectile>() && !sampleShots) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (const auto projectile = a_ref->As<RE::Projectile>()) {
					const auto shooter = projectile->GetProjectileRuntimeData().shooter.get();
					const auto whose = WhoseRef(shooter.get());
					const auto from = shotsSeen.find(a_ref->GetFormID());
					const auto reach = a_ref->GetFormType() == RE::FormType::ProjectileCone ? ObjectRules::CONE_REACH : ObjectRules::SHOT_REACH;
					AddCause(from != shotsSeen.end() ? from->second : at, at, reach, ObjectRules::Strength::kStrike, whose, a_now + ObjectRules::SHOT_HOLD);
					shotsNow[a_ref->GetFormID()] = at;
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto distance = ObjectRules::Distance(at, me);
				if (distance > SCAN_RANGE) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				Bodies bodies;
				if (const auto body = LooseBody(a_ref, &bodies)) {
					nearBy.push_back({ a_ref, body, distance, bodies.count > 1 });
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			shotsSeen.swap(shotsNow);
			if (nearBy.size() > MAX_LOOKED) {
				std::nth_element(nearBy.begin(), nearBy.begin() + static_cast<std::ptrdiff_t>(MAX_LOOKED), nearBy.end(),
					[](const Near& a_a, const Near& a_b) { return a_a.distance < a_b.distance; });
				nearBy.resize(MAX_LOOKED);
			}

			// First every look, and the paths of what flies (an object in flight knocks over what it passes); then whose
			// each move is, every path known.
			struct Looked
			{
				RE::FormID id;
				Vec        from;  // where what moved lay (the lead, or the part of several that moved most)
				Vec        to;    // and lies now
				Pose       pose;  // the lead's
			};
			std::vector<Looked> moved;
			for (const auto& one : nearBy) {
				const auto id = one.ref->GetFormID();
				if (!bodySense.Done() || !angleSense.Done()) {
					Learn(one.ref, BodiesOf(one.ref));
				}
				const auto pose = PoseOf(one.body);
				if (!pose) {
					continue;
				}
				// An object of several bodies: its parts, looked at with its lead (a knock on a chain's far end may leave
				// its lead, by the anchor, where it was).
				const auto parts = one.several ? PartPoses(BodiesOf(one.ref)) : ObjectRules::NamedPoses{};
				const auto partMove = one.several ? partsSeen.Look(id, parts, a_now) : std::nullopt;
				if ((ourHold && *ourHold == id) || friendHolds.contains(id)) {
					seen.Set(id, *pose, a_now);
					continue;
				}
				const auto root = one.ref->Get3D();
				const auto reach = ObjectRules::ObjectReach(root ? root->worldBound.radius : 0.0f);
				if (watches.contains(id)) {
					// Ours in flight: its path is ours.
					const auto prior = seen.Peek(id);
					seen.Set(id, *pose, a_now);
					if (prior && ObjectRules::Moved(*prior, *pose)) {
						AddCause(prior->at, pose->at, reach, ObjectRules::Strength::kStrike, Whose::kOurs, a_now + ObjectRules::SHOT_HOLD);
						++counts.ourPaths;
					}
					continue;
				}
				const auto before = seen.Look(id, *pose, a_now);
				if (!before && !partMove) {
					continue;
				}
				// What moved most: the lead, or a part that moved farther.
				Vec from = before ? before->at : pose->at;
				Vec to = pose->at;
				if (partMove && (!before || ObjectRules::Distance(partMove->from, partMove->to) > ObjectRules::Distance(from, to))) {
					from = partMove->from;
					to = partMove->to;
					++counts.partMoves;
				}
				if (quiet.Hushed(id, a_now)) {
					// A friend's move put it there: our physics settling it is not ours to say, and what it knocks over on
					// the way is theirs.
					++counts.hushed;
					AddCause(from, to, reach, ObjectRules::Strength::kStrike, Whose::kTheirs, a_now + ObjectRules::SHOT_HOLD);
					++counts.theirPaths;
					continue;
				}
				moved.push_back({ id, from, to, *pose });
			}

			for (const auto& one : moved) {
				const auto  id = one.id;
				const auto* pose = &one.pose;
				bool        ours = causes.Judge(id, one.from, one.to, a_now).Ours();
				if (!ours) {
					if (const auto follow = followOns.find(id); follow != followOns.end() && a_now < follow->second.until && follow->second.count < FOLLOW_ON_MAX) {
						ours = true;  // a rest of ours that rolls on
						++follow->second.count;
						++counts.followOns;
					}
				}
				if (!ours) {
					++counts.notOurs;
					continue;
				}
				if (watches.size() >= MAX_WATCHED) {
					continue;
				}
				watches[id] = Watch{ ObjectRules::Settle{ *pose, a_now }, a_now, false };
				++counts.started;
			}
		}

		// Our moves, followed until each rests (wherever it flew).
		void Follow(Clock::time_point a_now)
		{
			for (auto it = watches.begin(); it != watches.end();) {
				const auto id = it->first;
				auto&      watch = it->second;
				const auto ref = LoadedRef(id);
				const auto body = ref ? LooseBody(ref) : nullptr;
				const auto pose = body ? PoseOf(body) : std::nullopt;
				if (!pose) {
					if (a_now - watch.lastFound > LOST_AFTER) {
						++counts.lost;
						it = watches.erase(it);
					} else {
						++it;
					}
					continue;
				}
				watch.lastFound = a_now;
				bool rested = watch.settle.Look(*pose, a_now);
				// An object of several bodies is at rest once every loose part is (a chain's far end may swing on while its
				// lead, by the anchor, is still).
				if (const auto bodies = BodiesOf(ref); bodies.count > 1) {
					if (!watch.parts) {
						watch.parts.emplace(watch.settle.Began());
					}
					rested = watch.parts->Look(PartPoses(bodies), a_now) && rested;
				}
				if (!rested) {
					++it;
					continue;
				}
				if (watch.settle.GaveUp()) {
					++counts.gaveUp;
				}
				seen.Set(id, *pose, a_now);
				auto& follow = followOns[id];
				follow.until = a_now + FOLLOW_ON;
				if (echo.MaySay(id, a_now)) {
					const auto [cell, worldspace] = SpaceOf(ref);
					auto rest = ObjectRules::RestOf(id, Protocol::RestKind::kRest, cell, worldspace, *pose);
					if (AddParts(ref, rest)) {
						pacer.Push(rest, a_now);
						++counts.restsSaid;
					}
				}
				it = watches.erase(it);
			}
			std::erase_if(followOns, [&](const auto& a_entry) { return a_now >= a_entry.second.until; });
		}

		// ---- theirs ----

		// Two exterior cells' grid distance (none when either is not an exterior).
		std::optional<std::int32_t> CellsApart(RE::TESObjectCELL* a_a, RE::TESObjectCELL* a_b)
		{
			const auto a = a_a->IsExteriorCell() ? a_a->GetCoordinates() : nullptr;
			const auto b = a_b->IsExteriorCell() ? a_b->GetCoordinates() : nullptr;
			if (!a || !b) {
				return std::nullopt;
			}
			return (std::max)(std::abs(a->cellX - b->cellX), std::abs(a->cellY - b->cellY));
		}

		// Draws each friend's hold in front of their stand-in.
		void DrawHolds(Clock::time_point a_now, float a_dt)
		{
			for (auto it = friendHolds.begin(); it != friendHolds.end();) {
				auto& [id, hold] = *it;
				if (a_now - hold.heard > HOLD_STALE) {
					Release(id, hold);
					++counts.holdsEnded;
					it = friendHolds.erase(it);
					continue;
				}
				++it;
				const auto standIn = RemotePlayers::StandInOf(hold.holder);
				const auto ref = LoadedRef(id);
				if (!ref) {
					continue;
				}
				// Only one of the class here, before anything moves it: a hold names what its holder's game calls loose,
				// and a plugin only one game has may make that reference a door, a static, a quest's item or an actor
				// here. Such a one is never drawn, released or carried (it was drawn before: what it was is known).
				if (!MayCarry(ref) || (!hold.motion && !LooseBody(ref)) || BodiesOf(ref).count != 1) {
					continue;  // (an object of several bodies is never drawn in a friend's hands)
				}
				// The holder is not here (gone through a door, out of what is loaded): our copy falls where it is, and is
				// drawn again once the stand-in is back. The holder's rest says where it ends.
				if (!standIn || !standIn->Is3DLoaded() || standIn->IsDisabled()) {
					if (hold.keyframed) {
						Release(id, hold);
						hold.parted = true;
						hold.drawn.reset();
						++counts.parted;
					}
					continue;
				}
				// Carried into another cell (a door both went through, or far across exterior cells: ObjectRules::CarryDue):
				// our copy goes into the stand-in's cell first, then is drawn there (its 3D may be made again: keyframed and
				// out of collisions anew).
				const auto copyCell = ref->GetParentCell();
				const auto standInCell = standIn->GetParentCell();
				if (copyCell && standInCell && copyCell != standInCell &&
					ObjectRules::CarryDue(SpaceOf(ref) == SpaceOf(standIn.get()), CellsApart(copyCell, standInCell))) {
					if (!hold.carried || a_now - *hold.carried >= CARRY_EVERY) {
						hold.carried = a_now;
						ref->MoveTo(standIn.get());
						hold.reapply = hold.keyframed;  // still ours to put back as it was (Release), whatever its 3D now
						hold.refWritten.reset();
						++counts.carried;
					}
					continue;
				}
				hold.parted = false;
				const auto target = ObjectRules::FromHolder(hold.hold, At(standIn.get()), standIn->GetAngleZ());
				hold.drawn = hold.drawn ? ObjectRules::Ease(*hold.drawn, target, a_dt) : target;
				hold.trailMax = (std::max)(hold.trailMax, ObjectRules::Distance(hold.drawn->at, target.at));
				if (!ObjectRules::Writable(*hold.drawn, At(ref))) {
					++counts.badPoses;
					continue;
				}
				if (!hold.keyframed || hold.reapply) {
					hold.reapply = false;
					// What it was, read once (a copy set again after a move into another cell keeps the first reading).
					const auto bodies = BodiesOf(ref);
					if (!hold.motion) {
						hold.motion = MotionOf(bodies.first);
					}
					ref->SetMotionType(RE::hkpMotion::MotionType::kKeyframed, true);
					hold.keyframed = true;
					++counts.keyframes;
					// Out of collisions while it is drawn, as the grab takes its object out of the holder's way: keyframed, it
					// has no mass to give, and would shove our player and the clutter it passes.
					if (const auto node = BodiesOf(ref).node) {
						if (!hold.layer) {
							hold.layer = node->GetCollisionLayer();
						}
						node->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);
					}
				}
				const bool refDue = !hold.refWritten || a_now - *hold.refWritten >= HOLD_REF_EVERY;
				if (Write(ref, *hold.drawn, {}, refDue)) {
					++counts.holdsDrawn;
					if (refDue) {
						hold.refWritten = a_now;
					}
				}
			}
		}

		// A rest heard (a friend's, or the session's word): put it, or keep it waiting.
		bool Put(const Protocol::ObjectRest& a_rest, std::uint32_t a_sender, RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			const auto id = a_rest.key.id;
			const Vec  restAt{ a_rest.x, a_rest.y, a_rest.z };
			// Carried through a door in the sender's game: it rests in another cell than our copy lies in. When that is
			// our player's, our copy is brought into it (then put once its 3D is there); else it waits until we are there.
			// Outside, the same: a rest near our player of an object whose cell is not loaded here (carried far across
			// exterior cells).
			const auto farOutside = [&](RE::TESObjectREFR* a_form) {
				const auto cell = a_form->GetParentCell();
				return SameSpace(a_form, a_rest.cell, a_rest.worldspace) && a_rest.worldspace != 0 && cell && !cell->IsAttached() &&
				       ObjectRules::Distance(restAt, At(a_player)) <= ObjectRules::CARRY_OVER_NEAR;
			};
			if (const auto form = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				form && MayCarry(form) && SpaceOf(form) != std::pair<std::uint32_t, std::uint32_t>{} && SameSpace(a_player, a_rest.cell, a_rest.worldspace) &&
				ObjectRules::Distance(restAt, At(a_player)) <= ObjectRules::APPLY_RANGE && (!SameSpace(form, a_rest.cell, a_rest.worldspace) || farOutside(form))) {
				if (auto held = friendHolds.find(id); held != friendHolds.end()) {
					Release(id, held->second);
					friendHolds.erase(held);
				}
				form->MoveTo(a_player);
				// It lands at our player's feet until it is put: our physics shoving it there is not ours to say.
				quiet.Hush(id, a_now + ObjectRules::QUIET);
				++counts.carriedOver;
				logs::info("Objects: {:08X} rests in another cell in its mover's game: brought into ours", id);
				return false;
			}
			const auto ref = LoadedRef(id);
			if (!ref || !SameSpace(ref, a_rest.cell, a_rest.worldspace) || ObjectRules::Distance(At(ref), At(a_player)) > ObjectRules::APPLY_RANGE) {
				return false;
			}
			if (auto held = friendHolds.find(id); held != friendHolds.end()) {
				Release(id, held->second);
				friendHolds.erase(held);
			}
			const auto body = LooseBody(ref);
			const auto here = body ? PoseOf(body) : std::nullopt;
			const auto pose = ObjectRules::PoseOf(a_rest);
			const auto parts = a_rest.parts.empty() ? nullptr : &a_rest.parts;
			if (!here || !ObjectRules::Writable(pose, here->at)) {
				++counts.badPoses;
				return true;  // nothing to put it on here, or too far: not tried again
			}
			watches.erase(id);
			followOns.erase(id);  // a friend's word on it ends ours
			if (a_rest.kind == Protocol::RestKind::kLetGo) {
				if (Write(ref, pose, { a_rest.vx, a_rest.vy, a_rest.vz })) {
					++counts.letGosApplied;
				}
				quiet.Hush(id, a_now + ObjectRules::FRIEND_FLIGHT);
				return true;
			}
			if (ObjectRules::NeedsPut(*here, pose) || (parts && ObjectRules::PartsNeedPut(PartPoses(BodiesOf(ref)), *parts))) {
				if (Write(ref, pose, {}, true, parts)) {
					++(a_sender != 0 ? counts.restsApplied : counts.placesApplied);
				}
			} else {
				++counts.restsLeft;
			}
			seen.Set(id, pose, a_now);
			partsSeen.Forget(id);  // the next look at its parts is their baseline
			quiet.Hush(id, a_now + ObjectRules::QUIET);
			if (a_sender != 0) {
				echo.Theirs(id, a_now);
			}
			return true;
		}

		struct Heard
		{
			Protocol::ObjectRest rest;
			std::uint32_t        sender = 0;
		};
		std::vector<Heard> heard;

		void PutHeard(RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			std::vector<Heard> taken;
			taken.swap(heard);
			for (const auto& one : taken) {
				if (one.rest.kind == Protocol::RestKind::kGone) {
					waiting.Forget(one.rest.key);
					continue;
				}
				if (one.rest.key.kind != Protocol::ObjectKind::kRef) {
					continue;
				}
				if (!Put(one.rest, one.sender, a_player, a_now) && one.rest.kind == Protocol::RestKind::kRest) {
					waiting.Put(one.rest);  // not loaded here yet (a let-go's rest follows it)
				} else {
					waiting.Forget(one.rest.key);
				}
			}
			if (!nextWaiting || a_now >= *nextWaiting) {
				nextWaiting = a_now + WAITING_EVERY;
				waiting.Try([&](const Protocol::ObjectRest& a_rest) { return !friendHolds.contains(a_rest.key.id) && Put(a_rest, 0, a_player, a_now); });
			}
		}

		std::string Hex(std::uint32_t a_id) { return std::format("{:08X}", a_id); }

		std::string Probe(std::string_view a_id)
		{
			const auto id = FormsCommand::ParseHex(a_id);
			if (!id) {
				return "usage: objects probe <form id>";
			}
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(*id);
			if (!ref) {
				return std::format("probe {}: no reference", Hex(*id));
			}
			const auto bodies = BodiesOf(ref);
			const auto pose = PoseOf(bodies.first);
			const auto at = At(ref);
			const auto [cell, worldspace] = SpaceOf(ref);
			std::string out = std::format("probe {} base={} loaded={} disabled={} bodies={} motion={} loose={} cell={} worldspace={} parentCell={} at=({:.1f},{:.1f},{:.1f})",
				Hex(*id), Hex(ref->GetBaseObject() ? ref->GetBaseObject()->GetFormID() : 0), ref->Is3DLoaded() ? 1 : 0, ref->IsDisabled() ? 1 : 0, bodies.count,
				static_cast<int>(MotionOf(bodies.first)), LooseBody(ref) ? 1 : 0, Hex(cell), Hex(worldspace), Hex(ref->GetParentCell() ? ref->GetParentCell()->GetFormID() : 0),
				at.x, at.y, at.z);
			if (const auto node = bodies.node) {
				out += std::format(" layer={}", static_cast<int>(node->GetCollisionLayer()));
			}
			if (bodies.count > 1) {
				// An object of several bodies: each loose part by name, where it lies.
				const auto parts = PartPoses(bodies);
				out += std::format(" parts={}", parts.size());
				for (const auto& [name, part] : parts) {
					out += std::format(" {}@({:.1f},{:.1f},{:.1f})", Hex(name), part.at.x, part.at.y, part.at.z);
				}
			}
			if (pose) {
				out += std::format(" body=({:.1f},{:.1f},{:.1f}) rot=({:.4f},{:.4f},{:.4f},{:.4f})", pose->at.x, pose->at.y, pose->at.z, pose->rot.x, pose->rot.y, pose->rot.z,
					pose->rot.w);
			}
			if (const auto root = ref->Get3D()) {
				// After a put the reference, its 3D and its body agree: refTo3D near 0, anglesTo3D near 0 (once learnt).
				const auto& t = root->world.translate;
				const auto  angle = ref->GetAngle();
				out += std::format(" angle=({:.4f},{:.4f},{:.4f}) 3d=({:.1f},{:.1f},{:.1f}) refTo3D={:.2f}", angle.x, angle.y, angle.z, t.x, t.y, t.z,
					ObjectRules::Distance(at, { t.x, t.y, t.z }));
				if (angleSense.Settled() != ObjectRules::Sense::kUnknown) {
					out += std::format(" anglesTo3D={:.4f}",
						ObjectRules::Differ(ObjectRules::WorldOfAngles({ angle.x, angle.y, angle.z }, angleSense.Settled()), MatFrom(root->world.rotate)));
				}
				if (bodies.node && pose) {
					const auto& n = bodies.node->world.translate;
					out += std::format(" bodyTo3D={:.2f}", ObjectRules::Distance(pose->at, { n.x, n.y, n.z }));
				}
			}
			const auto key = Key{ Protocol::ObjectKind::kRef, *id };
			out += std::format(" watched={} heldByFriend={} ours={} waiting={} quiet={}", watches.contains(*id) ? 1 : 0, friendHolds.contains(*id) ? 1 : 0,
				ourHold && *ourHold == *id ? 1 : 0, waiting.Has(key) ? 1 : 0, quiet.Hushed(*id, Clock::now()) ? 1 : 0);
			return out;
		}

		// Each friend's hold drawn here: our copy's body, where the hold puts it before the stand-in, how far the drawn copy
		// trailed that at most, and the longest wait between two of its holds.
		std::string HeldList()
		{
			std::string out = std::format("held count={}", friendHolds.size());
			const auto  now = Clock::now();
			for (const auto& [id, hold] : friendHolds) {
				const auto ref = LoadedRef(id);
				const auto pose = ref ? PoseOf(BodiesOf(ref).first) : std::nullopt;
				const auto standIn = RemotePlayers::StandInOf(hold.holder);
				out += std::format(" | {} holder={} keyframed={} parted={} heard={} ageMs={} gapMaxMs={} trailMax={:.1f}", Hex(id), hold.holder, hold.keyframed ? 1 : 0,
					hold.parted ? 1 : 0, hold.heardCount, std::chrono::duration_cast<std::chrono::milliseconds>(now - hold.heard).count(),
					std::chrono::duration_cast<std::chrono::milliseconds>(hold.gapMax).count(), hold.trailMax);
				if (pose) {
					out += std::format(" body=({:.1f},{:.1f},{:.1f})", pose->at.x, pose->at.y, pose->at.z);
				}
				if (standIn && pose) {
					const auto target = ObjectRules::FromHolder(hold.hold, At(standIn.get()), standIn->GetAngleZ());
					out += std::format(" target=({:.1f},{:.1f},{:.1f}) toTarget={:.1f} toStandIn={:.1f}", target.at.x, target.at.y, target.at.z,
						ObjectRules::Distance(pose->at, target.at), ObjectRules::Distance(pose->at, At(standIn.get())));
				}
			}
			return out;
		}

		std::string NearList(std::string_view a_radius)
		{
			auto radius = SCAN_RANGE;
			if (!a_radius.empty()) {
				const auto n = DevLine::Number(a_radius);
				if (!n || *n == 0 || *n > 20000) {
					return "usage: objects near [radius]";
				}
				radius = static_cast<float>(*n);
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return "near: no player";
			}
			std::vector<std::pair<float, RE::FormID>> found;
			const auto                                me = At(player);
			tes->ForEachReferenceInRange(player, radius, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && a_ref != player && LooseBody(a_ref)) {
					found.emplace_back(ObjectRules::Distance(At(a_ref), me), a_ref->GetFormID());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			std::sort(found.begin(), found.end());
			std::string out = std::format("near count={}", found.size());
			for (std::size_t i = 0; i < found.size() && i < 20; ++i) {
				const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(found[i].second);
				out += std::format(" | {} base={} dist={:.0f}", Hex(found[i].second), Hex(ref && ref->GetBaseObject() ? ref->GetBaseObject()->GetFormID() : 0), found[i].first);
			}
			return out;
		}
	}

	bool Install()
	{
		REL::Relocation<std::uintptr_t> plain{ RE::VTABLE_Explosion[0] };
		REL::Relocation<std::uintptr_t> chain{ RE::VTABLE_ChainExplosion[0] };
		explosionsHooked += VtableHook::Slot<ExplosionUpdateFn>(plain.address(), EXPLOSION_UPDATE_SLOT, OnExplosionUpdate<0>, explosionUpdates[0], "Objects", "explosion update") ? 1 : 0;
		explosionsHooked += VtableHook::Slot<ExplosionUpdateFn>(chain.address(), EXPLOSION_UPDATE_SLOT, OnExplosionUpdate<1>, explosionUpdates[1], "Objects", "chain explosion update") ? 1 : 0;
		explosionHooked = explosionsHooked == 2;
		if (!explosionHooked) {
			logs::warn("Objects: the explosion update is not hooked: what a blast throws about is nobody's to report here");
		}
		if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(&hitSink);
			events->AddEventSink<RE::TESContainerChangedEvent>(&containerSink);
		}
		Shots::WatchFlights(&Flew);  // every projectile's path, from Shots' hook of their updates
		return explosionHooked;
	}

	void Started(std::uint32_t a_ourId)
	{
		ourId = a_ourId;
		Clear();
		active.store(true);
	}

	void Frame(const SendFn& a_send)
	{
		if (!active.load()) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			return;
		}
		const auto now = Clock::now();
		const auto dt = lastFrame ? std::chrono::duration<float>(now - *lastFrame).count() : 0.0f;
		lastFrame = now;
		if (askPlaces) {
			askPlaces = false;
			a_send(Protocol::EncodeAskObjectRests(), true);
		}
		TakeRecorded(now);
		SayHold(a_send, player, now);
		SayLetGo(player, now);
		DrawHolds(now, (std::min)(dt, 0.25f));
		PutHeard(player, now);
		if (!nextScan || now >= *nextScan) {
			nextScan = now + SCAN_EVERY;
			Scan(player, now);
			Follow(now);
		}
		const auto due = pacer.Due(now);
		if (!due.empty()) {
			Protocol::ObjectRests msg;
			msg.rests = due;
			counts.restsSent += due.size();
			a_send(Protocol::Encode(msg), true);
		}
	}

	void Holds(const Protocol::ObjectHolds& a_msg)
	{
		if (!active.load() || a_msg.sender == 0) {
			return;
		}
		const auto now = Clock::now();
		for (const auto& hold : a_msg.holds) {
			if (hold.key.kind != Protocol::ObjectKind::kRef) {
				continue;
			}
			++counts.holdsHeard;
			const auto id = hold.key.id;
			if (a_msg.sender == ourId) {
				continue;
			}
			// Not one that may be moved here (a plugin only one game has makes it a door, a static, a quest's item or an
			// actor): never drawn.
			if (const auto form = RE::TESForm::LookupByID<RE::TESObjectREFR>(id); form && (!MayCarry(form) || (form->Is3DLoaded() && BodiesOf(form).count > 1))) {
				++counts.notMovable;
				continue;
			}
			// Late: its holder let go of it or it came to rest a moment ago (the holds' channel is not the rests').
			if (!friendHolds.contains(id) && holdEnds.Late(id, a_msg.sender, now)) {
				++counts.lateHolds;
				continue;
			}
			// The server's answer to our hold (a friend held it first): our player lets go, and draws theirs.
			if (ourHold && *ourHold == id) {
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					DropOurGrab(player, id);
				}
				refusedGrab = id;
				ourHold.reset();
				ourHoldPose.reset();
				++counts.holdsRefused;
				++counts.grabsDropped;
				logs::info("Objects: player {} holds {:08X}; our player's grab of it was let go", a_msg.sender, id);
			}
			auto& friendHold = friendHolds[id];
			if (friendHold.holder != a_msg.sender) {
				friendHold.drawn.reset();
			}
			if (friendHold.heardCount > 0) {
				friendHold.gapMax = (std::max)(friendHold.gapMax, now - friendHold.heard);
			}
			++friendHold.heardCount;
			friendHold.holder = a_msg.sender;
			friendHold.hold = hold;
			friendHold.heard = now;
			watches.erase(id);
		}
	}

	void Rests(const Protocol::ObjectRests& a_msg)
	{
		if (!active.load() || (a_msg.sender != 0 && a_msg.sender == ourId)) {
			return;
		}
		if (a_msg.full) {
			waiting.Clear();
			++counts.catchUps;
		}
		const auto now = Clock::now();
		for (const auto& rest : a_msg.rests) {
			if (a_msg.sender != 0 && rest.key.kind == Protocol::ObjectKind::kRef) {
				holdEnds.Ended(rest.key.id, a_msg.sender, now);
			}
			if (rest.kind == Protocol::RestKind::kLetGo) {
				++counts.letGosHeard;
			} else if (rest.kind == Protocol::RestKind::kGone) {
				++counts.goneHeard;
			} else if (a_msg.sender != 0) {
				++counts.restsHeard;
			} else if (!a_msg.full) {
				++counts.answers;
			}
			heard.push_back({ rest, a_msg.sender });
		}
	}

	void Left(std::uint32_t a_playerId)
	{
		for (auto it = friendHolds.begin(); it != friendHolds.end();) {
			if (it->second.holder == a_playerId) {
				Release(it->first, it->second);
				it = friendHolds.erase(it);
			} else {
				++it;
			}
		}
	}

	void Clear()
	{
		active.store(false);
		ReleaseAll();
		causes.Clear();
		seen.Clear();
		partsSeen.Clear();
		pacer.Clear();
		holdPace.Reset();
		quiet.Clear();
		echo.Clear();
		waiting.Clear();
		falls.Clear();
		watches.clear();
		followOns.clear();
		heard.clear();
		shotsSeen.clear();
		ourHold.reset();
		ourHoldPose.reset();
		ourHoldTelekinesis = false;
		severalGrab.reset();
		holdEnds.Clear();
		letGo.reset();
		refusedGrab.reset();
		nextScan.reset();
		nextWaiting.reset();
		lastFrame.reset();
		askPlaces = false;
		std::scoped_lock l{ recordLock };
		blasts.clear();
		hits.clear();
		pickups.clear();
		flights.clear();
		shotPaths.clear();
	}

	void ForgetWorld()
	{
		// The world they were in is going: nothing is written to them any more (the load's own physics starts afresh).
		friendHolds.clear();
		seen.Clear();
		partsSeen.Clear();
		watches.clear();
		followOns.clear();
		shotsSeen.clear();
		ourHold.reset();
		ourHoldPose.reset();
		ourHoldTelekinesis = false;
		severalGrab.reset();
		holdEnds.Clear();
		letGo.reset();
		refusedGrab.reset();
		quiet.Clear();
		falls.Clear();
		// The session's places are its word on the new world too: asked for again once the game runs.
		askPlaces = active.load();
		std::scoped_lock l{ recordLock };
		blasts.clear();
		hits.clear();
		pickups.clear();
		flights.clear();
		shotPaths.clear();
	}

	void BeforeSave()
	{
		ReleaseAll();
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "probe") {
			return words.size() == 2 ? Probe(words[1]) : "usage: objects probe <form id>";
		}
		if (!words.empty() && words[0] == "held") {
			return HeldList();
		}
		if (!words.empty() && words[0] == "near") {
			return NearList(words.size() >= 2 ? words[1] : std::string_view{});
		}
		if (!words.empty() && words[0] == "grab") {
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto grab = player ? GrabOf(player) : OurGrab{};
			const auto grabbed = grab.ref;
			if (!grabbed) {
				return "grab none";
			}
			const auto body = LooseBody(grabbed.get());
			const auto pose = body ? PoseOf(body) : std::nullopt;
			const auto me = At(player);
			return std::format("grab {} telekinesis={} loose={} said={} player=({:.1f},{:.1f},{:.1f}){}", Hex(grabbed->GetFormID()), grab.telekinesis ? 1 : 0, body ? 1 : 0,
				ourHold && *ourHold == grabbed->GetFormID() ? 1 : 0, me.x, me.y, me.z,
				pose ? std::format(" body=({:.1f},{:.1f},{:.1f}) toPlayer={:.1f}", pose->at.x, pose->at.y, pose->at.z, ObjectRules::Distance(pose->at, me)) : "");
		}
		if (!words.empty()) {
			return "usage: objects [probe <form id> | near [radius] | grab | held]";
		}
		const auto& c = counts;
		return std::format(
			"objects active={} hooked={} holds sent={} heard={} drawn={} refused={} grabsDropped={} ended={} keyframes={} releases={} telekinesis={} late={} parted={} carried={} carriedOver={} leftBehind={} notMovable={} | parts rests={} written={} unlike={} grabs={} moves={} anchored={} | letGos sent={} heard={} applied={} | "
			"rests said={} sent={} heard={} applied={} left={} answers={} placesApplied={} catchUps={} waiting={} waitingRefused={} | gone sent={} heard={} | "
			"scans={} started={} notOurs={} hushed={} followOns={} lost={} gaveUp={} watched={} seen={} causes={} paced={} pacedDropped={} echoStopped={} | "
			"blasts={} hits={} pickups={} | flights watched={} heard={} dropped={} stretches={} stretchesDropped={} cones={} | bodyFalls={} ourPaths={} theirPaths={} causesJoined={} causesDropped={} | writes={} refWrites={} refUnwritten={} bodyOnly={} writeFails={} badPoses={} | senses body={} ({}/{}) angles={} ({}/{})",
			active.load() ? 1 : 0, explosionHooked ? 1 : 0, c.holdsSent, c.holdsHeard, c.holdsDrawn, c.holdsRefused, c.grabsDropped, c.holdsEnded, c.keyframes, c.releases,
			c.telekinesisHolds, c.lateHolds, c.parted, c.carried, c.carriedOver, c.leftBehind, c.notMovable, c.partRests, c.partsWritten, c.partsUnlike, c.severalGrabs, c.partMoves, c.partsOnly,
			c.letGosSent, c.letGosHeard, c.letGosApplied, c.restsSaid, c.restsSent, c.restsHeard, c.restsApplied, c.restsLeft, c.answers, c.placesApplied, c.catchUps,
			waiting.Size(), waiting.refused, c.goneSent, c.goneHeard, c.scans, c.started, c.notOurs, c.hushed, c.followOns, c.lost, c.gaveUp, watches.size(), seen.Size(),
			causes.Size(), pacer.Waiting(), pacer.dropped, echo.stopped, blastsHeard.load(), hitsHeard.load(), pickupsHeard.load(), Shots::AllHooked() ? 1 : 0,
			flightsHeard.load(), flightsDropped.load(), stretchesHeard.load(), stretchesDropped.load(), conesHeard.load(), c.bodyFalls, c.ourPaths, c.theirPaths, causes.joined, causes.dropped, c.writes, c.refWrites, c.refUnwritten, c.bodyOnly,
			c.writeFails, c.badPoses, SenseName(bodySense.Settled()), bodySense.same, bodySense.transposed, SenseName(angleSense.Settled()), angleSense.same,
			angleSense.transposed);
	}
}
