#include "Test.h"

#include "FormTranslate.h"
#include "PickupBook.h"
#include "ServerFixture.h"
#include "game/PickupRules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

// What players take from the world (docs/DESIGN.md, "Taken from the world"): the messages (common/Protocol.h), their form
// IDs on the wire (common/FormTranslate.h), the server's book (common/PickupBook.h) and its handling (common/Server.cpp),
// and each game's rules (src/game/PickupRules.h). What reads and changes the game itself (src/game/Pickups.cpp) is tested
// in a game only: tools/rig/steps/pickups.sh.

using Protocol::Pickup;
using Protocol::PickupBatch;

namespace
{
	using MT = Protocol::MessageType;
	using namespace std::chrono_literals;
	using Clock = PickupRules::Clock;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	constexpr std::uint32_t SWEETROLL = 0x00064B3D;
	constexpr std::uint32_t DAGGER = 0x0001397E;
	constexpr std::uint32_t NIRNROOT = 0x000B6FBE;  // a flora

	Pickup Taken(std::uint32_t a_ref, std::uint32_t a_base = SWEETROLL)
	{
		return Pickup{ .ref = a_ref, .base = a_base, .kind = Pickup::kTaken };
	}

	Pickup Harvest(std::uint32_t a_ref, std::uint32_t a_base = NIRNROOT)
	{
		return Pickup{ .ref = a_ref, .base = a_base, .kind = Pickup::kHarvested };
	}

	std::vector<std::uint8_t> Batch(std::vector<Pickup> a_entries)
	{
		PickupBatch batch;
		batch.entries = std::move(a_entries);
		return Protocol::Encode(batch);
	}

	std::vector<std::uint8_t> Ask()
	{
		return Protocol::Writer{ MT::kAskPickups }.Data();
	}

	// The kRemotePickups batches among a_packets.
	std::vector<PickupBatch> Heard(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<PickupBatch> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemotePickups) {
				if (const auto batch = Protocol::DecodePickupBatch(packet)) {
					out.push_back(*batch);
				}
			}
		}
		return out;
	}

	std::vector<Pickup> Entries(const std::vector<PickupBatch>& a_batches)
	{
		std::vector<Pickup> out;
		for (const auto& batch : a_batches) {
			out.insert(out.end(), batch.entries.begin(), batch.entries.end());
		}
		return out;
	}

	const Clock::time_point T0 = Clock::time_point{} + 1h;
}

// ---- the messages -------------------------------------------------------------------------------------------------

TEST("pickups: a game's batch and the server's go there and back as they were; the server's fields never leave a game")
{
	CHECK(Protocol::VERSION >= 21);
	PickupBatch ours;
	ours.entries = { Taken(0x0001A2B3), Harvest(0x0003C001), Taken(0x02000D10, DAGGER) };
	const auto packet = Protocol::Encode(ours);
	CHECK(Protocol::PeekType(packet) == MT::kPickups);
	CHECK(packet.size() == 2 + 3 * 9);
	CHECK(Protocol::DecodePickupBatch(packet) == ours);

	// A game's own entry carries no player and no age, whatever it holds.
	auto told = ours;
	told.entries[0].playerId = 7;
	told.entries[0].ageHours = 3.0f;
	CHECK(Protocol::DecodePickupBatch(Protocol::Encode(told)) == ours);

	PickupBatch theirs;
	theirs.catchUp = true;
	theirs.entries = { Pickup{ .ref = 0x0001A2B3, .base = SWEETROLL, .kind = Pickup::kTaken, .playerId = 2 },
		Pickup{ .ref = 0x0003C001, .base = NIRNROOT, .kind = Pickup::kHarvested, .playerId = 3, .ageHours = 48.5f },
		Pickup{ .ref = 0x0003C002, .base = NIRNROOT, .kind = Pickup::kHarvested, .playerId = 3, .ageHours = 0.0f } };
	const auto remote = Protocol::Encode(theirs, MT::kRemotePickups);
	CHECK(Protocol::PeekType(remote) == MT::kRemotePickups);
	CHECK(Protocol::DecodePickupBatch(remote) == theirs);
	theirs.catchUp = false;
	CHECK(Protocol::DecodePickupBatch(Protocol::Encode(theirs, MT::kRemotePickups))->catchUp == false);

	// The ask is its type alone.
	const auto ask = Ask();
	CHECK(ask.size() == 1);
	CHECK(Protocol::PeekType(ask) == MT::kAskPickups);
	CHECK(!Protocol::DecodePickupBatch(ask));
}

