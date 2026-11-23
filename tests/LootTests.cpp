#include "Test.h"

#include "FormTranslate.h"
#include "LootLog.h"
#include "ServerFixture.h"
#include "game/LootRules.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

// Containers and loot (docs/DESIGN.md, "Containers and loot"): the messages (common/Protocol.h), their form IDs on the
// wire (common/FormTranslate.h), the server's numbered log (common/LootLog.h) and passing on (common/Server.cpp), and
// each game's rules (src/game/LootRules.h). What reads and changes the game itself (src/game/LootSync.cpp) is tested in
// a game only: tools/rig/steps/loot.sh.

using Protocol::LootAsk;
using Protocol::LootBatch;
using Protocol::LootCaughtUp;
using namespace LootRules;

namespace
{
	using MT = Protocol::MessageType;
	using namespace std::chrono_literals;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	LootEntry Take(std::uint32_t a_holder, std::uint32_t a_item, std::uint32_t a_count)
	{
		return LootEntry{ .holderId = a_holder, .itemId = a_item, .count = a_count, .kind = LootEntry::kTake };
	}
	LootEntry Put(std::uint32_t a_holder, std::uint32_t a_item, std::uint32_t a_count)
	{
		return LootEntry{ .holderId = a_holder, .itemId = a_item, .count = a_count, .kind = LootEntry::kPut };
	}
	LootEntry Emptied(std::uint32_t a_holder, std::uint32_t a_asOf)
	{
		return LootEntry{ .holderId = a_holder, .itemId = 0, .count = a_asOf, .kind = LootEntry::kEmptied };
	}
	// The session's move: numbered, with who made it.
	LootEntry Numbered(LootEntry a_move, std::uint32_t a_number, std::uint32_t a_player)
	{
		a_move.number = a_number;
		a_move.playerId = a_player;
		return a_move;
	}

	std::vector<std::uint8_t> Report(std::vector<LootEntry> a_entries) { return Protocol::Encode(LootBatch{ false, std::move(a_entries) }, MT::kLoot); }

	// The moves of every batch of a_type in a_packets, in order.
	std::vector<LootEntry> MovesIn(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type = MT::kRemoteLoot)
	{
		std::vector<LootEntry> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == a_type) {
				if (const auto batch = Protocol::DecodeLootBatch(packet)) {
					out.insert(out.end(), batch->entries.begin(), batch->entries.end());
				}
			}
		}
		return out;
	}

	std::vector<LootCaughtUp> CaughtUpsIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<LootCaughtUp> out;
		for (const auto& packet : a_packets) {
			if (const auto msg = Protocol::DecodeLootCaughtUp(packet)) {
				out.push_back(*msg);
			}
		}
		return out;
	}

	bool AllCatchUp(const std::vector<std::vector<std::uint8_t>>& a_packets, bool a_catchUp)
	{
		return std::ranges::all_of(a_packets, [&](const auto& a_packet) {
			if (Protocol::PeekType(a_packet) != MT::kRemoteLoot) {
				return true;
			}
			const auto batch = Protocol::DecodeLootBatch(a_packet);
			return batch && batch->catchUp == a_catchUp;
		});
	}

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	constexpr Holder CHEST{ .kind = HolderKind::kContainer };
	constexpr Holder BODY{ .kind = HolderKind::kActor, .dead = true };
	constexpr Holder LIVING{ .kind = HolderKind::kActor };
	constexpr Item   SWORD{ .kind = ItemKind::kWeapon };
	constexpr Item   GOLD{ .kind = ItemKind::kMisc };
}

// ---- the messages -------------------------------------------------------------------------------------------------

TEST("loot: a batch goes to the server, back numbered with its player, and an answer carries why")
{
	const std::vector<LootEntry> moves{ Take(0x0001A332, 0x0000000F, 25), Put(0x0001A332, 0x00012EB7, 1), Emptied(0x0001A332, 7) };
	const auto                   report = Report(moves);
	CHECK(Protocol::PeekType(report) == MT::kLoot);
	auto back = Protocol::DecodeLootBatch(report);
	REQUIRE(back);
	CHECK(back->entries == moves);
	CHECK(!back->catchUp);

	std::vector<LootEntry> numbered;
	for (std::uint32_t i = 0; i < moves.size(); ++i) {
		numbered.push_back(Numbered(moves[i], 40 + i, 3));
	}
	for (const bool catchUp : { false, true }) {
		back = Protocol::DecodeLootBatch(Protocol::Encode(LootBatch{ catchUp, numbered }, MT::kRemoteLoot));
		REQUIRE(back);
		CHECK(back->entries == numbered);
		CHECK(back->catchUp == catchUp);
	}

	auto answered = moves;
	answered[0].answer = LootEntry::kBusy;
	answered[1].answer = LootEntry::kFull;
	answered[2].answer = LootEntry::kRefused;
	back = Protocol::DecodeLootBatch(Protocol::Encode(LootBatch{ false, answered }, MT::kLootAnswer));
	REQUIRE(back);
	CHECK(back->entries == answered);
	// A game's move does not carry a number, a player or an answer: they are the server's to give.
	auto stray = moves[0];
	stray.number = 9;
	stray.playerId = 9;
	stray.answer = LootEntry::kRefused;
	back = Protocol::DecodeLootBatch(Report({ stray }));
	REQUIRE(back);
	CHECK(back->entries[0] == moves[0]);
	CHECK(Protocol::VERSION >= 10);
}

