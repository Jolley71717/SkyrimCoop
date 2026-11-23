#include "game/DropSync.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/DropRules.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/Retire.h"
#include "net/FormsCommand.h"

#include <array>
#include <atomic>
#include <iterator>
#include <map>
#include <set>

namespace DropSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using DroppedItem = Protocol::DroppedItem;

		constexpr RE::FormID PLAYER_REF = 0x14;

		// An item our player dropped, as the container event named it, read on the next frame.
		struct DropEvent
		{
			RE::FormID    ref = 0;
			RE::FormID    base = 0;
			std::uint32_t count = 0;
			bool          byPlayer = false;  // a drop menu was open (the inventory, the console), or the dev channel dropped it
		};

		// What the sinks queue (under queueLock, never stateLock): our player's drops, and the references they activated.
		std::mutex              queueLock;
		std::vector<DropEvent>  dropQueue;
		std::vector<RE::FormID> activateQueue;
		constexpr std::size_t   MAX_QUEUED = 256;
		std::uint64_t           queueDropped = 0;

		// The dev channel's drop (`drops drop`, a script's DropObject as a weapon rack's is): the event it makes is our
		// player's own, by this flag when the engine sends it from inside the call, by the reference the call returned
		// when it sends it later. Every other drop with no drop menu open is a script's (DropRules::Givable::kScripted).
		std::atomic<bool>     devDropping{ false };
		std::set<RE::FormID>  devDrops;  // under stateLock
		constexpr std::size_t MAX_DEV_DROPS = 32;

		// Whether a menu our player drops items from is open: the inventory (Drop), or the console (player.drop).
		bool DropMenuOpen()
		{
			const auto ui = RE::UI::GetSingleton();
			return ui && (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) || ui->IsMenuOpen(RE::Console::MENU_NAME));
		}

		// Everything below, between the tick and SKSE's callbacks on the main thread. Never held while sending.
		std::mutex stateLock;

		// Our drop, coming to rest before it is reported.
		struct Pending
		{
			RE::FormID        ref = 0;
			RE::FormID        base = 0;
			Clock::time_point dropped{};
			Clock::time_point moved{};
			RE::NiPoint3      last{};
		};
		constexpr std::size_t MAX_PENDING = 64;

		// Our drop reported, waiting for its offer (by our nonce).
		struct Reported
		{
			RE::FormID        ref = 0;
			RE::FormID        base = 0;
			Clock::time_point sent{};
		};
		constexpr std::size_t MAX_REPORTED = 64;
		constexpr auto        REPORT_FORGET = std::chrono::seconds(60);  // no answer (a lost session): it stays ours

		// A take the server granted us, being finished.
		struct Grant
		{
			RE::FormID                       copy = 0;
			RE::FormID                       base = 0;
			std::uint32_t                    count = 0;
			std::int64_t                     before = 0;
			std::uint32_t                    tries = 1;
			Clock::time_point                lastTry{};
			std::optional<Clock::time_point> gone;
		};

		// A reference on its way out: disabled first, deleted later (Retire.h).
		struct Retiring
		{
			RE::FormID                       id = 0;
			RE::FormID                       base = 0;
			std::optional<Clock::time_point> disabledAt;
			std::uint64_t                    disabledTick = 0;
		};

		// One of our items going back to our world after a session (its copy, when one lies here), and the session the
		// record says it left our world in: unmade there once it is back (the record is bound to no session by then).
		struct Return
		{
			DroppedItem   item;
			RE::FormID    copy = 0;
			std::uint64_t session = 0;
		};

		bool          installed = false;
		std::uint32_t ourId = 0;
		std::uint64_t sessionId = 0;
		bool          recordDue = false;

		std::vector<Pending>                   pending;
		std::map<std::uint64_t, Reported>      reported;
		DropRules::Unconfirmed                 unconfirmed;
		DropRules::Lying                       lying;
		DropRules::Claims                      claims;
		DropRules::Record                      record;
		std::map<std::uint32_t, RE::FormID>    copies;  // drop ID -> our copy
		std::map<std::uint32_t, Grant>         grants;
		std::vector<Retiring>                  graveyard;
		std::vector<Return>                    returns;
		std::vector<std::vector<std::uint8_t>> outbox;  // from the session's handlers, sent by the next Frame
		Clock::time_point                      nextCopies{};
		constexpr auto                         COPIES_EVERY = std::chrono::milliseconds(250);
		std::mt19937_64                        nonces{ std::random_device{}() };

		// Read from the co-save at a load, taken by AfterLoad.
		std::optional<std::vector<std::uint8_t>>     loadedRecord;
		std::vector<std::pair<RE::FormID, RE::FormID>> loadedOut;  // copies the save was written without (id, base)
		std::vector<std::pair<RE::FormID, RE::FormID>> savedOut;   // ... the ones the next save is written without

		struct Counts
		{
			std::uint64_t events = 0;      // our drops the sink queued
			std::uint64_t reports = 0;     // reported
			std::uint64_t pickedUp = 0;    // picked up again before they came to rest
			std::uint64_t refused = 0;     // offers refused by the server (ours still)
			std::uint64_t swapped = 0;     // our items taken out of our world for the session's copy
			std::uint64_t kept = 0;        // offers our player had picked up again first
			std::uint64_t forgotten = 0;   // reports never answered
			std::uint64_t heard = 0;       // drops heard lying there
			std::uint64_t gone = 0;        // drops heard gone
			std::uint64_t placed = 0;      // copies placed
			std::uint64_t removed = 0;     // copies removed
			std::uint64_t lost = 0;        // copies that left the world on their own (a cell unloaded)
			std::uint64_t unplaceable = 0; // drops this game lacks a form of
			std::uint64_t claimsSent = 0;
			std::uint64_t granted = 0;
			std::uint64_t took = 0;
			std::uint64_t unsure = 0;      // taken, the count not seen to rise
			std::uint64_t gaveBack = 0;
			std::uint64_t returned = 0;    // our items given back to our world after a session
			std::uint64_t swept = 0;       // copies a load brought back, removed
			std::uint64_t offMain = 0;
			std::array<std::uint64_t, DropRules::GIVABLE_COUNT> judged{};
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
				logs::info("Drops: {} made off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); counted in `drops` as offmain",
					a_what);
			}
		}

		// A reference of ours by its ID, while it is the same thing (a runtime ID is used again once its form is gone).
		RE::TESObjectREFR* Ref(RE::FormID a_id, RE::FormID a_base)
		{
			if (a_id == 0) {
				return nullptr;
			}
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			if (!ref || ref->IsDeleted() || ref->IsMarkedForDeletion()) {
				return nullptr;
			}
			const auto base = ref->GetBaseObject();
			return base && base->GetFormID() == a_base ? ref : nullptr;
		}

		// In the world: enabled and in a cell.
		bool InWorld(RE::TESObjectREFR* a_ref)
		{
			return a_ref && !a_ref->IsDisabled() && a_ref->GetParentCell();
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

		// Where a reference is: its interior cell, or its worldspace.
		void PlaceOf(RE::TESObjectREFR* a_ref, std::uint32_t& a_cell, std::uint32_t& a_world)
		{
			a_cell = 0;
			a_world = 0;
			const auto cell = a_ref ? a_ref->GetParentCell() : nullptr;
			if (!cell) {
				return;
			}
			if (cell->IsInteriorCell()) {
				a_cell = cell->GetFormID();
			} else if (const auto world = a_ref->GetWorldspace()) {
				a_world = world->GetFormID();
			} else {
				a_cell = cell->GetFormID();
			}
		}

		std::int64_t CountOf(RE::TESBoundObject* a_base)
		{
			const auto player = Player();
			if (!player || !a_base) {
				return 0;
			}
			const auto held = player->GetInventoryCounts([&](RE::TESBoundObject& a_object) { return &a_object == a_base; });
			const auto it = held.find(a_base);
			return it != held.end() ? it->second : 0;
		}

		// The item a dropped reference is, as the wire carries it (our own IDs; FormTranslate maps them), and the facts it
		// is judged by.
		DroppedItem Describe(RE::TESObjectREFR* a_ref, DropRules::ItemFacts& a_facts)
		{
			DroppedItem item;
			const auto  base = a_ref->GetBaseObject();
			item.base = base ? base->GetFormID() : 0;
			auto& extra = a_ref->extraList;
			const auto count = extra.GetCount();
			item.count = count > 0 ? static_cast<std::uint32_t>(count) : 1;
			if (const auto health = extra.GetByType<RE::ExtraHealth>(); health && health->health > 1.0f) {
				item.health = health->health;
			}
			if (const auto ench = extra.GetByType<RE::ExtraEnchantment>(); ench && ench->enchantment) {
				item.enchantment = ench->enchantment->GetFormID();
				item.enchantmentMax = ench->charge;
				a_facts.runtimeEnchantment = FormIds::IsRuntime(item.enchantment);
			}
			if (const auto charge = extra.GetByType<RE::ExtraCharge>(); charge && item.enchantment != 0) {
				item.charge = charge->charge;
			}
			if (const auto poison = extra.GetByType<RE::ExtraPoison>(); poison && poison->poison) {
				item.poison = poison->poison->GetFormID();
				item.poisonCount = (std::min<std::uint32_t>)(poison->count, 65535);
				a_facts.runtimePoison = FormIds::IsRuntime(item.poison);
			}
			if (const auto soul = static_cast<std::int32_t>(extra.GetSoulLevel()); soul > 0 && soul <= Protocol::MAX_SOUL) {
				item.soul = static_cast<std::uint8_t>(soul);
			}
			if (const auto text = extra.GetExtraTextDisplayData(); text && text->IsPlayerSet() && !text->displayName.empty()) {
				std::string_view name{ text->displayName.c_str() };
				if (text->customNameLength > 0 && text->customNameLength < name.size()) {
					name = name.substr(0, text->customNameLength);  // without the temper's word
				}
				item.name = std::string{ name.substr(0, Protocol::MAX_ITEM_NAME_LENGTH) };
			}
			PlaceOf(a_ref, item.cell, item.worldspace);
			const auto at = a_ref->GetPosition();
			item.x = at.x;
			item.y = at.y;
			item.z = at.z;
			item.angle = a_ref->GetAngleZ();

			a_facts.kind = KindOf(base);
			a_facts.count = item.count;
			a_facts.runtimeBase = FormIds::IsRuntime(item.base);
			a_facts.quest = extra.HasQuestObjectAlias();
			return item;
		}

		// Puts the item's extras on a reference placed for it.
		void Dress(RE::TESObjectREFR* a_ref, const DroppedItem& a_item)
		{
			auto& extra = a_ref->extraList;
			if (a_item.count > 1) {
				extra.SetCount(static_cast<std::uint16_t>((std::min<std::uint32_t>)(a_item.count, Protocol::MAX_DROP_COUNT)));
			}
			if (a_item.health > 1.0f && !extra.HasType<RE::ExtraHealth>()) {
				extra.Add(new RE::ExtraHealth(a_item.health));
			}
			if (a_item.enchantment != 0) {
				if (const auto ench = RE::TESForm::LookupByID<RE::EnchantmentItem>(a_item.enchantment)) {
					extra.SetEnchantment(ench, a_item.enchantmentMax, false);
					if (a_item.charge >= 0.0f && !extra.HasType<RE::ExtraCharge>()) {
						auto charge = new RE::ExtraCharge();
						charge->charge = a_item.charge;
						extra.Add(charge);
					}
				}
			}
			if (a_item.poison != 0 && !extra.HasType<RE::ExtraPoison>()) {
				if (const auto poison = RE::TESForm::LookupByID<RE::AlchemyItem>(a_item.poison)) {
					extra.Add(new RE::ExtraPoison(poison, static_cast<std::int32_t>(a_item.poisonCount)));
				}
			}
			if (a_item.soul > 0 && a_item.soul <= Protocol::MAX_SOUL && !extra.HasType<RE::ExtraSoul>()) {
				extra.Add(new RE::ExtraSoul(static_cast<RE::SOUL_LEVEL>(a_item.soul)));
			}
			if (!a_item.name.empty()) {
				extra.SetOverrideName(a_item.name.c_str());
			}
		}

		// Places a reference of the item: at its own spot (a_atSpot) or at our player's feet. Null when it can't be.
		RE::TESObjectREFR* PlaceItem(const DroppedItem& a_item, bool a_atSpot)
		{
			const auto player = Player();
			const auto base = a_item.base != 0 ? RE::TESForm::LookupByID<RE::TESBoundObject>(a_item.base) : nullptr;
			if (!player || !base) {
				return nullptr;
			}
			NoteThread("a dropped item's copy placed");
			const auto placed = player->PlaceObjectAtMe(base, false);
			const auto ref = placed.get();
			if (!ref) {
				return nullptr;
			}
			Dress(ref, a_item);
			if (a_atSpot) {
				ref->SetPosition(RE::NiPoint3{ a_item.x, a_item.y, a_item.z });
				ref->SetAngle(RE::NiPoint3{ 0.0f, 0.0f, a_item.angle });
			}
			return ref;
		}

		void Bury(RE::FormID a_id, RE::FormID a_base)
		{
			if (a_id == 0) {
				return;
			}
			if (std::ranges::find(graveyard, a_id, &Retiring::id) == graveyard.end()) {
				graveyard.push_back({ .id = a_id, .base = a_base });
			}
		}

		// Buried with its first step taken now (disabled: out of the world and out of our player's reach this frame), the
		// delete still a second and some ticks later (Retire.h). For our own item the session's copy replaces: a Take on it
		// in between would give our player an item that also lies in the session.
		void BuryNow(RE::TESObjectREFR* a_ref, RE::FormID a_base, Clock::time_point a_now)
		{
			if (!a_ref) {
				return;
			}
			const auto id = a_ref->GetFormID();
			auto       it = std::ranges::find(graveyard, id, &Retiring::id);
			if (it == graveyard.end()) {
				graveyard.push_back({ .id = id, .base = a_base });
				it = std::prev(graveyard.end());
			}
			if (!it->disabledAt) {
				NoteThread("our dropped item disabled for the session's copy");
				a_ref->Disable();
				it->disabledAt = a_now;
				it->disabledTick = GameThread::Ticks();
			}
		}

		// Our copy of a drop goes (the drop with it, or our player walked away).
		void RemoveCopy(std::uint32_t a_dropId)
		{
			const auto it = copies.find(a_dropId);
			if (it == copies.end()) {
				return;
			}
			const auto drop = lying.Find(a_dropId);
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(it->second);
			const auto base = drop ? drop->base : (ref && ref->GetBaseObject() ? ref->GetBaseObject()->GetFormID() : 0);
			if (base != 0) {
				Bury(it->second, base);
				++counts.removed;
			}
			copies.erase(it);
		}

		RE::FormID BaseOf(RE::FormID a_ref)
		{
			const auto ref = a_ref ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref) : nullptr;
			const auto base = ref ? ref->GetBaseObject() : nullptr;
			return base ? base->GetFormID() : 0;
		}

		// The references on their way out: disabled, then deleted a second and some ticks later.
		void TendGraveyard(Clock::time_point a_now)
		{
			const auto ticks = GameThread::Ticks();
			for (auto it = graveyard.begin(); it != graveyard.end();) {
				const auto ref = Ref(it->id, it->base);
				if (!ref) {
					it = graveyard.erase(it);
					continue;
				}
				if (!it->disabledAt) {
					NoteThread("a copy disabled");
					ref->Disable();
					it->disabledAt = a_now;
					it->disabledTick = ticks;
					++it;
					continue;
				}
				const Retire::Facts facts{
					.has3D = ref->Is3DLoaded(),
					.sincePlaced = Retire::LOAD_WAIT,
					.sinceDisabled = a_now - *it->disabledAt,
					.ticksSinceDisabled = ticks - it->disabledTick,
				};
				if (Retire::Next(facts) == Retire::Step::kDelete) {
					NoteThread("a copy deleted");
					ref->SetDelete(true);
					it = graveyard.erase(it);
					continue;
				}
				++it;
			}
		}

		// Whether our player is where the item lies, and how far from it.
		bool SameSpace(const DroppedItem& a_item, float& a_distance)
		{
			const auto player = Player();
			std::uint32_t cell = 0;
			std::uint32_t world = 0;
			PlaceOf(player, cell, world);
			if ((cell == 0 && world == 0) || cell != a_item.cell || world != a_item.worldspace) {
				a_distance = -1.0f;
				return false;
			}
			a_distance = player->GetPosition().GetDistance(RE::NiPoint3{ a_item.x, a_item.y, a_item.z });
			return true;
		}

		// Our items going back to our world after a session: where they lay (our copy left there, unblocked, or placed
		// again) when our player is near, else at their feet.
		void TendReturns()
		{
			if (returns.empty() || !LocalPlayer::InWorld()) {
				return;
			}
			for (auto& back : std::exchange(returns, {})) {
				if (const auto copy = Ref(back.copy, back.item.base); copy && InWorld(copy)) {
					NoteThread("a copy made our own again");
					copy->SetActivationBlocked(false);
				} else {
					float      distance = 0;
					const bool nearby = SameSpace(back.item, distance) && distance <= DropRules::PLACE_WITHIN;
					if (!PlaceItem(back.item, nearby)) {
						logs::warn("Drops: an item of ours ({:08X} x {}) could not be given back to our world after the session (this game lacks it)", back.item.base,
							back.item.count);
						continue;
					}
				}
				// Back in our world: the session it left our world in no longer has it out (a rejoin, or a save made now
				// and loaded later in that session, sends this entry). One that could not be given back stays out.
				record.Unmade(back.session, back.item.nonce);
				++counts.returned;
			}
		}

		void Send(std::vector<std::uint8_t> a_packet)
		{
			outbox.push_back(std::move(a_packet));
		}

		void Claim(std::uint32_t a_dropId, Protocol::DropClaim a_op)
		{
			Send(Protocol::Encode(Protocol::ClaimDrop{ a_dropId, a_op }));
		}

		// Our drops: judged as they come, reported once at rest; one picked up again first is forgotten.
		void TendPending(Clock::time_point a_now)
		{
			std::vector<DropEvent> events;
			{
				std::lock_guard guard{ queueLock };
				events = std::exchange(dropQueue, {});
			}
			for (const auto& event : events) {
				++counts.events;
				const auto ref = event.ref != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(event.ref) : nullptr;
				DropRules::ItemFacts facts;
				facts.named = ref != nullptr;
				facts.created = FormIds::IsRuntime(event.ref);
				const bool dev = event.ref != 0 && devDrops.erase(event.ref) != 0;
				facts.scripted = !event.byPlayer && !dev;
				if (ref) {
					(void)Describe(ref, facts);
				}
				const auto why = DropRules::Judge(facts);
				++counts.judged[static_cast<std::size_t>(why)];
				if (why != DropRules::Givable::kYes || pending.size() >= MAX_PENDING) {
					continue;
				}
				pending.push_back({ .ref = event.ref, .base = event.base, .dropped = a_now, .moved = a_now, .last = ref->GetPosition() });
			}
			for (auto it = pending.begin(); it != pending.end();) {
				const auto ref = Ref(it->ref, it->base);
				if (!ref || !InWorld(ref)) {
					++counts.pickedUp;
					it = pending.erase(it);
					continue;
				}
				const auto at = ref->GetPosition();
				if (at.GetDistance(it->last) > DropRules::STILL_UNITS) {
					it->last = at;
					it->moved = a_now;
				}
				if (!DropRules::Settled(a_now - it->dropped, a_now - it->moved)) {
					++it;
					continue;
				}
				DropRules::ItemFacts facts;
				auto                 item = Describe(ref, facts);
				std::uint64_t        nonce = 0;
				while (nonce == 0 || reported.contains(nonce)) {
					nonce = nonces();
				}
				item.nonce = nonce;
				if (reported.size() < MAX_REPORTED) {
					reported[nonce] = { .ref = it->ref, .base = it->base, .sent = a_now };
					Send(Protocol::Encode(item, Protocol::MessageType::kReportDrop));
					++counts.reports;
				}
				it = pending.erase(it);
			}
			for (auto it = reported.begin(); it != reported.end();) {
				if (a_now - it->second.sent >= REPORT_FORGET) {
					++counts.forgotten;
					it = reported.erase(it);
				} else {
					++it;
				}
			}
		}

		// Our player's Take on a copy: a claim.
		void TendClaims(Clock::time_point a_now)
		{
			std::vector<RE::FormID> used;
			{
				std::lock_guard guard{ queueLock };
				used = std::exchange(activateQueue, {});
			}
			for (const auto id : used) {
				const auto it = std::ranges::find_if(copies, [&](const auto& a_entry) { return a_entry.second == id; });
				if (it == copies.end() || grants.contains(it->first)) {
					continue;
				}
				if (claims.Start(it->first, a_now)) {
					Claim(it->first, Protocol::DropClaim::kTake);
					++counts.claimsSent;
				}
			}
		}

		void Pick(RE::TESObjectREFR* a_copy, std::uint32_t a_count)
		{
			NoteThread("a granted copy picked up");
			a_copy->SetActivationBlocked(false);
			a_copy->ActivateRef(Player(), 0, nullptr, static_cast<std::int32_t>(a_count), true);
		}

		// The takes granted us: picked up, checked, given back when they can't be finished.
		void TendGrants(Clock::time_point a_now)
		{
			for (auto it = grants.begin(); it != grants.end();) {
				auto&      grant = it->second;
				const auto dropId = it->first;
				const auto copy = Ref(grant.copy, grant.base);
				const bool inWorld = copy && InWorld(copy);
				if (!inWorld && !grant.gone) {
					grant.gone = a_now;
				}
				const DropRules::TakeFacts facts{
					.copyInWorld = inWorld,
					.before = grant.before,
					.now = CountOf(RE::TESForm::LookupByID<RE::TESBoundObject>(grant.base)),
					.count = grant.count,
					.tries = grant.tries,
					.sinceTry = a_now - grant.lastTry,
					.sinceGone = grant.gone ? std::optional<Clock::duration>{ a_now - *grant.gone } : std::nullopt,
				};
				switch (DropRules::JudgeTake(facts)) {
				case DropRules::TakeState::kWait:
					++it;
					continue;
				case DropRules::TakeState::kRetry:
					++grant.tries;
					grant.lastTry = a_now;
					Pick(copy, grant.count);
					++it;
					continue;
				case DropRules::TakeState::kUnsure:
					++counts.unsure;
					logs::warn("Drops: drop {} left the world as we took it, but our count of {:08X} did not rise by {}; kept as taken", dropId, grant.base,
						grant.count);
					[[fallthrough]];
				case DropRules::TakeState::kDone:
					record.Took(dropId);
					++counts.took;
					copies.erase(dropId);
					break;
				case DropRules::TakeState::kGiveBack:
					logs::warn("Drops: drop {} could not be picked up in {} tries; given back to the session", dropId, grant.tries);
					if (copy) {
						Bury(grant.copy, grant.base);
					}
					copies.erase(dropId);
					Claim(dropId, Protocol::DropClaim::kGiveBack);
					++counts.gaveBack;
					break;
				}
				it = grants.erase(it);
			}
		}

		// The copies near our player: placed, removed, and forgotten when they left on their own.
		void TendCopies(Clock::time_point a_now)
		{
			if (a_now < nextCopies) {
				return;
			}
			nextCopies = a_now + COPIES_EVERY;
			for (auto it = copies.begin(); it != copies.end();) {
				const auto drop = lying.Find(it->first);
				if (grants.contains(it->first)) {
					++it;
					continue;
				}
				if (!drop) {
					const auto base = BaseOf(it->second);
					if (base != 0) {
						Bury(it->second, base);
						++counts.removed;
					}
					it = copies.erase(it);
					continue;
				}
				if (const auto copy = Ref(it->second, drop->base); !copy || !InWorld(copy)) {
					++counts.lost;  // its cell unloaded with it, or something took it out: placed again when wanted
					it = copies.erase(it);
					continue;
				}
				++it;
			}
			for (const auto& [id, drop] : lying.All()) {
				if (grants.contains(id)) {
					continue;
				}
				const bool placed = copies.contains(id);
				const bool placeable = drop.base != 0 && RE::TESForm::LookupByID<RE::TESBoundObject>(drop.base) && (drop.cell != 0 || drop.worldspace != 0);
				float      distance = -1.0f;
				const bool same = SameSpace(drop, distance);
				switch (DropRules::Want(placeable, same, distance, placed)) {
				case DropRules::CopyWant::kKeep:
					break;
				case DropRules::CopyWant::kPlace:
					if (const auto ref = PlaceItem(drop, true)) {
						ref->SetActivationBlocked(true);
						copies[id] = ref->GetFormID();
						++counts.placed;
					} else {
						++counts.unplaceable;
					}
					break;
				case DropRules::CopyWant::kRemove:
					RemoveCopy(id);
					break;
				}
			}
		}

		class Sinks :
			public RE::BSTEventSink<RE::TESContainerChangedEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Our player's drop: from our player to nobody, the reference it became named.
				if (!a_event || a_event->oldContainer != PLAYER_REF || a_event->newContainer != 0 || a_event->itemCount <= 0 || a_event->baseObj == 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto ref = a_event->reference.get();
				// Our player's own drop (a drop menu open, or the dev channel's call), else a script's.
				const bool      byPlayer = devDropping.exchange(false) || DropMenuOpen();
				std::lock_guard guard{ queueLock };
				if (dropQueue.size() < MAX_QUEUED) {
					dropQueue.push_back({ .ref = ref ? ref->GetFormID() : 0,
						.base = a_event->baseObj,
						.count = static_cast<std::uint32_t>(a_event->itemCount),
						.byPlayer = byPlayer });
				} else {
					++queueDropped;
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				// Our player's Take on a reference made at run time whose activation is blocked: maybe one of our copies.
				if (!a_event || !a_event->objectActivated || !a_event->actionRef) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto by = a_event->actionRef.get();
				const auto what = a_event->objectActivated.get();
				if (!by || by->GetFormID() != PLAYER_REF || !what || !FormIds::IsRuntime(what->GetFormID()) || !what->IsActivationBlocked()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::lock_guard guard{ queueLock };
				if (activateQueue.size() < MAX_QUEUED) {
					activateQueue.push_back(what->GetFormID());
				} else {
					++queueDropped;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sinks sinks;

		// The session's drops forgotten (the copies too: on their way out).
		void ForgetSession()
		{
			for (const auto& [id, copy] : copies) {
				if (const auto base = BaseOf(copy); base != 0) {
					Bury(copy, base);
					++counts.removed;
				}
			}
			copies.clear();
			lying.Clear();
			claims.Clear();
			grants.clear();
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
			logs::warn("Drops: the game's event source is not there; dropped items are not given");
			return false;
		}
		events->AddEventSink<RE::TESContainerChangedEvent>(&sinks);
		events->AddEventSink<RE::TESActivateEvent>(&sinks);
		installed = true;
		logs::info("Drops: watching our player's drops and takes");
		return true;
	}

	void Welcomed(std::uint32_t a_playerId, std::uint64_t a_sessionId)
	{
		std::lock_guard guard{ stateLock };
		ourId = a_playerId;
		sessionId = a_sessionId;
		ForgetSession();
		record.Bind(a_sessionId);
		recordDue = true;  // sent from the world: the server answers it with what lies there
		{
			std::lock_guard queueGuard{ queueLock };
			dropQueue.clear();  // dropped before we were in: ordinary items
			activateQueue.clear();
		}
	}

	void Frame(const SendFn& a_send)
	{
		std::vector<std::vector<std::uint8_t>> packets;
		{
			std::lock_guard guard{ stateLock };
			const auto      now = Clock::now();
			if (LocalPlayer::InWorld()) {
				// Not while items of ours are still going back to our world (Idle gives them back first): the record would
				// say they are out, and the server would lay them down again.
				if (recordDue && returns.empty()) {
					recordDue = false;
					const auto mine = record.ForSession();
					logs::info("Drops: told the session what this world holds of its drops ({} of ours out, {} taken in)", mine.made.size(), mine.taken.size());
					Send(Protocol::Encode(mine));
				}
				TendPending(now);
				TendClaims(now);
				TendGrants(now);
				TendCopies(now);
			}
			packets = std::exchange(outbox, {});
		}
		for (auto& packet : packets) {
			a_send(std::move(packet));
		}
	}

	void Idle()
	{
		std::lock_guard guard{ stateLock };
		TendGraveyard(Clock::now());
		TendReturns();  // before a new welcome's record goes: it says these are back
	}

	void Placed(const Protocol::DroppedItem& a_drop)
	{
		std::lock_guard guard{ stateLock };
		const auto      mine = a_drop.ownerId == ourId && ourId != 0 ? reported.find(a_drop.nonce) : reported.end();
		const bool      waiting = mine != reported.end();
		const RE::FormID   ourBase = waiting ? mine->second.base : 0;
		RE::TESObjectREFR* ours = waiting ? Ref(mine->second.ref, ourBase) : nullptr;
		switch (DropRules::HearPlaced(a_drop.dropId, waiting, waiting, ours && InWorld(ours))) {
		case DropRules::Offer::kRefused:
			if (waiting) {
				++counts.refused;
				reported.erase(mine);
			}
			return;
		case DropRules::Offer::kKept:
			++counts.kept;
			reported.erase(mine);
			Claim(a_drop.dropId, Protocol::DropClaim::kKept);
			return;
		case DropRules::Offer::kSwap:
			reported.erase(mine);
			if (!unconfirmed.Swapped(a_drop)) {
				++counts.kept;
				Claim(a_drop.dropId, Protocol::DropClaim::kKept);  // too many waiting: it stays ours
				return;
			}
			// Out of our world now, before kPlaced goes: a Take on it this frame would give our player the item the session's
			// copy stands for (disabled at once, deleted later).
			BuryNow(ours, ourBase, Clock::now());
			record.Made(a_drop.nonce);
			Claim(a_drop.dropId, Protocol::DropClaim::kPlaced);
			++counts.swapped;
			lying.Placed(a_drop);  // our copy of it, in its place, from the next look
			nextCopies = {};
			return;
		case DropRules::Offer::kLies:
			++counts.heard;
			unconfirmed.Heard(a_drop.dropId);
			lying.Placed(a_drop);
			return;
		}
	}

	void Gone(const Protocol::DropGone& a_gone)
	{
		std::lock_guard guard{ stateLock };
		++counts.gone;
		claims.Answered(a_gone.dropId);
		unconfirmed.Heard(a_gone.dropId);
		const auto drop = lying.Find(a_gone.dropId);
		if (a_gone.takerId != 0 && a_gone.takerId == ourId && drop && !grants.contains(a_gone.dropId)) {
			++counts.granted;
			const auto it = copies.find(a_gone.dropId);
			const auto copy = it != copies.end() ? Ref(it->second, drop->base) : nullptr;
			if (!copy || !InWorld(copy)) {
				// Granted, but our copy is gone (our player walked off, a load): it lies there again for anyone.
				Claim(a_gone.dropId, Protocol::DropClaim::kGiveBack);
				++counts.gaveBack;
				RemoveCopy(a_gone.dropId);
				lying.Gone(a_gone.dropId);
				return;
			}
			const auto base = RE::TESForm::LookupByID<RE::TESBoundObject>(drop->base);
			grants[a_gone.dropId] = { .copy = copy->GetFormID(), .base = drop->base, .count = drop->count, .before = CountOf(base), .tries = 1, .lastTry = Clock::now() };
			Pick(copy, drop->count);
			lying.Gone(a_gone.dropId);
			return;
		}
		if (!grants.contains(a_gone.dropId)) {
			RemoveCopy(a_gone.dropId);
		}
		lying.Gone(a_gone.dropId);
	}

	void Listed(const Protocol::DropList& a_list)
	{
		std::lock_guard guard{ stateLock };
		lying.BeginList();
		for (const auto& drop : a_list.drops) {
			unconfirmed.Heard(drop.dropId);
			lying.Listed(drop);
		}
		if (a_list.last) {
			for (const auto id : lying.EndList()) {
				if (!grants.contains(id)) {
					RemoveCopy(id);
				}
			}
			logs::info("Drops: {} item(s) lie in the session", lying.Size());
			nextCopies = {};
		}
	}

	void BeforeSave()
	{
		std::lock_guard guard{ stateLock };
		savedOut.clear();
		std::size_t taken = 0;
		for (auto it = copies.begin(); it != copies.end();) {
			if (grants.contains(it->first)) {
				++it;  // being picked up: ours in a moment
				continue;
			}
			if (const auto base = BaseOf(it->second); base != 0) {
				Bury(it->second, base);
				++taken;
			}
			it = copies.erase(it);
		}
		// Each is disabled now, so none is in the world the save is made of; deleted later (Retire.h), and named in the
		// co-save so a load of this save removes any it kept.
		TendGraveyard(Clock::now());
		for (const auto& out : graveyard) {
			savedOut.emplace_back(out.id, out.base);
		}
		if (taken > 0) {
			logs::info("Save: {} dropped item cop(ies) taken out of the world before the save; placed again after", taken);
		}
	}

	void AfterLoad()
	{
		std::lock_guard guard{ stateLock };
		// The world before the load is gone, its references with it: none of ours is touched again.
		copies.clear();
		graveyard.clear();
		grants.clear();  // the record decides: a take the save lacks lies there again
		claims.Clear();
		pending.clear();
		reported.clear();
		unconfirmed.Clear();
		returns.clear();
		devDrops.clear();
		{
			std::lock_guard queueGuard{ queueLock };
			dropQueue.clear();
			activateQueue.clear();
		}
		for (const auto& [id, base] : loadedOut) {
			if (const auto ref = Ref(id, base); ref && FormIds::IsRuntime(id)) {
				Bury(id, base);
				++counts.swept;
			}
		}
		if (!loadedOut.empty()) {
			logs::info("Load: the save named {} dropped item cop(ies) taken out for it", loadedOut.size());
		}
		loadedOut.clear();
		if (loadedRecord && record.Load(*loadedRecord)) {
			logs::info("Load: this save's record of dropped items holds {} session(s)", record.Sessions());
		} else {
			if (loadedRecord) {
				logs::warn("Load: a dropped items record that can't be read; this world is taken to hold none of the session's drops");
			}
			record.Clear();
		}
		loadedRecord.reset();
		if (ourId != 0) {
			record.Bind(sessionId);
			recordDue = true;
		}
	}

	void NewGame()
	{
		{
			std::lock_guard guard{ stateLock };
			loadedRecord.reset();
			loadedOut.clear();
		}
		AfterLoad();
	}

	void Clear()
	{
		std::lock_guard guard{ stateLock };
		// Ours lying there, and ours the server had not confirmed: back to our world (the server withdrew them as we left),
		// each unmade in this session's entry once it is back (TendReturns, after the record is bound to none).
		const auto              left = record.Current();
		std::set<std::uint32_t> back;
		for (const auto& drop : lying.OwnedBy(ourId)) {
			const auto it = copies.find(drop.dropId);
			returns.push_back({ drop, it != copies.end() ? it->second : 0, left });
			back.insert(drop.dropId);
		}
		for (const auto& drop : unconfirmed.TakeAll()) {
			if (back.insert(drop.dropId).second) {
				const auto it = copies.find(drop.dropId);
				returns.push_back({ drop, it != copies.end() ? it->second : 0, left });
			}
		}
		for (const auto id : back) {
			copies.erase(id);  // kept for the give-back, not removed
		}
		// A take not finished: its copy goes with the others (ForgetSession).
		ForgetSession();
		pending.clear();
		reported.clear();
		outbox.clear();
		recordDue = false;
		ourId = 0;
		sessionId = 0;
		record.Bind(0);
		if (!returns.empty()) {
			logs::info("Drops: {} item(s) of ours go back to our world as we leave the session", returns.size());
		}
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
		std::lock_guard guard{ stateLock };
		if (record.Empty() && savedOut.empty()) {
			return;  // a world that never had a drop given
		}
		if (!a_intf->OpenRecord(COSAVE_DROPS, COSAVE_DROPS_VERSION)) {
			logs::warn("Co-save: could not open the dropped items record");
			return;
		}
		const auto bytes = record.Save();
		Put(a_intf, static_cast<std::uint32_t>(bytes.size()));
		a_intf->WriteRecordData(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
		Put(a_intf, static_cast<std::uint32_t>(savedOut.size()));
		for (const auto& [id, base] : savedOut) {
			Put(a_intf, id);
			Put(a_intf, base);
		}
		logs::info("Co-save: dropped items record written ({} bytes, {} cop(ies) named)", bytes.size(), savedOut.size());
		savedOut.clear();
	}

	void ReadCoSave(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, std::uint32_t a_length)
	{
		std::lock_guard guard{ stateLock };
		loadedRecord.reset();
		loadedOut.clear();
		constexpr std::uint32_t MAX_BYTES = 1u << 20;
		constexpr std::uint32_t MAX_OUT = 4096;
		std::uint32_t           size = 0;
		if (a_version != COSAVE_DROPS_VERSION || !Get(a_intf, size) || size > MAX_BYTES || a_length < sizeof(size) + size + sizeof(std::uint32_t)) {
			logs::warn("Co-save: a dropped items record that can't be read (version {}, {} bytes)", a_version, a_length);
			return;
		}
		std::vector<std::uint8_t> bytes(size);
		if (size != 0 && a_intf->ReadRecordData(bytes.data(), size) != size) {
			logs::warn("Co-save: the dropped items record ended early");
			return;
		}
		std::uint32_t out = 0;
		if (!Get(a_intf, out) || out > MAX_OUT || a_length != sizeof(size) + size + sizeof(out) + out * 2 * sizeof(RE::FormID)) {
			logs::warn("Co-save: the dropped items record's copies can't be read");
			return;
		}
		for (std::uint32_t i = 0; i < out; ++i) {
			RE::FormID id = 0;
			RE::FormID base = 0;
			if (!Get(a_intf, id) || !Get(a_intf, base)) {
				break;
			}
			RE::FormID resolved = 0;
			if (a_intf->ResolveFormID(base, resolved)) {
				loadedOut.emplace_back(id, resolved);  // the copy's own ID is a runtime one: as it was
			}
		}
		loadedRecord = std::move(bytes);
	}

	void RevertCoSave()
	{
		std::lock_guard guard{ stateLock };
		loadedRecord.reset();
		loadedOut.clear();
	}

	namespace
	{
		std::string List()
		{
			const auto player = Player();
			std::string out = std::format("lying={}", lying.Size());
			std::size_t shown = 0;
			// What our crosshair is on (the rig's real Take: the copy in the crosshair, then the pad's a).
			const auto        pick = RE::CrosshairPickData::GetSingleton();
			const auto        aimedAt = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
			for (const auto& [id, drop] : lying.All()) {
				if (shown++ == 40) {
					break;
				}
				const auto it = copies.find(id);
				const auto copy = it != copies.end() ? Ref(it->second, drop.base) : nullptr;
				float      distance = -1.0f;
				if (player) {
					(void)SameSpace(drop, distance);
				}
				out += std::format(" | id={} owner={} base={:08X} count={} health={:.2f} ench={:08X} charge={:.0f} poison={:08X} poisonCount={} soul={} named={} copy={} blocked={} aimed={} dist={:.0f}",
					id, drop.ownerId, drop.base, drop.count, drop.health, drop.enchantment, drop.charge, drop.poison, drop.poisonCount, drop.soul,
					drop.name.empty() ? 0 : 1, copy ? std::format("{:08X}", copy->GetFormID()) : "none", copy && copy->IsActivationBlocked() ? 1 : 0,
					copy && aimedAt.get() == copy ? 1 : 0, distance);
			}
			return out;
		}

		std::string DropCommand(const std::vector<std::string_view>& a_words)
		{
			const auto itemId = a_words.size() == 3 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			const auto n = a_words.size() == 3 ? DevLine::Number(a_words[2]) : std::nullopt;
			const auto item = itemId ? RE::TESForm::LookupByID<RE::TESBoundObject>(*itemId) : nullptr;
			const auto player = Player();
			if (!item || !n || *n == 0 || *n > Protocol::MAX_DROP_COUNT || !player) {
				return "error: usage: drops drop <item hex> <count>";
			}
			if (!LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			const auto have = CountOf(item);
			if (have < static_cast<std::int64_t>(*n)) {
				return std::format("error: our player has {} of {:08X}", have, item->GetFormID());
			}
			logs::info("Drops: the developer channel has our player drop {} x {:08X}", *n, item->GetFormID());
			// A script's DropObject, which is ours here: the event it makes is our player's drop (devDropping when it is
			// sent from inside the call, devDrops when later); any other script's stays an ordinary local item.
			devDropping = true;
			const auto handle = player->DropObject(item, nullptr, static_cast<std::int32_t>(*n), nullptr, nullptr);
			devDropping = false;
			const auto ref = handle.get();
			if (ref) {
				if (devDrops.size() >= MAX_DEV_DROPS) {
					devDrops.erase(devDrops.begin());
				}
				devDrops.insert(ref->GetFormID());
			}
			return ref ? std::format("dropped ref={:08X}", ref->GetFormID()) : "dropped ref=none";
		}

		std::string TakeCommand(const std::vector<std::string_view>& a_words)
		{
			const auto id = a_words.size() == 2 ? DevLine::Number(a_words[1]) : std::nullopt;
			if (!id || *id == 0 || *id > 0xFFFFFFFF) {
				return "error: usage: drops take <drop id>";
			}
			const auto dropId = static_cast<std::uint32_t>(*id);
			const auto drop = lying.Find(dropId);
			const auto it = copies.find(dropId);
			const auto copy = drop && it != copies.end() ? Ref(it->second, drop->base) : nullptr;
			const auto player = Player();
			if (!copy || !player || !InWorld(copy)) {
				return "error: no copy of that drop here";
			}
			logs::info("Drops: the developer channel has our player take drop {}", dropId);
			// As Take on it: the activation is blocked, and the event is our claim.
			copy->ActivateRef(player, 0, nullptr, 0, false);
			return "asked";
		}

		std::string CountCommand(const std::vector<std::string_view>& a_words)
		{
			const auto itemId = a_words.size() == 2 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			const auto item = itemId ? RE::TESForm::LookupByID<RE::TESBoundObject>(*itemId) : nullptr;
			if (!item) {
				return "error: usage: drops count <item hex>";
			}
			return std::format("count={}", CountOf(item));
		}

		// ref <hex>: what became of a reference (the rig's check that a dropper's own item left its world in the swap).
		std::string RefCommand(const std::vector<std::string_view>& a_words)
		{
			const auto id = a_words.size() == 2 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			if (!id) {
				return "error: usage: drops ref <ref hex>";
			}
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(*id);
			if (!ref) {
				return std::format("ref={:08X} found=0 inworld=0", *id);
			}
			const bool deleted = ref->IsDeleted() || ref->IsMarkedForDeletion();
			return std::format("ref={:08X} found=1 inworld={} disabled={} deleted={} buried={}", *id, !deleted && InWorld(ref) ? 1 : 0, ref->IsDisabled() ? 1 : 0,
				deleted ? 1 : 0, std::ranges::find(graveyard, *id, &Retiring::id) != graveyard.end() ? 1 : 0);
		}
	}

	std::string Command(std::string_view a_args)
	{
		std::lock_guard guard{ stateLock };
		const auto      words = DevLine::Words(a_args);
		if (words.empty()) {
			std::uint64_t dropped = 0;
			{
				std::lock_guard queueGuard{ queueLock };
				dropped = queueDropped;
			}
			std::string judged;
			for (std::size_t i = 0; i < DropRules::GIVABLE_COUNT; ++i) {
				if (counts.judged[i] != 0) {
					judged += std::format("{}{}:{}", judged.empty() ? "" : ",", DropRules::GivableName(static_cast<DropRules::Givable>(i)), Counted(counts.judged[i]));
				}
			}
			const auto mine = record.ForSession();
			return std::format("installed={} us={} session={} lying={} copies={} pending={} reported={} unconfirmed={} claims={} grants={} graveyard={} returns={} "
							   "made={} taken={} events={} reports={} pickedUp={} refused={} swapped={} kept={} forgotten={} heard={} gone={} placed={} removed={} "
							   "lost={} unplaceable={} claimsSent={} granted={} took={} unsure={} gaveBack={} returned={} swept={} eventsDropped={} judged={} offmain={}",
				installed ? 1 : 0, ourId, sessionId != 0 ? 1 : 0, lying.Size(), copies.size(), pending.size(), reported.size(), unconfirmed.Size(), claims.Size(),
				grants.size(), graveyard.size(), returns.size(), mine.made.size(), mine.taken.size(), Counted(counts.events), Counted(counts.reports),
				Counted(counts.pickedUp), Counted(counts.refused), Counted(counts.swapped), Counted(counts.kept), Counted(counts.forgotten), Counted(counts.heard),
				Counted(counts.gone), Counted(counts.placed), Counted(counts.removed), Counted(counts.lost), Counted(counts.unplaceable), Counted(counts.claimsSent),
				Counted(counts.granted), Counted(counts.took), Counted(counts.unsure), Counted(counts.gaveBack), Counted(counts.returned), Counted(counts.swept),
				Counted(dropped), judged.empty() ? "-" : judged, Counted(counts.offMain));
		}
		if (words[0] == "list") {
			return List();
		}
		if (words[0] == "drop") {
			return DropCommand(words);
		}
		if (words[0] == "take") {
			return TakeCommand(words);
		}
		if (words[0] == "count") {
			return CountCommand(words);
		}
		if (words[0] == "ref") {
			return RefCommand(words);
		}
		return "error: usage: drops [list | drop <item hex> <n> | take <drop id> | count <item hex> | ref <ref hex>]";
	}
}
