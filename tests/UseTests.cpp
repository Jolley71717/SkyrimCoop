#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/UseRules.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

// What a mechanism's use did (docs/DESIGN.md, "What a use did"): the items lent with a use and its effects on the wire
// (common/Protocol.h), their form IDs (common/FormTranslate.h), the server's passing on (common/Server.cpp) and each
// game's rules (src/game/UseRules.h). What reads and changes the game itself (src/game/WorldObjects.cpp: the scripts'
// items, the animation hook, the enable scan, lending to a stand-in) is tested in a game only: tools/rig/steps/claws.sh.

using Protocol::Activation;
using Protocol::LentItem;
using Protocol::UseEffect;
using Protocol::UseEffects;
using UseRules::ItemKind;

namespace
{
	using MT = Protocol::MessageType;
	using Clock = std::chrono::steady_clock;
	using std::chrono::milliseconds;
	using std::chrono::seconds;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	constexpr std::uint32_t CLAW_DOOR = 0x0001A2B3;
	constexpr std::uint32_t RING = 0x0001A2B4;
	constexpr std::uint32_t WALL = 0x0001A2B5;
	constexpr std::uint32_t GOLDEN_CLAW = 0x000999B3;

	UseEffect Anim(std::uint32_t a_ref, std::string a_event, std::uint16_t a_at = 0)
	{
		UseEffect effect;
		effect.ref = a_ref;
		effect.what = UseEffect::kAnim;
		effect.atMs = a_at;
		effect.event = std::move(a_event);
		return effect;
	}

	UseEffect Toggle(std::uint32_t a_ref, std::uint8_t a_what, std::uint16_t a_at = 0)
	{
		UseEffect effect;
		effect.ref = a_ref;
		effect.what = a_what;
		effect.atMs = a_at;
		return effect;
	}

	UseEffects Batch(std::uint32_t a_player, std::uint32_t a_seq, std::vector<UseEffect> a_effects, std::uint32_t a_mechanism = CLAW_DOOR)
	{
		UseEffects msg;
		msg.playerId = a_player;
		msg.seq = a_seq;
		msg.mechanism = a_mechanism;
		msg.effects = std::move(a_effects);
		return msg;
	}

	std::size_t CountOf(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_packet) { return Protocol::PeekType(a_packet) == a_type; }));
	}
}

// ---- the messages -------------------------------------------------------------------------------------------------

TEST("use: a use goes with its number and the items it lends, and comes back naming who used it")
{
	CHECK(Protocol::VERSION >= 16);
	const Activation said{ .refId = CLAW_DOOR, .seq = 7, .lent = { { GOLDEN_CLAW, 1 }, { 0x0000000F, 250 } } };
	const auto       packet = Protocol::Encode(said);
	CHECK(Protocol::PeekType(packet) == MT::kActivate);
	const auto back = Protocol::DecodeActivation(packet);
	REQUIRE(back);
	CHECK(*back == said);

	auto passed = said;
	passed.playerId = 3;
	const auto heard = Protocol::DecodeActivation(Protocol::Encode(passed, MT::kRemoteActivate));
	REQUIRE(heard);
	CHECK(*heard == passed);

	// A use that lends nothing is its reference, its number and a count of 0.
	const auto plain = Protocol::DecodeActivation(Protocol::Encode(Activation{ .refId = CLAW_DOOR, .seq = 1 }));
	REQUIRE(plain);
	CHECK(plain->lent.empty());
	CHECK(plain->seq == 1);
}

TEST("use: a use lending an item of no base or no count, past MAX_LENT, or cut short is not read; Encode writes at most MAX_LENT")
{
	CHECK(!Protocol::DecodeActivation(Protocol::Encode(Activation{ .refId = CLAW_DOOR, .lent = { { 0, 1 } } })));
	CHECK(!Protocol::DecodeActivation(Protocol::Encode(Activation{ .refId = CLAW_DOOR, .lent = { { GOLDEN_CLAW, 0 } } })));

	Activation many{ .refId = CLAW_DOOR, .seq = 2 };
	for (std::uint32_t i = 1; i <= Protocol::MAX_LENT + 4; ++i) {
		many.lent.push_back({ 0x00010000 + i, 1 });
	}
	const auto written = Protocol::DecodeActivation(Protocol::Encode(many));
	REQUIRE(written);
	CHECK(written->lent.size() == Protocol::MAX_LENT);

	auto packet = Protocol::Encode(Activation{ .refId = CLAW_DOOR, .seq = 2, .lent = { { GOLDEN_CLAW, 1 } } });
	auto over = packet;
	over[1 + 4 + 4] = static_cast<std::uint8_t>(Protocol::MAX_LENT + 1);  // the count
	CHECK(!Protocol::DecodeActivation(over));
	auto cut = packet;
	cut.pop_back();
	CHECK(!Protocol::DecodeActivation(cut));
	auto longer = packet;
	longer.push_back(0);
	CHECK(!Protocol::DecodeActivation(longer));
}

