#include "game/QuestSync.h"

#include "DevLine.h"
#include "game/Console.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/QuestRules.h"
#include "game/QuestRunRules.h"
#include "game/RemotePlayers.h"
#include "net/FormsCommand.h"

namespace QuestSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Entry = Protocol::QuestEntry;
		using QuestRules::Class;
		using Fill = Protocol::AliasFill;

		// A stage event, or a quest's start or stop, the sinks queued (one queue: read on the next frame in the order
		// the game sent them).
		struct StageEvent
		{
			enum Kind : std::uint8_t
			{
				kStage,
				kStart,
				kStop,
			};
			RE::FormID    quest = 0;
			std::uint16_t stage = 0;
			Kind          kind = kStage;
		};

		std::mutex              queueLock;
		std::vector<StageEvent> queue;  // under queueLock
		constexpr std::size_t   MAX_QUEUED = 1024;
		std::uint64_t           queueDropped = 0;  // under queueLock

		// Every quest of the game, judged once by its record (QuestRules::Classify), built on the first frame in the world.
		struct Known
		{
			RE::TESQuest* quest = nullptr;
			Class         cls = Class::kUntyped;
		};
		std::vector<Known>                               catalog;
		std::unordered_map<RE::FormID, std::size_t>      byId;
		std::array<std::size_t, 6>                       classCounts{};
		std::optional<QuestRules::BitVerdict>            bit;  // judged once, with the catalog

		std::uint32_t                  ourId = 0;
		bool                           host = false;
		QuestRules::View               view;
		QuestRules::Maker              maker;
		QuestRules::MoveGuard          guard;
		QuestRules::Apart              apart;
		// The stages this game set while it ran (their events), by quest: done whatever the done bit reads.
		std::unordered_map<RE::FormID, std::unordered_set<std::uint16_t>> seen;
		struct Again
		{
			Entry             entry;
			Clock::time_point due;
		};
		std::vector<Again>   again;  // reports answered busy, to send again
		// Our stages of a quest whose session word may still be coming (QuestRules::Apart, kWait): judged again each frame.
		std::vector<Entry>   heldStages;
		constexpr std::size_t MAX_HELD = 1024;
		// Things made here by the console, read again a moment later: one the game does not read so did not take.
		struct Check
		{
			Entry             entry;
			Clock::time_point due;
		};
		std::vector<Check>   checks;
		constexpr auto       CHECK_AFTER = std::chrono::seconds{ 2 };
		constexpr std::size_t MAX_CHECKS = 256;
		bool                 askAgain = false;
		bool                 sayWorld = false;  // the host's game: kQuestReset and its snapshot are due
		bool                 rebase = false;    // this game's quests as they are now are its base (QuestRules::View)
		Clock::time_point    nextPoll{};
		constexpr auto       POLL_EVERY = std::chrono::seconds{ 1 };
		constexpr int        STEPS_PER_FRAME = 16;  // things judged per frame (one made at most: Maker's pace)

		struct Counts
		{
			std::uint64_t events = 0;      // stage events queued by the sink
			std::uint64_t sent = 0;        // entries reported
			std::uint64_t heard = 0;       // entries of the session's word new to this game
			std::uint64_t made = 0;        // ... made here (a console command run)
			std::uint64_t has = 0;         // ... this game had already
			std::uint64_t missing = 0;     // ... about a quest this game lacks
			std::uint64_t notHere = 0;     // ... about a quest this game's record does not share
			std::uint64_t own = 0;         // ... a stage the rules keep each game's own
			std::uint64_t farPast = 0;     // ... a stage that moves the player, said as the session's past: not made
			std::uint64_t passed = 0;      // ... a stage below the current one of a quest started here: not set again
			std::uint64_t notStarted = 0;  // ... an objective or end of a quest not started here
			std::uint64_t failedKept = 0;  // ... an objective failed or a quest failed: not made (no console for it)
			std::uint64_t answered = 0;    // our entries the server answered (not news)
			std::uint64_t busy = 0;        // ... of them busy: sent again
			std::uint64_t refused = 0;     // ... refused or full: kept as this game's own, not offered again
			std::uint64_t asks = 0;        // the session's word asked for again (a load)
			std::uint64_t resets = 0;      // the host's world new (heard)
			std::uint64_t rebases = 0;     // this game's quests taken as its base (a welcome, a reset, a load)
			std::uint64_t snapshots = 0;   // our world said, as the host
			std::uint64_t snapshotEntries = 0;
			std::uint64_t apartStages = 0; // our stages of a quest apart from the session: this game's own
			std::uint64_t moved = 0;       // stages made here that moved our player (MoveGuard)
			std::uint64_t unmade = 0;      // objectives and ends made here that did not read so after CHECK_AFTER
			std::uint64_t offMain = 0;     // console commands run off the game's main thread
			std::uint64_t noRun = 0;       // console commands the game made no script for
			// Runs (QuestRunRules.h)
			std::uint64_t runsSent = 0;      // the host's game: run words sent (a start, its fills, a radiant job's stop)
			std::uint64_t runsHeard = 0;     // a friend's game: run words heard
			std::uint64_t forced = 0;        // references forced into an alias here (a quest running as the session's run)
			std::uint64_t forcedAtStart = 0; // reference aliases a quest started here for its run filled with the host's reference
			std::uint64_t locsAtStart = 0;   // location aliases ... filled with the host's location
			std::uint64_t ownAtStart = 0;    // ... left to this game's own pick (a created actor, a form not loaded or lacking)
			std::uint64_t startFailed = 0;   // starts for a run the game would not make
			std::uint64_t cannotStart = 0;   // starts for a run not tried: an alias only the Story Manager's event fills had nothing forced
			std::uint64_t stopsMade = 0;     // radiant jobs stopped here for the host's stop
			std::uint64_t restartsMade = 0;  // radiant jobs stopped here to start again with the host's location
			std::uint64_t rewanted = 0;      // the session's word on a quest made again when its run came to run late
			std::uint64_t locUnread = 0;     // a quest's location map held a value that is no location (not used)
		};
		Counts      counts;
		QuestRunRules::Keeper   keeper;    // the host's game: the runs it says
		QuestRunRules::Follower follower;  // a friend's game: the host's runs, made here
		std::string lastRun = "none";

		// Location aliases (QuestRunRules::LocProbe, LocCheck): the slot of a location alias's record that holds its forced
		// location (found at the catalog, or none), every location of the load order (a value read from the engine is used
		// only when it is one of them: nothing unknown is ever read through), and whether the quests' location maps read as
		// they must.
		std::optional<std::size_t>          locSlot;
		std::unordered_set<std::uintptr_t>  locations;
		QuestRunRules::LocCheck             locCheck;
		std::unordered_set<std::uint64_t>   locChecked;  // quest and alias ids checked already
		constexpr std::size_t               LOC_SLOT_BASE = sizeof(RE::BGSBaseAlias);

		// An alias's record as it was before a start for a run rewrote it to a forced fill of the host's reference or
		// location: put back once the quest runs (or the start is given up, or before a load). Records are the game's data,
		// never saved.
		struct Rewrite
		{
			RE::BGSBaseAlias*          alias = nullptr;
			std::uint16_t              fillType = 0;
			std::size_t                offset = 0;  // the fill's bytes in the record
			std::size_t                size = 0;
			std::array<std::byte, 16>  bytes{};
		};
		struct Rewritten
		{
			RE::FormID           quest = 0;
			std::vector<Rewrite> rewrites;
			Clock::time_point    since{};
		};
		std::vector<Rewritten> rewritten;
		std::string lastMoved = "none";
		std::string lastMade = "none";
		std::string lastUnmade = "none";

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		std::string EditorId(const RE::TESQuest* a_quest)
		{
			const auto id = a_quest ? a_quest->formEditorID.c_str() : nullptr;
			return id ? std::string{ id } : std::string{};
		}

		std::string QuestName(std::uint32_t a_questId);
		void        NoteThread();

		QuestRules::Facts FactsOf(const RE::TESQuest* a_quest)
		{
			return {
				.type = static_cast<std::uint8_t>(a_quest->data.questType.underlying()),
				.event = static_cast<std::int32_t>(a_quest->eventID),
				.flags = a_quest->data.flags.underlying(),
				.editorId = EditorId(a_quest),
			};
		}

		// A quest's stages as the engine keeps them: its index, whether bit 0 (done) is set, and its record flags. An
		// entry all zero is the list's empty head (a stage 0 with no flags reads the same and is not listed).
		struct StageRead
		{
			std::uint16_t index = 0;
			bool          done = false;
			bool          startUp = false;
			bool          shutDown = false;
		};
		std::vector<StageRead> Stages(const RE::TESQuest* a_quest)
		{
			std::vector<StageRead> out;
			if (!a_quest || !a_quest->executedStages) {
				return out;
			}
			for (const auto& stage : *a_quest->executedStages) {
				if (!stage) {
					continue;
				}
				const auto flags = stage.data.flags.underlying();
				out.push_back({ stage.data.index, (flags & 1) != 0,
					stage.data.flags.any(RE::QUEST_STAGE_DATA::Flag::kStartUpStage), stage.data.flags.any(RE::QUEST_STAGE_DATA::Flag::kShutDownStage) });
			}
			std::ranges::sort(out, {}, &StageRead::index);
			return out;
		}

		std::vector<std::pair<std::uint16_t, std::uint8_t>> Objectives(RE::TESQuest* a_quest)
		{
			std::vector<std::pair<std::uint16_t, std::uint8_t>> out;
			if (!a_quest) {
				return out;
			}
			for (const auto objective : a_quest->objectives) {
				if (objective) {
					out.emplace_back(objective->index, static_cast<std::uint8_t>(objective->state.underlying()));
				}
			}
			std::ranges::sort(out);
			return out;
		}

		bool          Failed(const RE::TESQuest* a_quest) { return a_quest->data.flags.any(RE::QuestFlag::kFailed); }
		std::uint16_t Current(RE::TESQuest* a_quest) { return a_quest->GetCurrentStageID(); }
		// The quest runs here, or has ended here.
		bool Started(RE::TESQuest* a_quest) { return a_quest->IsEnabled() || a_quest->IsCompleted() || Failed(a_quest); }

		// Whether a stage is done in this game: set while it ran (its event), its current stage, or the done bit where
		// that is trusted (QuestRules::JudgeBit). A stage below the current one is judged apart (QuestRules::Judge:
		// passed, never set again).
		bool StageDone(RE::TESQuest* a_quest, std::uint16_t a_stage)
		{
			if (const auto it = seen.find(a_quest->GetFormID()); it != seen.end() && it->second.contains(a_stage)) {
				return true;
			}
			if (Current(a_quest) == a_stage && a_stage != 0) {
				return true;
			}
			if (bit && bit->trusted) {
				for (const auto& s : Stages(a_quest)) {
					if (s.index == a_stage) {
						return s.done;
					}
				}
			}
			return false;
		}

		std::uint8_t ObjectiveState(RE::TESQuest* a_quest, std::uint16_t a_index)
		{
			for (const auto& [index, state] : Objectives(a_quest)) {
				if (index == a_index) {
					return state;
				}
			}
			return 0;
		}

		RE::TESQuest* QuestOf(RE::FormID a_id)
		{
			return a_id != 0 ? RE::TESForm::LookupByID<RE::TESQuest>(a_id) : nullptr;
		}

		std::string QuestName(std::uint32_t a_questId)
		{
			const auto quest = QuestOf(a_questId);
			const auto name = quest ? EditorId(quest) : std::string{};
			return name.empty() ? std::format("{:08X}", a_questId) : name;
		}

		Class ClassOf(RE::TESQuest* a_quest)
		{
			const auto it = byId.find(a_quest->GetFormID());
			return it != byId.end() ? catalog[it->second].cls : QuestRules::Classify(FactsOf(a_quest));
		}

		// Whether this game shares the quest now: a shared one by its record; a radiant job while it runs as the
		// session's run (the host's game: one it runs; a friend's: one it runs for the host's run). A friend's own radiant
		// job is its own.
		bool SharedHere(RE::TESQuest* a_quest, Class a_class)
		{
			if (a_class == Class::kShared) {
				return true;
			}
			if (a_class != Class::kRadiant) {
				return false;
			}
			return host ? keeper.Live(a_quest->GetFormID()) : follower.Ours(a_quest->GetFormID());
		}
		bool SharedHere(RE::TESQuest* a_quest) { return SharedHere(a_quest, ClassOf(a_quest)); }

		// ---- runs and aliases (QuestRunRules.h) ----

		bool Running(RE::TESQuest* a_quest) { return a_quest->IsEnabled() && !a_quest->IsStarting(); }

		QuestRunRules::Here HereRun(RE::TESQuest* a_quest)
		{
			QuestRunRules::Here here;
			if (!a_quest) {
				return here;
			}
			const auto cls = ClassOf(a_quest);
			here.exists = true;
			here.shared = cls == Class::kShared;
			here.radiant = cls == Class::kRadiant;
			here.starting = a_quest->IsEnabled() && a_quest->IsStarting();
			here.running = Running(a_quest);
			// a radiant job ends again and again: only a shared quest ended here is ended for good
			here.ended = !here.radiant && !a_quest->IsEnabled() && (a_quest->IsCompleted() || Failed(a_quest));
			return here;
		}

		bool IsRefAlias(const RE::BGSBaseAlias* a_alias) { return a_alias && a_alias->GetVMTypeID() == RE::BGSRefAlias::VMTYPEID; }
		bool IsLocAlias(const RE::BGSBaseAlias* a_alias) { return a_alias && a_alias->GetVMTypeID() == RE::BGSLocAlias::VMTYPEID; }
		QuestRunRules::FillType FillTypeOf(const RE::BGSBaseAlias* a_alias) { return static_cast<QuestRunRules::FillType>(a_alias->fillType.underlying()); }

		// A location alias's record: the base alias, LOC_SLOTS pointer-sized members not named yet, and its conditions.
		static_assert(sizeof(RE::BGSBaseAlias) + QuestRunRules::LOC_SLOTS * sizeof(std::uintptr_t) + sizeof(RE::TESCondition*) == sizeof(RE::BGSLocAlias));

		// Slot a_slot of a location alias's record, as a number (compared with the load order's locations, never read
		// through).
		std::uintptr_t SlotValue(const RE::BGSBaseAlias* a_alias, std::size_t a_slot)
		{
			std::uintptr_t value = 0;
			std::memcpy(&value, reinterpret_cast<const std::byte*>(a_alias) + LOC_SLOT_BASE + a_slot * sizeof(std::uintptr_t), sizeof value);
			return value;
		}

		// The location the quest's location alias holds here, read from the quest's map of filled aliases beside its
		// references' (RE::TESQuest::unk0A0: alias id -> location, as the same engine's Fallout 4 keeps its aliasedLocMap),
		// walked as the library lays it out (no hash of ours) under the quest's alias lock. A value is used only when it is
		// a location of the load order (nothing else is read through); one that is not is counted (locUnread) and none.
		// nullptr: the alias holds nothing.
		std::optional<RE::BGSLocation*> LocationOf(RE::TESQuest* a_quest, std::uint32_t a_aliasId)
		{
			RE::BSReadLockGuard lock{ a_quest->aliasAccessLock };
			for (const auto& entry : a_quest->unk0A0) {
				if (static_cast<std::uint32_t>(entry.first) != a_aliasId) {
					continue;
				}
				if (!locations.contains(entry.second)) {
					++counts.locUnread;
					return std::nullopt;
				}
				return reinterpret_cast<RE::BGSLocation*>(entry.second);
			}
			return nullptr;
		}

		// At the catalog: the load order's locations, and the slot of a location alias's record that holds the location
		// it is forced to (QuestRunRules::LocProbe over every alias the load order forces to a specific location).
		void ProbeLocations(RE::TESDataHandler* a_data)
		{
			for (const auto loc : a_data->GetFormArray<RE::BGSLocation>()) {
				if (loc) {
					locations.insert(reinterpret_cast<std::uintptr_t>(loc));
				}
			}
			QuestRunRules::LocProbe probe;
			std::size_t             none = 0;
			for (const auto& known : catalog) {
				for (const auto alias : known.quest->aliases) {
					if (!IsLocAlias(alias) || FillTypeOf(alias) != QuestRunRules::FillType::kForced) {
						continue;
					}
					std::uint8_t hits = 0;
					for (std::size_t i = 0; i < QuestRunRules::LOC_SLOTS; ++i) {
						if (locations.contains(SlotValue(alias, i))) {
							hits = static_cast<std::uint8_t>(hits | (1u << i));
						}
					}
					if (hits == 0) {
						++none;  // forced to no location (or kept where this game does not look)
						continue;
					}
					probe.Sample(hits);
				}
			}
			locSlot = probe.Slot();
			if (locSlot) {
				logs::info("Quests: {} locations; a location alias's record keeps its forced location at +0x{:X} ({} aliases forced to a location agree, {} hold none): "
						   "a run's locations are given at its start",
					locations.size(), LOC_SLOT_BASE + *locSlot * sizeof(std::uintptr_t), probe.Samples(), none);
			} else {
				logs::warn("Quests: {} locations; where a location alias's record keeps its forced location was not found ({} aliases forced to a location read, "
						   "{} hold none): a run's locations stay each game's own pick here",
					locations.size(), probe.Samples(), none);
			}
		}

		// Whether the quests' location maps read as they must (QuestRunRules::LocCheck): each running quest's location
		// alias forced by its record to a location must hold that one. Every 10 s until decided.
		Clock::time_point nextLocCheck{};
		void CheckLocations(Clock::time_point a_now)
		{
			if (!locSlot || locCheck.Now() != QuestRunRules::LocCheck::State::kUnread || a_now < nextLocCheck) {
				return;
			}
			nextLocCheck = a_now + std::chrono::seconds{ 10 };
			for (const auto& known : catalog) {
				const auto quest = known.quest;
				if (!quest->IsEnabled() || quest->IsStarting()) {
					continue;
				}
				for (const auto alias : quest->aliases) {
					if (!IsLocAlias(alias) || FillTypeOf(alias) != QuestRunRules::FillType::kForced) {
						continue;
					}
					const auto said = SlotValue(alias, *locSlot);
					if (!locations.contains(said) || !locChecked.insert((std::uint64_t{ quest->GetFormID() } << 32) | alias->aliasID).second) {
						continue;
					}
					const auto held = LocationOf(quest, alias->aliasID);
					locCheck.Sample(held && *held && reinterpret_cast<std::uintptr_t>(*held) == said);
					if (locCheck.Now() != QuestRunRules::LocCheck::State::kUnread) {
						if (locCheck.Now() == QuestRunRules::LocCheck::State::kVerified) {
							logs::info("Quests: the quests' location maps read as they must ({} of {} forced location aliases of running quests hold their location): "
									   "a run's locations are said",
								locCheck.Held(), locCheck.Samples());
						} else {
							logs::warn("Quests: the quests' location maps do not read as they must (only {} of {} forced location aliases of running quests hold "
									   "their location): a run's locations are never said, each game's own pick",
								locCheck.Held(), locCheck.Samples());
						}
						return;
					}
				}
			}
		}

		bool LocationsRead() { return locSlot && locCheck.Now() == QuestRunRules::LocCheck::State::kVerified; }

		// What the quest's aliases hold here, in this game's numbering: its reference aliases' references, and its
		// location aliases' locations (unknown while the location map is not checked: QuestRunRules::LocFillOf);
		// a_locations counts its location aliases.
		std::vector<Fill> ReadFills(RE::TESQuest* a_quest, std::size_t* a_locations = nullptr)
		{
			std::vector<Fill> out;
			if (!a_quest) {
				return out;
			}
			const bool read = LocationsRead();
			for (const auto alias : a_quest->aliases) {
				if (!alias || out.size() >= Protocol::MAX_ALIAS_FILLS) {
					continue;
				}
				if (IsLocAlias(alias)) {
					if (a_locations) {
						++*a_locations;
					}
					std::optional<RE::BGSLocation*> loc;
					if (read) {
						loc = LocationOf(a_quest, alias->aliasID);
					}
					out.push_back(QuestRunRules::LocFillOf(alias->aliasID, loc && *loc ? (*loc)->GetFormID() : 0, loc.has_value()));
					continue;
				}
				if (!IsRefAlias(alias)) {
					continue;
				}
				const auto ref = a_quest->GetAliasedRef(alias->aliasID).get();
				out.push_back(QuestRunRules::FillOf(alias->aliasID, ref ? ref->GetFormID() : 0));
			}
			return out;
		}

		RE::TESObjectREFR* RefOf(std::uint32_t a_refId)
		{
			return a_refId != 0 && !FormIds::IsRuntime(a_refId) ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refId) : nullptr;
		}

		RE::BGSLocation* LocOf(std::uint32_t a_locId)
		{
			if (a_locId == 0 || FormIds::IsRuntime(a_locId)) {
				return nullptr;
			}
			const auto loc = RE::TESForm::LookupByID<RE::BGSLocation>(a_locId);
			return loc && locations.contains(reinterpret_cast<std::uintptr_t>(loc)) ? loc : nullptr;
		}

		// A location is given only at a start: a radiant job this game took as the run (it ran here with its own pick) that
		// holds a_apart locations other than the host's is stopped, to be started again for the run (once a run; its stages
		// made again after). True when stopped. A shared quest is never stopped for it (its progress here is the
		// session's): ForceFills says its locations apart once.
		bool RestartForLocations(RE::TESQuest* a_quest, std::size_t a_apart, Clock::time_point a_now)
		{
			const auto id = a_quest->GetFormID();
			if (a_apart == 0 || !locSlot || ClassOf(a_quest) != Class::kRadiant || follower.CountsOf(id).startedHere || !follower.Restart(id, a_now)) {
				return false;
			}
			if (Run(std::format("stopquest {}", EditorId(a_quest)))) {
				++counts.restartsMade;
				logs::info("Quests: {} ran here with {} location(s) other than the host's run {}: stopped, to start again with the host's", QuestName(id), a_apart,
					follower.RunOf(id));
			}
			return true;
		}

		// The run's references forced into the quest's aliases here where they do not hold. A reference not loaded here
		// (not persistent, its cell not loaded) is tried again FORCE_EVERY, said once per run and alias; a location that
		// differs (given only at a start) is said once per run and alias. Each forcing is said once per run, alias and
		// reference (a script that fills the alias again every time is not a line every 2 s).
		void ForceFills(RE::TESQuest* a_quest)
		{
			const auto id = a_quest->GetFormID();
			const auto fills = follower.Fills(id);
			if (!fills) {
				return;
			}
			const auto  compare = QuestRunRules::Against(*fills, ReadFills(a_quest));
			if (RestartForLocations(a_quest, compare.locs.size(), Clock::now())) {
				return;
			}
			std::size_t forcedNow = 0;
			std::size_t sayForced = 0;
			for (const auto& f : compare.force) {
				const auto ref = RefOf(f.refId);
				if (!ref) {
					if (follower.NotLoaded(id, f)) {
						logs::info("Quests: {} (the host's run {}): the host's reference {:08X} of its alias {} is not loaded here yet: forced once it is (said once)",
							QuestName(id), follower.RunOf(id), f.refId, f.aliasId);
					}
					continue;
				}
				a_quest->ForceRefIntoAlias(f.aliasId, ref);
				++forcedNow;
				sayForced += follower.Forced(id, f) ? 1 : 0;
			}
			counts.forced += forcedNow;
			for (const auto& l : compare.locs) {
				if (follower.LocApart(id, l)) {
					logs::warn("Quests: {} (the host's run {}): its location alias {} holds another location here than the host's {:08X}; a location is given only at "
							   "a start (said once)",
						QuestName(id), follower.RunOf(id), l.aliasId, l.refId);
				}
			}
			if (sayForced != 0) {
				logs::info("Quests: {} (the host's run {}): {} of its aliases given the host's reference here, {} the same already; {} the host's game made as it ran "
						   "and {} of plugins this game lacks stay this game's own",
					QuestName(id), follower.RunOf(id), forcedNow, compare.same, compare.runtime, compare.unknown);
			}
		}

		void Restore(Rewritten& a_rewritten)
		{
			for (auto& r : a_rewritten.rewrites) {
				r.alias->fillType = static_cast<RE::BGSBaseAlias::FILL_TYPE>(r.fillType);
				std::memcpy(reinterpret_cast<std::byte*>(r.alias) + r.offset, r.bytes.data(), r.size);
			}
			a_rewritten.rewrites.clear();
		}

		// The aliases' records of a quest started for a run put back (it runs, the start was given up).
		void RestoreRecords(RE::FormID a_quest)
		{
			for (auto it = rewritten.begin(); it != rewritten.end();) {
				if (it->quest == a_quest) {
					Restore(*it);
					it = rewritten.erase(it);
				} else {
					++it;
				}
			}
		}

		// Every record rewritten put back (before a load, a welcome, the session's end), and any older than its start can
		// take (a safety: a record is never left rewritten).
		void RestoreRecords(std::optional<Clock::time_point> a_olderThan = std::nullopt)
		{
			for (auto it = rewritten.begin(); it != rewritten.end();) {
				if (!a_olderThan || it->since < *a_olderThan) {
					Restore(*it);
					it = rewritten.erase(it);
				} else {
					++it;
				}
			}
		}

		// Records of quests that run now put back (the host's dev start at a place, and a friend's start for a run), and
		// any older than a start can take.
		void RestoreSettled(Clock::time_point a_now)
		{
			for (auto it = rewritten.begin(); it != rewritten.end();) {
				const auto quest = QuestOf(it->quest);
				if (!quest || Running(quest) || it->since < a_now - QuestRunRules::START_WAIT - std::chrono::seconds{ 5 }) {
					Restore(*it);
					it = rewritten.erase(it);
				} else {
					++it;
				}
			}
		}

		// The quest's aliases as their records have them (QuestRunRules::PlanStart).
		std::vector<QuestRunRules::AliasRecord> RecordsOf(RE::TESQuest* a_quest)
		{
			std::vector<QuestRunRules::AliasRecord> out;
			for (const auto alias : a_quest->aliases) {
				const bool loc = IsLocAlias(alias);
				if (!loc && !IsRefAlias(alias)) {
					continue;
				}
				out.push_back({ alias->aliasID, loc, FillTypeOf(alias), alias->flags.any(RE::BGSBaseAlias::FLAGS::kOptional) });
			}
			return out;
		}

		RE::BGSBaseAlias* AliasOf(RE::TESQuest* a_quest, std::uint32_t a_aliasId)
		{
			for (const auto alias : a_quest->aliases) {
				if (alias && alias->aliasID == a_aliasId) {
					return alias;
				}
			}
			return nullptr;
		}

		// One alias's record made a forced fill for a start (QuestRunRules::PlanStep): of a reference (its handle), of a
		// location (in the slot LocProbe found), or of none; as it was kept in a_done, to be put back.
		void RewriteAlias(Rewritten& a_done, RE::BGSBaseAlias* a_alias, const QuestRunRules::PlanStep& a_step)
		{
			static_assert(sizeof(RE::BGSRefAlias::GenericFillData) <= sizeof(Rewrite::bytes));
			Rewrite r;
			r.alias = a_alias;
			r.fillType = a_alias->fillType.underlying();
			if (a_step.location) {
				r.offset = LOC_SLOT_BASE + *locSlot * sizeof(std::uintptr_t);
				r.size = sizeof(std::uintptr_t);
			} else {
				const auto ref = static_cast<RE::BGSRefAlias*>(a_alias);
				r.offset = static_cast<std::size_t>(reinterpret_cast<std::byte*>(&ref->fillData) - reinterpret_cast<std::byte*>(a_alias));
				r.size = sizeof(ref->fillData);
			}
			const auto bytes = reinterpret_cast<std::byte*>(a_alias) + r.offset;
			std::memcpy(r.bytes.data(), bytes, r.size);
			a_done.rewrites.push_back(r);
			a_alias->fillType = RE::BGSBaseAlias::FILL_TYPE::kForced;
			if (a_step.location) {
				const auto loc = a_step.way == QuestRunRules::Way::kLoc ? LocOf(a_step.formId) : nullptr;
				const auto value = reinterpret_cast<std::uintptr_t>(loc);
				std::memcpy(bytes, &value, sizeof value);
			} else {
				const auto alias = static_cast<RE::BGSRefAlias*>(a_alias);
				std::memset(&alias->fillData, 0, sizeof(alias->fillData));
				if (a_step.way == QuestRunRules::Way::kRef) {
					if (const auto ref = RefOf(a_step.formId)) {
						alias->fillData.forced.forcedRef = ref->GetHandle();
					}
				}
			}
		}

		// Starts the quest here for the host's run (QuestRunRules::PlanStart): each alias the host's game filled with a
		// reference or a location this game has (loaded) is made a forced fill of it for the start (an optional one the
		// host left empty, a forced fill of none), so the quest's start-up stage and scripts run with the host's references
		// and locations, whatever filled them in the host's game (its conditions, the Story Manager's event, another alias);
		// its own pick stays in the rest (an actor the host's game created, a form not loaded here or of a plugin this game
		// lacks, a location while this game cannot write one). A plan that leaves an alias only the Story Manager's event
		// fills is not tried: the start would fail. The records are put back once the quest runs. The run is told how it
		// went (Follower::StartMade or StartFailed).
		void StartForRun(RE::TESQuest* a_quest, Clock::time_point a_now)
		{
			const auto id = a_quest->GetFormID();
			const auto plan = QuestRunRules::PlanStart(RecordsOf(a_quest), follower.Fills(id), locSlot.has_value(),
				[](std::uint32_t a_formId, bool a_location) { return a_location ? LocOf(a_formId) != nullptr : RefOf(a_formId) != nullptr; });
			lastRun = std::format("{} run {}", QuestName(id), follower.RunOf(id));
			if (!plan.CanStart()) {
				++counts.cannotStart;
				follower.StartFailed(id, a_now, false);
				std::string aliases;
				for (const auto alias : plan.needsEvent) {
					aliases += std::format("{}{}", aliases.empty() ? "" : ",", alias);
				}
				logs::warn("Quests: {}: the host's run {} cannot start here: its aliases {} are filled only by the Story Manager's event, and the host's {} not "
						   "given here ({} not loaded or lacking{}); tried again when the host's fills change, {}",
					QuestName(id), follower.RunOf(id), aliases, plan.needsEvent.size() == 1 ? "is" : "are", plan.note.notLoaded,
					locSlot ? "" : ", locations not written in this game", ClassOf(a_quest) == Class::kRadiant ? "its stages dropped meanwhile" : "its stages made as they come");
				return;
			}
			Rewritten done{ id, {}, a_now };
			for (const auto& step : plan.steps) {
				if (step.way == QuestRunRules::Way::kOwn) {
					continue;
				}
				if (const auto alias = AliasOf(a_quest, step.aliasId)) {
					RewriteAlias(done, alias, step);
				}
			}
			// a radiant job that ran here before starts afresh (the Story Manager resets one it starts again)
			if (ClassOf(a_quest) == Class::kRadiant && a_quest->alreadyRun && !a_quest->IsEnabled()) {
				a_quest->Reset();
			}
			bool       result = false;
			const bool started = a_quest->EnsureQuestStarted(result, true);
			NoteThread();
			const bool took = started || result;
			logs::info("Quests: started {} here for the host's run {} with {} of its aliases given the host's references, {} of its location aliases the host's "
					   "locations and {} made empty as the host's ({} this game's own pick, {} of them not loaded here): {}",
				QuestName(id), follower.RunOf(id), plan.note.refs, plan.note.locs, plan.note.nones, plan.note.own, plan.note.notLoaded,
				took ? "starting" : "the game would not start it (tried again later)");
			if (!took) {
				++counts.startFailed;
				Restore(done);
				follower.StartFailed(id, a_now, true);
				return;
			}
			counts.forcedAtStart += plan.note.refs;
			counts.locsAtStart += plan.note.locs;
			counts.ownAtStart += plan.note.own;
			follower.StartMade(id, plan.note);
			if (!done.rewrites.empty()) {
				rewritten.push_back(std::move(done));
			}
		}

		// The session's word on a quest its run came to run here late (after it was given up): what was dropped meanwhile
		// is made again (the maker judges each again; what this game has is passed over).
		void Rewant(std::uint32_t a_questId, Clock::time_point a_now)
		{
			std::size_t n = 0;
			for (const auto& e : view.All()) {
				if (e.questId == a_questId) {
					maker.Add(e, 0, a_now);
					++n;
				}
			}
			counts.rewanted += n;
			logs::info("Quests: {} runs here as the host's run {} after it was given up: the session's {} entries of it are made again as they apply",
				QuestName(a_questId), follower.RunOf(a_questId), n);
		}

		// The session's word on a quest of the host's runs is forgotten here (a radiant job begun again or stopped).
		void ForgetQuest(std::uint32_t a_questId)
		{
			view.Forget(a_questId);
			maker.Forget(a_questId);
			apart.Forget(a_questId);
			std::erase_if(heldStages, [&](const Entry& a_e) { return a_e.questId == a_questId; });
			std::erase_if(again, [&](const Again& a_a) { return a_a.entry.questId == a_questId; });
			std::erase_if(checks, [&](const Check& a_c) { return a_c.entry.questId == a_questId; });
		}

		void Catalog()
		{
			if (!catalog.empty()) {
				return;
			}
			const auto data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				return;
			}
			std::vector<QuestRules::BitSample> samples;
			for (const auto quest : data->GetFormArray<RE::TESQuest>()) {
				if (!quest) {
					continue;
				}
				const auto cls = QuestRules::Classify(FactsOf(quest));
				byId[quest->GetFormID()] = catalog.size();
				catalog.push_back({ quest, cls });
				++classCounts[static_cast<std::size_t>(cls)];
				if (quest->data.questType.underlying() != 0) {
					const auto stages = Stages(quest);
					QuestRules::BitSample sample;
					sample.running = quest->IsEnabled();
					sample.neverRun = !quest->IsEnabled() && !quest->IsCompleted() && !quest->alreadyRun && quest->GetCurrentStageID() == 0;
					sample.current = quest->GetCurrentStageID();
					sample.doneCount = static_cast<std::size_t>(std::ranges::count_if(stages, &StageRead::done));
					sample.currentDone = std::ranges::any_of(stages, [&](const StageRead& a_s) { return a_s.index == sample.current && a_s.done; });
					samples.push_back(sample);
				}
			}
			bit = QuestRules::JudgeBit(samples);
			logs::info("Quests: {} quests ({} shared, {} each player's own, {} radiant, {} misc, {} untyped, {} without an editor ID); "
					   "the stages' done bit is {} ({} of {} running quests read their current stage done, {} never run read a stage done)",
				catalog.size(), classCounts[0], classCounts[1], classCounts[2], classCounts[3], classCounts[4], classCounts[5],
				bit->trusted ? "trusted" : "NOT trusted: only stages seen set and current stages count as done", bit->currentDone, bit->currentChecked, bit->neverRunWithDone);
			ProbeLocations(data);
		}

		void Queue(const StageEvent& a_event)
		{
			std::lock_guard lock{ queueLock };
			if (queue.size() >= MAX_QUEUED) {
				++queueDropped;
				return;
			}
			queue.push_back(a_event);
		}

		std::vector<StageEvent> TakeQueue()
		{
			std::lock_guard lock{ queueLock };
			return std::exchange(queue, {});
		}

		class Sink : public RE::BSTEventSink<RE::TESQuestStageEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* a_event, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
			{
				if (a_event && a_event->formID != 0) {
					Queue({ a_event->formID, a_event->stage, StageEvent::kStage });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sink sink;
		// A quest's start or stop, in the same queue as its stages (the host's game says a run before the stages of it).
		class StartStopSink : public RE::BSTEventSink<RE::TESQuestStartStopEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStartStopEvent* a_event, RE::BSTEventSource<RE::TESQuestStartStopEvent>*) override
			{
				if (a_event && a_event->formID != 0) {
					Queue({ a_event->formID, 0, a_event->started ? StageEvent::kStart : StageEvent::kStop });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		StartStopSink startStopSink;
		bool installed = false;

		// The console commands are run from the tick, which until the frame hook runs on SKSE's task queue (as the doors'
		// writes are: WorldObjects.cpp): counted when off the main thread, said once.
		void NoteThread()
		{
			if (GameThread::IsLoadThread()) {
				return;
			}
			if (counts.offMain++ == 0) {
				logs::info("Quests: a console command run off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); "
						   "counted in `quests` as offmain");
			}
		}

		// False only when the game made no script to run it in (norun). A command the console could not compile is not
		// seen here: what is made is read again after CHECK_AFTER (unmade), and a stage that never reads done is stuck.
		bool Run(const std::string& a_command)
		{
			NoteThread();
			if (!Console::Run(a_command)) {
				++counts.noRun;
				return false;
			}
			return true;
		}

		QuestRules::Place PlaceOf(const RE::TESObjectREFR* a_ref)
		{
			QuestRules::Place place;
			const auto cell = a_ref ? a_ref->GetParentCell() : nullptr;
			if (!cell) {
				return place;
			}
			const auto world = a_ref->GetWorldspace();
			place.space = cell->IsInteriorCell() || !world ? cell->GetFormID() : world->GetFormID();
			const auto at = a_ref->GetPosition();
			place.x = at.x;
			place.y = at.y;
			place.z = at.z;
			return place;
		}

		bool Near(std::uint32_t a_playerId)
		{
			if (a_playerId == 0) {
				return false;
			}
			const auto standIn = RemotePlayers::StandInOf(a_playerId);
			const auto player = Player();
			if (!standIn || !player) {
				return false;
			}
			const auto a = PlaceOf(player);
			const auto b = PlaceOf(standIn);
			return a.space != 0 && a.space == b.space && std::hypot(a.x - b.x, a.y - b.y, a.z - b.z) <= QuestRules::NEAR_FRIEND;
		}

		// What this game reads of one thing the session says (QuestRules::Judge).
		QuestRules::Here Look(const Entry& a_entry, std::uint32_t a_reporter)
		{
			QuestRules::Here here;
			const auto quest = QuestOf(a_entry.questId);
			if (!quest) {
				return here;
			}
			here.exists = true;
			here.cls = ClassOf(quest);
			if (SharedHere(quest, here.cls)) {
				here.cls = Class::kShared;  // a radiant job this game runs as the session's run
			}
			here.hold = !host && follower.Hold(quest->GetFormID());
			here.started = Started(quest);
			here.current = Current(quest);
			switch (a_entry.kind) {
			case Entry::kStage:
				here.has = StageDone(quest, a_entry.index);
				here.rule = QuestRules::StageRuleOf(QuestRules::STAGE_RULES, EditorId(quest), a_entry.index);
				here.reporterNear = here.rule && Near(a_reporter);
				break;
			case Entry::kObjective:
				here.has = !QuestRules::ObjectiveToMake(a_entry, ObjectiveState(quest, a_entry.index), here.current).Any();
				break;
			case Entry::kCompleted:
				here.has = quest->IsCompleted();
				break;
			case Entry::kFailed:
				here.has = Failed(quest);
				break;
			default:
				break;
			}
			return here;
		}

		void CheckLater(const Entry& a_entry)
		{
			if (checks.size() < MAX_CHECKS) {
				checks.push_back({ a_entry, Clock::now() + CHECK_AFTER });
			}
		}

		// Makes one thing of the session's word here, by the game's own console command.
		void Make(const Entry& a_entry)
		{
			const auto quest = QuestOf(a_entry.questId);
			if (!quest) {
				return;
			}
			const auto name = EditorId(quest);
			switch (a_entry.kind) {
			case Entry::kStage:
				guard.Made(a_entry, PlaceOf(Player()), Clock::now());
				if (Run(std::format("setstage {} {}", name, a_entry.index))) {
					++counts.made;
					lastMade = std::format("{} {}", name, a_entry.index);
					logs::info("Quests: made {} stage {} here (the session's word)", name, a_entry.index);
				}
				break;
			case Entry::kObjective:
				{
					const auto ours = ObjectiveState(quest, a_entry.index);
					const auto make = QuestRules::ObjectiveToMake(a_entry, ours, Current(quest));
					bool       ran = false;
					if (make.show) {
						ran = Run(std::format("setobjectivedisplayed {} {} 1", name, a_entry.index)) || ran;
					}
					if (make.end == 1) {
						ran = Run(std::format("setobjectivecompleted {} {} 1", name, a_entry.index)) || ran;
					} else if (make.end == 2) {
						++counts.failedKept;  // no console command fails one objective: the quest's own stage does
					}
					if (ran) {
						++counts.made;
						lastMade = std::format("{} objective {}", name, a_entry.index);
						logs::info("Quests: made {} objective {} state {} here (the session's word, read at stage {}; it was {})", name, a_entry.index,
							QuestBook::MergeObjective(ours, a_entry.value), a_entry.stage, ours);
						CheckLater(a_entry);
					}
					break;
				}
			case Entry::kCompleted:
				if (Run(std::format("completequest {}", name))) {
					++counts.made;
					lastMade = std::format("{} completed", name);
					logs::info("Quests: completed {} here (the session's word)", name);
					CheckLater(a_entry);
				}
				break;
			case Entry::kFailed:
				++counts.failedKept;  // a failure is its stage's (a fail-quest stage): made as a stage, not on its own
				break;
			default:
				break;
			}
		}

		// The shared quests this game polls: those that run.
		template <class F>
		void EachRunning(F&& a_each)
		{
			for (const auto& known : catalog) {
				if (known.quest->IsEnabled() && SharedHere(known.quest, known.cls)) {
					a_each(known.quest);
				}
			}
		}

		QuestRules::QuestRead ReadOf(RE::TESQuest* a_quest)
		{
			QuestRules::QuestRead read;
			read.questId = a_quest->GetFormID();
			read.current = Current(a_quest);
			read.objectives = Objectives(a_quest);
			read.completed = a_quest->IsCompleted();
			read.failed = Failed(a_quest);
			return read;
		}

		// The host's world: every shared quest that runs, its done stages (or, with the done bit not trusted, its current
		// stage and the ones set while this game ran), objectives (with the stage each is read at) and end.
		std::vector<Entry> World()
		{
			std::vector<QuestRules::QuestRead> reads;
			EachRunning([&](RE::TESQuest* a_quest) {
				auto read = ReadOf(a_quest);
				if (bit && bit->trusted) {
					for (const auto& s : Stages(a_quest)) {
						if (s.done) {
							read.done.push_back(s.index);
						}
					}
				}
				if (read.current != 0) {
					read.done.push_back(read.current);
				}
				if (const auto it = seen.find(read.questId); it != seen.end()) {
					read.done.insert(read.done.end(), it->second.begin(), it->second.end());
				}
				reads.push_back(std::move(read));
			});
			return QuestRules::Snapshot(std::move(reads));
		}

		// What the poll reads now: the objectives and ends of the shared quests that run.
		std::vector<Entry> Polled()
		{
			std::vector<Entry> out;
			EachRunning([&](RE::TESQuest* a_quest) {
				const auto some = QuestRules::ObjectivesAndEnd(ReadOf(a_quest));
				out.insert(out.end(), some.begin(), some.end());
			});
			return out;
		}

		// This game's quests as they are now are its base: not reported, only what moves on from them (a welcome, the
		// host's reset, a load). A friend's quests further on than the session's are its own until the session gets there.
		void Rebase(Clock::time_point a_now)
		{
			const auto held = Polled();
			view.Rebase(held);
			std::vector<std::pair<std::uint32_t, std::uint16_t>> at;
			if (!host) {
				for (const auto& known : catalog) {
					if (SharedHere(known.quest, known.cls) && Started(known.quest) && Current(known.quest) != 0) {
						at.emplace_back(known.quest->GetFormID(), Current(known.quest));
					}
				}
			}
			apart.Rebase(at, a_now);
			++counts.rebases;
			logs::info("Quests: this game's quests as they are now are its own past ({} objectives and ends, {} quests started), not reported; "
					   "only what moves on from here is", held.size(), at.size());
		}

		// Whether to report what moved on in a quest here (QuestRules::Apart), said once per quest found apart.
		QuestRules::Apart::Say ApartSay(std::uint32_t a_questId, Clock::time_point a_now)
		{
			const auto before = apart.Quests();
			const auto stage = apart.StageOf(a_questId);
			const auto say = apart.Judge(a_questId, view, a_now);
			if (apart.Quests() != before) {
				logs::info("Quests: {} is further on here (stage {}) than in the session: what moves on in it here is this game's own until the session "
						   "reaches stage {}",
					QuestName(a_questId), stage, stage);
			}
			return say;
		}

		// A stage done here: reported, held for the session's word, or this game's own.
		void OfferStage(const Entry& a_entry, Clock::time_point a_now, std::vector<Entry>& a_news)
		{
			switch (ApartSay(a_entry.questId, a_now)) {
			case QuestRules::Apart::Say::kReport:
				if (view.Offer(a_entry)) {
					a_news.push_back(a_entry);
				}
				break;
			case QuestRules::Apart::Say::kWait:
				if (heldStages.size() < MAX_HELD) {
					heldStages.push_back(a_entry);
				}
				break;
			case QuestRules::Apart::Say::kOwn:
				++counts.apartStages;
				break;
			}
		}

		void Forget()
		{
			view.Clear();
			maker.Clear();
			guard.Clear();
			apart.Clear();
			again.clear();
			heldStages.clear();
			checks.clear();
			// What happened before is in the world as it is, not news; its stages were set here all the same (a stop ends
			// a run's stages: a radiant job started again sets them anew).
			for (const auto& event : TakeQueue()) {
				if (event.kind == StageEvent::kStage) {
					seen[event.quest].insert(event.stage);
				} else if (event.kind == StageEvent::kStop) {
					seen.erase(event.quest);
				}
			}
			// The runs: the host's game says its world's again (its snapshot), a friend's hears the session's again.
			keeper.Clear();
			follower.Clear();
			RestoreRecords();
		}

		void SendRun(const SendFn& a_send, const Protocol::QuestRun& a_run)
		{
			a_send(Protocol::Encode(a_run));
			++counts.runsSent;
		}

		// The host's game: a run of a_quest begun (its start, or its snapshot), said; a radiant job's earlier run's word
		// forgotten here as at the server, and this run's stages seen so far said again (the start event may come after
		// the start-up stage's), its objectives polled anew.
		void BeginRun(const SendFn& a_send, RE::TESQuest* a_quest, Class a_class, bool a_fresh, Clock::time_point a_now, std::vector<Entry>& a_news)
		{
			const auto id = a_quest->GetFormID();
			const auto run = keeper.Started(id, a_class == Class::kRadiant, a_fresh);
			SendRun(a_send, run);
			lastRun = std::format("{} run {}", QuestName(id), run.run);
			if (a_class == Class::kRadiant && a_fresh) {
				ForgetQuest(id);
				std::vector<std::uint16_t> stages;
				if (const auto it = seen.find(id); it != seen.end()) {
					stages.assign(it->second.begin(), it->second.end());
				}
				if (Current(a_quest) != 0) {
					stages.push_back(Current(a_quest));
				}
				std::ranges::sort(stages);
				const auto [first, last] = std::ranges::unique(stages);
				stages.erase(first, last);
				for (const auto s : stages) {
					OfferStage(Entry{ id, Entry::kStage, s, 0 }, a_now, a_news);
				}
			}
			if (Running(a_quest)) {
				if (auto filled = keeper.Read(id, ReadFills(a_quest), a_now)) {
					SendRun(a_send, *filled);
				}
			}
		}

		std::uint32_t Counted(std::uint64_t a_n) { return static_cast<std::uint32_t>((std::min<std::uint64_t>)(a_n, 0xFFFFFFFF)); }

		std::string Joined(const std::vector<std::string>& a_parts)
		{
			std::string out;
			for (const auto& p : a_parts) {
				out += out.empty() ? p : "," + p;
			}
			return out.empty() ? "-" : out;
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::warn("Quests: the game's event source is not there; quests are not shared");
			return false;
		}
		events->AddEventSink<RE::TESQuestStageEvent>(&sink);
		events->AddEventSink<RE::TESQuestStartStopEvent>(&startStopSink);
		installed = true;
		logs::info("Quests: watching quest stages (stage events), starts and stops (events), objectives, ends and aliases (polled)");
		return true;
	}

	void Welcomed(std::uint32_t a_playerId, bool a_host)
	{
		ourId = a_playerId;
		host = a_host;
		view.ClearAll();
		Forget();
		askAgain = false;
		sayWorld = a_host;
		rebase = true;
	}

	void Frame(const SendFn& a_send)
	{
		const auto now = Clock::now();
		if (!LocalPlayer::InWorld()) {
			return;  // a loading screen, the main menu: the events wait in the queue (up to MAX_QUEUED)
		}
		Catalog();
		if (std::exchange(rebase, false)) {
			Rebase(now);
		}
		if (std::exchange(askAgain, false)) {
			a_send(Protocol::Writer{ Protocol::MessageType::kAskQuests }.Data());
			++counts.asks;
			logs::info("Quests: asked the session for its quests again (a load)");
		}
		if (std::exchange(sayWorld, false)) {
			a_send(Protocol::Encode(Protocol::QuestReset{}, false));
			// the runs first (each shared quest and radiant job that runs, with its aliases' references), then the stages
			std::vector<Entry> none;
			std::size_t        runs = 0;
			for (const auto& known : catalog) {
				if ((known.cls == Class::kShared || known.cls == Class::kRadiant) && known.quest->IsEnabled() && !FormIds::IsRuntime(known.quest->GetFormID())) {
					BeginRun(a_send, known.quest, known.cls, false, now, none);
					++runs;
				}
			}
			const auto world = World();
			for (auto& packet : Protocol::Batches(world, Protocol::MessageType::kQuests)) {
				a_send(std::move(packet));
			}
			++counts.snapshots;
			counts.snapshotEntries += world.size();
			logs::info("Quests: said this world's quests to the session as the host's ({} runs, {} entries)", runs, world.size());
		}

		std::vector<Entry> news;
		for (const auto held = std::exchange(heldStages, {}); const auto& entry : held) {
			OfferStage(entry, now, news);
		}
		for (const auto& event : TakeQueue()) {
			++counts.events;
			const auto quest = QuestOf(event.quest);
			if (event.kind == StageEvent::kStart) {
				// the host's game: a shared quest or a radiant job begun here is a run of the session's
				if (host && quest && !FormIds::IsRuntime(event.quest)) {
					const auto cls = ClassOf(quest);
					if (cls == Class::kShared || cls == Class::kRadiant) {
						BeginRun(a_send, quest, cls, true, now, news);
						logs::info("Quests: {} started here: run {} of the session's{}", QuestName(event.quest), keeper.RunOf(event.quest),
							cls == Class::kRadiant ? " (a radiant job: every game does this one, with this game's targets)" : "");
					}
				}
				continue;
			}
			if (event.kind == StageEvent::kStop) {
				seen.erase(event.quest);  // a run's stages end with it: a radiant job started again sets them anew
				if (host) {
					if (const auto stop = keeper.Stopped(event.quest)) {
						SendRun(a_send, *stop);
						ForgetQuest(event.quest);
						logs::info("Quests: the radiant job {} stopped here: its run {} is over in every game", QuestName(event.quest), stop->run);
					}
				} else {
					follower.StoppedHere(event.quest);
				}
				continue;
			}
			seen[event.quest].insert(event.stage);
			maker.Done(event.quest, event.stage, now);
			if (quest && SharedHere(quest)) {
				OfferStage(Entry{ event.quest, Entry::kStage, event.stage, 0 }, now, news);
			}
		}

		// The runs: the host's game says its quests' aliases when first read and when they change; a friend's starts or
		// adopts the host's runs, forces their references, and stops a radiant job the host's stopped.
		CheckLocations(now);
		RestoreSettled(now);
		if (host) {
			if (now >= nextPoll) {
				for (const auto id : keeper.LiveQuests()) {
					const auto quest = QuestOf(id);
					if (quest && Running(quest)) {
						if (auto filled = keeper.Read(id, ReadFills(quest), now)) {
							SendRun(a_send, *filled);
						}
					}
				}
			}
		} else {
			for (const auto id : follower.Quests()) {
				const auto quest = QuestOf(id);
				switch (follower.Due(id, HereRun(quest), now)) {
				case QuestRunRules::Act::kStart:
					StartForRun(quest, now);
					break;
				case QuestRunRules::Act::kStarted:
					RestoreRecords(id);
					logs::info("Quests: {} runs here as the host's run {}", QuestName(id), follower.RunOf(id));
					if (follower.TakeLate(id)) {
						Rewant(id, now);
					}
					break;
				case QuestRunRules::Act::kAdopt:
					{
						lastRun = std::format("{} run {}", QuestName(id), follower.RunOf(id));
						logs::info("Quests: {} ran here already: it is the host's run {} now, with the host's references", QuestName(id), follower.RunOf(id));
						// a location is given only at a start: a radiant job that runs here with another location than the
						// host's is stopped and started again for the run (once a run); a shared quest's is said (ForceFills)
						const auto fills = follower.Fills(id);
						if (fills && RestartForLocations(quest, QuestRunRules::Against(*fills, ReadFills(quest)).locs.size(), now)) {
							break;
						}
						if (follower.TakeLate(id)) {
							Rewant(id, now);
						}
						break;
					}
				case QuestRunRules::Act::kGiveUp:
					RestoreRecords(id);
					logs::warn("Quests: {}: the host's run {} {} within {} s; {}", QuestName(id), follower.RunOf(id),
						follower.Fills(id) ? "did not start here" : "said no references",
						std::chrono::duration_cast<std::chrono::seconds>(QuestRunRules::START_WAIT).count(),
						quest && ClassOf(quest) == Class::kRadiant ? "the radiant job is not this game's: its stages are dropped" : "its stages are made here as they come");
					break;
				case QuestRunRules::Act::kStop:
					if (quest && Run(std::format("stopquest {}", EditorId(quest)))) {
						++counts.stopsMade;
						logs::info("Quests: stopped {} here (the host's run {} of the radiant job is over)", QuestName(id), follower.RunOf(id));
					}
					break;
				case QuestRunRules::Act::kNothing:
					break;
				}
				if (quest && Running(quest) && follower.ForceDue(id, now)) {
					ForceFills(quest);
				}
			}
		}
		if (now >= nextPoll) {
			nextPoll = now + POLL_EVERY;
			for (const auto& entry : Polled()) {
				if (ApartSay(entry.questId, now) == QuestRules::Apart::Say::kReport && view.Offer(entry)) {
					news.push_back(entry);
				}
			}
		}
		for (auto it = again.begin(); it != again.end();) {
			if (it->due <= now) {
				news.push_back(it->entry);
				it = again.erase(it);
			} else {
				++it;
			}
		}
		if (!news.empty()) {
			counts.sent += news.size();
			for (auto& packet : Protocol::Batches(news, Protocol::MessageType::kQuests)) {
				a_send(std::move(packet));
			}
		}

		// The session's word, made where this game is behind.
		for (int i = 0; i < STEPS_PER_FRAME; ++i) {
			const auto step = maker.Next(now, Look);
			if (!step) {
				break;
			}
			switch (step->verdict) {
			case QuestRules::Verdict::kMake:
				Make(step->want.entry);
				break;
			case QuestRules::Verdict::kHas:
				++counts.has;
				break;
			case QuestRules::Verdict::kMissing:
				++counts.missing;
				break;
			case QuestRules::Verdict::kNotHere:
				++counts.notHere;
				break;
			case QuestRules::Verdict::kOwn:
				++counts.own;
				break;
			case QuestRules::Verdict::kFarPast:
				++counts.farPast;
				break;
			case QuestRules::Verdict::kPassed:
				++counts.passed;
				break;
			case QuestRules::Verdict::kNotStarted:
				++counts.notStarted;
				break;
			case QuestRules::Verdict::kFar:
			case QuestRules::Verdict::kHold:
				break;
			}
		}
		// What was made, read again: the console runs a command it could not compile without a word.
		for (auto it = checks.begin(); it != checks.end();) {
			if (it->due > now) {
				++it;
				continue;
			}
			const auto here = Look(it->entry, 0);
			if (here.exists && !here.has) {
				lastUnmade = it->entry.kind == Entry::kObjective ? std::format("{} objective {}", QuestName(it->entry.questId), it->entry.index)
				                                                 : std::format("{} completed", QuestName(it->entry.questId));
				if (counts.unmade++ < 20) {
					logs::warn("Quests: made {} here, but the game does not read it so {} s later (the console could not compile the command, "
							   "or the game would not take it)",
						lastUnmade, std::chrono::duration_cast<std::chrono::seconds>(CHECK_AFTER).count());
				}
			}
			it = checks.erase(it);
		}
		if (const auto moved = guard.Check(PlaceOf(Player()), now)) {
			lastMoved = std::format("{} {}", QuestName(moved->questId), moved->index);
			counts.moved = guard.Moves();
			logs::warn("Quests: our player moved (another place, or {:.0f} u) within {} s of making {} stage {} here: the stage may move the player "
					   "(a row for QuestRules::STAGE_RULES), or our player took a load door at that moment",
				QuestRules::MOVE_FAR, QuestRules::MOVE_WATCH.count(), QuestName(moved->questId), moved->index);
		}
	}

	void Heard(const Protocol::QuestBatch& a_batch)
	{
		const auto now = Clock::now();
		const bool ours = a_batch.playerId != 0 && a_batch.playerId == ourId;
		for (const auto& entry : a_batch.entries) {
			if (entry.questId == 0) {
				continue;
			}
			if (!view.Heard(entry, ours)) {
				continue;
			}
			++counts.heard;
			if (ours) {
				continue;  // our own report, now the session's: this game has it
			}
			maker.Add(entry, a_batch.playerId, now);
		}
	}

	void Answered(const Protocol::QuestBatch& a_batch)
	{
		const auto now = Clock::now();
		for (const auto& entry : a_batch.entries) {
			const auto mine = view.Answered(entry);
			++counts.answered;
			if (entry.value == Entry::kBusy) {
				++counts.busy;
				// sent again as this game reads it then (an objective's state may have moved on); one of the host's
				// snapshot was never offered (not out), and goes again all the same
				auto resend = entry;
				resend.value = 0;
				resend.stage = 0;
				if (resend.kind == Entry::kObjective) {
					const auto quest = QuestOf(resend.questId);
					resend.value = quest ? ObjectiveState(quest, resend.index) : 0;
					resend.stage = quest ? Current(quest) : 0;
					if (resend.value == 0) {
						continue;
					}
				}
				if (!mine) {
					view.MarkOut(resend);
					again.push_back({ resend, now + QuestRules::AGAIN_AFTER });
				} else if (view.Offer(resend)) {
					again.push_back({ resend, now + QuestRules::AGAIN_AFTER });
				}
			} else if (entry.value == Entry::kRefused || entry.value == Entry::kFull) {
				if (counts.refused++ == 0) {
					logs::info("Quests: the session did not take our report on {} ({}); it stays this game's own and is not offered again", QuestName(entry.questId),
						entry.value == Entry::kFull ? "its book is full" : "refused");
				}
			}
		}
	}

	void HostReset(std::uint32_t a_hostId)
	{
		++counts.resets;
		view.Clear();
		maker.Clear();
		checks.clear();
		follower.Clear();  // the host's runs come again with its world
		RestoreRecords();
		rebase = true;  // what this game holds now is its own past: the host's world comes next
		logs::info("Quests: the host's world is new (player {} loaded a save or came in): its quests follow", a_hostId);
	}

	void HeardRun(const Protocol::QuestRun& a_run)
	{
		if (host || ourId == 0) {
			return;  // the host's game says the runs; it never takes one
		}
		const auto now = Clock::now();
		const auto quest = QuestOf(a_run.questId);
		const auto here = HereRun(quest);
		const bool forget = follower.Hear(a_run, here, now);
		if (forget) {
			ForgetQuest(a_run.questId);
		}
		++counts.runsHeard;
		if ((a_run.flags & Protocol::QuestRun::kStopped) != 0) {
			logs::info("Quests: the host's run {} of {} is over{}", a_run.run, QuestName(a_run.questId),
				here.radiant ? ": what the session said of it is forgotten here" : "");
		} else if ((a_run.flags & Protocol::QuestRun::kFilled) != 0) {
			logs::info("Quests: the host's run {} of {}: its aliases hold {}", a_run.run, QuestName(a_run.questId), QuestRunRules::Describe(a_run.fills));
		} else {
			logs::info("Quests: the host's game runs {} (run {}{}){}", QuestName(a_run.questId), a_run.run,
				(a_run.flags & Protocol::QuestRun::kFresh) != 0 ? ", begun afresh" : "", quest ? "" : "; this game lacks the quest");
		}
	}

	void BeforeLoad()
	{
		RestoreRecords();  // a record is never left rewritten into a load
	}

	void AfterLoad()
	{
		Forget();
		seen.clear();  // the loaded world's stages are its own
		if (ourId != 0) {
			view.Clear();
			rebase = true;
			if (host) {
				sayWorld = true;
			} else {
				askAgain = true;
			}
		}
	}

	void Clear()
	{
		ourId = 0;
		host = false;
		view.ClearAll();
		Forget();
		askAgain = false;
		sayWorld = false;
		rebase = false;
	}

	namespace
	{
		// A quest by editor ID or hex form ID.
		RE::TESQuest* FindQuest(std::string_view a_arg)
		{
			Catalog();
			for (const auto& known : catalog) {
				if (QuestRules::SameName(EditorId(known.quest), a_arg)) {
					return known.quest;
				}
			}
			if (const auto id = FormsCommand::ParseHex(a_arg)) {
				return QuestOf(*id);
			}
			return nullptr;
		}

		// `quests start <id> [here | at <location hex>]`: the quest started here by the engine's own start (the Story
		// Manager's way, as a radiant job starts). With a place, each of its location aliases the Story Manager's event
		// fills is made a forced fill of that location for the start (the player's location for `here`), as the event
		// would have filled it: a job that needs its event can be begun from the console (the rig's event leg). The
		// records are put back once it runs.
		std::string StartCommand(RE::TESQuest* a_quest, const std::vector<std::string_view>& a_words)
		{
			const auto id = a_quest->GetFormID();
			if (a_quest->IsEnabled()) {
				return std::format("error: {} runs here already", QuestName(id));
			}
			RE::BGSLocation* at = nullptr;
			Rewritten        done{ id, {}, Clock::now() };
			if (a_words.size() > 2) {
				if (!locSlot) {
					return "error: where a location alias's record keeps its location is not known in this game (quests: locSlot=none)";
				}
				if (a_words[2] == "here") {
					at = Player() ? Player()->GetCurrentLocation() : nullptr;
				} else if (const auto hex = FormsCommand::ParseHex(a_words[3])) {
					at = LocOf(*hex);
				}
				if (!at) {
					return a_words[2] == "here" ? "error: our player stands in no location" : "error: no such location (a hex form id of a location)";
				}
				for (const auto alias : a_quest->aliases) {
					if (IsLocAlias(alias) && FillTypeOf(alias) == QuestRunRules::FillType::kFromEvent) {
						RewriteAlias(done, alias, { alias->aliasID, true, QuestRunRules::Way::kLoc, at->GetFormID() });
					}
				}
				if (done.rewrites.empty()) {
					return std::format("error: {} has no location alias the Story Manager's event fills", QuestName(id));
				}
			}
			if (ClassOf(a_quest) == Class::kRadiant && a_quest->alreadyRun) {
				a_quest->Reset();
			}
			bool       result = false;
			const bool started = a_quest->EnsureQuestStarted(result, true);
			NoteThread();
			const auto given = done.rewrites.size();
			if (!started && !result) {
				Restore(done);
			} else if (!done.rewrites.empty()) {
				rewritten.push_back(std::move(done));
			}
			return std::format("quest={} id={:08X} started={} result={} at={} given={}", QuestName(id), id, started ? 1 : 0, result ? 1 : 0,
				at ? std::format("{:08X}", at->GetFormID()) : "-"s, given);
		}

		// `quests run <id>`: a quest's run as this game has it (the rig compares the two games' fills), and `quests start`.
		std::string RunCommand(const std::vector<std::string_view>& a_words)
		{
			const bool start = a_words[0] == "start";
			const bool shape = a_words.size() == 2 || (start && a_words.size() == 3 && a_words[2] == "here") || (start && a_words.size() == 4 && a_words[2] == "at");
			if (!shape) {
				return start ? "error: usage: quests start <editor id or hex form id> [here | at <location hex form id>]"s
				             : "error: usage: quests run <editor id or hex form id>"s;
			}
			if (!LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			const auto quest = FindQuest(a_words[1]);
			if (!quest) {
				return "error: no such quest here";
			}
			if (start) {
				return StartCommand(quest, a_words);
			}
			const auto  id = quest->GetFormID();
			std::size_t locations = 0;
			const auto  fills = ReadFills(quest, &locations);
			const auto  here = QuestRunRules::Describe(fills);
			const auto  cls = QuestRules::ClassName(ClassOf(quest));
			if (host) {
				const auto said = keeper.Fills(id);
				return std::format("quest={} id={:08X} class={} role=host run={} live={} fills={} refs={} locs={} here={} running={} locations={}", QuestName(id), id,
					cls, keeper.RunOf(id), keeper.Live(id) ? 1 : 0, said ? QuestRunRules::Describe(*said) : "-"s,
					said ? QuestRunRules::CountOf(*said, Fill::kRef) : 0, said ? QuestRunRules::CountOf(*said, Fill::kLoc) : 0, here, Running(quest) ? 1 : 0,
					locations);
			}
			const auto said = follower.Fills(id);
			const auto compare = said ? QuestRunRules::Against(*said, fills) : QuestRunRules::Compare{};
			const auto phase = follower.PhaseOf(id);
			const auto c = follower.CountsOf(id);
			return std::format("quest={} id={:08X} class={} role=friend run={} phase={} heard={} here={} running={} same={} locSame={} force={} locsApart={} "
							   "matched={} runtime={} unknown={} extra={} locations={} wasOurs={} startedHere={} atStart={} locAtStart={} noneAtStart={} "
							   "ownAtStart={} notLoadedAtStart={} missed={} later={} notLoaded={} locApart={} retries={} restarted={}",
				QuestName(id), id, cls, follower.RunOf(id), phase ? QuestRunRules::PhaseName(*phase) : "none", said ? QuestRunRules::Describe(*said) : "-"s, here,
				Running(quest) ? 1 : 0, said && compare.Same() ? 1 : 0, said && compare.LocsSame() ? 1 : 0, compare.force.size(), compare.locs.size(), compare.same,
				compare.runtime, compare.unknown, compare.extra, locations, follower.WasOurs(id) ? 1 : 0, c.startedHere ? 1 : 0, c.start.refs, c.start.locs,
				c.start.nones, c.start.own, c.start.notLoaded, c.missed, c.later, c.notLoaded, c.locApart, c.retries, c.restarted ? 1 : 0);
		}

		// `quests runpick <radiant|shared> [n] [plain|loc|event]`: the n-th quest of the class (from 0) with an editor ID
		// that does not run here and has not ended, for the rig's legs:
		// - plain: a reference alias the game fills by its conditions, and no alias (reference or location) that needs the
		//   Story Manager's event (a quest a console start can begin);
		// - loc: a location alias the game fills by its conditions (find a matching location: two games may pick apart),
		//   and none that needs the event;
		// - event: a location alias that needs the event (begun with `quests start <id> here`), no reference alias that
		//   does.
		std::string RunPick(const std::vector<std::string_view>& a_words)
		{
			const auto usage = "error: usage: quests runpick <radiant|shared> [n 0-4096] [plain|loc|event]";
			if (a_words.size() < 2 || a_words.size() > 4 || (a_words[1] != "radiant" && a_words[1] != "shared")) {
				return usage;
			}
			const auto skip = a_words.size() >= 3 ? DevLine::Number(a_words[2]) : std::optional<std::uint64_t>{ 0 };
			const auto mode = a_words.size() == 4 ? a_words[3] : std::string_view{ "plain" };
			if (!skip || *skip > 4096 || (mode != "plain" && mode != "loc" && mode != "event")) {
				return usage;
			}
			if (!LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			Catalog();
			const auto    want = a_words[1] == "radiant" ? Class::kRadiant : Class::kShared;
			std::uint64_t n = 0;
			for (const auto& known : catalog) {
				const auto quest = known.quest;
				if (known.cls != want || quest->IsEnabled() || quest->IsCompleted() || Failed(quest) || FormIds::IsRuntime(quest->GetFormID()) ||
					EditorId(quest).empty()) {
					continue;
				}
				std::size_t refs = 0, byConditions = 0, refEvent = 0, locs = 0, locByConditions = 0, locEvent = 0;
				for (const auto& alias : RecordsOf(quest)) {
					const bool byEvent = alias.fill == QuestRunRules::FillType::kFromEvent && !alias.optional;
					const bool byConds = alias.fill == QuestRunRules::FillType::kConditions;
					if (alias.location) {
						++locs;
						locByConditions += byConds ? 1 : 0;
						locEvent += byEvent ? 1 : 0;
					} else {
						++refs;
						byConditions += byConds ? 1 : 0;
						refEvent += byEvent ? 1 : 0;
					}
				}
				const bool fits = mode == "plain" ? byConditions != 0 && refEvent == 0 && locEvent == 0 :
				                  mode == "loc"   ? locByConditions != 0 && refEvent == 0 && locEvent == 0 :
				                                    locEvent != 0 && refEvent == 0;
				if (!fits || n++ < *skip) {
					continue;
				}
				return std::format("quest={} id={:08X} class={} refs={} byConditions={} locs={} locByConditions={} locEvent={}", EditorId(quest), quest->GetFormID(),
					QuestRules::ClassName(known.cls), refs, byConditions, locs, locByConditions, locEvent);
			}
			return std::format("error: no {}-th {} quest ({}) that a console start can begin here", *skip, a_words[1], mode);
		}
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.empty()) {
			std::uint64_t dropped = 0;
			{
				std::lock_guard lock{ queueLock };
				dropped = queueDropped;
			}
			const auto look = [](const Entry& a_e, std::uint32_t a_r) { return Look(a_e, a_r); };
			return std::format("installed={} host={} quests={} shared={} own={} radiant={} misc={} untyped={} noname={} doneBit={} session={}/{} outstanding={} "
							   "based={} kept={} wants={} inflight={} held={} events={} sent={} heard={} made={} has={} missing={} nothere={} ownStages={} farPast={} "
							   "passed={} notStarted={} failedKept={} answered={} busy={} refused={} stuck={} lastStuck={} asks={} resets={} rebases={} snapshots={}/{} "
							   "apart={} lastApart={} apartStages={} apartHeld={} moved={} lastMoved={} lastMade={} unmade={} lastUnmade={} wantsDropped={} "
							   "eventsDropped={} offmain={} norun={} runs={} runsSent={} runsHeard={} runsStarted={} runsAdopted={} runsGaveUp={} forced={} "
							   "forcedAtStart={} locsAtStart={} ownAtStart={} startFailed={} cannotStart={} runsRetried={} stopsMade={} restarts={} rewanted={} "
							   "notLoaded={} locApart={} rewritten={} locSlot={} locMap={} locChecked={}/{} locUnread={} lastRun={}",
				installed ? 1 : 0, host ? 1 : 0, catalog.size(), classCounts[0], classCounts[1], classCounts[2], classCounts[3], classCounts[4], classCounts[5],
				!bit ? "unread" : bit->trusted ? "trusted" : "untrusted", view.Quests(), view.Entries(), view.Outstanding(), view.Based(), view.Kept(), maker.Size(),
				maker.InFlight(), maker.Held(look), Counted(counts.events), Counted(counts.sent), Counted(counts.heard), Counted(counts.made), Counted(counts.has),
				Counted(counts.missing), Counted(counts.notHere), Counted(counts.own), Counted(counts.farPast), Counted(counts.passed), Counted(counts.notStarted),
				Counted(counts.failedKept), Counted(counts.answered), Counted(counts.busy), Counted(counts.refused), Counted(maker.Stuck()),
				maker.LastStuck() ? std::format("{:08X}:{}", maker.LastStuck()->questId, maker.LastStuck()->index) : "none"s, Counted(counts.asks),
				Counted(counts.resets), Counted(counts.rebases), Counted(counts.snapshots), Counted(counts.snapshotEntries), Counted(apart.Quests()),
				apart.Quests() ? QuestName(apart.LastQuest()) : "none"s, Counted(counts.apartStages), heldStages.size(), Counted(guard.Moves()), lastMoved, lastMade,
				Counted(counts.unmade), lastUnmade, Counted(maker.Dropped()), Counted(dropped), Counted(counts.offMain), Counted(counts.noRun),
				host ? keeper.LiveQuests().size() : follower.Live(), Counted(counts.runsSent), Counted(counts.runsHeard), Counted(follower.Started()),
				Counted(follower.Adopted()), Counted(follower.GaveUp()), Counted(counts.forced), Counted(counts.forcedAtStart), Counted(counts.locsAtStart),
				Counted(counts.ownAtStart), Counted(counts.startFailed), Counted(counts.cannotStart), Counted(follower.Retried()), Counted(counts.stopsMade),
				Counted(counts.restartsMade), Counted(counts.rewanted), Counted(follower.NotLoadedCount()), Counted(follower.LocApartCount()), rewritten.size(),
				locSlot ? std::format("+0x{:X}", LOC_SLOT_BASE + *locSlot * sizeof(std::uintptr_t)) : "none"s, QuestRunRules::LocStateName(locCheck.Now()),
				locCheck.Held(), locCheck.Samples(), Counted(counts.locUnread), lastRun);
		}
		if (words[0] == "run" || words[0] == "start") {
			return RunCommand(words);
		}
		if (words[0] == "runpick") {
			return RunPick(words);
		}
		if (words[0] != "pick" || words.size() > 2) {
			return "error: usage: quests [pick [n] | runpick <radiant|shared> [n] [plain|loc|event] | run <id> | start <id> [here | at <location>]]";
		}
		// The n-th shared quest (from 0) that has not started here and has at least six stages past its start-up stage,
		// for the rig to step through (it sets four or five of them, and passes over any its fragments set themselves):
		// its editor ID, its form ID and those stages.
		const auto skip = words.size() == 2 ? DevLine::Number(words[1]) : std::optional<std::uint64_t>{ 0 };
		if (!skip || *skip > 4096) {
			return "error: usage: quests pick [n 0-4096]";
		}
		if (!LocalPlayer::InWorld()) {
			return "error: not in the world";
		}
		Catalog();
		std::uint64_t n = 0;
		for (const auto& known : catalog) {
			const auto quest = known.quest;
			if (known.cls != Class::kShared || quest->IsEnabled() || quest->IsCompleted() || quest->alreadyRun || quest->GetCurrentStageID() != 0 ||
				FormIds::IsRuntime(quest->GetFormID())) {
				continue;
			}
			std::vector<std::string> steps;
			for (const auto& s : Stages(quest)) {
				if (s.index != 0 && !s.startUp && !s.shutDown && !s.done) {
					steps.push_back(std::to_string(s.index));
				}
			}
			if (steps.size() < 6 || n++ < *skip) {
				continue;
			}
			return std::format("quest={} id={:08X} type={} stages={}", EditorId(quest), quest->GetFormID(), quest->data.questType.underlying(), Joined(steps));
		}
		return std::format("error: no {}-th shared quest not started here with six stages", *skip);
	}

	std::string One(std::string_view a_args)
	{
		std::string_view arg = a_args;
		while (!arg.empty() && arg.front() == ' ') {
			arg.remove_prefix(1);
		}
		while (!arg.empty() && arg.back() == ' ') {
			arg.remove_suffix(1);
		}
		if (arg.empty()) {
			return "error: usage: quest <editor id or hex form id>";
		}
		Catalog();
		RE::TESQuest* quest = nullptr;
		for (const auto& known : catalog) {
			if (QuestRules::SameName(EditorId(known.quest), arg)) {
				quest = known.quest;
				break;
			}
		}
		if (!quest) {
			if (const auto id = FormsCommand::ParseHex(arg)) {
				quest = QuestOf(*id);
			}
		}
		if (!quest) {
			return "error: no such quest here";
		}
		const auto id = quest->GetFormID();
		std::vector<std::string> all;
		std::vector<std::string> done;
		for (const auto& s : Stages(quest)) {
			all.push_back(std::to_string(s.index));
			if (s.done) {
				done.push_back(std::to_string(s.index));
			}
		}
		std::vector<std::string> seenHere;
		if (const auto it = seen.find(id); it != seen.end()) {
			std::vector<std::uint16_t> sorted(it->second.begin(), it->second.end());
			std::ranges::sort(sorted);
			for (const auto s : sorted) {
				seenHere.push_back(std::to_string(s));
			}
		}
		std::vector<std::string> objectives;
		for (const auto& [index, state] : Objectives(quest)) {
			objectives.push_back(std::format("{}:{}", index, state));
		}
		std::vector<std::string> session;
		std::vector<std::string> sessionObjectives;
		std::string              sessionEnd = "-";
		for (const auto& e : view.All()) {
			if (e.questId != id) {
				continue;
			}
			switch (e.kind) {
			case Entry::kStage:
				session.push_back(std::to_string(e.index));
				break;
			case Entry::kObjective:
				sessionObjectives.push_back(std::format("{}:{}@{}", e.index, e.value, e.stage));
				break;
			case Entry::kCompleted:
				sessionEnd = "completed";
				break;
			case Entry::kFailed:
				sessionEnd = "failed";
				break;
			default:
				break;
			}
		}
		std::ranges::sort(session, {}, [](const std::string& a_s) { return std::stoi(a_s); });
		return std::format("quest={} id={:08X} type={} class={} enabled={} completed={} failed={} current={} stages={} done={} seen={} objectives={} "
						   "session={} sessionObjectives={} sessionEnd={} apart={}",
			EditorId(quest).empty() ? "-" : EditorId(quest), id, quest->data.questType.underlying(), QuestRules::ClassName(ClassOf(quest)),
			quest->IsEnabled() ? 1 : 0, quest->IsCompleted() ? 1 : 0, Failed(quest) ? 1 : 0, quest->GetCurrentStageID(), Joined(all), Joined(done),
			Joined(seenHere), Joined(objectives), Joined(session), Joined(sessionObjectives), sessionEnd, apart.StageOf(id));
	}
}