TEST("pickups: a batch with no entry, too many, an entry of no reference or base, a kind or an age not known, or bytes left over is not read")
{
	CHECK(!Protocol::DecodePickupBatch(Batch({})));
	CHECK(!Protocol::DecodePickupBatch(Batch({ Taken(0) })));
	CHECK(!Protocol::DecodePickupBatch(Batch({ Taken(0x1A2B3, 0) })));
	auto odd = Taken(0x1A2B3);
	odd.kind = Pickup::KIND_MAX + 1;
	CHECK(!Protocol::DecodePickupBatch(Batch({ odd })));

	for (const float age : { -2.0f, -0.5f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), Protocol::MAX_PICKUP_AGE_HOURS * 2.0f }) {
		PickupBatch batch;
		batch.entries = { Pickup{ .ref = 0x1A2B3, .base = NIRNROOT, .kind = Pickup::kHarvested, .playerId = 1, .ageHours = age } };
		CHECK(!Protocol::DecodePickupBatch(Protocol::Encode(batch, MT::kRemotePickups)));
	}

	// The count byte saying more than MAX_PICKUP_BATCH (the encoder never writes one).
	auto many = Batch({ Taken(0x1A2B3) });
	many[1] = static_cast<std::uint8_t>(Protocol::MAX_PICKUP_BATCH + 1);
	CHECK(!Protocol::DecodePickupBatch(many));

	auto longer = Batch({ Taken(0x1A2B3) });
	longer.push_back(0);
	CHECK(!Protocol::DecodePickupBatch(longer));
	auto shorter = Batch({ Taken(0x1A2B3) });
	shorter.pop_back();
	CHECK(!Protocol::DecodePickupBatch(shorter));

	PickupBatch flagged;
	flagged.entries = { Taken(0x1A2B3) };
	auto remote = Protocol::Encode(flagged, MT::kRemotePickups);
	remote[1] = 2;  // a flag not known
	CHECK(!Protocol::DecodePickupBatch(remote));
}

TEST("pickups: a long list goes in batches of MAX_PICKUP_BATCH, each marked a catch-up when asked; none for none")
{
	CHECK(Protocol::PickupBatches({}, MT::kRemotePickups, true).empty());
	std::vector<Pickup> list;
	for (std::uint32_t i = 1; i <= 130; ++i) {
		list.push_back(Pickup{ .ref = 0x00100000 + i, .base = SWEETROLL, .kind = Pickup::kTaken, .playerId = 2 });
	}
	const auto packets = Protocol::PickupBatches(list, MT::kRemotePickups, true);
	REQUIRE(packets.size() == 3);
	std::vector<Pickup> back;
	for (const auto& packet : packets) {
		const auto batch = Protocol::DecodePickupBatch(packet);
		REQUIRE(batch);
		CHECK(batch->catchUp);
		CHECK(batch->entries.size() <= Protocol::MAX_PICKUP_BATCH);
		back.insert(back.end(), batch->entries.begin(), batch->entries.end());
	}
	CHECK(back == list);
	const auto ours = Protocol::PickupBatches(std::vector<Pickup>(list.begin(), list.begin() + 3), MT::kPickups);
	REQUIRE(ours.size() == 1);
	CHECK(Protocol::PeekType(ours[0]) == MT::kPickups);
}

TEST("pickups: references and bases are named by their plugin on the wire; an entry a game lacks is left out, and a batch left with none is not sent")
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

	CHECK(FormTranslate::CarriesForms(MT::kPickups));
	CHECK(FormTranslate::CarriesForms(MT::kRemotePickups));
	CHECK(!FormTranslate::CarriesForms(MT::kAskPickups));

	// The guest took a sword of B.esp (its 01) lying in Skyrim.esm's cell, harvested a plant of Update.esm (its 02) whose
	// reference is Skyrim.esm's, and took an item of Extra.esp (its 04), which the host lacks.
	const auto out = FormTranslate::Translate(guestEdge, Way::kToSession,
		Batch({ Taken(0x00012345, 0x01000800), Harvest(0x00023456, 0x02000777), Taken(0x04000D11, SWEETROLL) }));
	REQUIRE(out.kind == Result::Kind::kChanged);
	const auto told = Protocol::DecodePickupBatch(out.packet);
	REQUIRE(told);
	REQUIRE(told->entries.size() == 3);
	CHECK(told->entries[0].base == 0x03000800);  // B.esp is the session's 03
	CHECK(told->entries[1].base == 0x01000777);  // Update.esm its 01
	CHECK(told->entries[1].kind == Pickup::kHarvested);
	CHECK(told->entries[2].ref == 0x04000D11);   // Extra.esp its 04

	// The server passes them on under the guest's number; the host lacks Extra.esp: that one is left out (counted).
	PickupBatch passed = *told;
	for (auto& entry : passed.entries) {
		entry.playerId = 2;
	}
	const auto atHost = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(passed, MT::kRemotePickups));
	REQUIRE(atHost.kind == Result::Kind::kChanged);
	CHECK(atHost.entries == 1);
	const auto heard = Protocol::DecodePickupBatch(atHost.packet);
	REQUIRE(heard);
	REQUIRE(heard->entries.size() == 2);
	CHECK(heard->entries[0].base == 0x03000800);  // the host's B.esp is 03 too
	CHECK(heard->entries[0].playerId == 2);
	CHECK(heard->entries[1].ref == 0x00023456);

	// Only what the host lacks: nothing to send.
	PickupBatch extraOnly;
	extraOnly.entries = { Pickup{ .ref = 0x04000D11, .base = SWEETROLL, .kind = Pickup::kTaken, .playerId = 2 } };
	CHECK(FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(extraOnly, MT::kRemotePickups)).kind == Result::Kind::kDrop);
	// ... nor a base it lacks.
	extraOnly.entries[0] = Pickup{ .ref = 0x00012345, .base = 0x04000800, .kind = Pickup::kTaken, .playerId = 2 };
	CHECK(FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(extraOnly, MT::kRemotePickups)).kind == Result::Kind::kDrop);

	// A runtime reference has no ID anywhere: left out on its way out.
	const auto runtime = FormTranslate::Translate(guestEdge, Way::kToSession, Batch({ Taken(0xFF000123), Taken(0x00012345) }));
	REQUIRE(runtime.kind == Result::Kind::kChanged);
	const auto kept = Protocol::DecodePickupBatch(runtime.packet);
	REQUIRE(kept);
	REQUIRE(kept->entries.size() == 1);
	CHECK(kept->entries[0].ref == 0x00012345);
}