TEST("loot: a batch that can't be read is none: empty, a kind or an answer not known, a count out of range, an emptied marker with an item")
{
	const auto none = [](const std::vector<std::uint8_t>& a_packet) { return !Protocol::DecodeLootBatch(a_packet); };
	CHECK(none(Report({})));
	auto move = Take(0x1A332, 0xF, 1);
	move.kind = LootEntry::KIND_MAX + 1;
	CHECK(none(Report({ move })));
	CHECK(none(Report({ Take(0x1A332, 0xF, 0) })));
	CHECK(none(Report({ Take(0x1A332, 0xF, Protocol::MAX_LOOT_COUNT + 1) })));
	CHECK(!none(Report({ Take(0x1A332, 0xF, Protocol::MAX_LOOT_COUNT) })));
	auto marker = Emptied(0x1A332, 3);
	marker.itemId = 0xF;
	CHECK(none(Report({ marker })));
	auto answer = Take(0x1A332, 0xF, 1);
	answer.answer = LootEntry::ANSWER_MAX + 1;
	CHECK(none(Protocol::Encode(LootBatch{ false, { answer } }, MT::kLootAnswer)));

	// The session's move needs its number and an item; a game's own and an answer are read with an item no form named
	// (FormTranslate made it 0), so the server answers it.
	CHECK(none(Protocol::Encode(LootBatch{ false, { Take(0x1A332, 0xF, 1) } }, MT::kRemoteLoot)));
	CHECK(none(Protocol::Encode(LootBatch{ false, { Numbered(Take(0x1A332, 0, 1), 1, 1) } }, MT::kRemoteLoot)));
	CHECK(!none(Protocol::Encode(LootBatch{ false, { Numbered(Emptied(0x1A332, 1), 1, 1) } }, MT::kRemoteLoot)));
	CHECK(!none(Report({ Take(0x1A332, 0, 1) })));
	CHECK(!none(Protocol::Encode(LootBatch{ false, { Take(0x1A332, 0, 1) } }, MT::kLootAnswer)));

	// Short, long, a flags byte not known.
	auto packet = Report({ Take(0x1A332, 0xF, 1) });
	CHECK(none(std::vector<std::uint8_t>(packet.begin(), packet.end() - 1)));
	packet.push_back(0);
	CHECK(none(packet));
	auto remote = Protocol::Encode(LootBatch{ true, { Numbered(Take(0x1A332, 0xF, 1), 1, 1) } }, MT::kRemoteLoot);
	CHECK(!none(remote));
	remote[1] = 2;
	CHECK(none(remote));
	CHECK(!Protocol::DecodeLootBatch(Protocol::Encode(LootAsk{})));
}

TEST("loot: a long list goes in batches of MAX_LOOT_BATCH, in order, and nothing for none")
{
	CHECK(Protocol::LootBatches({}, MT::kLoot).empty());
	std::vector<LootEntry> moves;
	for (std::uint32_t i = 0; i < 2 * Protocol::MAX_LOOT_BATCH + 2; ++i) {
		moves.push_back(Numbered(Take(0x1A332, 0x100 + i, 1), i + 1, 2));
	}
	const auto packets = Protocol::LootBatches(moves, MT::kRemoteLoot, true);
	REQUIRE(packets.size() == 3);
	CHECK(AllCatchUp(packets, true));
	CHECK(MovesIn(packets) == moves);
	CHECK(Protocol::DecodeLootBatch(packets[2])->entries.size() == 2);
	// Encode alone writes no more than a batch holds.
	CHECK(Protocol::DecodeLootBatch(Protocol::Encode(LootBatch{ false, moves }, MT::kRemoteLoot))->entries.size() == Protocol::MAX_LOOT_BATCH);
}

TEST("loot: an ask and the end of its catch-up go there and back; numbers start at 1")
{
	const LootAsk ask{ .logId = 0x1234'5678'9ABC'DEF0ull, .from = 41 };
	CHECK(Protocol::DecodeLootAsk(Protocol::Encode(ask)) == ask);
	CHECK(Protocol::DecodeLootAsk(Protocol::Encode(LootAsk{})) == LootAsk{});
	CHECK(!Protocol::DecodeLootAsk(Protocol::Encode(LootAsk{ .logId = 5, .from = 0 })));
	auto packet = Protocol::Encode(ask);
	packet.push_back(0);
	CHECK(!Protocol::DecodeLootAsk(packet));

	const LootCaughtUp done{ .logId = 7, .from = 3, .next = 9 };
	CHECK(Protocol::DecodeLootCaughtUp(Protocol::Encode(done)) == done);
	CHECK(Protocol::DecodeLootCaughtUp(Protocol::Encode(LootCaughtUp{ .logId = 7, .from = 1, .next = 1 })));
	CHECK(!Protocol::DecodeLootCaughtUp(Protocol::Encode(LootCaughtUp{ .logId = 7, .from = 0, .next = 9 })));
	CHECK(!Protocol::DecodeLootCaughtUp(Protocol::Encode(LootCaughtUp{ .logId = 7, .from = 10, .next = 9 })));
	CHECK(!Protocol::DecodeLootCaughtUp(Protocol::Encode(LootCaughtUp{ .logId = 7, .from = 1, .next = 0 })));

	CHECK(FormTranslate::CarriesForms(MT::kLoot));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteLoot));
	CHECK(FormTranslate::CarriesForms(MT::kLootAnswer));
	CHECK(!FormTranslate::CarriesForms(MT::kAskLoot));
	CHECK(!FormTranslate::CarriesForms(MT::kLootCaughtUp));
}

