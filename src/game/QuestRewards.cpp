#include "game/QuestRewards.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/Console.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/LootSync.h"
#include "game/QuestSync.h"
#include "game/RewardRules.h"

namespace QuestRewards
{
	namespace
	{
		using Clock = RewardRules::Clock;
		using namespace std::chrono_literals;

		constexpr RE::FormID PLAYER_REF = 0x14;
		constexpr RE::FormID GOLD = 0xF;  // Gold001

		// An item that came to our player from nothing (no container, no reference picked up: a script's AddItem), as the
		// sink saw it; or gold that left our player (paid: to nothing, or to a container or someone, `to`).
		struct Got
		{
			RE::FormID        item = 0;
			std::uint32_t     count = 0;
			Clock::time_point at;
			bool              paid = false;
			RE::FormID        to = 0;
		};

		// A topic info the game said: who said it, which, when.
		struct Said
		{
			RE::FormID        speaker = 0;
			RE::FormID        info = 0;
			Clock::time_point at;
		};

		std::mutex            queueLock;
		std::vector<Got>      gots;   // under queueLock
		std::vector<Said>     saids;  // under queueLock
		std::vector<Clock::time_point> stages;  // under queueLock: a stage of any quest made in this game
		constexpr std::size_t MAX_QUEUED = 1024;  // past this a new event is dropped and counted
		std::uint64_t         queueDropped = 0;   // under queueLock

		// Everything below: taken by every entry point (the tick, and SKSE's messages on the main thread), never while
		// sending. The sinks never take it.
		std::mutex stateLock;

		std::uint32_t                  ourId = 0;
		bool                           installed = false;
		RewardRules::Talk              talk;
		RE::FormID                     partner = 0;  // whom our player talks with (the Dialogue Menu's speaker), last seen
		RewardRules::Outbox            outbox;
		RewardRules::Ledger            ledger;
		std::vector<RewardRules::Heard> waiting;  // rewards heard, not given yet
		// Our own gives (`player.additem`), whose items come from nothing too: matched and left out of the judging and the
		// ledger, so a reward given here is never said again, nor taken for what a stage gave.
		struct Expect
		{
			RE::FormID        item = 0;
			std::uint32_t     count = 0;
			Clock::time_point at;
		};
		std::vector<Expect>   expected;
		constexpr auto        EXPECT_FOR = 10s;
		constexpr std::size_t MAX_EXPECTED = 64;
		constexpr int         GIVES_PER_FRAME = 4;

		struct Counts
		{
			std::uint64_t got = 0;          // items from nothing seen
			std::uint64_t own = 0;          // ... our own gives
			std::uint64_t infos = 0;        // topic infos of our talk read
			std::uint64_t sent = 0;         // rewards said
			std::uint64_t outboxFull = 0;   // ... dropped (MAX_OUTBOX)
			std::uint64_t heard = 0;        // other players' rewards heard
			std::uint64_t beforeReset = 0;  // ... while the host's game waited for its reset: dropped (the old world's)
			std::uint64_t waitFull = 0;     // ... dropped (MAX_WAITING)
			std::uint64_t given = 0;        // ... given here (a console command run)
			std::uint64_t covered = 0;      // ... our player had all of already (from the stage)
			std::uint64_t had = 0;          // items of rewards our player had already (left out of the give)
			std::uint64_t notShared = 0;    // ... of a quest not here or not shared here
			std::uint64_t notHere = 0;      // ... of a quest that never started here
			std::uint64_t stale = 0;        // ... whose stages never came
			std::uint64_t badItem = 0;      // ... an item this game lacks or a reward does not share
			std::uint64_t noRun = 0;        // ... the console made no script to run it in
			std::uint64_t offMain = 0;      // gives made off the game's main thread
			std::uint64_t paid = 0;         // gold our player paid in a talk (for nothing or to whom it talks with)
			std::uint64_t stages = 0;       // stages made in this game seen
			std::uint64_t oldWorld = 0;     // rewards heard dropped: the host loaded, or its world is new (a reset)
			std::array<std::uint64_t, RewardRules::WHY_COUNT> why{};  // items from nothing, by what they were judged
		};
		Counts counts;

		std::uint32_t Counted(std::uint64_t a_n)
		{
			return static_cast<std::uint32_t>((std::min<std::uint64_t>)(a_n, 0xFFFFFFFF));
		}

		void PushGot(const Got& a_got)
		{
			std::lock_guard guard{ queueLock };
			if (gots.size() < MAX_QUEUED) {
				gots.push_back(a_got);
			} else {
				++queueDropped;
			}
		}

		void PushSaid(const Said& a_said)
		{
			std::lock_guard guard{ queueLock };
			if (saids.size() < MAX_QUEUED) {
				saids.push_back(a_said);
			} else {
				++queueDropped;
			}
		}

