#include "game/WorldObjects.h"

#include "DevLine.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/LootSync.h"
#include "game/RefRules.h"
#include "game/RemotePlayers.h"
#include "game/Traps.h"
#include "game/UseRules.h"
#include "net/FormsCommand.h"

#include <atomic>
#include <mutex>

// windows.h names GetObject a macro (GetObjectA); a Papyrus variable's GetObject is CommonLib's own.
#ifdef GetObject
#	undef GetObject
#endif

namespace WorldObjects
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using RefState = Protocol::RefState;

		// An event the sinks queued, read on the next frame on the main thread.
		struct Event
		{
			enum class Kind : std::uint8_t
			{
				kOpenClose,
				kLock,
				kUse,
			};
			Kind        kind = Kind::kOpenClose;
			RE::FormID  ref = 0;
			bool        byOurPlayer = false;
			// Our player's use of a mechanism, read in the activate event's sink as it was made, before the mechanism's
			// script ran (UseRules: "what a use did"): when, what it lends, and the references near it enabled or not.
			bool                                         read = false;
			Clock::time_point                            at{};
			UseRules::LendPick                           pick;
			std::vector<std::pair<std::uint32_t, bool>>  baseline;
		};

		std::mutex         queueLock;
		std::vector<Event> queue;  // under queueLock
		// A frame's worth and more: past it a new event is dropped (the ones queued first are kept) and counted. A door
		// whose event was dropped is still read by the open-state scan (RefRules::OpenWatch) if it changes again.
		constexpr std::size_t MAX_QUEUED = 1024;
		std::uint64_t         queueDropped = 0;  // under queueLock

		std::uint32_t       ourId = 0;
		RefRules::Book      book;
		RefRules::Outbox    outbox;
		RefRules::Pending   pending;
		RefRules::Again     again;     // reports the server answered busy, to send again
		RefRules::Settle    settle;    // a friend's use made here: its script runs before the words it caused apply
		RefRules::OpenWatch watch;     // open states read near our player, for changes no event told of
		bool                askAgain = false;
		Clock::time_point   nextRetry{};
		Clock::time_point   nextScan{};

		struct Counts
		{
			std::uint64_t events = 0;    // queued by the sinks
			std::uint64_t sent = 0;      // reports sent
			std::uint64_t paced = 0;     // ... of them held a while first (RefRules::Outbox)
			std::uint64_t heard = 0;     // words of the session
			std::uint64_t applied = 0;   // ... brought a reference here to them
			std::uint64_t same = 0;      // ... found it so already
			std::uint64_t waited = 0;    // ... waited for the reference to load
			std::uint64_t stale = 0;     // ... our own, older than a report of ours still out (RefRules::Book)
			std::uint64_t usesSent = 0;  // our player's uses of a mechanism
			std::uint64_t usesMade = 0;  // a friend's, made here by their stand-in
			std::uint64_t usesRefused = 0;
			std::uint64_t asks = 0;      // every kept word asked for again (a load)
			std::uint64_t notTaken = 0;  // our reports the server answered with a refusal (busy, full, refused)
			std::uint64_t resent = 0;    // ... of them sent again (busy)
			std::uint64_t settled = 0;   // words held, and changes here not reported, while a friend's use settled
			std::uint64_t scanned = 0;   // open-state changes found by the scan, not by an event
			// game writes made off the game's main thread (the tick: GameThread.h); from SKSE's save message too
			std::atomic<std::uint64_t> offMain{ 0 };
		};
		Counts      counts;
		std::string lastRefused = "none";

		// ---- what a use did (UseRules; docs/DESIGN.md, "What a use did") ----
		std::uint32_t         useSeq = 0;  // our player's uses of a mechanism, numbered from 1
		UseRules::Capture     capture;     // what our player's uses did near their mechanisms
		UseRules::EnableWatch enables;     // ... the references near them enabled or not, read every scan
		// What friends' uses did, made here where our stand-in's use did not: from the tick, and from the animation hook
		// (on whichever thread sends an event: a script's, the main one), which holds back this game's own repeat of an
		// animation made here before the graph gets it; so under a lock. No game call is made while it is held.
		struct LockedMirror
		{
			struct MirrorCounts
			{
				std::uint64_t heard = 0;
				std::uint64_t matched = 0;
				std::uint64_t doubled = 0;
				std::size_t   waiting = 0;
			};

			void UseHeard(std::uint32_t a_player, std::uint32_t a_seq, Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				book.UseHeard(a_player, a_seq, a_now);
			}
			void MadeHere(std::uint32_t a_player, std::uint32_t a_seq)
			{
				std::lock_guard guard{ lock };
				book.MadeHere(a_player, a_seq);
			}
			void Heard(const Protocol::UseEffects& a_msg, Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				book.Heard(a_msg, a_now);
			}
			[[nodiscard]] std::vector<UseRules::Mirror::Make> Due(Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				return book.Due(a_now);
			}
			void Made(std::uint32_t a_ref, std::string_view a_event, Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				book.Made(a_ref, a_event, a_now);
			}
			void Unmade(std::uint32_t a_ref, std::string_view a_event)
			{
				std::lock_guard guard{ lock };
				book.Unmade(a_ref, a_event);
			}
			// The hook's: only while a friend's use is heard lately, an effect waits or one was made lately.
			[[nodiscard]] bool Repeat(std::uint32_t a_ref, std::string_view a_event, Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				return book.Listening(a_now) && book.Repeat(a_ref, a_event, a_now);
			}
			void Played(std::uint32_t a_ref, std::string_view a_event, Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				if (book.Listening(a_now)) {
					book.Played(a_ref, a_event, a_now);
				}
			}
			void OurUse()
			{
				std::lock_guard guard{ lock };
				book.OurUse();
			}
			[[nodiscard]] bool Listening(Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				return book.Listening(a_now);
			}
			[[nodiscard]] MirrorCounts Read()
			{
				std::lock_guard guard{ lock };
				return { book.heard, book.matched, book.doubled, book.Waiting() };
			}
			void Clear()
			{
				std::lock_guard guard{ lock };
				book.Clear();
			}

		private:
			std::mutex       lock;
			UseRules::Mirror book;
		};
		LockedMirror mirror;
		// Items lent to stand-ins for a friend's use: from the tick, from SKSE's save message (BeforeSave, on the main
		// thread) and from the container event's sink (which asks whether a stand-in lends what it moved); so under a lock.
		struct LockedLoans
		{
			void Lent(std::uint32_t a_standIn, const std::vector<Protocol::LentItem>& a_items, Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				book.Lent(a_standIn, a_items, a_now);
			}
			[[nodiscard]] bool Lends(std::uint32_t a_standIn, std::uint32_t a_base)
			{
				std::lock_guard guard{ lock };
				return book.Lends(a_standIn, a_base);
			}
			[[nodiscard]] std::uint32_t Moved(std::uint32_t a_standIn, std::uint32_t a_base, std::uint32_t a_count)
			{
				std::lock_guard guard{ lock };
				return book.Moved(a_standIn, a_base, a_count);
			}
			[[nodiscard]] std::vector<UseRules::Loan> Due(Clock::time_point a_now)
			{
				std::lock_guard guard{ lock };
				return book.Due(a_now);
			}
			[[nodiscard]] std::vector<UseRules::Loan> All()
			{
				std::lock_guard guard{ lock };
				return book.All();
			}
			[[nodiscard]] std::size_t Out()
			{
				std::lock_guard guard{ lock };
				return book.Out();
			}
			void Clear()
			{
				std::lock_guard guard{ lock };
				book.Clear();
			}

		private:
			std::mutex      lock;
			UseRules::Loans book;
		};
		LockedLoans        loans;     // items lent to stand-ins for a friend's use
		UseRules::AnimRing animRing;  // the last animation events read here, for the developer channel
		Clock::time_point     nextEnableScan{};
		// The developer channel's `refs refuse on`: every friend's use refused here, as a script that does nothing for a
		// stand-in would; what the use did then comes by its effects alone (the rig's leg for that).
		bool refuseUses = false;

		// Atomics: the tick and SKSE's save message (BeforeSave, on the main thread) both count the loans taken back.
		struct UseCounts
		{
			using N = std::atomic<std::uint64_t>;
			N lentSent{ 0 };       // items our uses lent (said with the use)
			N keptNamed{ 0 };      // ... named and held, of a kind never lent (an NPC would equip or use it)
			N lentHere{ 0 };       // items lent here to a stand-in for a friend's use
			N lentBack{ 0 };       // ... taken back
			N lentGone{ 0 };       // ... not there to take back (its stand-in gone, or the script took it)
			N lentMoved{ 0 };      // ... handed on by the stand-in's run of the script (into a holder, dropped, used up)
			N lentUndone{ 0 };     // ... of them taken back out of a holder the session shares (loot brings the user's)
			N effectBatches{ 0 };  // what our uses did, batches sent
			N made{ 0 };           // a friend's use's effects made here
			N already{ 0 };        // ... found so here already (an enable or disable)
			N unloaded{ 0 };       // ... on a reference not loaded here (not made)
			N refusedKind{ 0 };    // ... an enable or disable this game does not make of that kind (a carryable's disable)
			N notes{ 0 };          // animation events read (while a use was watched or made here)
			N early{ 0 };          // ... of them read before the tick began the use's watch (the sink's start counts)
		};
		UseCounts useCounts;

		// The last time our player used a mechanism (the activate event's sink, Clock ticks; 0 for never): the hook reads
		// animation events for LISTEN_GRACE after it, whether or not the tick has set `listening` yet.
		std::atomic<Clock::rep> ownUseAt{ 0 };
		constexpr auto          LISTEN_GRACE = std::chrono::seconds(5);

		bool OwnUseRecent(Clock::time_point a_now)
		{
			const auto at = ownUseAt.load(std::memory_order_relaxed);
			return at != 0 && a_now.time_since_epoch().count() - at < std::chrono::duration_cast<Clock::duration>(LISTEN_GRACE).count();
		}

		// Animation events sent to a plain reference's graph, queued by the hook (below) for the next frame.
		struct AnimNote
		{
			RE::FormID        ref = 0;
			std::string       event;
			Clock::time_point at{};
		};
		std::mutex            notesLock;
		std::vector<AnimNote> notes;             // under notesLock
		std::uint64_t         notesDropped = 0;  // under notesLock
		constexpr std::size_t MAX_NOTES = 512;
		std::atomic<bool>     listening{ false };  // a use is watched, or a friend's is being made here
		thread_local bool     making = false;      // the events our own Make sends: not the game's doing

		// IAnimationGraphManagerHolder::NotifyAnimationGraph (its vtable's slot 1) of a plain reference: TESObjectREFR's
		// own vtable, which activators, doors, containers, statics and movable statics have (actors and projectiles have
		// their own). An animation event sent to the reference's graph, by a script (PlayAnimation) or by the engine. Only
		// while `listening` (or just after our player's use, which the activate event's sink marks before the mechanism's
		// script runs): this game's own repeat of an animation made here for a friend's use is not passed to the graph
		// (UseRules::Mirror::Repeat; answered as not taken, so a script's PlayAnimationAndWait does not wait for it), and
		// what the graph took is queued (the caller's thread is any: a script's, the main one) for the frame to read on
		// the tick.
		struct NotifyHook
		{
			static bool Thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
			{
				if (!a_this || making) {
					return Original(a_this, a_event);
				}
				const auto now = Clock::now();
				if (!listening.load(std::memory_order_relaxed) && !OwnUseRecent(now)) {
					return Original(a_this, a_event);
				}
				const char* name = a_event.c_str();
				if (!name || name[0] == '\0') {
					return Original(a_this, a_event);
				}
				const auto id = static_cast<RE::TESObjectREFR*>(a_this)->GetFormID();
				if (mirror.Repeat(id, name, now)) {
					return false;  // made here already for a friend's use: the graph never plays it twice
				}
				const bool taken = Original(a_this, a_event);
				if (taken) {
					mirror.Played(id, name, now);
					std::lock_guard guard{ notesLock };
					if (notes.size() < MAX_NOTES) {
						notes.push_back({ id, name, now });
					} else {
						++notesDropped;
					}
				}
				return taken;
			}
			static inline REL::Relocation<decltype(Thunk)> Original;
		};
		bool hooked = false;

		std::vector<AnimNote> TakeNotes()
		{
			std::lock_guard guard{ notesLock };
			return std::exchange(notes, {});
		}

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		// The distance between two references in the same place (a cell's interior, or one worldspace outside); -1
		// when they are not (a distance between places means nothing).
		float Distance(const RE::TESObjectREFR* a_a, const RE::TESObjectREFR* a_b)
		{
			if (!a_a || !a_b) {
				return -1.0f;
			}
			const auto cellA = a_a->GetParentCell();
			const auto cellB = a_b->GetParentCell();
			if (!cellA || !cellB) {
				return -1.0f;
			}
			if (cellA->IsInteriorCell() || cellB->IsInteriorCell()) {
				if (cellA != cellB) {
					return -1.0f;
				}
			} else if (a_a->GetWorldspace() != a_b->GetWorldspace()) {
				return -1.0f;
			}
			return a_a->GetPosition().GetDistance(a_b->GetPosition());
		}

		RefRules::Facts FactsOf(RE::TESObjectREFR* a_ref)
		{
			RefRules::Facts facts;
			const auto      base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base) {
				return facts;
			}
			// The record's "is marker" flag (bit 23 of a base's flags): an editor marker nobody sees.
			facts.marker = (base->GetFormFlags() & (1u << 23)) != 0;
			switch (base->GetFormType()) {
			case RE::FormType::Door:
				{
					facts.base = RefRules::Base::kDoor;
					facts.loadDoor = a_ref->extraList.GetByType<RE::ExtraTeleport>() != nullptr;
					const auto door = base->As<RE::TESObjectDOOR>();
					facts.automatic = door && door->flags.any(RE::TESObjectDOOR::Flag::kAutomatic);
					break;
				}
			case RE::FormType::Activator:
				{
					facts.base = RefRules::Base::kActivator;
					const auto name = base->GetName();
					facts.named = name && name[0] != '\0';
					break;
				}
			case RE::FormType::Container:
				facts.base = RefRules::Base::kContainer;
				break;
			default:
				break;
			}
			return facts;
		}

		// What the reference reads here, whole: its open state (none for what has none) and its lock (a reference with
		// no lock reads unlocked, its level not known).
		RefState Read(RE::TESObjectREFR* a_ref)
		{
			RefState state;
			state.refId = a_ref->GetFormID();
			state.open = RefRules::OpenOf(static_cast<int>(RE::BGSOpenCloseForm::GetOpenState(a_ref)));
			if (const auto lock = a_ref->GetLock()) {
				state.locked = lock->IsLocked() ? RefState::kYes : RefState::kNo;
				// locked: the game's own level (a leveled lock scaled to this player); unlocked: the lock's own number,
				// unless it is leveled (its level then depends on the player, so it is not said)
				if (lock->IsLocked()) {
					state.lockLevel = RefRules::LevelOf(static_cast<int>(lock->GetLockLevel(a_ref)));
				} else if (!lock->flags.any(RE::REFR_LOCK::Flag::kLeveled)) {
					state.lockLevel = RefRules::LevelOfBase(lock->baseLevel);
				}
			} else {
				state.locked = RefState::kNo;
			}
			return state;
		}

		RE::TESObjectREFR* Find(RE::FormID a_id)
		{
			return a_id != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id) : nullptr;
		}

		// The game writes here (the lock, the open state, a stand-in's use) are made from the tick, which until the frame
		// hook is written runs on SKSE's task queue: the earlier rig runs saw it on the task pool's job threads once in
		// the world, not on the game's main thread (src/game/GameThread.h). Each write made off the main thread is
		// counted (`refs`: offmain=) and said once in the log, so the rig shows how they are made; the frame hook moves
		// them to the main thread.
		void NoteThread(std::string_view a_what)
		{
			if (GameThread::IsLoadThread()) {
				return;
			}
			if (counts.offMain++ == 0) {
				logs::info("World: {} made off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); "
						   "counted in `refs` as offmain",
					a_what);
			}
		}

		// Brings a loaded reference to the session's word; false when it is not loaded here (it waits).
		bool Apply(const RefState& a_want)
		{
			const auto ref = Find(a_want.refId);
			if (!ref || !ref->Is3DLoaded() || ref->IsDisabled() || ref->IsDeleted()) {
				return false;
			}
			const auto shares = RefRules::Of(FactsOf(ref));
			const auto change = RefRules::Plan(a_want, RefRules::Shared(Read(ref), shares));
			if (!change.Any()) {
				++counts.same;
				return true;
			}
			NoteThread("a door's lock or open state written");
			if (change.locked || change.level) {
				auto lock = ref->GetLock();
				if (!lock && change.locked.value_or(false)) {
					lock = ref->AddLock();
				}
				if (lock) {
					if (change.level) {
						lock->baseLevel = RefRules::BaseOfLevel(*change.level);
						lock->flags.reset(RE::REFR_LOCK::Flag::kLeveled);
					}
					if (change.locked) {
						lock->SetLocked(*change.locked);
					}
					ref->AddLockChange();  // kept in this game's save, as a lock Papyrus changes is
				}
			}
			if (change.open) {
				RE::BGSOpenCloseForm::SetOpenState(ref, *change.open, false);
			}
			++counts.applied;
			logs::info("World: {:08X} ({}) brought to the session's word:{}{}{}", ref->GetFormID(), RefRules::KindName(FactsOf(ref)),
				change.open ? (*change.open ? " opened" : " closed") : "", change.locked ? (*change.locked ? " locked" : " unlocked") : "",
				change.level ? std::format(" level {}", *change.level) : "");
			return true;
		}

		// ---- what a use did: the game side of UseRules ----

		UseRules::ItemKind KindOf(const RE::TESForm* a_form)
		{
			using Kind = UseRules::ItemKind;
			switch (a_form ? a_form->GetFormType() : RE::FormType::None) {
			case RE::FormType::Misc:
				return Kind::kMisc;
			case RE::FormType::KeyMaster:
				return Kind::kKey;
			case RE::FormType::SoulGem:
				return Kind::kSoulGem;
			case RE::FormType::Book:
				return Kind::kBook;
			case RE::FormType::Ingredient:
				return Kind::kIngredient;
			case RE::FormType::AlchemyItem:
				return Kind::kPotion;
			case RE::FormType::Weapon:
			case RE::FormType::Armor:
			case RE::FormType::Ammo:
			case RE::FormType::Scroll:
			case RE::FormType::Light:
				return Kind::kEquippable;
			default:
				return Kind::kNotItem;
			}
		}

		// How many of a_base our player holds now.
		std::int64_t Held(RE::TESObjectREFR* a_holder, RE::TESBoundObject* a_base)
		{
			if (!a_holder || !a_base) {
				return 0;
			}
			const auto counts = a_holder->GetInventoryCounts([&](RE::TESBoundObject& a_object) { return &a_object == a_base; });
			const auto it = counts.find(a_base);
			return it != counts.end() ? it->second : 0;
		}

		// The forms a reference's Papyrus scripts name in their variables (properties' too): each one an item, a list of
		// forms (each of them), or a reference alias (the base of the reference it holds); arrays of them as well. What a
		// mechanism's script may check its user for (a claw door's claw, a keyhole's key). At most 64 forms.
		std::vector<RE::TESForm*> ScriptForms(RE::TESObjectREFR* a_ref)
		{
			using namespace RE::BSScript;
			std::vector<RE::TESForm*> out;
			const auto vm = Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!a_ref || !policy) {
				return out;
			}
			constexpr std::size_t MAX_FORMS = 64;
			const auto add = [&](RE::TESForm* a_form) {
				if (a_form && out.size() < MAX_FORMS && std::ranges::find(out, a_form) == out.end()) {
					out.push_back(a_form);
				}
			};
			constexpr RE::FormType ITEM_TYPES[] = { RE::FormType::Misc, RE::FormType::KeyMaster, RE::FormType::SoulGem, RE::FormType::Book,
				RE::FormType::Ingredient, RE::FormType::AlchemyItem, RE::FormType::Weapon, RE::FormType::Armor, RE::FormType::Ammo,
				RE::FormType::Scroll, RE::FormType::Light };
			// One script object a variable holds: an item, a list, or an alias's reference's base.
			const auto take = [&](const RE::BSTSmartPointer<Object>& a_object) {
				if (!a_object) {
					return;
				}
				const auto handle = a_object->GetHandle();
				for (const auto type : ITEM_TYPES) {
					if (policy->HandleIsType(static_cast<RE::VMTypeID>(type), handle)) {
						add(policy->GetObjectForHandle(type, handle));
						return;
					}
				}
				if (policy->HandleIsType(static_cast<RE::VMTypeID>(RE::FormType::FormList), handle)) {
					if (const auto list = policy->GetObjectForHandle(RE::FormType::FormList, handle); list && list->Is(RE::FormType::FormList)) {
						const auto flst = static_cast<RE::BGSListForm*>(list);
						for (const auto form : flst->forms) {
							add(form);
						}
						if (flst->scriptAddedTempForms) {
							for (const auto id : *flst->scriptAddedTempForms) {
								add(RE::TESForm::LookupByID(id));
							}
						}
					}
					return;
				}
				if (policy->HandleIsType(RE::BGSRefAlias::VMTYPEID, handle)) {
					const auto alias = static_cast<RE::BGSRefAlias*>(policy->GetObjectForHandle(RE::BGSRefAlias::VMTYPEID, handle));
					const auto held = alias ? alias->GetReference() : nullptr;
					add(held ? held->GetBaseObject() : nullptr);
				}
			};
			const auto handle = policy->GetHandleForObject(a_ref->GetFormType(), a_ref);
			RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
			const auto it = vm->attachedScripts.find(handle);
			if (it == vm->attachedScripts.end()) {
				return out;
			}
			for (const auto& script : it->second) {
				const auto object = script.get();
				const auto type = object ? object->GetTypeInfo() : nullptr;
				const auto vars = type ? type->GetVariableIter() : nullptr;
				for (std::uint32_t i = 0; vars && i < type->GetNumVariables(); ++i) {
					const auto value = object->GetVariable(vars[i].name);
					if (!value) {
						continue;
					}
					if (value->IsObject()) {
						take(value->GetObject());
					} else if (value->IsObjectArray()) {
						if (const auto array = value->GetArray()) {
							for (std::uint32_t k = 0; k < array->size() && k < MAX_FORMS; ++k) {
								if ((*array)[k].IsObject()) {
									take((*array)[k].GetObject());
								}
							}
						}
					}
				}
			}
			return out;
		}

		// What our player's use of a_ref lends: the items its scripts name that our player holds (UseRules::PickLent).
		UseRules::LendPick LendFor(RE::TESObjectREFR* a_ref)
		{
			std::vector<UseRules::Named> named;
			for (const auto form : ScriptForms(a_ref)) {
				const auto kind = KindOf(form);
				if (kind == UseRules::ItemKind::kNotItem || !RefRules::Shareable(form->GetFormID())) {
					continue;  // not an item, or one made as the game ran (no other game has it)
				}
				named.push_back({ form->GetFormID(), kind, Held(Player(), form->As<RE::TESBoundObject>()) });
			}
			return UseRules::PickLent(named);
		}

		// A friend's lent items, given to their stand-in for its use here (taken back by Loans).
		void Lend(RE::Actor* a_standIn, const std::vector<Protocol::LentItem>& a_items)
		{
			std::vector<Protocol::LentItem> given;
			for (const auto& item : a_items) {
				const auto base = RE::TESForm::LookupByID<RE::TESBoundObject>(item.base);
				if (!base || !UseRules::Lendable(KindOf(base))) {
					continue;  // checked again here: a session's word never makes a stand-in hold a weapon
				}
				a_standIn->AddObjectToContainer(base, nullptr, item.count, nullptr);
				given.push_back(item);
				useCounts.lentHere += item.count;
			}
			if (!given.empty()) {
				loans.Lent(a_standIn->GetFormID(), given, Clock::now());
			}
		}

		void TakeBack(const std::vector<UseRules::Loan>& a_loans)
		{
			for (const auto& loan : a_loans) {
				const auto standIn = RE::TESForm::LookupByID<RE::TESObjectREFR>(loan.standIn);
				const auto base = RE::TESForm::LookupByID<RE::TESBoundObject>(loan.base);
				if (!standIn || !base || standIn->IsDeleted()) {
					useCounts.lentGone += loan.count;
					continue;
				}
				const auto back = UseRules::Loans::Back(loan, Held(standIn, base));
				if (back > 0) {
					NoteThread("an item lent to a stand-in taken back");
					standIn->RemoveItem(base, back, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				}
				useCounts.lentBack += static_cast<std::uint64_t>(back);
				useCounts.lentGone += loan.count - static_cast<std::uint64_t>(back);
			}
		}

		// A stand-in's move of an item it holds on loan (the container event's sink queues it; the tick or the save
		// message reads it): the stand-in's run of the mechanism's script handed it on.
		struct LentMove
		{
			std::uint32_t standIn = 0;
			std::uint32_t base = 0;
			std::uint32_t count = 0;
			std::uint32_t to = 0;       // the holder it went into; 0 for none
			bool          placed = false;  // to none, as a reference placed in the world (a drop)
		};
		std::mutex            movesLock;
		std::vector<LentMove> lentMoves;  // under movesLock
		constexpr std::size_t MAX_LENT_MOVES = 256;

		// Each lent item a stand-in's run handed on, out of the loans (not taken back from the stand-in later), and
		// taken back out of the holder it went into when that holder's moves are the session's: the user's game says its
		// own move there through loot sync, so the holder here gets it from there (UseRules::LentWent).
		void ReadLentMoves()
		{
			std::vector<LentMove> moves;
			{
				std::lock_guard guard{ movesLock };
				moves = std::exchange(lentMoves, {});
			}
			for (const auto& move : moves) {
				const auto lent = loans.Moved(move.standIn, move.base, move.count);
				if (lent == 0) {
					continue;  // the stand-in's own item of that base, or a loan already taken back
				}
				useCounts.lentMoved += lent;
				const auto went = move.to == 0 ? (move.placed ? UseRules::LentWent::kDropped : UseRules::LentWent::kUsedUp) :
				                  LootSync::SharesPut(move.to, move.base) ? UseRules::LentWent::kSharedHolder :
				                                                             UseRules::LentWent::kOwnHolder;
				std::int32_t undone = 0;
				if (UseRules::TakeOutOfHolder(went)) {
					const auto holder = Find(move.to);
					const auto base = RE::TESForm::LookupByID<RE::TESBoundObject>(move.base);
					undone = static_cast<std::int32_t>(std::clamp<std::int64_t>(Held(holder, base), 0, lent));
					if (undone > 0) {
						NoteThread("an item lent to a stand-in taken back out of the holder its script put it in");
						holder->RemoveItem(base, undone, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
						useCounts.lentUndone += static_cast<std::uint64_t>(undone);
					}
				}
				logs::info("World: {} lent {:08X} handed on by the stand-in {:08X}'s use: {}{}{}", lent, move.base, move.standIn, UseRules::LentWentName(went),
					move.to != 0 ? std::format(" {:08X}", move.to) : std::string{},
					UseRules::TakeOutOfHolder(went) ? std::format("; {} taken back out of it (the user's own put comes by loot)", undone) : std::string{});
			}
		}

		// Whether an animation event on a_ref can be a use's effect: an activator's or a movable static's (a claw door, a
		// pillar, a gate built as one), or the used mechanism's own; not a reference whose open state is shared as a
		// door's (its animation follows its open state in every game), nor an actor's (they have their own vtable).
		bool AnimEffectOf(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || a_ref->IsDeleted()) {
				return false;
			}
			const auto shares = RefRules::Of(FactsOf(a_ref));
			if (shares.open && RefRules::OpenOf(static_cast<int>(RE::BGSOpenCloseForm::GetOpenState(a_ref))) != RefState::kUnknown) {
				return false;
			}
			const auto type = base->GetFormType();
			return type == RE::FormType::Activator || type == RE::FormType::MovableStatic;
		}

		// What a_ref is, for its enabling or disabling as a use's effect (UseRules::EnableSays): a placed reference of a
		// plugin that is not an actor (NPC sync's) nor our player is fixed (a wall, a bridge, a light that can't be
		// carried) or carryable (an item, a light that can be: only its enabling is an effect, its disabling is most often
		// a pickup, loot's and drops').
		UseRules::Placed PlacedOf(RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || a_ref == Player() || !RefRules::Shareable(a_ref->GetFormID()) || a_ref->IsDeleted() || a_ref->Is(RE::FormType::ActorCharacter)) {
				return UseRules::Placed::kNot;
			}
			if (const auto light = base->As<RE::TESObjectLIGH>()) {
				return light->CanBeCarried() ? UseRules::Placed::kCarryable : UseRules::Placed::kFixed;
			}
			const bool item = KindOf(base) != UseRules::ItemKind::kNotItem || base->Is(RE::FormType::Note) || base->Is(RE::FormType::Apparatus);
			return item ? UseRules::Placed::kCarryable : UseRules::Placed::kFixed;
		}

		// The references within a use's reach of a_mechanism whose enabling can be an effect, each enabled or not (as
		// the sink reads them at a use, and every scan of a watched use does).
		std::vector<std::pair<std::uint32_t, bool>> EnableReads(RE::TESObjectREFR* a_mechanism)
		{
			std::vector<std::pair<std::uint32_t, bool>> out;
			const auto tes = RE::TES::GetSingleton();
			if (!tes || !a_mechanism || !a_mechanism->Is3DLoaded()) {
				return out;
			}
			tes->ForEachReferenceInRange(a_mechanism, UseRules::EFFECT_REACH, [&](RE::TESObjectREFR* a_ref) {
				if (PlacedOf(a_ref) != UseRules::Placed::kNot && Distance(a_mechanism, a_ref) >= 0.0f) {
					out.emplace_back(a_ref->GetFormID(), a_ref->IsDisabled());
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return out;
		}

		// The tick time the newest use's watch began at: animation events read before it were caught only because the
		// activate event's sink began listening (counted as early).
		Clock::time_point lastBegunAt{};

		// The animation events read since the last frame: each to the ring, and, while our player's use is watched, to the
		// use whose mechanism is nearest in time and within reach. (The mirror's matches and repeats are read by the hook
		// itself, as each is sent.)
		void ReadNotes(Clock::time_point a_now)
		{
			auto read = TakeNotes();
			std::uint64_t dropped = 0;
			{
				std::lock_guard guard{ notesLock };
				dropped = std::exchange(notesDropped, 0);
			}
			if (dropped != 0) {
				logs::info("World: {} animation events read past {} in a frame were not read", dropped, MAX_NOTES);
			}
			if (read.empty()) {
				return;
			}
			const auto watched = capture.Watched(a_now);
			for (const auto& note : read) {
				++useCounts.notes;
				animRing.Add(note.ref, note.event, false, note.at);
				if (watched.empty() || !RefRules::Shareable(note.ref)) {
					continue;
				}
				const auto ref = Find(note.ref);
				if (!ref || Traps::IsTrap(ref)) {
					continue;  // a trap's animation is its own word's (src/game/Traps.h), never a use's effect
				}
				for (const auto& use : watched) {
					const auto mechanism = Find(use.mechanism);
					const auto distance = Distance(mechanism, ref);
					if (note.ref == use.mechanism || (distance >= 0.0f && distance <= UseRules::EFFECT_REACH && AnimEffectOf(ref))) {
						if (capture.Add(use.seq, note.ref, Protocol::UseEffect::kAnim, note.event, note.at) && note.at < lastBegunAt) {
							++useCounts.early;
						}
						break;
					}
				}
			}
		}

		// The references near each watched use, enabled or not: a change since the last read (the sink's at the use, then
		// each scan's) is that use's effect, when its kind says it (UseRules::EnableSays). One whose enable parent changed
		// too follows its parent (the engine moves it), so only the parent is said.
		void ScanEnables(Clock::time_point a_now)
		{
			const auto watched = capture.Watched(a_now);
			if (watched.empty()) {
				enables.Clear();
				return;
			}
			struct Change
			{
				std::uint32_t      seq;
				RE::TESObjectREFR* ref;
				bool               disabled;
			};
			std::vector<Change> changed;
			enables.BeginScan();
			for (const auto& use : watched) {
				for (const auto& [id, disabled] : EnableReads(Find(use.mechanism))) {
					if (enables.Seen(id, disabled)) {
						if (const auto ref = Find(id)) {
							changed.push_back({ use.seq, ref, disabled });
						}
					}
				}
			}
			enables.EndScan();
			for (const auto& change : changed) {
				const auto parent = change.ref->extraList.GetByType<RE::ExtraEnableStateParent>();
				const auto parentRef = parent ? parent->parent.get() : RE::NiPointer<RE::TESObjectREFR>{};
				if (parentRef && std::ranges::any_of(changed, [&](const Change& a_c) { return a_c.ref == parentRef.get(); })) {
					continue;
				}
				if (!UseRules::EnableSays(PlacedOf(change.ref), change.disabled)) {
					continue;  // a carryable item gone: a pickup, loot's and drops'
				}
				if (Traps::IsTrap(change.ref)) {
					continue;  // a trap disabled (a magic trap's gem, a breakaway floor): its own word's
				}
				capture.Add(change.seq, change.ref->GetFormID(), change.disabled ? Protocol::UseEffect::kDisable : Protocol::UseEffect::kEnable, {}, a_now);
			}
		}

		// A friend's use's effects that are due: made here, each where this game's own run of the use did not.
		void MakeEffects(Clock::time_point a_now)
		{
			for (const auto& make : mirror.Due(a_now)) {
				const auto& effect = make.effect;
				const auto  ref = Find(effect.ref);
				const auto  cell = ref ? ref->GetParentCell() : nullptr;
				if (!ref || ref->IsDeleted() || !cell || !cell->IsAttached()) {
					++useCounts.unloaded;
					continue;
				}
				if (Traps::IsTrap(ref)) {
					++useCounts.refusedKind;  // a trap is made by its own word (src/game/Traps.h), never by a use's effect
					continue;
				}
				if (effect.what == Protocol::UseEffect::kAnim) {
					if (!ref->Is3DLoaded() || ref->IsDisabled()) {
						++useCounts.unloaded;
						continue;
					}
					NoteThread("a friend's use's animation played");
					mirror.Made(effect.ref, effect.event, a_now);  // before it is sent: a repeat racing it is held back too
					making = true;
					const bool taken = ref->NotifyAnimationGraph(RE::BSFixedString{ effect.event.c_str() });
					making = false;
					if (!taken) {
						mirror.Unmade(effect.ref, effect.event);  // nothing played: nothing to hold back
					}
					animRing.Add(effect.ref, effect.event, true, a_now);
					++useCounts.made;
					logs::info("World: player {}'s use {} played {:08X}'s '{}' in their game; played here{}", make.player, make.seq, effect.ref, effect.event,
						taken ? "" : " (its graph did not take it)");
					continue;
				}
				const bool wantDisabled = effect.what == Protocol::UseEffect::kDisable;
				if (!UseRules::EnableSays(PlacedOf(ref), wantDisabled)) {
					++useCounts.refusedKind;  // checked again here: a session's word never takes away an item lying here
					continue;
				}
				if (ref->IsDisabled() == wantDisabled) {
					++useCounts.already;
					continue;
				}
				NoteThread("a friend's use's enable or disable made");
				if (wantDisabled) {
					ref->Disable();
				} else {
					ref->Enable(false);
				}
				++useCounts.made;
				logs::info("World: player {}'s use {} {} {:08X} in their game; {} here", make.player, make.seq, wantDisabled ? "disabled" : "enabled", effect.ref,
					wantDisabled ? "disabled" : "enabled");
			}
		}

		void Report(const RefState& a_state, const SendFn& a_send)
		{
			book.Sent(a_state);
			a_send(Protocol::Encode(a_state));
			++counts.sent;
		}

		// A state read here, cut to the parts a change was about: reported if the session doesn't know it, now or once
		// its reference's pace allows.
		void Offer(const RefState& a_read, const SendFn& a_send, Clock::time_point a_now)
		{
			if (!book.News(a_read)) {
				return;
			}
			if (outbox.Offer(a_read, a_now)) {
				Report(a_read, a_send);
			} else {
				++counts.paced;
			}
		}

		// A queued event, read now: the part of the reference it is about, reported if the session doesn't know it.
		void Look(const Event& a_event, const SendFn& a_send, Clock::time_point a_now)
		{
			if (!RefRules::Shareable(a_event.ref)) {
				return;
			}
			const auto ref = Find(a_event.ref);
			if (!ref || !ref->Is3DLoaded()) {
				return;
			}
			if (Traps::IsTrap(ref)) {
				return;  // a trap's state and its use are its own word's (src/game/Traps.h): never a door's or a mechanism's
			}
			const auto facts = FactsOf(ref);
			const auto shares = RefRules::Of(facts);
			if (a_event.kind == Event::Kind::kUse) {
				if (shares.use && a_event.byOurPlayer) {
					// The items its scripts name that our player held as it used it go with it (lent to the stand-in that
					// makes it in the others), and what it does near it is watched from the moment it was made (its
					// effects, said after). Both were read in the activate event's sink, before the mechanism's script ran
					// (a script that takes the item, plays an animation or disables a wall in that frame is caught); read
					// now only when the sink did not.
					const auto pick = a_event.read ? a_event.pick : LendFor(ref);
					const auto at = a_event.read ? a_event.at : a_now;
					Protocol::Activation use{ .refId = a_event.ref, .seq = ++useSeq, .lent = pick.lent };
					a_send(Protocol::Encode(use));
					++counts.usesSent;
					for (const auto& item : pick.lent) {
						useCounts.lentSent += item.count;
					}
					useCounts.keptNamed += pick.kept;
					capture.Begin(use.seq, a_event.ref, at);
					lastBegunAt = a_now;
					listening.store(true, std::memory_order_relaxed);
					for (const auto& [id, disabled] : a_event.baseline) {
						enables.Baseline(id, disabled);
					}
					nextEnableScan = a_now + RefRules::SCAN_EVERY;
					ScanEnables(a_now);  // what changed since the sink's read (else the first read)
					logs::info("World: our player used {:08X} ({}), use {}; the others' stand-ins of them use it too{}", a_event.ref, RefRules::KindName(facts), use.seq,
						pick.lent.empty() ? "" : std::format(", holding the {} item(s) its scripts name that our player holds", pick.lent.size()));
					if (pick.kept != 0 && useCounts.keptNamed == pick.kept) {
						logs::info("World: {} item(s) the mechanism's scripts name are of a kind never lent to a stand-in (an NPC would equip or use them)", pick.kept);
					}
				}
				return;
			}
			// Only the part the event is about: the rest of what the reference reads here is this game's own.
			const auto part = a_event.kind == Event::Kind::kLock ? RefRules::Part::kLock : RefRules::Part::kOpen;
			const auto within = RefRules::Within(shares, part);
			if (!within.open && !within.lock) {
				return;
			}
			if (part == RefRules::Part::kOpen && !a_event.byOurPlayer && settle.Active(a_now)) {
				++counts.settled;  // the replayed script's doing: the user's game reports what it did
				return;
			}
			if (!RefRules::Reports(a_event.byOurPlayer, Distance(Player(), ref))) {
				return;
			}
			Offer(RefRules::Shared(Read(ref), within), a_send, a_now);
		}

		// A held or refused report read again: the parts it was about, as the reference reads them now.
		void LookAgain(const RefState& a_parts, const SendFn& a_send, Clock::time_point a_now)
		{
			const auto ref = Find(a_parts.refId);
			if (!ref || !ref->Is3DLoaded()) {
				return;
			}
			Offer(RefRules::Only(RefRules::Shared(Read(ref), RefRules::Of(FactsOf(ref))), a_parts), a_send, a_now);
		}

		// The open states near our player, read again: a change no event told of is taken as one (RefRules::OpenWatch).
		void Scan(const SendFn& a_send, Clock::time_point a_now)
		{
			const auto player = Player();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return;
			}
			std::vector<RE::FormID> changed;
			watch.BeginScan();
			tes->ForEachReferenceInRange(player, RefRules::REPORT_REACH, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && a_ref != player && RefRules::Shareable(a_ref->GetFormID()) && a_ref->Is3DLoaded() && RefRules::Of(FactsOf(a_ref)).open) {
					const auto open = RefRules::OpenOf(static_cast<int>(RE::BGSOpenCloseForm::GetOpenState(a_ref)));
					if (open != RefState::kUnknown && watch.Seen(a_ref->GetFormID(), open)) {
						changed.push_back(a_ref->GetFormID());
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			watch.EndScan();
			for (const auto id : changed) {
				const auto before = counts.sent + counts.paced;
				Look({ .kind = Event::Kind::kOpenClose, .ref = id, .byOurPlayer = false }, a_send, a_now);
				if (counts.sent + counts.paced != before) {
					++counts.scanned;  // news no event told of (one an event told of is no news by now)
				}
			}
		}

		std::vector<Event> TakeQueue()
		{
			std::lock_guard guard{ queueLock };
			return std::exchange(queue, {});
		}

		void Queue(Event a_event)
		{
			std::lock_guard guard{ queueLock };
			if (queue.size() < MAX_QUEUED) {
				queue.push_back(std::move(a_event));
			} else {
				++queueDropped;
			}
		}

		// Our player's use of a_ref, read as it is made (the activate event's sink, inside the game's activation and
		// before the mechanism's script gets its OnActivate): from now the animation hook reads what is sent, and what it
		// lends and the enabled states near it are read before the script can change them. Only for a mechanism whose use
		// is shared (as Look checks again on the tick).
		void ReadUse(RE::TESObjectREFR* a_ref, Event& a_event)
		{
			if (!a_ref || !RefRules::Shareable(a_ref->GetFormID()) || !a_ref->Is3DLoaded() || !RefRules::Of(FactsOf(a_ref)).use) {
				return;
			}
			a_event.at = Clock::now();
			ownUseAt.store(a_event.at.time_since_epoch().count(), std::memory_order_relaxed);
			mirror.OurUse();
			a_event.pick = LendFor(a_ref);
			a_event.baseline = EnableReads(a_ref);
			a_event.read = true;
		}

		// A stand-in's move of an item: queued when it holds a loan of it (ReadLentMoves).
		void QueueLentMove(const RE::TESContainerChangedEvent& a_event)
		{
			if (!loans.Lends(a_event.oldContainer, a_event.baseObj)) {
				return;
			}
			const bool placed = a_event.newContainer == 0 && static_cast<bool>(a_event.reference.get());
			std::lock_guard guard{ movesLock };
			if (lentMoves.size() < MAX_LENT_MOVES) {
				lentMoves.push_back({ a_event.oldContainer, a_event.baseObj, static_cast<std::uint32_t>(a_event.itemCount), a_event.newContainer, placed });
			}
		}

		bool IsOurPlayer(const RE::TESObjectREFR* a_ref)
		{
			return a_ref && a_ref == Player();
		}

		class Sinks :
			public RE::BSTEventSink<RE::TESOpenCloseEvent>,
			public RE::BSTEventSink<RE::TESLockChangedEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>,
			public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESOpenCloseEvent* a_event, RE::BSTEventSource<RE::TESOpenCloseEvent>*) override
			{
				if (a_event && a_event->ref) {
					Queue({ .kind = Event::Kind::kOpenClose, .ref = a_event->ref->GetFormID(), .byOurPlayer = IsOurPlayer(a_event->activeRef.get()) });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESLockChangedEvent* a_event, RE::BSTEventSource<RE::TESLockChangedEvent>*) override
			{
				if (a_event && a_event->lockedObject) {
					// The event names no cause: a lock picked, opened with a key or changed by a script near our player
					// all count (RefRules::Reports by distance), our player's own lockpicking among them.
					Queue({ .kind = Event::Kind::kLock, .ref = a_event->lockedObject->GetFormID() });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				// Only our player's uses: a stand-in's use (a friend's, made here) or an NPC's is not reported.
				if (a_event && a_event->objectActivated && IsOurPlayer(a_event->actionRef.get())) {
					const auto ref = a_event->objectActivated.get();
					Event use{ .kind = Event::Kind::kUse, .ref = ref ? ref->GetFormID() : 0, .byOurPlayer = true };
					ReadUse(ref, use);
					Queue(std::move(use));
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// A stand-in handing on an item lent to it for a friend's use (its run of the mechanism's script did):
				// anything else moved, by anyone, is not this class's.
				if (a_event && a_event->oldContainer != 0 && a_event->oldContainer != a_event->newContainer && a_event->baseObj != 0 && a_event->itemCount > 0) {
					QueueLentMove(*a_event);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sinks sinks;
		bool  installed = false;

		std::uint32_t Counted(std::uint64_t a_n)
		{
			return static_cast<std::uint32_t>((std::min<std::uint64_t>)(a_n, 0xFFFFFFFF));
		}

		std::string Line(RE::TESObjectREFR* a_ref)
		{
			const auto facts = FactsOf(a_ref);
			const auto read = Read(a_ref);
			const auto known = book.Known(a_ref->GetFormID());
			const auto value = [](std::uint8_t a_v) { return a_v == RefState::kUnknown ? "-"s : std::to_string(a_v); };
			const auto level = [](std::uint8_t a_v) { return a_v == RefState::LEVEL_UNKNOWN ? "-"s : std::to_string(a_v); };
			const auto at = a_ref->GetPosition();
			const auto dist = Distance(Player(), a_ref);
			// its name as players see it, one word (spaces as _; "-" for none): the rig picks a mechanism by it
			std::string name = a_ref->GetDisplayFullName() ? a_ref->GetDisplayFullName() : "";
			std::ranges::replace_if(name, [](char a_c) { return a_c == ' ' || a_c == '|' || a_c == '=' || static_cast<unsigned char>(a_c) < 0x20; }, '_');
			if (name.empty()) {
				name = "-";
			}
			// wants=: a mechanism's scripts' named items (hex ids, comma between; "-" for none), as a use of it would lend
			// them if our player held them: the rig picks a mechanism that checks its user's items by it
			std::string wants;
			if (RefRules::Of(facts).use) {
				for (const auto form : ScriptForms(a_ref)) {
					if (KindOf(form) != UseRules::ItemKind::kNotItem && RefRules::Shareable(form->GetFormID())) {
						wants += std::format("{}{:08X}", wants.empty() ? "" : ",", form->GetFormID());
					}
				}
			}
			return std::format("ref={:08X} kind={} open={} locked={} level={} loaded={} known={}/{}/{} dist={:.0f} at=({:.0f},{:.0f},{:.0f}) enabled={} wants={} name={}",
				a_ref->GetFormID(), RefRules::KindName(facts), value(read.open), value(read.locked), level(read.lockLevel), a_ref->Is3DLoaded() ? 1 : 0,
				known ? value(known->open) : "-", known ? value(known->locked) : "-", known ? level(known->lockLevel) : "-", dist, at.x, at.y, at.z,
				a_ref->IsDisabled() ? 0 : 1, wants.empty() ? "-" : wants, name);
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::warn("World: the game's event source is not there; doors and mechanisms are not shared");
			return false;
		}
		events->AddEventSink<RE::TESOpenCloseEvent>(&sinks);
		events->AddEventSink<RE::TESLockChangedEvent>(&sinks);
		events->AddEventSink<RE::TESActivateEvent>(&sinks);
		events->AddEventSink<RE::TESContainerChangedEvent>(&sinks);
		installed = true;
		logs::info("World: watching doors, locks and mechanisms (open, close, lock and use events)");
		if (!hooked) {
			// What a use did: the animation events sent to plain references' graphs (activators, movable statics), read
			// while a use is watched or a friend's made here. Without it a use's animations are not said nor matched (the
			// rest of doors and mechanisms runs as before).
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESObjectREFR[3] };
			NotifyHook::Original = vtable.write_vfunc(0x1, NotifyHook::Thunk);
			hooked = true;
			logs::info("World: reading the animation events sent to references (what a mechanism's use did)");
		}
		return true;
	}


	namespace
	{
		// Everything this game knew of the session's doors, forgotten (a welcome, a load, the session's end).
		void Forget()
		{
			book.Clear();
			outbox.Clear();
			pending.Clear();
			again.Clear();
			settle.Clear();
			watch.Clear();
			TakeQueue();  // what happened before is in the world as it is, not news
			// What a use did, either way: of the world before. Items still lent stay with a stand-in that is gone by now
			// (a load takes the stand-ins; the session's end too); a welcome finds none out.
			capture.Clear();
			enables.Clear();
			mirror.Clear();
			loans.Clear();
			{
				std::lock_guard guard{ movesLock };
				lentMoves.clear();
			}
			TakeNotes();
			listening.store(false, std::memory_order_relaxed);
		}
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		ourId = a_playerId;
		Forget();
		askAgain = false;
	}

	void Frame(const SendFn& a_send)
	{
		const auto now = Clock::now();
		if (!LocalPlayer::InWorld()) {
			return;  // a loading screen, the main menu: the events wait in the queue (up to MAX_QUEUED)
		}
		if (std::exchange(askAgain, false)) {
			a_send(Protocol::Writer{ Protocol::MessageType::kAskRefStates }.Data());
			++counts.asks;
			logs::info("World: asked the session for every door's state again (a load)");
		}
		for (const auto& event : TakeQueue()) {
			++counts.events;
			Look(event, a_send, now);
		}
		if (now >= nextScan) {
			nextScan = now + RefRules::SCAN_EVERY;
			Scan(a_send, now);
		}
		// What uses did: ours watched and said, friends' made where ours did not, items lent taken back.
		ReadNotes(now);
		if (capture.Active(now) && now >= nextEnableScan) {
			nextEnableScan = now + RefRules::SCAN_EVERY;
			ScanEnables(now);
		} else if (!capture.Active(now) && enables.Size() != 0) {
			enables.Clear();
		}
		for (const auto& batch : capture.Due(now)) {
			a_send(Protocol::Encode(batch));
			++useCounts.effectBatches;
		}
		MakeEffects(now);
		ReadLentMoves();
		TakeBack(loans.Due(now));
		listening.store(capture.Active(now) || mirror.Listening(now), std::memory_order_relaxed);
		// Held a while (paced), or answered busy by the server: what the parts they were about read now goes, if the
		// session still doesn't know it.
		for (const auto& held : outbox.Due(now)) {
			if (const auto ref = Find(held.refId); ref && ref->Is3DLoaded()) {
				const auto read = RefRules::Only(RefRules::Shared(Read(ref), RefRules::Of(FactsOf(ref))), held);
				if (book.News(read)) {
					Report(read, a_send);
				}
			}
		}
		for (const auto& parts : again.Due(now)) {
			++counts.resent;
			LookAgain(parts, a_send, now);
		}
		// The words that wait: for their reference to load, or for a friend's use made here to settle.
		if (now >= nextRetry && pending.Size() != 0 && !settle.Active(now)) {
			nextRetry = now + RefRules::RETRY_EVERY;
			for (const auto& want : pending.All()) {
				if (Apply(want)) {
					pending.Remove(want.refId);
				}
			}
		}
	}

	void Heard(const RefState& a_state)
	{
		if (!RefRules::Shareable(a_state.refId)) {
			return;  // a reference this game lacks (FormTranslate made it 0)
		}
		++counts.heard;
		if (!book.Heard(a_state, ourId)) {
			++counts.stale;
			return;
		}
		const auto want = book.Known(a_state.refId).value_or(a_state);
		if (a_state.open != RefState::kUnknown && settle.Active(Clock::now())) {
			// A friend's use is being made here: its script moves this first (RefRules::Settle), the word after.
			++counts.settled;
			pending.Add(want);
			return;
		}
		if (!LocalPlayer::InWorld() || !Apply(want)) {
			++counts.waited;
			pending.Add(want);
		} else {
			pending.Remove(want.refId);
		}
	}

	void Refused(const Protocol::RefRefusal& a_refusal)
	{
		if (!RefRules::Shareable(a_refusal.refId)) {
			return;  // a reference this game lacks (none of ours is: we report only our own)
		}
		++counts.notTaken;
		book.Refused(a_refusal.refId, a_refusal.Session());
		if (a_refusal.reason == Protocol::RefRefusal::kBusy) {
			again.Add(a_refusal.Said(), Clock::now());
		}
		if (counts.notTaken == 1) {
			logs::info("World: the session did not take our report on {:08X} ({}); {}", a_refusal.refId,
				a_refusal.reason == Protocol::RefRefusal::kBusy ? "busy" : a_refusal.reason == Protocol::RefRefusal::kFull ? "its book is full" : "refused",
				a_refusal.reason == Protocol::RefRefusal::kBusy ? "it goes again in a moment" : "it stays this game's own");
		}
	}

	void Used(const Protocol::Activation& a_use)
	{
		const auto ref = RefRules::Shareable(a_use.refId) ? Find(a_use.refId) : nullptr;
		const auto standIn = RemotePlayers::StandInOf(a_use.playerId);
		const auto facts = ref ? FactsOf(ref) : RefRules::Facts{};
		const auto verdict = RefRules::JudgeUse({
			.loaded = ref && ref->Is3DLoaded() && !ref->IsDisabled(),
			.mechanism = RefRules::Of(facts).use,
			.standIn = standIn != nullptr,
			.standInDistance = ref && standIn ? Distance(standIn, ref) : -1.0f,
			.trap = Traps::IsTrap(ref),
		});
		// Its effects (what it did in the user's game) are timed from now here, made or not: a use not made here is
		// followed by its effects alone.
		mirror.UseHeard(a_use.playerId, a_use.seq, Clock::now());
		listening.store(true, std::memory_order_relaxed);
		if (verdict != RefRules::UseVerdict::kUse || refuseUses) {
			++counts.usesRefused;
			lastRefused = verdict != RefRules::UseVerdict::kUse ? RefRules::UseVerdictName(verdict) : "dev refuse";
			logs::info("World: player {} used {:08X}; not made here: {}", a_use.playerId, a_use.refId, lastRefused);
			return;
		}
		// As the activate key does for a player, with the stand-in as the one who uses it: the mechanism's own script runs
		// here once, as it ran in the user's game. The stand-in's use is no use of our player's: it is not reported. The
		// words on what the script moves wait until it has run (RefRules::Settle). The stand-in holds what the user held
		// that the mechanism's scripts name (a claw, a key) while it does, so a script that checks its user finds it.
		NoteThread("a stand-in's use of a mechanism");
		Lend(standIn, a_use.lent);
		if (!ref->ActivateRef(standIn, 0, nullptr, 0, false)) {
			++counts.usesRefused;
			lastRefused = "the game refused it";
			logs::info("World: player {} used {:08X} ({}); the game refused their stand-in's use", a_use.playerId, a_use.refId, RefRules::KindName(facts));
			return;
		}
		settle.Used(Clock::now());
		mirror.MadeHere(a_use.playerId, a_use.seq);  // its own run does what the use did, as a rule: its effects wait longer
		++counts.usesMade;
		logs::info("World: player {} used {:08X} ({}), use {}; their stand-in used it here{}", a_use.playerId, a_use.refId, RefRules::KindName(facts), a_use.seq,
			a_use.lent.empty() ? "" : std::format(", holding {} lent item(s) while it does", a_use.lent.size()));
	}

	void HeardEffects(const Protocol::UseEffects& a_effects)
	{
		mirror.Heard(a_effects, Clock::now());
		listening.store(true, std::memory_order_relaxed);
	}

	void BeforeSave()
	{
		// No stand-in is in a save (RemotePlayers::BeforeSave takes them out next), nor anything lent to one: what is out
		// is taken back now, while they are still there (what a stand-in's run handed on first, so it is not taken twice).
		// On the main thread, while the tick may run on the task pool: the loans, the moves and the counts it touches are
		// locked or atomic.
		ReadLentMoves();
		const auto out = loans.All();
		if (!out.empty()) {
			TakeBack(out);
			logs::info("World: {} item(s) lent to stand-ins for friends' uses taken back before the save", out.size());
		}
	}

	void AfterLoad()
	{
		Forget();
		askAgain = ourId != 0;
	}

	void Clear()
	{
		ourId = 0;
		Forget();
		askAgain = false;
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.empty()) {
			std::uint64_t dropped = 0;
			{
				std::lock_guard guard{ queueLock };
				dropped = queueDropped;
			}
			const auto seen = mirror.Read();
			return std::format("installed={} book={} outstanding={} pending={} held={} events={} sent={} paced={} heard={} applied={} same={} waited={} stale={} "
							   "dropped={} uses={}/{}/{} lastRefused={} asks={} notTaken={} resent={} settled={} scanned={} watched={} eventsDropped={} offmain={}",
				installed ? 1 : 0, book.Size(), book.Outstanding(), pending.Size(), outbox.Held(), Counted(counts.events), Counted(counts.sent), Counted(counts.paced),
				Counted(counts.heard), Counted(counts.applied), Counted(counts.same), Counted(counts.waited), Counted(counts.stale), Counted(pending.Dropped()),
				Counted(counts.usesSent), Counted(counts.usesMade), Counted(counts.usesRefused), lastRefused, Counted(counts.asks), Counted(counts.notTaken),
				Counted(counts.resent), Counted(counts.settled), Counted(counts.scanned), watch.Size(), Counted(dropped), Counted(counts.offMain)) +
			       // what uses did: lent=sent/kept/here/back/gone/out/moved/undone (items our uses lent; named, never lent;
			       // lent here to stand-ins, taken back, not there to take back, still out, handed on by the stand-in's run,
			       // of them taken back out of a holder the session shares), effects=captured/cut/batches/early (our uses';
			       // early: animations read before the tick began the watch), mirror=heard/matched/made/already/unloaded/
			       // doubled/waiting/kind (friends' uses' effects here; doubled: our own run's repeats held back; kind: an
			       // enable or disable not made for its kind)
			       std::format(" lent={}/{}/{}/{}/{}/{}/{}/{} effects={}/{}/{}/{} mirror={}/{}/{}/{}/{}/{}/{}/{} notes={} refuse={} hooked={}",
					   Counted(useCounts.lentSent), Counted(useCounts.keptNamed), Counted(useCounts.lentHere), Counted(useCounts.lentBack),
					   Counted(useCounts.lentGone), loans.Out(), Counted(useCounts.lentMoved), Counted(useCounts.lentUndone), Counted(capture.captured),
					   Counted(capture.cut), Counted(useCounts.effectBatches), Counted(useCounts.early), Counted(seen.heard), Counted(seen.matched),
					   Counted(useCounts.made), Counted(useCounts.already), Counted(useCounts.unloaded), Counted(seen.doubled), seen.waiting,
					   Counted(useCounts.refusedKind), Counted(useCounts.notes), refuseUses ? 1 : 0, hooked ? 1 : 0);
		}
		if (words[0] == "refuse") {
			if (words.size() != 2 || (words[1] != "on" && words[1] != "off")) {
				return "error: usage: refs refuse <on|off>";
			}
			refuseUses = words[1] == "on";
			logs::info("World: friends' uses of mechanisms are {} here (the developer channel)", refuseUses ? "refused" : "made again");
			return std::format("refuse={}", refuseUses ? 1 : 0);
		}
		if (words[0] != "near" || words.size() > 2) {
			return "error: usage: refs [near [radius] | refuse <on|off>]";
		}
		const auto radius = words.size() == 2 ? DevLine::Number(words[1]) : std::optional<std::uint64_t>{ 3000 };
		if (!radius || *radius == 0 || *radius > 20000) {
			return "error: usage: refs near [radius 1-20000]";
		}
		const auto player = Player();
		const auto tes = RE::TES::GetSingleton();
		if (!player || !tes || !LocalPlayer::InWorld()) {
			return "error: not in the world";
		}
		// This class's references within the radius, nearest first, at most 40 (one line each, " | " between): what
		// shares a lock or a use, and what shares an open state and has one (not every trigger box an activator is).
		std::vector<std::pair<float, RE::TESObjectREFR*>> found;
		tes->ForEachReferenceInRange(player, static_cast<float>(*radius), [&](RE::TESObjectREFR* a_ref) {
			if (!a_ref || a_ref == player) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto shares = RefRules::Of(FactsOf(a_ref));
			if (shares.lock || shares.use || (shares.open && RefRules::OpenOf(static_cast<int>(RE::BGSOpenCloseForm::GetOpenState(a_ref))) != RefState::kUnknown)) {
				found.emplace_back(Distance(player, a_ref), a_ref);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::ranges::sort(found, {}, &std::pair<float, RE::TESObjectREFR*>::first);
		std::string out = std::format("near={}", found.size());
		for (std::size_t i = 0; i < found.size() && i < 40; ++i) {
			out += " | " + Line(found[i].second);
		}
		return out;
	}

	std::string One(std::string_view a_args)
	{
		const auto id = FormsCommand::ParseHex(a_args);
		const auto ref = id ? Find(*id) : nullptr;
		if (!ref) {
			return "error: usage: ref <hex id of a reference loaded here>";
		}
		return Line(ref);
	}

	std::string Use(std::string_view a_args)
	{
		const auto id = FormsCommand::ParseHex(a_args);
		const auto ref = id ? Find(*id) : nullptr;
		const auto player = Player();
		if (!ref || !player || !ref->Is3DLoaded()) {
			return "error: usage: use <hex id of a reference loaded here>";
		}
		logs::info("World: the developer channel has our player use {:08X}", ref->GetFormID());
		return ref->ActivateRef(player, 0, nullptr, 0, false) ? "used" : "error: the game refused it";
	}

	std::string Scripts(std::string_view a_args)
	{
		const auto id = FormsCommand::ParseHex(a_args);
		const auto ref = id ? Find(*id) : nullptr;
		if (!ref) {
			return "error: usage: script <hex id of a reference loaded here>";
		}
		// Each Papyrus script bound to the reference: its name, its state, and its own whole-number and true/false
		// variables (properties' too, as "::Name_var"), in the order the script declares them. What a mechanism's use
		// changed in its script (a pillar's face, a lever's "done"), for the rig to compare between two games.
		using namespace RE::BSScript;
		const auto vm = Internal::VirtualMachine::GetSingleton();
		const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
		if (!policy) {
			return "error: the script machine is not there";
		}
		const auto  handle = policy->GetHandleForObject(ref->GetFormType(), ref);
		std::size_t count = 0;
		std::string out;
		{
			RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
			if (const auto it = vm->attachedScripts.find(handle); it != vm->attachedScripts.end()) {
				for (const auto& script : it->second) {
					const auto object = script.get();
					const auto type = object ? object->GetTypeInfo() : nullptr;
					if (!type) {
						continue;
					}
					++count;
					out += std::format(" | {} state={}", type->GetName(), object->currentState.empty() ? "-" : object->currentState.c_str());
					const auto vars = type->GetVariableIter();
					for (std::uint32_t i = 0; vars && i < type->GetNumVariables(); ++i) {
						const auto value = object->GetVariable(vars[i].name);
						if (!value) {
							continue;
						}
						if (value->IsInt()) {
							out += std::format(" {}={}", vars[i].name.c_str(), value->GetSInt());
						} else if (value->IsBool()) {
							out += std::format(" {}={}", vars[i].name.c_str(), value->GetBool() ? 1 : 0);
						}
					}
				}
			}
		}
		std::ranges::replace_if(out, [](char a_c) { return static_cast<unsigned char>(a_c) < 0x20; }, '_');
		return std::format("ref={:08X} scripts={}{}", ref->GetFormID(), count, out.size() > 900 ? out.substr(0, 900) : out);
	}

	std::string Anims(std::string_view a_args)
	{
		const auto id = FormsCommand::ParseHex(a_args);
		if (!id || *id == 0) {
			return "error: usage: anims <hex id of a reference>";
		}
		// The animation events read on it here lately (while a use was watched or a friend's made here), oldest first, a
		// friend's use's made here marked with *: the rig compares the user's last with the watcher's.
		const auto entries = animRing.Of(*id);
		std::string events;
		for (const auto& entry : entries) {
			std::string name = entry.event;
			std::ranges::replace_if(name, [](char a_c) { return a_c == ' ' || a_c == ',' || a_c == '=' || static_cast<unsigned char>(a_c) < 0x20; }, '_');
			events += std::format("{}{}{}", events.empty() ? "" : ",", entry.made ? "*" : "", name);
		}
		std::string last = entries.empty() ? "-" : entries.back().event;
		std::ranges::replace_if(last, [](char a_c) { return a_c == ' ' || a_c == ',' || a_c == '=' || static_cast<unsigned char>(a_c) < 0x20; }, '_');
		return std::format("ref={:08X} played={} made={} last={} events={}", *id, entries.size(),
			std::ranges::count_if(entries, [](const UseRules::AnimRing::Entry& a_e) { return a_e.made; }), last, events.empty() ? "-" : events);
	}

	std::string Enables(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		const auto id = words.empty() ? std::nullopt : FormsCommand::ParseHex(words[0]);
		const auto radius = words.size() == 2 ? DevLine::Number(words[1]) : std::optional<std::uint64_t>{ static_cast<std::uint64_t>(UseRules::EFFECT_REACH) };
		const auto origin = id ? Find(*id) : nullptr;
		const auto tes = RE::TES::GetSingleton();
		if (!origin || !tes || words.size() > 2 || !radius || *radius == 0 || *radius > 20000) {
			return "error: usage: enables <hex id of a reference loaded here> [radius 1-20000]";
		}
		// The references near it whose enabling a use's effect can be (as a use's watch reads them), and which of them
		// are disabled now (hex ids, lowest first): the rig compares the two games' lists before and after a use.
		std::size_t                inRange = 0;
		std::vector<std::uint32_t> disabled;
		tes->ForEachReferenceInRange(origin, static_cast<float>(*radius), [&](RE::TESObjectREFR* a_ref) {
			if (PlacedOf(a_ref) != UseRules::Placed::kNot && Distance(origin, a_ref) >= 0.0f) {
				++inRange;
				if (a_ref->IsDisabled()) {
					disabled.push_back(a_ref->GetFormID());
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::ranges::sort(disabled);
		std::string list;
		for (std::size_t i = 0; i < disabled.size() && i < 64; ++i) {
			list += std::format("{}{:08X}", list.empty() ? "" : ",", disabled[i]);
		}
		return std::format("ref={:08X} near={} disabled={} ids={}", *id, inRange, disabled.size(), list.empty() ? "-" : list);
	}
}