// ---- the server's book --------------------------------------------------------------------------------------------

TEST("pickup book: a loose item is taken once, by its first picker; nobody is sent their own, the rest in the order taken")
{
	PickupBook book;
	Pickup     out;
	CHECK(book.Report(Taken(0x1A2B3), 1, std::nullopt, out) == PickupBook::Take::kNews);
	CHECK(out.playerId == 1);
	CHECK(out.ageHours == Pickup::AGE_UNKNOWN);
	CHECK(book.Report(Taken(0x1A2B3), 2, std::nullopt, out) == PickupBook::Take::kSame);  // both had it: the first kept
	CHECK(book.Report(Taken(0x1A2B3), 1, 5.0, out) == PickupBook::Take::kSame);
	CHECK(book.Report(Taken(0x1A2B4, DAGGER), 2, 5.0, out) == PickupBook::Take::kNews);
	CHECK(book.Report(Harvest(0x3C001), 1, 5.0, out) == PickupBook::Take::kNews);
	CHECK(book.Size() == 3);
	CHECK(book.Has(0x1A2B3));
	CHECK(!book.Has(0x1A2B5));

	const auto forThree = book.For(3, 5.0);
	REQUIRE(forThree.size() == 3);
	CHECK(forThree[0].ref == 0x1A2B3);
	CHECK(forThree[0].playerId == 1);
	CHECK(forThree[1].ref == 0x1A2B4);
	CHECK(forThree[1].base == DAGGER);
	CHECK(forThree[2].ref == 0x3C001);
	const auto forOne = book.For(1, 5.0);
	REQUIRE(forOne.size() == 1);
	CHECK(forOne[0].ref == 0x1A2B4);
	CHECK(book.For(2, 5.0).size() == 2);

	// A reference keeps the kind it came with.
	CHECK(book.Report(Harvest(0x1A2B3), 2, 9.0, out) == PickupBook::Take::kSame);
	CHECK(book.Report(Taken(0x3C001, NIRNROOT), 2, 9.0, out) == PickupBook::Take::kSame);
	CHECK(book.For(3, 5.0)[0].kind == Pickup::kTaken);

	book.Clear();
	CHECK(book.Size() == 0);
	CHECK(book.For(3, 5.0).empty());
}

TEST("pickup book: no reference, a runtime one, no base or a kind not known is refused; a full book keeps no new one")
{
	PickupBook book{ 2 };
	Pickup     out;
	CHECK(book.Report(Taken(0), 1, std::nullopt, out) == PickupBook::Take::kRefused);
	CHECK(book.Report(Taken(0xFF000801), 1, std::nullopt, out) == PickupBook::Take::kRefused);
	CHECK(book.Report(Taken(0x1A2B3, 0), 1, std::nullopt, out) == PickupBook::Take::kRefused);
	auto odd = Taken(0x1A2B3);
	odd.kind = 9;
	CHECK(book.Report(odd, 1, std::nullopt, out) == PickupBook::Take::kRefused);
	CHECK(book.Size() == 0);

	CHECK(book.Report(Taken(0x1), 1, std::nullopt, out) == PickupBook::Take::kNews);
	CHECK(book.Report(Harvest(0x2), 1, 1.0, out) == PickupBook::Take::kNews);
	CHECK(book.Report(Taken(0x3), 1, std::nullopt, out) == PickupBook::Take::kFull);
	CHECK(book.Size() == 2);
	// What it holds still takes reports: a harvest said again later is news.
	CHECK(book.Report(Taken(0x1), 2, std::nullopt, out) == PickupBook::Take::kSame);
	CHECK(book.Report(Harvest(0x2), 2, 3.0, out) == PickupBook::Take::kNews);
}

