#include "Test.h"

#include "FormTranslate.h"
#include "QuestBook.h"
#include "ServerFixture.h"
#include "game/QuestRules.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Shared quests (docs/DESIGN.md, "Quests"): the messages (common/Protocol.h), their form IDs on the wire
// (common/FormTranslate.h), the server's book (common/QuestBook.h) and passing on (common/Server.cpp), and each game's
// rules (src/game/QuestRules.h). What reads and changes the game itself (src/game/QuestSync.cpp) is tested in a game
// only: tools/rig/steps/quests.sh.

using Protocol::QuestBatch;
using Protocol::QuestEntry;
using Protocol::QuestReset;

namespace
{
	using MT = Protocol::MessageType;
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	QuestEntry Stage(std::uint32_t a_quest, std::uint16_t a_stage) { return { a_quest, QuestEntry::kStage, a_stage, 0 }; }
	QuestEntry Objective(std::uint32_t a_quest, std::uint16_t a_index, std::uint8_t a_state, std::uint16_t a_at = 0) { return { a_quest, QuestEntry::kObjective, a_index, a_state, a_at }; }
	QuestEntry Completed(std::uint32_t a_quest) { return { a_quest, QuestEntry::kCompleted, 0, 0 }; }
	QuestEntry Failed(std::uint32_t a_quest) { return { a_quest, QuestEntry::kFailed, 0, 0 }; }

	std::vector<std::uint8_t> Report(std::vector<QuestEntry> a_entries) { return Protocol::Encode(QuestBatch{ 0, std::move(a_entries) }); }

	// The entries of every batch of a_type in a_packets, in order, with the player each batch named.
	std::vector<std::pair<std::uint32_t, QuestEntry>> EntriesIn(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type = MT::kRemoteQuests)
	{
		std::vector<std::pair<std::uint32_t, QuestEntry>> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == a_type) {
				if (const auto batch = Protocol::DecodeQuestBatch(packet)) {
					for (const auto& e : batch->entries) {
						out.emplace_back(batch->playerId, e);
					}
				}
			}
		}
		return out;
	}

	std::size_t CountOf(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_packet) { return Protocol::PeekType(a_packet) == a_type; }));
	}

	const Clock::time_point T0 = Clock::time_point{} + 1h;
}

// ---- the messages -------------------------------------------------------------------------------------------------

TEST("quests: a batch goes to the server and back, its reporter named on the way back, and an answer carries why")
{
	const std::vector<QuestEntry> entries{ Stage(0x0001F0A1, 10), Objective(0x0001F0A1, 10, 1, 0x1234), Completed(0x0001F0A1), Failed(0x0002B000) };
	const auto                    report = Protocol::Encode(QuestBatch{ 99, entries });  // a client's batch carries no player
	CHECK(Protocol::PeekType(report) == MT::kQuests);
	auto back = Protocol::DecodeQuestBatch(report);
	REQUIRE(back);
	CHECK(back->entries == entries);
	CHECK(back->playerId == 0);

	const auto world = Protocol::Encode(QuestBatch{ 99, entries }, MT::kQuestWorld);  // the host's world: no player either
	CHECK(Protocol::PeekType(world) == MT::kQuestWorld);
	back = Protocol::DecodeQuestBatch(world);
	REQUIRE(back);
	CHECK(back->entries == entries);
	CHECK(back->playerId == 0);
	CHECK(FormTranslate::CarriesForms(MT::kQuestWorld));

	const auto remote = Protocol::Encode(QuestBatch{ 3, entries }, MT::kRemoteQuests);
	back = Protocol::DecodeQuestBatch(remote);
	REQUIRE(back);
	CHECK(back->playerId == 3);
	CHECK(back->entries == entries);

	auto answered = entries;
	answered[0].value = QuestEntry::kBusy;
	answered[1].value = QuestEntry::kRefused;  // an answer's value is why, not an objective's state
	const auto answer = Protocol::Encode(QuestBatch{ 0, answered }, MT::kQuestAnswer);
	back = Protocol::DecodeQuestBatch(answer);
	REQUIRE(back);
	CHECK(back->entries[0].value == QuestEntry::kBusy);
	CHECK(FormTranslate::CarriesForms(MT::kQuests));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteQuests));
	CHECK(FormTranslate::CarriesForms(MT::kQuestAnswer));
	CHECK(!FormTranslate::CarriesForms(MT::kAskQuests));
	CHECK(!FormTranslate::CarriesForms(MT::kQuestReset));
}

TEST("quests: a batch that can't be read is none: empty, a kind not known, a state or an answer out of range, short or long")
{
	CHECK(!Protocol::DecodeQuestBatch(Report({})));  // empty
	auto packet = Report({ Stage(0x10, 5) });
	auto bad = packet;
	bad[1 + 1 + 4] = static_cast<std::uint8_t>(QuestEntry::KIND_MAX + 1);  // type, count, questId, then the kind
	CHECK(!Protocol::DecodeQuestBatch(bad));
	bad = Report({ Objective(0x10, 1, static_cast<std::uint8_t>(QuestEntry::OBJECTIVE_STATE_MAX + 1)) });
	CHECK(!Protocol::DecodeQuestBatch(bad));
	CHECK(Protocol::DecodeQuestBatch(Report({ Objective(0x10, 1, QuestEntry::OBJECTIVE_STATE_MAX) })));
	bad = Protocol::Encode(QuestBatch{ 0, { Stage(0x10, 5) } }, MT::kQuestAnswer);
	bad[bad.size() - 3] = static_cast<std::uint8_t>(QuestEntry::ANSWER_MAX + 1);  // its value: then the stage's two bytes
	CHECK(!Protocol::DecodeQuestBatch(bad));
	bad = packet;
	bad.pop_back();
	CHECK(!Protocol::DecodeQuestBatch(bad));
	bad = packet;
	bad.push_back(0);
	CHECK(!Protocol::DecodeQuestBatch(bad));
	bad = packet;
	bad[1] = 2;  // says two entries, holds one
	CHECK(!Protocol::DecodeQuestBatch(bad));
	bad = packet;
	bad[1] = static_cast<std::uint8_t>(Protocol::MAX_QUEST_BATCH + 1);
	CHECK(!Protocol::DecodeQuestBatch(bad));
	CHECK(!Protocol::DecodeQuestBatch(Protocol::Encode(QuestReset{ 1 }, true)));
}

TEST("quests: a long list goes in batches of MAX_QUEST_BATCH, in order, and nothing for none")
{
	std::vector<QuestEntry> entries;
	for (std::uint16_t i = 0; i < 150; ++i) {
		entries.push_back(Stage(static_cast<std::uint32_t>(0x100 + i / 10), i));
	}
	const auto packets = Protocol::Batches(entries, MT::kRemoteQuests, 4);
	REQUIRE(packets.size() == 3);
	const auto back = EntriesIn(packets);
	REQUIRE(back.size() == entries.size());
	for (std::size_t i = 0; i < entries.size(); ++i) {
		CHECK(back[i].second == entries[i]);
		CHECK(back[i].first == 4);
	}
	CHECK(Protocol::DecodeQuestBatch(packets[0])->entries.size() == Protocol::MAX_QUEST_BATCH);
	CHECK(Protocol::Batches({}, MT::kQuests).empty());
	// Encode itself never writes more than a batch holds
	const auto one = Protocol::DecodeQuestBatch(Protocol::Encode(QuestBatch{ 0, entries }));
	REQUIRE(one);
	CHECK(one->entries.size() == Protocol::MAX_QUEST_BATCH);
}

TEST("quests: the host's reset is empty from the host and names it from the server")
{
	const auto fromHost = Protocol::Encode(QuestReset{}, false);
	CHECK(fromHost.size() == 1);
	CHECK(Protocol::DecodeQuestReset(fromHost, false));
	CHECK(!Protocol::DecodeQuestReset(fromHost, true));
	const auto fromServer = Protocol::Encode(QuestReset{ 7 }, true);
	const auto back = Protocol::DecodeQuestReset(fromServer, true);
	REQUIRE(back);
	CHECK(back->playerId == 7);
	CHECK(!Protocol::DecodeQuestReset(fromServer, false));
}

// ---- form IDs on the wire -----------------------------------------------------------------------------------------

TEST("quests: a quest is named by its plugin on the wire, and a quest the other game lacks is left out of the batch")
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

	// The guest's quest of B.esp (its slot 01) and one of Extra.esp, which the host lacks.
	const auto out = FormTranslate::Translate(guestEdge, Way::kToSession, Report({ Stage(0x01000D62, 10), Stage(0x04000800, 5), Stage(0x0001F0A1, 20) }));
	REQUIRE(out.kind == Result::Kind::kChanged);
	auto batch = Protocol::DecodeQuestBatch(out.packet);
	REQUIRE(batch);
	REQUIRE(batch->entries.size() == 3);
	CHECK(batch->entries[0].questId == 0x03000D62);  // B.esp is the session's 03
	CHECK(batch->entries[1].questId == 0x04000800);  // Extra.esp is the session's 04, after the host's plugins
	CHECK(batch->entries[2].questId == 0x0001F0A1);

	// Coming in to the host: Extra.esp's quest is left out, the rest named in the host's numbering.
	const auto remote = Protocol::Encode(*batch, MT::kRemoteQuests);
	const auto in = FormTranslate::Translate(hostEdge, Way::kToLocal, remote);
	REQUIRE(in.kind == Result::Kind::kChanged);
	CHECK(in.entries == 1);
	batch = Protocol::DecodeQuestBatch(in.packet);
	REQUIRE(batch);
	REQUIRE(batch->entries.size() == 2);
	CHECK(batch->entries[0].questId == 0x03000D62);
	CHECK(batch->entries[1].questId == 0x0001F0A1);

	// A batch of only a quest the host lacks is not read at all; a runtime quest is never sent.
	CHECK(FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(QuestBatch{ 2, { Stage(0x04000800, 5) } }, MT::kRemoteQuests)).kind == Result::Kind::kDrop);
	CHECK(FormTranslate::Translate(guestEdge, Way::kToSession, Report({ Stage(0xFF000123, 5) })).kind == Result::Kind::kDrop);
	CHECK(FormTranslate::Translate(FormIds::Mapper(), Way::kToSession, Report({ Stage(0xFF000123, 5) })).kind == Result::Kind::kDrop);
	CHECK(FormTranslate::Translate(FormIds::Mapper(), Way::kToSession, Report({ Stage(0x0001F0A1, 5) })).kind == Result::Kind::kSame);
}

// ---- the server's book --------------------------------------------------------------------------------------------