// ---- form IDs on the wire -----------------------------------------------------------------------------------------

TEST("loot: holders and items are named by their plugin on the wire; a move another game can't name is left out there, never a game's own")
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

	// The guest takes from a chest of B.esp (its 01) an item of Extra.esp, which the host lacks, and gold from it; and
	// says it emptied it. A runtime item has no ID in the session: it becomes 0 (the server refuses it, answered).
	const auto out = FormTranslate::Translate(guestEdge, Way::kToSession,
		Report({ Take(0x01000D62, 0x04000800, 1), Take(0x01000D62, 0x0000000F, 30), Emptied(0x01000D62, 4), Take(0x01000D62, 0xFF000123, 1) }));
	REQUIRE(out.kind == Result::Kind::kChanged);
	auto batch = Protocol::DecodeLootBatch(out.packet);
	REQUIRE(batch);
	REQUIRE(batch->entries.size() == 4);
	CHECK(batch->entries[0] == Take(0x03000D62, 0x04000800, 1));  // B.esp is the session's 03, Extra.esp its 04
	CHECK(batch->entries[1] == Take(0x03000D62, 0x0000000F, 30));
	CHECK(batch->entries[2] == Emptied(0x03000D62, 4));          // a ledger number, not a form: as it was
	CHECK(batch->entries[3] == Take(0x03000D62, 0, 1));

	// Passed on to the host: Extra.esp's move is left out, the rest named in the host's numbering.
	std::vector<LootEntry> numbered;
	for (std::uint32_t i = 0; i < 3; ++i) {
		numbered.push_back(Numbered(batch->entries[i], i + 1, 2));
	}
	const auto in = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(LootBatch{ false, numbered }, MT::kRemoteLoot));
	REQUIRE(in.kind == Result::Kind::kChanged);
	CHECK(in.entries == 1);
	batch = Protocol::DecodeLootBatch(in.packet);
	REQUIRE(batch);
	REQUIRE(batch->entries.size() == 2);
	CHECK(batch->entries[0] == Numbered(Take(0x03000D62, 0x0000000F, 30), 2, 2));
	CHECK(batch->entries[1] == Numbered(Emptied(0x03000D62, 4), 3, 2));

	// A batch of only a move the host can't name is not read at all; an answer keeps every move, one not named made 0.
	CHECK(FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(LootBatch{ false, { numbered[0] } }, MT::kRemoteLoot)).kind == Result::Kind::kDrop);
	auto answer = Take(0x03000D62, 0x04000800, 1);
	answer.answer = LootEntry::kRefused;
	const auto told = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(LootBatch{ false, { answer } }, MT::kLootAnswer));
	REQUIRE(told.kind == Result::Kind::kChanged);
	batch = Protocol::DecodeLootBatch(told.packet);
	REQUIRE(batch);
	REQUIRE(batch->entries.size() == 1);
	CHECK(batch->entries[0].itemId == 0);
	CHECK(batch->entries[0].answer == LootEntry::kRefused);
	CHECK(FormTranslate::Translate(FormIds::Mapper(), Way::kToSession, Report({ Take(0x0001A332, 0xF, 1) })).kind == Result::Kind::kSame);
	CHECK(FormTranslate::Translate(FormIds::Mapper(), Way::kToSession, Protocol::Encode(LootAsk{ .logId = 3, .from = 2 })).kind == Result::Kind::kSame);
}

// ---- the server's log ---------------------------------------------------------------------------------------------

TEST("loot log: moves are numbered from 1 with their player; no holder, a runtime form or a move no game could make is refused")
{
	LootLog   log;
	LootEntry out;
	CHECK(log.Report(Take(0x1A332, 0xF, 5), 2, out) == LootLog::Take::kNews);
	CHECK(out == Numbered(Take(0x1A332, 0xF, 5), 1, 2));
	CHECK(log.Report(Emptied(0x1A332, 2), 3, out) == LootLog::Take::kNews);
	CHECK(out.number == 2);
	CHECK(out.playerId == 3);
	CHECK(log.Next() == 3);

	CHECK(log.Report(Take(0, 0xF, 5), 2, out) == LootLog::Take::kRefused);
	CHECK(log.Report(Take(0xFF000800, 0xF, 5), 2, out) == LootLog::Take::kRefused);
	CHECK(log.Report(Put(0x1A332, 0xFF000801, 1), 2, out) == LootLog::Take::kRefused);
	CHECK(log.Report(Take(0x1A332, 0, 1), 2, out) == LootLog::Take::kRefused);  // an item the session can't name
	CHECK(log.Report(Take(0x1A332, 0xF, 0), 2, out) == LootLog::Take::kRefused);
	CHECK(log.Next() == 3);
	CHECK(log.Size() == 2);
}

