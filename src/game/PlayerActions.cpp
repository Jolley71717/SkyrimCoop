#include "game/PlayerActions.h"

#include "DevLine.h"
#include "game/Furniture.h"
#include "game/Pairs.h"
#include "game/Reactions.h"
#include "game/Gear.h"
#include "game/StandInRules.h"
#include "game/VtableHook.h"
#include "game/Water.h"
#include "net/FormsCommand.h"

#include <atomic>
#include <mutex>

namespace PlayerActions
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The attack states StandInRules::Seen names, the game's own numbers.
		using AS = RE::ATTACK_STATE_ENUM;
		static_assert(StandInRules::kAttackNone == std::to_underlying(AS::kNone) && StandInRules::kAttackDraw == std::to_underlying(AS::kDraw) &&
						  StandInRules::kAttackFollowThrough == std::to_underlying(AS::kFollowThrough) && StandInRules::kAttackBash == std::to_underlying(AS::kBash) &&
						  StandInRules::kAttackBowDraw == std::to_underlying(AS::kBowDraw) && StandInRules::kAttackFired == std::to_underlying(AS::kFired),
			"StandInRules' attack states are the game's");

		// ---- the hook: IAnimationGraphManagerHolder::NotifyAnimationGraph of the player ----
		// The player's own class has its own vtables: its holder's is the one in the player object (read from it, so no
		// index into the class's vtables is assumed), and slot 1 of it is the notify, as in Character's (NpcSync.cpp).
		// Every event the game's input and the player's magic and combat send the player's graph goes through it, on
		// whatever thread sends it. Our own events to stand-ins go through Character's vtable, never this one.
		using NotifyFn = bool (*)(RE::IAnimationGraphManagerHolder*, const RE::BSFixedString&);
		NotifyFn originalNotify = nullptr;

		std::mutex               lock;  // outbox, last and seen
		StandInRules::Outbox     outbox;
		std::string              last;
		StandInRules::Seen       seen;  // what the game shows our player doing (Sample): the rig's proof a press did something
		std::atomic<bool>        recording{ false };
		std::atomic<bool>        hooked{ false };
		std::atomic<std::uint64_t> notOurs{ 0 };  // events through the hook of a holder that is not our player (none expected)

		bool NotifyHook(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
		{
			const bool taken = originalNotify(a_this, a_event);
			if (!taken) {
				return taken;
			}
			const bool inSession = recording.load(std::memory_order_relaxed);
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || a_this != static_cast<RE::IAnimationGraphManagerHolder*>(player)) {
				if (inSession) {
					notOurs.fetch_add(1, std::memory_order_relaxed);
				}
				return taken;
			}
			const auto name = a_event.c_str();
			if (!name) {
				return taken;
			}
			const auto now = Clock::now();
			// Kept for the rig's `swim` (the events the game's own swimming sends our graph), session or not.
			Water::Heard(name, now);
			// The engine's furniture play (sitting down, the idle, getting up) is never sent: every game plays its own for
			// the stand-in it puts in its copy of the furniture (Furniture.h).
			if (!inSession || Furniture::EngineOwnsEvents(player)) {
				return taken;
			}
			// Nor a paired animation's (a kill move): its beginning is said as a pair, and every game plays the whole pair on
			// the actors that show both (Pairs.h).
			Pairs::Noted(player, name);
			if (Pairs::EventsStay(player)) {
				return taken;
			}
			std::scoped_lock l{ lock };
			if (outbox.Heard(name, Protocol::SentMs(now), now, Reactions::StaggerOf(player, name))) {
				last = name;
			}
			return taken;
		}
	}

	bool Install()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			logs::warn("Actions: no player yet: our player's actions are not sent");
			return false;
		}
		// The holder's vtable as the player object has it.
		const auto holder = static_cast<RE::IAnimationGraphManagerHolder*>(player);
		const auto vtable = *reinterpret_cast<const std::uintptr_t*>(holder);
		REL::Relocation<std::uintptr_t> listed{ RE::VTABLE_PlayerCharacter[3] };
		if (vtable != listed.address()) {
			// Said for the record (it tells whether the class's vtable list is in the order NpcSync assumes for Character's).
			logs::info("Actions: the player's graph holder vtable is not PlayerCharacter's fourth listed one (game+{:X} vs game+{:X}); the object's is hooked",
				vtable - REL::Module::get().base(), listed.address() - REL::Module::get().base());
		}
		if (!VtableHook::Slot<NotifyFn>(vtable, 1, &NotifyHook, originalNotify, "Actions", "player's graph notify")) {
			logs::warn("Actions: without the player's graph notify hook, the others see our walk and gear but no attack, cast or shout");
			return false;
		}
		hooked = true;
		return true;
	}

	void Recording(bool a_on)
	{
		recording = a_on;
		std::scoped_lock l{ lock };
		outbox.Clear();
	}

	std::vector<Protocol::PlayerAction> Take()
	{
		std::scoped_lock l{ lock };
		return outbox.Take();
	}

	void Sample()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			std::scoped_lock l{ lock };
			seen.Forget();
			return;
		}
		StandInRules::Doing doing;
		if (const auto state = player->AsActorState()) {
			doing.attackState = static_cast<std::uint32_t>(std::to_underlying(state->GetAttackState()));
			doing.drawn = state->GetWeaponState() == RE::WEAPON_STATE::kDrawn;
		}
		const auto process = player->GetActorRuntimeData().currentProcess;
		const auto high = process ? process->high : nullptr;
		doing.power = high && high->attackData && high->attackData->data.flags.all(RE::AttackData::AttackFlag::kPowerAttack);
		doing.blocking = player->IsBlocking();
		const auto casting = [&](RE::MagicSystem::CastingSource a_source) {
			const auto caster = player->GetMagicCaster(a_source);
			return caster && caster->state.get() != RE::MagicCaster::State::kNone;
		};
		doing.castLeft = casting(RE::MagicSystem::CastingSource::kLeftHand);
		doing.castRight = casting(RE::MagicSystem::CastingSource::kRightHand);
		doing.castVoice = casting(RE::MagicSystem::CastingSource::kOther);
		doing.voiceRecovery = player->GetVoiceRecoveryTime();
		if (const auto values = player->AsActorValueOwner()) {
			doing.stamina = values->GetActorValue(RE::ActorValue::kStamina);
			doing.magicka = values->GetActorValue(RE::ActorValue::kMagicka);
		}
		std::scoped_lock l{ lock };
		seen.Sample(doing);
	}

	std::string Equip(std::string_view a_args)
	{
		constexpr std::string_view USAGE = "error: usage: actions equip <right|left|ammo|voice> <hex form id>";
		const auto                 words = DevLine::Words(a_args);
		const auto                 id = words.size() == 2 ? FormsCommand::ParseHex(words[1]) : std::nullopt;
		const auto                 player = RE::PlayerCharacter::GetSingleton();
		if (!id || !player) {
			return std::string{ USAGE };
		}
		const auto form = RE::TESForm::LookupByID(*id);
		if (!form) {
			return std::format("error: no form {:08X} in this game", *id);
		}
		const auto  where = words[0];
		std::string why;
		if (where == "voice") {
			why = Gear::SetVoice(player, *id, false) ? "" : "not a shout or a power our player knows";
		} else if (where == "right" || where == "left" || where == "ammo") {
			why = Gear::EquipOne(player, form, where == "right" ? NpcRules::Hand::kRight : where == "left" ? NpcRules::Hand::kLeft : NpcRules::Hand::kNone);
		} else {
			return std::string{ USAGE };
		}
		if (!why.empty()) {
			return std::format("error: {:08X} can't go in the {}: {}", *id, where, why);
		}
		// Read back: the game may take it now or a moment later; the rig waits for our part's right=/left=/ammo=/voice=.
		const auto hands = Gear::HandsOf(player);
		const auto now = where == "right" ? hands.right : where == "left" ? hands.left : where == "ammo" ? hands.ammo : hands.voice;
		logs::info("Actions: our player's {} given {:08X} from the developer channel (holds {:08X} now)", where, *id, now);
		return std::format("{} {} {:08X} holds={:08X}", now == *id ? "equipped" : "asked", where, *id, now);
	}

	std::string Describe()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto hands = Gear::HandsOf(player);
		const auto values = player ? player->AsActorValueOwner() : nullptr;
		const auto health = values ? values->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
		const auto healthMax = values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : 0.0f;
		// A conjured weapon in a hand (the rig's proof its bound weapon spell made one).
		bool bound = false;
		if (player) {
			const auto [right, left] = Gear::HeldObjects(player);
			for (const auto held : { right, left }) {
				const auto weapon = held ? held->As<RE::TESObjectWEAP>() : nullptr;
				bound = bound || (weapon && weapon->IsBound());
			}
		}
		std::scoped_lock l{ lock };
		return std::format("actions hooked={} recording={} heard={} byKind={} sent={} dropped={} waiting={} notOurs={} last={} seen={} staminaSpent={:.1f} magickaSpent={:.1f}"
						   " right={:08X} left={:08X} ammo={:08X} voice={:08X} unnamed={:X} bound={} hp={:.1f}/{:.1f}",
			hooked ? 1 : 0, recording ? 1 : 0, outbox.heard, outbox.byAct.Line(), outbox.sent, outbox.dropped, outbox.Waiting(), notOurs.load(),
			last.empty() ? "none" : last, seen.byAct.Line(), seen.staminaSpent, seen.magickaSpent, hands.right, hands.left, hands.ammo, hands.voice, hands.unnamed,
			bound ? 1 : 0, health, healthMax);
	}
}