TEST("quest book: a stage once done stays done, and the same stage again is not news")
{
	QuestBook  book;
	QuestEntry out;
	CHECK(book.Report(Stage(0x10, 10), out) == QuestBook::Take::kNews);
	CHECK(out == Stage(0x10, 10));
	CHECK(book.Report(Stage(0x10, 10), out) == QuestBook::Take::kSame);
	CHECK(book.Report(Stage(0x10, 5), out) == QuestBook::Take::kNews);  // a lower stage is a stage too
	CHECK(book.Report({ 0x10, QuestEntry::kStage, 20, 9 }, out) == QuestBook::Take::kNews);
	CHECK(out.value == 0);  // a stage's value means nothing: kept as 0
	CHECK(book.Quests() == 1);
	CHECK(book.Entries() == 3);
}

TEST("quest book: an objective goes forward only: shown stays shown, the first end stays")
{
	using B = QuestBook;
	CHECK(B::MergeObjective(0, 1) == 1);
	CHECK(B::MergeObjective(1, 0) == 1);
	CHECK(B::MergeObjective(1, 2) == 3);  // shown, then completed: completed and shown
	CHECK(B::MergeObjective(3, 1) == 3);
	CHECK(B::MergeObjective(3, 5) == 3);  // completed first: failed later does not undo it
	CHECK(B::MergeObjective(4, 3) == 5);  // failed first stays failed, and is shown
	CHECK(B::Ahead(Objective(1, 1, 3), Objective(1, 1, 1)));
	CHECK(!B::Ahead(Objective(1, 1, 1), Objective(1, 1, 3)));
	CHECK(!B::Ahead(Objective(1, 1, 0), Objective(1, 1, 0)));
	CHECK(!B::Ahead(Stage(1, 10), Stage(1, 10)));

	QuestBook  book;
	QuestEntry out;
	CHECK(book.Report(Objective(0x10, 10, 0), out) == QuestBook::Take::kRefused);  // dormant says nothing
	CHECK(book.Report(Objective(0x10, 10, 1), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Objective(0x10, 10, 1), out) == QuestBook::Take::kSame);
	CHECK(book.Report(Objective(0x10, 10, 2), out) == QuestBook::Take::kNews);  // completed, not shown: news
	CHECK(out.value == 3);  // ... merged: completed and shown
	CHECK(book.Report(Objective(0x10, 10, 1), out) == QuestBook::Take::kSame);  // behind: the session's word is ahead
	CHECK(out.value == 3);
	CHECK(book.Entries() == 1);
	CHECK(book.Report(Objective(0x10, 10, 6), out) == QuestBook::Take::kRefused);
}

TEST("quest book: an objective's shown part holds at the stage it was read at; read shown at a later stage is news, at an earlier one is not")
{
	using B = QuestBook;
	CHECK(B::Ahead(Objective(1, 1, 1, 20), Objective(1, 1, 1, 10)));   // the quest moved on, the objective still shown
	CHECK(!B::Ahead(Objective(1, 1, 1, 10), Objective(1, 1, 1, 20)));  // an older word
	CHECK(!B::Ahead(Objective(1, 1, 2, 30), Objective(1, 1, 3, 10)));  // completed, not shown, later: nothing new
	CHECK(B::Merge(Objective(1, 1, 1, 10), Objective(1, 1, 2, 30)) == Objective(1, 1, 3, 10));  // the end joins; shown stays at 10
	CHECK(B::Merge(Objective(1, 1, 1, 10), Objective(1, 1, 1, 20)) == Objective(1, 1, 1, 20));
	CHECK(B::Merge(Objective(1, 1, 2, 10), Objective(1, 1, 1, 5)) == Objective(1, 1, 3, 5));   // first read shown at 5

	QuestBook  book;
	QuestEntry out;
	CHECK(book.Report(Objective(0x10, 10, 1, 10), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Objective(0x10, 10, 1, 10), out) == QuestBook::Take::kSame);
	CHECK(book.Report(Objective(0x10, 10, 1, 5), out) == QuestBook::Take::kSame);
	CHECK(out == Objective(0x10, 10, 1, 10));
	CHECK(book.Report(Objective(0x10, 10, 1, 20), out) == QuestBook::Take::kNews);
	CHECK(out == Objective(0x10, 10, 1, 20));
	CHECK(book.Report(Stage(0x10, 20), out) == QuestBook::Take::kNews);
	CHECK(out.stage == 0);  // a stage's and an end's stage mean nothing: kept as 0
	CHECK(book.Report({ 0x10, QuestEntry::kCompleted, 0, 0, 7 }, out) == QuestBook::Take::kNews);
	CHECK(out == Completed(0x10));
}

TEST("quest book: the session has reached a stage when it holds it, a later one, or the quest's end")
{
	QuestBook  book;
	QuestEntry out;
	CHECK(!book.Reached(0x10, 20));
	book.Report(Stage(0x10, 10), out);
	CHECK(!book.Reached(0x10, 20));
	CHECK(book.Reached(0x10, 10));
	book.Report(Objective(0x10, 30, 1, 30), out);
	CHECK(!book.Reached(0x10, 20));  // an objective is no stage
	book.Report(Stage(0x10, 25), out);
	CHECK(book.Reached(0x10, 20));
	book.Report(Failed(0x11), out);
	CHECK(book.Reached(0x11, 500));
}

TEST("quest book: a quest ends once, the first way said")
{
	QuestBook  book;
	QuestEntry out;
	CHECK(book.Report(Completed(0x10), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Completed(0x10), out) == QuestBook::Take::kSame);
	CHECK(book.Report(Failed(0x10), out) == QuestBook::Take::kRefused);
	CHECK(book.Report(Failed(0x11), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Completed(0x11), out) == QuestBook::Take::kRefused);
	CHECK(book.Report({ 0x12, QuestEntry::kCompleted, 7, 3 }, out) == QuestBook::Take::kNews);
	CHECK(out == Completed(0x12));  // an end's index and value are 0
}

TEST("quest book: no quest and a runtime quest are refused; past its limits a new quest or entry is full, never the oldest dropped")
{
	QuestBook  book{ 2, 3 };
	QuestEntry out;
	CHECK(book.Report(Stage(0, 10), out) == QuestBook::Take::kRefused);
	CHECK(book.Report(Stage(0xFF000010, 10), out) == QuestBook::Take::kRefused);
	CHECK(book.Report(Stage(0x10, 1), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Stage(0x11, 1), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Stage(0x12, 1), out) == QuestBook::Take::kFull);
	CHECK(book.Report(Stage(0x10, 2), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Stage(0x10, 3), out) == QuestBook::Take::kNews);
	CHECK(book.Report(Stage(0x10, 4), out) == QuestBook::Take::kFull);
	CHECK(book.Report(Stage(0x10, 3), out) == QuestBook::Take::kSame);  // one it holds is still answered so
	CHECK(book.Quests() == 2);
	CHECK(book.Entries() == 4);
}

TEST("quest book: everything kept comes back quest by quest in the order first reported, each one's entries in order")
{
	QuestBook  book;
	QuestEntry out;
	book.Report(Stage(0x20, 10), out);
	book.Report(Stage(0x10, 0), out);
	book.Report(Stage(0x20, 20), out);
	book.Report(Objective(0x20, 1, 1), out);
	book.Report(Stage(0x10, 5), out);
	book.Report(Objective(0x20, 1, 3), out);  // merged in place
	const auto all = book.All();
	REQUIRE(all.size() == 5);
	CHECK(all[0] == Stage(0x20, 10));
	CHECK(all[1] == Stage(0x20, 20));
	CHECK(all[2] == Objective(0x20, 1, 3));
	CHECK(all[3] == Stage(0x10, 0));
	CHECK(all[4] == Stage(0x10, 5));
	REQUIRE(book.Find(Objective(0x20, 1, 0)));
	CHECK(book.Find(Objective(0x20, 1, 0))->value == 3);
	CHECK(!book.Find(Stage(0x20, 30)));
	book.Clear();
	CHECK(book.All().empty());
	CHECK(book.Quests() == 0);
	CHECK(book.Report(Stage(0x20, 10), out) == QuestBook::Take::kNews);
}

// ---- which quests are shared --------------------------------------------------------------------------------------

TEST("quest rules: a quest is shared by its record's type; misc and untyped are each game's own")
{
	using QuestRules::Class;
	using QuestRules::Classify;
	const auto typed = [](std::uint8_t a_type) { return Classify({ .type = a_type, .editorId = "SomeQuest" }); };
	for (const int type : { 1, 2, 3, 4, 5, 7, 8, 9, 10, 11 }) {
		CHECK(typed(static_cast<std::uint8_t>(type)) == Class::kShared);
	}
	CHECK(typed(6) == Class::kMisc);
	CHECK(typed(0) == Class::kUntyped);
	CHECK(typed(12) == Class::kUntyped);
	CHECK(Classify({ .type = 1, .editorId = "" }) == Class::kNoName);
	CHECK(QuestRules::ClassName(Class::kShared) == "shared");
	CHECK(QuestRules::ClassName(Class::kRadiant) == "radiant");
}

TEST("quest rules: the Story Manager's repeating jobs are radiant; one it starts once only is shared")
{
	using QuestRules::Class;
	using QuestRules::Classify;
	CHECK(Classify({ .type = 5, .event = 3, .flags = 0, .editorId = "CR01" }) == Class::kRadiant);  // a script event, again and again
	CHECK(Classify({ .type = 8, .event = 2, .flags = QuestRules::FLAG_RUN_ONCE, .editorId = "Side01" }) == Class::kShared);
	CHECK(Classify({ .type = 8, .event = -1, .flags = 0, .editorId = "Side02" }) == Class::kShared);
	CHECK(Classify({ .type = 6, .event = 3, .flags = 0, .editorId = "Favor01" }) == Class::kMisc);  // the tab first
}

TEST("quest rules: the Helgen opening and a character's own life are each player's own, by editor ID in any case")
{
	using QuestRules::Class;
	CHECK(QuestRules::Classify({ .type = 1, .editorId = "MQ101" }) == Class::kOwn);
	CHECK(QuestRules::Classify({ .type = 1, .editorId = "mq101" }) == Class::kOwn);
	CHECK(QuestRules::Classify({ .type = 1, .editorId = "MQ102" }) == Class::kShared);
	CHECK(QuestRules::Classify({ .type = 1, .editorId = "MQ1010" }) == Class::kShared);
	REQUIRE(QuestRules::OwnQuestOf("relationshipmarriage"));
	CHECK(!QuestRules::OwnQuestOf("MQ"));
	for (const auto& own : QuestRules::OWN_QUESTS) {
		CHECK(!own.why.empty());
	}
}

