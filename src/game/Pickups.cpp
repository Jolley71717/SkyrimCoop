#include "game/Pickups.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/PickupRules.h"
#include "game/Traps.h"
#include "net/FormsCommand.h"

#include <array>
#include <atomic>
#include <deque>

namespace Pickups
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Pickup = Protocol::Pickup;
		using PickupRules::ItemKind;
		using PickupRules::Why;

		constexpr RE::FormID PLAYER_REF = 0x14;

		// What the sinks saw (under queueLock, never stateLock), read on the next tick.
		struct TakeEvent
		{
			RE::FormID ref = 0;
			RE::FormID base = 0;
			bool       quest = false;  // a quest alias held it as the event came
		};
		struct ActivateEvent
		{
			RE::FormID ref = 0;
			RE::FormID produce = 0;  // 0: a leveled produce (any harvest pairs with it)
			bool       harvested = false;
		};
		std::mutex                 queueLock;
		std::vector<TakeEvent>     takeQueue;
		std::vector<ActivateEvent> activateQueue;
		std::vector<RE::FormID>    harvestQueue;  // the produce the engine says our player harvested
		std::vector<RE::FormID>    attachQueue;   // references whose cell attached, while some wait
		struct PlantSeen
		{
			RE::FormID ref = 0;
			bool       harvested = false;
		};
		std::vector<PlantSeen>     plantQueue;    // plants whose cell attached, as they read then
		constexpr std::size_t      MAX_QUEUED = 256;
		std::uint64_t              queueDropped = 0;
		std::uint64_t              takesNoRef = 0;      // our player's gains from nowhere that named no reference (a script's
		                                                // AddItem, or a pickup the event does not name: the rig reads it)
		std::uint64_t              takesOtherBase = 0;  // ... naming a reference of another base than the item's (never a pickup)
		std::uint64_t              takesTrap = 0;       // ... naming a trap (a magic trap's soul gem): its own word's (src/game/Traps.h)
		std::atomic<bool>          active{ false };    // in a session: the sinks queue
		std::atomic<bool>          anyWaiting{ false };  // the attach sink queues only while something waits

		// Everything below, between the tick and SKSE's callbacks on the main thread. Never held while sending.
		std::mutex stateLock;

		bool                    installed = false;
		std::uint32_t           ourId = 0;
		bool                    askDue = false;
		PickupRules::Harvests   harvests;
		PickupRules::Outbox     outbox;
		PickupRules::Waiting    waiting;
		PickupRules::Known      known;
		PickupRules::Plants     plants;
		std::deque<Pickup>      heard;  // from the session, made a few a tick
		std::deque<bool>        heardNews;  // ... each: news (not a catch-up), for the log
		constexpr std::size_t   MAX_HEARD = Protocol::MAX_PICKUPS;
		constexpr std::size_t   MADE_PER_TICK = 64;
		constexpr std::size_t   SWEEP_COUNT = 32;
		constexpr auto          SWEEP_EVERY = std::chrono::seconds(2);
		Clock::time_point       nextSweep{};
		constexpr std::size_t   LOADED_SWEEP_COUNT = 64;  // the watched references in loaded cells: an enable undone soon
		constexpr auto          LOADED_SWEEP_EVERY = std::chrono::milliseconds(500);
		Clock::time_point       nextLoadedSweep{};
		constexpr float         SCAN_RADIUS = 600.0f;     // about 8 m: a harvest no activation explains is near our player
		constexpr float         CENSUS_RADIUS = 1500.0f;  // the plants near our player, read as they grow or not, ...
		constexpr auto          CENSUS_EVERY = std::chrono::seconds(1);  // ... this often (seen before a mod harvests them)
		Clock::time_point       nextCensus{};
		constexpr float         REACH = 250.0f;        // the dev channel's take: our player stands at it
		constexpr std::size_t   MAX_LOGGED_CATCHUP = 16;
		std::size_t             loggedCatchUp = 0;

		struct Counts
		{
			std::uint64_t takes = 0;          // our player's pickups of a loose reference the sink queued
			std::uint64_t activations = 0;    // our player's activations of a plant
			std::uint64_t oldActivations = 0;  // ... of one read harvested, never seen growing: an old harvest, not watched
			std::uint64_t harvestEvents = 0;  // the engine's word of our player's harvests
			std::uint64_t sent = 0;           // taken entries told to the session
			std::uint64_t harvestsSent = 0;   // harvests told to the session
			std::uint64_t paired = 0;         // harvests read from an activation
			std::uint64_t scans = 0;          // harvests no activation explained, looked for near our player
			std::uint64_t scanned = 0;        // ... the plants those scans found
			std::uint64_t heard = 0;          // entries heard
			std::uint64_t catchUps = 0;       // catch-up batches heard
			std::uint64_t trapsLeft = 0;      // taken entries naming a trap here: left to Traps (never disabled here)
			std::uint64_t applied = 0;        // references taken out of this world
			std::uint64_t harvestsApplied = 0;
			std::uint64_t already = 0;        // so here already
			std::uint64_t bothTook = 0;       // ... because our player took it too
			std::uint64_t parked = 0;         // taken, disabled here (not proof it was taken): watched until it reads enabled
			std::uint64_t reapplied = 0;      // taken out again: something enabled one this game had taken out, or one parked
			std::uint64_t held = 0;           // in an inventory here
			std::uint64_t waited = 0;         // waited for their cell
			std::uint64_t attached = 0;       // made as their cell attached
			std::uint64_t ownTakesWatched = 0;  // our player took one the session's word had us watch: ours now
			std::uint64_t heardDropped = 0;   // heard past MAX_HEARD not made yet: the oldest dropped (a load asks again)
			std::uint64_t censuses = 0;       // looks at the plants near our player
			std::uint64_t asks = 0;
			std::uint64_t offMain = 0;
			std::array<std::uint64_t, PickupRules::WHY_COUNT> sentJudged{};   // our own, by why
			std::array<std::uint64_t, PickupRules::WHY_COUNT> heardJudged{};  // the session's skipped here, by why
		};
		Counts counts;

		std::uint32_t Counted(std::uint64_t a_n)
		{
			return static_cast<std::uint32_t>((std::min<std::uint64_t>)(a_n, 0xFFFFFFFF));
		}

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		void NoteThread(std::string_view a_what)
		{
			if (GameThread::IsLoadThread()) {
				return;
			}
			if (counts.offMain++ == 0) {
				logs::info("Pickups: {} off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); counted in `pickups` as offmain",
					a_what);
			}
		}

		ItemKind KindOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return ItemKind::kOther;
			}
			switch (a_form->GetFormType()) {
			case RE::FormType::Weapon:
				return ItemKind::kWeapon;
			case RE::FormType::Armor:
				return ItemKind::kArmor;
			case RE::FormType::Ammo:
				return ItemKind::kAmmo;
			case RE::FormType::AlchemyItem:
				return ItemKind::kPotion;
			case RE::FormType::Ingredient:
				return ItemKind::kIngredient;
			case RE::FormType::Misc:
				return ItemKind::kMisc;
			case RE::FormType::Book:
				return ItemKind::kBook;
			case RE::FormType::KeyMaster:
				return ItemKind::kKey;
			case RE::FormType::SoulGem:
				return ItemKind::kSoulGem;
			case RE::FormType::Scroll:
				return ItemKind::kScroll;
			case RE::FormType::Light:
				{
					// Only a light that can be carried (a torch) is an item: a lantern on a post or a fire's light source is
					// part of the place, and activating it takes nothing (as WorldObjects' PlacedOf tells them apart).
					const auto light = a_form->As<RE::TESObjectLIGH>();
					return light && light->CanBeCarried() ? ItemKind::kLight : ItemKind::kOther;
				}
			case RE::FormType::Apparatus:
				return ItemKind::kApparatus;
			default:
				return ItemKind::kOther;
			}
		}

		const char* KindName(ItemKind a_kind)
		{
			switch (a_kind) {
			case ItemKind::kWeapon:
				return "weapon";
			case ItemKind::kArmor:
				return "armor";
			case ItemKind::kAmmo:
				return "ammo";
			case ItemKind::kPotion:
				return "potion";
			case ItemKind::kIngredient:
				return "ingredient";
			case ItemKind::kMisc:
				return "misc";
			case ItemKind::kBook:
				return "book";
			case ItemKind::kKey:
				return "key";
			case ItemKind::kSoulGem:
				return "soulgem";
			case ItemKind::kScroll:
				return "scroll";
			case ItemKind::kLight:
				return "light";
			case ItemKind::kApparatus:
				return "apparatus";
			default:
				return "other";
			}
		}

		// A plant's produce: a flora's, or a tree's that has one. Null for anything else (and for a tree with none: not a
		// plant). a_plant: whether it is one.
		RE::TESBoundObject* ProduceOf(const RE::TESForm* a_base, bool& a_plant)
		{
			a_plant = false;
			if (!a_base) {
				return nullptr;
			}
			if (const auto flora = a_base->As<RE::TESFlora>()) {
				a_plant = true;
				return flora->produceItem;
			}
			if (const auto tree = a_base->As<RE::TESObjectTREE>(); tree && tree->produceItem) {
				a_plant = true;
				return tree->produceItem;
			}
			return nullptr;
		}

		// The produce as a harvest pairs it: 0 for a leveled one (the engine's harvest may name the item it gave).
		RE::FormID PairedProduce(const RE::TESBoundObject* a_produce)
		{
			if (!a_produce || a_produce->GetFormType() == RE::FormType::LeveledItem) {
				return 0;
			}
			return a_produce->GetFormID();
		}

		bool IsHarvested(const RE::TESObjectREFR* a_ref)
		{
			return a_ref && (a_ref->GetFormFlags() & RE::TESObjectREFR::RecordFlags::kHarvested) != 0;
		}

		bool QuestAlias(RE::TESObjectREFR* a_ref)
		{
			return a_ref && a_ref->extraList.HasQuestObjectAlias();
		}

		// This game's iHoursToRespawnCell: how long a harvested plant takes to grow back (0: not known).
		float RespawnHours()
		{
			const auto settings = RE::GameSettingCollection::GetSingleton();
			const auto setting = settings ? settings->GetSetting("iHoursToRespawnCell") : nullptr;
			return setting ? static_cast<float>(setting->GetInteger()) : 0.0f;
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

		// What this game knows of its own reference of an entry.
		PickupRules::Facts Read(RE::FormID a_ref)
		{
			PickupRules::Facts facts;
			const auto         ref = a_ref != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref) : nullptr;
			if (!ref) {
				return facts;
			}
			facts.found = true;
			const auto base = ref->GetBaseObject();
			facts.base = base ? base->GetFormID() : 0;
			facts.item = KindOf(base);
			(void)ProduceOf(base, facts.plant);
			facts.questAlias = QuestAlias(ref);
			const auto cell = ref->GetParentCell();
			facts.inCell = cell != nullptr;
			facts.attached = cell && cell->IsAttached();
			facts.disabled = ref->IsDisabled();
			facts.deleted = ref->IsDeleted() || ref->IsMarkedForDeletion();
			facts.harvested = IsHarvested(ref);
			return facts;
		}

		// Our player harvested a_ref (judged here: a plant of the load order no quest holds): told to the session.
		void ReportHarvest(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return;
			}
			const auto base = a_ref->GetBaseObject();
			bool       plant = false;
			(void)ProduceOf(base, plant);
			const auto id = a_ref->GetFormID();
			const auto why = PickupRules::Harvestable(plant, FormIds::IsRuntime(id), QuestAlias(a_ref));
			++counts.sentJudged[static_cast<std::size_t>(why)];
			if (why != Why::kShared || !base) {
				return;
			}
			plants.Reported(id);
			if (outbox.Add({ .ref = id, .base = base->GetFormID(), .kind = Pickup::kHarvested })) {
				known.Ours(id);
				++counts.harvestsSent;
				logs::info("Pickups: our player harvested {:08X} ({:08X}); told to the session", id, base->GetFormID());
			}
		}

		// The plants near our player as they read now (PickupRules::Plants): what a scan compares with.
		void Census()
		{
			const auto player = Player();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return;
			}
			++counts.censuses;
			tes->ForEachReferenceInRange(player, CENSUS_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref == player || FormIds::IsRuntime(a_ref->GetFormID()) || a_ref->IsDisabled()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				bool plant = false;
				(void)ProduceOf(a_ref->GetBaseObject(), plant);
				if (plant) {
					plants.Saw(a_ref->GetFormID(), IsHarvested(a_ref));
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		// The plants near our player of a_produce (any, for a leveled one) that read harvested now, were seen growing
		// before and not reported since (Plants: a harvest since, never one from the save or our own from before the last
		// load; one the session told us of or that we told it is reported, and is news again only once it grew back): a
		// harvest no activation explained (a mod's, by script).
		void Scan(RE::FormID a_produce)
		{
			const auto player = Player();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return;
			}
			++counts.scans;
			std::vector<RE::TESObjectREFR*> found;
			tes->ForEachReferenceInRange(player, SCAN_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref == player || !IsHarvested(a_ref) || !plants.SeenGrowing(a_ref->GetFormID())) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				bool       plant = false;
				const auto produce = PairedProduce(ProduceOf(a_ref->GetBaseObject(), plant));
				if (plant && (produce == 0 || produce == a_produce)) {
					found.push_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto ref : found) {
				++counts.scanned;
				ReportHarvest(ref);
			}
		}

		// Takes a reference out of this world for the session, or harvests it.
		void Make(RE::TESObjectREFR* a_ref, const Pickup& a_entry)
		{
			if (a_entry.kind == Pickup::kHarvested) {
				NoteThread("a plant harvested for the session");
				a_ref->formFlags |= RE::TESObjectREFR::RecordFlags::kHarvested;
				a_ref->AddChange(static_cast<std::uint32_t>(RE::TESForm::ChangeFlags::kFlags) | static_cast<std::uint32_t>(RE::TESObjectREFR::ChangeFlags::kEmpty));  // kept in a save
				if (a_ref->Is3DLoaded()) {
					a_ref->Disable();  // its 3D made again, harvested
					a_ref->Enable(false);
				}
				plants.Reported(a_ref->GetFormID());  // the session's harvest: a scan never reports it
				++counts.harvestsApplied;
				return;
			}
			NoteThread("a reference taken out of the world for the session");
			a_ref->Disable();  // never deleted: a load of a save brings back what its plugin placed
			++counts.applied;
		}

		// The session's word on an entry, made here or left waiting. a_news: heard as it happened (logged).
		void Try(const Pickup& a_entry, bool a_news, bool a_attached)
		{
			using State = PickupRules::Waiting::State;
			if (a_entry.kind == Pickup::kTaken) {
				// A trap is never taken out of the world here (a magic trap's soul gem disabled with its controller still
				// set would leave the trap casting): Traps makes the word that it was taken, through its own script.
				if (const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_entry.ref); ref && Traps::IsTrap(ref)) {
					++counts.trapsLeft;
					waiting.Remove(a_entry.ref);
					return;
				}
			}
			const auto facts = Read(a_entry.ref);
			const auto judged = PickupRules::Receive(a_entry, facts, RespawnHours());
			const auto was = waiting.StateOf(a_entry.ref);
			const bool ours = known.IsOurs(a_entry.ref);
			switch (judged.verdict) {
			case PickupRules::Verdict::kApply:
				if (const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_entry.ref)) {
					Make(ref, a_entry);
					if (a_attached) {
						++counts.attached;
					}
					if (was == State::kMade || was == State::kParked) {
						if (++counts.reapplied <= MAX_LOGGED_CATCHUP) {
							logs::info("Pickups: {:08X} player {} took reads enabled here again ({}); out of this world again", a_entry.ref, a_entry.playerId,
								was == State::kMade ? "something enabled it after we took it out" : "it was disabled here when we heard of it");
						}
					} else if (a_news || a_attached || loggedCatchUp++ < MAX_LOGGED_CATCHUP) {
						logs::info("Pickups: {} {:08X} in this world (player {} {} it)", a_entry.kind == Pickup::kHarvested ? "harvested" : "took out", a_entry.ref,
							a_entry.playerId, a_entry.kind == Pickup::kHarvested ? "harvested" : "took");
					}
				}
				known.Theirs(a_entry.ref);
				break;
			case PickupRules::Verdict::kAlready:
				++counts.already;
				if (ours) {
					++counts.bothTook;
					logs::info("Pickups: player {} and our player both {} {:08X} before either heard of the other", a_entry.playerId,
						a_entry.kind == Pickup::kHarvested ? "harvested" : "took", a_entry.ref);
				}
				if (a_entry.kind == Pickup::kHarvested) {
					plants.Reported(a_entry.ref);  // the session holds this harvest: a scan never says it again (until it grows back)
				}
				known.Theirs(a_entry.ref);
				break;
			case PickupRules::Verdict::kParked:
				if (ours) {
					++counts.already;
					++counts.bothTook;
				} else if (!was) {
					++counts.parked;
					logs::info("Pickups: {:08X} player {} took reads disabled here (not proof it was taken here); watched, taken out if it reads enabled",
						a_entry.ref, a_entry.playerId);
				}
				known.Theirs(a_entry.ref);
				break;
			case PickupRules::Verdict::kHeld:
				++counts.held;
				known.Theirs(a_entry.ref);
				break;
			case PickupRules::Verdict::kWait:
				if (!was) {
					++counts.waited;
				}
				break;
			case PickupRules::Verdict::kSkip:
				++counts.heardJudged[static_cast<std::size_t>(judged.why)];
				break;
			}
			if (const auto keep = PickupRules::KeepAfter(judged.verdict, a_entry.kind, ours, was)) {
				const bool loaded = facts.found && facts.attached && judged.verdict != PickupRules::Verdict::kWait;
				if (waiting.Add(a_entry, *keep, loaded)) {
					anyWaiting = true;
				}
			} else {
				waiting.Remove(a_entry.ref);
			}
		}

		class Sinks :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>,
			public RE::BSTEventSink<RE::TESHarvestedEvent::ItemHarvested>,
			public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Our player's pickup of a loose reference: from nobody to our player, the reference named.
				if (!active || !a_event || a_event->oldContainer != 0 || a_event->newContainer != PLAYER_REF || a_event->baseObj == 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto ref = a_event->reference.get();
				if (!ref) {
					std::lock_guard guard{ queueLock };
					++takesNoRef;
					return RE::BSEventNotifyControl::kContinue;
				}
				// The reference must be one of the item gained: a source of another kind (a plant on its produce's gain, a
				// script's AddItem naming some reference) is no pickup, and one wrong word would hold that reference's first
				// kind in the server's book for the whole session.
				const auto base = ref->GetBaseObject();
				if (!base || base->GetFormID() != a_event->baseObj) {
					std::lock_guard guard{ queueLock };
					++takesOtherBase;
					return RE::BSEventNotifyControl::kContinue;
				}
				if (Traps::IsTrap(ref.get())) {
					std::lock_guard guard{ queueLock };
					++takesTrap;  // a trap taken (a magic trap's soul gem) is told by Traps, which makes its script's own end
					return RE::BSEventNotifyControl::kContinue;
				}
				TakeEvent event{ .ref = ref->GetFormID(), .base = a_event->baseObj, .quest = ref->extraList.HasQuestObjectAlias() };
				std::lock_guard guard{ queueLock };
				if (takeQueue.size() < MAX_QUEUED) {
					takeQueue.push_back(event);
				} else {
					++queueDropped;
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				// Our player's activation of a plant: its harvested flag read now (the event may come before or after it).
				if (!active || !a_event || !a_event->objectActivated || !a_event->actionRef) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto by = a_event->actionRef.get();
				const auto what = a_event->objectActivated.get();
				if (!by || by->GetFormID() != PLAYER_REF || !what) {
					return RE::BSEventNotifyControl::kContinue;
				}
				bool       plant = false;
				const auto produce = ProduceOf(what->GetBaseObject(), plant);
				if (!plant) {
					return RE::BSEventNotifyControl::kContinue;
				}
				ActivateEvent event{ .ref = what->GetFormID(), .produce = PairedProduce(produce), .harvested = IsHarvested(what) };
				std::lock_guard guard{ queueLock };
				if (activateQueue.size() < MAX_QUEUED) {
					activateQueue.push_back(event);
				} else {
					++queueDropped;
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHarvestedEvent::ItemHarvested* a_event, RE::BSTEventSource<RE::TESHarvestedEvent::ItemHarvested>*) override
			{
				if (!active || !a_event || !a_event->harvester || a_event->harvester->GetFormID() != PLAYER_REF) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto      produce = a_event->produceItem ? a_event->produceItem->GetFormID() : 0;
				std::lock_guard guard{ queueLock };
				if (harvestQueue.size() < MAX_QUEUED) {
					harvestQueue.push_back(produce);
				} else {
					++queueDropped;
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				// A reference attached: made now if the session's word on it waits (looked up on the tick); a plant is seen as
				// it reads (growing or harvested: what a scan compares with).
				if (!active || !a_event || !a_event->attached || !a_event->reference) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto ref = a_event->reference.get();
				const auto id = ref->GetFormID();
				if (FormIds::IsRuntime(id)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				bool plant = false;
				(void)ProduceOf(ref->GetBaseObject(), plant);
				const bool      wanted = anyWaiting;
				std::lock_guard guard{ queueLock };
				if (plant && !ref->IsDisabled()) {
					if (plantQueue.size() < 4 * MAX_QUEUED) {
						plantQueue.push_back({ id, IsHarvested(ref) });
					} else {
						++queueDropped;  // the census sees it later, if near
					}
				}
				if (wanted) {
					if (attachQueue.size() < 4 * MAX_QUEUED) {
						attachQueue.push_back(id);
					} else {
						++queueDropped;  // the sweep makes it later
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sinks sinks;

		void ClearQueues()
		{
			std::lock_guard guard{ queueLock };
			takeQueue.clear();
			activateQueue.clear();
			harvestQueue.clear();
			attachQueue.clear();
			plantQueue.clear();
		}

		// What this world was told is forgotten: a load or a new game made it the save's.
		void ForgetWorld()
		{
			harvests.Clear();
			waiting.Clear();
			known.Clear();
			plants.Clear();
			heard.clear();
			heardNews.clear();
			anyWaiting = false;
			loggedCatchUp = 0;
		}
	}

	bool Install()
	{
		std::lock_guard guard{ stateLock };
		if (installed) {
			return true;
		}
		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::warn("Pickups: the game's event source is not there; what players take from the world is not shared");
			return false;
		}
		events->AddEventSink<RE::TESContainerChangedEvent>(&sinks);
		events->AddEventSink<RE::TESActivateEvent>(&sinks);
		events->AddEventSink<RE::TESCellAttachDetachEvent>(&sinks);
		if (const auto harvested = RE::TESHarvestedEvent::GetEventSource()) {
			harvested->AddEventSink(&sinks);
		} else {
			logs::warn("Pickups: the engine's harvest events are not there; a harvest is read from our player's activations only");
		}
		installed = true;
		logs::info("Pickups: watching what our player takes from the world");
		return true;
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		std::lock_guard guard{ stateLock };
		ourId = a_playerId;
		ForgetWorld();
		outbox.Clear();
		askDue = false;  // the server sends what it holds with the welcome
		ClearQueues();   // taken before we were in: this world's own
		active = true;
	}

	void Frame(const SendFn& a_send)
	{
		std::vector<std::vector<std::uint8_t>> packets;
		{
			std::lock_guard            guard{ stateLock };
			const auto                 now = Clock::now();
			std::vector<TakeEvent>     takes;
			std::vector<ActivateEvent> activations;
			std::vector<RE::FormID>    harvestEvents;
			std::vector<RE::FormID>    attaches;
			std::vector<PlantSeen>     plantsSeen;
			{
				std::lock_guard queueGuard{ queueLock };
				takes.swap(takeQueue);
				activations.swap(activateQueue);
				harvestEvents.swap(harvestQueue);
				attaches.swap(attachQueue);
				plantsSeen.swap(plantQueue);
			}
			for (const auto& seen : plantsSeen) {
				plants.Saw(seen.ref, seen.harvested);
			}

			// Our player's pickups.
			for (const auto& take : takes) {
				++counts.takes;
				const auto base = RE::TESForm::LookupByID(take.base);
				const auto why = PickupRules::Takeable(KindOf(base), FormIds::IsRuntime(take.ref), take.quest);
				++counts.sentJudged[static_cast<std::size_t>(why)];
				if (waiting.Find(take.ref)) {
					// The session's word had us watch it (parked, or taken out here and enabled since): our player has it now,
					// so it is ours (never taken out of our world again).
					waiting.Remove(take.ref);
					++counts.ownTakesWatched;
				}
				if (why == Why::kShared && outbox.Add({ .ref = take.ref, .base = take.base, .kind = Pickup::kTaken })) {
					known.Ours(take.ref);
					++counts.sent;
					logs::info("Pickups: our player took {:08X} ({:08X}) from the world; told to the session", take.ref, take.base);
				}
			}

			// Our player's harvests.
			for (const auto& activation : activations) {
				++counts.activations;
				if (!harvests.Activated(activation.ref, activation.produce, activation.harvested, plants.SeenGrowing(activation.ref), now) && activation.harvested) {
					++counts.oldActivations;  // harvested long ago (the save's, or before a load): it takes no harvest's event
				}
			}
			for (const auto produce : harvestEvents) {
				++counts.harvestEvents;
				harvests.Harvested(produce, now);
			}
			const auto due = harvests.Tend(now, [](std::uint32_t a_ref) -> std::optional<bool> {
				const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
				if (!ref || ref->IsDeleted()) {
					return std::nullopt;
				}
				return IsHarvested(ref);
			});
			for (const auto id : due.refs) {
				++counts.paired;
				ReportHarvest(RE::TESForm::LookupByID<RE::TESObjectREFR>(id));
			}
			if (LocalPlayer::InWorld()) {
				for (const auto produce : due.scans) {
					Scan(produce);
				}
				if (now >= nextCensus) {
					nextCensus = now + CENSUS_EVERY;
					Census();
				}
			}

			// The session's word, made here.
			if (LocalPlayer::InWorld()) {
				for (const auto id : attaches) {
					if (const auto entry = waiting.Find(id)) {
						const auto copy = *entry;
						Try(copy, false, true);
					}
				}
				for (std::size_t i = 0; i < MADE_PER_TICK && !heard.empty(); ++i) {
					const auto entry = heard.front();
					const bool news = heardNews.front();
					heard.pop_front();
					heardNews.pop_front();
					Try(entry, news, false);
				}
				if (now >= nextLoadedSweep) {
					nextLoadedSweep = now + LOADED_SWEEP_EVERY;
					for (const auto& entry : waiting.SweepLoaded(LOADED_SWEEP_COUNT)) {
						Try(entry, false, false);
					}
				}
				if (now >= nextSweep) {
					nextSweep = now + SWEEP_EVERY;
					for (const auto& entry : waiting.Sweep(SWEEP_COUNT)) {
						Try(entry, false, false);
					}
				}
				anyWaiting = waiting.Size() != 0;
			}

			if (askDue) {
				askDue = false;
				++counts.asks;
				packets.push_back(Protocol::Writer{ Protocol::MessageType::kAskPickups }.Data());
				logs::info("Pickups: asked the session again for what players took from the world");
			}
			if (const auto batch = outbox.Due(now)) {
				Protocol::PickupBatch msg;
				msg.entries = *batch;
				packets.push_back(Protocol::Encode(msg));
			}
		}
		for (auto& packet : packets) {
			a_send(std::move(packet));
		}
	}

	void Heard(const Protocol::PickupBatch& a_batch)
	{
		std::lock_guard guard{ stateLock };
		if (a_batch.catchUp) {
			++counts.catchUps;
		}
		for (const auto& entry : a_batch.entries) {
			++counts.heard;
			if (heard.size() >= MAX_HEARD) {
				heard.pop_front();
				heardNews.pop_front();
				if (counts.heardDropped++ == 0) {
					logs::warn("Pickups: more of the session's word heard than this game made yet ({} waiting); the oldest dropped (counted in `pickups` as heardDropped)",
						MAX_HEARD);
				}
			}
			heard.push_back(entry);
			heardNews.push_back(!a_batch.catchUp);
		}
	}

	void AfterLoad()
	{
		std::lock_guard guard{ stateLock };
		ForgetWorld();
		askDue = active;  // in a session: the session's word again (our own pickups come back with the save: ours again)
	}

	void NewGame()
	{
		AfterLoad();
	}

	void Clear()
	{
		std::lock_guard guard{ stateLock };
		active = false;
		ForgetWorld();
		outbox.Clear();
		askDue = false;
		ourId = 0;
		ClearQueues();
	}

	namespace
	{
		// The loose items and plants near our player, nearest first: a_sort "" (all), or one sort for pick.
		struct Near
		{
			RE::TESObjectREFR* ref = nullptr;
			float              distance = 0.0f;
			bool               plant = false;
			bool               tree = false;  // ... a tree with something to harvest (else a flora)
			ItemKind           item = ItemKind::kOther;
		};

		std::vector<Near> FindNear(float a_radius)
		{
			std::vector<Near> found;
			const auto        player = Player();
			const auto        tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return found;
			}
			tes->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref == player || a_ref->IsDisabled() || a_ref->IsDeleted()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto base = a_ref->GetBaseObject();
				Near       spot{ .ref = a_ref, .distance = Distance(player, a_ref), .item = KindOf(base) };
				(void)ProduceOf(base, spot.plant);
				spot.tree = spot.plant && base && base->Is(RE::FormType::Tree);
				if (spot.plant || spot.item != ItemKind::kOther) {
					found.push_back(spot);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			std::ranges::sort(found, {}, &Near::distance);
			return found;
		}

		std::optional<float> Radius(const std::vector<std::string_view>& a_words, std::size_t a_at, float a_default)
		{
			if (a_words.size() <= a_at) {
				return a_default;
			}
			const auto n = DevLine::Number(a_words[a_at]);
			if (!n || *n == 0 || *n > 8000) {
				return std::nullopt;
			}
			return static_cast<float>(*n);
		}

		std::string Line(const Near& a_near, RE::TESObjectREFR* a_aimed)
		{
			const auto base = a_near.ref->GetBaseObject();
			return std::format("ref={:08X} base={:08X} sort={} harvested={} quest={} runtime={} crime={} aimed={} dist={:.0f}", a_near.ref->GetFormID(),
				base ? base->GetFormID() : 0, a_near.plant ? (a_near.tree ? "tree" : "flora") : KindName(a_near.item), IsHarvested(a_near.ref) ? 1 : 0,
				QuestAlias(a_near.ref) ? 1 : 0, FormIds::IsRuntime(a_near.ref->GetFormID()) ? 1 : 0, a_near.ref->IsCrimeToActivate() ? 1 : 0,
				a_near.ref == a_aimed ? 1 : 0, a_near.distance);
		}

		RE::TESObjectREFR* Aimed()
		{
			const auto pick = RE::CrosshairPickData::GetSingleton();
			return pick ? pick->GetActiveTarget().get().get() : nullptr;
		}

		std::string NearCommand(const std::vector<std::string_view>& a_words)
		{
			const auto radius = Radius(a_words, 1, 600.0f);
			if (!radius || a_words.size() > 2) {
				return "error: usage: pickups near [radius]";
			}
			if (!LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			const auto  found = FindNear(*radius);
			const auto  aimed = Aimed();
			std::string out = std::format("near={}", found.size());
			for (std::size_t i = 0; i < found.size() && i < 30; ++i) {
				out += " | " + Line(found[i], aimed);
			}
			return out;
		}

		// pick <item|book|key|flora|tree> [radius]: the nearest one of that sort the session would share a take of (of the
		// load order, no quest's, in the world, its 3D loaded: never a marker-like reference with nothing drawn; a plant not
		// harvested), that our player's activation takes as a player's does (not blocked by a script, which would take it
		// its own way or not at all; a light only if it can be carried, KindOf; a book that can be taken and teaches
		// nothing, so reading it in the rig's game changes no skill or spell), and not a crime to take (the rig never steals:
		// a bounty would set the town's guards on the players). "item": any shared item kind; "flora": a flora (FLOR);
		// "tree": a tree with something to harvest.
		std::string PickCommand(const std::vector<std::string_view>& a_words)
		{
			constexpr auto USAGE = "error: usage: pickups pick <item|book|key|flora|tree> [radius]";
			const auto     radius = Radius(a_words, 2, 1500.0f);
			if (a_words.size() < 2 || a_words.size() > 3 || !radius) {
				return USAGE;
			}
			const auto sort = a_words[1];
			if (sort != "item" && sort != "book" && sort != "key" && sort != "flora" && sort != "tree") {
				return USAGE;
			}
			if (!LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			const auto aimed = Aimed();
			for (const auto& spot : FindNear(*radius)) {
				if (FormIds::IsRuntime(spot.ref->GetFormID()) || QuestAlias(spot.ref) || !spot.ref->GetParentCell() || spot.distance < 0.0f ||
					spot.ref->IsCrimeToActivate() || spot.ref->IsActivationBlocked() || !spot.ref->Is3DLoaded()) {
					continue;
				}
				bool match = false;
				if (sort == "flora" || sort == "tree") {
					match = spot.plant && spot.tree == (sort == "tree") && !IsHarvested(spot.ref);
				} else if (sort == "book") {
					const auto book = spot.plant ? nullptr : spot.ref->GetBaseObject()->As<RE::TESObjectBOOK>();
					match = book && spot.item == ItemKind::kBook && book->CanBeTaken() && !book->TeachesSpell() && !book->TeachesSkill();
				} else if (sort == "key") {
					match = !spot.plant && spot.item == ItemKind::kKey;
				} else {
					match = !spot.plant && PickupRules::Takeable(spot.item, false, false) == Why::kShared;
				}
				if (match) {
					const auto at = spot.ref->GetPosition();
					return std::format("{} x={:.0f} y={:.0f} z={:.0f}", Line(spot, aimed), at.x, at.y, at.z);
				}
			}
			return "none";
		}

		// What a reference shows: the geometry drawn under its 3D root (an object the game culls is not drawn; a switch node
		// draws its chosen child only), counted, with a hash of their names. A harvested plant's 3D, made again, shows
		// another shape than the growing plant (its flowers or fruit gone): the rig compares the harvester's with the other's.
		struct Shape
		{
			bool          loaded = false;
			std::uint32_t count = 0;
			std::uint32_t hash = 2166136261u;  // FNV-1a of the drawn geometry's names, in walk order
		};

		void Walk(RE::NiAVObject* a_object, Shape& a_shape, int a_depth)
		{
			if (!a_object || a_depth > 64 || a_object->GetAppCulled()) {
				return;
			}
			if (a_object->AsGeometry()) {
				++a_shape.count;
				for (const char* c = a_object->name.c_str(); c && *c; ++c) {
					a_shape.hash = (a_shape.hash ^ static_cast<unsigned char>(*c)) * 16777619u;
				}
				a_shape.hash = (a_shape.hash ^ 0xFFu) * 16777619u;  // ends a name: "ab","c" hashes apart from "a","bc"
				return;
			}
			if (const auto switchNode = a_object->AsSwitchNode()) {
				auto&      children = switchNode->GetChildren();
				const auto index = switchNode->index;
				if (index >= 0 && static_cast<std::size_t>(index) < static_cast<std::size_t>(children.size())) {
					Walk(children[static_cast<std::uint16_t>(index)].get(), a_shape, a_depth + 1);
				}
				return;
			}
			if (const auto node = a_object->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					Walk(child.get(), a_shape, a_depth + 1);
				}
			}
		}

		Shape ShapeOf(RE::TESObjectREFR* a_ref)
		{
			Shape      shape;
			const auto root = a_ref && a_ref->Is3DLoaded() ? a_ref->Get3D() : nullptr;
			if (root) {
				shape.loaded = true;
				Walk(root, shape, 0);
			}
			return shape;
		}

		const char* StateName(std::optional<PickupRules::Waiting::State> a_state)
		{
			using State = PickupRules::Waiting::State;
			if (!a_state) {
				return "none";
			}
			switch (*a_state) {
			case State::kWaiting:
				return "wait";
			case State::kParked:
				return "parked";
			case State::kMade:
				return "made";
			}
			return "?";
		}

		std::string RefCommand(const std::vector<std::string_view>& a_words)
		{
			const auto id = a_words.size() == 2 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			if (!id) {
				return "error: usage: pickups ref <ref hex>";
			}
			const auto facts = Read(*id);
			const auto ref = facts.found ? RE::TESForm::LookupByID<RE::TESObjectREFR>(*id) : nullptr;
			const auto shape = ShapeOf(ref);
			return std::format("ref={:08X} found={} base={:08X} inworld={} disabled={} deleted={} harvested={} held={} waiting={} watch={} known={} ours={} "
							   "seenGrowing={} loaded3d={} shown={}:{:08X} dist={:.0f}",
				*id, facts.found ? 1 : 0, facts.base, facts.found && facts.inCell && !facts.disabled && !facts.deleted ? 1 : 0, facts.disabled ? 1 : 0,
				facts.deleted ? 1 : 0, facts.harvested ? 1 : 0, facts.found && !facts.inCell ? 1 : 0, waiting.Find(*id) ? 1 : 0, StateName(waiting.StateOf(*id)),
				known.Has(*id) ? 1 : 0, known.IsOurs(*id) ? 1 : 0, plants.SeenGrowing(*id) ? 1 : 0, shape.loaded ? 1 : 0, shape.count, shape.hash,
				ref ? Distance(Player(), ref) : -1.0f);
		}

		// take <ref hex>: our player's activation of a reference within reach (the rig walks there first): a Take, or a
		// harvest, as the activate key does.
		std::string TakeCommand(const std::vector<std::string_view>& a_words)
		{
			const auto id = a_words.size() == 2 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			if (!id) {
				return "error: usage: pickups take <ref hex>";
			}
			const auto player = Player();
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(*id);
			if (!player || !ref || ref->IsDisabled() || ref->IsDeleted() || !ref->GetParentCell()) {
				return "error: no such reference in the world here";
			}
			const auto distance = Distance(player, ref);
			if (distance < 0.0f || distance > REACH) {
				return std::format("error: out of reach (dist={:.0f}, reach {:.0f})", distance, REACH);
			}
			if (ref->IsCrimeToActivate()) {
				return "error: taking it is a crime here";
			}
			logs::info("Pickups: the developer channel has our player activate {:08X}", *id);
			ref->ActivateRef(player, 0, nullptr, 0, false);
			return "activated";
		}
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && (words[0] == "near" || words[0] == "pick" || words[0] == "take")) {
			// Not under the state lock: an activation sends its events from inside the call, and they only queue.
			if (words[0] == "near") {
				return NearCommand(words);
			}
			if (words[0] == "pick") {
				return PickCommand(words);
			}
			return TakeCommand(words);
		}
		std::lock_guard guard{ stateLock };
		if (words.empty()) {
			std::uint64_t dropped = 0;
			std::uint64_t noRef = 0;
			std::uint64_t otherBase = 0;
			std::uint64_t trapTakes = 0;
			{
				std::lock_guard queueGuard{ queueLock };
				dropped = queueDropped;
				noRef = takesNoRef;
				otherBase = takesOtherBase;
				trapTakes = takesTrap;
			}
			const auto judged = [](const std::array<std::uint64_t, PickupRules::WHY_COUNT>& a_counts) {
				std::string out;
				for (std::size_t i = 0; i < PickupRules::WHY_COUNT; ++i) {
					if (a_counts[i] != 0) {
						out += std::format("{}{}:{}", out.empty() ? "" : ",", PickupRules::WhyName(static_cast<Why>(i)), Counted(a_counts[i]));
					}
				}
				return out.empty() ? std::string{ "-" } : out;
			};
			using State = PickupRules::Waiting::State;
			return std::format("installed={} us={} session={} queued={} waiting={} parkedNow={} madeNow={} loadedNow={} known={} plants={} heardQueued={} "
							   "watched={} takes={} takesNoRef={} takesOtherBase={} takesTrap={} trapsLeft={} activations={} oldActivations={} harvestEvents={} sent={} harvestsSent={} paired={} scans={} "
							   "scanned={} censuses={} heard={} catchUps={} applied={} harvestsApplied={} already={} bothTook={} parked={} reapplied={} held={} "
							   "waited={} attached={} ownTakesWatched={} asks={} overflow={} refused={} eventsDropped={} heardDropped={} plantsForgot={} "
							   "judged={} skipped={} respawnHours={:.0f} offmain={}",
				installed ? 1 : 0, ourId, active ? 1 : 0, outbox.Size(), waiting.Count(State::kWaiting), waiting.Count(State::kParked),
				waiting.Count(State::kMade), waiting.Loaded(), known.Size(), plants.Size(), heard.size(), harvests.Watched(), Counted(counts.takes), Counted(noRef),
				Counted(otherBase), Counted(trapTakes), Counted(counts.trapsLeft), Counted(counts.activations), Counted(counts.oldActivations), Counted(counts.harvestEvents), Counted(counts.sent), Counted(counts.harvestsSent),
				Counted(counts.paired), Counted(counts.scans), Counted(counts.scanned), Counted(counts.censuses), Counted(counts.heard), Counted(counts.catchUps),
				Counted(counts.applied), Counted(counts.harvestsApplied), Counted(counts.already), Counted(counts.bothTook), Counted(counts.parked),
				Counted(counts.reapplied), Counted(counts.held), Counted(counts.waited), Counted(counts.attached), Counted(counts.ownTakesWatched),
				Counted(counts.asks), Counted(outbox.Overflow()), Counted(waiting.Refused()), Counted(dropped), Counted(counts.heardDropped),
				Counted(plants.Forgot()), judged(counts.sentJudged), judged(counts.heardJudged), RespawnHours(), Counted(counts.offMain));
		}
		if (words[0] == "ref") {
			return RefCommand(words);
		}
		return "error: usage: pickups [near [radius] | pick <item|book|key|flora|tree> [radius] | ref <ref hex> | take <ref hex>]";
	}
}
