#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/RewardRules.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// Quest rewards for everyone (docs/DESIGN.md, "Quests", "Rewards"): the messages (common/Protocol.h), their form IDs on
// the wire (common/FormTranslate.h), the server's passing on (common/Server.cpp), and each game's rules
// (src/game/RewardRules.h). What reads and changes the game itself (src/game/QuestRewards.cpp) is tested in a game only.

using Protocol::QuestReward;
using namespace RewardRules;

namespace
{
	using MT = Protocol::MessageType;
	using LootRules::ItemKind;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	const Clock::time_point T0 = Clock::time_point{} + std::chrono::hours{ 1 };

	// Our player in a conversation of quest 0x40 since T0, the info said at T0.
	Talk Talking()
	{
		Talk talk;
		talk.dialogue = T0;
		talk.infoQuest = 0x40;
		talk.infoAt = T0;
		return talk;
	}

	QuestHere Ran(std::optional<Clock::time_point> a_lastMade = std::nullopt)
	{
		return { .shared = true, .ran = true, .stagesWaiting = false, .lastMade = a_lastMade };
	}

	Heard Reward40(std::uint32_t a_item, std::uint32_t a_count, Clock::time_point a_at) { return { 2, { 0x40, a_item, a_count }, a_at }; }

	std::size_t CountOf(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_p) { return Protocol::PeekType(a_p) == a_type; }));
	}

	std::vector<QuestReward> RewardsIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<QuestReward> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemoteQuestReward) {
				if (const auto msg = Protocol::DecodeQuestReward(packet)) {
					out.push_back(*msg);
				}
			}
		}
		return out;
	}
}

// ---- the messages -----------------------------------------------------------------------------------------------------

TEST("rewards: a reward goes and comes back as it was; a remote one carries its player")
{
	const QuestReward mine{ .playerId = 9, .questId = 0x0001F0A2, .itemId = 0xF, .count = 500 };
	const auto        packet = Protocol::Encode(mine, MT::kQuestReward);
	CHECK(Protocol::PeekType(packet) == MT::kQuestReward);
	auto back = Protocol::DecodeQuestReward(packet);
	REQUIRE(back);
	CHECK(back->playerId == 0);  // a client's reward carries no player: the server knows whose it is
	CHECK(back->questId == mine.questId);
	CHECK(back->itemId == mine.itemId);
	CHECK(back->count == mine.count);

	back = Protocol::DecodeQuestReward(Protocol::Encode(mine, MT::kRemoteQuestReward));
	REQUIRE(back);
	CHECK(*back == mine);

	CHECK(FormTranslate::CarriesForms(MT::kQuestReward));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteQuestReward));
}

TEST("rewards: a reward that can't be read is none: no quest, no item, no count, too many, short or long")
{
	CHECK(!Protocol::DecodeQuestReward(Protocol::Encode(QuestReward{ .questId = 0, .itemId = 0xF, .count = 1 }, MT::kQuestReward)));
	CHECK(!Protocol::DecodeQuestReward(Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0, .count = 1 }, MT::kQuestReward)));
	CHECK(!Protocol::DecodeQuestReward(Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0xF, .count = 0 }, MT::kQuestReward)));
	CHECK(!Protocol::DecodeQuestReward(
		Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0xF, .count = Protocol::MAX_REWARD_COUNT + 1 }, MT::kQuestReward)));
	CHECK(Protocol::DecodeQuestReward(Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0xF, .count = Protocol::MAX_REWARD_COUNT }, MT::kQuestReward)));

	auto packet = Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0xF, .count = 1 }, MT::kQuestReward);
	auto longer = packet;
	longer.push_back(0);
	CHECK(!Protocol::DecodeQuestReward(longer));
	packet.pop_back();
	CHECK(!Protocol::DecodeQuestReward(packet));
	CHECK(!Protocol::DecodeQuestReward(Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0xF, .count = 1 }, MT::kQuestRun)));
}