TEST("quest rules: a stage's row is found by editor ID in any case and stage")
{
	using QuestRules::StageRule;
	using QuestRules::StageVerdict;
	const std::array rows{ StageRule{ "DA01", 5, StageVerdict::kOwn }, StageRule{ "MQ305", 100, StageVerdict::kMoves } };
	CHECK(QuestRules::StageRuleOf(rows, "mq305", 100) == StageVerdict::kMoves);
	CHECK(!QuestRules::StageRuleOf(rows, "MQ305", 90));
	CHECK(!QuestRules::StageRuleOf(rows, "MQ30", 100));
	CHECK(QuestRules::StageRuleOf(rows, "DA01", 5) == StageVerdict::kOwn);
	CHECK(!QuestRules::StageRuleOf(rows, "DA02", 5));
	CHECK(!QuestRules::StageRuleOf(std::span<const StageRule>{}, "DA01", 5));
}

TEST("quest rules: the scan's stage rows are sorted as the lookup needs, each once, and name the stages that move the player")
{
	using QuestRules::StageVerdict;
	const auto& rows = QuestRules::STAGE_RULES;
	REQUIRE(rows.size() > 50);
	for (std::size_t i = 1; i < rows.size(); ++i) {
		CHECK(QuestRules::RuleLess(rows[i - 1], rows[i]));  // sorted and no row twice: the binary search finds each
	}
	for (const auto& row : rows) {
		CHECK(!row.editorId.empty());
		CHECK(QuestRules::StageRuleOf(rows, row.editorId, row.stage) == row.verdict);
	}
	// the ones the scan found by their own fragment's MoveTo of the player, through the quest's script, and by the
	// controls taken away; Sovngarde's return
	CHECK(QuestRules::StageRuleOf(rows, "TG05", 60) == StageVerdict::kMoves);
	CHECK(QuestRules::StageRuleOf(rows, "DA09", 200) == StageVerdict::kMoves);
	CHECK(QuestRules::StageRuleOf(rows, "MQ302", 60) == StageVerdict::kMoves);
	CHECK(QuestRules::StageRuleOf(rows, "mq305", 260) == StageVerdict::kMoves);
	// not rows: a stage that ends another quest that runs (MQ102 10 sets MQ101 900, never MQ101's start again), and one
	// that moves the player's housecarl (Alias_PlayerHousecarl is not the player)
	CHECK(!QuestRules::StageRuleOf(rows, "MQ102", 10));
	CHECK(!QuestRules::StageRuleOf(rows, "MQ104", 160));
	CHECK(QuestRules::CompareName("mg06", "MG07") < 0);
	CHECK(QuestRules::CompareName("MQ_A", "MQZ") > 0);  // as the scan sorts: upper-cased, '_' after the letters
}

TEST("quest rules: the scan's rows cover the class: the screen held, controls, the player frozen, chargen; the worst effect is said")
{
	using QuestRules::StageEffect;
	const auto effect = [](std::string_view a_edid, std::uint16_t a_stage) {
		const auto row = QuestRules::StageRowOf(QuestRules::STAGE_RULES, a_edid, a_stage);
		return row ? std::optional<StageEffect>{ row->effect } : std::nullopt;
	};
	CHECK(effect("DLC1VQ06", 61) == StageEffect::kScreen);   // an image space cross-faded and held (fade to white)
	CHECK(effect("MG02", 50) == StageEffect::kScreen);       // ... popped to and held
	CHECK(effect("MQ201", 95) == StageEffect::kControls);    // DisablePlayerControls
	CHECK(effect("MQ101", 98) == StageEffect::kFrozen);      // SetRestrained on the player
	CHECK(effect("DLC2MQ02", 50) == StageEffect::kChargen);  // SetInChargen
	CHECK(effect("MQ305", 260) == StageEffect::kMove);       // fades and moves: the move is said
	CHECK(effect("TG05", 60) == StageEffect::kMove);
	CHECK(QuestRules::EffectName(StageEffect::kScreen) == "fades or holds the screen black");
}

TEST("quest rules: a stage of a quest not started here is judged by the quest's start-up row too (setstage runs it)")
{
	using QuestRules::StageRule;
	using QuestRules::StageVerdict;
	using QuestRules::StageEffect;
	const std::array rows{ StageRule{ "MG08", 0, StageVerdict::kMoves, StageEffect::kMove, true }, StageRule{ "MG08", 30, StageVerdict::kMoves, StageEffect::kScreen },
		StageRule{ "MQ305", 260, StageVerdict::kMoves } };
	REQUIRE(QuestRules::StartUpRowOf(rows, "mg08"));
	CHECK(QuestRules::StartUpRowOf(rows, "mg08")->stage == 0);
	CHECK(!QuestRules::StartUpRowOf(rows, "MQ305"));
	CHECK(!QuestRules::StartUpRowOf(rows, "MG07"));
	// MG08 10 has no row of its own: while MG08 is not started here, making it would run stage 0 (a move)
	const auto* row = QuestRules::StageRowHere(rows, "MG08", 10, false);
	REQUIRE(row);
	CHECK(row->stage == 0);
	CHECK(!QuestRules::StageRowHere(rows, "MG08", 10, true));
	// a stage's own row comes first
	REQUIRE(QuestRules::StageRowHere(rows, "MG08", 30, false));
	CHECK(QuestRules::StageRowHere(rows, "MG08", 30, false)->effect == StageEffect::kScreen);
	CHECK(!QuestRules::StageRowHere(rows, "MQ305", 250, false));
	// and the scan's own rows say MG08's start-up stage
	REQUIRE(QuestRules::StartUpRowOf(QuestRules::STAGE_RULES, "MG08"));
	CHECK(QuestRules::StartUpRowOf(QuestRules::STAGE_RULES, "MG08")->stage == 0);
	CHECK(!QuestRules::StartUpRowOf(QuestRules::STAGE_RULES, "MQ305"));
}

TEST("quest rules: a stage that moves the player waits for the friend who did it, and is not made from the session's past")
{
	using QuestRules::Verdict;
	QuestRules::Here here{ .exists = true, .cls = QuestRules::Class::kShared, .started = true, .current = 250 };
	here.rule = QuestRules::StageRuleOf(QuestRules::STAGE_RULES, "MQ305", 260);
	REQUIRE(here.rule);
	CHECK(QuestRules::Judge(Stage(0x46EF2, 260), 7, here) == Verdict::kFar);
	CHECK(QuestRules::Judge(Stage(0x46EF2, 260), 0, here) == Verdict::kFarPast);
	here.reporterNear = true;
	CHECK(QuestRules::Judge(Stage(0x46EF2, 260), 7, here) == Verdict::kMake);
	here.rule = QuestRules::StageRuleOf(QuestRules::STAGE_RULES, "MQ305", 200);
	here.reporterNear = false;
	CHECK(QuestRules::Judge(Stage(0x46EF2, 200), 0, here) == Verdict::kPassed);
}

TEST("quest rules: the done bit is trusted only when no quest never run reads a stage done and running quests read their current stage done")
{
	using QuestRules::BitSample;
	std::vector<BitSample> good;
	for (int i = 0; i < 20; ++i) {
		good.push_back({ .running = true, .current = 10, .currentDone = true, .doneCount = 2 });
		good.push_back({ .neverRun = true, .doneCount = 0 });
	}
	good.push_back({ .running = true, .current = 20, .currentDone = false, .doneCount = 1 });  // one just set, not done yet
	CHECK(QuestRules::JudgeBit(good).trusted);
	CHECK(QuestRules::JudgeBit(good).currentChecked == 21);

	auto neverRun = good;
	neverRun.push_back({ .neverRun = true, .doneCount = 1 });
	const auto v = QuestRules::JudgeBit(neverRun);
	CHECK(!v.trusted);
	CHECK(v.neverRunWithDone == 1);

	std::vector<BitSample> notDone;
	for (int i = 0; i < 10; ++i) {
		notDone.push_back({ .running = true, .current = 10, .currentDone = i < 5, .doneCount = 1 });
	}
	CHECK(!QuestRules::JudgeBit(notDone).trusted);
	CHECK(QuestRules::JudgeBit({}).trusted);  // a new game: nothing against it
}

// ---- what a game reports ------------------------------------------------------------------------------------------

TEST("quest view: a change is reported once, not with every poll, and not at all once the session has it")
{
	QuestRules::View view;
	CHECK(view.Offer(Stage(0x10, 10)));
	CHECK(!view.Offer(Stage(0x10, 10)));  // out, not answered yet
	CHECK(view.Outstanding() == 1);
	CHECK(view.Heard(Stage(0x10, 10), true));  // the session's word, from our report: answers it
	CHECK(view.Outstanding() == 0);
	CHECK(!view.Offer(Stage(0x10, 10)));
	CHECK(!view.Heard(Stage(0x10, 10), false));  // nothing new

	CHECK(!view.Offer(Objective(0x10, 1, 0)));  // dormant says nothing
	CHECK(view.Offer(Objective(0x10, 1, 1, 10)));
	CHECK(!view.Offer(Objective(0x10, 1, 1, 10)));
	CHECK(view.Offer(Objective(0x10, 1, 3, 10)));  // further on than the one out
	CHECK(view.Outstanding() == 2);
	CHECK(view.Heard(Objective(0x10, 1, 3, 10), true));  // answers the oldest only: the other is answered on its own
	CHECK(view.Outstanding() == 1);
	CHECK(view.Answered({ 0x10, QuestEntry::kObjective, 1, QuestEntry::kSame }));
	CHECK(view.Outstanding() == 0);
	CHECK(!view.Offer(Objective(0x10, 1, 1, 10)));  // behind the session's word
	CHECK(!view.Offer(Objective(0x10, 1, 3, 10)));
	CHECK(view.Offer(Objective(0x10, 1, 3, 20)));  // the quest moved on, the objective still shown: said again

	CHECK(view.Offer(Stage(0x11, 5)));
	CHECK(!view.Heard(Stage(0x10, 10), false));  // another player's word does not answer ours
	CHECK(view.Heard(Stage(0x11, 5), false));
	CHECK(view.Outstanding() == 2);
	view.Clear();  // a load: the session's word forgotten, ours still out
	CHECK(view.Quests() == 0);
	CHECK(view.Outstanding() == 2);
	CHECK(view.Offer(Stage(0x10, 10)));  // the session's word is asked for again; ours may go meanwhile
	view.ClearAll();
	CHECK(view.Outstanding() == 0);
}