TEST("loot log: past its most a new move is full, never the oldest dropped; the moves from a number come back in order")
{
	LootLog   log{ 3 };
	LootEntry out;
	for (std::uint32_t i = 1; i <= 3; ++i) {
		REQUIRE(log.Report(Take(0x1A332, 0x100 + i, 1), 1, out) == LootLog::Take::kNews);
	}
	CHECK(log.Report(Take(0x1A332, 0x200, 1), 1, out) == LootLog::Take::kFull);
	CHECK(log.Size() == 3);
	CHECK(log.From(0).size() == 3);
	CHECK(log.From(1).size() == 3);
	const auto later = log.From(2);
	REQUIRE(later.size() == 2);
	CHECK(later[0].number == 2);
	CHECK(later[1].number == 3);
	CHECK(log.From(4).empty());
}

TEST("loot log: a catch-up starts at a ledger of this log; another session's, none, or one past the end starts at 1; a restart is a new log")
{
	LootLog log;
	log.Restart(0xABCD);
	LootEntry out;
	for (int i = 0; i < 4; ++i) {
		log.Report(Take(0x1A332, 0xF, 1), 1, out);
	}
	CHECK(log.StartFor(0xABCD, 3) == 3);
	CHECK(log.StartFor(0xABCD, 5) == 5);  // holds them all: nothing to send
	CHECK(log.StartFor(0xABCD, 6) == 1);  // past this log's end: not this log's ledger after all
	CHECK(log.StartFor(0xABCE, 3) == 1);
	CHECK(log.StartFor(0, 3) == 1);

	log.Restart(0);
	CHECK(log.Id() != 0);
	CHECK(log.Next() == 1);
	CHECK(log.Size() == 0);
	CHECK(log.StartFor(0xABCD, 3) == 1);
}

// ---- the server ---------------------------------------------------------------------------------------------------

TEST("server: a move of loot goes to every game numbered, its reporter's too; one it refused goes back to the reporter alone")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	f.fake->Deliver(2, Report({ Take(0x1A332, 0xF, 25), Put(0x1A332, 0x12EB7, 1) }));
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 2 } }) {
		const auto packets = f.fake->TakeThrough(peer, Type(MT::kRemoteLoot));
		CHECK(AllCatchUp(packets, false));
		const auto heard = MovesIn(packets);
		REQUIRE(heard.size() == 2);
		CHECK(heard[0] == Numbered(Take(0x1A332, 0xF, 25), 1, 2));
		CHECK(heard[1] == Numbered(Put(0x1A332, 0x12EB7, 1), 2, 2));
	}
	f.fake->Deliver(1, Report({ Take(0, 0xF, 1), Emptied(0x1A332, 3), Take(0xFF000800, 0xF, 1) }));
	REQUIRE(f.fake->Settle());
	const auto toA = f.fake->Take(1);
	const auto news = MovesIn(toA);
	REQUIRE(news.size() == 1);
	CHECK(news[0] == Numbered(Emptied(0x1A332, 3), 3, 1));
	const auto answers = MovesIn(toA, MT::kLootAnswer);
	REQUIRE(answers.size() == 2);
	CHECK(answers[0].holderId == 0);
	CHECK(answers[0].answer == LootEntry::kRefused);
	CHECK(answers[1].holderId == 0xFF000800);
	CHECK(answers[1].answer == LootEntry::kRefused);
	const auto toB = f.fake->Take(2);
	CHECK(MovesIn(toB).size() == 1);
	CHECK(MovesIn(toB, MT::kLootAnswer).empty());
}

TEST("server: every move of loot is answered: past the budget the reporter hears busy, and the others nothing")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	constexpr int BATCHES = 200;  // the budget holds 64 at once, and 16 more a second
	for (int i = 0; i < BATCHES; ++i) {
		f.fake->Deliver(1, Report({ Take(0x1A332, 0x100 + static_cast<std::uint32_t>(i), 1), Put(0x1A332, 0xF, 1) }));
	}
	REQUIRE(f.fake->Settle());
	const auto toA = f.fake->Take(1);
	const auto news = MovesIn(toA);
	const auto answers = MovesIn(toA, MT::kLootAnswer);
	CHECK(news.size() + answers.size() == BATCHES * 2);
	REQUIRE(!answers.empty());
	for (const auto& answer : answers) {
		CHECK(answer.answer == LootEntry::kBusy);
	}
	for (std::size_t i = 0; i < news.size(); ++i) {
		CHECK(news[i].number == i + 1);  // numbered without a gap: a busy move took no number
	}
	CHECK(MovesIn(f.fake->Take(2)).size() == news.size());
	CHECK(f.LoggedSoon("reports loot faster than the server passes it on"));
}

