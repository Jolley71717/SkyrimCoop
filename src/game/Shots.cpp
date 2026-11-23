#include "game/Shots.h"

#include "DevLine.h"
#include "game/NpcSync.h"
#include "game/RemotePlayers.h"
#include "game/ShotRules.h"
#include "game/StateBuffer.h"
#include "game/VtableHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <utility>

namespace Shots
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		using Kind = Protocol::ShotKind;
		using Key = Protocol::NpcKey;

		// Projectile::UpdateImpl, the update of every projectile class (SE and AE; this plugin is not built for VR).
		constexpr std::size_t UPDATE_SLOT = 0xAB;
		// How long a native being ended has its updates skipped, and a launch of ours is known by its id at its first update.
		constexpr auto ENDING_FOR = 2s;
		constexpr auto LAUNCHED_FOR = 5s;
		// The projectiles the hook saw at their first update, for the main thread to say (each said by the next frame).
		constexpr std::size_t MAX_CANDIDATES = 256;

		using UpdateFn = void (*)(RE::Projectile*, float);

		struct ClassHook
		{
			REL::VariantID   vtable;
			std::string_view what;
		};
		// The arrow class is first: its own data holds an arrow's enchantment and poison.
		const std::array<ClassHook, 7> CLASSES{ {
			{ RE::VTABLE_ArrowProjectile[0], "arrow projectile update" },
			{ RE::VTABLE_MissileProjectile[0], "missile projectile update" },
			{ RE::VTABLE_FlameProjectile[0], "flame projectile update" },
			{ RE::VTABLE_BeamProjectile[0], "beam projectile update" },
			{ RE::VTABLE_ConeProjectile[0], "cone projectile update" },
			{ RE::VTABLE_GrenadeProjectile[0], "lobbed projectile update" },
			{ RE::VTABLE_BarrierProjectile[0], "barrier projectile update" },
		} };
		std::array<UpdateFn, CLASSES.size()> originals{};
		std::size_t                          hooked = 0;

		std::atomic<bool>          active{ false };
		std::atomic<std::uint32_t> ourId{ 0 };

		// A projectile of an actor's at its first update: the main thread says it when the actor is our player or an NPC
		// we run.
		struct Candidate
		{
			RE::ObjectRefHandle shooter;
			Protocol::Shot      shot;  // all but its shooter
		};

		// What the hook (any thread) and the main thread share.
		std::mutex                                 lock;
		ShotRules::FirstSight                      sight;
		ShotRules::Pending                         pending;
		std::map<RE::FormID, Clock::time_point>    launchedIds;  // launches of ours, by id, once the launch returned
		struct Ending
		{
			Clock::time_point at;
			bool              killed = false;
		};
		std::map<RE::FormID, Ending> ending;  // natives of stand-ins and moved copies: no update, then ended
		std::atomic<std::size_t>     endingCount{ 0 };
		std::vector<Candidate>       candidates;
		struct HookCounts
		{
			std::uint64_t firsts = 0, oursSeen = 0, nativeEnded = 0, skipped = 0, candidatesDropped = 0, raced = 0;
			ShotRules::KindCounts nativeByKind;
		};
		HookCounts hookCounts;

		// Main thread only.
		ShotRules::Outbox outbox;
		ShotRules::Inbox  inbox;
		ShotRules::Live   live;
		struct AimCheck
		{
			float pitch = 0.0f, heading = 0.0f;
		};
		std::map<RE::FormID, AimCheck> aimChecks;  // launches of ours whose aim is read again at the next frame

		struct Counts
		{
			std::uint64_t notOurs = 0, shooterGone = 0, unfit = 0, saidNpc = 0, saidDual = 0, launchedDual = 0;
			std::uint64_t in = 0, noShooter = 0, tooFar = 0, unknown = 0, full = 0, launchFailed = 0, runByUs = 0, unmirrored = 0;
			std::uint64_t endedSave = 0, reaimed = 0, reaimedLate = 0;
			float         aimErrMax = 0.0f, aimLeftMax = 0.0f;
			ShotRules::KindCounts launched;
		};
		Counts counts;

		struct PerPlayer
		{
			std::uint64_t in = 0, launched = 0, npcIn = 0, npcLaunched = 0;
		};
		std::map<std::uint32_t, PerPlayer> perPlayer;  // by the game that sent them

		std::string lastSaid = "none";
		std::string lastLaunched = "none";

		std::string KeyText(const Key& a_key)
		{
			if (a_key.id == 0) {
				return "none";
			}
			return a_key.IsSpawn() ? std::format("{:08X}@{}", a_key.id, a_key.spawner) : std::format("{:08X}", a_key.id);
		}

		std::string ShotText(const Protocol::Shot& a_shot)
		{
			return std::format("{} {} projectile={:08X} weapon={:08X} ammo={:08X} spell={:08X} source={} dual={} held={} at={:.0f},{:.0f},{:.0f} pitch={:.3f} heading={:.3f} power={:.2f} scale={:.2f}",
				a_shot.player != 0 ? std::format("player={}", a_shot.player) : std::format("npc={}", KeyText(a_shot.npc)), ShotRules::NameOf(a_shot.kind), a_shot.projectile,
				a_shot.weapon, a_shot.ammo, a_shot.spell, a_shot.source, (a_shot.flags & Protocol::kShotDual) != 0 ? 1 : 0, (a_shot.flags & Protocol::kShotHeld) != 0 ? 1 : 0,
				a_shot.x, a_shot.y, a_shot.z, a_shot.pitch, a_shot.heading, a_shot.power, a_shot.scale);
		}

		std::uint32_t IdOf(const RE::TESForm* a_form)
		{
			return a_form ? a_form->GetFormID() : 0;
		}

		float Amount(float a_value)
		{
			return std::isfinite(a_value) ? std::clamp(a_value, 0.0f, 10.0f) : 1.0f;
		}

		float Turn(float a_angle)
		{
			if (!std::isfinite(a_angle)) {
				return 0.0f;
			}
			constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;
			const float     turned = std::fmod(a_angle, TWO_PI);
			return turned < 0.0f ? turned + TWO_PI : turned;
		}

		float Pitch(float a_angle)
		{
			return std::isfinite(a_angle) ? std::clamp(a_angle, -3.2f, 3.2f) : 0.0f;
		}

		// A magic item by id, whatever its kind (a spell, a scroll, an enchantment, a potion or poison, an ingredient).
		RE::MagicItem* MagicOf(std::uint32_t a_id)
		{
			const auto form = a_id != 0 ? RE::TESForm::LookupByID(a_id) : nullptr;
			if (!form) {
				return nullptr;
			}
			switch (form->GetFormType()) {
			case RE::FormType::Spell:
				return form->As<RE::SpellItem>();
			case RE::FormType::Scroll:
				return form->As<RE::ScrollItem>();
			case RE::FormType::Enchantment:
				return form->As<RE::EnchantmentItem>();
			case RE::FormType::AlchemyItem:
				return form->As<RE::AlchemyItem>();
			case RE::FormType::Ingredient:
				return form->As<RE::IngredientItem>();
			default:
				return nullptr;
			}
		}

		// What a projectile at its first update says: all but its shooter.
		Protocol::Shot Snapshot(RE::Projectile* a_projectile, const RE::BGSProjectile* a_base, bool a_arrowClass, Clock::time_point a_now)
		{
			auto&          rd = a_projectile->GetProjectileRuntimeData();
			Protocol::Shot shot;
			shot.sentMs = Protocol::SentMs(a_now);
			shot.projectile = IdOf(a_base);
			shot.weapon = IdOf(rd.weaponSource);
			shot.ammo = IdOf(rd.ammoSource);
			shot.spell = IdOf(rd.spell);
			if (a_arrowClass) {
				auto& arrow = static_cast<RE::ArrowProjectile*>(a_projectile)->GetArrowRuntimeData();
				shot.enchant = IdOf(arrow.enchantItem);
				shot.poison = IdOf(arrow.poison);
			}
			shot.kind = ShotRules::KindOf(a_base->data.types.underlying());
			const auto source = static_cast<std::uint32_t>(rd.castingSource);
			shot.source = static_cast<std::uint8_t>(source < Protocol::SHOT_SOURCE_COUNT ? source : static_cast<std::uint32_t>(RE::MagicSystem::CastingSource::kOther));
			if (rd.flags.any(RE::Projectile::Flags::kIsDual)) {
				shot.flags |= Protocol::kShotDual;
			}
			if (rd.spell && rd.spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
				shot.flags |= Protocol::kShotHeld;
			}
			const auto& at = a_projectile->GetPosition();
			shot.x = at.x;
			shot.y = at.y;
			shot.z = at.z;
			shot.pitch = Pitch(a_projectile->data.angle.x);
			shot.heading = Turn(a_projectile->data.angle.z);
			shot.power = Amount(rd.power);
			shot.scale = Amount(rd.scale);
			return shot;
		}

		// Every update of every projectile, on whatever thread the game updates it. True: its update is skipped (a native
		// being ended).
		bool OnUpdate(RE::Projectile* a_projectile, bool a_arrowClass)
		{
			if (!active.load(std::memory_order_relaxed) || !a_projectile) {
				return false;
			}
			const auto id = a_projectile->GetFormID();
			if (endingCount.load(std::memory_order_relaxed) > 0) {
				std::scoped_lock l{ lock };
				if (ending.contains(id)) {
					++hookCounts.skipped;
					return true;
				}
			}
			const auto& rd = a_projectile->GetProjectileRuntimeData();
			if (!(rd.livingTime < ShotRules::YOUNG)) {
				return false;  // never new again (and NaN is never new)
			}
			const auto now = Clock::now();
			{
				std::scoped_lock l{ lock };
				if (!sight.New(reinterpret_cast<std::uintptr_t>(a_projectile), id, rd.livingTime, now)) {
					return false;
				}
				++hookCounts.firsts;
			}
			const auto base = a_projectile->GetProjectileBase();
			const auto ref = rd.shooter.get();
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			ShotRules::Shooter who;
			who.actor = actor != nullptr && base != nullptr;
			if (who.actor) {
				// Never under our lock: these take their own.
				who.standIn = RemotePlayers::PlayerOf(actor) != 0;
				who.movedCopy = !who.standIn && NpcSync::Mirrored(actor);
			}
			const auto& at = a_projectile->GetPosition();
			std::optional<Candidate> candidate;
			if (who.actor && !who.standIn && !who.movedCopy) {
				candidate = Candidate{ rd.shooter, Snapshot(a_projectile, base, a_arrowClass, now) };
			}
			std::scoped_lock l{ lock };
			who.launchedByUs = launchedIds.contains(id) ||
			                   (who.actor && pending.Match(reinterpret_cast<std::uintptr_t>(actor), base->GetFormID(), at.x, at.y, at.z, now));
			switch (ShotRules::Judge(who)) {
			case ShotRules::Fate::kOurs:
				++hookCounts.oursSeen;
				return false;
			case ShotRules::Fate::kEnd:
				ending[id] = Ending{ now, false };
				endingCount.store(ending.size(), std::memory_order_relaxed);
				++hookCounts.nativeEnded;
				hookCounts.nativeByKind.Add(ShotRules::KindOf(base->data.types.underlying()));
				return true;
			case ShotRules::Fate::kSay:
				if (!candidate) {
					return false;
				}
				if (candidates.size() >= MAX_CANDIDATES) {
					++hookCounts.candidatesDropped;
				} else {
					candidates.push_back(std::move(*candidate));
				}
				return false;
			case ShotRules::Fate::kLeave:
			default:
				return false;
			}
		}

		template <std::size_t I>
		void UpdateHook(RE::Projectile* a_this, float a_delta)
		{
			if (OnUpdate(a_this, I == 0)) {
				return;
			}
			originals[I](a_this, a_delta);
		}

		template <std::size_t... I>
		constexpr std::array<UpdateFn, sizeof...(I)> HooksFor(std::index_sequence<I...>)
		{
			return { &UpdateHook<I>... };
		}
		const auto HOOKS = HooksFor(std::make_index_sequence<CLASSES.size()>{});

		// ---- ours ----

		void SayOurs(const SendFn& a_send, std::vector<Candidate>& a_taken, Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto us = ourId.load();
			for (auto& candidate : a_taken) {
				const auto ref = candidate.shooter.get();
				const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor) {
					++counts.shooterGone;
					continue;
				}
				if (actor == player) {
					candidate.shot.player = us;
				} else if (const auto key = NpcSync::KeyOf(actor); key && NpcSync::RunnerOf(*key) == us) {
					candidate.shot.npc = *key;
				} else {
					++counts.notOurs;  // another game's NPC on its own AI here, or one no game shares
					continue;
				}
				if (!Protocol::ShotHolds(candidate.shot)) {
					++counts.unfit;
					continue;
				}
				if (outbox.Seen(candidate.shot, a_now)) {
					counts.saidNpc += candidate.shot.player == 0 ? 1 : 0;
					counts.saidDual += (candidate.shot.flags & Protocol::kShotDual) != 0 ? 1 : 0;
					lastSaid = ShotText(candidate.shot);
				}
			}
			auto shots = outbox.Take();
			if (shots.empty()) {
				return;
			}
			Protocol::Shots msg;
			msg.shots = std::move(shots);
			a_send(Protocol::Encode(msg), true);
		}

		// ---- theirs ----

		// The velocity of a launch of ours turned back to its shooter's aim when the engine aimed it elsewhere; the error
		// it had. None: it has no velocity to read (a beam, a cone).
		std::optional<float> FixAim(RE::Projectile* a_projectile, float a_pitch, float a_heading)
		{
			auto&      rd = a_projectile->GetProjectileRuntimeData();
			const auto v = rd.linearVelocity;
			const auto error = ShotRules::AimError(v.x, v.y, v.z, a_pitch, a_heading);
			if (!error) {
				return std::nullopt;
			}
			if (const auto turned = ShotRules::Reaimed(v.x, v.y, v.z, a_pitch, a_heading)) {
				const RE::NiPoint3 now{ (*turned)[0], (*turned)[1], (*turned)[2] };
				// velocity is the same speed's in most launches; only then is it turned too.
				const float speed = v.Length();
				const float other = rd.velocity.Length();
				if (std::isfinite(other) && std::fabs(other - speed) <= 0.1f * speed) {
					rd.velocity = now;
				}
				rd.linearVelocity = now;
				a_projectile->data.angle.x = a_pitch;
				a_projectile->data.angle.z = a_heading;
			}
			return error;
		}

		void LaunchOne(const Protocol::Shot& a_shot, Clock::time_point a_now)
		{
			RE::NiPointer<RE::Actor> actor;
			if (a_shot.player != 0) {
				actor = RemotePlayers::StandInOf(a_shot.player);
			} else {
				if (NpcSync::RunnerOf(a_shot.npc) == ourId.load()) {
					++counts.runByUs;  // its runner changed to us since: our own AI launches its shots now
					return;
				}
				actor = NpcSync::ActorOf(a_shot.npc);
				if (actor && !NpcSync::Mirrored(actor.get())) {
					++counts.unmirrored;  // a copy on its own AI here launches its own shots
					return;
				}
			}
			if (!actor || !actor->Is3DLoaded()) {
				++counts.noShooter;
				return;
			}
			const RE::NiPoint3 origin{ a_shot.x, a_shot.y, a_shot.z };
			if (!ShotRules::Launchable(actor->GetPosition().GetDistance(origin), actor->GetHeight())) {
				++counts.tooFar;
				return;
			}
			const auto base = RE::TESForm::LookupByID<RE::BGSProjectile>(a_shot.projectile);
			if (!base) {
				++counts.unknown;
				return;
			}
			if (!live.Room()) {
				++counts.full;
				return;
			}
			RE::Projectile::LaunchData data(base, actor.get(), origin, RE::Projectile::ProjectileRot{ a_shot.pitch, a_shot.heading });
			data.combatController = nullptr;  // no combat aim: the shooter's own aim is the one
			data.desiredTarget = nullptr;
			data.weaponSource = a_shot.weapon != 0 ? RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_shot.weapon) : nullptr;
			data.ammoSource = a_shot.ammo != 0 ? RE::TESForm::LookupByID<RE::TESAmmo>(a_shot.ammo) : nullptr;
			data.spell = MagicOf(a_shot.spell);
			data.castingSource = static_cast<RE::MagicSystem::CastingSource>(a_shot.source);
			data.enchantItem = a_shot.enchant != 0 ? RE::TESForm::LookupByID<RE::EnchantmentItem>(a_shot.enchant) : nullptr;
			data.poison = a_shot.poison != 0 ? RE::TESForm::LookupByID<RE::AlchemyItem>(a_shot.poison) : nullptr;
			if (data.spell) {
				const auto effect = data.spell->GetCostliestEffectItem();
				data.area = effect ? static_cast<std::int32_t>(effect->GetArea()) : 0;
			}
			data.power = a_shot.power;
			data.scale = a_shot.scale;
			data.autoAim = false;
			data.useOrigin = true;
			data.alwaysHit = false;
			data.forceConeOfFire = false;
			data.deferInitialization = false;

			std::uint64_t token = 0;
			{
				std::scoped_lock l{ lock };
				token = pending.Add(reinterpret_cast<std::uintptr_t>(actor.get()), base->GetFormID(), origin.x, origin.y, origin.z, a_now);
			}
			RE::ProjectileHandle handle;
			RE::Projectile::Launch(&handle, data);  // never under our lock: its first update may run in it
			const auto projectile = handle.get();
			{
				std::scoped_lock l{ lock };
				pending.Cancel(token);
				if (projectile) {
					const auto id = projectile->GetFormID();
					launchedIds[id] = a_now;
					if (ending.erase(id) > 0) {
						// Its first update ran before the launch returned and was taken for a native: it flies after all.
						++hookCounts.raced;
						endingCount.store(ending.size(), std::memory_order_relaxed);
					}
				}
			}
			if (!projectile) {
				++counts.launchFailed;
				logs::info("Shots: the launch of {} failed", ShotText(a_shot));
				return;
			}
			if (const auto error = FixAim(projectile.get(), a_shot.pitch, a_shot.heading)) {
				counts.aimErrMax = (std::max)(counts.aimErrMax, *error);
				if (*error > ShotRules::AIM_FIX) {
					++counts.reaimed;
				}
			}
			const auto id = projectile->GetFormID();
			live.Add(id, a_shot.kind, base->data.lifetime, a_now);
			aimChecks[id] = AimCheck{ a_shot.pitch, a_shot.heading };
			counts.launched.Add(a_shot.kind);
			counts.launchedDual += (a_shot.flags & Protocol::kShotDual) != 0 ? 1 : 0;
			if (counts.launched.Total() == 1) {
				logs::info("Shots: the first friend's shot launched here: {}", ShotText(a_shot));
			}
			lastLaunched = ShotText(a_shot);
		}

		void Kill(RE::FormID a_id)
		{
			if (const auto projectile = RE::TESForm::LookupByID<RE::Projectile>(a_id); projectile && !projectile->IsDeleted()) {
				projectile->Kill();
			}
		}

		// The launches of ours, once a frame: their aim read again once (the engine may turn it in its first update), and
		// each ended when its flight is over.
		void StepLive(Clock::time_point a_now)
		{
			for (const auto id : live.Ids()) {
				const auto projectile = RE::TESForm::LookupByID<RE::Projectile>(id);
				const bool gone = !projectile || projectile->IsDeleted() ||
				                  projectile->GetProjectileRuntimeData().flags.any(RE::Projectile::Flags::kDestroyed);
				bool stopped = false;
				if (!gone) {
					auto& rd = projectile->GetProjectileRuntimeData();
					stopped = rd.linearVelocity.Length() < 1.0f && rd.livingTime > ShotRules::YOUNG;
					if (const auto check = aimChecks.find(id); check != aimChecks.end() && rd.livingTime > 0.0f) {
						if (const auto offBy = FixAim(projectile.get(), check->second.pitch, check->second.heading)) {
							counts.aimLeftMax = (std::max)(counts.aimLeftMax, *offBy);
							if (*offBy > ShotRules::AIM_FIX) {
								++counts.reaimedLate;
							}
						}
						aimChecks.erase(check);
					}
				}
				if (live.Step(id, gone, stopped, a_now)) {
					Kill(id);
				}
				if (gone) {
					aimChecks.erase(id);
				}
			}
			std::erase_if(aimChecks, [](const auto& a_entry) { return !RE::TESForm::LookupByID(a_entry.first); });
		}

		void EndAll(bool a_forSave)
		{
			for (const auto id : live.TakeAll()) {
				Kill(id);
				if (a_forSave) {
					++counts.endedSave;
				}
			}
			aimChecks.clear();
			std::vector<RE::FormID> natives;
			{
				std::scoped_lock l{ lock };
				for (const auto& [id, entry] : ending) {
					natives.push_back(id);
				}
			}
			for (const auto id : natives) {
				Kill(id);
			}
		}

		void ForgetShared()
		{
			std::scoped_lock l{ lock };
			sight.Clear();
			pending.Clear();
			launchedIds.clear();
			ending.clear();
			endingCount.store(0);
			candidates.clear();
		}

		// ---- the rig's finds ----

		std::string FindCrossbow()
		{
			const auto dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return "none: no data yet";
			}
			RE::TESObjectWEAP* bow = nullptr;
			for (const auto weap : dh->GetFormArray<RE::TESObjectWEAP>()) {
				if (weap && weap->IsCrossbow() && weap->GetPlayable() && (!bow || weap->GetFormID() < bow->GetFormID())) {
					bow = weap;
				}
			}
			RE::TESAmmo* bolt = nullptr;
			for (const auto ammo : dh->GetFormArray<RE::TESAmmo>()) {
				if (ammo && ammo->IsBolt() && ammo->GetPlayable() && (!bolt || ammo->GetFormID() < bolt->GetFormID())) {
					bolt = ammo;
				}
			}
			if (!bow || !bolt) {
				return "none: no crossbow or bolt in the load order";
			}
			return std::format("weapon={:08X} ammo={:08X} name=\"{}\"", bow->GetFormID(), bolt->GetFormID(), bow->GetName());
		}

		// The lowest-id spell whose costliest effect launches a projectile of a_kind: aimed (or at a place, a rune),
		// fire and forget or held; a spell before a shout.
		std::string FindSpell(Kind a_kind)
		{
			const auto dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return "none: no data yet";
			}
			RE::SpellItem*  best = nullptr;
			RE::BGSProjectile* bestBase = nullptr;
			const auto rank = [](const RE::SpellItem* a_spell) { return a_spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell ? 0 : 1; };
			for (const auto spell : dh->GetFormArray<RE::SpellItem>()) {
				if (!spell) {
					continue;
				}
				const auto type = spell->GetSpellType();
				if (type != RE::MagicSystem::SpellType::kSpell && type != RE::MagicSystem::SpellType::kVoicePower) {
					continue;
				}
				const auto casting = spell->GetCastingType();
				if (casting != RE::MagicSystem::CastingType::kFireAndForget && casting != RE::MagicSystem::CastingType::kConcentration) {
					continue;
				}
				const auto delivery = spell->GetDelivery();
				if (delivery != RE::MagicSystem::Delivery::kAimed && delivery != RE::MagicSystem::Delivery::kTargetLocation) {
					continue;
				}
				const auto effect = spell->GetCostliestEffectItem();
				const auto setting = effect ? effect->baseEffect : nullptr;
				const auto base = setting ? setting->data.projectileBase : nullptr;
				if (!base || ShotRules::KindOf(base->data.types.underlying()) != a_kind) {
					continue;
				}
				if (!best || rank(spell) < rank(best) || (rank(spell) == rank(best) && spell->GetFormID() < best->GetFormID())) {
					best = spell;
					bestBase = base;
				}
			}
			if (!best) {
				return std::format("none: no spell launches a {}", ShotRules::NameOf(a_kind));
			}
			return std::format("spell={:08X} projectile={:08X} kind={} casting={} shout={} name=\"{}\"", best->GetFormID(), bestBase->GetFormID(), ShotRules::NameOf(a_kind),
				best->GetCastingType() == RE::MagicSystem::CastingType::kConcentration ? "held" : "ff", rank(best), best->GetName());
		}

		std::string Report()
		{
			HookCounts shared;
			std::size_t sightSize = 0, pendingSize = 0, endingSize = 0, waiting = 0;
			{
				std::scoped_lock l{ lock };
				shared = hookCounts;
				sightSize = sight.Size();
				pendingSize = pending.Size();
				endingSize = ending.size();
				waiting = candidates.size();
			}
			// Parts by their first word, for the rig (tools/rig/steps/shots.sh): shots, ours, theirs, natives, flying, aim,
			// hook, then one "from" per game heard from.
			std::string out = std::format("shots active={} hooked={}/{}", active.load() ? 1 : 0, hooked, CLASSES.size());
			out += std::format(" | ours seen={} sent={} byKind={} npc={} dual={} coalesced={} dropped={} notOurs={} shooterGone={} unfit={} waiting={}", outbox.seen, outbox.sent,
				outbox.byKind.Line(), counts.saidNpc, counts.saidDual, outbox.coalesced, outbox.dropped, counts.notOurs, counts.shooterGone, counts.unfit, waiting);
			out += std::format(" | theirs in={} launched={} byKind={} dual={} late={} inboxDropped={} queued={} noShooter={} far={} unknown={} full={} launchFailed={} runByUs={} unmirrored={}",
				counts.in, counts.launched.Total(), counts.launched.Line(), counts.launchedDual, inbox.late, inbox.dropped, inbox.Size(), counts.noShooter, counts.tooFar, counts.unknown,
				counts.full, counts.launchFailed, counts.runByUs, counts.unmirrored);
			out += std::format(" | natives ended={} byKind={} skipped={} ending={}", shared.nativeEnded, shared.nativeByKind.Line(), shared.skipped, endingSize);
			out += std::format(" | flying oursSeen={} raced={} pending={} live={} liveArrows={} gone={} endedStuck={} endedOld={} endedSave={}", shared.oursSeen, shared.raced,
				pendingSize, live.Count(), live.Count(Kind::kArrow), live.gone, live.endedStuck, live.endedOld, counts.endedSave);
			out += std::format(" | aim reaimed={} reaimedLate={} errMax={:.1f} left={:.1f}", counts.reaimed, counts.reaimedLate, counts.aimErrMax, counts.aimLeftMax);
			out += std::format(" | hook firsts={} sight={} dropped={}", shared.firsts, sightSize, shared.candidatesDropped);
			for (const auto& [player, stats] : perPlayer) {
				out += std::format(" | from player={} in={} launched={} npcIn={} npcLaunched={}", player, stats.in, stats.launched, stats.npcIn, stats.npcLaunched);
			}
			return out;
		}
	}

	bool Install()
	{
		for (std::size_t i = 0; i < CLASSES.size(); ++i) {
			REL::Relocation<std::uintptr_t> vtable{ CLASSES[i].vtable };
			if (VtableHook::Slot<UpdateFn>(vtable.address(), UPDATE_SLOT, HOOKS[i], originals[i], "Shots", CLASSES[i].what)) {
				++hooked;
			}
		}
		if (hooked < CLASSES.size()) {
			logs::warn("Shots: {} of {} projectile classes hooked: the others' projectiles of the rest are neither said nor shown", hooked, CLASSES.size());
		}
		return hooked > 0;
	}

	void Started(std::uint32_t a_ourId)
	{
		ourId.store(a_ourId);
		outbox.Clear();
		inbox.Clear();
		ForgetShared();
		active.store(true);
	}

	void Frame(const SendFn& a_send)
	{
		if (!active.load()) {
			return;
		}
		const auto              now = Clock::now();
		std::vector<Candidate>  taken;
		std::vector<RE::FormID> toKill;
		{
			std::scoped_lock l{ lock };
			taken.swap(candidates);
			for (auto it = ending.begin(); it != ending.end();) {
				if (now - it->second.at > ENDING_FOR) {
					it = ending.erase(it);
					continue;
				}
				if (!it->second.killed) {
					it->second.killed = true;
					toKill.push_back(it->first);
				}
				++it;
			}
			endingCount.store(ending.size(), std::memory_order_relaxed);
			std::erase_if(launchedIds, [&](const auto& a_entry) { return now - a_entry.second > LAUNCHED_FOR; });
		}
		for (const auto id : toKill) {
			Kill(id);
		}
		SayOurs(a_send, taken, now);
		const auto drawn = now - std::chrono::milliseconds(StateBuffer::delayMs.load());
		for (const auto& shot : inbox.Due(drawn)) {
			const auto before = counts.launched.Total();
			LaunchOne(shot, now);
			if (counts.launched.Total() > before) {
				// Credited to the game that sent it: a player's own, or the NPC's runner's.
				if (shot.player != 0) {
					++perPlayer[shot.player].launched;
				} else {
					++perPlayer[NpcSync::RunnerOf(shot.npc)].npcLaunched;
				}
			}
		}
		StepLive(now);
	}

	void Heard(const Protocol::Shots& a_msg)
	{
		if (!active.load()) {
			return;
		}
		const auto now = Clock::now();
		auto&      from = perPlayer[a_msg.sender];
		for (const auto& shot : a_msg.shots) {
			++counts.in;
			std::optional<Clock::time_point> at;
			if (shot.player != 0) {
				++from.in;
				at = RemotePlayers::SenderTime(shot.player, shot.sentMs);
			} else {
				++from.npcIn;
				at = NpcSync::SenderTime(shot.npc, shot.sentMs);
			}
			inbox.Push(shot, at, now);
		}
	}

	void Clear()
	{
		if (active.exchange(false)) {
			EndAll(false);
		}
		outbox.Clear();
		inbox.Clear();
		ForgetShared();
		perPlayer.clear();
	}

	void ForgetWorld()
	{
		// The world's projectiles went with it: nothing to end, nothing held.
		(void)live.TakeAll();
		aimChecks.clear();
		inbox.Clear();
		ForgetShared();
	}

	void BeforeSave()
	{
		EndAll(true);
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "find") {
			if (words.size() != 2) {
				return "usage: shots find <missile|flame|beam|lobber|cone|crossbow>";
			}
			if (words[1] == "crossbow") {
				return FindCrossbow();
			}
			const auto kind = ShotRules::KindNamed(words[1]);
			if (!kind || *kind == Kind::kArrow || *kind == Kind::kOther) {
				return "usage: shots find <missile|flame|beam|lobber|cone|barrier|crossbow>";
			}
			return FindSpell(*kind);
		}
		if (!words.empty() && words[0] == "last") {
			return std::format("said {} | launched {}", lastSaid, lastLaunched);
		}
		return Report();
	}
}
