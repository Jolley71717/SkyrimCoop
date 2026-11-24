#include "game/NpcSync.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/CopyHealth.h"
#include "game/Furniture.h"
#include "game/PairRules.h"
#include "game/Pairs.h"
#include "game/Gait.h"
#include "game/Gear.h"
#include "game/LocalPlayer.h"
#include "game/NpcRules.h"
#include "game/PeaceRules.h"
#include "game/RemotePlayers.h"
#include "game/Riding.h"
#include "game/SpellRules.h"
#include "game/StateBuffer.h"
#include "game/VtableHook.h"
#include "game/Water.h"

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
		// A copy's gear is read back this long after it was dressed (the game's equips may land a frame or two later), and
		// this often after that (its own scripts may change it).
		constexpr auto WEAR_CHECK_AFTER = 500ms;
		constexpr auto WEAR_CHECK_EVERY = 2s;
		// A mirrored copy holding a spell or a voice has the magicka it spent here given back this often (SpellRules::MagickaBack).
		constexpr auto MAGICKA_EVERY = 250ms;
		constexpr std::size_t MAX_PENDING_EVENTS = 512;
		// Skyrim.esm's keyword for people (every race a person is of has it).
		constexpr RE::FormID ACTOR_TYPE_NPC = 0x00013794;

		std::atomic<bool> active{ false };  // read by the hit sink
		std::uint32_t ourId = 0;
		std::uint32_t hostId = 0;

		// A friend's hit on an NPC we run counts as theirs at its death for this long (its killer, for the others).
		constexpr auto KILLER_HOLD = 5s;

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
			std::optional<Clock::time_point> saidFollows;  // NpcRules::Facts::saidFollows
			std::optional<Protocol::NpcGear> sentGear;
			Clock::time_point                nextGear{};
			// Its health and death, while we run it (CopyHealth::Reporter; deathSent: its death went out).
			CopyHealth::Reporter             reporter;
			bool                             deathSent = false;
			std::uint32_t                    lastHitter = 0;  // the friend whose hit we applied last, and when
			Clock::time_point                lastHitAt{};
			std::optional<Clock::time_point> killMoveAt;  // last seen in a kill move
			bool                             aliveUnrun = false;  // last seen alive while no other game ran it (a death here after is ours to say)
			std::uint64_t                    castsSent = 0;  // its graph's cast events said (SpellRules::CastEvent), while we ran it
		};
		std::map<Key, Loaded> loaded;
		// The numbers of the states of every NPC we ran this session (not forgotten with its loaded entry).
		NpcRules::Sequences sequences;
		// Who runs each NPC, as the server said (0 is never kept: nobody).
		std::map<Key, std::uint32_t> owners;
		Clock::time_point            nextScan{};

		// The copies of NPCs other games run that this game knows states of. The hook reads them, so under copyLock.
		struct Copy
		{
			NpcRules::Feed        feed;  // whose states it holds
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
			std::size_t           gearLacks = 0;  // what its last dressing counted missing (PlanGear)
			std::optional<NpcRules::GearShown> wears;  // its gear read back after it was dressed (none: not yet)
			Clock::time_point     nextWearCheck{};
			bool                  saidLook = false;
			Protocol::PlayerState lastDrawn;
			bool                  hasDrawn = false;
			std::uint64_t         drawn = 0;
			std::uint64_t         played = 0;   // runner's events its graph took
			std::uint64_t         refused = 0;  // runner's events its graph turned away
			std::uint64_t         walkRefused = 0;  // the walk's own events (Gait.h) its graph turned away
			std::uint64_t         swimTaken = 0;    // of those, the swim events taken and turned away, the changes given up
			std::uint64_t         swimRefused = 0;
			std::uint64_t         swimGivenUp = 0;
			std::uint64_t         refloats = 0;  // the water moved it off the height it is shown at (RemotePlayers::HoldDepth)
			float                 drift = 0.0f;  // the engine's last update's move off its height while swimming
			std::string           lastRefused;
			// Spells (SpellRules.h): another game runs its NPC (the effect hook refuses what such a copy makes); it holds a
			// spell or a voice (its magicka is kept up while mirrored); its runner's cast events its graph took and turned
			// away, and the last cast it played; the magicka given back.
			bool                  othersRun = false;
			bool                  holdsMagic = false;
			std::uint64_t         castsPlayed = 0;
			std::uint64_t         castsRefused = 0;
			std::string           lastCast;
			float                 magickaBack = 0.0f;
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
		std::atomic<std::uint64_t>    heldActionsDropped{ 0 };  // a copy's runner's actions dropped while a pair held it (Pairs::Held)
		std::uint64_t                 pairActionsSent = 0;      // events of a pair's (PairRules::PairEvent) sent as an NPC's actions: none should be

		// The last moment our player fought each actor: a blow either way, or its combat target was our player (any thread).
		std::mutex                                   fightLock;
		std::unordered_map<RE::FormID, Clock::time_point> fights;
		// Our player's last hit on each actor, from its hit event (under fightLock): what a drop on a copy is judged by.
		std::unordered_map<RE::FormID, CopyHealth::OurHit> ourHits;

		// ---- health and deaths (docs/DESIGN.md, "NPC health and deaths"; main thread) ----
		std::map<Key, CopyHealth::Copy>  health;  // each copy's (CopyHealth.h), by its NPC
		CopyHealth::Counts               healthCounts;
		std::uint32_t                    hitSeq = 0;      // our hits on copies, numbered for the session
		Protocol::NpcHitAnswers          answersOwed;     // to friends' hits on NPCs we run: sent next frame, before healths
		std::set<Key>                    hitApplied;      // NPCs we run whose health goes out at once (a friend's hit applied)
		std::map<Key, Protocol::NpcDeath> sessionDead;    // every NPC the session has dead (its runner's word), and how
		std::set<Key>                    ourDeaths;       // of those, the ones this game said
		CopyHealth::DeadHere             deadHere;        // the session's dead killed here, since the world loaded

		// Counts for the developer channel.
		struct Counts
		{
			std::uint64_t claims = 0, granted = 0, refused = 0, released = 0, hands = 0, letGo = 0, graphsReset = 0;
			std::uint64_t statesSent = 0, statesIn = 0, actionsSent = 0, actionsIn = 0, gearSent = 0, gearIn = 0, gearMissing = 0, gearSkipped = 0, gearTakenOff = 0;
			std::uint64_t spawnsPlaced = 0, spawnsTaken = 0, unshared = 0, lookDiffers = 0;
			std::uint64_t fallbackSilent = 0, fallbackElsewhere = 0, fallbackFar = 0;
			// A friend's hits on NPCs we run: came, applied; the healths and deaths we said and heard; NPCs killed as
			// they loaded for the session's word; copies set to a share.
			std::uint64_t hitsIn = 0, hitsApplied = 0, healthsSent = 0, healthsIn = 0, deathsSent = 0, deathsIn = 0, killsOnLoad = 0, healthSets = 0;
			// Spells (SpellRules.h): a copy's voice readied as its runner's, or not known here; magicka given back to copies
			// (times); actors a stand-in or another game's NPC's copy made here, ended by their effect's dispel or taken out.
			// madeEnded: found (each once); madeDispelled: its making effect dispelled (each try that found one);
			// madeRemoved: a runtime one handed to the way out (RemotePlayers::TakeOut); madeRaised: a load-order one (a
			// raised corpse of the world, a thrall), never deleted.
			std::uint64_t voicesSet = 0, voicesLacked = 0, magickaTopUps = 0, madeEnded = 0, madeDispelled = 0, madeRemoved = 0, madeRaised = 0;
		};
		Counts counts;
		// A copy's effects refused by kind (SpellRules::CopyMagic; any thread: the effect hook), and whether they are
		// (`npcs spells refuse off`: the rig lets one through to see the scan end what it made).
		std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(SpellRules::CopyMagic::kCount)> copyMagicRefused{};
		std::atomic<bool>                                                                               refuseCopyMagic{ true };
		bool                                                                                            magicHookAsked = false;
		// The copies of NPCs another game runs, by form id, for the effect hook on any thread (no lock: PeaceRules::IdSet,
		// written by TendCopies when the copies change). Far above any scene's; past it, the rest are not judged (said once).
		constexpr std::size_t                                                                           MAX_COPY_IDS = 512;
		PeaceRules::IdSet<MAX_COPY_IDS>                                                                 othersCopyIds;
		std::vector<std::uint32_t>                                                                      othersCopyList;
		bool                                                                                            saidCopyIdsFull = false;
		// Actors a stand-in or a copy made here, on their way out (SpellRules::NextMade): when their effect's dispel was
		// last tried, whether it found one, and whether a runtime one was handed to the way out.
		struct MadeEnding
		{
			std::optional<Clock::time_point> triedAt;
			bool                             dispelled = false;
			bool                             takenOut = false;
		};
		std::unordered_map<RE::FormID, MadeEnding>                                                       madeEnding;
		Clock::time_point                                                                               nextMagicka{};
		// What `npcs spells arm` taught an NPC we run (its form id, the spell's), taken back by `arm ... 0`.
		std::set<std::pair<RE::FormID, RE::FormID>>                                                    armTaught;

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

		// A body a spell or an effect has up again for a while (Raise Zombie, Dead Thrall, Reanimate Corpse, the Vampire
		// Lord's raise: life state kReanimate): its caster's summon, which ends as ash, never the NPC living again. The
		// session's dead are never killed again for it, and its runner never says it lives (CopyHealth::DeadHere).
		bool Reanimated(RE::Actor* a_actor)
		{
			const auto state = a_actor->AsActorState();
			return state && state->GetLifeState() == RE::ACTOR_LIFE_STATE::kReanimate;
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
			// The engine's furniture play (sitting down, the idle, getting up) is not sent: each game plays its own
			// (Furniture.h; it takes no lock). Nor is a paired animation's (a kill move): its beginning is said as a pair, and
			// every game plays the whole pair itself (Pairs.h; its lock is taken after this one, never before).
			const auto actor = static_cast<RE::Actor*>(ref);
			if (runIds.contains(id)) {
				Pairs::Noted(actor, name);
			}
			if (runIds.contains(id) && !Furniture::EngineOwnsEvents(actor) && !Pairs::EventsStay(actor)) {
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
					const auto now = Clock::now();
					std::scoped_lock l{ fightLock };
					fights[other->GetFormID()] = now;
					if (other == target) {
						// Our player's hit: a drop on a copy of it is that hit's (CopyHealth::Copy::Look).
						ourHits[target->GetFormID()] = { now, FlagsOf(*a_event) };
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			// The hit's kind, as Protocol::NpcHitFlags: a spell's (a spell, a scroll, an enchantment, a poison, an
			// ingredient, or a staff's) is said so. It explains one drop as a blow's does; what its lasting effects take
			// after is judged by OurDrainOn, as they tick, never by a window after it.
			static std::uint8_t FlagsOf(const RE::TESHitEvent& a_event)
			{
				using Flag = RE::TESHitEvent::Flag;
				std::uint8_t flags = 0;
				flags |= a_event.flags.all(Flag::kPowerAttack) ? Protocol::kHitPower : 0;
				flags |= a_event.flags.all(Flag::kSneakAttack) ? Protocol::kHitSneak : 0;
				flags |= a_event.flags.all(Flag::kBashAttack) ? Protocol::kHitBash : 0;
				if (const auto source = a_event.source != 0 ? RE::TESForm::LookupByID(a_event.source) : nullptr) {
					using FT = RE::FormType;
					const auto type = source->GetFormType();
					const auto weapon = source->As<RE::TESObjectWEAP>();
					if (type == FT::Spell || type == FT::Scroll || type == FT::Enchantment || type == FT::AlchemyItem || type == FT::Ingredient || (weapon && weapon->IsStaff())) {
						flags |= Protocol::kHitSpell;
					}
				}
				return flags;
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
			std::erase_if(ourHits, [&](const auto& a_entry) { return a_now - a_entry.second.at > CopyHealth::HIT_REACH; });
		}

		std::optional<CopyHealth::OurHit> OurHitOn(RE::FormID a_id)
		{
			std::scoped_lock l{ fightLock };
			const auto       it = ourHits.find(a_id);
			return it == ourHits.end() ? std::nullopt : std::optional{ it->second };
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
			flags |= Water::Under(a_actor, (flags & Protocol::kSwimming) != 0) ? Protocol::kUnderwater : 0;
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
				const auto slots = Gear::SlotsOf(item);
				const auto shared = FormIds::Shareable(item->GetFormID(), Gear::ArmorTemplateOf);
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
			const auto [right, left] = Gear::HeldObjects(a_actor);
			gear.right = held(right, Protocol::kRightUnnamed);
			gear.left = held(left, Protocol::kLeftUnnamed);
			gear.ammo = held(a_actor->GetCurrentAmmo(), Protocol::kAmmoUnnamed);
			gear.voice = held(Gear::VoiceOf(a_actor), Protocol::kVoiceUnnamed);  // the shout or power it has ready
			return gear;
		}

		// ---- moving a copy (the hook) ----
		// Tells a copy's graph its walk's events (a_step's) and its runner's (a_events), and counts what it took and turned
		// away. A walk event turned away is not asked again (a creature's graph may have no sneak or sprint at all), but a
		// swim event is, SWIM_TRIES times for one change (Gait::Settle).
		void TellCopy(RE::Actor* a_actor, const Key& a_key, const Gait::Step& a_step, const std::vector<std::string>& a_events)
		{
			std::uint64_t played = 0;
			std::uint64_t refused = 0;
			std::uint64_t walkRefused = 0;
			std::uint64_t swimTaken = 0;
			std::uint64_t swimRefused = 0;
			std::uint64_t castsPlayed = 0;
			std::uint64_t castsRefused = 0;
			std::string   lastRefused;
			std::string   lastCast;
			std::vector<std::pair<std::string_view, bool>> results;
			results.reserve(a_step.events.size());
			for (const auto event : a_step.events) {
				const bool ok = a_actor->NotifyAnimationGraph(Name(event));
				results.emplace_back(event, ok);
				if (!ok) {
					++walkRefused;
				}
				if (event == Gait::SWIM_START || event == Gait::SWIM_STOP) {
					++(ok ? swimTaken : swimRefused);
				}
			}
			for (const auto& event : a_events) {
				const bool cast = SpellRules::CastEvent(event);
				if (a_actor->NotifyAnimationGraph(Name(event))) {
					++played;
					if (cast) {
						++castsPlayed;
						lastCast = event;
					}
				} else {
					++refused;
					lastRefused = event;
					castsRefused += cast ? 1 : 0;
				}
			}
			if (played == 0 && refused == 0 && results.empty()) {
				return;
			}
			const auto       settled = Gait::Settle(a_step.shown, results, false);
			std::scoped_lock l{ copyLock };
			if (const auto it = copies.find(a_key); it != copies.end()) {
				auto& copy = it->second;
				copy.played += played;
				copy.refused += refused;
				copy.walkRefused += walkRefused;
				copy.swimTaken += swimTaken;
				copy.swimRefused += swimRefused;
				copy.castsPlayed += castsPlayed;
				copy.castsRefused += castsRefused;
				if (!lastCast.empty()) {
					copy.lastCast = lastCast;
				}
				if (settled.swimGivenUp) {
					++copy.swimGivenUp;
				}
				if (copy.actorId == a_actor->GetFormID()) {
					copy.shown = settled;
				}
				if (!lastRefused.empty()) {
					copy.lastRefused = lastRefused;
				}
			}
		}

		// ---- letting go of a copy's actor (main thread, under copyLock): a spawn's copy taken out of the world, an NPC's legs its own ----

		// A load-order NPC's copy let go of: the hook no longer moves it, and its own AI runs it again from where it
		// stands. What the hook left on it is not the AI's: the move flags SetMoveFlags wrote, and a graph in whatever
		// state its runner's last events put it in (a furniture idle with no furniture, a stagger, a kneel), which an AI
		// that did not enter it has no way out of, and which a load-order NPC would keep in the save. So its legs are freed
		// at once, and once copyLock is let go (ResetLetGo) its move flags are cleared and its graph sent back to its
		// default state, unless its own state says it really sits, sleeps, rides, bleeds out or is dead: that graph is
		// the AI's. Its weapon is left as it is: a copy's weapon state follows its graph (as a stand-in's does), so the
		// AI sees it drawn and puts it away itself.
		std::vector<RE::ActorHandle> letGoPending;  // main thread
		// When each actor was last let go of: once a second at most, so a hand-over (its copy let go of as the NPC turns
		// nobody's, and again as it is granted here) resets its graph once, not again over what its own AI just began.
		std::unordered_map<RE::FormID, Clock::time_point> letGoAt;  // main thread
		constexpr auto                                     LET_GO_AGAIN = std::chrono::seconds(1);

		void LetCopyGo(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return;
			}
			RemotePlayers::StopLegs(a_actor);
			const auto now = Clock::now();
			if (const auto last = letGoAt.find(a_actor->GetFormID()); last != letGoAt.end() && now - last->second < LET_GO_AGAIN) {
				return;
			}
			if (letGoAt.size() >= Protocol::MAX_NPCS) {
				std::erase_if(letGoAt, [&](const auto& a_entry) { return now - a_entry.second >= LET_GO_AGAIN; });
			}
			letGoAt[a_actor->GetFormID()] = now;
			++counts.letGo;
			if (letGoPending.size() < Protocol::MAX_NPCS) {
				letGoPending.push_back(a_actor->GetHandle());
			}
		}

		// Main thread, outside copyLock (it calls the game: a graph's notify, which the graph event hook hears).
		void ResetLetGo()
		{
			auto pending = std::move(letGoPending);
			letGoPending.clear();
			for (const auto& handle : pending) {
				const auto actor = handle.get();
				if (!actor || actor->IsDeleted()) {
					continue;
				}
				{
					// mirrored again meanwhile (another game took it on): the hook moves it, and its graph is its runner's
					std::scoped_lock l{ copyLock };
					if (const auto byActor = copyByActor.find(actor.get()); byActor != copyByActor.end()) {
						if (const auto copy = copies.find(byActor->second); copy != copies.end() && copy->second.mirror) {
							continue;
						}
					}
				}
				const auto state = actor->AsActorState();
				if (!state) {
					continue;
				}
				// The flags the hook wrote, cleared whether or not it is loaded (an unloaded NPC keeps them in the save);
				// the graph only when it has one to send back, and its own state does not hold it there.
				auto& flags = state->actorState1;
				flags.sneaking = 0;
				flags.sprinting = 0;
				flags.running = 0;
				flags.walking = 0;
				flags.movingForward = 0;
				if (actor->IsDisabled() || !actor->Is3DLoaded() || actor->IsDead() || actor->IsOnMount() || state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal ||
					state->IsBleedingOut()) {
					continue;
				}
				if (actor->NotifyAnimationGraph(Name("IdleForceDefaultState"))) {
					++counts.graphsReset;
				}
			}
		}

		void TakeAwaySpawn(const Key& a_key, Copy& a_copy, std::string_view a_why)
		{
			const auto actor = a_copy.handle.get();
			if (a_copy.spawned && actor && actor->GetFormID() == a_copy.actorId) {
				RemotePlayers::TakeOut(actor.get(), a_copy.placedAt);
				++counts.spawnsTaken;
				logs::info("NPCs: the copy {:08X} of player {}'s spawn {:08X} taken away ({})", a_copy.actorId, a_key.spawner, a_key.id, a_why);
			} else if (actor) {
				LetCopyGo(actor.get());
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

		// Who rides a mount here (docs/DESIGN.md, "Riding"): our player or an NPC we run (kOurs), a friend's stand-in or a
		// copy of an NPC another game runs (kTheirs), or nobody (an NPC nobody runs is nobody's word).
		enum class RiddenBy
		{
			kNobody,
			kOurs,
			kTheirs,
		};

		RiddenBy RiddenByOf(RE::Actor* a_mount, RE::PlayerCharacter* a_player)
		{
			RE::NiPointer<RE::Actor> rider;
			if (!a_mount->GetMountedBy(rider) || !rider) {
				return RiddenBy::kNobody;
			}
			if (rider.get() == a_player) {
				return RiddenBy::kOurs;
			}
			if (RemotePlayers::IsOurs(rider.get())) {
				return RiddenBy::kTheirs;
			}
			// The NPC the rider shows, as the session keys it: a copy of another game's spawn by that game's key (its id
			// here is ours, its spawner theirs), an NPC of ours by its own.
			const auto key = KeyOf(rider.get());
			const auto owner = key ? owners.find(*key) : owners.end();
			if (owner == owners.end()) {
				return RiddenBy::kNobody;
			}
			return owner->second == ourId ? RiddenBy::kOurs : RiddenBy::kTheirs;
		}

		// A mount of ours to share as our spawn when it is a runtime reference: ridden by ours, or our player's last horse.
		bool OurMount(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (RiddenByOf(a_actor, a_player) == RiddenBy::kOurs) {
				return true;
			}
			const auto last = a_player ? a_player->QLastRiddenMount().get() : RE::NiPointer<RE::Actor>{};
			return last && last.get() == a_actor;
		}

		// ---- spells (SpellRules.h; main thread) ----

		// Who made an actor this game made as it ran, of those the scan does not share: a friend's stand-in, a copy of an
		// NPC another game runs, or anyone else.
		SpellRules::MadeBy MadeByOf(RE::Actor* a_commander)
		{
			if (!a_commander) {
				return SpellRules::MadeBy::kNobody;
			}
			if (RemotePlayers::IsOurs(a_commander)) {
				return SpellRules::MadeBy::kStandIn;
			}
			std::scoped_lock l{ copyLock };
			const auto       byActor = copyByActor.find(a_commander);
			const auto       copy = byActor != copyByActor.end() ? copies.find(byActor->second) : copies.end();
			return copy != copies.end() && copy->second.othersRun && copy->second.actorId == a_commander->GetFormID() ? SpellRules::MadeBy::kOthersCopy :
			                                                                                                            SpellRules::MadeBy::kOther;
		}

		// The effect that made a_made (a summon's, on its caster; a raised corpse's or a command's, on itself) dispelled:
		// the engine's own end of it (the summon's light, the corpse falls). False: none found.
		bool DispelMaker(RE::Actor* a_made, RE::Actor* a_commander)
		{
			for (auto* holder : { a_commander, a_made }) {
				const auto target = holder ? holder->AsMagicTarget() : nullptr;
				const auto list = target ? target->GetActiveEffectList() : nullptr;
				if (!list) {
					continue;
				}
				for (auto* effect : *list) {
					if (!effect) {
						continue;
					}
					RE::ActorHandle commanded;
					if (const auto summon = skyrim_cast<RE::SummonCreatureEffect*>(effect)) {
						commanded = summon->commandedActor;
					} else if (const auto command = skyrim_cast<RE::CommandEffect*>(effect)) {
						commanded = command->commandedActor;
					} else {
						continue;
					}
					if (const auto actor = commanded.get(); actor && actor.get() == a_made) {
						effect->Dispel(true);
						return true;
					}
				}
			}
			return false;
		}

		// Whether an actor stands in the world as one alive (a raised corpse is: its life state is the raise's, not death).
		bool Up(RE::Actor* a_actor)
		{
			const auto state = a_actor->AsActorState();
			return !a_actor->IsDead() || (state && state->IsReanimated());
		}

		// An actor a stand-in or another game's NPC's copy made here, as the scan found it: ended after the scan's loop
		// over the process lists (EndMades), never inside it.
		struct Made
		{
			RE::ActorHandle    actor;
			RE::FormID         id = 0;
			RE::ActorHandle    commander;
			SpellRules::MadeBy by = SpellRules::MadeBy::kNobody;
		};

		// Who made it, as an actor that made one (an actor this game shows for another game) is said.
		std::string_view MadeByName(SpellRules::MadeBy a_by)
		{
			return a_by == SpellRules::MadeBy::kStandIn ? "the stand-in" : "the copy";
		}

		// Each actor a stand-in or another game's NPC's copy made here, a step further out (SpellRules::NextMade): its
		// making effect dispelled; a runtime one with none, or still there after, handed to the stand-ins' way out
		// (RemotePlayers::TakeOut: disabled, deleted only once the game has taken it out); a load-order one never deleted.
		void EndMades(const std::vector<Made>& a_made, Clock::time_point a_now)
		{
			for (const auto& made : a_made) {
				const auto actor = made.actor.get();
				if (!actor || actor->GetFormID() != made.id || actor->IsDeleted() || actor->IsDisabled()) {
					continue;
				}
				const bool loadOrder = !FormIds::IsRuntime(made.id);
				const auto commander = made.commander.get();
				auto [it, fresh] = madeEnding.try_emplace(made.id);
				auto& ending = it->second;
				if (fresh) {
					++counts.madeEnded;
					counts.madeRaised += loadOrder ? 1 : 0;
				}
				const auto facts = [&] {
					return SpellRules::MadeFacts{ .loadOrder = loadOrder,
						.sinceTried = ending.triedAt ? std::optional{ a_now - *ending.triedAt } : std::nullopt,
						.dispelled = ending.dispelled,
						.takenOut = ending.takenOut };
				};
				auto step = SpellRules::NextMade(facts());
				if (step == SpellRules::MadeStep::kDispel) {
					const bool first = !ending.triedAt;
					ending.triedAt = a_now;
					ending.dispelled = DispelMaker(actor.get(), commander.get());
					counts.madeDispelled += ending.dispelled ? 1 : 0;
					if (first || !ending.dispelled) {
						logs::info("NPCs: {:08X} ({}, made here by {} {:08X}) {}: its game makes the real one", made.id, loadOrder ? "of the load order" : "made as the game ran",
							MadeByName(made.by), commander ? commander->GetFormID() : 0,
							ending.dispelled ? "ended by its effect's dispel" : loadOrder ? "has no effect of it found; left (never deleted), tried again" : "has no effect of it found; taken out");
					}
					step = SpellRules::NextMade(facts());
				}
				if (step == SpellRules::MadeStep::kTakeOut) {
					ending.takenOut = true;
					++counts.madeRemoved;
					logs::info("NPCs: {:08X} (made here by {} {:08X}) taken out of the world{}", made.id, MadeByName(made.by), commander ? commander->GetFormID() : 0,
						ending.dispelled ? ": still there after its effect's dispel" : "");
					RemotePlayers::TakeOut(actor.get(), {});
				}
			}
		}

		// A copy's voice readied as its runner's NPC's (a shout or a power): one it does not know is not taught to a copy
		// of a load-order NPC (it stays in its save), only to a spawn's copy (a_mayAdd); an unnamed one leaves its own.
		void ReadyVoice(const Protocol::NpcGear& a_gear, bool a_mayAdd, RE::Actor* a_actor)
		{
			if ((a_gear.unnamed & Protocol::kVoiceUnnamed) != 0) {
				return;
			}
			const auto held = Gear::VoiceOf(a_actor);
			if ((held ? held->GetFormID() : 0u) == a_gear.voice) {
				return;
			}
			if (a_gear.voice != 0 && !a_mayAdd && !Gear::Knows(a_actor, a_gear.voice)) {
				++counts.voicesLacked;
				return;
			}
			if (Gear::SetVoice(a_actor, a_gear.voice, a_mayAdd)) {
				counts.voicesSet += a_gear.voice != 0 ? 1 : 0;
			} else {
				++counts.voicesLacked;
			}
		}

		// The magicka a mirrored copy spent here given back (SpellRules::MagickaBack): its runner's NPC is the truth of it.
		void GiveMagickaBack(const Key& a_key)
		{
			RE::NiPointer<RE::Actor> actor;
			bool                     mirror = false;
			bool                     magic = false;
			{
				std::scoped_lock l{ copyLock };
				const auto       it = copies.find(a_key);
				if (it == copies.end() || it->second.actorId == 0) {
					return;
				}
				actor = it->second.handle.get();
				mirror = it->second.mirror;
				magic = it->second.holdsMagic;
				if (!actor || actor->GetFormID() != it->second.actorId) {
					return;
				}
			}
			const auto values = actor->AsActorValueOwner();
			if (!values || actor->IsDead()) {
				return;
			}
			const float back = SpellRules::MagickaBack(mirror, magic, -actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka));
			if (back <= 0.0f) {
				return;
			}
			values->RestoreActorValue(RE::ActorValue::kMagicka, back);
			++counts.magickaTopUps;
			std::scoped_lock l{ copyLock };
			if (const auto it = copies.find(a_key); it != copies.end()) {
				it->second.magickaBack += back;
			}
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
			std::set<Key>     found;
			std::vector<Made> made;  // ended after the loop (EndMades): nothing in it changes the lists it walks
			// One a stand-in or another game's NPC's copy commands here (its own AI's conjuring, reanimating or commanding, a
			// cast the effect hook did not see) is ended: that game makes the real one (SpellRules::EndsMade).
			const auto madeHere = [&](RE::Actor* a_actor) {
				if (!Up(a_actor)) {
					return false;
				}
				const auto by = a_actor->GetCommandingActor();
				if (!by || by.get() == player) {
					return false;
				}
				const auto kind = MadeByOf(by.get());
				if (!SpellRules::EndsMade(kind)) {
					return false;
				}
				if (made.size() < Protocol::MAX_NPCS) {
					made.push_back({ a_actor->GetHandle(), a_actor->GetFormID(), by->GetHandle(), kind });
				}
				return true;
			};
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
						// which every game makes its own of) is not shared yet. A horse of ours is (NpcRules::SharedRuntime).
						const auto commander = CommanderOf(actor.get(), player);
						if (!loaded.contains(Key{ id, ourId }) && !NpcRules::SharedRuntime(commander, OurMount(actor.get(), player))) {
							// One a stand-in or another game's NPC's copy made here: ended after the loop, never shared.
							if (commander != NpcRules::Commander::kOther || !madeHere(actor.get())) {
								++counts.unshared;
							}
							continue;
						}
						key.spawner = ourId;
					} else {
						// An NPC of the load order (a corpse of the world raised, a thrall, a commanded creature) that a
						// stand-in or another game's NPC's copy commands here: its raise or command ended after the loop,
						// never the NPC (it stays this world's, found and shared as any other).
						static_cast<void>(madeHere(actor.get()));
					}
					found.insert(key);
					auto& npc = loaded[key];
					if (npc.id != id) {
						npc = Loaded{ .handle = actor->GetHandle(), .id = id };
					}
					npc.kind = KindOf(actor.get());
					npc.seen = a_now;
					if (!actor->IsDead()) {
						const auto owner = owners.find(key);
						npc.aliveUnrun = owner == owners.end() || owner->second == ourId;
					}
				}
			}
			std::erase_if(madeEnding, [&](const auto& a_entry) { return std::ranges::none_of(made, [&](const Made& a_m) { return a_m.id == a_entry.first; }); });
			EndMades(made, a_now);
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
				const bool ours = owner != owners.end() && owner->second == ourId;
				// The session has it dead (a death said while it was not loaded here, or before we joined) and it lives
				// here: killed, not asked for (CopyHealth::KillsOnLoad: the host's game keeps its own save's word). A kill
				// that does not take is tried again, the engine's kill then the one that ignores the essential flag; one seen
				// dead here that lives again by no word of the session's (this game's respawn of its cell, a script's or the
				// console's raise in a game that does not run it) is killed again (CopyHealth::DeadHere). Never killed again:
				// - a raise in the game that runs it, of one it saw die while it ran it (a quest's Resurrect, the console):
				//   the session's raise, which SendFates says next (its health), and the session has it alive again;
				// - a body a spell or an effect has up for a while (Reanimated: a thrall, a zombie): the caster's summon,
				//   looked at as the body it still is, and never run.
				// (CopyHealth::LookOfDead)
				const bool reanimated = Reanimated(actor.get());
				const auto looks = CopyHealth::LookOfDead(actor->IsDead(), reanimated, ours, npc.deathSent);
				if (sessionDead.contains(key) && looks != CopyHealth::DeadLooks::kSessionsRaise && CopyHealth::KillsOnLoad(ourId == hostId, ourDeaths.contains(key))) {
					const bool alive = looks == CopyHealth::DeadLooks::kRisen;
					// (one in a kill move or another pair is left to it, and killed after it if it lives)
					if (!alive || (!actor->IsInKillMove() && !Pairs::Held(actor.get()))) {
						const auto risenBefore = deadHere.risen;
						switch (deadHere.Look(npc.id, alive, a_now)) {
						case CopyHealth::DeadDo::kKill:
							{
								++counts.killsOnLoad;
								const auto& death = sessionDead[key];
								const auto  by = RemotePlayers::StandInOf(death.killer);
								const auto  tries = deadHere.Tries(npc.id);
								if (tries == 1) {
									logs::info("NPCs: {:08X} is dead in the session (player {}'s kill) and lives here{}: killed", key.id, death.killer,
										deadHere.risen != risenBefore ? " again (it rose here after it died)" : " as it loaded");
								} else {
									logs::warn("NPCs: {:08X} is dead in the session and still lives here: killed again (try {} of {})", key.id, tries, CopyHealth::KILL_TRIES);
								}
								if (tries % 2 == 1) {
									actor->KillImpl(by.get(), 0.0f, true, false);
								} else {
									actor->KillImmediate();
								}
								break;
							}
						case CopyHealth::DeadDo::kGiveUp:
							logs::warn("NPCs: {:08X} is dead in the session but this game keeps it alive after {} kills: left alive here, never run (is it essential here only?)", key.id,
								CopyHealth::KILL_TRIES);
							break;
						case CopyHealth::DeadDo::kNothing:
							break;
						}
					}
					if (alive || reanimated) {
						continue;  // (a reanimated body is its caster's: never asked for, never run)
					}
				} else {
					deadHere.Forget(npc.id);
				}
				// A body is nobody's to run, unless we ran it as it died (its death is ours to say), or it died here while
				// nobody ran it, after this game saw it alive this session (whoever's blow it was: our player's, an NPC's we
				// run, a trap's): then its death is this game's to say, so it is asked for as a fight and said once granted
				// (SendFates). Refused, another game runs it: a copy here, whose killing blow goes there if our player
				// killed it (CopyHealth::Seen::killedByUs), and which is raised if not. One that died here as a copy is
				// its runner's to say, never ours.
				bool sayDeath = false;
				if (actor->IsDead() && !ours) {
					if (owner != owners.end() || !npc.aliveUnrun || sessionDead.contains(key)) {
						continue;
					}
					sayDeath = true;
				}
				NpcRules::Facts facts;
				facts.owner = owner == owners.end() ? NpcRules::Owner::kNobody : owner->second == ourId ? NpcRules::Owner::kUs : NpcRules::Owner::kOther;
				if (facts.owner == NpcRules::Owner::kNobody) {
					npc.unownedSince = npc.unownedSince.value_or(a_now);
				} else {
					npc.unownedSince.reset();
				}
				facts.unownedSince = npc.unownedSince;
				facts.lastClaim = npc.lastClaim;
				facts.saidFollows = npc.saidFollows;
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
				facts.fighting = sayDeath || FoughtLately(npc.id, a_now);
				const auto ridden = RiddenByOf(actor.get(), player);
				facts.ridden = ridden == RiddenBy::kOurs;
				facts.riddenThere = ridden == RiddenBy::kTheirs;
				facts.followsUs = facts.ridden || (player && FollowsUs(actor.get(), player));
				facts.spawn = key.IsSpawn();
				const auto ask = NpcRules::Decide(facts, a_now);
				if (ask == NpcRules::Ask::kNone) {
					continue;
				}
				npc.lastClaim = a_now;
				if (ask == NpcRules::Ask::kFollower) {
					npc.saidFollows = a_now;
				} else if (ask == NpcRules::Ask::kHave || ask == NpcRules::Ask::kRelease) {
					npc.saidFollows.reset();  // the book holds it as no follower of ours after this
				}
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
				state->sequence = sequences.Next(key);
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
				if (SpellRules::CastEvent(event.event)) {
					++loaded[key->second].castsSent;
				}
				pairActionsSent += PairRules::PairEvent(event.event) ? 1 : 0;  // (Pairs::EventsStay keeps them: the rig reads 0)
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
				bool                  check = false;  // read its gear back (else: dress it)
				bool                  magicka = false;  // give back the magicka it spent here (else: its gear)
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
					copy.othersRun = othersRun;
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
								LetCopyGo(was.get());
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
					// A body is never moved: it lies where its death left it (ragdolled by its own game).
					const bool mirror = actor && mode == NpcRules::Mode::kMirror && !actor->IsDead();
					if (copy.mirror && !mirror) {
						copy.actions.Clear();
						if (actor) {
							LetCopyGo(actor);
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
						} else if (copy.gear && a_now >= copy.nextWearCheck) {
							copy.nextWearCheck = a_now + WEAR_CHECK_EVERY;
							work.push_back({ .key = key, .check = true });
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
				std::vector<std::uint32_t> others;
				for (const auto& [key, copy] : copies) {
					if (copy.othersRun && copy.actorId != 0) {
						others.push_back(copy.actorId);
					}
				}
				std::ranges::sort(others);
				if (others != othersCopyList) {
					if (!othersCopyIds.Assign(others) && !saidCopyIdsFull) {
						saidCopyIdsFull = true;
						logs::warn("NPCs: more than {} copies: the effects of the rest are not judged (what they make here is still ended by the scan)", MAX_COPY_IDS);
					}
					othersCopyList = std::move(others);
				}
				if (a_now >= nextMagicka) {
					nextMagicka = a_now + MAGICKA_EVERY;
					for (const auto& [key, copy] : copies) {
						if (copy.mirror && copy.holdsMagic && copy.actorId != 0) {
							work.push_back({ .key = key, .magicka = true });
						}
					}
				}
			}
			ResetLetGo();
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
				if (job.magicka) {
					GiveMagickaBack(job.key);
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
				if (!magicHookAsked) {
					// The effect hook that keeps a copy from making anything here goes in with the first copy dressed, if
					// no stand-in put it in yet.
					magicHookAsked = true;
					RemotePlayers::HookMagic(actor.get());
				}
				if (job.check) {
					// Read back what it wears and holds now, a while after it was dressed.
					const auto shown = Gear::Check(gear, actor.get());
					const bool magic = Gear::HoldsMagic(actor.get());
					std::scoped_lock l{ copyLock };
					if (const auto it = copies.find(job.key); it != copies.end() && it->second.actorId == actor->GetFormID()) {
						it->second.holdsMagic = magic;
					}
					if (const auto it = copies.find(job.key); it != copies.end() && it->second.actorId == actor->GetFormID() && it->second.gear == gear && it->second.geared) {
						if (!shown.Complete(it->second.gearLacks) && (!it->second.wears || it->second.wears->Complete(it->second.gearLacks))) {
							logs::info("NPCs: copy {:08X} of {:08X} does not show its runner's gear: wears {}/{} of it, {} left on, hands {}/{}, ammo {} ({} it lacks)", actor->GetFormID(),
								job.key.id, shown.matched, shown.named, shown.extra, shown.right ? "ok" : "differ", shown.left ? "ok" : "differ", shown.ammo ? "ok" : "differs",
								it->second.gearLacks);
						}
						it->second.wears = shown;
					}
					continue;
				}
				const auto dressed = Gear::Dress(gear, mayAdd, actor.get());
				ReadyVoice(gear, mayAdd, actor.get());
				const bool       magic = Gear::HoldsMagic(actor.get());
				std::scoped_lock l{ copyLock };
				if (const auto it = copies.find(job.key); it != copies.end() && it->second.actorId == actor->GetFormID() && it->second.gear == gear) {
					it->second.holdsMagic = magic;
					it->second.geared = true;
					it->second.gearLacks = dressed.missing;
					it->second.wears.reset();
					it->second.nextWearCheck = a_now + WEAR_CHECK_AFTER;
				}
				counts.gearMissing += dressed.missing;
				counts.gearSkipped += dressed.skipped;
				counts.gearTakenOff += dressed.unequipped;
				logs::debug("NPCs: copy {:08X} of {:08X} took off {} and put on {} of its runner's items ({} it lacks, {} left to its runner)", actor->GetFormID(), job.key.id, dressed.unequipped, dressed.equipped,
					dressed.missing, dressed.skipped);
			}
		}

		// ---- health and deaths (main thread) ----
		float HealthOf(RE::Actor* a_actor)
		{
			const auto values = a_actor->AsActorValueOwner();
			return values ? values->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
		}

		// Its health set to a_share of its most (a damage or a restore: no hit, no event).
		void SetShare(RE::Actor* a_actor, float a_share, float a_max)
		{
			const auto values = a_actor->AsActorValueOwner();
			if (!values) {
				return;
			}
			const auto change = CopyHealth::ChangeTo(a_share, a_max, values->GetActorValue(RE::ActorValue::kHealth));
			if (change > 0.0f) {
				values->RestoreActorValue(RE::ActorValue::kHealth, change);
			} else if (change < 0.0f) {
				values->DamageActorValue(RE::ActorValue::kHealth, -change);
			}
		}

		// The actor that shows a player here: our player, or a friend's stand-in (none: not placed, or no player).
		RE::Actor* ActorOfPlayer(std::uint32_t a_playerId, RE::NiPointer<RE::Actor>& a_hold)
		{
			if (a_playerId == 0) {
				return nullptr;
			}
			if (a_playerId == ourId) {
				return RE::PlayerCharacter::GetSingleton();
			}
			a_hold = RemotePlayers::StandInOf(a_playerId);
			return a_hold.get();
		}

		// What our player's lasting effects on a_actor take from its health a second, as a share of a_most: each active
		// harmful effect our player cast on its health that lasts (a burn, a poison, an enchantment's, a concentration
		// spell's), at its magnitude (health a second, its resistances counted). An instant one comes with its hit event.
		// Another's effects (a follower's, a summon's, a trap's) are not ours: what they take is the runner's game's to deal.
		float OurDrainOn(RE::Actor* a_actor, float a_most, RE::PlayerCharacter* a_player)
		{
			const auto target = a_actor ? a_actor->AsMagicTarget() : nullptr;
			if (!target || !a_player || !std::isfinite(a_most) || a_most <= 0.0f) {
				return 0.0f;
			}
			const auto ours = a_player->GetHandle();
			float      perSecond = 0.0f;
			target->VisitActiveEffects([&](RE::ActiveEffect* a_effect) {
				using Flag = RE::ActiveEffect::Flag;
				using Archetype = RE::EffectSetting::Archetype;
				if (!a_effect || a_effect->flags.any(Flag::kInactive) || a_effect->flags.any(Flag::kDispelled) || a_effect->caster != ours) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto base = a_effect->GetBaseObject();
				if (!base || !base->IsDetrimental()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto archetype = base->GetArchetype();
				const bool drains = archetype == Archetype::kValueModifier || archetype == Archetype::kDualValueModifier || archetype == Archetype::kAbsorb;
				const bool onHealth = base->data.primaryAV == RE::ActorValue::kHealth || (archetype == Archetype::kDualValueModifier && base->data.secondaryAV == RE::ActorValue::kHealth);
				const bool lasts = a_effect->duration > 0.0f || (a_effect->spell && a_effect->spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration);
				if (drains && onHealth && lasts && std::isfinite(a_effect->magnitude)) {
					perSecond += std::fabs(a_effect->magnitude);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return perSecond / a_most;
		}

		// Our player and a_actor in one kill move together: each in a kill move, one the other's partner (its process's
		// target). A follower's kill move on a_actor, or our player's on another NPC, is not this.
		bool InKillMoveWith(RE::PlayerCharacter* a_player, RE::Actor* a_actor)
		{
			if (!a_player || !a_actor || !a_player->IsInKillMove() || !a_actor->IsInKillMove()) {
				return false;
			}
			const auto targetOf = [](RE::Actor* a_of) -> RE::RefHandle {
				const auto process = a_of->GetActorRuntimeData().currentProcess;
				return process ? process->target : 0;
			};
			const auto playerTarget = targetOf(a_player);
			const auto actorTarget = targetOf(a_actor);
			return (playerTarget != 0 && playerTarget == a_actor->GetHandle().native_handle()) || (actorTarget != 0 && actorTarget == a_player->GetHandle().native_handle());
		}

		// Who killed an NPC we ran: the player its killer shows (our player, or a friend's stand-in), else the friend
		// whose hit we applied to it last, a moment ago; 0: no player.
		std::uint32_t KillerOf(RE::Actor* a_actor, const Loaded& a_npc, Clock::time_point a_now)
		{
			if (const auto killer = a_actor->GetKiller()) {
				if (killer->IsPlayerRef()) {
					return ourId;
				}
				if (const auto player = RemotePlayers::PlayerOf(killer)) {
					return player;
				}
			}
			return a_npc.lastHitter != 0 && a_now - a_npc.lastHitAt <= KILLER_HOLD ? a_npc.lastHitter : 0;
		}

		// The answers to friends' hits, then the health and deaths of the NPCs we run.
		void SendFates(const SendFn& a_send, Clock::time_point a_now)
		{
			for (std::size_t i = 0; i < answersOwed.answers.size(); i += Protocol::MAX_NPC_HITS) {
				Protocol::NpcHitAnswers chunk;
				const auto              begin = answersOwed.answers.begin() + static_cast<std::ptrdiff_t>(i);
				chunk.answers.assign(begin, begin + static_cast<std::ptrdiff_t>((std::min)(Protocol::MAX_NPC_HITS, answersOwed.answers.size() - i)));
				a_send(Protocol::Encode(chunk), true);
			}
			answersOwed.answers.clear();

			Protocol::NpcHealths healths;
			Protocol::NpcDeaths  deaths;
			const auto           flush = [&] {
				if (!healths.npcs.empty()) {
					counts.healthsSent += healths.npcs.size();
					a_send(Protocol::Encode(healths), true);
					healths.npcs.clear();
				}
				if (!deaths.deaths.empty()) {
					counts.deathsSent += deaths.deaths.size();
					a_send(Protocol::Encode(deaths), true);
					deaths.deaths.clear();
				}
			};
			for (auto& [key, npc] : loaded) {
				if (!Runs(key)) {
					continue;
				}
				const auto actor = npc.handle.get();
				if (!actor || actor->GetFormID() != npc.id) {
					continue;
				}
				if (actor->IsInKillMove()) {
					npc.killMoveAt = a_now;
				}
				if (actor->IsDead()) {
					if (!npc.deathSent) {
						npc.deathSent = true;
						const bool killMove = npc.killMoveAt && a_now - *npc.killMoveAt <= 3s;
						const Protocol::NpcDeath death{ .key = key, .killer = KillerOf(actor.get(), npc, a_now), .flags = static_cast<std::uint8_t>(killMove ? Protocol::kDeathKillMove : 0) };
						deaths.deaths.push_back(death);
						sessionDead[key] = death;
						ourDeaths.insert(key);
						logs::info("NPCs: {:08X}{} died here (killer: player {}{})", key.id, key.IsSpawn() ? std::format(" (player {}'s spawn)", key.spawner) : "", death.killer,
							killMove ? ", a kill move" : "");
					}
					continue;
				}
				if (npc.deathSent && Reanimated(actor.get())) {
					continue;  // up as a thrall or a zombie (Reanimated): still dead to the session, never said alive
				}
				if (npc.deathSent) {
					// Raised (a quest's Resurrect, a script, the console): its health says so, and the session has it alive
					// again.
					npc.deathSent = false;
					npc.reporter.Reset();
					sessionDead.erase(key);
					ourDeaths.erase(key);
				}
				const auto max = actor->GetActorValueMax(RE::ActorValue::kHealth);
				// A living NPC's share is never said as 0 (that is a death): one down at none (an essential one) is at least this.
				const auto share = (std::max)(CopyHealth::ShareOf(HealthOf(actor.get()), max), 0.001f);
				if (npc.reporter.Due(share, hitApplied.contains(key), a_now)) {
					npc.reporter.Sent(share, a_now);
					healths.npcs.push_back({ key, share });
				}
				if (healths.npcs.size() >= Protocol::MAX_NPC_HEALTHS || deaths.deaths.size() >= Protocol::MAX_NPC_HEALTHS) {
					flush();
				}
			}
			hitApplied.clear();
			flush();
		}

		// Each copy's health and death (CopyHealth::Copy): our player's hits on it sent to its runner, what nothing of
		// ours explains set back, killed for its runner's death, raised for its runner's NPC living.
		void TendCopyHealth(const SendFn& a_send, Clock::time_point a_now)
		{
			struct Shown
			{
				Key             key;
				RE::ActorHandle handle;
				RE::FormID      id = 0;
				std::uint32_t   runner = 0;
			};
			std::vector<Shown> shown;
			{
				std::scoped_lock l{ copyLock };
				for (const auto& [key, copy] : copies) {
					const auto owner = owners.find(key);
					if (copy.actorId != 0 && owner != owners.end() && owner->second != ourId) {
						shown.push_back({ key, copy.handle, copy.actorId, owner->second });
					}
				}
			}
			const auto      player = RE::PlayerCharacter::GetSingleton();
			Protocol::NpcHits hits;
			const auto      flush = [&] {
				if (!hits.hits.empty()) {
					a_send(Protocol::Encode(hits), true);
					hits.hits.clear();
				}
			};
			const auto nextSeq = [] {
				if (++hitSeq == 0) {
					++hitSeq;  // 0 is no hit
				}
				return hitSeq;
			};
			for (const auto& one : shown) {
				const auto actor = one.handle.get();
				if (!actor || actor->GetFormID() != one.id || !actor->Is3DLoaded()) {
					continue;
				}
				const auto max = actor->GetActorValueMax(RE::ActorValue::kHealth);
				if (!std::isfinite(max) || max <= 0.0f) {
					continue;
				}
				if (health.size() >= Protocol::MAX_NPCS && !health.contains(one.key)) {
					continue;
				}
				auto& copy = health[one.key];
				// A copy whose runner has it dead and that a spell here has up as a thrall or a zombie (Reanimated) is still
				// the body: never killed again for it, the caster's summon until it ends.
				const bool dead = actor->IsDead() || (copy.RunnerDead() && Reanimated(actor.get()));
				const auto killer = dead ? actor->GetKiller() : nullptr;
				// What our player's lasting effects on it take, and whether our player killed it: only those of ours count.
				const CopyHealth::Seen seen{ .share = CopyHealth::ShareOf(HealthOf(actor.get()), max),
					.dead = dead,
					.inKillMove = actor->IsInKillMove(),
					.floor = 1.0f / max,
					.ourDrain = dead ? 0.0f : OurDrainOn(actor.get(), max, player),
					.killedByUs = killer && player && killer == player };
				auto ourHit = OurHitOn(one.id);
				if (InKillMoveWith(player, actor.get())) {
					// Our player's kill move on this copy (the paired move has no hit event); a follower's kill move on it,
					// or ours on another NPC, is not ours.
					ourHit = CopyHealth::OurHit{ a_now, static_cast<std::uint8_t>((ourHit ? ourHit->flags : 0) | Protocol::kHitKillMove) };
				}
				// Raised when its runner's NPC lives: in every game but the host's, whose save is the truth of who is dead
				// (a body there that was never alive this session is killed in its runner's game instead).
				const bool hostsSave = ourId == hostId;
				const auto step = copy.Look(one.key, seen, ourHit, nextSeq, copy.SeenAlive() || !hostsSave, hostsSave, a_now, healthCounts);
				switch (step.what) {
				case CopyHealth::Do::kSend:
					hits.hits.push_back(step.hit);
					if (hits.hits.size() >= Protocol::MAX_NPC_HITS) {
						flush();
					}
					break;
				case CopyHealth::Do::kSet:
					SetShare(actor.get(), step.share, max);
					++counts.healthSets;
					break;
				case CopyHealth::Do::kKill:
					{
						const auto               death = sessionDead.find(one.key);
						RE::NiPointer<RE::Actor> hold;
						const auto               by = death != sessionDead.end() ? ActorOfPlayer(death->second.killer, hold) : nullptr;
						logs::info("NPCs: copy {:08X} of {:08X} killed: its runner (player {}) said it died", one.id, one.key.id, one.runner);
						actor->KillImpl(by, 0.0f, true, false);
					}
					break;
				case CopyHealth::Do::kRevive:
					logs::info("NPCs: copy {:08X} of {:08X} raised: it lives in its runner's game (player {}) at {:.0f}%", one.id, one.key.id, one.runner, step.share * 100.0f);
					actor->Resurrect(false, true);
					SetShare(actor.get(), step.share, max);
					break;
				case CopyHealth::Do::kNothing:
					break;
				}
			}
			flush();
		}

		Copy& CopyOf(const Key& a_key, std::uint32_t a_runner)  // under copyLock
		{
			auto& copy = copies[a_key];
			if (copy.feed.From(a_runner)) {
				// A new runner, or its runner again after it changed hands: its states count and its clock start over.
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

	void Started(std::uint32_t a_ourId, std::uint32_t a_hostId)
	{
		Clear();
		ourId = a_ourId;
		hostId = a_hostId;
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
		// The answers to friends' hits and the deaths of the NPCs we run go first, before this frame's releases and claims:
		// a death the server reads after we let go of its NPC is taken only for a moment (NpcFates::DEATH_GRACE).
		SendFates(a_send, now);
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
		TendCopyHealth(a_send, now);
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
			othersCopyIds.Assign({});
			othersCopyList.clear();
		}
		madeEnding.clear();
		ResetLetGo();
		{
			std::scoped_lock l{ eventLock };
			runIds.clear();
			heard.clear();
			recording = 0;
		}
		{
			std::scoped_lock l{ fightLock };
			fights.clear();
			ourHits.clear();
		}
		loaded.clear();
		sequences.Clear();
		owners.clear();
		active = false;
		ourId = 0;
		hostId = 0;
		health.clear();
		healthCounts = {};
		hitSeq = 0;
		answersOwed.answers.clear();
		hitApplied.clear();
		sessionDead.clear();
		ourDeaths.clear();
		deadHere.Clear();
	}

	void Owners(const Protocol::NpcOwners& a_msg)
	{
		if (!active) {
			return;
		}
		std::map<Key, std::uint32_t> before;  // a full list's: who ran each before it
		if (a_msg.full) {
			before.swap(owners);
		}
		for (const auto& [key, owner] : a_msg.owners) {
			const auto it = owners.find(key);
			const auto was = it == owners.end() ? 0u : it->second;
			{
				// Changed hands (through nobody too): its copy's states start over with the next runner's next state,
				// whatever that runner numbers it (NpcRules::Feed).
				const auto old = before.find(key);
				const auto previous = a_msg.full ? (old == before.end() ? 0u : old->second) : was;
				if (previous != owner) {
					std::scoped_lock l{ copyLock };
					if (const auto copy = copies.find(key); copy != copies.end()) {
						copy->second.feed.HandsChanged();
					}
				}
			}
			if (owner == 0) {
				owners.erase(key);
				// Its next runner says its health (a dead one's copy stays dead).
				if (const auto h = health.find(key); h != health.end() && !h->second.RunnerDead()) {
					health.erase(h);
				}
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
					npc->second.reporter.Reset();  // and its health (CopyHealth::Reporter)
				}
				logs::info("NPCs: {:08X}{} is run {} now{}", key.id, key.IsSpawn() ? std::format(" (player {}'s spawn)", key.spawner) : "",
					owner == ourId ? "here" : std::format("by player {}", owner), was != 0 ? std::format(" (was player {})", was) : "");
			}
			if (owner == ourId) {
				// Ours now: its copy, if this game showed one, is let go of; its AI runs here from where it stands, at the
				// health it shows (our hits on it still in flight stand: CopyHealth::Copy::Answer's kYours).
				health.erase(key);
				std::scoped_lock l{ copyLock };
				if (const auto copy = copies.find(key); copy != copies.end()) {
					if (const auto actor = copy->second.handle.get()) {
						copyByActor.erase(actor.get());
						if (copy->second.spawned) {
							RemotePlayers::StopLegs(actor.get());
						} else {
							LetCopyGo(actor.get());
						}
					}
					copies.erase(copy);
					RemotePlayers::SetCopies(copyByActor.size());
				}
			}
		}
		ResetLetGo();
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
			if (it == copies.end() || it->second.feed.runner != a_msg.runner || !NpcRules::Replayed(action.event)) {
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
		if (Runs(a_msg.key) || (copies.size() >= Protocol::MAX_NPCS && !copies.contains(a_msg.key))) {
			return;  // ours now, or too many (as States: a join's replay of the session's gear makes no more copies than states would)
		}
		auto& copy = CopyOf(a_msg.key, a_msg.runner);
		copy.gear = a_msg;
		copy.geared = false;
		++counts.gearIn;
	}

	void Hits(const Protocol::NpcHits& a_msg)
	{
		if (!active || a_msg.hitter == 0 || a_msg.hitter == ourId) {
			return;
		}
		const auto now = Clock::now();
		for (const auto& hit : a_msg.hits) {
			++counts.hitsIn;
			Protocol::NpcHitAnswer   answer{ .key = hit.key, .hitter = a_msg.hitter, .seq = hit.seq, .verdict = Protocol::HitVerdict::kNotHere };
			const auto               npc = Runs(hit.key) ? loaded.find(hit.key) : loaded.end();
			RE::NiPointer<RE::Actor> actor;
			if (npc != loaded.end()) {
				actor = npc->second.handle.get();
			}
			if (!actor || actor->GetFormID() != npc->second.id) {
				answersOwed.answers.push_back(answer);  // not loaded here, or not ours (an old book's): the hitter gives it back
				continue;
			}
			if (actor->IsDead()) {
				answer.verdict = Protocol::HitVerdict::kDead;
				answersOwed.answers.push_back(answer);
				continue;
			}
			// The same share of its most as it took off the friend's copy. The friend's stand-in is its attacker (its
			// killer when it dies of it: a killing blow kills it whatever it has left, the friend's copy being dead).
			auto&                    run = npc->second;
			RE::NiPointer<RE::Actor> hold;
			const auto               by = ActorOfPlayer(a_msg.hitter, hold);
			const auto               max = actor->GetActorValueMax(RE::ActorValue::kHealth);
			const auto               damage = CopyHealth::DamageOf(hit.share, max);
			const bool               killing = (hit.flags & Protocol::kHitKilling) != 0 || HealthOf(actor.get()) - damage <= 0.0f;
			run.lastHitter = a_msg.hitter;
			run.lastHitAt = now;
			if ((hit.flags & Protocol::kHitKillMove) != 0) {
				run.killMoveAt = now;
			}
			if (killing && !actor->IsEssential()) {
				actor->KillImpl(by, damage, true, false);  // killed first, so the friend is its killer
			}
			if (const auto values = actor->AsActorValueOwner(); values && damage > 0.0f) {
				values->DamageActorValue(RE::ActorValue::kHealth, damage);  // an essential one goes down
			}
			answer.verdict = Protocol::HitVerdict::kApplied;
			answer.share = actor->IsDead() ? 0.0f : CopyHealth::ShareOf(HealthOf(actor.get()), max);
			answersOwed.answers.push_back(answer);
			hitApplied.insert(hit.key);
			++counts.hitsApplied;
		}
	}

	void HitAnswers(const Protocol::NpcHitAnswers& a_msg)
	{
		if (!active) {
			return;
		}
		for (const auto& answer : a_msg.answers) {
			const auto it = answer.hitter == ourId ? health.find(answer.key) : health.end();
			if (it == health.end()) {
				continue;  // ours now (what the copy took stands), or not our hit
			}
			const auto refusedBefore = healthCounts.refused;
			it->second.Answer(answer.seq, answer.verdict, answer.share, healthCounts);
			// The first refusals, then one in a hundred: a refused hit is given back, which a player sees.
			if (healthCounts.refused != refusedBefore && (healthCounts.refused <= 20 || healthCounts.refused % 100 == 0)) {
				logs::info("NPCs: our hit {} on {:08X} was refused ({}): given back ({} refused in all)", answer.seq, answer.key.id, static_cast<int>(answer.verdict), healthCounts.refused);
			}
		}
	}

	void Healths(const Protocol::NpcHealths& a_msg)
	{
		if (!active || (a_msg.runner != 0 && a_msg.runner == ourId)) {
			return;
		}
		for (const auto& npc : a_msg.npcs) {
			++counts.healthsIn;
			if (Runs(npc.key) || (health.size() >= Protocol::MAX_NPCS && !health.contains(npc.key))) {
				continue;  // ours now (an old runner's last word), or too many
			}
			health[npc.key].Word(npc.share);
			// It lives: the session has it dead no more (its runner raised it).
			sessionDead.erase(npc.key);
			ourDeaths.erase(npc.key);
		}
	}

	void Deaths(const Protocol::NpcDeaths& a_msg)
	{
		if (!active || (a_msg.runner != 0 && a_msg.runner == ourId)) {
			return;
		}
		for (const auto& death : a_msg.deaths) {
			++counts.deathsIn;
			if (sessionDead.size() < Protocol::MAX_NPCS || sessionDead.contains(death.key)) {
				sessionDead[death.key] = death;
			}
			// A copy here dies with it (CopyHealth::Copy::Look); one we run and have alive is killed as SendClaims finds it
			// (our game ran it after its death was said: a hand-over on its way).
			if (!Runs(death.key) && (health.size() < Protocol::MAX_NPCS || health.contains(death.key))) {
				health[death.key].Died();
			}
		}
	}

	bool UpdateCopy(RE::Actor* a_actor, float a_delta)
	{
		const auto                          now = Clock::now();
		Key                                 key;
		std::optional<StateBuffer::Sampled> sample;
		Gait::Step                          step;
		std::vector<std::string>            events;
		// A copy our player sits on is ridden here: the engine moves it until its runner hands it over (a mount goes
		// with its rider, NpcRules::Facts::ridden). A copy seated on its mount (Riding.h) is carried by the engine: not
		// placed, and its walk not told.
		if (const auto player = RE::PlayerCharacter::GetSingleton(); player && player->IsOnMount()) {
			if (RE::NiPointer<RE::Actor> rider; a_actor->GetMountedBy(rider) && rider.get() == player) {
				return false;
			}
		}
		// Getting in or out of furniture (Furniture.h), the engine's own update runs for it (its sit procedure is the AI's),
		// for a few seconds at most. In it, or waiting by furniture another actor is in, it is not placed, its walk not told.
		// The same in a paired animation played here (Pairs.h: a kill move): the engine's pairing places both actors.
		if (Furniture::Assisted(a_actor)) {
			return false;
		}
		// Its runner's actions meanwhile are dropped: they are its walk and its fight from before the pair, and played once
		// it is let go they would pull it out of the pair's end (its death, or where the pairing left it).
		if (Pairs::Held(a_actor)) {
			std::scoped_lock l{ copyLock };
			if (const auto byActor = copyByActor.find(a_actor); byActor != copyByActor.end()) {
				if (const auto it = copies.find(byActor->second); it != copies.end() && it->second.actorId == a_actor->GetFormID()) {
					heldActionsDropped.fetch_add(it->second.actions.Size(), std::memory_order_relaxed);
					it->second.actions.Clear();
				}
			}
			return false;
		}
		const bool seated = Riding::Seated(a_actor) || Furniture::Held(a_actor);
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
			step = Gait::Next(copy.shown, sample->state, sample->held, RemotePlayers::SwimsAsMode(a_actor));
			if (seated) {
				step.events.clear();  // a rider's graph rides: its walk is told again once it is off
			} else {
				copy.shown = step.shown;
			}
			events = copy.actions.Due(now - StateBuffer::Buffer::Delay());
			copy.lastDrawn = sample->state;
			copy.hasDrawn = true;
			++copy.drawn;
		}
		if (seated) {
			RemotePlayers::DriveLegs(a_actor, { step.speed, step.direction });
			a_actor->UpdateNoAI(a_delta);
			TellCopy(a_actor, key, step, events);
			return true;
		}
		// Its AI does not run: where its runner has it, turned its way (a flyer pitched too, and a swimmer under water),
		// its legs its runner's.
		a_actor->SetPosition({ sample->state.x, sample->state.y, sample->state.z }, true);
		// Its pitch is set every frame (Gait::CopyPitch: a flyer's, a diver's, else level), so a dragon that lands or a
		// swimmer taken out of the water from under it is level at once.
		const float pitch = Gait::CopyPitch(sample->state);
		if (pitch != 0.0f) {
			a_actor->SetAngle({ pitch, 0.0f, sample->state.heading });
		} else {
			a_actor->SetHeading(sample->state.heading);
			a_actor->data.angle.x = 0.0f;
		}
		RemotePlayers::ShowMoves(a_actor, Gait::Bits(step, sample->state));
		RemotePlayers::DriveLegs(a_actor, { step.speed, step.direction });
		a_actor->UpdateNoAI(a_delta);
		// In water, at its runner's depth: the water's pull to the surface does not lift it.
		float      drift = 0.0f;
		const bool held = RemotePlayers::HoldDepth(a_actor, sample->state, &drift);
		if ((sample->state.flags & Protocol::kSwimming) != 0) {
			std::scoped_lock l{ copyLock };
			if (const auto it = copies.find(key); it != copies.end() && it->second.actorId == a_actor->GetFormID()) {
				it->second.drift = drift;
				it->second.refloats += held ? 1 : 0;
			}
		}
		TellCopy(a_actor, key, step, events);
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
		othersCopyIds.Assign({});
		othersCopyList.clear();
		madeEnding.clear();
		loaded.clear();
		// The new world's actors are new ones: the session's dead are killed again as they load; our hits were on the old.
		deadHere.Clear();
		hitApplied.clear();
		std::scoped_lock f{ fightLock };
		ourHits.clear();
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

	namespace
	{
		std::string Spells(const std::vector<std::string_view>& a_words);  // `npcs spells` (below)
	}

	std::string Describe(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "spells") {
			return Spells(words);
		}
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
				"active={} loaded={} run={} owners={} copies={} shown={} mirrored={} spawnCopies={} kinds={} claims={} granted={} refused={} released={} hands={} letGo={} graphsReset={} statesSent={} statesIn={} "
				"actionsSent={} actionsIn={} played={} refusedEvents={} walkRefused={} eventsLost={} gearSent={} gearIn={} gearMissing={} gearSkipped={} gearTakenOff={} spawnsPlaced={} "
				"spawnsTaken={} unshared={} lookDiffers={} fallbacks={}/{}/{} hooked={} hitsSent={} hitsApplied={} hitsRefused={} hitsLost={} givenBack={} takenBack={} "
				"hitsIn={} hitsInApplied={} healthsSent={} healthsIn={} deathsSent={} deathsIn={} copyKills={} killsOnLoad={} deadRisen={} deadGivenUp={} saveKills={} revived={} healthSets={} dead={} "
				"pairActionsSent={} heldActionsDropped={}",
				active ? 1 : 0, loaded.size(), run, owners.size(), copies.size(), shown, mirrored, spawnCopies, kindList, counts.claims, counts.granted, counts.refused, counts.released,
				counts.hands, counts.letGo, counts.graphsReset, counts.statesSent, counts.statesIn, counts.actionsSent, counts.actionsIn, played, refused, walkRefused, eventsLost.load(), counts.gearSent, counts.gearIn,
				counts.gearMissing, counts.gearSkipped, counts.gearTakenOff, counts.spawnsPlaced, counts.spawnsTaken, counts.unshared, counts.lookDiffers, counts.fallbackSilent, counts.fallbackElsewhere,
				counts.fallbackFar, originalNotify ? 1 : 0, healthCounts.sent, healthCounts.applied, healthCounts.refused, healthCounts.lost, healthCounts.givenBack,
				healthCounts.takenBack, counts.hitsIn, counts.hitsApplied, counts.healthsSent, counts.healthsIn, counts.deathsSent, counts.deathsIn, healthCounts.kills, counts.killsOnLoad, deadHere.risen, deadHere.givenUp,
				healthCounts.saveKills, healthCounts.revived, counts.healthSets, sessionDead.size(), pairActionsSent, heldActionsDropped.load());
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
			// child: a child (kind human includes children); weapon: its weapon or hands drawn; combat: in combat.
			out += std::format(" | {:08X}{} local={:08X} owner={} role={} kind={} at={} dead={} child={} weapon={} combat={}", key.id,
				key.IsSpawn() ? std::format(" spawner={}", key.spawner) : "", localId, runner, role, npc != loaded.end() ? NpcRules::KindName(npc->second.kind) : "?", at,
				actor && actor->IsDead() ? 1 : 0, actor && actor->IsChild() ? 1 : 0, actor && actor->AsActorState() && actor->AsActorState()->IsWeaponDrawn() ? 1 : 0, actor && actor->IsInCombat() ? 1 : 0);
			// hp: its health here, as a share of its most; for a copy, its runner's word and our hits on it not answered yet;
			// sessionDead: its runner said it died.
			if (actor) {
				out += std::format(" hp={:.3f}", CopyHealth::ShareOf(HealthOf(actor), actor->GetActorValueMax(RE::ActorValue::kHealth)));
			}
			if (const auto h = health.find(key); h != health.end()) {
				const auto word = h->second.RunnersWord();
				out += std::format(" word={} inFlight={} runnerDead={}", word ? std::format("{:.3f}", *word) : std::string{ "none" }, h->second.InFlight(), h->second.RunnerDead() ? 1 : 0);
			}
			if (const auto death = sessionDead.find(key); death != sessionDead.end()) {
				out += std::format(" sessionDead=1 killer={}", death->second.killer);
			} else {
				out += " sessionDead=0";
			}
			if (only && actor) {
				// One NPC asked for: its base confidence and aggression too (the rig sets them for a fight and puts them back).
				if (const auto values = actor->AsActorValueOwner()) {
					out += std::format(" confidence={:.0f} aggression={:.0f}", values->GetBaseActorValue(RE::ActorValue::kConfidence), values->GetBaseActorValue(RE::ActorValue::kAggression));
					// and its health (the rig's combat step brings it low with the console before it is killed), essential or not
					out += std::format(" health={:.0f} max={:.0f} essential={} protected={}", values->GetActorValue(RE::ActorValue::kHealth), actor->GetActorValueMax(RE::ActorValue::kHealth),
						actor->IsEssential() ? 1 : 0, actor->IsProtected() ? 1 : 0);
				}
			}
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
				// geared: it was dressed; wears/left/hands/ammo/lacks: what it wears and holds, read back after (NpcRules::CheckGear),
				// and dressed=1 when that is all of it apart from what it lacks (?: not read back yet).
				std::string wears = " wears=? dressed=?";
				if (c.gear && c.geared && c.wears) {
					const auto& w = *c.wears;
					wears = std::format(" wears={}/{} leftOn={} hands={} ammo={} lacks={} dressed={}", w.matched, w.named, w.extra,
						w.right && w.left ? std::string{ "ok" } : std::format("{}/{}", w.right ? "ok" : "differs", w.left ? "ok" : "differs"), w.ammo ? "ok" : "differs", c.gearLacks,
						w.Complete(c.gearLacks) ? 1 : 0);
				}
				out += std::format(" runner={} mode={} target={} apart={} base={:08X} states={} drawn={} played={} refused={} walkRefused={}{} gear={} geared={}{} graphSpeed={}",
					c.feed.runner, NpcRules::ModeName(c.mode), target, apart, c.base, c.buffer.snapshots.size(), c.drawn, c.played, c.refused, c.walkRefused,
					c.lastRefused.empty() ? "" : " last=" + c.lastRefused, c.gear ? 1 : 0, c.geared ? 1 : 0, wears, hasSpeed ? std::format("{:.0f}", graphSpeed) : "none");
			}
		}
		return out;
	}

	RE::NiPointer<RE::Actor> ActorOf(const Protocol::NpcKey& a_key)
	{
		{
			std::scoped_lock l{ copyLock };
			if (const auto it = copies.find(a_key); it != copies.end() && it->second.actorId != 0) {
				if (auto actor = it->second.handle.get(); actor && actor->GetFormID() == it->second.actorId) {
					return actor;
				}
			}
		}
		if (const auto it = loaded.find(a_key); it != loaded.end()) {
			if (auto actor = it->second.handle.get(); actor && actor->GetFormID() == it->second.id) {
				return actor;
			}
		}
		return {};
	}

	std::optional<Protocol::NpcKey> KeyOf(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return std::nullopt;
		}
		{
			std::scoped_lock l{ copyLock };
			if (const auto it = copyByActor.find(a_actor); it != copyByActor.end()) {
				return it->second;
			}
		}
		const auto id = a_actor->GetFormID();
		const Key  key{ id, FormIds::IsRuntime(id) ? ourId : 0 };
		if (loaded.contains(key)) {
			return key;
		}
		return std::nullopt;
	}

	std::uint32_t RunnerOf(const Protocol::NpcKey& a_key)
	{
		const auto it = owners.find(a_key);
		return it != owners.end() ? it->second : 0;
	}

	bool Mirrored(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::scoped_lock l{ copyLock };
		const auto       byActor = copyByActor.find(const_cast<RE::Actor*>(a_actor));
		if (byActor == copyByActor.end()) {
			return false;
		}
		const auto it = copies.find(byActor->second);
		return it != copies.end() && it->second.mirror && it->second.actorId == a_actor->GetFormID();
	}

	std::optional<Clock::time_point> SenderTime(const Protocol::NpcKey& a_key, std::uint32_t a_sentMs)
	{
		if (a_sentMs == 0) {
			return std::nullopt;
		}
		std::scoped_lock l{ copyLock };
		const auto       it = copies.find(a_key);
		return it != copies.end() ? it->second.buffer.sender.At(a_sentMs) : std::nullopt;
	}

	std::vector<std::pair<Protocol::NpcKey, RE::NiPointer<RE::Actor>>> RunActors()
	{
		std::vector<std::pair<Protocol::NpcKey, RE::NiPointer<RE::Actor>>> out;
		for (const auto& [key, npc] : loaded) {
			if (!Runs(key)) {
				continue;
			}
			if (auto actor = npc.handle.get(); actor && actor->GetFormID() == npc.id) {
				out.emplace_back(key, std::move(actor));
			}
		}
		return out;
	}

	bool RefusesCopyMagic(const RE::Actor* a_caster, std::int32_t a_archetype)
	{
		const auto kind = SpellRules::JudgeCopyMagic(a_archetype);
		if (kind == SpellRules::CopyMagic::kLet || !a_caster || !refuseCopyMagic.load(std::memory_order_relaxed) || !othersCopyIds.Contains(a_caster->GetFormID())) {
			return false;
		}
		if (copyMagicRefused[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed) == 0) {
			logs::info("NPCs: a copy's effect that would {} here was refused, the first of its kind ({:08X}; its runner's game makes the real one; counted in `npcs spells`)",
				SpellRules::CopyMagicName(kind), a_caster->GetFormID());
		}
		return true;
	}

	namespace
	{
		std::string HexOrNone(const RE::TESForm* a_form)
		{
			return std::format("{:08X}", a_form ? a_form->GetFormID() : 0u);
		}

		// `npcs spells find <aimed|self|summon|power>`: a spell of the load order of that kind, for the rig: aimed, a
		// one-handed spell cast at a target; self, a one-handed spell on its caster (a heal, a flesh spell; no summon, no
		// bound weapon); summon, one whose costliest effect conjures a creature; power, a lesser power or a power. The
		// lowest id of Skyrim.esm first.
		std::string FindSpell(std::string_view a_kind)
		{
			using RE::MagicSystem::Delivery;
			using RE::MagicSystem::SpellType;
			const auto dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return "none: no data yet";
			}
			if (a_kind != "aimed" && a_kind != "self" && a_kind != "summon" && a_kind != "power") {
				return "error: usage: npcs spells find <aimed|self|summon|power>";
			}
			RE::SpellItem* best = nullptr;
			for (const auto spell : dh->GetFormArray<RE::SpellItem>()) {
				if (!spell || std::string_view{ spell->GetName() }.empty()) {
					continue;
				}
				const auto effect = spell->GetCostliestEffectItem();
				const auto setting = effect ? effect->baseEffect : nullptr;
				if (!setting) {
					continue;
				}
				const auto archetype = static_cast<std::int32_t>(std::to_underlying(setting->GetArchetype()));
				const auto type = spell->GetSpellType();
				const auto delivery = spell->GetDelivery();
				const bool oneHand = type == SpellType::kSpell && !Gear::TwoHanded(spell);
				bool       fits = false;
				if (a_kind == "aimed") {
					fits = oneHand && delivery == Delivery::kAimed && archetype != SpellRules::kArchSummonCreature;
				} else if (a_kind == "self") {
					fits = oneHand && delivery == Delivery::kSelf && SpellRules::JudgeCopyMagic(archetype) == SpellRules::CopyMagic::kLet && archetype != SpellRules::kArchBoundWeapon;
				} else if (a_kind == "summon") {
					fits = type == SpellType::kSpell && archetype == SpellRules::kArchSummonCreature;
				} else {
					fits = (type == SpellType::kLesserPower || type == SpellType::kPower) && SpellRules::JudgeCopyMagic(archetype) == SpellRules::CopyMagic::kLet;
				}
				if (fits && (!best || spell->GetFormID() < best->GetFormID())) {
					best = spell;
				}
			}
			if (!best) {
				return std::format("none: no {} spell", a_kind);
			}
			return std::format("spell id={:08X} kind={} casting={} name=\"{}\"", best->GetFormID(), a_kind,
				best->GetCastingType() == RE::MagicSystem::CastingType::kConcentration ? "held" : "ff", best->GetName());
		}

		// `npcs spells arm <local id> <right|left|voice> <spell id | 0>`: the rig's set-up of an NPC this game runs: the
		// spell (or power, or shout for the voice) taught if it does not know it and put in that hand; 0 empties the hand
		// (or the voice) and takes back what arm taught it that it no longer holds.
		std::string Arm(const std::vector<std::string_view>& a_words)
		{
			const auto local = a_words.size() == 5 ? FormId(a_words[2]) : std::nullopt;
			const auto id = a_words.size() == 5 ? FormId(a_words[4]) : std::nullopt;
			const auto handWord = a_words.size() == 5 ? a_words[3] : std::string_view{};
			if (!local || !id || (handWord != "right" && handWord != "left" && handWord != "voice")) {
				return "error: usage: npcs spells arm <local id> <right|left|voice> <spell id | 0>";
			}
			const auto actor = RE::TESForm::LookupByID<RE::Actor>(*local);
			const auto key = actor ? KeyOf(actor) : std::nullopt;
			if (!actor || !key || !Runs(*key)) {
				return std::format("error: {:08X} is no NPC this game runs", *local);
			}
			const bool voice = handWord == "voice";
			const auto hand = handWord == "left" ? NpcRules::Hand::kLeft : NpcRules::Hand::kRight;
			const auto holds = [&] {
				return voice ? HexOrNone(Gear::VoiceOf(actor)) : HexOrNone(actor->GetEquippedObject(hand == NpcRules::Hand::kLeft));
			};
			if (*id == 0) {
				if (voice) {
					Gear::SetVoice(actor, 0, false);
				} else if (const auto held = actor->GetEquippedObject(hand == NpcRules::Hand::kLeft); Gear::IsSpell(held)) {
					// DeselectSpell empties both hands of it: the other hand's, when it held the same, is put back.
					const bool other = actor->GetEquippedObject(hand != NpcRules::Hand::kLeft) == held;
					actor->DeselectSpell(held->As<RE::SpellItem>());
					if (other) {
						static_cast<void>(Gear::EquipOne(actor, held, hand == NpcRules::Hand::kLeft ? NpcRules::Hand::kRight : NpcRules::Hand::kLeft));
					}
				}
				std::size_t untaught = 0;
				for (auto it = armTaught.begin(); it != armTaught.end();) {
					const auto spell = it->first == actor->GetFormID() ? RE::TESForm::LookupByID<RE::SpellItem>(it->second) : nullptr;
					const bool held = spell && (actor->GetEquippedObject(false) == spell || actor->GetEquippedObject(true) == spell || Gear::VoiceOf(actor) == spell);
					if (spell && !held) {
						actor->RemoveSpell(spell);
						++untaught;
						it = armTaught.erase(it);
					} else {
						++it;
					}
				}
				return std::format("cleared {} holds={} untaught={}", handWord, holds(), untaught);
			}
			const auto form = RE::TESForm::LookupByID(*id);
			const auto spell = form && Gear::IsSpell(form) ? form->As<RE::SpellItem>() : nullptr;
			if (!spell && !(voice && form && form->Is(RE::FormType::Shout))) {
				return std::format("error: {:08X} is no spell{}", *id, voice ? ", power or shout" : "");
			}
			bool taught = false;
			if (spell && !actor->HasSpell(spell)) {
				actor->AddSpell(spell);
				armTaught.emplace(actor->GetFormID(), spell->GetFormID());
				taught = true;
			}
			if (voice) {
				if (!Gear::SetVoice(actor, *id, true)) {
					return std::format("error: the voice refused {:08X} (taught={})", *id, taught ? 1 : 0);
				}
			} else if (const auto why = Gear::EquipOne(actor, form, hand); !why.empty()) {
				return std::format("error: {} (taught={})", why, taught ? 1 : 0);
			}
			return std::format("armed {} {:08X} taught={} holds={}", handWord, *id, taught ? 1 : 0, holds());
		}

		// How many actors a stand-in or another game's NPC's copy commands here now (up: alive or raised; enabled), made as
		// the game ran (a summon) or of the load order (a raised corpse, a thrall).
		std::size_t MadeNow()
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			std::size_t n = 0;
			if (!lists) {
				return n;
			}
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (const auto& handle : *handles) {
					const auto actor = handle.get();
					if (!actor || actor->IsDisabled() || actor->IsDeleted() || !Up(actor.get())) {
						continue;
					}
					const auto by = actor->GetCommandingActor();
					n += by && SpellRules::EndsMade(MadeByOf(by.get())) ? 1 : 0;
				}
			}
			return n;
		}

		// `npcs spells [<form id>]`: the counts, then each copy that holds or should hold a spell or a voice (its runner's
		// against its own, each hand judged: SpellRules::JudgeHand), its magicka and the casts its graph played, and each
		// NPC this game runs that holds one, with the casts it said. One NPC asked for is listed whatever it holds.
		std::string Spells(const std::vector<std::string_view>& a_words)
		{
			if (a_words.size() >= 2 && a_words[1] == "find") {
				return a_words.size() == 3 ? FindSpell(a_words[2]) : "error: usage: npcs spells find <aimed|self|summon|power>";
			}
			if (a_words.size() >= 2 && a_words[1] == "arm") {
				return Arm(a_words);
			}
			if (a_words.size() >= 2 && a_words[1] == "refuse") {
				if (a_words.size() != 3 || (a_words[2] != "on" && a_words[2] != "off")) {
					return "error: usage: npcs spells refuse <on|off>";
				}
				refuseCopyMagic = a_words[2] == "on";
				return std::format("refuse={}", refuseCopyMagic ? 1 : 0);
			}
			if (a_words.size() >= 2 && a_words[1] == "knows") {
				// Whether an actor (any: the rig asks of a copy before its console teaches it) knows a spell, a power or a
				// shout, so the step takes back only what it taught.
				const auto local = a_words.size() == 4 ? FormId(a_words[2]) : std::nullopt;
				const auto id = a_words.size() == 4 ? FormId(a_words[3]) : std::nullopt;
				if (!local || !id) {
					return "error: usage: npcs spells knows <local id> <spell id>";
				}
				const auto actor = RE::TESForm::LookupByID<RE::Actor>(*local);
				if (!actor) {
					return std::format("error: {:08X} is no actor", *local);
				}
				if (!Gear::IsMagic(*id)) {
					return std::format("error: {:08X} is no spell, power or shout", *id);
				}
				return std::format("knows={}", Gear::Knows(actor, *id) ? 1 : 0);
			}
			const auto only = a_words.size() == 2 ? FormId(a_words[1]) : std::nullopt;
			if (a_words.size() > 2 || (a_words.size() == 2 && !only)) {
				return "error: usage: npcs spells [<form id> | find <kind> | arm <local id> <right|left|voice> <spell id | 0> | knows <local id> <spell id> | refuse <on|off>]";
			}
			const auto  madeNow = MadeNow();
			std::string refused;
			for (std::size_t i = 1; i < copyMagicRefused.size(); ++i) {
				refused += std::format("{}{}:{}", refused.empty() ? "" : ",", SpellRules::CopyMagicName(static_cast<SpellRules::CopyMagic>(i)), copyMagicRefused[i].load());
			}
			std::uint64_t castsSent = 0;
			for (const auto& [key, npc] : loaded) {
				castsSent += npc.castsSent;
			}
			std::string   rows;
			std::size_t   others = 0;
			std::size_t   holding = 0;
			std::uint64_t played = 0;
			std::uint64_t turned = 0;
			std::array<std::size_t, 5> states{};
			{
				std::scoped_lock l{ copyLock };
				for (const auto& [key, copy] : copies) {
					if (!copy.othersRun || copy.actorId == 0) {
						continue;
					}
					++others;
					holding += copy.holdsMagic ? 1 : 0;
					played += copy.castsPlayed;
					turned += copy.castsRefused;
					const auto actor = copy.handle.get();
					if (!actor || actor->GetFormID() != copy.actorId) {
						continue;
					}
					const auto  gear = copy.gear.value_or(Protocol::NpcGear{});
					const auto  [right, left] = Gear::HeldObjects(actor.get());
					const auto  voice = Gear::VoiceOf(actor.get());
					const bool  spawned = copy.spawned;
					std::string hands;
					bool        magic = false;
					const auto  judge = [&](std::string_view a_name, std::uint32_t a_want, std::uint8_t a_bit, const RE::TESForm* a_held) {
						const auto held = a_held ? a_held->GetFormID() : 0u;
						const bool wantMagic = Gear::IsMagic(a_want);
						magic = magic || wantMagic || Gear::IsMagic(held);
						const auto state = SpellRules::JudgeHand(a_want, (gear.unnamed & a_bit) != 0, held, wantMagic, Gear::Knows(actor.get(), a_want), spawned);
						++states[static_cast<std::size_t>(state)];
						hands += std::format(" {}={:08X}/{:08X}:{}", a_name, a_want, held, SpellRules::HandName(state));
					};
					judge("right", gear.right, Protocol::kRightUnnamed, right);
					judge("left", gear.left, Protocol::kLeftUnnamed, left);
					judge("voice", gear.voice, Protocol::kVoiceUnnamed, voice);
					const bool asked = only && (*only == key.id || *only == copy.actorId);
					if ((!magic && !asked) || (only && !asked)) {
						continue;
					}
					const auto values = actor->AsActorValueOwner();
					rows += std::format(" | {:08X}{} local={:08X} role=copy runner={} mirror={} gear={}{} magicka={:.0f}/{:.0f} magickaBack={:.0f} castsPlayed={} castsRefused={} lastCast={}", key.id,
						key.IsSpawn() ? std::format(" spawner={}", key.spawner) : "", copy.actorId, copy.feed.runner, copy.mirror ? 1 : 0, copy.gear ? 1 : 0, hands,
						values ? values->GetActorValue(RE::ActorValue::kMagicka) : 0.0f, actor->GetActorValueMax(RE::ActorValue::kMagicka), copy.magickaBack, copy.castsPlayed,
						copy.castsRefused, copy.lastCast.empty() ? "none" : copy.lastCast);
				}
			}
			for (const auto& [key, npc] : loaded) {
				if (!Runs(key)) {
					continue;
				}
				const auto actor = npc.handle.get();
				if (!actor || actor->GetFormID() != npc.id) {
					continue;
				}
				const bool asked = only && (*only == key.id || *only == npc.id);
				if ((only && !asked) || (!asked && !Gear::HoldsMagic(actor.get()))) {
					continue;
				}
				const auto [right, left] = Gear::HeldObjects(actor.get());
				rows += std::format(" | {:08X}{} local={:08X} role=run right={} left={} voice={} castsSent={}", key.id, key.IsSpawn() ? std::format(" spawner={}", key.spawner) : "", npc.id,
					HexOrNone(right), HexOrNone(left), HexOrNone(Gear::VoiceOf(actor.get())), npc.castsSent);
			}
			return std::format("spells copies={} holding={} hands=shown:{},lacks:{},unnamed:{},differs:{} castsPlayed={} castsRefused={} castsSent={} voicesSet={} voicesLacked={} "
							   "magickaTopUps={} refuse={} refused={} madeEnded={} madeDispelled={} madeRemoved={} madeRaised={} madeNow={}{}",
				others, holding, states[static_cast<std::size_t>(SpellRules::Hand::kShown)], states[static_cast<std::size_t>(SpellRules::Hand::kLacks)],
				states[static_cast<std::size_t>(SpellRules::Hand::kUnnamed)], states[static_cast<std::size_t>(SpellRules::Hand::kDiffers)], played, turned, castsSent, counts.voicesSet,
				counts.voicesLacked, counts.magickaTopUps, refuseCopyMagic ? 1 : 0, refused, counts.madeEnded, counts.madeDispelled, counts.madeRemoved, counts.madeRaised, madeNow, rows);
		}
	}

	std::string Swim()
	{
		constexpr std::size_t MAX_SWIM_LISTED = 8;
		std::size_t           swimming = 0;  // copies whose runner's newest state drawn has them swimming
		std::size_t           under = 0;
		std::size_t           shown = 0;  // ... shown swimming (their graph told so)
		std::uint64_t         taken = 0, refused = 0, givenUp = 0, refloats = 0;
		std::string           list;
		std::size_t           listed = 0;
		std::scoped_lock      l{ copyLock };
		for (const auto& [key, copy] : copies) {
			taken += copy.swimTaken;
			refused += copy.swimRefused;
			givenUp += copy.swimGivenUp;
			refloats += copy.refloats;
			if (!copy.hasDrawn) {
				continue;
			}
			const auto actor = copy.handle.get();
			const bool here = actor && actor->GetFormID() == copy.actorId;
			const bool runnerSwims = (copy.lastDrawn.flags & Protocol::kSwimming) != 0;
			const bool walks = !here || RemotePlayers::SwimsAsMode(actor.get());
			// Listed: each copy whose runner has it swimming, and each copy of a race that does not walk (a fish, whose
			// graph is never told to swim: the rig checks it was told nothing).
			if (!runnerSwims && walks) {
				continue;
			}
			if (runnerSwims) {
				++swimming;
				under += (copy.lastDrawn.flags & Protocol::kUnderwater) != 0 ? 1 : 0;
				shown += copy.shown.swimming ? 1 : 0;
			}
			if (listed < MAX_SWIM_LISTED) {
				// What the engine says of it (its own flags and its character controller's state; 5 is swimming), not
				// only what the plugin set and told its graph.
				int engineSwimming = -1;
				int charState = -1;
				if (here) {
					engineSwimming = actor->GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kSwimming) ? 1 : 0;
					if (const auto controller = actor->GetCharController()) {
						charState = static_cast<int>(std::to_underlying(controller->context.currentState));
					}
				}
				list += std::format(" | copy {:08X}{} base={:08X} walks={} runnerSwims={} shown={} under={} engineSwimming={} charState={} swimTaken={} swimRefused={}"
									" swimGivenUp={} refloats={} drift={:.0f} z={} runnerZ={:.0f} depthErr={}",
					key.id, key.IsSpawn() ? std::format(" spawner={}", key.spawner) : "", copy.base, walks ? 1 : 0, runnerSwims ? 1 : 0,
					copy.shown.swimming ? "w" : "-", (copy.lastDrawn.flags & Protocol::kUnderwater) != 0 ? 1 : 0, engineSwimming, charState, copy.swimTaken,
					copy.swimRefused, copy.swimGivenUp, copy.refloats, copy.drift, here ? std::format("{:.0f}", actor->GetPositionZ()) : "none", copy.lastDrawn.z,
					here ? std::format("{:.0f}", actor->GetPositionZ() - copy.lastDrawn.z) : "none");
				++listed;
			}
		}
		return std::format(" | copies swimming={} under={} shownSwimming={} swimTaken={} swimRefused={} swimGivenUp={} refloats={}{}", swimming, under, shown, taken,
			refused, givenUp, refloats, list);
	}
}
