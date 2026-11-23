#include "game/NpcSync.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/Gait.h"
#include "game/LocalPlayer.h"
#include "game/NpcRules.h"
#include "game/RemotePlayers.h"
#include "game/StateBuffer.h"
#include "game/VtableHook.h"

#include <array>
#include <atomic>
#include <charconv>
#include <map>
#include <mutex>
#include <numbers>
#include <set>
#include <unordered_map>
#include <unordered_set>

// Windows' headers name GetObjectA GetObject; BGSDefaultObjectManager's GetObject is meant here.
#ifdef GetObject
#	undef GetObject
#endif

namespace NpcSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Key = Protocol::NpcKey;

		constexpr auto SCAN_EVERY = 250ms;   // the loaded NPCs are looked over this often
		constexpr auto GEAR_EVERY = 1s;      // what an NPC we run wears is read this often, and sent when it changed
		constexpr auto FORGET_COPY = 10s;    // a copy with no state for this long, that nobody else runs, is forgotten
		constexpr auto RESPAWN_AFTER = 1s;   // a spawn's copy taken away is placed again no sooner than this
		constexpr std::size_t MAX_PENDING_EVENTS = 512;
		// Skyrim.esm's keyword for people (every race a person is of has it).
		constexpr RE::FormID ACTOR_TYPE_NPC = 0x00013794;

		std::atomic<bool> active{ false };  // read by the hit sink
		std::uint32_t ourId = 0;

		// Every NPC this game has loaded and shares, whoever runs it (main thread).
		struct Loaded
		{
			RE::ActorHandle                  handle;
			RE::FormID                       id = 0;
			NpcRules::Kind                   kind = NpcRules::Kind::kCreature;
			Clock::time_point                seen{};
			std::optional<Clock::time_point> unownedSince;
			std::optional<Clock::time_point> lastClaim;
			Clock::time_point                nextState{};
			std::uint32_t                    sequence = 0;
			std::optional<Protocol::NpcGear> sentGear;
			Clock::time_point                nextGear{};
		};
		std::map<Key, Loaded> loaded;
		// Who runs each NPC, as the server said (0 is never kept: nobody).
		std::map<Key, std::uint32_t> owners;
		Clock::time_point            nextScan{};

		// The copies of NPCs other games run that this game knows states of. The hook reads them, so under copyLock.
		struct Copy
		{
			std::uint32_t         runner = 0;
			std::uint32_t         base = 0;
			StateBuffer::Buffer   buffer;
			Gait::Shown           shown;
			NpcRules::ActionQueue actions;
			NpcRules::Mode        mode = NpcRules::Mode::kSilent;
			bool                  mirror = false;  // the hook moves it now
			RE::FormID            actorId = 0;     // the actor that shows it here (0: none)
			RE::ActorHandle       handle;
			bool                  spawned = false;  // that actor is one this game placed (a spawn's copy)
			Clock::time_point     placedAt{};
			Clock::time_point     placeAfter{};
			std::optional<Protocol::NpcGear> gear;
			bool                  geared = false;
			bool                  saidLook = false;
			Protocol::PlayerState lastDrawn;
			bool                  hasDrawn = false;
			std::uint64_t         drawn = 0;
			std::uint64_t         played = 0;   // runner's events its graph took
			std::uint64_t         refused = 0;  // runner's events its graph turned away
			std::uint64_t         walkRefused = 0;  // the walk's own events (Gait.h) its graph turned away
			std::string           lastRefused;
		};
		std::mutex                                copyLock;
		std::map<Key, Copy>                       copies;
		std::unordered_map<RE::Actor*, Key>       copyByActor;

		// The graph events of the NPCs we run, as the hook heard them (any thread).
		struct Heard
		{
			RE::FormID    id = 0;
			std::uint32_t sentMs = 0;
			std::string   event;
		};
		std::mutex                    eventLock;
		std::unordered_set<RE::FormID> runIds;  // the actors whose events are kept: those we run
		std::vector<Heard>            heard;
		std::atomic<std::size_t>      recording{ 0 };
		std::atomic<std::uint64_t>    eventsLost{ 0 };

		// The last moment our player fought each actor: a blow either way, or its combat target was our player (any thread).
		std::mutex                                   fightLock;
		std::unordered_map<RE::FormID, Clock::time_point> fights;

		// Counts for the developer channel.
		struct Counts
		{
			std::uint64_t claims = 0, granted = 0, refused = 0, released = 0, hands = 0;
			std::uint64_t statesSent = 0, statesIn = 0, actionsSent = 0, actionsIn = 0, gearSent = 0, gearIn = 0, gearMissing = 0, gearSkipped = 0, gearTakenOff = 0;
			std::uint64_t spawnsPlaced = 0, spawnsTaken = 0, unshared = 0, lookDiffers = 0;
			std::uint64_t fallbackSilent = 0, fallbackElsewhere = 0, fallbackFar = 0;
		};
		Counts counts;

		// ---- the graph's names (a BSFixedString takes the game's string cache's lock) ----
		const RE::BSFixedString& Name(std::string_view a_text)
		{
			static std::mutex                                            namesLock;
			static std::map<std::string, RE::BSFixedString, std::less<>> names;
			std::scoped_lock                                             l{ namesLock };
			auto                                                         it = names.find(a_text);
			if (it == names.end()) {
				it = names.emplace(std::string{ a_text }, RE::BSFixedString{ a_text }).first;
			}
			return it->second;
		}

		// ---- the graph event hook: IAnimationGraphManagerHolder::NotifyAnimationGraph of every Character ----
		// The holder's vtable in Character's is its fourth (VTABLE_Character[3]: TESObjectREFR's bases are TESForm,
		// BSHandleRefObject, BSTEventSink<BSAnimationGraphEvent>, IAnimationGraphManagerHolder); slot 1 is the notify.
		// Every event an NPC's AI sends its graph goes through it, on whatever thread sends it.
		using NotifyFn = bool (*)(RE::IAnimationGraphManagerHolder*, const RE::BSFixedString&);
		NotifyFn originalNotify = nullptr;

		bool NotifyHook(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
		{
			const bool taken = originalNotify(a_this, a_event);
			if (!taken || recording.load(std::memory_order_relaxed) == 0) {
				return taken;
			}
			const auto ref = static_cast<RE::TESObjectREFR*>(a_this);
			const auto id = ref->GetFormID();
			const auto name = a_event.c_str();
			if (!name || !NpcRules::Replayed(name)) {
				return taken;
			}
			std::scoped_lock l{ eventLock };
			if (runIds.contains(id)) {
				if (heard.size() < MAX_PENDING_EVENTS) {
					heard.push_back({ id, Protocol::SentMs(Clock::now()), std::string{ name } });
				} else {
					eventsLost.fetch_add(1, std::memory_order_relaxed);
				}
			}
			return taken;
		}

		// ---- fights ----
		struct HitSink final : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !active) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto target = a_event->target.get();
				const auto cause = a_event->cause.get();
				if (!target || !cause) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// Our player hit it, or it hit our player.
				const RE::TESObjectREFR* other = cause->IsPlayerRef() ? target : target->IsPlayerRef() ? cause : nullptr;
				if (other && other->IsActor()) {
					std::scoped_lock l{ fightLock };
					fights[other->GetFormID()] = Clock::now();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		HitSink hitSink;

		bool FoughtLately(RE::FormID a_id, Clock::time_point a_now)
		{
			std::scoped_lock l{ fightLock };
			const auto       it = fights.find(a_id);
			return it != fights.end() && a_now - it->second < NpcRules::FIGHT_HOLD;
		}

		void NoteFight(RE::FormID a_id, Clock::time_point a_now)
		{
			std::scoped_lock l{ fightLock };
			fights[a_id] = a_now;
		}

		void ForgetOldFights(Clock::time_point a_now)
		{
			std::scoped_lock l{ fightLock };
			std::erase_if(fights, [&](const auto& a_entry) { return a_now - a_entry.second >= NpcRules::FIGHT_HOLD; });
		}

		// A form id as the console shows it (hex, an 0x before it or not).
		std::optional<std::uint32_t> FormId(std::string_view a_text)
		{
			if (a_text.starts_with("0x") || a_text.starts_with("0X")) {
				a_text.remove_prefix(2);
			}
			std::uint32_t value = 0;
			const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
			if (a_text.empty() || error != std::errc{} || end != a_text.data() + a_text.size()) {
				return std::nullopt;
			}
			return value;
		}

		// ---- reading an actor ----
		std::optional<Protocol::PlayerState> Space(const RE::Actor* a_actor)
		{
			const auto cell = a_actor->GetParentCell();
			if (!cell) {
				return std::nullopt;
			}
			Protocol::PlayerState state;
			if (cell->IsInteriorCell()) {
				state.cell = cell->GetFormID();
			} else if (const auto world = a_actor->GetWorldspace()) {
				state.worldspace = world->GetFormID();
			} else {
				state.cell = cell->GetFormID();
			}
			return state;
		}

		bool SameSpace(const Protocol::PlayerState& a_a, const Protocol::PlayerState& a_b)
		{
			return a_a.cell == a_b.cell && a_a.worldspace == a_b.worldspace;
		}

		float Distance(const Protocol::PlayerState& a_a, const Protocol::PlayerState& a_b)
		{
			return std::hypot(a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z);
		}

		float GraphFloat(RE::Actor* a_actor, std::string_view a_name, float a_default)
		{
			float value = a_default;
			if (!a_actor->GetGraphVariableFloat(Name(a_name), value) || !std::isfinite(value)) {
				return a_default;
			}
			return value;
		}

		bool Flying(RE::Actor* a_actor)
		{
			const auto state = a_actor->AsActorState();
			if (!state) {
				return false;
			}
			using FS = RE::FLY_STATE;
			const auto fly = state->GetFlyState();
			return fly == FS::kTakeOff || fly == FS::kCruising || fly == FS::kHovering || fly == FS::kLanding || fly == FS::kAction;
		}

		std::optional<Protocol::PlayerState> Read(RE::Actor* a_actor)
		{
			auto state = Space(a_actor);
			if (!state) {
				return std::nullopt;
			}
			const auto at = a_actor->GetPosition();
			state->x = at.x;
			state->y = at.y;
			state->z = at.z;
			state->heading = a_actor->GetAngleZ();
			state->speed = GraphFloat(a_actor, "Speed", 0.0f);
			state->direction = Protocol::PackDirection(GraphFloat(a_actor, "Direction", 0.0f));
			state->pitch = Protocol::PackPitch(-a_actor->GetAngleX() * 180.0f / std::numbers::pi_v<float>);
			std::uint8_t flags = Protocol::kNone;
			const auto   actorState = a_actor->AsActorState();
			flags |= a_actor->IsSneaking() ? Protocol::kSneaking : 0;
			flags |= actorState && actorState->IsSprinting() ? Protocol::kSprinting : 0;
			flags |= actorState && actorState->IsWeaponDrawn() ? Protocol::kWeaponDrawn : 0;
			flags |= actorState && actorState->IsSwimming() ? Protocol::kSwimming : 0;
			flags |= a_actor->IsInMidair() ? Protocol::kJumping : 0;
			flags |= a_actor->IsOnMount() ? Protocol::kMounted : 0;
			flags |= Flying(a_actor) ? Protocol::kFlying : 0;
			state->flags = flags;
			if (!state->IsFinite()) {
				return std::nullopt;
			}
			return state;
		}

		// The NPC it was made of: its own base, or for one placed from a leveled list (whose base is a form made as the
		// game ran) the leveled pick; 0 when that is a runtime form too.
		std::uint32_t BaseOf(RE::Actor* a_actor)
		{
			const auto base = a_actor->GetActorBase();
			if (base && !FormIds::IsRuntime(base->GetFormID())) {
				return base->GetFormID();
			}
			const auto pick = a_actor->GetTemplateBase();
			return pick && !FormIds::IsRuntime(pick->GetFormID()) ? pick->GetFormID() : 0;
		}

		bool FollowsUs(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (a_actor->IsPlayerTeammate() || a_actor->GetCommandingActor().get() == a_player) {
				return true;
			}
			RE::NiPointer<RE::Actor> mount;
			return a_player->GetMount(mount) && mount.get() == a_actor;
		}

		NpcRules::Kind KindOf(RE::Actor* a_actor)
		{
			const auto race = a_actor->GetRace();
			const auto editorId = race ? std::string_view{ race->GetFormEditorID() ? race->GetFormEditorID() : "" } : std::string_view{};
			const bool commanded = a_actor->GetCommandingActor() != nullptr;
			const bool flies = race && race->data.flags.all(RE::RACE_DATA::Flag::kFlies);
			const bool mount = a_actor->IsAMount() || a_actor->IsBeingRidden() || editorId.find("Horse") != std::string_view::npos;
			const bool person = race && race->HasKeywordID(ACTOR_TYPE_NPC);
			return NpcRules::KindOf(commanded, a_actor->IsPlayerTeammate(), flies, mount, person);
		}

		// A runtime armor's plugin armor (the one it was made from: an enchanted or tempered copy), for FormIds::Shareable.
		std::uint32_t ArmorTemplateOf(std::uint32_t a_id)
		{
			const auto armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_id);
			return armor && armor->templateArmor ? armor->templateArmor->GetFormID() : 0;
		}

		std::uint32_t SlotsOf(const RE::TESBoundObject* a_object)
		{
			const auto armor = a_object ? a_object->As<RE::TESObjectARMO>() : nullptr;
			return armor ? armor->GetSlotMask().underlying() : 0;
		}

		// Held in both hands: a weapon of a two-handed type (greatsword, battleaxe, warhammer, bow, crossbow), or any
		// weapon or spell whose equip slot is none of the one-hand ones (the game's both-hands slot, which a ritual spell
		// and a mod's two-handed weapon of any type name the same way).
		bool TwoHanded(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return false;
			}
			if (const auto weapon = a_form->As<RE::TESObjectWEAP>();
				weapon && (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow())) {
				return true;
			}
			const auto type = a_form->As<RE::BGSEquipType>();
			const auto slot = type ? type->GetEquipSlot() : nullptr;
			const auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
			if (!slot || !defaults) {
				return false;
			}
			for (const auto one : { RE::DEFAULT_OBJECTS::kLeftHandEquip, RE::DEFAULT_OBJECTS::kRightHandEquip, RE::DEFAULT_OBJECTS::kEitherHandEquip,
					 RE::DEFAULT_OBJECTS::kVoiceEquip, RE::DEFAULT_OBJECTS::kPotionEquip }) {
				if (defaults->GetObject<RE::BGSEquipSlot>(one) == slot) {
					return false;
				}
			}
			return true;
		}

		// The object in each hand, a two-handed one in the right alone (NpcRules::OneReportEach), for both a runner's
		// NPC (GearOf) and a copy (Dress).
		std::pair<RE::TESForm*, RE::TESForm*> HeldObjects(RE::Actor* a_actor)
		{
			const auto right = a_actor->GetEquippedObject(false);
			const auto left = a_actor->GetEquippedObject(true);
			const auto idOf = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0u; };
			const auto hands = NpcRules::OneReportEach(idOf(right), idOf(left), TwoHanded(right), TwoHanded(left));
			const auto pick = [&](std::uint32_t a_id) -> RE::TESForm* {
				return a_id == 0 ? nullptr : a_id == idOf(right) ? right : left;
			};
			return { pick(hands.right), pick(hands.left) };
		}

		// What it wears and holds, as the others are told: a runtime armor as the plugin armor it was made from
		// (FormIds::Shareable), or left out with its body slots said; a runtime weapon or spell as an unnamed hand, never
		// as an empty one (the others' copies keep their own there: NpcRules::PlanGear).
		Protocol::NpcGear GearOf(RE::Actor* a_actor, const Key& a_key)
		{
			Protocol::NpcGear gear;
			gear.key = a_key;
			for (const auto& [item, data] : a_actor->GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); })) {
				const auto& [count, entry] = data;
				if (count <= 0 || !entry || !entry->IsWorn()) {
					continue;
				}
				const auto slots = SlotsOf(item);
				const auto shared = FormIds::Shareable(item->GetFormID(), ArmorTemplateOf);
				if (!shared || gear.worn.size() >= Protocol::MAX_WORN) {
					gear.unnamedSlots |= slots;
				} else if (std::ranges::none_of(gear.worn, [&](const Protocol::WornItem& a_w) { return a_w.id == *shared; })) {
					gear.worn.push_back({ *shared, slots });
				}
			}
			std::ranges::sort(gear.worn);
			const auto held = [&](const RE::TESForm* a_form, std::uint8_t a_unnamedBit) -> std::uint32_t {
				if (!a_form) {
					return 0;
				}
				if (FormIds::IsRuntime(a_form->GetFormID())) {
					gear.unnamed |= a_unnamedBit;
					return 0;
				}
				return a_form->GetFormID();
			};
			const auto [right, left] = HeldObjects(a_actor);
			gear.right = held(right, Protocol::kRightUnnamed);
			gear.left = held(left, Protocol::kLeftUnnamed);
			gear.ammo = held(a_actor->GetCurrentAmmo(), Protocol::kAmmoUnnamed);
			return gear;
		}

		// ---- moving a copy (the hook) ----
		void SetMoveFlags(RE::Actor* a_actor, const Gait::Step& a_step, const Protocol::PlayerState& a_state)
		{
			const auto state = a_actor->AsActorState();
			if (!state) {
				return;
			}
			auto& flags = state->actorState1;
			flags.sneaking = a_step.shown.sneaking ? 1 : 0;
			flags.sprinting = a_step.shown.sprinting ? 1 : 0;
			flags.running = a_step.shown.moving && !a_step.shown.sprinting && a_state.speed > 200.0f ? 1 : 0;
			flags.walking = a_step.shown.moving && a_state.speed <= 200.0f ? 1 : 0;
			flags.movingForward = a_step.shown.moving ? 1 : 0;
		}

		void TellCopy(RE::Actor* a_actor, const Key& a_key, const std::vector<std::string_view>& a_walk, const std::vector<std::string>& a_events)
		{
			std::uint64_t played = 0;
			std::uint64_t refused = 0;
			std::uint64_t walkRefused = 0;
			std::string   lastRefused;
			for (const auto event : a_walk) {
				if (!a_actor->NotifyAnimationGraph(Name(event))) {
					++walkRefused;
				}
			}
			for (const auto& event : a_events) {
				if (a_actor->NotifyAnimationGraph(Name(event))) {
					++played;
				} else {
					++refused;
					lastRefused = event;
				}
			}
			if (played == 0 && refused == 0 && walkRefused == 0) {
				return;
			}
			std::scoped_lock l{ copyLock };
			if (const auto it = copies.find(a_key); it != copies.end()) {
				it->second.played += played;
				it->second.refused += refused;
				it->second.walkRefused += walkRefused;
				if (!lastRefused.empty()) {
					it->second.lastRefused = lastRefused;
				}
			}
		}

		// ---- letting go of a copy's actor (main thread, under copyLock): a spawn's copy taken out of the world, an NPC's legs its own ----
		void TakeAwaySpawn(const Key& a_key, Copy& a_copy, std::string_view a_why)
		{
			const auto actor = a_copy.handle.get();
			if (a_copy.spawned && actor && actor->GetFormID() == a_copy.actorId) {
				RemotePlayers::TakeOut(actor.get(), a_copy.placedAt);
				++counts.spawnsTaken;
				logs::info("NPCs: the copy {:08X} of player {}'s spawn {:08X} taken away ({})", a_copy.actorId, a_key.spawner, a_key.id, a_why);
			} else if (actor) {
				RemotePlayers::StopLegs(actor.get());
			}
			copyByActor.erase(actor.get());
			a_copy.handle = {};
			a_copy.actorId = 0;
			a_copy.spawned = false;
			a_copy.mirror = false;
			a_copy.geared = false;
			a_copy.placeAfter = Clock::now() + RESPAWN_AFTER;
		}

		// A new actor of a_base where a_at says (outside copyLock: the game's update of it may run at once on a job thread).
		RE::Actor* PlaceActor(std::uint32_t a_base, const Protocol::PlayerState& a_at)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto base = a_base == 0 || FormIds::IsRuntime(a_base) ? nullptr : RE::TESForm::LookupByID<RE::TESNPC>(a_base);
			if (!player || !base) {
				return nullptr;
			}
			const auto placed = player->PlaceObjectAtMe(base, false);
			const auto actor = placed ? placed->As<RE::Actor>() : nullptr;
			if (!actor) {
				return nullptr;
			}
			actor->SetPosition({ a_at.x, a_at.y, a_at.z }, true);
			actor->SetHeading(a_at.heading);
			// Another game's summon: nothing to say to it here.
			actor->extraList.SetExtraFlags(static_cast<RE::ExtraFlags::Flag>(
				std::to_underlying(RE::ExtraFlags::Flag::kBlockActivate) | std::to_underlying(RE::ExtraFlags::Flag::kBlockPlayerActivate) |
				std::to_underlying(RE::ExtraFlags::Flag::kBlockActivateText)), true);
			return actor;
		}

		// ---- dressing a copy (main thread, outside copyLock) ----
		struct Dressed
		{
			std::size_t equipped = 0;
			std::size_t unequipped = 0;
			std::size_t missing = 0;
			std::size_t skipped = 0;
		};

		Dressed Dress(const Protocol::NpcGear& a_gear, bool a_mayAdd, RE::Actor* a_actor)
		{
			Dressed    out;
			const auto manager = RE::ActorEquipManager::GetSingleton();
			if (!manager) {
				return out;
			}
			NpcRules::Wearing                now;
			std::vector<NpcRules::Count>     carried;
			for (const auto& [item, data] : a_actor->GetInventory()) {
				const auto& [count, entry] = data;
				if (count <= 0) {
					continue;
				}
				carried.push_back({ item->GetFormID(), count });
				if (entry && entry->IsWorn() && item->IsArmor()) {
					now.armor.push_back({ item->GetFormID(), FormIds::Shareable(item->GetFormID(), ArmorTemplateOf).value_or(0), SlotsOf(item) });
				}
			}
			const auto idOf = [](const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0u; };
			const auto [right, left] = HeldObjects(a_actor);
			now.right = idOf(right);
			now.left = idOf(left);
			now.ammo = idOf(a_actor->GetCurrentAmmo());
			// A spell in the runner's NPC's hand is never in an inventory, so it is not looked for (a load-order copy would
			// count it missing): the hand is planned as empty and the spell counted skipped. Spells in a copy's hands come
			// with the magic work.
			auto       wanted = a_gear;
			const auto spell = [](std::uint32_t a_id) {
				const auto form = a_id != 0 ? RE::TESForm::LookupByID(a_id) : nullptr;
				return form && form->Is(RE::FormType::Spell);
			};
			for (auto* hand : { &wanted.right, &wanted.left }) {
				if (spell(*hand)) {
					*hand = 0;
					++out.skipped;
				}
			}
			const auto plan = NpcRules::PlanGear(wanted, now, carried, a_mayAdd);
			const auto defaults = RE::BGSDefaultObjectManager::GetSingleton();
			const auto slotOf = [&](NpcRules::Hand a_hand, const RE::TESForm* a_form) -> const RE::BGSEquipSlot* {
				// A one-handed weapon goes in the hand named; a two-handed one, a torch or a shield where the game puts it
				// (a two-handed weapon's own slot is both hands: a one-hand slot would be the wrong one).
				if (!defaults || a_hand == NpcRules::Hand::kNone || !a_form->Is(RE::FormType::Weapon) || TwoHanded(a_form)) {
					return nullptr;
				}
				return defaults->GetObject<RE::BGSEquipSlot>(a_hand == NpcRules::Hand::kLeft ? RE::DEFAULT_OBJECTS::kLeftHandEquip : RE::DEFAULT_OBJECTS::kRightHandEquip);
			};
			// A spell in a hand is the runner's AI's to cast; what a copy shows of it is the runner's events (the cast's
			// look on a copy comes with the magic work: docs/DESIGN.md). Neither taken off nor put on here.
			const auto object = [&](std::uint32_t a_id) -> RE::TESBoundObject* {
				const auto form = RE::TESForm::LookupByID(a_id);
				return form && !form->Is(RE::FormType::Spell) ? form->As<RE::TESBoundObject>() : nullptr;
			};
			for (const auto& item : plan.unequip) {
				const auto bound = object(item.id);
				if (!bound) {
					++out.skipped;
					continue;
				}
				manager->UnequipObject(a_actor, bound, nullptr, 1, slotOf(item.hand, bound), false, true, false, true);
				++out.unequipped;
			}
			for (const auto& add : plan.add) {
				if (const auto bound = object(add.id); bound && add.count > 0) {
					a_actor->AddObjectToContainer(bound, nullptr, add.count, nullptr);
				}
			}
			for (const auto& item : plan.equip) {
				const auto bound = object(item.id);
				if (!bound) {
					++out.skipped;
					continue;
				}
				manager->EquipObject(a_actor, bound, nullptr, 1, slotOf(item.hand, bound), false, true, false, true);
				++out.equipped;
			}
			out.missing = plan.missing;
			return out;
		}

		// ---- the loaded NPCs (main thread) ----

		// Who commands an actor: our player, an NPC this game runs (a load-order one by its own id, a runtime one as our
		// spawn), or anyone else.
		NpcRules::Commander CommanderOf(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			const auto commander = a_actor->GetCommandingActor();
			if (!commander) {
				return NpcRules::Commander::kNobody;
			}
			if (commander.get() == a_player) {
				return NpcRules::Commander::kOurPlayer;
			}
			const auto id = commander->GetFormID();
			const Key  key{ id, FormIds::IsRuntime(id) ? ourId : 0 };
			const auto owner = owners.find(key);
			return owner != owners.end() && owner->second == ourId ? NpcRules::Commander::kOurNpc : NpcRules::Commander::kOther;
		}

		void Scan(Clock::time_point a_now)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!lists || !player) {
				return;
			}
			std::set<RE::FormID> spawnCopies;
			{
				std::scoped_lock l{ copyLock };
				for (const auto& [key, copy] : copies) {
					if (copy.spawned && copy.actorId != 0) {
						spawnCopies.insert(copy.actorId);
					}
				}
			}
			std::set<Key> found;
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (const auto& handle : *handles) {
					const auto actor = handle.get();
					if (!actor || actor.get() == player || actor->IsDeleted() || actor->IsDisabled() || !actor->Is3DLoaded()) {
						continue;
					}
					const auto id = actor->GetFormID();
					if (spawnCopies.contains(id) || RemotePlayers::IsOurs(actor.get())) {
						continue;  // our stand-ins and the copies we placed are no NPC of this game's
					}
					Key key{ id, 0 };
					if (FormIds::IsRuntime(id)) {
						// Made as this game ran: shared as our spawn when our player or an NPC we run commands it (every
						// summon, raised corpse and thrall, whoever cast it); any other (a quest's or an encounter's,
						// which every game makes its own of) is not shared yet.
						if (!loaded.contains(Key{ id, ourId }) && !NpcRules::SharedAsSpawn(CommanderOf(actor.get(), player))) {
							++counts.unshared;
							continue;
						}
						key.spawner = ourId;
					}
					found.insert(key);
					auto& npc = loaded[key];
					if (npc.id != id) {
						npc = Loaded{ .handle = actor->GetHandle(), .id = id };
					}
					npc.kind = KindOf(actor.get());
					npc.seen = a_now;
				}
			}
			// Gone from this game's loaded world: one we ran is let go of (the next scan of the others' games claims it).
			for (auto it = loaded.begin(); it != loaded.end();) {
				if (!found.contains(it->first)) {
					it = loaded.erase(it);
				} else {
					++it;
				}
			}
		}

		bool Runs(const Key& a_key)
		{
			const auto it = owners.find(a_key);
			return it != owners.end() && it->second == ourId;
		}

		void SendClaims(const SendFn& a_send, Clock::time_point a_now, const std::optional<Protocol::PlayerState>& a_here, const std::vector<Protocol::PlayerState>& a_friends)
		{
			const auto           player = RE::PlayerCharacter::GetSingleton();
			Protocol::NpcClaims  msg;
			const auto           flush = [&] {
				if (!msg.claims.empty()) {
					counts.claims += msg.claims.size();
					a_send(Protocol::Encode(msg), true);
					msg.claims.clear();
				}
			};
			// The dialogue's speaker counts as a fight: talking takes an NPC over.
			if (const auto topics = RE::MenuTopicManager::GetSingleton()) {
				if (const auto speaker = topics->speaker.get()) {
					NoteFight(speaker->GetFormID(), a_now);
				}
			}
			for (auto& [key, npc] : loaded) {
				const auto actor = npc.handle.get();
				if (!actor || actor->GetFormID() != npc.id) {
					continue;
				}
				const auto owner = owners.find(key);
				NpcRules::Facts facts;
				facts.owner = owner == owners.end() ? NpcRules::Owner::kNobody : owner->second == ourId ? NpcRules::Owner::kUs : NpcRules::Owner::kOther;
				if (facts.owner == NpcRules::Owner::kNobody) {
					npc.unownedSince = npc.unownedSince.value_or(a_now);
				} else {
					npc.unownedSince.reset();
				}
				facts.unownedSince = npc.unownedSince;
				facts.lastClaim = npc.lastClaim;
				const auto at = Read(actor.get());
				if (at && a_here && SameSpace(*at, *a_here)) {
					facts.ours = Distance(*at, *a_here);
				}
				for (const auto& state : a_friends) {
					if (at && SameSpace(*at, state)) {
						const auto d = Distance(*at, state);
						facts.nearestFriend = facts.nearestFriend ? (std::min)(*facts.nearestFriend, d) : d;
					}
				}
				const auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
				if (target && target.get() == player) {
					NoteFight(npc.id, a_now);
				}
				facts.fighting = FoughtLately(npc.id, a_now);
				facts.followsUs = player && FollowsUs(actor.get(), player);
				facts.spawn = key.IsSpawn();
				const auto ask = NpcRules::Decide(facts, a_now);
				if (ask == NpcRules::Ask::kNone) {
					continue;
				}
				npc.lastClaim = a_now;
				msg.claims.push_back({ key, NpcRules::WhyOf(ask) });
				if (msg.claims.size() >= Protocol::MAX_NPC_CLAIMS) {
					flush();
				}
			}
			flush();
		}

		// The NPCs we run: their states at their rate, their gear when it changed, and their events as they came.
		void SendOurs(const SendFn& a_send, Clock::time_point a_now, const std::optional<Protocol::PlayerState>& a_here, const std::vector<Protocol::PlayerState>& a_friends)
		{
			Protocol::NpcStates states;
			const auto          flush = [&] {
				if (!states.npcs.empty()) {
					counts.statesSent += states.npcs.size();
					a_send(Protocol::Encode(states), false);
					states.npcs.clear();
				}
			};
			std::unordered_set<RE::FormID> run;
			for (auto& [key, npc] : loaded) {
				if (!Runs(key)) {
					continue;
				}
				const auto actor = npc.handle.get();
				if (!actor || actor->GetFormID() != npc.id) {
					continue;
				}
				run.insert(npc.id);
				if (a_now >= npc.nextGear) {
					npc.nextGear = a_now + GEAR_EVERY;
					auto gear = GearOf(actor.get(), key);
					if (gear != npc.sentGear) {
						a_send(Protocol::Encode(gear), true);
						npc.sentGear = std::move(gear);
						++counts.gearSent;
					}
				}
				if (a_now < npc.nextState || actor->IsDead()) {
					continue;  // a body's place is its fall's (shared with deaths), not a walk's
				}
				auto state = Read(actor.get());
				if (!state) {
					continue;
				}
				std::optional<float> nearest;
				const auto           consider = [&](const Protocol::PlayerState& a_player) {
					if (SameSpace(*state, a_player)) {
						const auto d = Distance(*state, a_player);
						nearest = nearest ? (std::min)(*nearest, d) : d;
					}
				};
				if (a_here) {
					consider(*a_here);
				}
				for (const auto& friendState : a_friends) {
					consider(friendState);
				}
				const auto every = NpcRules::SendEvery(actor->IsInCombat(), nearest);
				npc.nextState = a_now - npc.nextState < every ? npc.nextState + every : a_now + every;
				state->sequence = ++npc.sequence;
				state->sentMs = Protocol::SentMs(a_now);
				states.npcs.push_back({ key, BaseOf(actor.get()), *state });
				if (states.npcs.size() >= Protocol::MAX_NPC_STATES) {
					flush();
				}
			}
			flush();

			// The events the hook heard of the ones we run, and from now on those of the ones we run now.
			std::vector<Heard> events;
			{
				std::scoped_lock l{ eventLock };
				events.swap(heard);
				runIds = std::move(run);
				recording.store(runIds.size(), std::memory_order_relaxed);
			}
			if (events.empty()) {
				return;
			}
			std::unordered_map<RE::FormID, Key> keyOf;
			for (const auto& [key, npc] : loaded) {
				keyOf.emplace(npc.id, key);
			}
			Protocol::NpcActions actions;
			for (auto& event : events) {
				const auto key = keyOf.find(event.id);
				if (key == keyOf.end() || !Runs(key->second)) {
					continue;
				}
				actions.actions.push_back({ key->second, event.sentMs, std::move(event.event) });
				if (actions.actions.size() >= Protocol::MAX_NPC_ACTIONS) {
					counts.actionsSent += actions.actions.size();
					a_send(Protocol::Encode(actions), true);
					actions.actions.clear();
				}
			}
			if (!actions.actions.empty()) {
				counts.actionsSent += actions.actions.size();
				a_send(Protocol::Encode(actions), true);
			}
		}

		// The copies: each one's actor here found (a load-order NPC's own reference, or a spawn's copy placed), whether
		// the hook moves it now, and its gear put on.
		void TendCopies(Clock::time_point a_now, const std::optional<Protocol::PlayerState>& a_here)
		{
			struct Work
			{
				Key                   key;
				bool                  place = false;
				std::uint32_t         base = 0;
				Protocol::PlayerState at;
			};
			std::vector<Work> work;
			{
				std::scoped_lock l{ copyLock };
				for (auto it = copies.begin(); it != copies.end();) {
					auto& [key, copy] = *it;
					const auto owner = owners.find(key);
					const bool nobody = owner == owners.end();
					const bool othersRun = !nobody && owner->second != ourId;
					copy.buffer.Frame(a_now);
					const auto sample = copy.buffer.Sample(a_now);
					// Ours now, or nobody's for a while: forgotten (a spawn's copy goes with it). Nobody's a short while:
					// kept (its own AI shows it) until another game takes it.
					if (!othersRun) {
						const bool fresh = !copy.buffer.snapshots.empty() && a_now - copy.buffer.snapshots.back().stamp < FORGET_COPY;
						if (!nobody || !fresh) {
							const auto held = copy.handle.get();
							TakeAwaySpawn(key, copy, nobody ? "nobody runs it" : "run here now");
							if (held) {
								RemotePlayers::StopLegs(held.get());
							}
							it = copies.erase(it);
							continue;
						}
					}
					// The actor that shows it here.
					RE::Actor* actor = nullptr;
					if (key.IsSpawn()) {
						const auto held = copy.handle.get();
						actor = held && held->GetFormID() == copy.actorId && !held->IsDeleted() ? held.get() : nullptr;
						if (!actor && copy.actorId != 0) {
							TakeAwaySpawn(key, copy, "gone from the world");
						}
					} else {
						const auto found = othersRun ? RE::TESForm::LookupByID<RE::Actor>(key.id) : nullptr;
						actor = found && !found->IsDeleted() && !found->IsDisabled() && found->Is3DLoaded() && found->GetParentCell() && !found->IsPlayerRef() ? found : nullptr;
						if (actor && copy.actorId != actor->GetFormID()) {
							copy.handle = actor->GetHandle();
							copy.actorId = actor->GetFormID();
							copy.geared = false;
						} else if (!actor && copy.actorId != 0) {
							if (const auto was = copy.handle.get()) {
								RemotePlayers::StopLegs(was.get());
							}
							copy.handle = {};
							copy.actorId = 0;
						}
					}
					const bool sameSpace = sample && a_here && SameSpace(sample->state, *a_here);
					const auto mode = NpcRules::CopyMode(sample.has_value() && othersRun, sameSpace, sameSpace ? std::optional{ Distance(sample->state, *a_here) } : std::nullopt);
					if (mode != copy.mode && copy.mode == NpcRules::Mode::kMirror) {
						counts.fallbackSilent += mode == NpcRules::Mode::kSilent ? 1 : 0;
						counts.fallbackElsewhere += mode == NpcRules::Mode::kElsewhere ? 1 : 0;
						counts.fallbackFar += mode == NpcRules::Mode::kFar ? 1 : 0;
					}
					copy.mode = mode;
					if (key.IsSpawn()) {
						// A spawn's copy is there only while it is mirrored: left to its own AI, another game's summon
						// would be nobody's here.
						const bool want = mode == NpcRules::Mode::kMirror && copy.base != 0;
						if (want && !actor && a_now >= copy.placeAfter) {
							work.push_back({ .key = key, .place = true, .base = copy.base, .at = sample->state });
						} else if (!want && actor) {
							TakeAwaySpawn(key, copy, NpcRules::ModeName(mode));
							actor = nullptr;
						}
					}
					const bool mirror = actor && mode == NpcRules::Mode::kMirror;
					if (copy.mirror && !mirror) {
						copy.actions.Clear();
						if (actor) {
							RemotePlayers::StopLegs(actor);
						}
					}
					if (mirror && !copy.mirror) {
						// Its own AI may have changed what it wears and holds meanwhile (a torch lit, a weapon drawn): its
						// runner's gear is put on again.
						copy.geared = false;
					}
					copy.mirror = mirror;
					if (actor) {
						if (copy.gear && !copy.geared) {
							work.push_back({ .key = key });
						}
						if (!copy.saidLook && copy.base != 0 && !key.IsSpawn()) {
							copy.saidLook = true;
							if (const auto here = BaseOf(actor); here != copy.base) {
								++counts.lookDiffers;
								logs::info("NPCs: {:08X} is made of {:08X} here and of {:08X} in its runner's game (a leveled pick of its own): it looks as this game made it", key.id,
									here, copy.base);
							}
						}
					}
					++it;
				}
				// Which actor shows which copy, made again (an actor's pointer is looked at, never followed: it may be gone).
				copyByActor.clear();
				for (const auto& [key, copy] : copies) {
					if (copy.actorId == 0) {
						continue;
					}
					if (const auto held = copy.handle.get(); held && held->GetFormID() == copy.actorId) {
						copyByActor[held.get()] = key;
					}
				}
				RemotePlayers::SetCopies(copyByActor.size());
			}
			// What calls the game at length (placing, equipping) runs outside the lock: the hook takes it on job threads,
			// and a placed actor's first update may come at once.
			for (const auto& job : work) {
				if (job.place) {
					const auto actor = PlaceActor(job.base, job.at);
					std::scoped_lock l{ copyLock };
					const auto       it = copies.find(job.key);
					if (!actor) {
						logs::warn("NPCs: the game placed no actor for player {}'s spawn {:08X} (base {:08X})", job.key.spawner, job.key.id, job.base);
						if (it != copies.end()) {
							it->second.placeAfter = a_now + 10s;
						}
						continue;
					}
					if (it == copies.end() || it->second.actorId != 0) {
						RemotePlayers::TakeOut(actor, a_now);  // no longer wanted, or shown already
						continue;
					}
					auto& copy = it->second;
					copy.handle = actor->GetHandle();
					copy.actorId = actor->GetFormID();
					copy.spawned = true;
					copy.placedAt = a_now;
					copy.geared = false;
					copy.mirror = true;
					copyByActor[actor] = job.key;
					RemotePlayers::SetCopies(copyByActor.size());
					++counts.spawnsPlaced;
					logs::info("NPCs: player {}'s spawn {:08X} shown by a copy {:08X} of {:08X} at ({:.0f},{:.0f},{:.0f})", job.key.spawner, job.key.id, copy.actorId, job.base, job.at.x,
						job.at.y, job.at.z);
					continue;
				}
				RE::NiPointer<RE::Actor> actor;
				Protocol::NpcGear        gear;
				bool                     mayAdd = false;
				{
					std::scoped_lock l{ copyLock };
					const auto       it = copies.find(job.key);
					if (it == copies.end() || !it->second.gear || it->second.actorId == 0) {
						continue;
					}
					actor = it->second.handle.get();
					gear = *it->second.gear;
					mayAdd = it->second.spawned;
				}
				if (!actor || !actor->Is3DLoaded()) {
					continue;
				}
				const auto dressed = Dress(gear, mayAdd, actor.get());
				std::scoped_lock l{ copyLock };
				if (const auto it = copies.find(job.key); it != copies.end() && it->second.actorId == actor->GetFormID() && it->second.gear == gear) {
					it->second.geared = true;
				}
				counts.gearMissing += dressed.missing;
				counts.gearSkipped += dressed.skipped;
				counts.gearTakenOff += dressed.unequipped;
				logs::debug("NPCs: copy {:08X} of {:08X} took off {} and put on {} of its runner's items ({} it lacks, {} left to its runner)", actor->GetFormID(), job.key.id, dressed.unequipped, dressed.equipped,
					dressed.missing, dressed.skipped);
			}
		}

		Copy& CopyOf(const Key& a_key, std::uint32_t a_runner)  // under copyLock
		{
			auto& copy = copies[a_key];
			if (copy.runner != a_runner) {
				// A new runner: its states count and its clock start over.
				copy.runner = a_runner;
				copy.buffer.Clear();
				copy.actions.Clear();
				copy.shown = {};
			}
			return copy;
		}
	}

	bool Install()
	{
		if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(&hitSink);
		}
		REL::Relocation<std::uintptr_t> holder{ RE::VTABLE_Character[3] };
		if (!VtableHook::Slot<NotifyFn>(holder.address(), 1, &NotifyHook, originalNotify, "NPCs", "Character's graph notify")) {
			logs::warn("NPCs: without the graph notify hook, copies of the NPCs this game runs show their walk but not their attacks or other actions");
			return false;
		}
		return true;
	}

	void Started(std::uint32_t a_ourId)
	{
		Clear();
		ourId = a_ourId;
		active = true;
		logs::info("NPCs: in a session as player {}: the NPCs near us are shared", ourId);
	}

	void Frame(const SendFn& a_send)
	{
		if (!active) {
			return;
		}
		const auto now = Clock::now();
		const auto here = LocalPlayer::Read();
		if (!here) {
			return;  // a loading screen or a menu with no world: nothing loaded to share
		}
		const auto friends = RemotePlayers::FriendStates();
		if (now >= nextScan) {
			nextScan = now + SCAN_EVERY;
			// What we ran and no longer have loaded: let go of.
			std::vector<Key> before;
			for (const auto& [key, owner] : owners) {
				if (owner == ourId) {
					before.push_back(key);
				}
			}
			Scan(now);
			ForgetOldFights(now);
			Protocol::NpcClaims releases;
			for (const auto& key : before) {
				if (!loaded.contains(key) && releases.claims.size() < Protocol::MAX_NPC_CLAIMS) {
					releases.claims.push_back({ key, Protocol::ClaimWhy::kRelease });
				}
			}
			if (!releases.claims.empty()) {
				counts.claims += releases.claims.size();
				a_send(Protocol::Encode(releases), true);
			}
			SendClaims(a_send, now, here, friends);
		}
		SendOurs(a_send, now, here, friends);
		TendCopies(now, here);
	}

	void Clear()
	{
		{
			std::scoped_lock l{ copyLock };
			for (auto& [key, copy] : copies) {
				TakeAwaySpawn(key, copy, "the session ended");
			}
			copies.clear();
			copyByActor.clear();
			RemotePlayers::SetCopies(0);
		}
		{
			std::scoped_lock l{ eventLock };
			runIds.clear();
			heard.clear();
			recording = 0;
		}
		{
			std::scoped_lock l{ fightLock };
			fights.clear();
		}
		loaded.clear();
		owners.clear();
		active = false;
		ourId = 0;
	}

	void Owners(const Protocol::NpcOwners& a_msg)
	{
		if (!active) {
			return;
		}
		if (a_msg.full) {
			owners.clear();
		}
		for (const auto& [key, owner] : a_msg.owners) {
			const auto it = owners.find(key);
			const auto was = it == owners.end() ? 0u : it->second;
			if (owner == 0) {
				owners.erase(key);
				counts.released += was != 0 ? 1 : 0;
				continue;
			}
			owners[key] = owner;
			if (was == owner) {
				// a refused claim's answer: someone else runs it
				counts.refused += owner != ourId ? 1 : 0;
				continue;
			}
			++counts.granted;
			if (was != 0) {
				++counts.hands;
			}
			if (owner == ourId || was == ourId) {
				if (const auto npc = loaded.find(key); npc != loaded.end()) {
					npc->second.sentGear.reset();  // the new runner's gear goes out at once
					npc->second.nextGear = {};
				}
				logs::info("NPCs: {:08X}{} is run {} now{}", key.id, key.IsSpawn() ? std::format(" (player {}'s spawn)", key.spawner) : "",
					owner == ourId ? "here" : std::format("by player {}", owner), was != 0 ? std::format(" (was player {})", was) : "");
			}
			if (owner == ourId) {
				// Ours now: its copy, if this game showed one, is let go of; its AI runs here from where it stands.
				std::scoped_lock l{ copyLock };
				if (const auto copy = copies.find(key); copy != copies.end()) {
					if (const auto actor = copy->second.handle.get()) {
						copyByActor.erase(actor.get());
						RemotePlayers::StopLegs(actor.get());
					}
					copies.erase(copy);
					RemotePlayers::SetCopies(copyByActor.size());
				}
			}
		}
	}

	void States(const Protocol::NpcStates& a_msg)
	{
		if (!active || a_msg.runner == ourId) {
			return;
		}
		const auto       now = Clock::now();
		std::scoped_lock l{ copyLock };
		for (const auto& npc : a_msg.npcs) {
			if (Runs(npc.key) || (copies.size() >= Protocol::MAX_NPCS && !copies.contains(npc.key))) {
				continue;  // ours now (an old runner's last states), or too many
			}
			auto& copy = CopyOf(npc.key, a_msg.runner);
			copy.base = npc.base;
			copy.buffer.Push(npc.state, now);
			++counts.statesIn;
		}
	}

	void Actions(const Protocol::NpcActions& a_msg)
	{
		if (!active || a_msg.runner == ourId) {
			return;
		}
		const auto       now = Clock::now();
		std::scoped_lock l{ copyLock };
		for (const auto& action : a_msg.actions) {
			const auto it = copies.find(action.key);
			if (it == copies.end() || it->second.runner != a_msg.runner || !NpcRules::Replayed(action.event)) {
				continue;
			}
			it->second.actions.Push(action.event, it->second.buffer.sender.At(action.sentMs), now);
			++counts.actionsIn;
		}
	}

	void Gear(const Protocol::NpcGear& a_msg)
	{
		if (!active || a_msg.runner == ourId || a_msg.key.id == 0) {
			return;  // id 0: an NPC this game lacks (FormTranslate.h)
		}
		std::scoped_lock l{ copyLock };
		if (Runs(a_msg.key)) {
			return;
		}
		auto& copy = CopyOf(a_msg.key, a_msg.runner);
		copy.gear = a_msg;
		copy.geared = false;
		++counts.gearIn;
	}

	bool UpdateCopy(RE::Actor* a_actor, float a_delta)
	{
		const auto                          now = Clock::now();
		Key                                 key;
		std::optional<StateBuffer::Sampled> sample;
		Gait::Step                          step;
		std::vector<std::string>            events;
		{
			std::scoped_lock l{ copyLock };
			const auto       byActor = copyByActor.find(a_actor);
			if (byActor == copyByActor.end()) {
				return false;
			}
			key = byActor->second;
			const auto it = copies.find(key);
			if (it == copies.end() || !it->second.mirror || it->second.actorId != a_actor->GetFormID()) {
				return false;
			}
			auto& copy = it->second;
			sample = copy.buffer.Sample(now);
			if (!sample) {
				return false;
			}
			step = Gait::Next(copy.shown, sample->state, sample->held);
			copy.shown = step.shown;
			events = copy.actions.Due(now - StateBuffer::Buffer::Delay());
			copy.lastDrawn = sample->state;
			copy.hasDrawn = true;
			++copy.drawn;
		}
		// Its AI does not run: where its runner has it, turned its way (a flyer pitched too), its legs its runner's.
		a_actor->SetPosition({ sample->state.x, sample->state.y, sample->state.z }, true);
		if ((sample->state.flags & Protocol::kFlying) != 0) {
			const float pitch = -static_cast<float>(sample->state.pitch) * std::numbers::pi_v<float> / 180.0f;
			a_actor->SetAngle({ pitch, 0.0f, sample->state.heading });
		} else {
			a_actor->SetHeading(sample->state.heading);
		}
		SetMoveFlags(a_actor, step, sample->state);
		RemotePlayers::DriveLegs(a_actor, { step.speed, step.direction });
		a_actor->UpdateNoAI(a_delta);
		TellCopy(a_actor, key, step.events, events);
		return true;
	}

	void ForgetWorld()
	{
		if (!active) {
			return;
		}
		// The load takes every actor of the old world: copies are found again in the new one (a spawn's copy placed
		// again); what we ran is let go of in the next frame (Frame's releases: none of it is loaded now).
		std::scoped_lock l{ copyLock };
		for (auto& [key, copy] : copies) {
			copy.handle = {};
			copy.actorId = 0;
			copy.spawned = false;
			copy.mirror = false;
			copy.geared = false;
			copy.actions.Clear();
		}
		copyByActor.clear();
		RemotePlayers::SetCopies(0);
		loaded.clear();
	}

	void BeforeSave()
	{
		std::scoped_lock l{ copyLock };
		std::size_t      taken = 0;
		for (auto& [key, copy] : copies) {
			if (copy.spawned && copy.actorId != 0) {
				TakeAwaySpawn(key, copy, "a save is written");
				++taken;
			}
		}
		RemotePlayers::SetCopies(copyByActor.size());
		if (taken > 0) {
			logs::info("Save: {} copy(ies) of other players' spawns taken out of the world before the save; placed again after", taken);
		}
	}

	std::string Describe(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		std::array<std::size_t, static_cast<std::size_t>(NpcRules::Kind::kCount)> kinds{};
		std::size_t run = 0;
		for (const auto& [key, npc] : loaded) {
			++kinds[static_cast<std::size_t>(npc.kind)];
			run += Runs(key) ? 1 : 0;
		}
		std::string kindList;
		for (std::size_t i = 0; i < kinds.size(); ++i) {
			kindList += std::format("{}{}:{}", kindList.empty() ? "" : ",", NpcRules::KindName(static_cast<NpcRules::Kind>(i)), kinds[i]);
		}
		std::scoped_lock l{ copyLock };
		std::size_t      mirrored = 0;
		std::size_t      shown = 0;
		std::size_t      spawnCopies = 0;
		std::uint64_t    played = 0;
		std::uint64_t    refused = 0;
		std::uint64_t    walkRefused = 0;
		for (const auto& [key, copy] : copies) {
			mirrored += copy.mirror ? 1 : 0;
			shown += copy.actorId != 0 ? 1 : 0;
			spawnCopies += copy.spawned && copy.actorId != 0 ? 1 : 0;
			played += copy.played;
			refused += copy.refused;
			walkRefused += copy.walkRefused;
		}
		if (words.empty()) {
			return std::format(
				"active={} loaded={} run={} owners={} copies={} shown={} mirrored={} spawnCopies={} kinds={} claims={} granted={} refused={} released={} hands={} statesSent={} statesIn={} "
				"actionsSent={} actionsIn={} played={} refusedEvents={} walkRefused={} eventsLost={} gearSent={} gearIn={} gearMissing={} gearSkipped={} gearTakenOff={} spawnsPlaced={} "
				"spawnsTaken={} unshared={} lookDiffers={} fallbacks={}/{}/{} hooked={}",
				active ? 1 : 0, loaded.size(), run, owners.size(), copies.size(), shown, mirrored, spawnCopies, kindList, counts.claims, counts.granted, counts.refused, counts.released,
				counts.hands, counts.statesSent, counts.statesIn, counts.actionsSent, counts.actionsIn, played, refused, walkRefused, eventsLost.load(), counts.gearSent, counts.gearIn,
				counts.gearMissing, counts.gearSkipped, counts.gearTakenOff, counts.spawnsPlaced, counts.spawnsTaken, counts.unshared, counts.lookDiffers, counts.fallbackSilent, counts.fallbackElsewhere,
				counts.fallbackFar, originalNotify ? 1 : 0);
		}
		// One line per NPC: `npcs list`, or `npcs <form id>` for one. Its local form id, who runs it, its kind, where its
		// actor is here (at=) and, for a copy, where its runner's state puts it (target=) and how far apart they are.
		const auto only = words[0] == "list" ? std::nullopt : FormId(words[0]);
		if (words[0] != "list" && !only) {
			return "error: usage: npcs [list | <form id>]";
		}
		std::set<Key> keys;
		for (const auto& [key, npc] : loaded) {
			keys.insert(key);
		}
		for (const auto& [key, copy] : copies) {
			keys.insert(key);
		}
		std::string out = std::format("npcs={}", keys.size());
		std::size_t listed = 0;
		for (const auto& key : keys) {
			const auto npc = loaded.find(key);
			const auto copy = copies.find(key);
			const auto localId = npc != loaded.end() ? npc->second.id : copy != copies.end() ? copy->second.actorId : 0;
			if (only && *only != key.id && *only != localId) {
				continue;
			}
			if (listed++ >= 60) {
				out += " | ...";
				break;
			}
			const auto owner = owners.find(key);
			const auto runner = owner == owners.end() ? 0u : owner->second;
			RE::Actor* actor = nullptr;
			if (npc != loaded.end()) {
				const auto held = npc->second.handle.get();
				actor = held && held->GetFormID() == npc->second.id ? held.get() : nullptr;
			} else if (copy != copies.end() && copy->second.actorId != 0) {
				const auto held = copy->second.handle.get();
				actor = held && held->GetFormID() == copy->second.actorId ? held.get() : nullptr;
			}
			std::string at = "none";
			if (actor) {
				const auto p = actor->GetPosition();
				at = std::format("({:.0f},{:.0f},{:.0f})", p.x, p.y, p.z);
			}
			const auto role = runner == ourId ? "run" : copy != copies.end() && copy->second.mirror ? "copy" : runner != 0 ? "other" : "none";
			out += std::format(" | {:08X}{} local={:08X} owner={} role={} kind={} at={} dead={}", key.id, key.IsSpawn() ? std::format(" spawner={}", key.spawner) : "", localId,
				runner, role, npc != loaded.end() ? NpcRules::KindName(npc->second.kind) : "?", at, actor && actor->IsDead() ? 1 : 0);
			if (copy != copies.end()) {
				const auto& c = copy->second;
				std::string target = "none";
				std::string apart = "?";
				if (c.hasDrawn) {
					target = std::format("({:.0f},{:.0f},{:.0f})", c.lastDrawn.x, c.lastDrawn.y, c.lastDrawn.z);
					if (actor) {
						const auto p = actor->GetPosition();
						apart = std::format("{:.0f}", std::hypot(p.x - c.lastDrawn.x, p.y - c.lastDrawn.y, p.z - c.lastDrawn.z));
					}
				}
				float graphSpeed = 0.0f;
				const bool hasSpeed = actor && actor->GetGraphVariableFloat(Name("Speed"), graphSpeed);
				// played/refused: the runner's events; walkRefused: the walk's own (Gait.h); gear: its runner's gear came,
				// geared: it is on.
				out += std::format(" runner={} mode={} target={} apart={} base={:08X} states={} drawn={} played={} refused={} walkRefused={}{} gear={} geared={} graphSpeed={}",
					c.runner, NpcRules::ModeName(c.mode), target, apart, c.base, c.buffer.snapshots.size(), c.drawn, c.played, c.refused, c.walkRefused,
					c.lastRefused.empty() ? "" : " last=" + c.lastRefused, c.gear ? 1 : 0, c.geared ? 1 : 0, hasSpeed ? std::format("{:.0f}", graphSpeed) : "none");
			}
		}
		return out;
	}
}
