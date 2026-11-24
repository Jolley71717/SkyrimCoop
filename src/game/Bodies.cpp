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
		// A skeleton is walked whole: the cap is only against a 3D gone wrong, far past any real skeleton (an extended
		// skeleton has several hundred nodes). A walk cut short would leave bones out, and a body is laid whole or not.
		constexpr std::size_t MAX_NODES = 8192;
		constexpr int         MAX_DEPTH = 96;

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

		std::map<Key, Clock::time_point>  dying;      // deaths this game said, their fall not read yet: since when
		std::map<Key, Watch>              watches;    // our NPCs' bodies settling
		std::map<Key, Clock::time_point>  deadSince;  // copies with a rest waiting: when first seen dead here
		BodyRules::Waiting                waiting;
		std::vector<Protocol::Body>       outbox;
		std::optional<Clock::time_point>  nextWatch;
		std::optional<Clock::time_point>  nextLay;

		struct Counts
		{
			std::uint64_t deaths = 0, fallsSent = 0, fallsEarly = 0, restsSent = 0, gaveUp = 0, notWhole = 0, lost = 0, unwatched = 0, handedOver = 0;
			std::uint64_t fallsHeard = 0, restsHeard = 0, catchUps = 0, fallsPushed = 0, laid = 0, left = 0, ours = 0, tooFar = 0, partial = 0, moved = 0;
			std::uint64_t restored = 0, badPoses = 0;
		};
		Counts counts;

		// ---- the engine ----

		void FindBones(RE::NiAVObject* a_node, std::vector<BoneBody>& a_out, std::size_t& a_seen, int a_depth)
		{
			if (!a_node || a_depth > MAX_DEPTH || ++a_seen > MAX_NODES) {
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

		// Whether an actor's ragdoll has started: the engine says so, or its pelvis is a body physics moves.
		bool Ragdolling(RE::Actor* a_actor, const BoneBody* a_pelvis)
		{
			if (a_actor->IsInRagdollState()) {
				return true;
			}
			const auto hk = a_pelvis && a_pelvis->body ? a_pelvis->body->GetRigidBody() : nullptr;
			return hk && ObjectRules::Loose(static_cast<ObjectRules::Motion>(hk->motion.type.underlying()));
		}

		// Every death this game said (NpcSync: one it ran as it died, and one that died here while nobody ran it, said once
		// its claim was granted): its fall once its ragdoll starts (the blow's velocity, not the death animation's), or
		// FALL_READ_WAIT after the death; its rest once it settles.
		void WatchOurs(Clock::time_point a_now)
		{
			for (const auto& key : NpcSync::TakeDeathsSaid()) {
				++counts.deaths;
				dying.try_emplace(key, a_now);
			}
			for (auto it = dying.begin(); it != dying.end();) {
				const auto key = it->first;
				const auto actor = NpcSync::ActorOf(key);
				if (!actor || !actor->Is3DLoaded() || !actor->IsDead()) {
					++counts.lost;  // unloaded or raised before it fell
					it = dying.erase(it);
					continue;
				}
				const auto bones = BonesOf(actor.get());
				const auto pelvis = Pelvis(actor.get(), bones);
				const bool started = Ragdolling(actor.get(), pelvis);
				if (!started && a_now - it->second < BodyRules::FALL_READ_WAIT) {
					++it;
					continue;
				}
				if (!started) {
					++counts.fallsEarly;  // no ragdoll yet: what its pelvis has now
				}
				const auto [cell, worldspace] = SpaceOf(actor.get());
				const auto fall = BodyRules::FallOf(key, cell, worldspace, At(actor.get()), pelvis ? VelocityOf(pelvis->body) : Vec{});
				if (Protocol::BodyHolds(fall)) {
					outbox.push_back(fall);
					++counts.fallsSent;
				}
				if (watches.size() < MAX_WATCHED) {
					watches[key] = Watch{ BodyRules::Settle{ JustBones(bones), a_now } };
				} else {
					++counts.unwatched;
				}
				it = dying.erase(it);
			}
		}

		// Our bodies settling: said once at rest, even if the NPC changed hands meanwhile (the server takes the rest from
		// the game that said its fall).
		void SettleOurs(Clock::time_point a_now)
		{
			for (auto it = watches.begin(); it != watches.end();) {
				const auto& key = it->first;
				const auto  actor = NpcSync::ActorOf(key);
				if (!actor || !actor->Is3DLoaded() || !actor->IsDead()) {
					++counts.lost;  // unloaded or raised: nothing to say
					it = watches.erase(it);
					continue;
				}
				if (!it->second.handedOver && NpcSync::RunnerOf(key) != ourId) {
					it->second.handedOver = true;
					++counts.handedOver;  // still ours to say: we said its fall
				}
				const auto bones = BonesOf(actor.get());
				if (!it->second.settle.Look(JustBones(bones), a_now)) {
					++it;
					continue;
				}
				if (it->second.settle.GaveUp()) {
					++counts.gaveUp;
				}
				const auto [cell, worldspace] = SpaceOf(actor.get());
				if (const auto rest = BodyRules::RestOf(key, cell, worldspace, At(actor.get()), JustBones(bones))) {
					outbox.push_back(*rest);
					++counts.restsSent;
				} else {
					++counts.notWhole;  // more than MAX_BONES, a bone named twice or gone wrong: never sent in part
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
			const auto here = JustBones(bones);
			if (!BodyRules::NeedsLaying(here, a_rest.bones)) {
				++counts.left;
				return true;
			}
			// Whole or not at all: every ragdoll body here is named in the rest once.
			if (!BodyRules::LaysWhole(here, a_rest.bones)) {
				++counts.partial;
				logs::info("Bodies: {:08X}'s rest names {} bones, this game's ragdoll has {} not all named in it: left as it lies", a_rest.key.id, a_rest.bones.size(),
					bones.size());
				return true;
			}
			std::vector<const Protocol::Bone*> targets;
			targets.reserve(bones.size());
			for (const auto& one : bones) {
				const auto it = std::find_if(a_rest.bones.begin(), a_rest.bones.end(), [&](const Protocol::Bone& a_bone) { return a_bone.name == one.bone.name; });
				if (ObjectRules::Distance(BodyRules::At(*it), BodyRules::At(one.bone)) > ObjectRules::WRITE_JUMP) {
					++counts.tooFar;
					return true;  // a report never pulls a body across the map
				}
				targets.push_back(&*it);
			}
			// The reference first, to where the runner's lies: loot, activation, a cell's reload and the save follow it, not
			// its bones.
			if (ObjectRules::Distance(At(actor.get()), there) > BodyRules::REF_SLACK) {
				actor->SetPosition(RE::NiPoint3{ there.x, there.y, there.z });
				++counts.moved;
			}
			const auto write = [&](const std::vector<BoneBody>& a_bones, const std::vector<const Protocol::Bone*>& a_to) {
				Locked(actor.get(), [&] {
					for (std::size_t i = 0; i < a_bones.size(); ++i) {
						auto             at = ToHavok(BodyRules::At(*a_to[i]));
						RE::hkQuaternion rot;
						rot.vec = RE::hkVector4{ a_to[i]->rot.x, a_to[i]->rot.y, a_to[i]->rot.z, a_to[i]->rot.w };
						a_bones[i].body->SetPositionAndRotation(at, rot);
						a_bones[i].body->SetLinearVelocity(RE::hkVector4{ 0.0f });
						a_bones[i].body->SetAngularVelocity(RE::hkVector4{ 0.0f });
					}
				});
			};
			write(bones, targets);
			// Read back: a bone that came out not finite (or is gone) puts every bone back where it lay.
			if (BonesOf(actor.get()).size() != bones.size()) {
				std::vector<const Protocol::Bone*> before;
				before.reserve(bones.size());
				for (const auto& one : bones) {
					before.push_back(&one.bone);
				}
				write(bones, before);
				++counts.restored;
				logs::warn("Bodies: {:08X} laid where its runner's lies read back wrong: every bone put back where it lay", a_rest.key.id);
				return true;
			}
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
		dying.clear();
		watches.clear();
		deadSince.clear();
		waiting.Clear();
		outbox.clear();
		nextWatch.reset();
		nextLay.reset();
	}

	void ForgetWorld()
	{
		dying.clear();
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
			"bodies active={} | ours deaths={} fallsSent={} fallsEarly={} restsSent={} gaveUp={} notWhole={} lost={} unwatched={} handedOver={} dying={} watched={} | "
			"theirs fallsHeard={} restsHeard={} catchUps={} fallsPushed={} fallsLate={} laid={} moved={} left={} partial={} restored={} ours={} far={} waitingRests={} "
			"waitingFalls={} waitingRefused={} | badPoses={}",
			active ? 1 : 0, c.deaths, c.fallsSent, c.fallsEarly, c.restsSent, c.gaveUp, c.notWhole, c.lost, c.unwatched, c.handedOver, dying.size(), watches.size(),
			c.fallsHeard, c.restsHeard, c.catchUps, c.fallsPushed, waiting.late, c.laid, c.moved, c.left, c.partial, c.restored, c.ours, c.tooFar, waiting.Rests(),
			waiting.Falls(), waiting.refused, c.badPoses);
	}
}
