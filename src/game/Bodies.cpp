#include "game/Bodies.h"

#include "DevLine.h"
#include "game/BodyRules.h"
#include "game/NpcSync.h"
#include "net/FormsCommand.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <xmmintrin.h>

namespace Bodies
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		using BodyRules::Vec;
		using Key = Protocol::NpcKey;

		// The runners' bodies are looked at this often while they settle, and the waiting rests tried this often.
		constexpr auto        WATCH_EVERY = 250ms;
		constexpr auto        LAY_EVERY = 500ms;
		constexpr std::size_t MAX_WATCHED = 64;
		constexpr std::size_t MAX_NODES = 256;  // a skeleton walked no further than this

		std::uint32_t ourId = 0;
		bool          active = false;

		// A ragdoll bone: its name and pose, and its body here.
		struct BoneBody
		{
			Protocol::Bone    bone;
			RE::bhkRigidBody* body = nullptr;
		};

		struct Watch
		{
			BodyRules::Settle settle;
		};

		std::set<Key>                     aliveRun;   // NPCs this game runs, seen alive
		std::map<Key, Watch>              watches;    // our NPCs' bodies settling
		std::map<Key, Clock::time_point>  deadSince;  // copies with a rest waiting: when first seen dead here
		BodyRules::Waiting                waiting;
		std::vector<Protocol::Body>       outbox;
		std::optional<Clock::time_point>  nextWatch;
		std::optional<Clock::time_point>  nextLay;

		struct Counts
		{
			std::uint64_t deaths = 0, fallsSent = 0, restsSent = 0, gaveUp = 0, noBones = 0, lost = 0;
			std::uint64_t fallsHeard = 0, restsHeard = 0, catchUps = 0, fallsPushed = 0, laid = 0, left = 0, ours = 0, tooFar = 0, badPoses = 0;
		};
		Counts counts;

		// ---- the engine ----

		void FindBones(RE::NiAVObject* a_node, std::vector<BoneBody>& a_out, std::size_t& a_seen, int a_depth)
		{
			if (!a_node || a_depth > 48 || ++a_seen > MAX_NODES) {
				return;
			}
			if (const auto collision = a_node->collisionObject.get()) {
				if (const auto niCollision = netimmerse_cast<RE::bhkNiCollisionObject*>(collision)) {
					if (const auto body = niCollision->body ? netimmerse_cast<RE::bhkRigidBody*>(niCollision->body.get()) : nullptr) {
						BoneBody one;
						one.bone.name = BodyRules::NameHash(std::string_view{ a_node->name.c_str() ? a_node->name.c_str() : "" });
						one.body = body;
						a_out.push_back(one);
					}
				}
			}
			if (const auto node = a_node->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					FindBones(child.get(), a_out, a_seen, a_depth + 1);
				}
			}
		}

		Vec FromHavok(const RE::hkVector4& a_v, float a_scale)
		{
			alignas(16) float f[4];
			_mm_store_ps(f, a_v.quad);
			return { f[0] * a_scale, f[1] * a_scale, f[2] * a_scale };
		}

		// The ragdoll's bones of an actor, with their poses in game units (a bone whose pose is not finite is left out).
		std::vector<BoneBody> BonesOf(RE::Actor* a_actor)
		{
			std::vector<BoneBody> found;
			std::size_t           seen = 0;
			FindBones(a_actor ? a_actor->Get3D() : nullptr, found, seen, 0);
			const auto inverse = RE::bhkWorld::GetWorldScaleInverse();
			std::vector<BoneBody> out;
			out.reserve(found.size());
			for (auto& one : found) {
				RE::hkVector4    at;
				RE::hkQuaternion rot;
				one.body->GetPosition(at);
				one.body->GetRotation(rot);
				alignas(16) float q[4];
				_mm_store_ps(q, rot.vec.quad);
				const auto p = FromHavok(at, inverse);
				one.bone.x = p.x;
				one.bone.y = p.y;
				one.bone.z = p.z;
				one.bone.rot = ObjectRules::Normalized({ q[0], q[1], q[2], q[3] });
				if (!ObjectRules::Finite(p) || !Protocol::QuatHolds(one.bone.rot)) {
					++counts.badPoses;
					continue;
				}
				out.push_back(one);
			}
			return out;
		}

		std::vector<Protocol::Bone> JustBones(const std::vector<BoneBody>& a_bones)
		{
			std::vector<Protocol::Bone> out;
			out.reserve(a_bones.size());
			for (const auto& one : a_bones) {
				out.push_back(one.bone);
			}
			return out;
		}

		// The pelvis: the bone whose node name holds "pelvis", else the first (a creature's root).
		const BoneBody* Pelvis(RE::Actor* a_actor, const std::vector<BoneBody>& a_bones)
		{
			if (a_bones.empty()) {
				return nullptr;
			}
			(void)a_actor;
			for (const auto& one : a_bones) {
				if (one.bone.name == BodyRules::NameHash("NPC Pelvis [Pelv]") || one.bone.name == BodyRules::NameHash("Pelvis")) {
					return &one;
				}
			}
			return &a_bones.front();
		}

		Vec VelocityOf(RE::bhkRigidBody* a_body)
		{
			const auto hk = a_body ? a_body->GetRigidBody() : nullptr;
			return hk ? FromHavok(hk->motion.linearVelocity, RE::bhkWorld::GetWorldScaleInverse()) : Vec{};
		}

		std::pair<std::uint32_t, std::uint32_t> SpaceOf(const RE::TESObjectREFR* a_ref)
		{
			const auto cell = a_ref->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				return { cell->GetFormID(), 0 };
			}
			const auto world = a_ref->GetWorldspace();
			return { 0, world ? world->GetFormID() : 0 };
		}

		Vec At(const RE::TESObjectREFR* a_ref)
		{
			const auto p = a_ref->GetPosition();
			return { p.x, p.y, p.z };
		}

		// Writes each bone's pose (and a velocity) under its world's lock.
		template <class Fn>
		void Locked(RE::Actor* a_actor, Fn&& a_write)
		{
			const auto cell = a_actor->GetParentCell();
			const auto world = cell ? cell->GetbhkWorld() : nullptr;
			if (world) {
				RE::BSWriteLockGuard guard{ world->worldLock };
				a_write();
			} else {
				a_write();
			}
		}

		RE::hkVector4 ToHavok(const Vec& a_v)
		{
			const auto scale = RE::bhkWorld::GetWorldScale();
			return RE::hkVector4{ a_v.x * scale, a_v.y * scale, a_v.z * scale, 0.0f };
		}

		// ---- ours ----

		void WatchOurs(Clock::time_point a_now)
		{
			for (const auto& [key, actor] : NpcSync::RunActors()) {
				if (!actor || !actor->Is3DLoaded()) {
					continue;
				}
				if (!actor->IsDead()) {
					aliveRun.insert(key);
					continue;
				}
				if (!aliveRun.erase(key)) {
					continue;  // dead already when we began to run it: the session has its body from whoever saw it fall
				}
				// It died now: its fall at once, its rest once its ragdoll settles.
				++counts.deaths;
				const auto bones = BonesOf(actor.get());
				const auto pelvis = Pelvis(actor.get(), bones);
				const auto [cell, worldspace] = SpaceOf(actor.get());
				const auto fall = BodyRules::FallOf(key, cell, worldspace, At(actor.get()), pelvis ? VelocityOf(pelvis->body) : Vec{});
				if (Protocol::BodyHolds(fall)) {
					outbox.push_back(fall);
					++counts.fallsSent;
				}
				if (watches.size() < MAX_WATCHED) {
					watches[key] = Watch{ BodyRules::Settle{ JustBones(bones), a_now } };
				}
			}
			if (aliveRun.size() > 4096) {
				aliveRun.clear();  // stale keys of NPCs long gone: the next frame sees the living again
			}
		}

		void SettleOurs(Clock::time_point a_now)
		{
			for (auto it = watches.begin(); it != watches.end();) {
				const auto& key = it->first;
				const auto  actor = NpcSync::RunnerOf(key) == ourId ? NpcSync::ActorOf(key) : RE::NiPointer<RE::Actor>{};
				if (!actor || !actor->Is3DLoaded() || !actor->IsDead()) {
					++counts.lost;  // handed over, unloaded or raised: not ours to say
					it = watches.erase(it);
					continue;
				}
				const auto bones = BonesOf(actor.get());
				if (!it->second.settle.Look(JustBones(bones), a_now)) {
					++it;
					continue;
				}
				if (it->second.settle.GaveUp()) {
					++counts.gaveUp;
				}
				const auto pelvis = Pelvis(actor.get(), bones);
				const auto [cell, worldspace] = SpaceOf(actor.get());
				if (const auto rest = BodyRules::RestOf(key, cell, worldspace, At(actor.get()), JustBones(bones), pelvis ? pelvis->bone.name : 0)) {
					outbox.push_back(*rest);
					++counts.restsSent;
				} else {
					++counts.noBones;
				}
				it = watches.erase(it);
			}
		}

		// ---- theirs ----

		// A copy that died here: every bone given its runner's fall.
		bool Push(const Protocol::Body& a_fall)
		{
			if (NpcSync::RunnerOf(a_fall.key) == ourId) {
				return true;
			}
			const auto actor = NpcSync::ActorOf(a_fall.key);
			if (!actor || !actor->Is3DLoaded() || !actor->IsDead()) {
				return false;
			}
			const auto bones = BonesOf(actor.get());
			const auto pelvis = Pelvis(actor.get(), bones);
			const auto hk = pelvis && pelvis->body ? pelvis->body->GetRigidBody() : nullptr;
			if (!hk || !ObjectRules::Loose(static_cast<ObjectRules::Motion>(hk->motion.type.underlying()))) {
				return false;  // not in its ragdoll yet
			}
			const auto velocity = ToHavok(BodyRules::Capped({ a_fall.vx, a_fall.vy, a_fall.vz }));
			Locked(actor.get(), [&] {
				for (const auto& one : bones) {
					one.body->SetLinearVelocity(velocity);
				}
			});
			++counts.fallsPushed;
			return true;
		}

		// A copy dead here a moment: laid where its runner's lies, when it is far off.
		bool Lay(const Protocol::Body& a_rest, RE::PlayerCharacter* a_player, Clock::time_point a_now)
		{
			if (NpcSync::RunnerOf(a_rest.key) == ourId) {
				++counts.ours;
				return true;  // ours now: our own body is the truth
			}
			const auto actor = NpcSync::ActorOf(a_rest.key);
			if (!actor || !actor->Is3DLoaded() || !actor->IsDead()) {
				deadSince.erase(a_rest.key);
				return false;
			}
			const auto [cell, worldspace] = SpaceOf(actor.get());
			const bool sameSpace = (cell != 0 && cell == a_rest.cell) || (worldspace != 0 && worldspace == a_rest.worldspace);
			const Vec  there{ a_rest.x, a_rest.y, a_rest.z };
			if (!sameSpace || ObjectRules::Distance(At(actor.get()), there) > BodyRules::APPLY_RANGE ||
				ObjectRules::Distance(At(a_player), there) > ObjectRules::APPLY_RANGE) {
				return false;
			}
			const auto since = deadSince.try_emplace(a_rest.key, a_now).first->second;
			if (a_now - since < BodyRules::DEAD_FOR) {
				return false;  // its own fall here is not over
			}
			const auto bones = BonesOf(actor.get());
			if (bones.empty()) {
				return false;
			}
			deadSince.erase(a_rest.key);
			if (!BodyRules::NeedsLaying(JustBones(bones), a_rest.bones)) {
				++counts.left;
				return true;
			}
			for (const auto& one : bones) {
				const auto it = std::find_if(a_rest.bones.begin(), a_rest.bones.end(), [&](const Protocol::Bone& a_bone) { return a_bone.name == one.bone.name; });
				if (it != a_rest.bones.end() && ObjectRules::Distance(BodyRules::At(*it), BodyRules::At(one.bone)) > ObjectRules::WRITE_JUMP) {
					++counts.tooFar;
					return true;  // a report never pulls a body across the map
				}
			}
			Locked(actor.get(), [&] {
				for (const auto& one : bones) {
					const auto it = std::find_if(a_rest.bones.begin(), a_rest.bones.end(), [&](const Protocol::Bone& a_bone) { return a_bone.name == one.bone.name; });
					if (it == a_rest.bones.end()) {
						continue;
					}
					auto             at = ToHavok(BodyRules::At(*it));
					RE::hkQuaternion rot;
					rot.vec = RE::hkVector4{ it->rot.x, it->rot.y, it->rot.z, it->rot.w };
					one.body->SetPositionAndRotation(at, rot);
					one.body->SetLinearVelocity(RE::hkVector4{ 0.0f });
					one.body->SetAngularVelocity(RE::hkVector4{ 0.0f });
				}
			});
			actor->AddChange(static_cast<std::uint32_t>(RE::TESObjectREFR::ChangeFlags::kHavokMoved));
			++counts.laid;
			return true;
		}

		std::string Hex(std::uint32_t a_id) { return std::format("{:08X}", a_id); }

		std::string Probe(std::string_view a_id)
		{
			const auto id = FormsCommand::ParseHex(a_id);
			if (!id) {
				return "usage: bodies probe <form id>";
			}
			const auto actor = RE::TESForm::LookupByID<RE::Actor>(*id);
			if (!actor) {
				return std::format("probe {}: no actor", Hex(*id));
			}
			const auto bones = BonesOf(actor);
			const auto pelvis = Pelvis(actor, bones);
			std::string out = std::format("probe {} loaded={} dead={} ragdoll={} bones={}", Hex(*id), actor->Is3DLoaded() ? 1 : 0, actor->IsDead() ? 1 : 0,
				actor->IsInRagdollState() ? 1 : 0, bones.size());
			if (pelvis) {
				const auto v = VelocityOf(pelvis->body);
				const auto hk = pelvis->body->GetRigidBody();
				out += std::format(" pelvis=({:.1f},{:.1f},{:.1f}) v=({:.0f},{:.0f},{:.0f}) motion={}", pelvis->bone.x, pelvis->bone.y, pelvis->bone.z, v.x, v.y, v.z,
					hk ? static_cast<int>(hk->motion.type.underlying()) : -1);
			}
			if (const auto key = NpcSync::KeyOf(actor)) {
				out += std::format(" runner={} watched={} restWaiting={}", NpcSync::RunnerOf(*key), watches.contains(*key) ? 1 : 0, waiting.HasRest(*key) ? 1 : 0);
			}
			return out;
		}
	}

	void Started(std::uint32_t a_ourId)
	{
		Clear();
		ourId = a_ourId;
		active = true;
	}

	void Frame(const SendFn& a_send)
	{
		if (!active) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			return;
		}
		const auto now = Clock::now();
		WatchOurs(now);
		if (!nextWatch || now >= *nextWatch) {
			nextWatch = now + WATCH_EVERY;
			SettleOurs(now);
			waiting.TryFalls(now, [&](const Protocol::Body& a_fall) { return Push(a_fall); });
		}
		if (!nextLay || now >= *nextLay) {
			nextLay = now + LAY_EVERY;
			waiting.TryRests([&](const Protocol::Body& a_rest) { return Lay(a_rest, player, now); });
			if (deadSince.size() > 2 * BodyRules::MAX_WAITING) {
				deadSince.clear();
			}
		}
		for (std::size_t first = 0; first < outbox.size(); first += Protocol::MAX_BODIES) {
			Protocol::Bodies msg;
			const auto       end = (std::min)(outbox.size(), first + Protocol::MAX_BODIES);
			msg.bodies.assign(outbox.begin() + static_cast<std::ptrdiff_t>(first), outbox.begin() + static_cast<std::ptrdiff_t>(end));
			a_send(Protocol::Encode(msg), true);
		}
		outbox.clear();
	}

	void Heard(const Protocol::Bodies& a_msg)
	{
		if (!active || (a_msg.sender != 0 && a_msg.sender == ourId)) {
			return;
		}
		const auto now = Clock::now();
		if (a_msg.full) {
			waiting.Clear();
			++counts.catchUps;
		}
		for (const auto& body : a_msg.bodies) {
			if (body.kind == Protocol::BodyKind::kFall) {
				++counts.fallsHeard;
				waiting.Fall(body, now);
			} else {
				++counts.restsHeard;
				waiting.Rest(body);
			}
		}
	}

	void Clear()
	{
		active = false;
		aliveRun.clear();
		watches.clear();
		deadSince.clear();
		waiting.Clear();
		outbox.clear();
		nextWatch.reset();
		nextLay.reset();
	}

	void ForgetWorld()
	{
		aliveRun.clear();
		watches.clear();
		deadSince.clear();
		outbox.clear();
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "probe") {
			return words.size() == 2 ? Probe(words[1]) : "usage: bodies probe <form id>";
		}
		if (!words.empty()) {
			return "usage: bodies [probe <form id>]";
		}
		const auto& c = counts;
		return std::format(
			"bodies active={} | ours deaths={} fallsSent={} restsSent={} gaveUp={} noBones={} lost={} watched={} | theirs fallsHeard={} restsHeard={} catchUps={} "
			"fallsPushed={} fallsLate={} laid={} left={} ours={} far={} waitingRests={} waitingFalls={} waitingRefused={} | badPoses={}",
			active ? 1 : 0, c.deaths, c.fallsSent, c.restsSent, c.gaveUp, c.noBones, c.lost, watches.size(), c.fallsHeard, c.restsHeard, c.catchUps, c.fallsPushed,
			waiting.late, c.laid, c.left, c.ours, c.tooFar, waiting.Rests(), waiting.Falls(), waiting.refused, c.badPoses);
	}
}