TEST("server: an ask hears the moves from its ledger as a catch-up, then the log's id and next number; another log's ledger hears them all")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	f.fake->Deliver(1, Report({ Take(0x1A332, 0xF, 1), Take(0x1A332, 0xF, 2), Put(0x1A332, 0x12EB7, 1) }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteLoot)));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));

	// Bo's world never was in a session.
	f.fake->Deliver(2, Protocol::Encode(LootAsk{}));
	auto packets = f.fake->TakeThrough(2, Type(MT::kLootCaughtUp));
	CHECK(AllCatchUp(packets, true));
	auto heard = MovesIn(packets);
	REQUIRE(heard.size() == 3);
	CHECK(heard[0] == Numbered(Take(0x1A332, 0xF, 1), 1, 1));
	CHECK(heard[2].number == 3);
	auto done = CaughtUpsIn(packets);
	REQUIRE(done.size() == 1);
	CHECK(done[0].from == 1);
	CHECK(done[0].next == 4);
	CHECK(Protocol::PeekType(packets.back()) == MT::kLootCaughtUp);  // the moves first, then the end
	const auto logId = done[0].logId;

	// Its ledger after a save: the moves from 3 on.
	f.fake->Deliver(2, Protocol::Encode(LootAsk{ .logId = logId, .from = 3 }));
	packets = f.fake->TakeThrough(2, Type(MT::kLootCaughtUp));
	heard = MovesIn(packets);
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].number == 3);
	done = CaughtUpsIn(packets);
	REQUIRE(done.size() == 1);
	CHECK((done[0] == LootCaughtUp{ .logId = logId, .from = 3, .next = 4 }));

	// A ledger of another log (an old session's save): every move.
	f.fake->Deliver(2, Protocol::Encode(LootAsk{ .logId = logId + 1, .from = 3 }));
	packets = f.fake->TakeThrough(2, Type(MT::kLootCaughtUp));
	CHECK(MovesIn(packets).size() == 3);
	CHECK(CaughtUpsIn(packets).at(0).from == 1);
	REQUIRE(f.fake->Settle());
	CHECK(MovesIn(f.fake->Take(1)).empty());  // Ada heard nothing of Bo's asks
}

TEST("server: an ask past its budget is owed and answered once there is room, never dropped")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	constexpr std::uint32_t ASKS = 7;  // the budget holds 4 at once, then one in 2 s
	for (std::uint32_t i = 1; i <= ASKS; ++i) {
		f.fake->Deliver(1, Protocol::Encode(LootAsk{ .logId = 0, .from = i }));
	}
	REQUIRE(f.fake->Settle());
	auto done = CaughtUpsIn(f.fake->Take(1));
	CHECK(done.size() >= 4);
	// The newest one owed is answered once the budget has room (the older ones it replaced asked for the same world).
	if (done.size() < 5) {
		const auto later = CaughtUpsIn(f.fake->TakeThrough(1, Type(MT::kLootCaughtUp)));
		done.insert(done.end(), later.begin(), later.end());
	}
	CHECK(done.size() == 5);
	for (const auto& end : done) {
		CHECK(end.from == 1);  // a ledger of no log: from the first move
		CHECK(end.next == 1);
	}
}

TEST("server: a new session starts the log again under a new id")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	f.fake->Deliver(1, Report({ Take(0x1A332, 0xF, 1) }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteLoot)));
	f.fake->Deliver(1, Protocol::Encode(LootAsk{}));
	const auto old = CaughtUpsIn(f.fake->TakeThrough(1, Type(MT::kLootCaughtUp)));
	REQUIRE(old.size() == 1);
	CHECK(old[0].next == 2);
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));  // the first player in again: a new session
	f.fake->Deliver(2, Protocol::Encode(LootAsk{ .logId = old[0].logId, .from = 2 }));
	const auto packets = f.fake->TakeThrough(2, Type(MT::kLootCaughtUp));
	CHECK(MovesIn(packets).empty());
	const auto fresh = CaughtUpsIn(packets);
	REQUIRE(fresh.size() == 1);
	CHECK(fresh[0].logId != old[0].logId);
	CHECK(fresh[0].from == 1);
	CHECK(fresh[0].next == 1);
	f.fake->Deliver(2, Report({ Put(0x1A332, 0xF, 1) }));
	const auto heard = MovesIn(f.fake->TakeThrough(2, Type(MT::kRemoteLoot)));
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].number == 1);
}

// ---- what the session shares --------------------------------------------------------------------------------------