TEST("rewards: the quest and the item are named by their plugin on the wire; a reward another game can't name is not sent there")
{
	using FormTranslate::Result;
	using FormTranslate::Way;
	using Names = std::vector<std::string>;
	const Names host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "A.esp", "B.esp" }, {});
	const Names guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "B.esp", "Update.esm", "A.esp", "Extra.esp" }, {});
	Names       session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper hostEdge(host, session);
	const FormIds::Mapper guestEdge(guest, session);

	// The guest got gold for a quest of B.esp (its 01): named in the session's numbering (B.esp is its 03).
	auto out = FormTranslate::Translate(guestEdge, Way::kToSession, Protocol::Encode(QuestReward{ .questId = 0x01000D62, .itemId = 0xF, .count = 25 }, MT::kQuestReward));
	REQUIRE(out.kind == Result::Kind::kChanged);
	auto msg = Protocol::DecodeQuestReward(out.packet);
	REQUIRE(msg);
	CHECK(msg->questId == 0x03000D62);
	CHECK(msg->itemId == 0xF);
	CHECK(Protocol::PeekType(out.packet) == MT::kQuestReward);

	// Passed on to the host, whose numbering is the session's: as it was.
	out = FormTranslate::Translate(hostEdge, Way::kToLocal,
		Protocol::Encode(QuestReward{ .playerId = 2, .questId = 0x03000D62, .itemId = 0x01000800, .count = 1 }, MT::kRemoteQuestReward));
	CHECK(out.kind == Result::Kind::kSame);

	// Passed on to the guest: in its numbering (B.esp its 01, Update.esm its 02), with the player kept.
	out = FormTranslate::Translate(guestEdge, Way::kToLocal,
		Protocol::Encode(QuestReward{ .playerId = 2, .questId = 0x03000D62, .itemId = 0x01000800, .count = 1 }, MT::kRemoteQuestReward));
	REQUIRE(out.kind == Result::Kind::kChanged);
	msg = Protocol::DecodeQuestReward(out.packet);
	REQUIRE(msg);
	CHECK(Protocol::PeekType(out.packet) == MT::kRemoteQuestReward);
	CHECK(msg->playerId == 2);
	CHECK(msg->questId == 0x01000D62);
	CHECK(msg->itemId == 0x02000800);
	CHECK(msg->count == 1);

	// An item of Extra.esp (the session's 04), which the host lacks: the host gets nothing, rather than another item.
	out = FormTranslate::Translate(hostEdge, Way::kToLocal,
		Protocol::Encode(QuestReward{ .playerId = 2, .questId = 0x03000D62, .itemId = 0x04000800, .count = 1 }, MT::kRemoteQuestReward));
	CHECK(out.kind == Result::Kind::kDrop);
	// ... and a quest of Extra.esp likewise.
	out = FormTranslate::Translate(hostEdge, Way::kToLocal,
		Protocol::Encode(QuestReward{ .playerId = 2, .questId = 0x04000D62, .itemId = 0xF, .count = 1 }, MT::kRemoteQuestReward));
	CHECK(out.kind == Result::Kind::kDrop);
	// A runtime item has no ID in the session: not sent.
	out = FormTranslate::Translate(guestEdge, Way::kToSession, Protocol::Encode(QuestReward{ .questId = 0x01000D62, .itemId = 0xFF000123, .count = 1 }, MT::kQuestReward));
	CHECK(out.kind == Result::Kind::kDrop);
}

// ---- the talker's game ------------------------------------------------------------------------------------------------

TEST("reward judge: an item from nothing in a conversation of a shared quest is its reward; not out of one, in a trade, or of no quest")
{
	const auto talk = Talking();
	CHECK(Judge(talk, T0 + 500ms, ItemKind::kMisc) == Why::kReward);
	CHECK(Judge(talk, T0 + TALK_GRACE, ItemKind::kWeapon) == Why::kReward);  // the info's end fragment, as the menu shuts
	CHECK(Judge(talk, T0 + TALK_GRACE + 1ms, ItemKind::kWeapon) == Why::kNotTalking);

	Talk none;
	CHECK(Judge(none, T0, ItemKind::kMisc) == Why::kNotTalking);  // crafting, harvest, a script of its own

	auto trading = talk;
	trading.trade = T0;  // a merchant's barter from the conversation
	CHECK(Judge(trading, T0 + 500ms, ItemKind::kMisc) == Why::kTrading);
	CHECK(Judge(trading, T0 + TRADE_GRACE + 1ms, ItemKind::kMisc) == Why::kReward);  // the trade shut, the talk goes on

	auto noQuest = talk;
	noQuest.infoQuest = 0;  // the info's quest is not shared here (or none)
	CHECK(Judge(noQuest, T0 + 500ms, ItemKind::kMisc) == Why::kNoQuest);
	auto oldInfo = talk;
	oldInfo.dialogue = T0 + 20s;  // still talking, but the last info of a shared quest was long ago
	CHECK(Judge(oldInfo, T0 + 20s, ItemKind::kMisc) == Why::kNoQuest);
}