TEST("use: what a use did goes in batches, its player named on the way back; a bad batch is not read")
{
	const auto said = Batch(0, 4, { Anim(CLAW_DOOR, "Open", 120), Toggle(WALL, UseEffect::kDisable, 900), Toggle(RING, UseEffect::kEnable, 901) });
	const auto packet = Protocol::Encode(said);
	CHECK(Protocol::PeekType(packet) == MT::kUseEffects);
	const auto back = Protocol::DecodeUseEffects(packet);
	REQUIRE(back);
	CHECK(*back == said);

	auto passed = said;
	passed.playerId = 2;
	const auto heard = Protocol::DecodeUseEffects(Protocol::Encode(passed, MT::kRemoteUseEffects));
	REQUIRE(heard);
	CHECK(*heard == passed);

	CHECK(!Protocol::DecodeUseEffects(Protocol::Encode(Batch(0, 4, {}))));                                       // no effect
	CHECK(!Protocol::DecodeUseEffects(Protocol::Encode(Batch(0, 4, { Anim(0, "Open") }))));                      // no reference
	CHECK(!Protocol::DecodeUseEffects(Protocol::Encode(Batch(0, 4, { Anim(RING, "") }))));                       // an animation of no event
	CHECK(!Protocol::DecodeUseEffects(Protocol::Encode(Batch(0, 4, { Toggle(RING, static_cast<std::uint8_t>(Protocol::USE_EFFECT_MAX + 1)) }))));  // a kind not known
	CHECK(!Protocol::DecodeUseEffects(Protocol::Encode(Activation{ .refId = CLAW_DOOR })));

	// An enable with an event: Encode never writes one (the animation's kind byte made an enable's here).
	auto withEvent = Protocol::Encode(Batch(0, 4, { Anim(RING, "Open") }));
	CHECK(Protocol::DecodeUseEffects(withEvent));
	withEvent[1 + 4 + 4 + 1 + 4] = UseEffect::kEnable;  // type, seq, mechanism, count, ref: then the kind
	CHECK(!Protocol::DecodeUseEffects(withEvent));
	auto cut = Protocol::Encode(Batch(0, 4, { Toggle(RING, UseEffect::kEnable) }));
	CHECK(Protocol::DecodeUseEffects(cut));
	cut.pop_back();
	CHECK(!Protocol::DecodeUseEffects(cut));

	// At most MAX_USE_EFFECTS a batch, and an event at most MAX_ANIM_EVENT bytes.
	UseEffects big = Batch(0, 5, {});
	for (std::uint32_t i = 1; i <= Protocol::MAX_USE_EFFECTS + 3; ++i) {
		big.effects.push_back(Toggle(0x00020000 + i, UseEffect::kEnable));
	}
	const auto bigBack = Protocol::DecodeUseEffects(Protocol::Encode(big));
	REQUIRE(bigBack);
	CHECK(bigBack->effects.size() == Protocol::MAX_USE_EFFECTS);
	const auto longName = Protocol::DecodeUseEffects(Protocol::Encode(Batch(0, 5, { Anim(RING, std::string(Protocol::MAX_ANIM_EVENT + 10, 'x')) })));
	REQUIRE(longName);
	CHECK(longName->effects[0].event.size() <= Protocol::MAX_ANIM_EVENT);
}

// ---- form IDs on the wire -----------------------------------------------------------------------------------------

