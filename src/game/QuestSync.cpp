#include "game/QuestSync.h"

#include "DevLine.h"
#include "game/Console.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/QuestRules.h"
#include "game/RemotePlayers.h"
#include "net/FormsCommand.h"

namespace QuestSync
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Entry = Protocol::QuestEntry;
		using QuestRules::Class;

		// A stage event the sink queued, read on the next frame.
		struct StageEvent
		{
			RE::FormID    quest = 0;
			std::uint16_t stage = 0;
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
		};
		Counts      counts;
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
					Queue({ a_event->formID, a_event->stage });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sink sink;
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
				if (known.cls == Class::kShared && known.quest->IsEnabled()) {
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
					if (known.cls == Class::kShared && Started(known.quest) && Current(known.quest) != 0) {
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
			// What happened before is in the world as it is, not news; its stages were set here all the same.
			for (const auto& event : TakeQueue()) {
				seen[event.quest].insert(event.stage);
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
		installed = true;
		logs::info("Quests: watching quest stages (stage events), objectives and ends (polled)");
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
			const auto world = World();
			a_send(Protocol::Encode(Protocol::QuestReset{}, false));
			for (auto& packet : Protocol::Batches(world, Protocol::MessageType::kQuests)) {
				a_send(std::move(packet));
			}
			++counts.snapshots;
			counts.snapshotEntries += world.size();
			logs::info("Quests: said this world's quests to the session as the host's ({} entries)", world.size());
		}

		std::vector<Entry> news;
		for (const auto held = std::exchange(heldStages, {}); const auto& entry : held) {
			OfferStage(entry, now, news);
		}
		for (const auto& event : TakeQueue()) {
			++counts.events;
			seen[event.quest].insert(event.stage);
			maker.Done(event.quest, event.stage, now);
			const auto quest = QuestOf(event.quest);
			if (quest && ClassOf(quest) == Class::kShared) {
				OfferStage(Entry{ event.quest, Entry::kStage, event.stage, 0 }, now, news);
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
		rebase = true;  // what this game holds now is its own past: the host's world comes next
		logs::info("Quests: the host's world is new (player {} loaded a save or came in): its quests follow", a_hostId);
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
							   "eventsDropped={} offmain={} norun={}",
				installed ? 1 : 0, host ? 1 : 0, catalog.size(), classCounts[0], classCounts[1], classCounts[2], classCounts[3], classCounts[4], classCounts[5],
				!bit ? "unread" : bit->trusted ? "trusted" : "untrusted", view.Quests(), view.Entries(), view.Outstanding(), view.Based(), view.Kept(), maker.Size(),
				maker.InFlight(), maker.Held(look), Counted(counts.events), Counted(counts.sent), Counted(counts.heard), Counted(counts.made), Counted(counts.has),
				Counted(counts.missing), Counted(counts.notHere), Counted(counts.own), Counted(counts.farPast), Counted(counts.passed), Counted(counts.notStarted),
				Counted(counts.failedKept), Counted(counts.answered), Counted(counts.busy), Counted(counts.refused), Counted(maker.Stuck()),
				maker.LastStuck() ? std::format("{:08X}:{}", maker.LastStuck()->questId, maker.LastStuck()->index) : "none"s, Counted(counts.asks),
				Counted(counts.resets), Counted(counts.rebases), Counted(counts.snapshots), Counted(counts.snapshotEntries), Counted(apart.Quests()),
				apart.Quests() ? QuestName(apart.LastQuest()) : "none"s, Counted(counts.apartStages), heldStages.size(), Counted(guard.Moves()), lastMoved, lastMade,
				Counted(counts.unmade), lastUnmade, Counted(maker.Dropped()), Counted(dropped), Counted(counts.offMain), Counted(counts.noRun));
		}
		if (words[0] != "pick" || words.size() > 2) {
			return "error: usage: quests [pick [n]]";
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