TEST("quest view: what the session would not take stays this game's own and is not offered again; what it had is learnt; busy goes again")
{
	QuestRules::View view;
	// Game A failed the quest first; this game completed it its own way: refused once, never again every poll.
	REQUIRE(view.Offer(Completed(0x10)));
	const auto refused = view.Answered({ 0x10, QuestEntry::kCompleted, 0, QuestEntry::kRefused });
	REQUIRE(refused);
	CHECK(*refused == Completed(0x10));
	CHECK(view.Outstanding() == 0);
	CHECK(view.Kept() == 1);
	for (int poll = 0; poll < 5; ++poll) {
		CHECK(!view.Offer(Completed(0x10)));
	}
	// The book full: an objective kept as ours; one further on is offered once more.
	REQUIRE(view.Offer(Objective(0x20, 1, 1, 10)));
	view.Answered({ 0x20, QuestEntry::kObjective, 1, QuestEntry::kFull });
	CHECK(!view.Offer(Objective(0x20, 1, 1, 10)));
	CHECK(view.Offer(Objective(0x20, 1, 3, 10)));
	// The session had it already: learnt as the session's, not offered again.
	REQUIRE(view.Offer(Stage(0x30, 5)));
	view.Answered({ 0x30, QuestEntry::kStage, 5, QuestEntry::kSame });
	CHECK(view.Known(Stage(0x30, 5)));
	CHECK(!view.Offer(Stage(0x30, 5)));
	// Busy: the report comes back to the caller to send again, and is no longer out.
	REQUIRE(view.Offer(Stage(0x31, 5)));
	const auto busy = view.Answered({ 0x31, QuestEntry::kStage, 5, QuestEntry::kBusy });
	REQUIRE(busy);
	CHECK(view.Offer(Stage(0x31, 5)));
	// An answer to nothing out (the host's snapshot) is nothing of ours; MarkOut makes a resend of it out.
	CHECK(!view.Answered({ 0x40, QuestEntry::kStage, 5, QuestEntry::kBusy }));
	view.MarkOut(Stage(0x40, 5));
	CHECK(!view.Offer(Stage(0x40, 5)));
	// A new session's book (the host's reset): what it refused may be taken now.
	view.Clear();
	CHECK(view.Kept() == 0);
	CHECK(view.Offer(Completed(0x10)));
}

TEST("quest view: what this game held when the session's word was new to it (a welcome, the host's reset, a load) is its base, never reported")
{
	// Both players reached stage 20 of 0x10: objective 10 completed, 20 shown, and this friend finished quest 0x11. The
	// host loads a save from before stage 20: the friend's view is cleared and takes its base.
	QuestRules::View view;
	const std::vector<QuestEntry> held{ Objective(0x10, 10, 3, 20), Objective(0x10, 20, 1, 20), Completed(0x11) };
	view.Rebase(held);
	CHECK(view.Based() == 3);
	for (const auto& e : held) {
		CHECK(!view.Offer(e));  // its own past: the host's world never gets it from a poll
	}
	CHECK(!view.Offer(Objective(0x10, 10, 1, 10)));  // behind its base
	// What moves on from it is reported.
	CHECK(view.Offer(Objective(0x10, 20, 3, 20)));
	CHECK(view.Offer(Objective(0x10, 30, 1, 20)));
	CHECK(view.Offer(Stage(0x10, 30)));
	// A new base replaces the old one; ClearAll forgets it.
	view.Rebase(std::vector<QuestEntry>{});
	CHECK(view.Based() == 0);
	view.Rebase(held);
	view.ClearAll();
	CHECK(view.Based() == 0);
}

TEST("quest apart: a friend's quest further on than the session's stays its own until the session gets there; the host's never does")
{
	using QuestRules::Apart;
	QuestRules::View view;
	Apart            apart;
	const std::vector<std::pair<std::uint32_t, std::uint16_t>> at{ { 0x10, 20 }, { 0x11, 5 }, { 0x12, 0 } };
	apart.Rebase(at, T0);
	CHECK(apart.StageOf(0x10) == 20);
	CHECK(apart.StageOf(0x12) == 0);  // at stage 0: not started as far as stages go
	CHECK(apart.Judge(0x13, view, T0) == Apart::Say::kReport);  // a quest not started here at the base
	CHECK(apart.Judge(0x10, view, T0 + 1s) == Apart::Say::kWait);  // the session's word may still be coming
	// The host's world comes in: 0x11 at stage 5 (as here), 0x10 only at stage 10 (the host loaded an older save).
	view.Heard(Stage(0x11, 5), false);
	view.Heard(Stage(0x10, 10), false);
	CHECK(apart.Judge(0x11, view, T0 + 1s) == Apart::Say::kReport);
	CHECK(apart.StageOf(0x11) == 0);  // with the session from now on
	CHECK(apart.Judge(0x10, view, T0 + QuestRules::APART_WAIT - 1ms) == Apart::Say::kWait);
	CHECK(apart.Judge(0x10, view, T0 + QuestRules::APART_WAIT) == Apart::Say::kOwn);
	CHECK(apart.Judge(0x10, view, T0 + QuestRules::APART_WAIT + 1s) == Apart::Say::kOwn);
	CHECK(apart.Quests() == 1);  // said once
	CHECK(apart.LastQuest() == 0x10);
	// The host plays on to stage 20 (or past it): the friend's quest is with the session again.
	view.Heard(Stage(0x10, 25), false);
	CHECK(apart.Judge(0x10, view, T0 + 1min) == Apart::Say::kReport);
	// The host's game takes no quest as apart (its base is empty).
	apart.Rebase({}, T0);
	CHECK(apart.Judge(0x10, QuestRules::View{}, T0 + 1min) == Apart::Say::kReport);
}

// ---- making the session's word here -------------------------------------------------------------------------------

TEST("quest judge: made only where this game shares the quest and is behind; a moving stage waits for its friend near")
{
	using QuestRules::Here;
	using QuestRules::Judge;
	using QuestRules::StageVerdict;
	using QuestRules::Verdict;
	const Here shared{ .exists = true, .cls = QuestRules::Class::kShared };
	CHECK(Judge(Stage(1, 10), 2, shared) == Verdict::kMake);
	CHECK(Judge(Stage(1, 10), 2, Here{}) == Verdict::kMissing);
	CHECK(Judge(Stage(1, 10), 2, Here{ .exists = true, .cls = QuestRules::Class::kRadiant }) == Verdict::kNotHere);
	CHECK(Judge(Stage(1, 10), 2, Here{ .exists = true, .cls = QuestRules::Class::kShared, .has = true }) == Verdict::kHas);
	auto moving = shared;
	moving.rule = StageVerdict::kMoves;
	CHECK(Judge(Stage(1, 10), 2, moving) == Verdict::kFar);
	CHECK(Judge(Stage(1, 10), 0, moving) == Verdict::kFarPast);  // the session's past, no friend doing it now
	moving.reporterNear = true;
	CHECK(Judge(Stage(1, 10), 2, moving) == Verdict::kMake);
	auto own = shared;
	own.rule = StageVerdict::kOwn;
	CHECK(Judge(Stage(1, 10), 2, own) == Verdict::kOwn);
	CHECK(Judge(Objective(1, 10, 1), 2, moving) == Verdict::kNotStarted);  // rows are stages'; an objective needs its quest started
	auto running = moving;
	running.started = true;
	CHECK(Judge(Objective(1, 10, 1), 2, running) == Verdict::kMake);
	CHECK(Judge(Completed(1), 2, shared) == Verdict::kNotStarted);
	CHECK(Judge(Completed(1), 2, running) == Verdict::kMake);
	// A stage below the current one of a quest started here: passed, never set again (its fragment would run twice).
	const Here at30{ .exists = true, .cls = QuestRules::Class::kShared, .started = true, .current = 30 };
	CHECK(Judge(Stage(1, 10), 2, at30) == Verdict::kPassed);
	CHECK(Judge(Stage(1, 40), 2, at30) == Verdict::kMake);
	CHECK(Judge(Stage(1, 10), 0, at30) == Verdict::kPassed);
	CHECK(QuestRules::VerdictName(Verdict::kPassed) == "passed");
}

namespace
{
	// A game for the Maker: which stages are done here, which quests it has and shares, the stage rules, who is near.
	// A quest's current stage is the highest done (or `at`), and it is started once it has one (or is in `started`).
	struct FakeGame
	{
		std::set<std::pair<std::uint32_t, std::uint16_t>> done;
		std::map<std::uint32_t, std::uint8_t>             objectives;  // index -> state, for quest 0x10
		std::set<std::uint32_t>                           missing;
		std::set<std::uint32_t>                           radiant;
		std::set<std::pair<std::uint32_t, std::uint16_t>> moves;
		std::set<std::uint32_t>                           near;
		std::map<std::uint32_t, std::uint16_t>            at;
		std::set<std::uint32_t>                           started;
		std::set<std::uint32_t>                           ended;  // quests the session's word has ended
		bool                                              completed = false;

		std::uint16_t Current(std::uint32_t a_quest) const
		{
			if (const auto it = at.find(a_quest); it != at.end()) {
				return it->second;
			}
			std::uint16_t current = 0;
			for (const auto& [quest, stage] : done) {
				if (quest == a_quest) {
					current = (std::max)(current, stage);
				}
			}
			return current;
		}

		QuestRules::Here Look(const QuestEntry& a_e, std::uint32_t a_reporter) const
		{
			QuestRules::Here here;
			here.exists = !missing.contains(a_e.questId);
			here.cls = radiant.contains(a_e.questId) ? QuestRules::Class::kRadiant : QuestRules::Class::kShared;
			here.current = Current(a_e.questId);
			here.started = here.current != 0 || started.contains(a_e.questId);
			here.sessionEnded = ended.contains(a_e.questId);
			if (a_e.kind == QuestEntry::kStage) {
				here.has = done.contains({ a_e.questId, a_e.index }) || (a_e.index == here.current && here.current != 0);
				if (moves.contains({ a_e.questId, a_e.index })) {
					here.rule = QuestRules::StageVerdict::kMoves;
				}
			} else if (a_e.kind == QuestEntry::kObjective) {
				const auto it = objectives.find(a_e.index);
				here.has = !QuestRules::ObjectiveToMake(a_e, it == objectives.end() ? 0 : it->second, here.current).Any();
			} else {
				here.has = completed;
			}
			here.reporterNear = near.contains(a_reporter);
			return here;
		}
		QuestRules::Maker::Look Looker() const
		{
			return [this](const QuestEntry& a_e, std::uint32_t a_r) { return Look(a_e, a_r); };
		}