TEST("use: the items lent and the effects cross by plugin; what a game lacks is left out, and a batch of nothing it has is not read")
{
	using FormIds::Mapper;
	using FormTranslate::Result;
	using FormTranslate::Way;
	for (const auto type : { MT::kActivate, MT::kRemoteActivate, MT::kUseEffects, MT::kRemoteUseEffects }) {
		CHECK(FormTranslate::CarriesForms(type));
	}

	// The host has Claws.esp only; the guest has Extra.esp before it (Claws.esp is 01 at the host and in the session, 02
	// at the guest; Extra.esp is the session's 02 and none at the host).
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Claws.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Extra.esp", "Claws.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const Mapper hostEdge(host, session);
	const Mapper guestEdge(guest, session);

	// The guest's use: its door and claw in Claws.esp (02 there), a gem in Extra.esp (01 there).
	const Activation use{ .refId = 0x02000ABC, .seq = 9, .lent = { { 0x02000D01, 1 }, { 0x01000E01, 2 }, { GOLDEN_CLAW, 1 } } };
	const auto       out = FormTranslate::Translate(guestEdge, Way::kToSession, Protocol::Encode(use));
	REQUIRE(out.kind == Result::Kind::kChanged);
	auto onWire = Protocol::DecodeActivation(out.packet);
	REQUIRE(onWire);
	CHECK(onWire->refId == 0x01000ABC);
	CHECK(onWire->seq == 9);
	REQUIRE(onWire->lent.size() == 3);
	CHECK(onWire->lent[0] == (LentItem{ 0x01000D01, 1 }));
	CHECK(onWire->lent[1] == (LentItem{ 0x02000E01, 2 }));

	onWire->playerId = 2;
	const auto in = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(*onWire, MT::kRemoteActivate));
	REQUIRE(in.kind == Result::Kind::kChanged);
	CHECK(in.entries == 1);  // the gem of Extra.esp
	const auto atHost = Protocol::DecodeActivation(in.packet);
	REQUIRE(atHost);
	CHECK(atHost->refId == 0x01000ABC);
	CHECK(atHost->playerId == 2);
	CHECK(atHost->lent == (std::vector<LentItem>{ { 0x01000D01, 1 }, { GOLDEN_CLAW, 1 } }));

	// What the guest's use did: a ring of Claws.esp, a wall of Extra.esp, the door's own fall.
	const auto effects = Batch(0, 9, { Anim(0x02000AB1, "Ring1Turn", 100), Toggle(0x01000F00, UseEffect::kDisable, 900), Anim(0x02000ABC, "Open", 2000) }, 0x02000ABC);
	const auto sent = FormTranslate::Translate(guestEdge, Way::kToSession, Protocol::Encode(effects));
	REQUIRE(sent.kind == Result::Kind::kChanged);
	auto said = Protocol::DecodeUseEffects(sent.packet);
	REQUIRE(said);
	CHECK(said->mechanism == 0x01000ABC);
	REQUIRE(said->effects.size() == 3);
	CHECK(said->effects[1].ref == 0x02000F00);
	said->playerId = 2;
	const auto read = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(*said, MT::kRemoteUseEffects));
	REQUIRE(read.kind == Result::Kind::kChanged);
	CHECK(read.entries == 1);
	const auto made = Protocol::DecodeUseEffects(read.packet);
	REQUIRE(made);
	CHECK(made->playerId == 2);
	REQUIRE(made->effects.size() == 2);
	CHECK(made->effects[0] == Anim(0x01000AB1, "Ring1Turn", 100));
	CHECK(made->effects[1] == Anim(0x01000ABC, "Open", 2000));

	// Only Extra.esp's wall: nothing the host has, not read at all.
	const auto onlyWall = Batch(2, 9, { Toggle(0x02000F00, UseEffect::kDisable) }, 0x01000ABC);
	CHECK(FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(onlyWall, MT::kRemoteUseEffects)).kind == Result::Kind::kDrop);
	// A mechanism the host lacks still says what it did to what the host has: the mechanism reads as 0.
	const auto lackedMech = Batch(2, 9, { Anim(0x01000AB1, "Ring1Turn") }, 0x02000ABC);
	const auto lacked = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(lackedMech, MT::kRemoteUseEffects));
	REQUIRE(lacked.kind == Result::Kind::kChanged);
	const auto lackedMsg = Protocol::DecodeUseEffects(lacked.packet);
	REQUIRE(lackedMsg);
	CHECK(lackedMsg->mechanism == 0);
	CHECK(lackedMsg->effects.size() == 1);
}

// ---- the server ---------------------------------------------------------------------------------------------------

TEST("server: a use's number and lent items go to the others with it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	const Activation use{ .refId = CLAW_DOOR, .seq = 12, .lent = { { GOLDEN_CLAW, 1 } } };
	f.fake->Deliver(1, Protocol::Encode(use));
	const auto packet = f.fake->WaitFor(2, Type(MT::kRemoteActivate));
	REQUIRE(packet);
	const auto heard = Protocol::DecodeActivation(*packet);
	REQUIRE(heard);
	CHECK(heard->playerId == 1);
	CHECK(heard->seq == 12);
	CHECK(heard->lent == use.lent);
}

TEST("server: what a use did goes to the others, named, never back to its user nor kept for a joiner")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	const auto said = Batch(0, 3, { Anim(RING, "Ring1Turn", 50), Toggle(WALL, UseEffect::kDisable, 800) });
	f.fake->Deliver(1, Protocol::Encode(said));
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto packet = f.fake->WaitFor(peer, Type(MT::kRemoteUseEffects));
		REQUIRE(packet);
		const auto heard = Protocol::DecodeUseEffects(*packet);
		REQUIRE(heard);
		CHECK(heard->playerId == 1);
		CHECK(heard->seq == 3);
		CHECK(heard->mechanism == CLAW_DOOR);
		CHECK(heard->effects == said.effects);
	}
	// Then a use, which goes to the others only: whatever came to the user before it, no effects of its own.
	f.fake->Deliver(1, Protocol::Encode(Activation{ .refId = CLAW_DOOR, .seq = 4 }));
	f.fake->Deliver(2, Protocol::Encode(Activation{ .refId = CLAW_DOOR, .seq = 1 }));
	CHECK(CountOf(f.fake->TakeThrough(1, Type(MT::kRemoteActivate)), MT::kRemoteUseEffects) == 0);
	REQUIRE(f.Join(4, Fixture::MakeHello("Di")));
	f.fake->Deliver(1, Protocol::Encode(Activation{ .refId = CLAW_DOOR, .seq = 5 }));
	CHECK(CountOf(f.fake->TakeThrough(4, Type(MT::kRemoteActivate)), MT::kRemoteUseEffects) == 0);
}