TEST("pickup book: a harvest said again an hour of the session's clock later is news, by its new harvester; ages by the clock now")
{
	PickupBook book;
	Pickup     out;
	CHECK(book.Report(Harvest(0x3C001), 1, 10.0, out) == PickupBook::Take::kNews);
	CHECK(out.ageHours == 0.0f);
	CHECK(book.Report(Taken(0x1A2B3), 1, 10.0, out) == PickupBook::Take::kNews);
	CHECK(book.Report(Harvest(0x3C001), 2, 10.0, out) == PickupBook::Take::kSame);
	CHECK(book.Report(Harvest(0x3C001), 2, 10.0 + 0.5 / 24.0, out) == PickupBook::Take::kSame);
	CHECK(book.Report(Harvest(0x3C001), 2, std::nullopt, out) == PickupBook::Take::kSame);  // no clock: not later

	// Two days later by the session's clock.
	auto aged = book.For(3, 12.0);
	REQUIRE(aged.size() == 2);
	CHECK(aged[0].ref == 0x3C001);
	CHECK(std::abs(aged[0].ageHours - 48.0f) < 0.01f);
	CHECK(aged[1].ageHours == Pickup::AGE_UNKNOWN);  // a loose item has no age
	// A clock gone back (a load) ages it 0; no clock now, not known.
	CHECK(book.For(3, 9.0)[0].ageHours == 0.0f);
	CHECK(book.For(3, std::nullopt)[0].ageHours == Pickup::AGE_UNKNOWN);
	CHECK(book.For(3, std::numeric_limits<double>::quiet_NaN())[0].ageHours == Pickup::AGE_UNKNOWN);

	// Harvested again by player 2: its harvester now, said last.
	CHECK(book.Report(Harvest(0x3C001), 2, 12.0, out) == PickupBook::Take::kNews);
	CHECK(out.playerId == 2);
	CHECK(out.ageHours == 0.0f);
	aged = book.For(3, 12.5);
	REQUIRE(aged.size() == 2);
	CHECK(aged[1].ref == 0x3C001);
	CHECK(std::abs(aged[1].ageHours - 12.0f) < 0.01f);
	CHECK(book.For(2, 12.5).size() == 1);  // its own now
	CHECK(book.For(1, 12.5).size() == 1);  // player 1's loose item is theirs; the harvest is player 2's

	// A harvest kept with no clock is news when said with one; one said with a clock that is no number is kept with none.
	CHECK(book.Report(Harvest(0x3C002), 1, std::numeric_limits<double>::infinity(), out) == PickupBook::Take::kNews);
	CHECK(out.ageHours == Pickup::AGE_UNKNOWN);
	CHECK(book.Report(Harvest(0x3C002), 2, 1.0, out) == PickupBook::Take::kNews);
	CHECK(out.playerId == 2);
}

// ---- the server ---------------------------------------------------------------------------------------------------

TEST("server: a pickup goes to the others under its picker's number, never back to the picker; the same again is not news")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	const auto cy = f.Join(3, Fixture::MakeHello("Cy"));
	REQUIRE(ada);
	REQUIRE(bo);
	REQUIRE(cy);
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);
	(void)f.fake->Take(3);

	f.fake->Deliver(1, Batch({ Taken(0x1A2B3), Harvest(0x3C001) }));
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto heard = Heard(f.fake->TakeThrough(peer, Type(MT::kRemotePickups)));
		REQUIRE(heard.size() == 1);
		CHECK(!heard[0].catchUp);
		REQUIRE(heard[0].entries.size() == 2);
		CHECK(heard[0].entries[0].ref == 0x1A2B3);
		CHECK(heard[0].entries[0].playerId == ada->playerId);
		CHECK(heard[0].entries[1].kind == Pickup::kHarvested);
		CHECK(heard[0].entries[1].ageHours == Pickup::AGE_UNKNOWN);  // no clock in this session yet
	}
	REQUIRE(f.fake->Settle());
	CHECK(Heard(f.fake->Take(1)).empty());

	// Said again, by Ada or by Bo (who took it too before hearing): nothing new for anyone. A runtime reference: kept by none.
	f.fake->Deliver(1, Batch({ Taken(0x1A2B3) }));
	f.fake->Deliver(2, Batch({ Taken(0x1A2B3), Harvest(0x3C001), Taken(0xFF000801) }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 2 }, PeerId{ 3 } }) {
		CHECK(Heard(f.fake->Take(peer)).empty());
	}
	// One new among old ones: that one alone.
	f.fake->Deliver(2, Batch({ Taken(0x1A2B3), Taken(0x1A2B4, DAGGER) }));
	const auto news = Entries(Heard(f.fake->TakeThrough(1, Type(MT::kRemotePickups))));
	REQUIRE(news.size() == 1);
	CHECK(news[0].ref == 0x1A2B4);
	CHECK(news[0].playerId == bo->playerId);
}