		// Makes what the maker says, as the game would: a stage done (and current), an objective's state, the end.
		void Made(const QuestEntry& a_e)
		{
			if (a_e.kind == QuestEntry::kStage) {
				done.insert({ a_e.questId, a_e.index });
				at.erase(a_e.questId);
			} else if (a_e.kind == QuestEntry::kObjective) {
				const auto make = QuestRules::ObjectiveToMake(a_e, objectives[a_e.index], Current(a_e.questId));
				objectives[a_e.index] = QuestBook::State(QuestBook::Shown(objectives[a_e.index]) || make.show,
					QuestBook::End(objectives[a_e.index]) != 0 ? QuestBook::End(objectives[a_e.index]) : make.end);
			} else if (a_e.kind == QuestEntry::kCompleted) {
				completed = true;
			}
		}
	};
}

TEST("quest maker: one stage of a quest at a time, in order, the next once the last reads done; other quests go between")
{
	QuestRules::Maker maker;
	FakeGame          game;
	auto              now = T0;
	maker.Add(Stage(0x10, 10), 2, now);
	maker.Add(Stage(0x10, 20), 2, now);
	maker.Add(Stage(0x20, 5), 2, now);
	auto step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kMake);
	CHECK(step->want.entry == Stage(0x10, 10));
	CHECK(!maker.Next(now, game.Looker()));  // the pace: one thing at a time
	now += QuestRules::MAKE_EVERY;
	step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x20, 5));  // 0x10's next waits for its 10 to read done
	now += QuestRules::MAKE_EVERY;
	CHECK(!maker.Next(now, game.Looker()));
	CHECK(maker.InFlight() == 2);
	maker.Done(0x10, 10, now);
	game.done.insert({ 0x10, 10 });
	step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x10, 20));
	CHECK(maker.Size() == 0);
}

TEST("quest maker: a stage this game has already, or a quest it lacks or does not share, is taken out and said, without the pace")
{
	QuestRules::Maker maker;
	FakeGame          game;
	game.done.insert({ 0x10, 10 });
	game.missing.insert(0x30);
	game.radiant.insert(0x40);
	maker.Add(Stage(0x10, 10), 2, T0);
	maker.Add(Stage(0x30, 1), 2, T0);
	maker.Add(Stage(0x40, 1), 2, T0);
	maker.Add(Stage(0x10, 20), 2, T0);
	std::vector<QuestRules::Verdict> verdicts;
	while (const auto step = maker.Next(T0, game.Looker())) {
		verdicts.push_back(step->verdict);
	}
	using V = QuestRules::Verdict;
	CHECK((verdicts == std::vector<V>{ V::kHas, V::kMissing, V::kNotHere, V::kMake }));
}

TEST("quest maker: a stage never read done is given up after STAGE_WAIT and the quest goes on, counted stuck")
{
	QuestRules::Maker maker;
	FakeGame          game;
	maker.Add(Stage(0x10, 10), 2, T0);
	maker.Add(Stage(0x10, 20), 2, T0);
	REQUIRE(maker.Next(T0, game.Looker()));
	CHECK(!maker.Next(T0 + QuestRules::STAGE_WAIT - 1ms, game.Looker()));
	const auto step = maker.Next(T0 + QuestRules::STAGE_WAIT, game.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x10, 20));
	CHECK(maker.Stuck() == 1);
	REQUIRE(maker.LastStuck());
	CHECK(*maker.LastStuck() == Stage(0x10, 10));
}

TEST("quest maker: a quest's objectives and end wait for its stages and SETTLE, then are made if still behind")
{
	QuestRules::Maker maker;
	FakeGame          game;
	auto              now = T0;
	maker.Add(Stage(0x10, 10), 2, now);
	maker.Add(Objective(0x10, 1, 1, 10), 2, now);
	maker.Add(Objective(0x10, 2, 1, 10), 2, now);
	maker.Add(Completed(0x10), 2, now);
	REQUIRE(maker.Next(now, game.Looker())->want.entry == Stage(0x10, 10));
	now += 1s;
	maker.Done(0x10, 10, now);
	game.done.insert({ 0x10, 10 });
	game.objectives[1] = 1;  // the stage's own fragment showed objective 1
	CHECK(!maker.Next(now + QuestRules::SETTLE - 1ms, game.Looker()));
	now += QuestRules::SETTLE;
	auto step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kHas);  // objective 1: the fragment did it
	step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kMake);
	CHECK(step->want.entry == Objective(0x10, 2, 1, 10));
	now += QuestRules::MAKE_EVERY;
	step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Completed(0x10));
}

TEST("quest maker: a moving stage waits for the friend who did it to be near, holding its quest's later stages, not other quests")
{
	QuestRules::Maker maker;
	FakeGame          game;
	game.moves.insert({ 0x10, 50 });
	auto now = T0;
	maker.Add(Stage(0x10, 50), 2, now);
	maker.Add(Stage(0x10, 60), 2, now);
	maker.Add(Stage(0x20, 1), 2, now);
	auto step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x20, 1));
	now += QuestRules::MAKE_EVERY;
	CHECK(!maker.Next(now, game.Looker()));
	CHECK(maker.Held(game.Looker(), now) == 1);
	game.near.insert(2);
	step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x10, 50));

	// The same stage from the session's past (a join, a load: nobody did it now) is not made at all.
	QuestRules::Maker past;
	FakeGame          far;
	far.moves.insert({ 0x10, 50 });
	past.Add(Stage(0x10, 50), 0, T0);
	past.Add(Stage(0x10, 60), 0, T0);
	step = past.Next(T0, far.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kFarPast);
	CHECK(!past.Next(T0, far.Looker()));  // 60 would start the quest: the past may still say it ended (PAST_SETTLE)
	step = past.Next(T0 + QuestRules::PAST_SETTLE, far.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x10, 60));
}

TEST("quest maker: a moving stage's nearness is the friend's as its word came, not where the stage has since taken them")
{
	// The friend was beside ours when they set 50 (near at the step); by the time it is judged here the stage has moved
	// their stand-in to its destination (not near now). It is made: the friends who were with them go with them.
	QuestRules::Maker maker;
	FakeGame          game;
	game.moves.insert({ 0x10, 50 });
	maker.Add(Stage(0x10, 50), 2, T0, true);
	maker.Add(Stage(0x10, 60), 2, T0, true);
	auto step = maker.Next(T0 + 1s, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kMake);
	CHECK(step->want.entry == Stage(0x10, 50));
	CHECK(maker.TakeHeld().empty());

	// Not near at the step, near now (the friend walked over): made too.
	QuestRules::Maker later;
	FakeGame          walked;
	walked.moves.insert({ 0x10, 50 });
	later.Add(Stage(0x10, 50), 2, T0, false);
	CHECK(!later.Next(T0, walked.Looker()));
	walked.near.insert(2);
	step = later.Next(T0 + 10s, walked.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kMake);

	// A newer word on the same stage from another friend takes that friend's nearness.
	QuestRules::Maker other;
	FakeGame          none;
	none.moves.insert({ 0x10, 50 });
	other.Add(Stage(0x10, 50), 2, T0, true);
	other.Add(Stage(0x10, 50), 3, T0, false);
	CHECK(!other.Next(T0, none.Looker()));
	CHECK(other.Held(none.Looker(), T0) == 1);
}

TEST("quest maker: a stage held for a far friend is given up after FAR_WAIT, or when they leave, and its quest goes on")
{
	QuestRules::Maker maker;
	FakeGame          game;
	game.moves.insert({ 0x10, 50 });
	game.moves.insert({ 0x20, 7 });
	auto now = T0;
	maker.Add(Stage(0x10, 50), 2, now);
	maker.Add(Stage(0x10, 60), 2, now);
	maker.Add(Stage(0x20, 7), 3, now);
	maker.Add(Stage(0x20, 8), 3, now);
	CHECK(!maker.Next(now, game.Looker()));
	const auto held = maker.TakeHeld();
	REQUIRE(held.size() == 2);  // each said once
	CHECK(held[0].entry == Stage(0x10, 50));
	CHECK(held[1].entry == Stage(0x20, 7));
	CHECK(!maker.Next(now + 1s, game.Looker()));
	CHECK(maker.TakeHeld().empty());

	// player 3 leaves: 0x20's 7 is given up (not made), its 8 goes on
	maker.Left(3);
	now += 1s;
	auto step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kFarPast);
	CHECK(step->release == QuestRules::Maker::Release::kLeft);
	CHECK(step->want.entry == Stage(0x20, 7));
	step = maker.Next(now, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kMake);
	CHECK(step->want.entry == Stage(0x20, 8));
	maker.Done(0x20, 8, now);

	// player 2 stays far: 0x10's 50 is held until FAR_WAIT, then given up, and its 60 is made
	CHECK(!maker.Next(T0 + QuestRules::FAR_WAIT - 1ms, game.Looker()));
	step = maker.Next(T0 + QuestRules::FAR_WAIT, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kFarPast);
	CHECK(step->release == QuestRules::Maker::Release::kWaited);
	CHECK(step->want.entry == Stage(0x10, 50));
	step = maker.Next(T0 + QuestRules::FAR_WAIT, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kMake);
	CHECK(step->want.entry == Stage(0x10, 60));
	CHECK(maker.Size() == 0);

	// Left for a friend with nothing held, or for player 0, changes nothing.
	QuestRules::Maker quiet;
	quiet.Add(Stage(0x30, 1), 2, T0);
	quiet.Left(0);
	quiet.Left(9);
	step = quiet.Next(T0, game.Looker());
	REQUIRE(step);
	CHECK(step->verdict == QuestRules::Verdict::kMake);
}

namespace
{
	// Runs the maker until it has nothing more due, a stage's fragment run by a_fragment as the game would; returns
	// every step's verdict with its entry, in order.
	std::vector<std::pair<QuestRules::Verdict, QuestEntry>> RunMaker(QuestRules::Maker& a_maker, FakeGame& a_game, Clock::time_point& a_now,
		const std::function<void(const QuestEntry&, FakeGame&)>& a_fragment = {})
	{
		std::vector<std::pair<QuestRules::Verdict, QuestEntry>> out;
		for (int tick = 0; tick < 400 && a_maker.Size() != 0; ++tick) {
			a_now += 100ms;
			while (const auto step = a_maker.Next(a_now, a_game.Looker())) {
				out.emplace_back(step->verdict, step->want.entry);
				if (step->verdict != QuestRules::Verdict::kMake) {
					continue;
				}
				a_game.Made(step->want.entry);
				if (step->want.entry.kind == QuestEntry::kStage) {
					if (a_fragment) {
						a_fragment(step->want.entry, a_game);
					}
					a_maker.Done(step->want.entry.questId, step->want.entry.index, a_now);
				}
			}
		}
		return out;
	}
}