TEST("reward judge: a key is the door's, not a reward; anything else a player carries is")
{
	const auto talk = Talking();
	CHECK(Judge(talk, T0, ItemKind::kKey) == Why::kKind);
	CHECK(Judge(talk, T0, ItemKind::kOther) == Why::kKind);
	for (const auto kind : { ItemKind::kWeapon, ItemKind::kArmor, ItemKind::kAmmo, ItemKind::kPotion, ItemKind::kIngredient, ItemKind::kMisc,
			 ItemKind::kBook, ItemKind::kSoulGem, ItemKind::kScroll, ItemKind::kLight, ItemKind::kApparatus }) {
		CHECK(KindShared(kind));
		CHECK(Judge(talk, T0, kind) == Why::kReward);
	}
	CHECK(std::string{ WhyName(Why::kMade) } == "made");
	CHECK(WHY_COUNT == 6);
}

TEST("reward judge: an item a stage of the quest made here from the session's word just gave is that stage's, not our reward")
{
	auto talk = Talking();
	talk.madeAt = T0 + 1s;  // another player's step made here as we talk: its fragment gave this
	CHECK(Judge(talk, T0 + 1500ms, ItemKind::kMisc) == Why::kMade);
	CHECK(Judge(talk, T0 + 1s + MADE_GRACE, ItemKind::kMisc) == Why::kMade);
	CHECK(Judge(talk, T0 + 1s + MADE_GRACE + 1ms, ItemKind::kMisc) == Why::kReward);
	CHECK(Judge(talk, T0 + 500ms, ItemKind::kMisc) == Why::kReward);  // before the stage was made: ours
}

TEST("reward outbox: the same item for the same quest within SEND_AFTER is one reward, said SEND_AFTER after its last part")
{
	Outbox outbox;
	CHECK(outbox.Add({ 0x40, 0xF, 100 }, T0));
	CHECK(outbox.Add({ 0x40, 0xF, 50 }, T0 + 500ms));  // gold given in two parts
	CHECK(outbox.Add({ 0x40, 0x1234, 1 }, T0 + 500ms));
	CHECK(outbox.Add({ 0x41, 0xF, 7 }, T0 + 600ms));    // another quest's gold: its own
	CHECK(outbox.Due(T0 + 1s).empty());
	auto due = outbox.Due(T0 + 1500ms);
	REQUIRE(due.size() == 2);
	CHECK((due[0] == Reward{ 0x40, 0xF, 150 }));
	CHECK((due[1] == Reward{ 0x40, 0x1234, 1 }));
	due = outbox.Due(T0 + 1600ms);
	REQUIRE(due.size() == 1);
	CHECK((due[0] == Reward{ 0x41, 0xF, 7 }));
	CHECK(outbox.Size() == 0);

	CHECK(outbox.Add({ 0x40, 0xF, MAX_COUNT }, T0));
	CHECK(outbox.Add({ 0x40, 0xF, 5 }, T0));
	due = outbox.Due(T0 + 2s);
	REQUIRE(due.size() == 1);
	CHECK(due[0].count == MAX_COUNT);  // never past what the message carries

	for (std::uint32_t i = 0; i < MAX_OUTBOX; ++i) {
		CHECK(outbox.Add({ 0x40, 0x100 + i, 1 }, T0));
	}
	CHECK(!outbox.Add({ 0x40, 0x999, 1 }, T0));  // a game giving more than that a second is broken
	CHECK(outbox.Add({ 0x40, 0x100, 1 }, T0));   // one already there still adds up
}