TEST("loot scope: every container and every dead actor's body shares its plain items; a living actor, a merchant's chest and barter never")
{
	CHECK(Of(CHEST, SWORD, false) == Scope::kShared);
	CHECK(Of(CHEST, GOLD, false) == Scope::kShared);
	CHECK(Of(BODY, SWORD, false) == Scope::kShared);
	for (const auto kind : { ItemKind::kArmor, ItemKind::kAmmo, ItemKind::kPotion, ItemKind::kIngredient, ItemKind::kSoulGem, ItemKind::kScroll,
			 ItemKind::kLight, ItemKind::kApparatus }) {
		CHECK(Of(CHEST, Item{ .kind = kind }, false) == Scope::kShared);
	}
	CHECK(Of(LIVING, GOLD, false) == Scope::kLiving);
	CHECK(Of(Holder{ .kind = HolderKind::kContainer, .vendor = true }, GOLD, false) == Scope::kVendor);
	CHECK(Of(CHEST, GOLD, true) == Scope::kBarter);
	CHECK(Of(Holder{ .kind = HolderKind::kContainer, .runtime = true }, GOLD, false) == Scope::kRuntimeHolder);
	CHECK(Of(Holder{}, GOLD, false) == Scope::kNotHolder);
	// Storage shares its moves (the marker leaves it alone: Judge).
	CHECK(Of(Holder{ .kind = HolderKind::kContainer, .storage = true }, GOLD, false) == Scope::kShared);
	// The holder decides first: a living actor's key is the living actor's.
	CHECK(Of(LIVING, Item{ .kind = ItemKind::kKey }, false) == Scope::kLiving);
}

TEST("loot scope: keys, books, quest items, instances and runtime forms stay each player's own")
{
	CHECK(Of(CHEST, Item{ .kind = ItemKind::kKey }, false) == Scope::kKey);
	CHECK(Of(CHEST, Item{ .kind = ItemKind::kBook }, false) == Scope::kBook);
	CHECK(Of(BODY, Item{ .kind = ItemKind::kWeapon, .quest = true }, false) == Scope::kQuest);
	CHECK(Of(CHEST, Item{ .kind = ItemKind::kWeapon, .instance = true }, false) == Scope::kInstance);
	CHECK(Of(CHEST, Item{ .kind = ItemKind::kPotion, .runtime = true }, false) == Scope::kRuntimeItem);
	CHECK(Of(CHEST, Item{}, false) == Scope::kNotItem);
	for (std::size_t i = 0; i < SCOPE_COUNT; ++i) {
		CHECK(std::string_view{ ScopeName(static_cast<Scope>(i)) } != "?");
	}
}

TEST("loot judge: made when loaded and shared here; a holder not loaded waits, a body alive here waits for its death, the rest is skipped")
{
	CHECK(Judge(true, CHEST, SWORD, false).verdict == Verdict::kMake);
	CHECK(Judge(false, CHEST, SWORD, false).verdict == Verdict::kWait);
	CHECK(Judge(false, LIVING, SWORD, false).verdict == Verdict::kWait);
	CHECK(Judge(true, LIVING, SWORD, false).verdict == Verdict::kDead);
	CHECK(Judge(true, BODY, SWORD, false).verdict == Verdict::kMake);
	const auto vendor = Judge(true, Holder{ .kind = HolderKind::kContainer, .vendor = true }, GOLD, false);
	CHECK(vendor.verdict == Verdict::kSkip);
	CHECK(vendor.scope == Scope::kVendor);
	const auto book = Judge(true, CHEST, Item{ .kind = ItemKind::kBook }, false);
	CHECK(book.verdict == Verdict::kSkip);
	CHECK(book.scope == Scope::kBook);
	// The emptied marker: judged by its holder alone; storage keeps each player's own.
	CHECK(Judge(true, CHEST, Item{}, true).verdict == Verdict::kMake);
	CHECK(Judge(true, BODY, Item{}, true).verdict == Verdict::kMake);
	CHECK(Judge(true, LIVING, Item{}, true).verdict == Verdict::kDead);
	CHECK(Judge(true, Holder{ .kind = HolderKind::kContainer, .storage = true }, Item{}, true).verdict == Verdict::kSkip);
	CHECK(Judge(true, Holder{ .kind = HolderKind::kContainer, .storage = true }, GOLD, false).verdict == Verdict::kMake);
}

// ---- the ledger ---------------------------------------------------------------------------------------------------

TEST("loot ledger: a world in no session asks from 1; moves heard while it asks are held, the catch-up makes them, then live moves go on")
{
	Ledger ledger;
	CHECK(ledger.Saved() == Ledger::Mark{});
	const auto ask = ledger.Ask();
	CHECK((ask == LootAsk{ .logId = 0, .from = 1 }));
	CHECK(ledger.Asking());
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 3, 2), false, 1) == Ledger::Heard::kHeld);
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 1, 2), true, 1) == Ledger::Heard::kMake);
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 2, 1), true, 1) == Ledger::Heard::kMake);  // our own too: the world lacks it
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 3, 2), true, 1) == Ledger::Heard::kMake);
	CHECK(ledger.CaughtUp(LootCaughtUp{ .logId = 77, .from = 1, .next = 4 }));
	CHECK(!ledger.Asking());
	CHECK((ledger.Saved() == Ledger::Mark{ .logId = 77, .next = 4 }));
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 4, 2), false, 1) == Ledger::Heard::kMake);
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 3, 2), false, 1) == Ledger::Heard::kHad);
	CHECK(ledger.Saved().next == 5);
	// A catch-up nobody asked for, and its end, are strays.
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 9, 2), true, 1) == Ledger::Heard::kVoid);
	CHECK(!ledger.CaughtUp(LootCaughtUp{ .logId = 77, .from = 9, .next = 10 }));
	CHECK(ledger.Saved().next == 5);
}