TEST("quest maker: an objective a later stage's own script withdrew is not shown again, catching up or after a load")
{
	// Stage 10 shows objective 10; stage 15's fragment withdraws it (SetObjectiveDisplayed(10, false)) and shows 15. The
	// session's book keeps 10 as shown at stage 10, and 15 as shown at 15.
	const auto fragments = [](const QuestEntry& a_stage, FakeGame& a_game) {
		if (a_stage.index == 10) {
			a_game.objectives[10] = 1;
		} else if (a_stage.index == 15) {
			a_game.objectives[10] = 0;
			a_game.objectives[15] = 1;
		}
	};
	const std::vector<QuestEntry> book{ Stage(0x10, 10), Stage(0x10, 15), Objective(0x10, 10, 1, 10), Objective(0x10, 15, 1, 15) };
	using V = QuestRules::Verdict;

	// A game catching up makes both stages; their fragments do the objectives; objective 10 stays withdrawn.
	QuestRules::Maker maker;
	FakeGame          game;
	auto              now = T0;
	for (const auto& e : book) {
		maker.Add(e, 0, now);
	}
	auto steps = RunMaker(maker, game, now, fragments);
	for (const auto& [verdict, entry] : steps) {
		CHECK(!(verdict == V::kMake && entry.kind == QuestEntry::kObjective));
	}
	CHECK(game.objectives[10] == 0);
	CHECK(game.objectives[15] == 1);

	// The same game loads (or rejoins) and hears the whole book again: nothing is made, objective 10 stays withdrawn.
	for (const auto& e : book) {
		maker.Add(e, 0, now);
	}
	steps = RunMaker(maker, game, now, fragments);
	REQUIRE(steps.size() == 4);
	for (const auto& [verdict, entry] : steps) {
		CHECK(verdict != V::kMake);
	}
	CHECK(game.objectives[10] == 0);

	// An objective a script (not a stage) showed at the stage this game is at is still made: the class is withdrawn ones.
	maker.Add(Objective(0x10, 16, 1, 15), 2, now);
	steps = RunMaker(maker, game, now);
	REQUIRE(steps.size() == 1);
	CHECK(steps[0].first == V::kMake);
	CHECK(game.objectives[16] == 1);
	// ... and its end, whatever stage it was read at.
	maker.Add(Objective(0x10, 15, 2, 12), 2, now);
	steps = RunMaker(maker, game, now);
	REQUIRE(steps.size() == 1);
	CHECK(steps[0].first == V::kMake);
	CHECK(game.objectives[15] == 3);
}

TEST("quest maker: after a load, stages below the game's current one are never set again, whatever the done bit reads")
{
	// The game loaded a save at stage 30 of 0x10 (the done bit not trusted, no stage seen set since the load) and an
	// allow-repeat quest 0x11 at 5. The session's book holds 10, 20, 30 and 40 of 0x10 and 1, 5 of 0x11.
	QuestRules::Maker maker;
	FakeGame          game;
	game.at[0x10] = 30;
	game.at[0x11] = 5;
	auto now = T0;
	for (const auto stage : { 10, 20, 30, 40 }) {
		maker.Add(Stage(0x10, static_cast<std::uint16_t>(stage)), 0, now);
	}
	maker.Add(Stage(0x11, 1), 0, now);
	maker.Add(Stage(0x11, 5), 0, now);
	const auto steps = RunMaker(maker, game, now);
	using V = QuestRules::Verdict;
	const std::vector<std::pair<V, QuestEntry>> want{ { V::kPassed, Stage(0x10, 10) }, { V::kPassed, Stage(0x10, 20) }, { V::kHas, Stage(0x10, 30) },
		{ V::kMake, Stage(0x10, 40) }, { V::kPassed, Stage(0x11, 1) }, { V::kHas, Stage(0x11, 5) } };
	CHECK(steps == want);
}

TEST("quest maker: a newer word on a waiting objective joins it; past MAX_WANTS a new one is dropped; Clear forgets all")
{
	QuestRules::Maker maker;
	maker.Add(Objective(0x10, 1, 1), 2, T0);
	maker.Add(Objective(0x10, 1, 2), 3, T0);
	REQUIRE(maker.Size() == 1);
	CHECK(maker.Wants().front().entry.value == 3);
	CHECK(maker.Wants().front().reporter == 3);
	for (std::size_t i = 1; i < QuestRules::MAX_WANTS; ++i) {
		REQUIRE(maker.Add(Stage(0x20, static_cast<std::uint16_t>(i)), 2, T0));
	}
	CHECK(!maker.Add(Stage(0x30, 1), 2, T0));
	CHECK(maker.Dropped() == 1);
	maker.Clear();
	CHECK(maker.Size() == 0);
	CHECK(maker.InFlight() == 0);
}

TEST("quest guard: a stage made here that moved our player is said once; a still player, or a jump after the watch, is not")
{
	QuestRules::MoveGuard guard;
	const QuestRules::Place riverwood{ 0x3C, 1000, 2000, 0 };
	guard.Made(Stage(0x10, 30), riverwood, T0);
	CHECK(!guard.Check({ 0x3C, 1500, 2100, 0 }, T0 + 1s));  // walked a little
	const auto moved = guard.Check({ 0x3C, 9000, 2000, 0 }, T0 + 2s);
	REQUIRE(moved);
	CHECK(*moved == Stage(0x10, 30));
	CHECK(!guard.Check({ 0x1A26F, 0, 0, 0 }, T0 + 2s));  // said once
	CHECK(guard.Moves() == 1);

	guard.Made(Stage(0x10, 40), riverwood, T0);
	CHECK(guard.Check({ 0x1A26F, 1000, 2000, 0 }, T0 + 1s));  // another space, however near
	guard.Made(Stage(0x10, 50), riverwood, T0);
	CHECK(!guard.Check(riverwood, T0 + QuestRules::MOVE_WATCH));
	CHECK(!guard.Check({ 0x1A26F, 0, 0, 0 }, T0 + QuestRules::MOVE_WATCH + 1s));  // the watch was over
	CHECK(guard.Moves() == 2);
}

TEST("quest snapshot: quest by quest, stages ascending and once each, then the objectives shown, then the end")
{
	std::vector<QuestRules::QuestRead> reads;
	reads.push_back({ .questId = 0x20, .current = 30, .done = { 30, 10, 10, 20 }, .objectives = { { 2, 0 }, { 1, 3 } }, .completed = true });
	reads.push_back({ .questId = 0x10, .done = { 5 }, .objectives = {}, .failed = true });
	reads.push_back({ .questId = 0xFF000001, .done = { 5 } });  // runtime: never said
	const auto out = QuestRules::Snapshot(reads);
	const std::vector<QuestEntry> want{ Stage(0x10, 5), Failed(0x10), Stage(0x20, 10), Stage(0x20, 20), Stage(0x20, 30), Objective(0x20, 1, 3, 30), Completed(0x20) };
	CHECK(out == want);
}

// ---- the server ---------------------------------------------------------------------------------------------------

TEST("server: a quest's news goes to every game, its reporter's too; what it had goes back to the reporter alone, answered")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	f.fake->Deliver(2, Report({ Stage(0x0001F0A1, 10), Objective(0x0001F0A1, 10, 1) }));
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 2 } }) {
		const auto heard = EntriesIn(f.fake->TakeThrough(peer, Type(MT::kRemoteQuests)));
		REQUIRE(heard.size() == 2);
		CHECK(heard[0].first == 2);
		CHECK(heard[0].second == Stage(0x0001F0A1, 10));
		CHECK(heard[1].second == Objective(0x0001F0A1, 10, 1));
	}
	// Ada's game did the same stage at the same moment, and one new: the same is answered to Ada alone.
	f.fake->Deliver(1, Report({ Stage(0x0001F0A1, 10), Stage(0x0001F0A1, 20), Stage(0xFF000001, 1) }));
	REQUIRE(f.fake->Settle());
	const auto toA = f.fake->Take(1);
	const auto newsA = EntriesIn(toA);
	REQUIRE(newsA.size() == 1);
	CHECK(newsA[0].second == Stage(0x0001F0A1, 20));
	CHECK(newsA[0].first == 1);
	const auto answers = EntriesIn(toA, MT::kQuestAnswer);
	REQUIRE(answers.size() == 2);
	CHECK(answers[0].second.questId == 0x0001F0A1);
	CHECK(answers[0].second.value == QuestEntry::kSame);
	CHECK(answers[1].second.value == QuestEntry::kRefused);
	const auto toB = f.fake->Take(2);
	CHECK(EntriesIn(toB).size() == 1);
	CHECK(CountOf(toB, MT::kQuestAnswer) == 0);
}

TEST("server: a joiner hears the session's quests after the welcome, from nobody; a load's ask hears them again")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	f.fake->Deliver(1, Report({ Stage(0x30, 10), Stage(0x30, 20), Objective(0x30, 1, 3, 20) }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteQuests)));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	const auto packets = f.fake->TakeThrough(2, Type(MT::kRemoteQuests));
	const auto heard = EntriesIn(packets);
	REQUIRE(heard.size() == 3);
	CHECK(heard[0].first == 0);
	CHECK(heard[0].second == Stage(0x30, 10));
	CHECK(heard[2].second == Objective(0x30, 1, 3, 20));
	CHECK(CountOf(packets, MT::kPlayerJoined) == 1);  // after the welcome and who is there

	f.fake->Deliver(2, Protocol::Writer{ MT::kAskQuests }.Data());
	const auto again = EntriesIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests)));
	CHECK(again.size() == 3);
	REQUIRE(f.fake->Settle());
	CHECK(EntriesIn(f.fake->Take(1)).empty());  // Ada heard nothing of Bo's ask
}