TEST("server: a joiner hears every kept pickup but its own as a catch-up; a game that loaded asks and hears them again")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	std::vector<Pickup> adas;
	for (std::uint32_t i = 1; i <= 70; ++i) {
		adas.push_back(Taken(0x00100000 + i));
	}
	for (const auto& packet : Protocol::PickupBatches(adas, MT::kPickups)) {
		f.fake->Deliver(1, packet);
	}
	f.fake->Deliver(2, Batch({ Harvest(0x3C001) }));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	const auto cy = f.Join(3, Fixture::MakeHello("Cy"));
	REQUIRE(cy);
	REQUIRE(f.fake->Settle());
	const auto caught = Heard(f.fake->Take(3));
	REQUIRE(caught.size() == 2);
	CHECK(std::ranges::all_of(caught, &PickupBatch::catchUp));
	const auto all = Entries(caught);
	REQUIRE(all.size() == 71);
	CHECK(all[0].ref == 0x00100001);
	CHECK(all[69].ref == 0x00100046);
	CHECK(all[70].ref == 0x3C001);
	CHECK(all[70].playerId == bo->playerId);
	CHECK(Heard(f.fake->Take(1)).empty());  // a joiner's welcome sends the others nothing of it

	// Bo loads a save: it asks, and hears Ada's, never its own harvest (its save has the plant as it was: Bo's to harvest).
	f.fake->Deliver(2, Ask());
	REQUIRE(f.fake->Settle());
	const auto again = Entries(Heard(f.fake->Take(2)));
	REQUIRE(again.size() == 70);
	CHECK(std::ranges::none_of(again, [](const Pickup& a_p) { return a_p.ref == 0x3C001; }));
	// An ask with more bytes is no ask.
	auto odd = Ask();
	odd.push_back(0);
	f.fake->Deliver(2, odd);
	REQUIRE(f.fake->Settle());
	CHECK(Heard(f.fake->Take(2)).empty());
}

TEST("server: a harvest is aged by the session's clock for a joiner; harvested again a game hour later it is the new harvester's")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));  // the clock's owner: in the longest
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	f.fake->Deliver(1, Protocol::Encode(Protocol::ClockState{ .gameHour = 9.0f, .daysPassed = 10.0f, .timeScale = 20.0f, .worldspace = 0x3C }));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kRemoteClock)));
	f.fake->Deliver(1, Batch({ Harvest(0x3C001) }));
	const auto first = Entries(Heard(f.fake->TakeThrough(2, Type(MT::kRemotePickups))));
	REQUIRE(first.size() == 1);
	CHECK(first[0].ageHours == 0.0f);

	// Two game days later Cy joins: the harvest is 48 hours old.
	f.fake->Deliver(1, Protocol::Encode(Protocol::ClockState{ .gameHour = 9.0f, .daysPassed = 12.0f, .timeScale = 20.0f, .worldspace = 0x3C }));
	REQUIRE(f.fake->Settle());
	const auto cy = f.Join(3, Fixture::MakeHello("Cy"));
	REQUIRE(cy);
	REQUIRE(f.fake->Settle());
	const auto caught = Entries(Heard(f.fake->Take(3)));
	REQUIRE(caught.size() == 1);
	CHECK(std::abs(caught[0].ageHours - 48.0f) < 0.01f);
	CHECK(caught[0].playerId == ada->playerId);

	// Bo harvests it again (it grew back in Bo's world): news to Ada and Cy, Bo's now.
	(void)f.fake->Take(1);
	f.fake->Deliver(2, Batch({ Harvest(0x3C001) }));
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 3 } }) {
		const auto again = Entries(Heard(f.fake->TakeThrough(peer, Type(MT::kRemotePickups))));
		REQUIRE(again.size() == 1);
		CHECK(again[0].playerId == bo->playerId);
		CHECK(again[0].ageHours == 0.0f);
	}
	// The same hour again: not news.
	f.fake->Deliver(3, Batch({ Harvest(0x3C001) }));
	REQUIRE(f.fake->Settle());
	CHECK(Heard(f.fake->Take(1)).empty());
	CHECK(Heard(f.fake->Take(2)).empty());
	// Ada asks: Bo's harvest is not Ada's own, so Ada hears it.
	f.fake->Deliver(1, Ask());
	const auto asked = Entries(Heard(f.fake->TakeThrough(1, Type(MT::kRemotePickups))));
	REQUIRE(asked.size() == 1);
	CHECK(asked[0].playerId == bo->playerId);
}

TEST("server: past its budget a player's pickup batches are dropped, said in the log once; a new session starts with none kept")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	constexpr std::uint32_t BURST = 60;  // the budget holds 32 at once, and 8 more a second
	for (std::uint32_t i = 1; i <= BURST; ++i) {
		f.fake->Deliver(1, Batch({ Taken(0x00200000 + i) }));
	}
	REQUIRE(f.fake->Settle());
	const auto passed = Heard(f.fake->Take(2)).size();
	CHECK(passed >= 32);
	CHECK(passed < BURST);
	CHECK(f.LoggedSoon("faster than the server passes it on"));
	const auto lines = f.LogLines();
	CHECK(std::ranges::count_if(lines, [](const std::string& a_l) { return a_l.find("faster than the server passes it on") != std::string::npos; }) == 1);

	// Everyone leaves; the next player in starts a new session: nothing kept.
	f.fake->Drop(1);
	f.fake->Drop(2);
	REQUIRE(f.fake->Settle());
	const auto cy = f.Join(3, Fixture::MakeHello("Cy"));
	REQUIRE(cy);
	f.fake->Deliver(3, Ask());
	REQUIRE(f.fake->Settle());
	CHECK(Heard(f.fake->Take(3)).empty());
}