TEST("loot ledger: our own move comes back as ours, matched by what it says whatever the order; the same move by another is theirs")
{
	Ledger ledger;
	ledger.Loaded({ .logId = 5, .next = 1 });
	const auto a = Take(0x1A332, 0xF, 10);
	const auto b = Put(0x1A332, 0x12EB7, 1);
	ledger.Sent(a, T0);
	ledger.Sent(b, T0);
	CHECK(ledger.Out() == 2);
	auto answered = b;
	answered.answer = LootEntry::kBusy;
	CHECK(ledger.Answered(answered));  // b's answer before a's echo
	CHECK(ledger.Take(Numbered(a, 1, 4), false, 3) == Ledger::Heard::kMake);  // another player's take of the same
	CHECK(ledger.Out() == 1);
	CHECK(ledger.Take(Numbered(a, 2, 3), false, 3) == Ledger::Heard::kOwn);
	CHECK(ledger.Out() == 0);
	CHECK(!ledger.Answered(answered));  // given once
	CHECK(ledger.Saved().next == 3);
}

TEST("loot ledger: an ask made for a world a load replaced is void; the later ask's catch-up is the one made")
{
	Ledger ledger;
	ledger.Loaded({ .logId = 5, .next = 3 });
	CHECK((ledger.Ask() == LootAsk{ .logId = 5, .from = 3 }));
	ledger.Loaded({ .logId = 5, .next = 8 });  // a later save loaded before the answer came
	CHECK((ledger.Ask() == LootAsk{ .logId = 5, .from = 8 }));
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 3, 2), true, 1) == Ledger::Heard::kVoid);
	CHECK(!ledger.CaughtUp(LootCaughtUp{ .logId = 5, .from = 3, .next = 9 }));
	CHECK(ledger.Asking());
	CHECK(ledger.Saved().next == 8);
	CHECK(ledger.Take(Numbered(Take(0x1A332, 0xF, 1), 8, 2), true, 1) == Ledger::Heard::kMake);
	CHECK(ledger.CaughtUp(LootCaughtUp{ .logId = 5, .from = 8, .next = 9 }));
	CHECK((ledger.Saved() == Ledger::Mark{ .logId = 5, .next = 9 }));

	// A save of no number reads as 1; the session's end leaves the mark (the world's) and forgets what was out.
	ledger.Sent(Take(0x1A332, 0xF, 1), T0);
	(void)ledger.Ask();
	ledger.Disconnected();
	CHECK(ledger.Out() == 0);
	CHECK(!ledger.Asking());
	CHECK(ledger.Saved().next == 9);
	ledger.Loaded({ .logId = 6, .next = 0 });
	CHECK((ledger.Saved() == Ledger::Mark{ .logId = 6, .next = 1 }));
}

TEST("loot ledger: a move out too long is lost; our puts out count for the marker, by holder and item")
{
	Ledger ledger;
	ledger.Sent(Put(0x1A332, 0xF, 10), T0);
	ledger.Sent(Put(0x1A332, 0xF, 5), T0 + 30s);
	ledger.Sent(Put(0x1A333, 0xF, 7), T0 + 30s);
	ledger.Sent(Take(0x1A332, 0xF, 3), T0 + 30s);
	CHECK(ledger.PutsOut(0x1A332, 0xF) == 15);
	CHECK(ledger.PutsOut(0x1A332, 0x10) == 0);
	ledger.Expire(T0 + 61s);
	CHECK(ledger.Out() == 3);
	CHECK(ledger.Lost() == 1);
	CHECK(ledger.PutsOut(0x1A332, 0xF) == 5);
}

// ---- moves that wait ----------------------------------------------------------------------------------------------

TEST("loot waiting: each holder's moves in order, a waiting one holding back its later ones, other holders not")
{
	Waiting waiting;
	waiting.Add(Numbered(Put(0xA, 0xF, 5), 1, 2), T0);
	waiting.Add(Numbered(Take(0xA, 0xF, 5), 2, 2), T0);
	waiting.Add(Numbered(Take(0xB, 0xF, 1), 3, 2), T0);
	CHECK(waiting.Has(0xA));
	CHECK(!waiting.Has(0xC));
	std::vector<std::uint32_t> tried;
	waiting.Retry(T0, [&](const LootEntry& a_move) {
		tried.push_back(a_move.number);
		return a_move.holderId == 0xA ? Verdict::kWait : Verdict::kMake;
	});
	CHECK((tried == std::vector<std::uint32_t>{ 1, 3 }));
	CHECK(waiting.Size() == 2);
	tried.clear();
	waiting.Retry(T0, [&](const LootEntry& a_move) {
		tried.push_back(a_move.number);
		return a_move.number == 1 ? Verdict::kMake : Verdict::kSkip;
	});
	CHECK((tried == std::vector<std::uint32_t>{ 1, 2 }));
	CHECK(waiting.Size() == 0);

	std::vector<std::uint32_t> all;
	waiting.Add(Numbered(Take(0xA, 0xF, 1), 4, 2), T0);
	waiting.Add(Numbered(Take(0xA, 0xF, 1), 5, 2), T0);
	waiting.ForEach([&](const LootEntry& a_move) { all.push_back(a_move.number); });
	CHECK((all == std::vector<std::uint32_t>{ 4, 5 }));
	waiting.Clear();
	CHECK(waiting.Size() == 0);
}