TEST("server: past its budget a player's batches are dropped, said in the log once")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	for (std::uint32_t i = 1; i <= 60; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Batch(0, i, { Anim(RING, "Ring1Turn") })));
	}
	f.fake->Deliver(1, Protocol::Encode(Activation{ .refId = CLAW_DOOR, .seq = 99 }));
	const auto got = CountOf(f.fake->TakeThrough(2, Type(MT::kRemoteActivate)), MT::kRemoteUseEffects);
	CHECK(got >= 16);
	CHECK(got < 60);
	std::lock_guard lock{ f.logMutex };
	CHECK(std::ranges::count_if(f.logLines, [](const std::string& a_line) { return a_line.find("says what its uses did faster") != std::string::npos; }) == 1);
}

// ---- the items lent -----------------------------------------------------------------------------------------------

TEST("use: a use lends each named item our player holds once, all of it, never what a stand-in could equip, drink or read on its own")
{
	using UseRules::Named;
	CHECK(UseRules::Lendable(ItemKind::kMisc));
	CHECK(UseRules::Lendable(ItemKind::kKey));
	CHECK(UseRules::Lendable(ItemKind::kSoulGem));
	CHECK(UseRules::Lendable(ItemKind::kBook));
	CHECK(UseRules::Lendable(ItemKind::kIngredient));
	CHECK(!UseRules::Lendable(ItemKind::kPotion));
	CHECK(!UseRules::Lendable(ItemKind::kEquippable));
	CHECK(!UseRules::Lendable(ItemKind::kNotItem));

	const auto pick = UseRules::PickLent({
		Named{ GOLDEN_CLAW, ItemKind::kMisc, 1 },
		Named{ GOLDEN_CLAW, ItemKind::kMisc, 1 },     // named twice (a property and a list): once
		Named{ 0x00000010, ItemKind::kKey, 0 },       // not held
		Named{ 0x00000011, ItemKind::kEquippable, 1 },  // a sword: kept
		Named{ 0x00000012, ItemKind::kPotion, 3 },    // kept
		Named{ 0x00000013, ItemKind::kNotItem, 1 },   // a sound, a message: passed over
		Named{ 0x0000000F, ItemKind::kMisc, 100000 },  // gold, more than a count holds
		Named{ 0, ItemKind::kMisc, 1 },
	});
	CHECK(pick.lent == (std::vector<LentItem>{ { GOLDEN_CLAW, 1 }, { 0x0000000F, 0xFFFF } }));
	CHECK(pick.kept == 2);
	CHECK(pick.cut == 0);

	std::vector<Named> many;
	for (std::uint32_t i = 1; i <= Protocol::MAX_LENT + 5; ++i) {
		many.push_back({ 0x00030000 + i, ItemKind::kKey, 1 });
	}
	const auto capped = UseRules::PickLent(many);
	CHECK(capped.lent.size() == Protocol::MAX_LENT);
	CHECK(capped.cut == 5);
}

TEST("use: lent items are taken back after LEND_FOR, all at once before a save, the oldest early past MAX_LOANS")
{
	UseRules::Loans loans;
	const auto      t0 = Clock::now();
	loans.Lent(0xFF000801, { { GOLDEN_CLAW, 1 }, { 0, 1 }, { 0x10, 0 } }, t0);  // no base, no count: nothing
	CHECK(loans.Out() == 1);
	loans.Lent(0xFF000802, { { 0x20, 2 } }, t0 + seconds(5));
	CHECK(loans.Due(t0 + seconds(9)).empty());
	const auto first = loans.Due(t0 + UseRules::LEND_FOR);
	REQUIRE(first.size() == 1);
	CHECK(first[0].standIn == 0xFF000801);
	CHECK(first[0].base == GOLDEN_CLAW);
	CHECK(loans.Out() == 1);

	// What to take back: what was lent, at most what the stand-in holds now.
	CHECK(UseRules::Loans::Back(first[0], 1) == 1);
	CHECK(UseRules::Loans::Back(first[0], 5) == 1);
	CHECK(UseRules::Loans::Back(first[0], 0) == 0);
	CHECK(UseRules::Loans::Back(first[0], -1) == 0);

	const auto saved = loans.All();
	CHECK(saved.size() == 1);
	CHECK(loans.Out() == 0);

	for (std::uint32_t i = 0; i < UseRules::MAX_LOANS + 3; ++i) {
		loans.Lent(0xFF000900 + i, { { 0x30, 1 } }, t0);
	}
	CHECK(loans.Out() == UseRules::MAX_LOANS + 3);
	const auto early = loans.Due(t0);
	REQUIRE(early.size() == 3);
	CHECK(early[0].standIn == 0xFF000900);
	CHECK(loans.Out() == UseRules::MAX_LOANS);
	loans.Clear();
	CHECK(loans.Out() == 0);
}

