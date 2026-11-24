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
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace Shots
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		using Kind = Protocol::ShotKind;
		using Key = Protocol::NpcKey;
		using Ident = ShotRules::Ident;
		using Aim = ShotRules::Aim;

		// Projectile::UpdateImpl, the update of every projectile class (SE and AE; this plugin is not built for VR).
		constexpr std::size_t UPDATE_SLOT = 0xAB;
		// The projectiles the hook saw at their first update, for the main thread to say (each said by the next frame).
		constexpr std::size_t MAX_CANDIDATES = 256;
		// Before a save, how long a launch of ours being updated right then is waited for (it is ended once that is over).
		constexpr auto SAVE_WAIT = 20ms;

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

		// What the hook (any thread), the hit events (any thread) and the main thread share.
		std::mutex                 lock;
		ShotRules::FirstSight      sight;
		ShotRules::Pending         pending;
		ShotRules::Tracker         tracker;  // natives being ended, launches of ours (by object and id: ShotRules::Ident)
		std::atomic<std::size_t>   trackedCount{ 0 };
		ShotRules::HitBook         hitBook;
		std::vector<Candidate>     candidates;
		struct HookCounts
		{
			std::uint64_t firsts = 0, oursSeen = 0, nativeEnded = 0, skipped = 0, candidatesDropped = 0, raced = 0;
			std::uint64_t reaimed = 0, reaimedLate = 0;
			float         aimErrMax = 0.0f, turnedMax = 0.0f, aimLeftMax = 0.0f;
			ShotRules::KindCounts nativeByKind;
		};
		HookCounts hookCounts;

		// Main thread only.
		ShotRules::Outbox  outbox;
		ShotRules::Inbox   inbox;
		ShotRules::Live    live;
		std::vector<Ident> toEnd;  // launches of ours to end, whose update ran when they were due (tried again each frame)

		struct Counts
		{
			std::uint64_t notOurs = 0, shooterGone = 0, unfit = 0, saidNpc = 0, saidDual = 0, launchedDual = 0;
			std::uint64_t in = 0, noShooter = 0, tooFar = 0, unknown = 0, full = 0, launchFailed = 0, runByUs = 0, unmirrored = 0;
			std::uint64_t endedSave = 0, endWaits = 0, endMissed = 0, untracked = 0, survived = 0, lingering = 0;
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

		Ident IdentOf(const RE::Projectile* a_projectile)
		{
			return Ident{ reinterpret_cast<std::uintptr_t>(a_projectile), a_projectile->GetFormID() };
		}

		// The shooter a hit event names (its cause), as a launch of ours was put in the hit book.
		std::uintptr_t ShooterKey(const RE::TESObjectREFR* a_ref)
		{
			return reinterpret_cast<std::uintptr_t>(a_ref);
		}

		// The projectile a_p names, when it is still that object: none when its id is free, or names another by now.
		RE::Projectile* Find(Ident a_p)
		{
			const auto projectile = RE::TESForm::LookupByID<RE::Projectile>(a_p.id);
			return projectile && reinterpret_cast<std::uintptr_t>(projectile) == a_p.object ? projectile : nullptr;
		}

		bool Ended(const RE::Projectile* a_projectile)
		{
			return !a_projectile || a_projectile->IsDeleted() || a_projectile->GetProjectileRuntimeData().flags.any(RE::Projectile::Flags::kDestroyed);
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

		// The error of a projectile's flight off a_aim, in degrees, and (a_fix) its velocity turned to a_aim when it is
		// further off than AIM_FIX. None: it has no velocity to read (a beam, a cone). Only in the projectile's own update
		// (the update hook), on the thread that updates it.
		std::optional<float> AimAt(RE::Projectile* a_projectile, Aim a_aim, bool a_fix)
		{
			auto&      rd = a_projectile->GetProjectileRuntimeData();
			const auto v = rd.linearVelocity;
			const auto error = ShotRules::AimError(v.x, v.y, v.z, a_aim.pitch, a_aim.heading);
			if (!error || !a_fix) {
				return error;
			}
			if (const auto turned = ShotRules::Reaimed(v.x, v.y, v.z, a_aim.pitch, a_aim.heading)) {
				const RE::NiPoint3 now{ (*turned)[0], (*turned)[1], (*turned)[2] };
				// velocity is the same speed's in most launches; only then is it turned too.
				const float speed = v.Length();
				const float other = rd.velocity.Length();
				if (std::isfinite(other) && std::fabs(other - speed) <= 0.1f * speed) {
					rd.velocity = now;
				}
				rd.linearVelocity = now;
				a_projectile->data.angle.x = a_aim.pitch;
				a_projectile->data.angle.z = a_aim.heading;
			}
			return error;
		}

		// What the hook does at one update.
		struct HookStep
		{
			bool               skip = false;     // the update is not run
			bool               tracked = false;  // a launch of ours: in flight until the update is over (Tracker::Left)
			Ident              ident;
			std::optional<Aim> aim;
			bool               fix = false;
			bool               first = false;
		};

		HookStep StepOf(const ShotRules::Tracker::Update& a_update, Ident a_p)
		{
			if (a_update.skip) {
				++hookCounts.skipped;
				return HookStep{ .skip = true };
			}
			hookCounts.oursSeen += a_update.first ? 1 : 0;
			return HookStep{ .skip = false, .tracked = true, .ident = a_p, .aim = a_update.aim, .fix = a_update.fix, .first = a_update.first };
		}

		// Every update of every projectile, on whatever thread the game updates it.
		HookStep OnUpdate(RE::Projectile* a_projectile, bool a_arrowClass)
		{
			if (!active.load(std::memory_order_relaxed) || !a_projectile) {
				return {};
			}
			const auto p = IdentOf(a_projectile);
			if (trackedCount.load(std::memory_order_relaxed) > 0) {
				std::scoped_lock l{ lock };
				if (const auto update = tracker.Enter(p)) {
					return StepOf(*update, p);
				}
			}
			const auto& rd = a_projectile->GetProjectileRuntimeData();
			if (!(rd.livingTime < ShotRules::YOUNG)) {
				return {};  // never new again (and NaN is never new)
			}
			const auto now = Clock::now();
			{
				std::scoped_lock l{ lock };
				if (!sight.New(p.object, p.id, rd.livingTime, now)) {
					return {};
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
			// Tracked since the first look (its launch returned on another thread meanwhile): it is that.
			if (const auto update = tracker.Enter(p)) {
				return StepOf(*update, p);
			}
			std::optional<Aim> matched;
			if (who.actor) {
				matched = pending.Match(reinterpret_cast<std::uintptr_t>(actor), base->GetFormID(), at.x, at.y, at.z, now);
			}
			who.launchedByUs = matched.has_value();
			switch (ShotRules::Judge(who)) {
			case ShotRules::Fate::kOurs:
				{
					(void)tracker.AddOurs(p, matched, now);
					trackedCount.store(tracker.Count(), std::memory_order_relaxed);
					const auto update = tracker.Enter(p);  // its first update, run now
					return update ? StepOf(*update, p) : HookStep{};
				}
			case ShotRules::Fate::kEnd:
				tracker.AddNative(p, now);
				trackedCount.store(tracker.Count(), std::memory_order_relaxed);
				++hookCounts.nativeEnded;
				hookCounts.nativeByKind.Add(ShotRules::KindOf(base->data.types.underlying()));
				return HookStep{ .skip = true };
			case ShotRules::Fate::kSay:
				if (candidate) {
					if (candidates.size() >= MAX_CANDIDATES) {
						++hookCounts.candidatesDropped;
					} else {
						candidates.push_back(std::move(*candidate));
					}
				}
				return {};
			case ShotRules::Fate::kLeave:
			default:
				return {};
			}
		}

		template <std::size_t I>
		void UpdateHook(RE::Projectile* a_this, float a_delta)
		{
			const auto step = OnUpdate(a_this, I == 0);
			if (step.skip) {
				return;
			}
			if (!step.tracked) {
				originals[I](a_this, a_delta);
				return;
			}
			// A launch of ours: turned to its shooter's aim here, in its own update, before the game's update (its launch
			// may have aimed it elsewhere) and after (the update may turn it itself); then read once, unturned (left).
			std::optional<float> before;
			std::optional<float> after;
			if (step.aim) {
				before = AimAt(a_this, *step.aim, step.fix);
			}
			originals[I](a_this, a_delta);
			if (step.aim && step.fix) {
				after = AimAt(a_this, *step.aim, true);
			}
			std::scoped_lock l{ lock };
			tracker.Left(step.ident);
			if (before) {
				if (!step.fix) {
					hookCounts.aimLeftMax = (std::max)(hookCounts.aimLeftMax, *before);
				} else if (step.first) {
					hookCounts.aimErrMax = (std::max)(hookCounts.aimErrMax, *before);
					hookCounts.reaimed += *before > ShotRules::AIM_FIX ? 1 : 0;
				} else {
					hookCounts.turnedMax = (std::max)(hookCounts.turnedMax, *before);
					hookCounts.reaimedLate += *before > ShotRules::AIM_FIX ? 1 : 0;
				}
			}
			if (after) {
				hookCounts.turnedMax = (std::max)(hookCounts.turnedMax, *after);
				hookCounts.reaimedLate += *after > ShotRules::AIM_FIX ? 1 : 0;
			}
		}

		template <std::size_t... I>
		constexpr std::array<UpdateFn, sizeof...(I)> HooksFor(std::index_sequence<I...>)
		{
			return { &UpdateHook<I>... };
		}
		const auto HOOKS = HooksFor(std::make_index_sequence<CLASSES.size()>{});

		// What the game reports each projectile hit: put on the launch of ours it was (the hit book).
		struct HitSink final : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !a_event->cause || a_event->projectile == 0 || !active.load(std::memory_order_relaxed)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// The event names the projectile's record; should it name the projectile itself, its record is read.
				auto base = a_event->projectile;
				if (const auto projectile = RE::TESForm::LookupByID<RE::Projectile>(base)) {
					base = IdOf(projectile->GetProjectileBase());
				}
				const bool onPlayer = a_event->target && a_event->target.get() == RE::PlayerCharacter::GetSingleton();
				const auto shooter = ShooterKey(a_event->cause.get());
				std::scoped_lock l{ lock };
				if (hitBook.Hit(shooter, base, onPlayer, Clock::now()) == ShotRules::HitBook::Took::kFirst && onPlayer && hitBook.onPlayer == 1) {
					logs::info("Shots: a friend's shot launched here hit our player (shooter {:08X}, record {:08X})", a_event->cause->GetFormID(), base);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		HitSink hitSink;

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

			const Aim     aim{ a_shot.pitch, a_shot.heading };
			std::uint64_t token = 0;
			{
				std::scoped_lock l{ lock };
				token = pending.Add(reinterpret_cast<std::uintptr_t>(actor.get()), base->GetFormID(), origin.x, origin.y, origin.z, aim, a_now);
			}
			RE::ProjectileHandle handle;
			RE::Projectile::Launch(&handle, data);  // never under our lock: its first update may run in it
			const auto projectile = handle.get();
			std::optional<Ident> p;
			{
				std::scoped_lock l{ lock };
				pending.Cancel(token);
				if (projectile) {
					p = IdentOf(projectile.get());
					// Its aim is fixed in its own first updates (the hook). Its first update may have run before the launch
					// returned and been taken for a native: it flies after all.
					hookCounts.raced += tracker.AddOurs(*p, aim, a_now) ? 1 : 0;
					trackedCount.store(tracker.Count(), std::memory_order_relaxed);
					hitBook.Launched(ShooterKey(actor.get()), base->GetFormID(), a_now);
				}
			}
			if (!p) {
				++counts.launchFailed;
				logs::info("Shots: the launch of {} failed", ShotText(a_shot));
				return;
			}
			live.Add(*p, a_shot.kind, base->data.lifetime, a_now);
			counts.launched.Add(a_shot.kind);
			counts.launchedDual += (a_shot.flags & Protocol::kShotDual) != 0 ? 1 : 0;
			if (counts.launched.Total() == 1) {
				logs::info("Shots: the first friend's shot launched here: {}", ShotText(a_shot));
			}
			lastLaunched = ShotText(a_shot);
		}

		enum class EndResult : std::uint8_t
		{
			kDone,  // ended, or gone already: forgotten
			kWait,  // an update of it runs now: try again later
		};

		// Ends a launch of ours, only while no update of it runs (Tracker::Claim): the tick runs on the game's job threads,
		// and the game may update projectiles on others meanwhile.
		EndResult EndOurs(Ident a_p)
		{
			{
				std::scoped_lock l{ lock };
				if (!tracker.Claim(a_p)) {
					if (tracker.IsOurs(a_p)) {
						return EndResult::kWait;
					}
					++counts.untracked;  // never so by design: not ended, as nothing says whether it is being updated
					return EndResult::kDone;
				}
			}
			if (const auto projectile = Find(a_p); projectile && !projectile->IsDeleted()) {
				projectile->Kill();
			}
			std::scoped_lock l{ lock };
			tracker.Forget(a_p);
			trackedCount.store(tracker.Count(), std::memory_order_relaxed);
			return EndResult::kDone;
		}

		void ForgetTracked(Ident a_p)
		{
			std::scoped_lock l{ lock };
			tracker.Forget(a_p);
			trackedCount.store(tracker.Count(), std::memory_order_relaxed);
		}

		// The natives of stand-ins and moved copies: each killed once (they are never updated, so never under an update),
		// and looked at once more when their time is over: one still there and not ended would fly twice (survived=), so
		// it is killed again and held.
		void StepNatives(Clock::time_point a_now)
		{
			std::vector<Ident> toKill;
			std::vector<Ident> lapsed;
			{
				std::scoped_lock l{ lock };
				toKill = tracker.NativesToKill();
				lapsed = tracker.NativesLapsed(a_now);
			}
			for (const auto p : toKill) {
				if (const auto projectile = Find(p); projectile && !projectile->IsDeleted()) {
					projectile->Kill();
				}
			}
			for (const auto p : lapsed) {
				const auto projectile = Find(p);
				if (projectile && !projectile->IsDeleted() && !Ended(projectile)) {
					if (++counts.survived == 1) {
						logs::warn("Shots: a stand-in's or a copy's own projectile {:08X} was still there {} ms after it was killed; held, killed again",
							p.id, std::chrono::duration_cast<std::chrono::milliseconds>(ShotRules::ENDING_FOR).count());
					}
					projectile->Kill();
					std::scoped_lock l{ lock };
					tracker.HoldSurvivor(p, a_now);
					continue;
				}
				if (projectile && !projectile->IsDeleted()) {
					++counts.lingering;  // killed, not removed yet: its updates are the game's again
				}
				ForgetTracked(p);
			}
		}

		// The launches of ours, once a frame: each ended when its flight is over (read only here: what changes them is
		// done in their own update, or after a Claim).
		void StepLive(Clock::time_point a_now)
		{
			std::erase_if(toEnd, [](Ident a_p) {
				if (Ended(Find(a_p))) {
					ForgetTracked(a_p);
					return true;
				}
				if (EndOurs(a_p) == EndResult::kWait) {
					++counts.endWaits;
					return false;
				}
				return true;
			});
			for (const auto p : live.Ids()) {
				const auto projectile = Find(p);
				const bool gone = Ended(projectile);
				bool       stopped = false;
				if (!gone) {
					const auto& rd = projectile->GetProjectileRuntimeData();
					stopped = rd.linearVelocity.Length() < 1.0f && rd.livingTime > ShotRules::YOUNG;
				}
				if (live.Step(p, gone, stopped, a_now) && EndOurs(p) == EndResult::kWait) {
					++counts.endWaits;
					toEnd.push_back(p);
				}
				if (gone) {
					ForgetTracked(p);
				}
			}
		}

		// Every launch of ours ended now (a save, the session's end), each once no update of it runs (waited for at most
		// SAVE_WAIT), and every native killed.
		void EndAll(bool a_forSave)
		{
			auto all = live.TakeAll();
			all.insert(all.end(), toEnd.begin(), toEnd.end());
			toEnd.clear();
			const auto until = Clock::now() + SAVE_WAIT;
			for (const auto p : all) {
				auto result = EndOurs(p);
				while (result == EndResult::kWait && Clock::now() < until) {
					std::this_thread::yield();
					result = EndOurs(p);
				}
				if (result == EndResult::kWait) {
					++counts.endMissed;
					logs::warn("Shots: launch {:08X} was being updated through the wait: not ended{}", p.id, a_forSave ? " before the save" : "");
					continue;
				}
				counts.endedSave += a_forSave ? 1 : 0;
			}
			std::vector<Ident> natives;
			{
				std::scoped_lock l{ lock };
				natives = tracker.NativesToKill();
			}
			for (const auto p : natives) {
				if (const auto projectile = Find(p); projectile && !projectile->IsDeleted()) {
					projectile->Kill();
				}
			}
		}

		void ForgetShared()
		{
			std::scoped_lock l{ lock };
			sight.Clear();
			pending.Clear();
			tracker.Clear();
			trackedCount.store(0);
			hitBook.Clear();
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

		// `shots strays [record]`: the projectiles in the world (the game's projectile lists) whose shooter is missing or a
		// stand-in (or an actor made as one: a stand-in a save kept), of that record only when one is named. After a load,
		// any is a projectile of a friend's that a save kept.
		std::string Strays(std::string_view a_record)
		{
			std::optional<std::uint32_t> only;
			if (!a_record.empty()) {
				std::uint32_t value = 0;
				const auto    [end, error] = std::from_chars(a_record.data(), a_record.data() + a_record.size(), value, 16);
				if (error != std::errc{} || end != a_record.data() + a_record.size()) {
					return "usage: shots strays [record id, hex]";
				}
				only = value;
			}
			const auto manager = RE::Projectile::Manager::GetSingleton();
			if (!manager) {
				return "none: no projectile lists yet";
			}
			std::vector<RE::ProjectileHandle> handles;
			{
				RE::BSSpinLockGuard guard{ manager->projectileLock };
				for (const auto list : { &manager->unlimited, &manager->limited, &manager->pending }) {
					handles.insert(handles.end(), list->begin(), list->end());
				}
			}
			std::size_t total = 0, strays = 0, killed = 0;
			std::string listed;
			for (const auto& handle : handles) {
				const auto projectile = handle.get();
				if (!projectile || projectile->IsDeleted()) {
					continue;
				}
				++total;
				const auto  base = IdOf(projectile->GetProjectileBase());
				const auto& rd = projectile->GetProjectileRuntimeData();
				const auto  ref = rd.shooter.get();
				const auto  actor = ref ? ref->As<RE::Actor>() : nullptr;
				const bool  standIn = actor && (RemotePlayers::PlayerOf(actor) != 0 || RemotePlayers::MadeAsStandIn(actor));
				if ((ref && !standIn) || (only && base != *only)) {
					continue;
				}
				++strays;
				const bool ended = rd.flags.any(RE::Projectile::Flags::kDestroyed);
				killed += ended ? 1 : 0;
				if (listed.size() < 400) {
					listed += std::format(" {:08X}:record={:08X}:shooter={}{}", projectile->GetFormID(), base, ref ? std::format("{:08X}", ref->GetFormID()) : "none",
						ended ? ":killed" : "");
				}
			}
			return std::format("strays={} killed={} projectiles={}{}", strays, killed, total, listed);
		}

		std::string Report()
		{
			HookCounts       shared;
			ShotRules::HitBook hits;
			std::size_t      sightSize = 0, pendingSize = 0, endingSize = 0, tracked = 0, waiting = 0;
			{
				std::scoped_lock l{ lock };
				shared = hookCounts;
				hits = hitBook;
				sightSize = sight.Size();
				pendingSize = pending.Size();
				endingSize = tracker.Count(ShotRules::Tracker::Role::kNative);
				tracked = tracker.Count();
				waiting = candidates.size();
			}
			// Parts by their first word, for the rig (tools/rig/steps/shots.sh): shots, ours, theirs, hits, natives, flying,
			// aim, hook, then one "from" per game heard from.
			std::string out = std::format("shots active={} hooked={}/{}", active.load() ? 1 : 0, hooked, CLASSES.size());
			out += std::format(" | ours seen={} sent={} byKind={} npc={} dual={} coalesced={} dropped={} notOurs={} shooterGone={} unfit={} waiting={}", outbox.seen, outbox.sent,
				outbox.byKind.Line(), counts.saidNpc, counts.saidDual, outbox.coalesced, outbox.dropped, counts.notOurs, counts.shooterGone, counts.unfit, waiting);
			out += std::format(" | theirs in={} launched={} byKind={} dual={} late={} inboxDropped={} queued={} noShooter={} far={} unknown={} full={} launchFailed={} runByUs={} unmirrored={}",
				counts.in, counts.launched.Total(), counts.launched.Line(), counts.launchedDual, inbox.late, inbox.dropped, inbox.Size(), counts.noShooter, counts.tooFar, counts.unknown,
				counts.full, counts.launchFailed, counts.runByUs, counts.unmirrored);
			out += std::format(" | hits hit={} onPlayer={} again={} againOnPlayer={}", hits.hits, hits.onPlayer, hits.hitsAgain, hits.onPlayerAgain);
			out += std::format(" | natives ended={} byKind={} skipped={} ending={} survived={} lingering={}", shared.nativeEnded, shared.nativeByKind.Line(), shared.skipped, endingSize,
				counts.survived, counts.lingering);
			out += std::format(" | flying oursSeen={} raced={} pending={} live={} liveArrows={} gone={} endedStuck={} endedOld={} endedSave={} endWaits={} endMissed={} untracked={} tracked={}",
				shared.oursSeen, shared.raced, pendingSize, live.Count(), live.Count(Kind::kArrow), live.gone, live.endedStuck, live.endedOld, counts.endedSave, counts.endWaits,
				counts.endMissed, counts.untracked, tracked);
			out += std::format(" | aim reaimed={} reaimedLate={} errMax={:.1f} turned={:.1f} left={:.1f}", shared.reaimed, shared.reaimedLate, shared.aimErrMax, shared.turnedMax,
				shared.aimLeftMax);
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
		if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(&hitSink);
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
		const auto             now = Clock::now();
		std::vector<Candidate> taken;
		{
			std::scoped_lock l{ lock };
			taken.swap(candidates);
		}
		StepNatives(now);
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
		// Ended while the hook still holds their updates back (Tracker::Claim needs it active), then off.
		if (active.load()) {
			EndAll(false);
			active.store(false);
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
		toEnd.clear();
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
				return "usage: shots find <missile|flame|beam|lobber|cone|barrier|crossbow>";
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
		if (!words.empty() && words[0] == "strays") {
			return words.size() > 2 ? "usage: shots strays [record id, hex]" : Strays(words.size() == 2 ? words[1] : std::string_view{});
		}
		return Report();
	}
}