// ---- each game's rules --------------------------------------------------------------------------------------------

TEST("pickup rules: every loose item kind is the session's; keys, books, a quest's, a runtime reference and what is no item are not")
{
	using PickupRules::ItemKind;
	using PickupRules::Why;
	for (const auto kind : { ItemKind::kWeapon, ItemKind::kArmor, ItemKind::kAmmo, ItemKind::kPotion, ItemKind::kIngredient, ItemKind::kMisc,
			 ItemKind::kSoulGem, ItemKind::kScroll, ItemKind::kLight, ItemKind::kApparatus }) {
		CHECK(PickupRules::Takeable(kind, false, false) == Why::kShared);
		CHECK(PickupRules::Takeable(kind, true, false) == Why::kRuntime);
		CHECK(PickupRules::Takeable(kind, false, true) == Why::kQuest);
	}
	CHECK(PickupRules::Takeable(ItemKind::kKey, false, false) == Why::kKey);
	CHECK(PickupRules::Takeable(ItemKind::kBook, false, false) == Why::kBook);
	CHECK(PickupRules::Takeable(ItemKind::kOther, false, false) == Why::kNotItem);
	CHECK(PickupRules::Harvestable(true, false, false) == Why::kShared);
	CHECK(PickupRules::Harvestable(false, false, false) == Why::kNotItem);
	CHECK(PickupRules::Harvestable(true, true, false) == Why::kRuntime);
	CHECK(PickupRules::Harvestable(true, false, true) == Why::kQuest);
	for (std::size_t i = 0; i < PickupRules::WHY_COUNT; ++i) {
		CHECK(std::string{ PickupRules::WhyName(static_cast<Why>(i)) } != "?");
	}
}

TEST("pickup rules: the receiver takes its own reference out, or knows it gone, held or not loaded; another base, a key or a quest's stays")
{
	using PickupRules::Facts;
	using PickupRules::ItemKind;
	using PickupRules::Judged;
	using PickupRules::Verdict;
	using PickupRules::Why;
	const Facts lying{ .found = true, .base = SWEETROLL, .item = ItemKind::kPotion, .inCell = true };
	const auto  with = [&](auto a_change, const Pickup& a_entry = Taken(0x1A2B3)) {
		auto facts = lying;
		a_change(facts);
		return PickupRules::Receive(a_entry, facts, 240.0f);
	};
	const auto same = [](Facts&) {};
	CHECK((with(same) == Judged{ Verdict::kApply, Why::kShared }));
	CHECK((with([](Facts& a_f) { a_f.disabled = true; }) == Judged{ Verdict::kAlready, Why::kShared }));
	CHECK((with([](Facts& a_f) { a_f.deleted = true; }) == Judged{ Verdict::kAlready, Why::kShared }));
	CHECK((with([](Facts& a_f) { a_f.inCell = false; }) == Judged{ Verdict::kHeld, Why::kShared }));
	CHECK((with([](Facts& a_f) { a_f = Facts{}; }) == Judged{ Verdict::kWait, Why::kShared }));
	CHECK((with([](Facts& a_f) { a_f.base = DAGGER; }) == Judged{ Verdict::kSkip, Why::kOtherBase }));
	CHECK((with([](Facts& a_f) { a_f.item = ItemKind::kKey; }) == Judged{ Verdict::kSkip, Why::kKey }));
	CHECK((with([](Facts& a_f) { a_f.item = ItemKind::kBook; }) == Judged{ Verdict::kSkip, Why::kBook }));
	CHECK((with([](Facts& a_f) { a_f.item = ItemKind::kOther; }) == Judged{ Verdict::kSkip, Why::kNotItem }));
	CHECK((with([](Facts& a_f) { a_f.questAlias = true; }) == Judged{ Verdict::kSkip, Why::kQuest }));
	CHECK((with(same, Taken(0xFF000123)) == Judged{ Verdict::kSkip, Why::kRuntime }));
}