// ---- what our player's use did ------------------------------------------------------------------------------------

TEST("use: what happens near a watched use is its effect, sent every SEND_EVERY and the rest when the watch ends")
{
	UseRules::Capture capture;
	const auto        t0 = Clock::now();
	CHECK(!capture.Active(t0));
	capture.Begin(1, CLAW_DOOR, t0);
	CHECK(capture.Active(t0));
	REQUIRE(capture.Watched(t0).size() == 1);
	CHECK(capture.Watched(t0)[0].mechanism == CLAW_DOOR);

	CHECK(capture.Add(1, RING, UseEffect::kAnim, "Ring1Turn", t0 + milliseconds(120)));
	CHECK(!capture.Add(2, RING, UseEffect::kAnim, "Ring1Turn", t0));                       // no such use watched
	CHECK(!capture.Add(1, 0, UseEffect::kAnim, "Ring1Turn", t0));                          // no reference
	CHECK(!capture.Add(1, RING, UseEffect::kAnim, "", t0));                                // an animation of no event
	CHECK(!capture.Add(1, WALL, UseEffect::kDisable, "Open", t0));                         // a disable with one
	CHECK(!capture.Add(1, RING, UseEffect::kAnim, "Late", t0 + UseRules::WATCH_FOR));     // after the watch
	CHECK(capture.captured == 1);

	CHECK(capture.Due(t0 + milliseconds(200)).empty());  // SEND_EVERY has not passed
	const auto first = capture.Due(t0 + UseRules::SEND_EVERY);
	REQUIRE(first.size() == 1);
	CHECK(first[0].seq == 1);
	CHECK(first[0].mechanism == CLAW_DOOR);
	CHECK(first[0].playerId == 0);
	REQUIRE(first[0].effects.size() == 1);
	CHECK(first[0].effects[0] == Anim(RING, "Ring1Turn", 120));

	CHECK(capture.Add(1, WALL, UseEffect::kDisable, "", t0 + milliseconds(900)));
	CHECK(capture.Due(t0 + milliseconds(800)).empty());  // 300 ms after the last batch
	CHECK(capture.Add(1, CLAW_DOOR, UseEffect::kAnim, std::string(100, 'o'), t0 + seconds(9)));
	const auto last = capture.Due(t0 + UseRules::WATCH_FOR);  // the watch ended: all it holds, then it is forgotten
	REQUIRE(last.size() == 1);
	REQUIRE(last[0].effects.size() == 2);
	CHECK(last[0].effects[0] == Toggle(WALL, UseEffect::kDisable, 900));
	CHECK(last[0].effects[1].event.size() == Protocol::MAX_ANIM_EVENT);
	CHECK(last[0].effects[1].atMs == 9000);
	CHECK(!capture.Active(t0 + UseRules::WATCH_FOR));
	CHECK(capture.Due(t0 + seconds(20)).empty());
	CHECK(capture.batches == 2);
}

TEST("use: a use says at most MAX_PER_USE effects, in batches of MAX_USE_EFFECTS; a fifth use ends the oldest's watch")
{
	UseRules::Capture capture;
	const auto        t0 = Clock::now();
	capture.Begin(1, CLAW_DOOR, t0);
	for (std::size_t i = 0; i < UseRules::MAX_PER_USE + 6; ++i) {
		capture.Add(1, 0x00040000 + static_cast<std::uint32_t>(i), UseEffect::kEnable, "", t0 + milliseconds(10));
	}
	CHECK(capture.captured == UseRules::MAX_PER_USE);
	CHECK(capture.cut == 6);
	const auto batches = capture.Due(t0 + UseRules::SEND_EVERY);
	REQUIRE(batches.size() == UseRules::MAX_PER_USE / Protocol::MAX_USE_EFFECTS);
	for (const auto& batch : batches) {
		CHECK(batch.effects.size() == Protocol::MAX_USE_EFFECTS);
		CHECK(Protocol::DecodeUseEffects(Protocol::Encode(batch)).has_value());
	}
	CHECK(!capture.Add(1, RING, UseEffect::kEnable, "", t0 + seconds(1)));  // said its most already

	for (std::uint32_t seq = 2; seq <= UseRules::MAX_WATCHED + 1; ++seq) {
		capture.Begin(seq, CLAW_DOOR + seq, t0 + seconds(2));
	}
	const auto watched = capture.Watched(t0 + seconds(2));
	REQUIRE(watched.size() == UseRules::MAX_WATCHED);
	CHECK(watched.front().seq == UseRules::MAX_WATCHED + 1);  // newest first
	CHECK(std::ranges::none_of(watched, [](const auto& a_w) { return a_w.seq == 1; }));
	capture.Clear();
	CHECK(!capture.Active(t0 + seconds(2)));
}