// ---- another game -----------------------------------------------------------------------------------------------------

TEST("reward give: a reward from the conversation alone is given whole once the quest's stages are made and settled")
{
	Ledger     ledger;
	const auto heard = Reward40(0x1234, 1, T0);
	auto       quest = Ran();
	quest.stagesWaiting = true;  // the stage the conversation set is still to make here
	CHECK(Judge(heard, quest, ledger, T0 + 1s).verdict == Verdict::kWait);

	quest = Ran(T0 + 2s);  // made
	CHECK(Judge(heard, quest, ledger, T0 + 2s + SETTLE - 1ms).verdict == Verdict::kWait);  // its fragments may still run
	const auto d = Judge(heard, quest, ledger, T0 + 2s + SETTLE);
	CHECK(d.verdict == Verdict::kGive);
	CHECK(d.give == 1);
	CHECK(d.had == 0);
}

TEST("reward give: what the stage gave here already is not given twice; only the rest is")
{
	Ledger ledger;
	// The conversation set stage 20 in the talker's game; its fragment gives 100 gold in every game, and the info's own
	// fragment gave a sword there only. The talker's game said both (it can't tell them apart).
	const auto made = T0 + 1s;  // stage 20 made here
	ledger.Add(0xF, 100, made + 200ms);
	CHECK(Judge(Reward40(0xF, 100, T0), Ran(made), ledger, made + SETTLE).verdict == Verdict::kGive);

	Ledger     fresh;
	fresh.Add(0xF, 100, made + 200ms);
	auto d = Judge(Reward40(0xF, 150, T0), Ran(made), fresh, made + SETTLE);  // the info gave 50 more
	CHECK(d.verdict == Verdict::kGive);
	CHECK(d.give == 50);
	CHECK(d.had == 100);
	// What a reward used up is not used again by a second reward of the same item.
	d = Judge(Reward40(0xF, 100, T0), Ran(made), fresh, made + SETTLE);
	CHECK(d.give == 100);

	Ledger all;
	all.Add(0xF, 100, made + 200ms);
	d = Judge(Reward40(0xF, 100, T0), Ran(made), all, made + SETTLE);
	CHECK(d.verdict == Verdict::kGive);
	CHECK(d.give == 0);  // the stage gave it all here
	CHECK(d.had == 100);
}

TEST("reward give: gold our player got long before the reward's stage is not taken for it")
{
	Ledger ledger;
	ledger.Add(0xF, 100, T0);  // gold a script gave a minute before
	const auto made = T0 + 60s;
	const auto d = Judge(Reward40(0xF, 100, made - 1s), Ran(made), ledger, made + SETTLE);
	CHECK(d.give == 100);
	// A conversation that set no stage lately: only what came since the word counts.
	Ledger later;
	later.Add(0xF, 30, T0 + 1s);
	const auto e = Judge(Reward40(0xF, 100, T0 + 1s), Ran(T0 - 5min), later, T0 + 1s + SETTLE);
	CHECK(e.give == 70);
}

TEST("reward give: a quest not shared here gets nothing; one never started waits START_WAIT, then gets nothing; any waits GIVE_UP at most")
{
	Ledger ledger;
	const auto heard = Reward40(0xF, 10, T0);
	CHECK(Judge(heard, QuestHere{}, ledger, T0).verdict == Verdict::kNotShared);

	QuestHere notRun{ .shared = true };
	CHECK(Judge(heard, notRun, ledger, T0 + START_WAIT - 1ms).verdict == Verdict::kWait);
	CHECK(Judge(heard, notRun, ledger, T0 + START_WAIT).verdict == Verdict::kNotHere);

	auto waitingStages = Ran();
	waitingStages.stagesWaiting = true;  // held for a friend far away, say
	CHECK(Judge(heard, waitingStages, ledger, T0 + GIVE_UP - 1ms).verdict == Verdict::kWait);
	CHECK(Judge(heard, waitingStages, ledger, T0 + GIVE_UP).verdict == Verdict::kStale);
}