TEST("loot waiting: a body alive here is waited for LIVING_WAIT, counted from when it was first seen alive; past MAX_WAITING the oldest goes")
{
	Waiting waiting;
	waiting.Add(Numbered(Take(0xA, 0xF, 1), 1, 2), T0);
	const auto alive = [](const LootEntry&) { return Verdict::kDead; };
	waiting.Retry(T0 + 1h, alive);  // unloaded an hour, then seen alive: the wait starts now
	waiting.Retry(T0 + 1h + Waiting::LIVING_WAIT, alive);
	CHECK(waiting.Size() == 1);
	waiting.Retry(T0 + 1h + Waiting::LIVING_WAIT + 1s, alive);
	CHECK(waiting.Size() == 0);
	CHECK(waiting.Expired() == 1);

	// Unloaded again in between: the living wait starts over.
	waiting.Add(Numbered(Take(0xA, 0xF, 1), 2, 2), T0);
	waiting.Retry(T0, alive);
	waiting.Retry(T0 + 9min, [](const LootEntry&) { return Verdict::kWait; });
	waiting.Retry(T0 + 12min, alive);
	waiting.Retry(T0 + 21min, alive);
	CHECK(waiting.Size() == 1);

	waiting.Clear();
	for (std::uint32_t i = 0; i <= Waiting::MAX_WAITING; ++i) {
		waiting.Add(Numbered(Take(0xA, 0xF, 1), i + 1, 2), T0);
	}
	CHECK(waiting.Size() == Waiting::MAX_WAITING);
	CHECK(waiting.Dropped() == 1);
	std::uint32_t first = 0;
	waiting.ForEach([&](const LootEntry& a_move) {
		if (first == 0) {
			first = a_move.number;
		}
	});
	CHECK(first == 2);
}

// ---- the emptied marker -------------------------------------------------------------------------------------------

TEST("loot marker: what another player took from a holder comes out of our copy, but the puts the sender had not seen stay")
{
	TakenBook book;
	book.Took(0xA, 0x100, true);
	book.Took(0xA, 0x200, false);  // our own take says nothing of the other copy
	book.Took(0xA, 0x300, true);
	book.Put(0xA, 0x100, 2, 5);  // numbered after the sender's ledger: it had not seen it
	book.Put(0xA, 0x100, 4, 2);  // before: the sender's take took it
	book.Took(0xB, 0x100, true);
	const auto out = book.Emptied(0xA, 4, [](std::uint32_t a_item) { return a_item == 0x300 ? 3u : 0u; });
	REQUIRE(out.size() == 2);
	CHECK((out[0] == std::pair<std::uint32_t, std::uint32_t>{ 0x100, 2 }));
	CHECK((out[1] == std::pair<std::uint32_t, std::uint32_t>{ 0x300, 3 }));  // our put still out
	CHECK(book.Emptied(0xA, 4, [](std::uint32_t) { return 0u; }).empty());  // the holder starts again
	CHECK(book.Size() == 1);
	CHECK(book.Emptied(0xC, 1, [](std::uint32_t) { return 0u; }).empty());

	CHECK(EmptyHere(10, 2) == 8);
	CHECK(EmptyHere(2, 5) == 0);
	CHECK(EmptyHere(-1, 0) == 0);
	CHECK(TakeHere(5, 3) == 3);
	CHECK(TakeHere(2, 10) == 2);
	CHECK(TakeHere(5, 0) == 0);
	CHECK(TakeHere(5, -2) == 0);
}

TEST("loot marker: said once each time our take leaves the holder holding nothing shared, again only after it held something")
{
	EmptyWatch watch;
	CHECK(!watch.AfterTake(0xA, true));
	CHECK(watch.AfterTake(0xA, false));
	CHECK(!watch.AfterTake(0xA, false));
	CHECK(watch.AfterTake(0xB, false));
	watch.AfterPut(0xA);
	CHECK(watch.AfterTake(0xA, false));
	CHECK(!watch.AfterTake(0xA, true));
	CHECK(watch.AfterTake(0xA, false));
	watch.Clear();
	CHECK(watch.AfterTake(0xB, false));
}

TEST("loot again: a move answered busy goes again AGAIN_AFTER later, as it was, without the answer")
{
	Again again;
	auto  busy = Take(0xA, 0xF, 3);
	busy.answer = LootEntry::kBusy;
	again.Add(busy, T0);
	CHECK(again.Due(T0).empty());
	const auto due = again.Due(T0 + Again::AGAIN_AFTER);
	REQUIRE(due.size() == 1);
	CHECK(due[0] == Take(0xA, 0xF, 3));
	CHECK(again.Size() == 0);
}