TEST("use: an enable or disable is a change between two scans in a row; a reference first read, or read again after a gap, says nothing")
{
	UseRules::EnableWatch watch;
	watch.BeginScan();
	CHECK(!watch.Seen(WALL, false));  // first read: the watch began
	CHECK(!watch.Seen(RING, true));
	watch.EndScan();
	CHECK(watch.Size() == 2);

	watch.BeginScan();
	CHECK(watch.Seen(WALL, true));    // disabled since
	CHECK(!watch.Seen(WALL, false));  // read twice in one scan: the first counts
	CHECK(!watch.Seen(0x00050001, false));
	watch.EndScan();  // RING not read: forgotten
	CHECK(watch.Size() == 2);

	watch.BeginScan();
	CHECK(!watch.Seen(RING, false));  // came back within reach: a first read, though it reads otherwise
	CHECK(!watch.Seen(WALL, true));   // unchanged
	watch.EndScan();
	watch.Clear();
	CHECK(watch.Size() == 0);
}

// ---- what a friend's use did, made here ---------------------------------------------------------------------------

TEST("use: a friend's effect our own run of the use did too is matched, and not made again")
{
	UseRules::Mirror mirror;
	const auto       t0 = Clock::now();
	mirror.UseHeard(2, 7, t0);
	CHECK(mirror.Listening(t0));
	mirror.Heard(Batch(2, 7, { Anim(RING, "Ring1Turn", 100), Anim(CLAW_DOOR, "Open", 2000) }), t0 + milliseconds(600));
	CHECK(mirror.heard == 2);
	CHECK(mirror.Waiting() == 2);

	mirror.Own(RING, "Ring1Turn", t0 + milliseconds(150));    // our stand-in's run turned the ring
	mirror.Own(CLAW_DOOR, "Open", t0 + milliseconds(1900));   // and dropped the door
	// The ring happened at 100 ms but was heard at 600 ms: it waits MATCH_WAIT from when it was heard, then is matched.
	CHECK(mirror.Due(t0 + milliseconds(2099)).empty());
	CHECK(mirror.matched == 0);
	CHECK(mirror.Due(t0 + milliseconds(2100)).empty());
	CHECK(mirror.matched == 1);
	CHECK(mirror.Due(t0 + milliseconds(3500)).empty());
	CHECK(mirror.matched == 2);
	CHECK(mirror.toMake == 0);
	CHECK(mirror.Waiting() == 0);
	CHECK(mirror.doubled == 0);
}

TEST("use: a friend's effect our run did not make is made here, in the order it happened; every enable and disable goes to the game")
{
	UseRules::Mirror mirror;
	const auto       t0 = Clock::now();
	mirror.UseHeard(2, 7, t0);
	// A script that does nothing for a stand-in: no event of ours. The batch lists the wall last, though it was first.
	mirror.Heard(Batch(2, 7, { Anim(CLAW_DOOR, "Open", 2000), Toggle(WALL, UseEffect::kDisable, 50) }), t0 + milliseconds(2200));
	mirror.Own(RING, "Ring1Turn", t0 + milliseconds(100));  // another reference's event: no match
	mirror.Own(CLAW_DOOR, "Close", t0 + milliseconds(100));  // another event: no match
	const auto made = mirror.Due(t0 + seconds(4));
	REQUIRE(made.size() == 2);
	CHECK(made[0].effect == Toggle(WALL, UseEffect::kDisable, 50));
	CHECK(made[1].effect == Anim(CLAW_DOOR, "Open", 2000));
	CHECK(made[1].player == 2);
	CHECK(made[1].seq == 7);
	CHECK(mirror.toMake == 2);

	// An event of ours long before the use (OWN_SLACK) is not this use's.
	mirror.Own(RING, "Ring2Turn", t0 + seconds(5));
	mirror.UseHeard(2, 8, t0 + seconds(8));
	mirror.Heard(Batch(2, 8, { Anim(RING, "Ring2Turn", 0) }), t0 + seconds(8));
	const auto again = mirror.Due(t0 + seconds(10));
	REQUIRE(again.size() == 1);
	CHECK(again[0].effect.event == "Ring2Turn");
}

TEST("use: effects of a use this game never heard are timed from when they arrive; an animation made here then played by our own run is a double")
{
	UseRules::Mirror mirror;
	const auto       t0 = Clock::now();
	mirror.Heard(Batch(3, 1, { Anim(RING, "Ring1Turn", 4000) }), t0);  // its use was dropped on the way
	CHECK(mirror.Due(t0 + milliseconds(1499)).empty());
	const auto made = mirror.Due(t0 + UseRules::MATCH_WAIT);
	REQUIRE(made.size() == 1);
	mirror.Made(RING, "Ring1Turn", t0 + UseRules::MATCH_WAIT);
	CHECK(mirror.Listening(t0 + seconds(3)));
	mirror.Own(RING, "Ring1Turn", t0 + seconds(3));  // our own run, late: the ring turned twice
	CHECK(mirror.doubled == 1);
	mirror.Own(RING, "Ring1Turn", t0 + seconds(4));  // one double per make
	CHECK(mirror.doubled == 1);

	// An event awaited by an effect is a match, not a double, even right after one of ours was made.
	mirror.UseHeard(3, 2, t0 + seconds(5));
	mirror.Made(CLAW_DOOR, "Open", t0 + seconds(5));
	mirror.Heard(Batch(3, 2, { Anim(CLAW_DOOR, "Open", 100) }), t0 + seconds(5));
	mirror.Own(CLAW_DOOR, "Open", t0 + seconds(5) + milliseconds(200));
	CHECK(mirror.doubled == 1);
	CHECK(mirror.Due(t0 + seconds(7)).empty());
	CHECK(mirror.matched == 1);

	mirror.Clear();
	CHECK(mirror.Waiting() == 0);
	CHECK(!mirror.Listening(t0 + seconds(7)));
}