TEST("reward ledger: keeps KEEP, at most MAX_LEDGER items; takes only the item, from the time asked")
{
	Ledger ledger;
	ledger.Add(0xF, 10, T0);
	ledger.Add(0x1234, 1, T0);
	ledger.Add(0, 5, T0);  // nothing
	ledger.Add(0xF, 0, T0);
	CHECK(ledger.Size() == 2);
	CHECK(ledger.Take(0xF, T0 + 1ms, 10) == 0);
	CHECK(ledger.Take(0xF, T0, 4) == 4);
	CHECK(ledger.Take(0xF, T0, 10) == 6);
	ledger.Prune(T0);
	CHECK(ledger.Size() == 1);  // the used-up gold went
	ledger.Prune(T0 + KEEP + 1ms);
	CHECK(ledger.Size() == 0);
	for (std::size_t i = 0; i < MAX_LEDGER + 10; ++i) {
		ledger.Add(0xF, 1, T0);
	}
	CHECK(ledger.Size() == MAX_LEDGER);
	CHECK(KEEP > GIVE_UP);
}

// ---- the server -------------------------------------------------------------------------------------------------------

TEST("server: a quest reward is passed on to the other players with its player, never back to its own, never kept")
{
	Fixture f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	REQUIRE(ada);
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	f.fake->Take(1);
	f.fake->Take(2);
	f.fake->Take(3);

	f.fake->Deliver(1, Protocol::Encode(QuestReward{ .playerId = 77, .questId = 0x40, .itemId = 0xF, .count = 100 }, MT::kQuestReward));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kRemoteQuestReward)));
	REQUIRE(f.fake->WaitFor(3, Type(MT::kRemoteQuestReward)));
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(1), MT::kRemoteQuestReward) == 0);

	// A joiner after it gets nothing of it: a reward is never kept.
	REQUIRE(f.Join(4, Fixture::MakeHello("Di")));
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(4), MT::kRemoteQuestReward) == 0);

	f.fake->Deliver(2, Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0x1234, .count = 1 }, MT::kQuestReward));
	const auto rewards = RewardsIn(f.fake->TakeThrough(1, Type(MT::kRemoteQuestReward)));
	REQUIRE(rewards.size() == 1);
	CHECK(rewards[0].playerId != ada->playerId);
	CHECK(rewards[0].itemId == 0x1234);
}

TEST("server: a quest reward sent before its game heard the host's reset is dropped, as its quest reports are")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	REQUIRE(f.Join(1, Fixture::MakeHello("Host", 0x4057)));
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	REQUIRE(f.fake->Settle());
	f.fake->Take(1);
	f.fake->Take(2);

	f.fake->Deliver(1, Protocol::Encode(Protocol::QuestReset{}, false));
	f.fake->Deliver(2, Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0xF, .count = 100 }, MT::kQuestReward));
	REQUIRE(f.LoggedSoon("said a quest reward of the world before the host's reset"));
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(1), MT::kRemoteQuestReward) == 0);

	f.fake->Deliver(2, Protocol::Writer{ MT::kQuestResetSeen }.Data());
	f.fake->Deliver(2, Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0xF, .count = 5 }, MT::kQuestReward));
	const auto rewards = RewardsIn(f.fake->TakeThrough(1, Type(MT::kRemoteQuestReward)));
	REQUIRE(rewards.size() == 1);
	CHECK(rewards[0].count == 5);
}

TEST("server: a game saying rewards faster than a conversation gives them has the rest dropped, said once")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	f.fake->Take(1);
	f.fake->Take(2);
	for (std::uint32_t i = 0; i < 100; ++i) {
		f.fake->Deliver(1, Protocol::Encode(QuestReward{ .questId = 0x40, .itemId = 0x100 + i, .count = 1 }, MT::kQuestReward));
	}
	REQUIRE(f.LoggedSoon("says quest rewards faster than a conversation gives them"));
	REQUIRE(f.fake->Settle());
	const auto passed = RewardsIn(f.fake->Take(2)).size();
	CHECK(passed >= 32);
	CHECK(passed < 100);
	const auto lines = f.LogLines();
	CHECK(std::ranges::count_if(lines, [](const std::string& a_l) { return a_l.find("faster than a conversation") != std::string::npos; }) == 1);
}