TEST("server: the host's reset clears the session's quests and tells the others; a friend's is ignored")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	const auto host = f.Join(1, Fixture::MakeHello("Host", 0x4057));
	REQUIRE(host);
	REQUIRE(host->hostId == host->playerId);
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	f.fake->Deliver(2, Report({ Stage(0x40, 10) }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteQuests)));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kRemoteQuests)));

	// The friend says its world is new: nothing happens.
	f.fake->Deliver(2, Protocol::Encode(QuestReset{}, false));
	REQUIRE(f.LoggedSoon("is not the host: its quest reset is ignored"));
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(1), MT::kQuestReset) == 0);

	// The host loaded a save: the book is its world from now on, and the friend hears the reset, then the host's quests.
	f.fake->Deliver(1, Protocol::Encode(QuestReset{}, false));
	f.fake->Deliver(1, Report({ Stage(0x40, 5) }));
	const auto toFriend = f.fake->TakeThrough(2, Type(MT::kRemoteQuests));
	REQUIRE(CountOf(toFriend, MT::kQuestReset) == 1);
	const auto reset = std::ranges::find_if(toFriend, [](const auto& a_packet) { return Protocol::PeekType(a_packet) == MT::kQuestReset; });
	REQUIRE(Protocol::DecodeQuestReset(*reset, true));
	CHECK(Protocol::DecodeQuestReset(*reset, true)->playerId == host->playerId);
	CHECK(Protocol::PeekType(toFriend.back()) == MT::kRemoteQuests);  // the reset first, then the host's quests
	CHECK(CountOf(f.fake->TakeThrough(1, Type(MT::kRemoteQuests)), MT::kQuestReset) == 0);  // not back to the host
	f.fake->Deliver(2, Protocol::Writer{ MT::kAskQuests }.Data());
	const auto book = EntriesIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests)));
	REQUIRE(book.size() == 1);
	CHECK(book[0].second == Stage(0x40, 5));  // the friend's 10 went with the old world
}

TEST("server: every quest entry is answered: past the budget the reporter hears busy, and the others nothing")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	constexpr int BATCHES = 200;  // the budget holds 64 at once, and 8 more a second
	for (int i = 0; i < BATCHES; ++i) {
		f.fake->Deliver(1, Report({ Stage(0x50 + static_cast<std::uint32_t>(i), 1), Stage(0x50 + static_cast<std::uint32_t>(i), 2) }));
	}
	REQUIRE(f.fake->Settle());
	const auto toA = f.fake->Take(1);
	const auto news = EntriesIn(toA);
	const auto answers = EntriesIn(toA, MT::kQuestAnswer);
	CHECK(news.size() + answers.size() == BATCHES * 2);
	REQUIRE(!answers.empty());
	for (const auto& [player, answer] : answers) {
		CHECK(answer.value == QuestEntry::kBusy);
	}
	CHECK(EntriesIn(f.fake->Take(2)).size() == news.size());
	CHECK(f.LoggedSoon("reports quests faster than the server passes them on"));
}

TEST("server: a new session forgets the last one's quests")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	f.fake->Deliver(1, Report({ Stage(0x60, 10) }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteQuests)));
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));  // the first player in again: a new session
	f.fake->Deliver(2, Protocol::Writer{ MT::kAskQuests }.Data());
	f.fake->Deliver(2, Report({ Stage(0x61, 1) }));
	const auto heard = EntriesIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests)));
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].second == Stage(0x61, 1));
}

// ---- two games through the server ---------------------------------------------------------------------------------

TEST("quests, two games: the host's world reaches a friend who joins, and the friend's step reaches the host, each made in order")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	const auto host = f.Join(1, Fixture::MakeHello("Host", 0x4057));
	REQUIRE(host);

	// The host's game says its world: MQ102 at stage 20 (10 and 20 done, objective 10 completed, 20 shown).
	std::vector<QuestRules::QuestRead> world;
	world.push_back({ .questId = 0x0003372B, .current = 20, .done = { 20, 10 }, .objectives = { { 10, 3 }, { 20, 1 } } });
	f.fake->Deliver(1, Protocol::Encode(QuestReset{}, false));
	for (const auto& packet : Protocol::Batches(QuestRules::Snapshot(world), MT::kQuestWorld)) {
		f.fake->Deliver(1, packet);
	}
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteQuests)));

	// A friend joins, with a fresh character: hears it all and makes it, stage 10 before 20, the objectives after.
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	QuestRules::View  view;
	QuestRules::Maker maker;
	FakeGame          game;
	auto              now = T0;
	for (const auto& [player, entry] : EntriesIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests)))) {
		if (view.Heard(entry, false)) {
			maker.Add(entry, player, now);
		}
	}
	std::vector<QuestEntry> made;
	for (int tick = 0; tick < 200 && maker.Size() != 0; ++tick) {
		now += 100ms;
		while (const auto step = maker.Next(now, game.Looker())) {
			if (step->verdict != QuestRules::Verdict::kMake) {
				continue;
			}
			const auto& e = step->want.entry;
			made.push_back(e);
			if (e.kind == QuestEntry::kStage) {
				game.done.insert({ e.questId, e.index });
				maker.Done(e.questId, e.index, now);
			} else if (e.kind == QuestEntry::kObjective) {
				game.objectives[e.index] = QuestBook::MergeObjective(game.objectives[e.index], e.value);
			}
		}
	}
	const std::vector<QuestEntry> want{ Stage(0x0003372B, 10), Stage(0x0003372B, 20), Objective(0x0003372B, 10, 3, 20), Objective(0x0003372B, 20, 1, 20) };
	CHECK(made == want);

	// The friend does the next step: its view reports it once, the host hears it from the friend.
	REQUIRE(view.Offer(Stage(0x0003372B, 30)));
	CHECK(!view.Offer(Stage(0x0003372B, 30)));
	f.fake->Deliver(2, Report({ Stage(0x0003372B, 30) }));
	const auto atHost = EntriesIn(f.fake->TakeThrough(1, Type(MT::kRemoteQuests)));
	REQUIRE(!atHost.empty());
	CHECK(atHost.back().first == 2);
	CHECK(atHost.back().second == Stage(0x0003372B, 30));
}

TEST("quests, two games: after the host loads an older save, the friend's own past is not made in the host's world")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	const auto host = f.Join(1, Fixture::MakeHello("Host", 0x4057));
	REQUIRE(host);
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	auto now = T0;

	// Both reached stage 20 of 0x40: objective 10 completed, 20 shown; the friend also finished 0x41. The host loads a
	// save from before stage 20: its reset, then its world (stage 10, objective 10 shown at 10).
	f.fake->Deliver(1, Protocol::Encode(QuestReset{}, false));
	f.fake->Deliver(1, Report({ Stage(0x40, 10), Objective(0x40, 10, 1, 10) }));

	// The friend: its view cleared, its base taken (what it holds), its quests started at their stages.
	QuestRules::View  view;
	QuestRules::Apart apart;
	const std::vector<QuestEntry> held{ Objective(0x40, 10, 3, 20), Objective(0x40, 20, 1, 20), Completed(0x41) };
	view.Rebase(held);
	const std::vector<std::pair<std::uint32_t, std::uint16_t>> at{ { 0x40, 20 }, { 0x41, 50 } };
	apart.Rebase(at, now);
	for (const auto& [player, entry] : EntriesIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests)))) {
		view.Heard(entry, false);
	}
	// Its poll a second later, and every second after, says nothing of its past...
	std::vector<QuestEntry> sent;
	for (int poll = 1; poll <= 10; ++poll) {
		now += 1s;
		for (const auto& e : held) {
			if (apart.Judge(e.questId, view, now) == QuestRules::Apart::Say::kReport && view.Offer(e)) {
				sent.push_back(e);
			}
		}
	}
	CHECK(sent.empty());
	// ... and its next stage of 0x40 (it is at 20, the session at 10) is its own, not the host's.
	CHECK(apart.Judge(0x40, view, now) == QuestRules::Apart::Say::kOwn);

	// The server's book is the host's world alone, for any later joiner.
	f.fake->Deliver(2, Protocol::Writer{ MT::kAskQuests }.Data());
	const auto book = EntriesIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests)));
	const std::vector<std::pair<std::uint32_t, QuestEntry>> want{ { 0, Stage(0x40, 10) }, { 0, Objective(0x40, 10, 1, 10) } };
	CHECK(book == want);
}

TEST("quests, two games: an end the session refused is not reported again every second")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	// Ada's game failed quest 0x50; Bo's completed it its own way.
	f.fake->Deliver(1, Report({ Failed(0x50) }));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kRemoteQuests)));
	f.fake->Take(2);
	QuestRules::View bo;
	int              reports = 0;
	for (int poll = 0; poll < 10; ++poll) {
		if (!bo.Offer(Completed(0x50))) {
			continue;
		}
		++reports;
		f.fake->Deliver(2, Report({ Completed(0x50) }));
		for (const auto& [player, answer] : EntriesIn(f.fake->TakeThrough(2, Type(MT::kQuestAnswer)), MT::kQuestAnswer)) {
			CHECK(answer.value == QuestEntry::kRefused);
			bo.Answered(answer);
		}
	}
	CHECK(reports == 1);
	CHECK(bo.Kept() == 1);
}

// ---- quests the host's world ended ----------------------------------------------------------------------------------

TEST("quest judge: the session's past on a quest it ended that this game never started is put aside; a live word or a started quest is made")
{
	using V = QuestRules::Verdict;
	QuestRules::Here here;
	here.exists = true;
	here.cls = QuestRules::Class::kShared;
	here.sessionEnded = true;
	CHECK(QuestRules::Judge(Stage(0x10, 10), 0, here) == V::kParked);
	CHECK(QuestRules::Judge(Objective(0x10, 1, 3, 10), 0, here) == V::kParked);
	CHECK(QuestRules::Judge(Completed(0x10), 0, here) == V::kParked);
	CHECK(QuestRules::Judge(Failed(0x10), 0, here) == V::kParked);
	CHECK(QuestRules::Judge(Stage(0x10, 10), 2, here) == V::kMake);  // said live: a friend did it just now
	CHECK(QuestRules::Judge(Completed(0x10), 2, here) == V::kNotStarted);

	// Started here (its own play, or a stage made here): caught up to the session's end as ever.
	here.started = true;
	here.current = 5;
	CHECK(QuestRules::Judge(Stage(0x10, 10), 0, here) == V::kMake);
	CHECK(QuestRules::Judge(Stage(0x10, 1), 0, here) == V::kPassed);
	CHECK(QuestRules::Judge(Completed(0x10), 0, here) == V::kMake);
	here.has = true;
	CHECK(QuestRules::Judge(Completed(0x10), 0, here) == V::kHas);

	// A quest the session has not ended is begun from its past as ever; what is not here stays not here.
	here = {};
	here.exists = true;
	here.cls = QuestRules::Class::kShared;
	CHECK(QuestRules::Judge(Stage(0x10, 10), 0, here) == V::kMake);
	here.sessionEnded = true;
	here.cls = QuestRules::Class::kMisc;
	CHECK(QuestRules::Judge(Stage(0x10, 10), 0, here) == V::kNotHere);
	here.exists = false;
	CHECK(QuestRules::Judge(Stage(0x10, 10), 0, here) == V::kMissing);
	CHECK(QuestRules::VerdictName(V::kParked) == "parked");
}