TEST("use: a flood of effects keeps at most MAX_PENDING_EFFECTS waiting, and the last animations read are kept for anims")
{
	UseRules::Mirror mirror;
	const auto       t0 = Clock::now();
	for (std::uint32_t seq = 1; seq <= 10; ++seq) {
		UseEffects batch = Batch(4, seq, {});
		for (std::uint32_t i = 0; i < Protocol::MAX_USE_EFFECTS; ++i) {
			batch.effects.push_back(Toggle(0x00060000 + i, UseEffect::kEnable));
		}
		mirror.Heard(batch, t0);
	}
	CHECK(mirror.Waiting() == UseRules::MAX_PENDING_EFFECTS);
	CHECK(mirror.cut == 10 * Protocol::MAX_USE_EFFECTS - UseRules::MAX_PENDING_EFFECTS);

	UseRules::AnimRing ring;
	for (std::size_t i = 0; i < UseRules::ANIM_RING + 2; ++i) {
		ring.Add(i % 2 ? RING : CLAW_DOOR, i == UseRules::ANIM_RING + 1 ? "Open" : "Idle", i == UseRules::ANIM_RING + 1, t0);
	}
	CHECK(ring.total == UseRules::ANIM_RING + 2);
	const auto door = ring.Of(RING);
	CHECK(door.size() == UseRules::ANIM_RING / 2);
	CHECK(door.back().event == "Open");
	CHECK(door.back().made);
	ring.Clear();
	CHECK(ring.Of(RING).empty());
}

// ---- review fixes: a use read as it is made, a late own run, a lent item handed on, carryable items -----------------

TEST("use: a reference read as the use was made is the baseline, so a script's change before the first scan is an effect")
{
	UseRules::EnableWatch watch;
	// The activate event's sink read the wall enabled; the script disabled it before the tick's first scan.
	watch.Baseline(WALL, false);
	watch.Baseline(0, false);  // no reference: not kept
	CHECK(watch.Size() == 1);
	watch.BeginScan();
	CHECK(watch.Seen(WALL, true));
	watch.EndScan();

	// A reference an earlier watch reads already keeps its read: the sink's read of it does not replace it.
	watch.Baseline(WALL, false);
	watch.BeginScan();
	CHECK(!watch.Seen(WALL, true));  // unchanged since the last scan's read
	watch.EndScan();
}

TEST("use: a fixed reference's enable and disable are effects; a carryable item's enable only (its disable is a pickup)")
{
	using UseRules::Placed;
	CHECK(UseRules::EnableSays(Placed::kFixed, true));
	CHECK(UseRules::EnableSays(Placed::kFixed, false));
	CHECK(UseRules::EnableSays(Placed::kCarryable, false));  // a reward gem put out, a brazier's carryable torch shown
	CHECK(!UseRules::EnableSays(Placed::kCarryable, true));
	CHECK(!UseRules::EnableSays(Placed::kNot, false));
	CHECK(!UseRules::EnableSays(Placed::kNot, true));
}

TEST("use: a friend's use made here by our stand-in waits MATCH_WAIT_MADE for our own run, which is then matched")
{
	UseRules::Mirror mirror;
	const auto       t0 = Clock::now();
	mirror.UseHeard(2, 9, t0);
	mirror.MadeHere(2, 9);
	mirror.MadeHere(3, 1);  // no such use: nothing
	mirror.Heard(Batch(2, 9, { Anim(RING, "Trans01", 100) }), t0 + milliseconds(600));
	CHECK(mirror.Due(t0 + milliseconds(600) + UseRules::MATCH_WAIT).empty());  // not yet: our run is under way
	CHECK(mirror.toMake == 0);
	mirror.Own(RING, "Trans01", t0 + seconds(3));  // our run, 3 s late (a slow game): still a match
	CHECK(mirror.Due(t0 + milliseconds(600) + UseRules::MATCH_WAIT_MADE).empty());
	CHECK(mirror.matched == 1);
	CHECK(mirror.toMake == 0);
	CHECK(mirror.doubled == 0);

	// A use not made here (refused, no stand-in) keeps MATCH_WAIT.
	mirror.UseHeard(2, 10, t0 + seconds(10));
	mirror.Heard(Batch(2, 10, { Anim(RING, "Trans02", 0) }), t0 + seconds(10));
	CHECK(mirror.Due(t0 + seconds(10) + UseRules::MATCH_WAIT).size() == 1);
}

