#include "game/PlayerActions.h"

#include "DevLine.h"
#include "game/Gear.h"
#include "game/StandInRules.h"
#include "game/VtableHook.h"
#include "net/FormsCommand.h"

#include <atomic>
#include <mutex>

namespace PlayerActions
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// ---- the hook: IAnimationGraphManagerHolder::NotifyAnimationGraph of the player ----
		// The player's own class has its own vtables: its holder's is the one in the player object (read from it, so no
		// index into the class's vtables is assumed), and slot 1 of it is the notify, as in Character's (NpcSync.cpp).
		// Every event the game's input and the player's magic and combat send the player's graph goes through it, on
		// whatever thread sends it. Our own events to stand-ins go through Character's vtable, never this one.
		using NotifyFn = bool (*)(RE::IAnimationGraphManagerHolder*, const RE::BSFixedString&);
		NotifyFn originalNotify = nullptr;

		std::mutex               lock;  // outbox and last
		StandInRules::Outbox     outbox;
		std::string              last;
		std::atomic<bool>        recording{ false };
		std::atomic<bool>        hooked{ false };
		std::atomic<std::uint64_t> notOurs{ 0 };  // events through the hook of a holder that is not our player (none expected)

		bool NotifyHook(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
		{
			const bool taken = originalNotify(a_this, a_event);
			if (!taken || !recording.load(std::memory_order_relaxed)) {
				return taken;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || a_this != static_cast<RE::IAnimationGraphManagerHolder*>(player)) {
				notOurs.fetch_add(1, std::memory_order_relaxed);
				return taken;
			}
			const auto name = a_event.c_str();
			if (!name) {
				return taken;
			}
			const auto       now = Clock::now();
			std::scoped_lock l{ lock };
			if (outbox.Heard(name, Protocol::SentMs(now), now)) {
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
		const auto where = words[0];
		bool       done = false;
		if (where == "voice") {
			done = Gear::SetVoice(player, *id, false);
		} else if (where == "right" || where == "left" || where == "ammo") {
			done = Gear::EquipOne(player, form, where == "right" ? NpcRules::Hand::kRight : where == "left" ? NpcRules::Hand::kLeft : NpcRules::Hand::kNone);
		} else {
			return std::string{ USAGE };
		}
		if (!done) {
			return std::format("error: {:08X} can't go there (not carried, not known, or not that kind)", *id);
		}
		logs::info("Actions: our player's {} given {:08X} from the developer channel", where, *id);
		return std::format("equipped {} {:08X}", where, *id);
	}

	std::string Describe()
	{
		std::scoped_lock l{ lock };
		return std::format("actions hooked={} recording={} heard={} byKind={} sent={} dropped={} waiting={} notOurs={} last={}", hooked ? 1 : 0, recording ? 1 : 0,
			outbox.heard, outbox.byAct.Line(), outbox.sent, outbox.dropped, outbox.Waiting(), notOurs.load(), last.empty() ? "none" : last);
	}
}