TEST("pickup rules: a harvest is made on a plant not harvested here; one older than this game's respawn time has grown back")
{
	using PickupRules::Facts;
	using PickupRules::Judged;
	using PickupRules::Verdict;
	using PickupRules::Why;
	const Facts plant{ .found = true, .base = NIRNROOT, .plant = true, .inCell = true };
	auto        entry = Harvest(0x3C001);
	CHECK((PickupRules::Receive(entry, plant, 240.0f) == Judged{ Verdict::kApply, Why::kShared }));
	auto harvested = plant;
	harvested.harvested = true;
	CHECK((PickupRules::Receive(entry, harvested, 240.0f) == Judged{ Verdict::kAlready, Why::kShared }));
	auto notPlant = plant;
	notPlant.plant = false;
	CHECK((PickupRules::Receive(entry, notPlant, 240.0f) == Judged{ Verdict::kSkip, Why::kNotItem }));
	auto quest = plant;
	quest.questAlias = true;
	CHECK((PickupRules::Receive(entry, quest, 240.0f) == Judged{ Verdict::kSkip, Why::kQuest }));
	CHECK((PickupRules::Receive(entry, Facts{}, 240.0f) == Judged{ Verdict::kWait, Why::kShared }));

	entry.ageHours = 239.0f;
	CHECK((PickupRules::Receive(entry, plant, 240.0f) == Judged{ Verdict::kApply, Why::kShared }));
	entry.ageHours = 240.0f;
	CHECK((PickupRules::Receive(entry, plant, 240.0f) == Judged{ Verdict::kSkip, Why::kRegrown }));
	CHECK((PickupRules::Receive(entry, plant, 0.0f) == Judged{ Verdict::kApply, Why::kShared }));  // no respawn time known
	entry.ageHours = Pickup::AGE_UNKNOWN;
	CHECK((PickupRules::Receive(entry, plant, 240.0f) == Judged{ Verdict::kApply, Why::kShared }));
	// A harvested plant's reference is never "held" (it never leaves its cell): not in a cell still harvests it.
	auto loose = plant;
	loose.inCell = false;
	CHECK((PickupRules::Receive(entry, loose, 240.0f) == Judged{ Verdict::kApply, Why::kShared }));
}

TEST("pickup rules: a harvest is read from our player's activation, before or after the harvest; one no activation explains asks for a scan")
{
	PickupRules::Harvests harvests;
	std::map<std::uint32_t, std::optional<bool>> world;  // what each reference reads now
	const auto read = [&](std::uint32_t a_ref) -> std::optional<bool> {
		const auto it = world.find(a_ref);
		return it != world.end() ? it->second : std::nullopt;
	};

	// The activation's event came before the harvest: watched until it reads harvested.
	world[0xA1] = false;
	harvests.Activated(0xA1, NIRNROOT, false, T0);
	auto due = harvests.Tend(T0 + 16ms, read);
	CHECK(due.refs.empty());
	CHECK(harvests.Watched() == 1);
	world[0xA1] = true;
	harvests.Harvested(NIRNROOT, T0 + 20ms);
	due = harvests.Tend(T0 + 32ms, read);
	CHECK((due.refs == std::vector<std::uint32_t>{ 0xA1 }));
	CHECK(harvests.Watched() == 0);
	due = harvests.Tend(T0 + 5s, read);  // its harvest event was taken by it: no scan
	CHECK(due.scans.empty());

	// The event came after: harvested already, and a harvest of its produce within PAIR makes it ours.
	world[0xA2] = true;
	harvests.Activated(0xA2, NIRNROOT, true, T0 + 10s);
	harvests.Harvested(NIRNROOT, T0 + 10s + 5ms);
	due = harvests.Tend(T0 + 10s + 16ms, read);
	CHECK((due.refs == std::vector<std::uint32_t>{ 0xA2 }));

	// Harvested before (it gives nothing, no harvest comes): not ours, forgotten after WATCH.
	world[0xA3] = true;
	harvests.Activated(0xA3, NIRNROOT, true, T0 + 20s);
	CHECK(harvests.Tend(T0 + 21s, read).refs.empty());
	CHECK(harvests.Watched() == 1);
	CHECK(harvests.Tend(T0 + 24s, read).refs.empty());
	CHECK(harvests.Watched() == 0);

	// Not harvested, and never: forgotten after WATCH. Gone from memory: forgotten at once.
	world[0xA4] = false;
	harvests.Activated(0xA4, NIRNROOT, false, T0 + 30s);
	CHECK(harvests.Tend(T0 + 34s, read).refs.empty());
	CHECK(harvests.Watched() == 0);
	harvests.Activated(0xA5, NIRNROOT, false, T0 + 40s);
	CHECK(harvests.Tend(T0 + 40s + 16ms, read).refs.empty());
	CHECK(harvests.Watched() == 0);

	// A plant of a leveled produce (0) pairs with whatever item the harvest names.
	world[0xA6] = true;
	harvests.Activated(0xA6, 0, true, T0 + 50s);
	harvests.Harvested(0x0006BC00, T0 + 50s + 10ms);
	CHECK((harvests.Tend(T0 + 50s + 16ms, read).refs == std::vector<std::uint32_t>{ 0xA6 }));

	// A harvest of a produce another plant's activation does not pair with: unexplained, so a scan (once a produce).
	world[0xA7] = true;
	harvests.Activated(0xA7, NIRNROOT, true, T0 + 60s);
	harvests.Harvested(0x0003AD5B, T0 + 60s);
	harvests.Harvested(0x0003AD5B, T0 + 60s + 100ms);
	harvests.Harvested(0x0004DA22, T0 + 60s + 100ms);
	due = harvests.Tend(T0 + 61s, read);
	CHECK(due.refs.empty());
	CHECK(due.scans.empty());  // within PAIR: still waiting for its activation
	due = harvests.Tend(T0 + 63s, read);
	CHECK(due.refs.empty());
	REQUIRE(due.scans.size() == 2);
	CHECK(due.scans[0] == 0x0003AD5B);
	CHECK(due.scans[1] == 0x0004DA22);
	CHECK(harvests.Tend(T0 + 64s, read).scans.empty());

	// Watched once a reference: a second activation replaces the first. Clear forgets all.
	world[0xA8] = false;
	harvests.Activated(0xA8, NIRNROOT, false, T0 + 70s);
	harvests.Activated(0xA8, NIRNROOT, false, T0 + 70s + 100ms);
	CHECK(harvests.Watched() == 1);
	harvests.Activated(0, NIRNROOT, false, T0 + 70s);
	CHECK(harvests.Watched() == 1);
	harvests.Clear();
	CHECK(harvests.Watched() == 0);
}