TEST("use: our own run's repeat of an animation made here is held back once per make, never one an effect waits for")
{
	UseRules::Mirror mirror;
	const auto       t0 = Clock::now();
	mirror.Heard(Batch(3, 1, { Anim(RING, "Trans01", 0) }), t0);
	REQUIRE(mirror.Due(t0 + UseRules::MATCH_WAIT).size() == 1);
	mirror.Made(RING, "Trans01", t0 + UseRules::MATCH_WAIT);
	// Our own run, late: its event is held back (the graph never gets it), and not taken as played.
	CHECK(mirror.Repeat(RING, "Trans01", t0 + seconds(4)));
	CHECK(mirror.doubled == 1);
	CHECK(!mirror.Repeat(RING, "Trans01", t0 + seconds(5)));  // one per make: a second is the game's own again
	CHECK(!mirror.Repeat(RING, "Trans02", t0 + seconds(5)));  // another event
	CHECK(!mirror.Repeat(WALL, "Trans01", t0 + seconds(5)));  // another reference
	mirror.Made(RING, "Trans01", t0 + seconds(6));
	CHECK(!mirror.Repeat(RING, "Trans01", t0 + seconds(6) + UseRules::DOUBLE_WINDOW + milliseconds(1)));  // too late to be one

	// A make the graph did not take holds nothing back.
	mirror.Made(CLAW_DOOR, "Open", t0 + seconds(20));
	mirror.Unmade(CLAW_DOOR, "Open");
	CHECK(!mirror.Repeat(CLAW_DOOR, "Open", t0 + seconds(21)));

	// An event an effect waits for is the game's own run of that use, not a repeat.
	mirror.UseHeard(3, 2, t0 + seconds(30));
	mirror.Made(CLAW_DOOR, "Open", t0 + seconds(30));
	mirror.Heard(Batch(3, 2, { Anim(CLAW_DOOR, "Open", 0) }), t0 + seconds(30));
	CHECK(!mirror.Repeat(CLAW_DOOR, "Open", t0 + seconds(31)));
	mirror.Played(CLAW_DOOR, "Open", t0 + seconds(31));
	CHECK(mirror.Due(t0 + seconds(33)).empty());
	CHECK(mirror.matched == 1);

	// Our player's own use: what follows is our player's doing, never held back.
	mirror.Made(RING, "Trans03", t0 + seconds(40));
	mirror.OurUse();
	CHECK(!mirror.Repeat(RING, "Trans03", t0 + seconds(41)));
	CHECK(mirror.doubled == 1);
}

TEST("use: a lent item the stand-in's run hands on leaves the loan, oldest first; the rest of a move is the stand-in's own")
{
	UseRules::Loans loans;
	const auto      t0 = Clock::now();
	constexpr std::uint32_t STAND_IN = 0xFF000801;
	loans.Lent(STAND_IN, { { GOLDEN_CLAW, 1 } }, t0);
	loans.Lent(STAND_IN, { { GOLDEN_CLAW, 2 } }, t0 + seconds(1));
	CHECK(loans.Lends(STAND_IN, GOLDEN_CLAW));
	CHECK(!loans.Lends(STAND_IN, 0x20));
	CHECK(!loans.Lends(0xFF000802, GOLDEN_CLAW));

	CHECK(loans.Moved(STAND_IN, GOLDEN_CLAW, 2) == 2);  // the first loan and one of the second
	CHECK(loans.Out() == 1);
	CHECK(loans.Moved(STAND_IN, GOLDEN_CLAW, 5) == 1);  // the last lent; four of its own
	CHECK(loans.Out() == 0);
	CHECK(!loans.Lends(STAND_IN, GOLDEN_CLAW));
	CHECK(loans.Moved(STAND_IN, GOLDEN_CLAW, 1) == 0);
	CHECK(loans.Due(t0 + seconds(20)).empty());  // nothing left to take back from it

	// Moved out of the early ones too (past MAX_LOANS).
	for (std::uint32_t i = 0; i < UseRules::MAX_LOANS + 1; ++i) {
		loans.Lent(STAND_IN, { { 0x30, 1 } }, t0);
	}
	CHECK(loans.Moved(STAND_IN, 0x30, 1) == 1);
	CHECK(loans.Out() == UseRules::MAX_LOANS);

	// Only a holder whose moves the session shares has the stand-in's copy taken back out (loot brings the user's).
	CHECK(UseRules::TakeOutOfHolder(UseRules::LentWent::kSharedHolder));
	CHECK(!UseRules::TakeOutOfHolder(UseRules::LentWent::kOwnHolder));
	CHECK(!UseRules::TakeOutOfHolder(UseRules::LentWent::kDropped));
	CHECK(!UseRules::TakeOutOfHolder(UseRules::LentWent::kUsedUp));
	CHECK(std::string{ UseRules::LentWentName(UseRules::LentWent::kSharedHolder) } == "put in a holder the session shares");
}