		class ItemSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				// Only an item to our player from nothing: no container gave it and no reference in the world was taken.
				if (a_event && a_event->itemCount > 0 && a_event->baseObj != 0 && a_event->newContainer == PLAYER_REF && a_event->oldContainer == 0 &&
					a_event->reference.native_handle() == 0) {
					PushGot({ a_event->baseObj, static_cast<std::uint32_t>(a_event->itemCount), Clock::now() });
				} else if (a_event && a_event->itemCount > 0 && a_event->baseObj == GOLD && a_event->oldContainer == PLAYER_REF) {
					// gold our player paid: what comes from nothing near it in a talk is bought (RewardRules::Why::kBought)
					PushGot({ GOLD, static_cast<std::uint32_t>(a_event->itemCount), Clock::now(), true, a_event->newContainer });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		ItemSink itemSink;

		// A stage of any quest made in this game: by the game (our player's step, a conversation's line) or from the
		// session's word. What comes from nothing after it in a talk is its fragment's, every game's own.
		class StageSink : public RE::BSTEventSink<RE::TESQuestStageEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* a_event, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
			{
				if (a_event && a_event->formID != 0) {
					std::lock_guard guard{ queueLock };
					if (stages.size() < MAX_QUEUED) {
						stages.push_back(Clock::now());
					} else {
						++queueDropped;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		StageSink stageSink;

		class InfoSink : public RE::BSTEventSink<RE::TESTopicInfoEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESTopicInfoEvent* a_event, RE::BSTEventSource<RE::TESTopicInfoEvent>*) override
			{
				if (a_event && a_event->speakerRef && a_event->topicInfoFormID != 0) {
					PushSaid({ a_event->speakerRef->GetFormID(), a_event->topicInfoFormID, Clock::now() });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		InfoSink infoSink;

		void Forget()
		{
			{
				std::lock_guard guard{ queueLock };
				gots.clear();
				saids.clear();
				stages.clear();
			}
			talk = {};
			partner = 0;
			outbox.Clear();
			ledger.Clear();
			expected.clear();
		}

		// The rewards heard and not given yet are of a world the host left: dropped, never given.
		void DropWaiting(std::string_view a_why)
		{
			if (waiting.empty()) {
				return;
			}
			counts.oldWorld += waiting.size();
			logs::info("Rewards: {} reward(s) heard and not given yet dropped ({}): their conversations were of the world before", waiting.size(), a_why);
			waiting.clear();
		}

		// The quest a topic info belongs to (its topic's), when the session shares it here; 0 otherwise.
		std::uint32_t QuestOfInfo(RE::FormID a_info)
		{
			const auto info = RE::TESForm::LookupByID<RE::TESTopicInfo>(a_info);
			const auto topic = info ? info->parentTopic : nullptr;
			const auto quest = topic ? topic->ownerQuest : nullptr;
			if (!quest) {
				return 0;
			}
			const auto id = quest->GetFormID();
			return QuestSync::SharesQuest(id) ? id : 0;
		}

		// The menus our player has open, read once a tick: the Dialogue Menu (and whom it is with) and the trading ones.
		void ReadMenus(Clock::time_point a_now)
		{
			const auto ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			const auto topics = RE::MenuTopicManager::GetSingleton();
			if (topics && topics->menuOpen && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
				talk.Open(a_now);
				const auto speaker = topics->speaker.get();
				partner = speaker ? speaker->GetFormID() : 0;
			}
			if (ui->IsMenuOpen(RE::BarterMenu::MENU_NAME) || ui->IsMenuOpen(RE::GiftMenu::MENU_NAME) || ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) ||
				ui->IsMenuOpen(RE::TrainingMenu::MENU_NAME)) {
				talk.trade = a_now;
			}
		}

		// Our give's own item, back from the sink: true (used up) when it is one.
		bool OwnGive(const Got& a_got)
		{
			for (auto it = expected.begin(); it != expected.end(); ++it) {
				if (it->item == a_got.item && a_got.count <= it->count) {
					it->count -= a_got.count;
					if (it->count == 0) {
						expected.erase(it);
					}
					return true;
				}
			}
			return false;
		}

		void Judge(const Got& a_got)
		{
			if (a_got.paid) {
				if (a_got.to == 0 || a_got.to == partner) {
					talk.paid.Add(a_got.at);  // for nothing (a script's RemoveItem) or to whom we talk with
					++counts.paid;
				}
				return;
			}
			++counts.got;
			if (OwnGive(a_got)) {
				++counts.own;
				return;
			}
			ledger.Add(a_got.item, a_got.count, a_got.at);
			const auto form = RE::TESForm::LookupByID(a_got.item);
			const auto kind = LootSync::KindOf(form);
			const auto why = (form && FormIds::IsRuntime(a_got.item)) ? RewardRules::Why::kKind : RewardRules::Judge(talk, a_got.at, kind);
			++counts.why[static_cast<std::size_t>(why)];
			if (why != RewardRules::Why::kReward) {
				return;
			}
			if (!outbox.Add({ talk.infoQuest, a_got.item, a_got.count }, a_got.at)) {
				++counts.outboxFull;
			}
		}

		// Gives a reward heard: false when it could not be made (counted).
		bool Give(const RewardRules::Heard& a_heard, std::uint32_t a_count)
		{
			const auto form = RE::TESForm::LookupByID(a_heard.reward.item);
			if (!form || FormIds::IsRuntime(a_heard.reward.item) || !RewardRules::KindShared(LootSync::KindOf(form))) {
				++counts.badItem;
				return false;
			}
			if (!GameThread::IsLoadThread() && counts.offMain++ == 0) {
				logs::info("Rewards: a give made off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); counted in `rewards` as offmain");
			}
			if (expected.size() >= MAX_EXPECTED) {
				expected.erase(expected.begin());
			}
			expected.push_back({ a_heard.reward.item, a_count, Clock::now() });
			if (!Console::Run(std::format("player.additem {:08X} {}", a_heard.reward.item, a_count))) {
				expected.pop_back();
				++counts.noRun;
				return false;
			}
			return true;
		}

		void GiveWaiting(Clock::time_point a_now)
		{
			int        made = 0;
			const bool weTalk = RewardRules::Talking(talk, a_now);
			std::erase_if(waiting, [&](const RewardRules::Heard& a_heard) {
				if (made >= GIVES_PER_FRAME) {
					return false;
				}
				const auto quest = QuestSync::QuestHere(a_heard.reward.quest);
				const auto decision = RewardRules::Judge(a_heard, quest, ledger, a_now, weTalk);
				switch (decision.verdict) {
				case RewardRules::Verdict::kWait:
					return false;
				case RewardRules::Verdict::kNotShared:
					++counts.notShared;
					return true;
				case RewardRules::Verdict::kNotHere:
					++counts.notHere;
					logs::info("Rewards: player {}'s reward for quest {:08X} ({} of {:08X}) not given: the quest never started here", a_heard.from,
						a_heard.reward.quest, a_heard.reward.count, a_heard.reward.item);
					return true;
				case RewardRules::Verdict::kStale:
					++counts.stale;
					logs::info("Rewards: player {}'s reward for quest {:08X} ({} of {:08X}) dropped: its stages never came here", a_heard.from, a_heard.reward.quest,
						a_heard.reward.count, a_heard.reward.item);
					return true;
				case RewardRules::Verdict::kGive:
					counts.had += decision.had;
					if (decision.give == 0) {
						++counts.covered;
						return true;
					}
					++made;
					if (Give(a_heard, decision.give)) {
						++counts.given;
						logs::info("Rewards: gave player {}'s reward for quest {:08X}: {} of {:08X} ({} had from the quest's stage already)", a_heard.from,
							a_heard.reward.quest, decision.give, a_heard.reward.item, decision.had);
					}
					return true;
				}
				return true;
			});
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
			logs::warn("Rewards: the game's event source is not there; quest rewards are each player's own");
			return false;
		}
		events->AddEventSink<RE::TESContainerChangedEvent>(&itemSink);
		events->AddEventSink<RE::TESTopicInfoEvent>(&infoSink);
		events->AddEventSink<RE::TESQuestStageEvent>(&stageSink);
		installed = true;
		logs::info("Rewards: watching our player's items from nothing, the gold it pays, the topic infos said and the stages made");
		return true;
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		std::lock_guard guard{ stateLock };
		ourId = a_playerId;
		Forget();
		waiting.clear();
	}

	void Frame(const SendFn& a_send)
	{
		std::vector<std::vector<std::uint8_t>> packets;
		{
			std::lock_guard guard{ stateLock };
			if (!LocalPlayer::InWorld()) {
				return;  // a loading screen, the main menu: the events wait in the queue
			}
			const auto now = Clock::now();
			std::vector<Got>               gotNow;
			std::vector<Said>              saidNow;
			std::vector<Clock::time_point> stagedNow;
			{
				std::lock_guard queueGuard{ queueLock };
				gotNow = std::exchange(gots, {});
				saidNow = std::exchange(saids, {});
				stagedNow = std::exchange(stages, {});
			}
			// The stages made and the gold paid first: an item is judged by those before it and near it, whichever event
			// came first this tick.
			for (const auto at : stagedNow) {
				talk.staged.Add(at);
				++counts.stages;
			}
			// The topic info said last in our talk (by our player or whom it talks with; not a friend's talk played here)
			// names the quest. Read before the items: an info's fragment gives as its line ends.
			for (const auto& said : saidNow) {
				if (said.speaker != PLAYER_REF && said.speaker != partner) {
					continue;
				}
				++counts.infos;
				talk.infoQuest = QuestOfInfo(said.info);
				talk.infoAt = said.at;
			}
			// Judged by the menus as they were up to now; read after.
			for (const auto& got : gotNow) {
				if (got.paid) {
					Judge(got);
				}
			}
			for (const auto& got : gotNow) {
				if (!got.paid) {
					Judge(got);
				}
			}
			ReadMenus(now);
			for (const auto& reward : outbox.Due(now)) {
				packets.push_back(Protocol::Encode(Protocol::QuestReward{ .questId = reward.quest, .itemId = reward.item, .count = reward.count },
					Protocol::MessageType::kQuestReward));
				++counts.sent;
				logs::info("Rewards: our player got {} of {:08X} in a conversation of quest {:08X}: said to the session", reward.count, reward.item, reward.quest);
			}
			GiveWaiting(now);
			std::erase_if(expected, [&](const Expect& a_e) { return now - a_e.at > EXPECT_FOR; });
			ledger.Prune(now);
		}
		for (auto& packet : packets) {
			a_send(std::move(packet));
		}
	}

	void Heard(const Protocol::QuestReward& a_reward)
	{
		std::lock_guard guard{ stateLock };
		if (a_reward.playerId == ourId) {
			return;
		}
		++counts.heard;
		if (!QuestSync::HearsOthers()) {
			++counts.beforeReset;
			return;
		}
		if (waiting.size() >= RewardRules::MAX_WAITING) {
			if (counts.waitFull++ == 0) {
				logs::warn("Rewards: more than {} rewards wait to be given here; the rest are dropped", RewardRules::MAX_WAITING);
			}
			return;
		}
		waiting.push_back({ a_reward.playerId, { a_reward.questId, a_reward.itemId, a_reward.count }, Clock::now() });
	}

	void AfterLoad()
	{
		std::lock_guard guard{ stateLock };
		Forget();
		// A friend's load: the rewards heard still wait (the session's stages are made again in the loaded world, then they
		// are given). The host's: its world is the session's and is from before them; given now, each would come again
		// when its step is done again in this world.
		if (QuestSync::HostsWorld()) {
			DropWaiting("this game is the host's and loaded a save");
		}
	}

	void HostReset(std::uint32_t a_hostId)
	{
		std::lock_guard guard{ stateLock };
		DropWaiting(std::format("the host (player {}) loaded a save or came in: its world is new", a_hostId));
	}

	void Clear()
	{
		std::lock_guard guard{ stateLock };
		ourId = 0;
		Forget();
		waiting.clear();
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
			std::string why;
			for (std::size_t i = 0; i < RewardRules::WHY_COUNT; ++i) {
				why += std::format("{}{}:{}", why.empty() ? "" : ",", RewardRules::WhyName(static_cast<RewardRules::Why>(i)), Counted(counts.why[i]));
			}
			return std::format("installed={} got={} own={} why={} infos={} infoQuest={:08X} partner={:08X} out={} sent={} outboxFull={} heard={} beforeReset={} "
							   "waiting={} waitFull={} given={} covered={} had={} notShared={} notHere={} stale={} badItem={} norun={} ledger={} expected={} "
							   "eventsDropped={} offmain={} paid={} stages={} oldWorld={}",
				installed ? 1 : 0, Counted(counts.got), Counted(counts.own), why, Counted(counts.infos), talk.infoQuest, partner, outbox.Size(), Counted(counts.sent),
				Counted(counts.outboxFull), Counted(counts.heard), Counted(counts.beforeReset), waiting.size(), Counted(counts.waitFull), Counted(counts.given),
				Counted(counts.covered), Counted(counts.had), Counted(counts.notShared), Counted(counts.notHere), Counted(counts.stale), Counted(counts.badItem),
				Counted(counts.noRun), ledger.Size(), expected.size(), Counted(dropped), Counted(counts.offMain), Counted(counts.paid), Counted(counts.stages), Counted(counts.oldWorld));
		}
		if (words[0] == "list") {
			const auto  now = Clock::now();
			std::string out;
			for (const auto& w : waiting) {
				out += std::format("{}{:08X}:{:08X}:{}:p{}:{}s", out.empty() ? "" : " ", w.reward.quest, w.reward.item, w.reward.count, w.from,
					std::chrono::duration_cast<std::chrono::seconds>(now - w.at).count());
			}
			return out.empty() ? "waiting=none" : "waiting=" + out;
		}
		return "error: usage: rewards [list]";
	}
}
