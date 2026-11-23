#include "game/LootSync.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/LootRules.h"
#include "net/FormsCommand.h"

namespace LootSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using LootEntry = Protocol::LootEntry;

		// A move of our player's the sink queued, read on the next frame.
		struct Event
		{
			RE::FormID    holder = 0;
			RE::FormID    item = 0;
			std::uint32_t count = 0;
			std::uint16_t uniqueID = 0;  // the moved stack's own id, when it has extras (0: a plain stack)
			bool          take = true;   // from the holder to our player; false: from our player into the holder
		};

		std::mutex         queueLock;
		std::vector<Event> queue;  // under queueLock
		// A Take All of a big chest is a few dozen; past this a new event is dropped (the first kept) and counted.
		constexpr std::size_t MAX_QUEUED = 2048;
		std::uint64_t         queueDropped = 0;  // under queueLock

		constexpr RE::FormID PLAYER_REF = 0x14;
		constexpr RE::FormID PLAYER_BASE = 0x7;
		constexpr RE::FormID PLAYER_FACTION = 0xDB1;
		constexpr auto       RETRY_EVERY = std::chrono::seconds(2);

		std::uint32_t                  ourId = 0;
		LootRules::Ledger              ledger;
		LootRules::Waiting             waiting;
		LootRules::TakenBook           taken;
		LootRules::EmptyWatch          empty;
		LootRules::Again               again;
		std::unordered_set<RE::FormID> vendorChests;  // every faction's merchant container
		bool                           askAgain = false;
		bool                           installed = false;
		Clock::time_point              lastBarter{};
		Clock::time_point              nextRetry{};
		RE::FormID                     lastOpened = 0;  // the holder our player's container menu showed last frame
		// Read from the co-save at a load, taken by AfterLoad.
		std::optional<LootRules::Ledger::Mark> loadedMark;
		std::vector<LootEntry>                 loadedWaiting;

		struct Counts
		{
			std::uint64_t events = 0;         // queued by the sink
			std::uint64_t sent = 0;           // our moves sent
			std::uint64_t again = 0;          // ... of them sent again (busy)
			std::uint64_t emptiedSent = 0;    // our emptied markers
			std::uint64_t heard = 0;          // moves of the session heard
			std::uint64_t made = 0;           // ... made here
			std::uint64_t takes = 0;          // ... of them takes
			std::uint64_t puts = 0;           // ... puts
			std::uint64_t emptied = 0;        // ... emptied markers
			std::uint64_t removed = 0;        // items the markers took out of our copies
			std::uint64_t shortTakes = 0;     // takes that found fewer here than the move took
			std::uint64_t own = 0;            // our own, echoed (nothing to make)
			std::uint64_t had = 0;            // numbered before what this world holds
			std::uint64_t held = 0;           // heard while a catch-up was coming
			std::uint64_t voided = 0;         // a catch-up asked for a world since replaced
			std::uint64_t waited = 0;         // waited for their holder
			std::uint64_t skipped = 0;        // not this game's to make (its own judgement)
			std::uint64_t asks = 0;           // catch-ups asked for
			std::uint64_t caughtUp = 0;       // catch-ups done
			std::uint64_t caughtUpMade = 0;   // moves a catch-up made
			std::uint64_t busy = 0;           // our moves answered busy
			std::uint64_t full = 0;           // ... the log full
			std::uint64_t refused = 0;        // ... refused
			std::uint64_t stray = 0;          // answers or echoes matching no move of ours out
			std::uint64_t opened = 0;         // waiting moves made as our player opened their holder
			std::uint64_t offMain = 0;        // game writes made off the game's main thread
			std::array<std::uint64_t, LootRules::SCOPE_COUNT> kept{};  // our moves by scope (kShared: reported)
		};
		Counts counts;

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		RE::TESObjectREFR* Find(RE::FormID a_id)
		{
			return a_id != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id) : nullptr;
		}

		void NoteThread(std::string_view a_what)
		{
			if (GameThread::IsLoadThread()) {
				return;
			}
			if (counts.offMain++ == 0) {
				logs::info("Loot: {} made off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); counted in `loot` as offmain",
					a_what);
			}
		}

		LootRules::ItemKind KindOf(const RE::TESForm* a_form)
		{
			using K = LootRules::ItemKind;
			if (!a_form) {
				return K::kOther;
			}
			switch (a_form->GetFormType()) {
			case RE::FormType::Weapon:
				return K::kWeapon;
			case RE::FormType::Armor:
				return K::kArmor;
			case RE::FormType::Ammo:
				return K::kAmmo;
			case RE::FormType::AlchemyItem:
				return K::kPotion;
			case RE::FormType::Ingredient:
				return K::kIngredient;
			case RE::FormType::Misc:
				return K::kMisc;
			case RE::FormType::Book:
				return K::kBook;
			case RE::FormType::KeyMaster:
				return K::kKey;
			case RE::FormType::SoulGem:
				return K::kSoulGem;
			case RE::FormType::Scroll:
				return K::kScroll;
			case RE::FormType::Light:
				return K::kLight;
			case RE::FormType::Apparatus:
				return K::kApparatus;
			default:
				return K::kOther;
			}
		}

		const char* KindName(LootRules::ItemKind a_kind)
		{
			using K = LootRules::ItemKind;
			switch (a_kind) {
			case K::kWeapon:
				return "weapon";
			case K::kArmor:
				return "armor";
			case K::kAmmo:
				return "ammo";
			case K::kPotion:
				return "potion";
			case K::kIngredient:
				return "ingredient";
			case K::kMisc:
				return "misc";
			case K::kBook:
				return "book";
			case K::kKey:
				return "key";
			case K::kSoulGem:
				return "soulgem";
			case K::kScroll:
				return "scroll";
			case K::kLight:
				return "light";
			case K::kApparatus:
				return "apparatus";
			case K::kOther:
				break;
			}
			return "other";
		}

		// A stack with extras of its own: tempered, enchanted or poisoned by a player, renamed, a soul a player put in. A
		// move carries the base form only, so such a stack is never shared, and never taken here for another game's
		// plain one.
		bool IsInstance(RE::ExtraDataList* a_list)
		{
			if (!a_list) {
				return false;
			}
			if (a_list->HasType<RE::ExtraEnchantment>() || a_list->HasType<RE::ExtraPoison>() || a_list->HasType<RE::ExtraTextDisplayData>() ||
				a_list->HasType<RE::ExtraSoul>()) {
				return true;
			}
			const auto health = a_list->GetByType<RE::ExtraHealth>();
			return health && health->health > 1.0f;
		}

		bool IsQuestList(RE::ExtraDataList* a_list)
		{
			return a_list && a_list->HasQuestObjectAlias();
		}

		// How many of a_item a_ref holds as plain stacks: its count less the stacks that are instances or quest objects.
		std::int64_t PlainCount(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_item)
		{
			auto inventory = a_ref->GetInventory([&](RE::TESBoundObject& a_object) { return &a_object == a_item; });
			const auto it = inventory.find(a_item);
			if (it == inventory.end()) {
				return 0;
			}
			std::int64_t count = it->second.first;
			if (const auto& entry = it->second.second; entry && entry->extraLists) {
				for (const auto list : *entry->extraLists) {
					if (list && (IsInstance(list) || IsQuestList(list))) {
						count -= list->GetCount();
					}
				}
			}
			return count;
		}

		bool OwnedByPlayer(const RE::TESForm* a_owner)
		{
			return a_owner && (a_owner->GetFormID() == PLAYER_BASE || a_owner->GetFormID() == PLAYER_FACTION);
		}

		LootRules::Holder HolderOf(RE::TESObjectREFR* a_ref)
		{
			LootRules::Holder holder;
			if (!a_ref || a_ref == Player()) {
				return holder;
			}
			holder.runtime = FormIds::IsRuntime(a_ref->GetFormID());
			if (const auto actor = a_ref->As<RE::Actor>()) {
				holder.kind = LootRules::HolderKind::kActor;
				holder.dead = actor->IsDead();
			} else if (const auto base = a_ref->GetBaseObject(); base && base->GetFormType() == RE::FormType::Container) {
				holder.kind = LootRules::HolderKind::kContainer;
			}
			holder.vendor = vendorChests.contains(a_ref->GetFormID());
			const auto cell = a_ref->GetParentCell();
			holder.storage = OwnedByPlayer(a_ref->GetOwner()) || (cell && OwnedByPlayer(cell->GetOwner()));
			return holder;
		}

		// The item a move is about, as the side that holds it now reads it: the stack moved (by its unique id, when it has
		// one) is an instance or a quest object, or a plain stack (then a quest object of that item there counts).
		LootRules::Item ItemOf(RE::TESBoundObject* a_item, RE::TESObjectREFR* a_side, std::uint16_t a_uniqueID)
		{
			LootRules::Item item;
			item.kind = KindOf(a_item);
			item.runtime = FormIds::IsRuntime(a_item->GetFormID());
			if (!a_side) {
				return item;
			}
			auto       inventory = a_side->GetInventory([&](RE::TESBoundObject& a_object) { return &a_object == a_item; });
			const auto it = inventory.find(a_item);
			if (it == inventory.end() || !it->second.second) {
				return item;
			}
			const auto& entry = it->second.second;
			if (a_uniqueID != 0 && entry->extraLists) {
				for (const auto list : *entry->extraLists) {
					const auto id = list ? list->GetByType<RE::ExtraUniqueID>() : nullptr;
					if (id && id->uniqueID == a_uniqueID) {
						item.instance = IsInstance(list);
						item.quest = IsQuestList(list);
						return item;
					}
				}
			}
			item.quest = entry->IsQuestObject();
			return item;
		}

		bool Bartering(Clock::time_point a_now)
		{
			return a_now - lastBarter < LootRules::BARTER_GRACE;
		}

		// Whether a_ref holds anything the session shares, as plain stacks.
		bool HoldsShared(RE::TESObjectREFR* a_ref)
		{
			for (const auto& [object, value] : a_ref->GetInventory()) {
				if (!object || value.first <= 0) {
					continue;
				}
				LootRules::Item item{ .kind = KindOf(object), .runtime = FormIds::IsRuntime(object->GetFormID()) };
				if (LootRules::ItemScope(item) == LootRules::Scope::kShared && PlainCount(a_ref, object) > 0) {
					return true;
				}
			}
			return false;
		}

		// The holder our player's container menu shows now (0: none open).
		RE::FormID OpenHolder()
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui || !ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
				return 0;
			}
			const auto ref = RE::TESObjectREFR::LookupByHandle(RE::ContainerMenu::GetTargetRefHandle());
			return ref ? ref->GetFormID() : 0;
		}

		// Whether this game can make a move at a_ref now: its cell is in the game's loaded area (an ash pile's or a
		// disabled body's too), or our player has it open.
		bool Loaded(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || a_ref->IsDeleted()) {
				return false;
			}
			const auto cell = a_ref->GetParentCell();
			return (cell && cell->IsAttached()) || OpenHolder() == a_ref->GetFormID();
		}

		// The container menu shows the new contents of a holder our player has open.
		void Refresh(RE::FormID a_holder)
		{
			if (OpenHolder() == a_holder) {
				if (const auto queue_ = RE::UIMessageQueue::GetSingleton()) {
					queue_->AddMessage(RE::ContainerMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUpdate, nullptr);
				}
			}
		}

		// Makes a move of the session's here, or says why not now.
		LootRules::Verdict Make(const LootEntry& a_move)
		{
			const auto ref = Find(a_move.holderId);
			const auto item = a_move.kind == LootEntry::kEmptied ? nullptr : RE::TESForm::LookupByID<RE::TESBoundObject>(a_move.itemId);
			if (!ref || (a_move.kind != LootEntry::kEmptied && !item)) {
				++counts.skipped;
				return LootRules::Verdict::kSkip;  // a form this game lacks (FormTranslate leaves those out)
			}
			const auto facts = HolderOf(ref);
			LootRules::Item itemFacts{ .kind = KindOf(item), .runtime = item && FormIds::IsRuntime(item->GetFormID()) };
			const auto judged = LootRules::Judge(Loaded(ref), facts, itemFacts, a_move.kind == LootEntry::kEmptied);
			if (judged.verdict == LootRules::Verdict::kSkip) {
				++counts.skipped;
			}
			if (judged.verdict != LootRules::Verdict::kMake) {
				return judged.verdict;
			}
			NoteThread("a container's inventory changed");
			// Its inventory as this game makes it when the place loads (its leveled roll), before the move goes in.
			(void)ref->GetInventoryChanges();
			switch (a_move.kind) {
			case LootEntry::kTake:
				{
					const auto n = LootRules::TakeHere(a_move.count, PlainCount(ref, item));
					if (n < a_move.count) {
						++counts.shortTakes;
					}
					if (n > 0) {
						ref->RemoveItem(item, static_cast<std::int32_t>(n), RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
					}
					taken.Took(a_move.holderId, a_move.itemId, a_move.playerId != ourId);
					++counts.takes;
					break;
				}
			case LootEntry::kPut:
				ref->AddObjectToContainer(item, nullptr, static_cast<std::int32_t>(a_move.count), nullptr);
				taken.Put(a_move.holderId, a_move.itemId, a_move.count, a_move.number);
				++counts.puts;
				break;
			case LootEntry::kEmptied:
				{
					const auto holder = a_move.holderId;
					for (const auto& [itemId, keep] : taken.Emptied(holder, a_move.count, [&](std::uint32_t a_item) { return ledger.PutsOut(holder, a_item); })) {
						const auto object = RE::TESForm::LookupByID<RE::TESBoundObject>(itemId);
						const auto n = object ? LootRules::EmptyHere(PlainCount(ref, object), keep) : 0;
						if (n > 0) {
							ref->RemoveItem(object, static_cast<std::int32_t>(n), RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
							counts.removed += n;
						}
					}
					++counts.emptied;
					break;
				}
			default:
				return LootRules::Verdict::kSkip;
			}
			++counts.made;
			Refresh(a_move.holderId);
			return LootRules::Verdict::kMake;
		}

		// A move to make: now, or after the ones for its holder that wait already.
		void MakeOrWait(const LootEntry& a_move, Clock::time_point a_now)
		{
			if (waiting.Has(a_move.holderId) || !LocalPlayer::InWorld()) {
				waiting.Add(a_move, a_now);
				++counts.waited;
				return;
			}
			switch (Make(a_move)) {
			case LootRules::Verdict::kWait:
			case LootRules::Verdict::kDead:
				waiting.Add(a_move, a_now);
				++counts.waited;
				break;
			default:
				break;
			}
		}

		std::vector<LootEntry> outbox;  // our moves this frame, sent at its end

		void Queue(const LootEntry& a_move)
		{
			outbox.push_back(a_move);
		}

		// A queued event, read now: reported when the session shares it. A holder whose moves are the session's and our
		// player took from goes on a_takenFrom (once), for the emptied marker after every event of the frame.
		void Look(const Event& a_event, Clock::time_point a_now, std::vector<RE::FormID>& a_takenFrom)
		{
			const auto ref = Find(a_event.holder);
			const auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(a_event.item);
			if (!ref || !item || a_event.count == 0) {
				return;
			}
			const auto holder = HolderOf(ref);
			const auto barter = Bartering(a_now);
			const auto itemFacts = ItemOf(item, a_event.take ? Player() : ref, a_event.uniqueID);
			const auto scope = LootRules::Of(holder, itemFacts, barter);
			++counts.kept[static_cast<std::size_t>(scope)];
			if (scope == LootRules::Scope::kShared) {
				Queue(LootEntry{ .holderId = a_event.holder, .itemId = a_event.item, .count = (std::min)(a_event.count, Protocol::MAX_LOOT_COUNT),
					.kind = a_event.take ? LootEntry::kTake : LootEntry::kPut });
			}
			if (LootRules::HolderScope(holder, barter) != LootRules::Scope::kShared || holder.storage) {
				return;  // storage keeps each player's own: never said empty
			}
			if (!a_event.take) {
				empty.AfterPut(a_event.holder);
			} else if (std::ranges::find(a_takenFrom, a_event.holder) == a_takenFrom.end()) {
				a_takenFrom.push_back(a_event.holder);
			}
		}

		// After the frame's takes from a_holder (a Take All is a few dozen events, all read in one frame): when its copy
		// here holds nothing the session shares now, the emptied marker goes after those takes, so every other game has
		// made them before it reads the marker.
		void AfterTakes(RE::FormID a_holder)
		{
			const auto ref = Find(a_holder);
			if (ref && empty.AfterTake(a_holder, HoldsShared(ref))) {
				Queue(LootEntry{ .holderId = a_holder, .itemId = 0, .count = ledger.Saved().next, .kind = LootEntry::kEmptied });
				++counts.emptiedSent;
				logs::info("Loot: our player emptied {:08X} of what the session shares; the others' copies are told", a_holder);
			}
		}

		void Flush(const SendFn& a_send, Clock::time_point a_now)
		{
			if (outbox.empty()) {
				return;
			}
			for (const auto& move : outbox) {
				ledger.Sent(move, a_now);
			}
			for (auto& packet : Protocol::LootBatches(outbox, Protocol::MessageType::kLoot)) {
				a_send(std::move(packet));
			}
			counts.sent += outbox.size();
			outbox.clear();
		}

		std::vector<Event> TakeQueue()
		{
			std::lock_guard guard{ queueLock };
			return std::exchange(queue, {});
		}

		void Push(const Event& a_event)
		{
			std::lock_guard guard{ queueLock };
			if (queue.size() < MAX_QUEUED) {
				queue.push_back(a_event);
			} else {
				++queueDropped;
			}
		}

		class Sink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Only our player's own moves: one side is our player, the other a holder. A move made here for the session
				// (between a holder and nobody) or between two others is not.
				if (!a_event || a_event->itemCount <= 0 || a_event->baseObj == 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto count = static_cast<std::uint32_t>(a_event->itemCount);
				if (a_event->newContainer == PLAYER_REF && a_event->oldContainer != 0 && a_event->oldContainer != PLAYER_REF) {
					Push({ .holder = a_event->oldContainer, .item = a_event->baseObj, .count = count, .uniqueID = a_event->uniqueID, .take = true });
				} else if (a_event->oldContainer == PLAYER_REF && a_event->newContainer != 0 && a_event->newContainer != PLAYER_REF) {
					Push({ .holder = a_event->newContainer, .item = a_event->baseObj, .count = count, .uniqueID = a_event->uniqueID, .take = false });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sink sink;

		std::uint32_t Counted(std::uint64_t a_n)
		{
			return static_cast<std::uint32_t>((std::min<std::uint64_t>)(a_n, 0xFFFFFFFF));
		}

		// Our moves this world made while the session was not there are not news: they are in this world already.
		void Forget()
		{
			TakeQueue();
			outbox.clear();
			again.Clear();
			empty.Clear();
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::warn("Loot: the game's event source is not there; containers and loot are not shared");
			return false;
		}
		events->AddEventSink<RE::TESContainerChangedEvent>(&sink);
		if (const auto data = RE::TESDataHandler::GetSingleton()) {
			for (const auto faction : data->GetFormArray<RE::TESFaction>()) {
				if (faction && faction->vendorData.merchantContainer) {
					vendorChests.insert(faction->vendorData.merchantContainer->GetFormID());
				}
			}
		}
		installed = true;
		logs::info("Loot: watching our player's takes and puts ({} merchants' chests known)", vendorChests.size());
		return true;
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		ourId = a_playerId;
		ledger.Disconnected();
		Forget();
		askAgain = true;
	}

	void Frame(const SendFn& a_send)
	{
		const auto now = Clock::now();
		if (!LocalPlayer::InWorld()) {
			return;  // a loading screen, the main menu: the events wait in the queue
		}
		if (std::exchange(askAgain, false)) {
			const auto ask = ledger.Ask();
			a_send(Protocol::Encode(ask));
			++counts.asks;
			logs::info("Loot: asked the session for its moves from {} ({})", ask.from, ask.logId == 0 ? "this world was in no session" : "this world's ledger");
		}
		if (const auto ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::BarterMenu::MENU_NAME)) {
			lastBarter = now;
		}
		std::vector<RE::FormID> takenFrom;
		for (const auto& event : TakeQueue()) {
			++counts.events;
			Look(event, now, takenFrom);
		}
		for (const auto holder : takenFrom) {
			AfterTakes(holder);
		}
		for (const auto& move : again.Due(now)) {
			++counts.again;
			Queue(move);
		}
		Flush(a_send, now);
		ledger.Expire(now);

		// The moves that wait: every RETRY_EVERY, and at once for a holder our player opens (opening it makes its
		// inventory: what waits for it goes in before the player takes anything).
		const auto opened = OpenHolder();
		const bool justOpened = opened != 0 && opened != lastOpened && waiting.Has(opened);
		lastOpened = opened;
		if (waiting.Size() != 0 && (now >= nextRetry || justOpened)) {
			nextRetry = now + RETRY_EVERY;
			const auto before = counts.made;
			waiting.Retry(now, [](const LootEntry& a_move) { return Make(a_move); });
			if (justOpened && counts.made != before) {
				++counts.opened;
			}
		}
	}

	void Heard(const Protocol::LootBatch& a_batch)
	{
		const auto now = Clock::now();
		std::uint32_t made = 0;
		std::uint32_t own = 0;
		std::uint32_t player = 0;
		for (const auto& move : a_batch.entries) {
			if (move.holderId == 0) {
				continue;  // a holder this game lacks
			}
			++counts.heard;
			switch (ledger.Take(move, a_batch.catchUp, ourId)) {
			case LootRules::Ledger::Heard::kMake:
				{
					const auto before = counts.made;
					MakeOrWait(move, now);
					if (counts.made != before) {
						++made;
						player = move.playerId;
					}
					if (a_batch.catchUp) {
						++counts.caughtUpMade;
					}
					break;
				}
			case LootRules::Ledger::Heard::kOwn:
				++counts.own;
				++own;
				// Our put, numbered: a friend's emptied marker as of before it keeps it.
				if (move.kind == LootEntry::kPut) {
					taken.Put(move.holderId, move.itemId, move.count, move.number);
				}
				break;
			case LootRules::Ledger::Heard::kHeld:
				++counts.held;
				break;
			case LootRules::Ledger::Heard::kHad:
				++counts.had;
				break;
			case LootRules::Ledger::Heard::kVoid:
				++counts.voided;
				break;
			}
		}
		if (made != 0 && !a_batch.catchUp) {
			logs::info("Loot: made {} move(s) of player {}'s here (now at {})", made, player, ledger.Saved().next);
		}
	}

	void CaughtUp(const Protocol::LootCaughtUp& a_msg)
	{
		if (!ledger.CaughtUp(a_msg)) {
			return;
		}
		++counts.caughtUp;
		logs::info("Loot: caught up with the session's moves from {} to {} ({} made here, {} waiting for their holder)", a_msg.from, a_msg.next,
			counts.caughtUpMade, waiting.Size());
	}

	void Answered(const Protocol::LootBatch& a_batch)
	{
		const auto now = Clock::now();
		for (const auto& move : a_batch.entries) {
			if (!ledger.Answered(move)) {
				++counts.stray;
			}
			switch (move.answer) {
			case LootEntry::kBusy:
				++counts.busy;
				again.Add(move, now);
				break;
			case LootEntry::kFull:
				if (counts.full++ == 0) {
					logs::info("Loot: the session's log is full; our moves stay this game's own");
				}
				break;
			default:
				if (counts.refused++ == 0) {
					logs::info("Loot: the session refused our move at {:08X} (item {:08X}); it stays this game's own", move.holderId, move.itemId);
				}
				break;
			}
		}
	}

	void AfterLoad()
	{
		Forget();
		ledger.Loaded(loadedMark.value_or(LootRules::Ledger::Mark{}));
		waiting.Clear();
		taken.Clear();
		const auto now = Clock::now();
		for (const auto& move : loadedWaiting) {
			waiting.Add(move, now);
		}
		logs::info("Loot: this world's ledger is {} (next {}), {} move(s) waiting for their holder", loadedMark ? "the save's" : "none",
			ledger.Saved().next, loadedWaiting.size());
		loadedMark.reset();
		loadedWaiting.clear();
		askAgain = ourId != 0;
	}

	void Clear()
	{
		ourId = 0;
		ledger.Disconnected();
		Forget();
		askAgain = false;
	}

	namespace
	{
		template <class T>
		void Put(SKSE::SerializationInterface* a_intf, const T& a_value)
		{
			a_intf->WriteRecordData(&a_value, sizeof(T));
		}
		template <class T>
		bool Get(SKSE::SerializationInterface* a_intf, T& a_value)
		{
			return a_intf->ReadRecordData(&a_value, sizeof(T)) == sizeof(T);
		}
	}

	void WriteCoSave(SKSE::SerializationInterface* a_intf)
	{
		const auto mark = ledger.Saved();
		if (mark.logId == 0 && waiting.Size() == 0) {
			return;  // a world that never was in a session
		}
		if (!a_intf->OpenRecord(COSAVE_LOOT, COSAVE_LOOT_VERSION)) {
			logs::warn("Co-save: could not open the loot record");
			return;
		}
		std::vector<LootEntry> held;
		waiting.ForEach([&](const LootEntry& a_move) { held.push_back(a_move); });
		Put(a_intf, mark.logId);
		Put(a_intf, mark.next);
		Put(a_intf, static_cast<std::uint32_t>(held.size()));
		for (const auto& move : held) {
			Put(a_intf, move.number);
			Put(a_intf, move.playerId);
			Put(a_intf, move.holderId);
			Put(a_intf, move.itemId);
			Put(a_intf, move.count);
			Put(a_intf, move.kind);
		}
		logs::info("Co-save: loot ledger written (next {}, {} move(s) waiting)", mark.next, held.size());
	}

	void ReadCoSave(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, std::uint32_t a_length)
	{
		loadedMark.reset();
		loadedWaiting.clear();
		LootRules::Ledger::Mark mark;
		std::uint32_t           count = 0;
		constexpr std::uint32_t MOVE_BYTES = 5 * sizeof(std::uint32_t) + 1;
		if (a_version != COSAVE_LOOT_VERSION || !Get(a_intf, mark.logId) || !Get(a_intf, mark.next) || !Get(a_intf, count) ||
			count > LootRules::Waiting::MAX_WAITING || a_length != sizeof(mark.logId) + 2 * sizeof(std::uint32_t) + count * MOVE_BYTES) {
			logs::warn("Co-save: a loot record that can't be read (version {}, {} bytes); this world asks the session for every move", a_version, a_length);
			return;
		}
		std::uint32_t lacking = 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			LootEntry move;
			if (!Get(a_intf, move.number) || !Get(a_intf, move.playerId) || !Get(a_intf, move.holderId) || !Get(a_intf, move.itemId) || !Get(a_intf, move.count) ||
				!Get(a_intf, move.kind)) {
				logs::warn("Co-save: the loot record ended early; this world asks the session for every move");
				loadedWaiting.clear();
				return;
			}
			// Saved in that game's numbering: the load order may have changed since.
			RE::FormID holder = 0;
			RE::FormID item = 0;
			if (!a_intf->ResolveFormID(move.holderId, holder) || (move.itemId != 0 && !a_intf->ResolveFormID(move.itemId, item))) {
				++lacking;
				continue;
			}
			move.holderId = holder;
			move.itemId = item;
			loadedWaiting.push_back(move);
		}
		loadedMark = mark;
		logs::info("Co-save: this save's loot ledger is at {} with {} move(s) waiting{}", mark.next, loadedWaiting.size(),
			lacking ? std::format(" ({} of a plugin no longer loaded dropped)", lacking) : "");
	}

	void RevertCoSave()
	{
		loadedMark.reset();
		loadedWaiting.clear();
	}

	namespace
	{
		std::string Line(RE::TESObjectREFR* a_ref, float a_dist)
		{
			const auto holder = HolderOf(a_ref);
			std::size_t items = 0;
			std::size_t shared = 0;
			for (const auto& [object, value] : a_ref->GetInventory()) {
				if (!object || value.first <= 0) {
					continue;
				}
				++items;
				LootRules::Item item{ .kind = KindOf(object), .runtime = FormIds::IsRuntime(object->GetFormID()) };
				if (LootRules::ItemScope(item) == LootRules::Scope::kShared && PlainCount(a_ref, object) > 0) {
					++shared;
				}
			}
			const char* kind = holder.kind == LootRules::HolderKind::kContainer ? "container" :
			                   holder.kind == LootRules::HolderKind::kActor     ? (holder.dead ? "body" : "npc") :
			                                                                      "other";
			bool creature = false;
			bool essential = false;
			if (const auto actor = a_ref->As<RE::Actor>()) {
				const auto race = actor->GetRace();
				creature = race && !race->data.flags.any(RE::RACE_DATA::Flag::kPlayable);
				essential = actor->IsEssential() || actor->IsProtected();
			}
			const auto at = a_ref->GetPosition();
			return std::format("ref={:08X} kind={} loaded={} items={} shared={} vendor={} storage={} runtime={} creature={} essential={} dist={:.0f} at=({:.0f},{:.0f},{:.0f})",
				a_ref->GetFormID(), kind, Loaded(a_ref) ? 1 : 0, items, shared, holder.vendor ? 1 : 0, holder.storage ? 1 : 0, holder.runtime ? 1 : 0,
				creature ? 1 : 0, essential ? 1 : 0, a_dist, at.x, at.y, at.z);
		}

		float Distance(const RE::TESObjectREFR* a_a, const RE::TESObjectREFR* a_b)
		{
			const auto cellA = a_a ? a_a->GetParentCell() : nullptr;
			const auto cellB = a_b ? a_b->GetParentCell() : nullptr;
			if (!cellA || !cellB) {
				return -1.0f;
			}
			if ((cellA->IsInteriorCell() || cellB->IsInteriorCell()) ? cellA != cellB : a_a->GetWorldspace() != a_b->GetWorldspace()) {
				return -1.0f;
			}
			return a_a->GetPosition().GetDistance(a_b->GetPosition());
		}

		std::string Near(const std::vector<std::string_view>& a_words)
		{
			const auto radius = a_words.size() == 2 ? DevLine::Number(a_words[1]) : std::optional<std::uint64_t>{ 3000 };
			if (a_words.size() > 2 || !radius || *radius == 0 || *radius > 20000) {
				return "error: usage: loot near [radius 1-20000]";
			}
			const auto player = Player();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes || !LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			std::vector<std::pair<float, RE::TESObjectREFR*>> found;
			tes->ForEachReferenceInRange(player, static_cast<float>(*radius), [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && a_ref != player && !a_ref->IsDisabled() && HolderOf(a_ref).kind != LootRules::HolderKind::kOther) {
					found.emplace_back(Distance(player, a_ref), a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			std::ranges::sort(found, {}, &std::pair<float, RE::TESObjectREFR*>::first);
			std::string out = std::format("near={}", found.size());
			for (std::size_t i = 0; i < found.size() && i < 30; ++i) {
				out += " | " + Line(found[i].second, found[i].first);
			}
			return out;
		}

		std::string Inventory(const std::vector<std::string_view>& a_words)
		{
			const auto id = a_words.size() == 2 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			const auto ref = id ? Find(*id) : nullptr;
			if (!ref) {
				return "error: usage: loot inv <hex id of a reference loaded here>";
			}
			std::vector<std::string> items;
			for (const auto& [object, value] : ref->GetInventory()) {
				if (!object || value.first <= 0) {
					continue;
				}
				LootRules::Item item{ .kind = KindOf(object), .runtime = FormIds::IsRuntime(object->GetFormID()) };
				items.push_back(std::format("{:08X}:{}:{}:{}", object->GetFormID(), PlainCount(ref, object), KindName(item.kind),
					LootRules::ScopeName(LootRules::ItemScope(item))));
			}
			std::ranges::sort(items);
			std::string out = std::format("ref={:08X} items={}", ref->GetFormID(), items.size());
			for (std::size_t i = 0; i < items.size() && i < 80; ++i) {
				out += (i == 0 ? " | " : " ") + items[i];
			}
			return out;
		}

		// `loot take|put <holder> <item> <n>`: our player moves n of the item as the container menu's take and store do.
		std::string Move(const std::vector<std::string_view>& a_words)
		{
			const bool take = a_words[0] == "take";
			const auto holderId = a_words.size() == 4 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			const auto itemId = a_words.size() == 4 ? FormsCommand::ParseHex(a_words[2]) : std::nullopt;
			const auto n = a_words.size() == 4 ? DevLine::Number(a_words[3]) : std::nullopt;
			const auto holder = holderId ? Find(*holderId) : nullptr;
			const auto item = itemId ? RE::TESForm::LookupByID<RE::TESBoundObject>(*itemId) : nullptr;
			const auto player = Player();
			if (!holder || !item || !n || *n == 0 || *n > Protocol::MAX_LOOT_COUNT || !player || holder == player) {
				return std::format("error: usage: loot {} <holder hex> <item hex> <count>", take ? "take" : "put");
			}
			const auto from = take ? holder : static_cast<RE::TESObjectREFR*>(player);
			const auto to = take ? static_cast<RE::TESObjectREFR*>(player) : holder;
			const auto have = PlainCount(from, item);
			if (have < static_cast<std::int64_t>(*n)) {
				return std::format("error: {} holds {} of {:08X} plain", take ? "the holder" : "our player", have, item->GetFormID());
			}
			logs::info("Loot: the developer channel has our player {} {} x {:08X} {} {:08X}", take ? "take" : "put", *n, item->GetFormID(), take ? "from" : "into",
				holder->GetFormID());
			from->RemoveItem(item, static_cast<std::int32_t>(*n), RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, to);
			return std::format("{} {}", take ? "took" : "put", *n);
		}

		// `loot find <kind>`: the first playable item of that kind the base game (Skyrim.esm) has, by form id.
		std::string FindKind(const std::vector<std::string_view>& a_words)
		{
			if (a_words.size() != 2) {
				return "error: usage: loot find <book|key|weapon|misc>";
			}
			const auto data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				return "error: no game data";
			}
			RE::FormID best = 0;
			const auto consider = [&](RE::TESForm* a_form, bool a_ok) {
				if (a_ok && a_form && a_form->GetPlayable() && (a_form->GetFormID() >> 24) == 0 && a_form->GetName() && a_form->GetName()[0] != '\0' &&
					(best == 0 || a_form->GetFormID() < best)) {
					best = a_form->GetFormID();
				}
			};
			if (a_words[1] == "book") {
				for (const auto book : data->GetFormArray<RE::TESObjectBOOK>()) {
					consider(book, book && book->CanBeTaken() && !book->TeachesSpell() && !book->TeachesSkill());
				}
			} else if (a_words[1] == "key") {
				for (const auto key : data->GetFormArray<RE::TESKey>()) {
					consider(key, true);
				}
			} else if (a_words[1] == "weapon") {
				for (const auto weapon : data->GetFormArray<RE::TESObjectWEAP>()) {
					consider(weapon, weapon && !weapon->formEnchanting && weapon->GetWeaponType() == RE::WEAPON_TYPE::kOneHandDagger);
				}
			} else if (a_words[1] == "misc") {
				for (const auto misc : data->GetFormArray<RE::TESObjectMISC>()) {
					consider(misc, misc && misc->GetFormID() != 0xF);  // not gold: the rig moves gold on its own
				}
			} else {
				return "error: usage: loot find <book|key|weapon|misc>";
			}
			return best != 0 ? std::format("found={:08X}", best) : "found=none";
		}
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
			std::string kept;
			for (std::size_t i = 1; i < LootRules::SCOPE_COUNT; ++i) {
				if (counts.kept[i] != 0) {
					kept += std::format("{}{}:{}", kept.empty() ? "" : ",", LootRules::ScopeName(static_cast<LootRules::Scope>(i)), Counted(counts.kept[i]));
				}
			}
			const auto mark = ledger.Saved();
			return std::format("installed={} log={:08X} next={} asking={} out={} waiting={} again={} events={} sent={} resent={} emptiedSent={} heard={} made={} "
							   "takes={} puts={} emptied={} removed={} short={} own={} had={} held={} voided={} waited={} skipped={} asks={} caughtUp={} "
							   "busy={} full={} refused={} stray={} lost={} opened={} dropped={} expired={} eventsDropped={} kept={} offmain={}",
				installed ? 1 : 0, static_cast<std::uint32_t>(mark.logId), mark.next, ledger.Asking() ? 1 : 0, ledger.Out(), waiting.Size(), again.Size(),
				Counted(counts.events), Counted(counts.sent), Counted(counts.again), Counted(counts.emptiedSent), Counted(counts.heard), Counted(counts.made),
				Counted(counts.takes), Counted(counts.puts), Counted(counts.emptied), Counted(counts.removed), Counted(counts.shortTakes), Counted(counts.own),
				Counted(counts.had), Counted(counts.held), Counted(counts.voided), Counted(counts.waited), Counted(counts.skipped), Counted(counts.asks),
				Counted(counts.caughtUp), Counted(counts.busy), Counted(counts.full), Counted(counts.refused), Counted(counts.stray), Counted(ledger.Lost()),
				Counted(counts.opened), Counted(waiting.Dropped()), Counted(waiting.Expired()), Counted(dropped), kept.empty() ? "-" : kept, Counted(counts.offMain));
		}
		if (words[0] == "near") {
			return Near(words);
		}
		if (words[0] == "inv") {
			return Inventory(words);
		}
		if (words[0] == "take" || words[0] == "put") {
			return Move(words);
		}
		if (words[0] == "find") {
			return FindKind(words);
		}
		return "error: usage: loot [near [radius] | inv <hex> | take <holder> <item> <n> | put <holder> <item> <n> | find <book|key|weapon|misc>]";
	}
}