TEST("pickup rules: our entries go once each, at most MAX_PICKUP_BATCH a batch, a batch every PACE; past MAX the oldest go")
{
	PickupRules::Outbox outbox;
	CHECK(!outbox.Due(T0));
	CHECK(outbox.Add(Taken(0x1)));
	CHECK(!outbox.Add(Taken(0x1)));
	CHECK(outbox.Add(Harvest(0x1)));  // another kind of the same reference
	auto told = Taken(0x2);
	told.playerId = 9;
	told.ageHours = 5.0f;
	CHECK(outbox.Add(told));
	const auto first = outbox.Due(T0);
	REQUIRE(first);
	REQUIRE(first->size() == 3);
	CHECK((*first)[2].playerId == 0);  // ours: no player, no age
	CHECK((*first)[2].ageHours == Pickup::AGE_UNKNOWN);
	CHECK(outbox.Size() == 0);

	for (std::uint32_t i = 1; i <= 100; ++i) {
		CHECK(outbox.Add(Taken(0x100 + i)));
	}
	CHECK(!outbox.Due(T0 + 100ms));  // PACE after the last batch
	const auto second = outbox.Due(T0 + PickupRules::Outbox::PACE);
	REQUIRE(second);
	CHECK(second->size() == Protocol::MAX_PICKUP_BATCH);
	CHECK(second->front().ref == 0x101);
	CHECK(!outbox.Due(T0 + PickupRules::Outbox::PACE + 1ms));
	const auto third = outbox.Due(T0 + 2 * PickupRules::Outbox::PACE);
	REQUIRE(third);
	CHECK(third->size() == 100 - Protocol::MAX_PICKUP_BATCH);
	CHECK(!outbox.Due(T0 + 10s));

	for (std::uint32_t i = 1; i <= PickupRules::Outbox::MAX + 2; ++i) {
		(void)outbox.Add(Taken(0x10000 + i));
	}
	CHECK(outbox.Size() == PickupRules::Outbox::MAX);
	CHECK(outbox.Overflow() == 2);
	CHECK(outbox.Due(T0 + 20s)->front().ref == 0x10003);
	outbox.Clear();
	CHECK(outbox.Size() == 0);
}

TEST("pickup rules: what waits for its cell is kept by reference, the newest word on each, swept round a few at a time; a full list refuses new ones")
{
	PickupRules::Waiting waiting;
	CHECK(waiting.Add(Taken(0x1)));
	CHECK(waiting.Add(Harvest(0x2)));
	CHECK(waiting.Add(Taken(0x3)));
	auto newer = Harvest(0x2);
	newer.ageHours = 12.0f;
	CHECK(waiting.Add(newer));
	CHECK(waiting.Size() == 3);
	REQUIRE(waiting.Find(0x2));
	CHECK(waiting.Find(0x2)->ageHours == 12.0f);
	CHECK(!waiting.Find(0x4));

	auto swept = waiting.Sweep(2);
	REQUIRE(swept.size() == 2);
	CHECK(swept[0].ref == 0x1);
	CHECK(swept[1].ref == 0x2);
	swept = waiting.Sweep(2);
	REQUIRE(swept.size() == 2);
	CHECK(swept[0].ref == 0x3);
	CHECK(swept[1].ref == 0x1);
	waiting.Remove(0x2);
	swept = waiting.Sweep(5);  // each once a sweep at most
	CHECK(swept.size() == 2);
	CHECK(waiting.Size() == 2);
	waiting.Clear();
	CHECK(waiting.Size() == 0);
	CHECK(waiting.Sweep(5).empty());

	for (std::uint32_t i = 1; i <= PickupRules::Waiting::MAX; ++i) {
		REQUIRE(waiting.Add(Taken(0x100000 + i)));
	}
	CHECK(!waiting.Add(Taken(0x1)));
	CHECK(waiting.Refused() == 1);
	CHECK(waiting.Add(Harvest(0x100001)));  // a newer word on one it holds still goes in
}

TEST("pickup rules: the references told or made since the welcome are known, ours apart from the session's")
{
	PickupRules::Known known;
	known.Ours(0x1);
	known.Theirs(0x2);
	CHECK(known.Has(0x1));
	CHECK(known.Has(0x2));
	CHECK(!known.Has(0x3));
	CHECK(known.IsOurs(0x1));
	CHECK(!known.IsOurs(0x2));
	CHECK(known.Size() == 2);
	known.Clear();
	CHECK(!known.Has(0x1));
}