TEST("quest maker: the past on an ended quest never started here waits aside, and is made in order once the quest starts here")
{
	using V = QuestRules::Verdict;
	QuestRules::Maker maker;
	FakeGame          game;
	game.ended.insert(0x10);
	auto now = T0;
	// The host finished 0x10 before the session (two stages, an objective, its end); 0x20 runs there.
	for (const auto& e : { Stage(0x10, 10), Stage(0x10, 20), Objective(0x10, 1, 3, 20), Completed(0x10), Stage(0x20, 5) }) {
		maker.Add(e, 0, now);
	}
	auto steps = RunMaker(maker, game, now);
	std::vector<std::pair<V, QuestEntry>> want{ { V::kParked, Stage(0x10, 10) }, { V::kParked, Stage(0x10, 20) }, { V::kParked, Objective(0x10, 1, 3, 20) },
		{ V::kParked, Completed(0x10) }, { V::kMake, Stage(0x20, 5) } };
	CHECK(steps == want);
	CHECK(maker.Parked() == 4);
	CHECK(maker.ParkedQuests() == 1);
	CHECK(!game.done.contains({ 0x10, 10 }));
	CHECK(!game.completed);
	CHECK(maker.Unpark(0x20, now) == 0);  // nothing aside for it

	// A newer word on a thing put aside takes its place (an objective's state goes forward only).
	maker.Add(Objective(0x10, 1, 1, 20), 0, now);
	steps = RunMaker(maker, game, now);
	REQUIRE(steps.size() == 1);
	CHECK(steps[0].first == V::kParked);
	CHECK(maker.Parked() == 4);

	// The friend starts 0x10 by its own play, and a friend's live word on it is heard after: the past goes first, in the
	// order heard, the stages one at a time, then the objective and the end.
	game.started.insert(0x10);
	maker.Add(Stage(0x10, 30), 2, now);
	CHECK(maker.Unpark(0x10, now) == 4);
	CHECK(maker.Parked() == 0);
	CHECK(maker.ParkedQuests() == 0);
	CHECK(maker.Unparked() == 4);
	CHECK(maker.Unpark(0x10, now) == 0);
	steps = RunMaker(maker, game, now);
	want = { { V::kMake, Stage(0x10, 10) }, { V::kMake, Stage(0x10, 20) }, { V::kMake, Stage(0x10, 30) }, { V::kMake, Objective(0x10, 1, 3, 20) },
		{ V::kMake, Completed(0x10) } };
	CHECK(steps == want);
	CHECK(game.completed);

	// A quest forgotten (a radiant run over) or a new world (the host's reset) leaves nothing aside.
	game.ended.insert(0x30);
	game.ended.insert(0x40);
	maker.Add(Stage(0x30, 1), 0, now);
	maker.Add(Stage(0x40, 1), 0, now);
	RunMaker(maker, game, now);
	CHECK(maker.Parked() == 2);
	maker.Forget(0x30);
	CHECK(maker.Parked() == 1);
	CHECK(maker.Unpark(0x30, now) == 0);
	maker.Clear();
	CHECK(maker.Parked() == 0);
	CHECK(maker.Unpark(0x40, now) == 0);
}

TEST("quest maker: a quest not started here is not begun from the past until it has come whole, so an end in a later batch is known")
{
	using V = QuestRules::Verdict;
	QuestRules::Maker maker;
	FakeGame          game;
	// 0x70's stages came in one batch; its end comes in the next, a second later.
	auto now = T0;
	maker.Add(Stage(0x70, 10), 0, now);
	maker.Add(Stage(0x70, 20), 0, now);
	now += 1s;
	CHECK(!maker.Next(now, game.Looker()));
	game.ended.insert(0x70);
	maker.Add(Completed(0x70), 0, now);
	const auto steps = RunMaker(maker, game, now);
	const std::vector<std::pair<V, QuestEntry>> want{ { V::kParked, Stage(0x70, 10) }, { V::kParked, Stage(0x70, 20) }, { V::kParked, Completed(0x70) } };
	CHECK(steps == want);
	CHECK(game.done.empty());

	// A quest started here, and a live word, do not wait for the past to settle.
	QuestRules::Maker other;
	FakeGame          started;
	started.started.insert(0x71);
	other.Add(Stage(0x71, 10), 0, T0);
	auto step = other.Next(T0, started.Looker());
	REQUIRE(step);
	CHECK(step->verdict == V::kMake);
	other.Add(Stage(0x72, 10), 2, T0);
	step = other.Next(T0 + QuestRules::MAKE_EVERY, started.Looker());
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x72, 10));
	CHECK(step->verdict == V::kMake);
}

TEST("server: the host's world goes to the others as the session's word (player 0) and back to the host as its own; a friend's is ignored")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	const auto host = f.Join(1, Fixture::MakeHello("Host", 0x4057));
	REQUIRE(host);
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));

	// A friend's game cannot say the world.
	f.fake->Deliver(2, Protocol::Encode(QuestBatch{ 0, { Stage(0x40, 10) } }, MT::kQuestWorld));
	REQUIRE(f.LoggedSoon("is not the host: its quest world is ignored"));
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(1), MT::kRemoteQuests) == 0);
	CHECK(CountOf(f.fake->Take(2), MT::kRemoteQuests) == 0);

	// The host's world after a load, a long game's: 100 batches, past play's burst of 64, and none of it answered busy.
	std::vector<QuestEntry> world;
	for (std::uint32_t q = 0; q < 100; ++q) {
		for (std::uint16_t s = 1; s <= Protocol::MAX_QUEST_BATCH; ++s) {
			world.push_back(Stage(0x1000 + q, s));
		}
	}
	f.fake->Deliver(1, Protocol::Encode(QuestReset{}, false));
	for (const auto& packet : Protocol::Batches(world, MT::kQuestWorld)) {
		f.fake->Deliver(1, packet);
	}
	REQUIRE(f.fake->Settle());
	const auto toHost = f.fake->Take(1);
	const auto toFriend = f.fake->Take(2);
	const auto atHost = EntriesIn(toHost);
	const auto atFriend = EntriesIn(toFriend);
	REQUIRE(atHost.size() == world.size());
	REQUIRE(atFriend.size() == world.size());
	CHECK(std::ranges::all_of(atHost, [&](const auto& a_e) { return a_e.first == host->playerId; }));
	CHECK(std::ranges::all_of(atFriend, [](const auto& a_e) { return a_e.first == 0; }));
	CHECK(atFriend.front().second == world.front());
	CHECK(atFriend.back().second == world.back());
	CHECK(EntriesIn(toHost, MT::kQuestAnswer).empty());

	// Play after it is news as ever, with who did it.
	f.fake->Deliver(1, Report({ Stage(0x40, 10) }));
	const auto live = EntriesIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests)));
	REQUIRE(live.size() == 1);
	CHECK(live[0].first == host->playerId);
}

TEST("quests, two games: a quest the host finished before the session waits aside in a friend's game until it starts there; one behind catches up")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	REQUIRE(f.Join(1, Fixture::MakeHello("Host", 0x4057)));

	// The host's world: 0x50 completed (stages 10 and 20, objective 1 completed), 0x60 running at stage 5.
	std::vector<QuestRules::QuestRead> reads;
	reads.push_back({ .questId = 0x50, .current = 20, .done = { 10, 20 }, .objectives = { { 1, 3 } }, .completed = true });
	reads.push_back({ .questId = 0x60, .current = 5, .done = { 5 } });
	f.fake->Deliver(1, Protocol::Encode(QuestReset{}, false));
	for (const auto& packet : Protocol::Batches(QuestRules::Snapshot(reads), MT::kQuestWorld)) {
		f.fake->Deliver(1, packet);
	}
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteQuests)));
	REQUIRE(f.fake->Settle());

	// Two friends join: one never started 0x50, one is at its stage 10.
	REQUIRE(f.Join(2, Fixture::MakeHello("New", 0x2222)));
	REQUIRE(f.Join(3, Fixture::MakeHello("Behind", 0x3333)));
	using V = QuestRules::Verdict;
	using Steps = std::vector<std::pair<V, QuestEntry>>;
	const auto play = [&](PeerId a_peer, FakeGame& a_game) {
		QuestRules::View  view;
		QuestRules::Maker maker;
		auto              now = T0;
		for (const auto& [player, entry] : EntriesIn(f.fake->TakeThrough(a_peer, Type(MT::kRemoteQuests)))) {
			CHECK(player == 0);
			if (view.Heard(entry, false)) {
				maker.Add(entry, player, now);
			}
			if (view.Known(Completed(entry.questId)) || view.Known(Failed(entry.questId))) {
				a_game.ended.insert(entry.questId);
			}
		}
		Steps steps = RunMaker(maker, a_game, now);
		if (a_game.started.contains(0x50) || a_game.Current(0x50) != 0) {
			return steps;
		}
		// it starts 0x50 by its own play: the session's word on it is made then
		a_game.started.insert(0x50);
		CHECK(maker.Unpark(0x50, now) == 4);
		const auto more = RunMaker(maker, a_game, now);
		steps.insert(steps.end(), more.begin(), more.end());
		return steps;
	};
	const auto madeOf = [](const Steps& a_steps) {
		std::vector<QuestEntry> made;
		for (const auto& [verdict, entry] : a_steps) {
			if (verdict == V::kMake) {
				made.push_back(entry);
			}
		}
		return made;
	};

	// The new friend: nothing of 0x50 before it starts there (0x60 is begun); then 0x50 caught up to its end.
	FakeGame   fresh;
	const auto first = play(2, fresh);
	const std::vector<QuestEntry> wantFresh{ Stage(0x60, 5), Stage(0x50, 10), Stage(0x50, 20), Objective(0x50, 1, 3, 20), Completed(0x50) };
	CHECK(madeOf(first) == wantFresh);
	CHECK(fresh.completed);

	// The friend behind in it: caught up as ever, nothing put aside.
	FakeGame behind;
	behind.done.insert({ 0x50, 10 });
	const auto second = play(3, behind);
	CHECK(std::ranges::none_of(second, [](const auto& a_s) { return a_s.first == V::kParked; }));
	const std::vector<QuestEntry> wantBehind{ Stage(0x50, 20), Stage(0x60, 5), Objective(0x50, 1, 3, 20), Completed(0x50) };
	CHECK(madeOf(second) == wantBehind);
	CHECK(behind.completed);
}
