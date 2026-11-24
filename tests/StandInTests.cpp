#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/StandInRules.h"

#include <cmath>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

// A friend's stand-in doing what they do (docs/DESIGN.md, "Players"): their player's graph events on the wire and
// through the server, what kind of action each is, how many go, what the stand-in holds, when its weapon is drawn by
// hand, how it aims, and which of its spells' effects take hold.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using StandInRules::Act;
	using Steady = std::chrono::steady_clock;

	Protocol::PlayerLooks Armed()
	{
		Protocol::PlayerLooks looks;
		looks.race = 0x00013746;
		looks.weight = 50.0f;
		looks.worn = { 0x00012E49, 0x00012E4B };
		looks.right = 0x00012EB7;  // an iron sword
		looks.left = 0x00012EB6;   // a shield
		looks.ammo = 0x0001397D;
		looks.voice = 0x00013E07;
		return looks;
	}

	// Every kRemoteActions the server sent a_peer so far, decoded, with whether it went reliably.
	std::vector<std::pair<Protocol::RemoteActions, bool>> SentActions(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<std::pair<Protocol::RemoteActions, bool>> out;
		for (auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kRemoteActions) {
				if (const auto msg = Protocol::DecodeRemoteActions(sent.data)) {
					out.emplace_back(*msg, sent.reliable);
				}
			}
		}
		return out;
	}

	Protocol::PlayerActions Swing(std::uint32_t a_at)
	{
		return Protocol::PlayerActions{ { { a_at, "attackStart" }, { a_at + 400, "attackStop" } } };
	}
}

// ---- the wire ----

TEST("stand-in protocol: a player's actions come back as they went, alone and as another player's")
{
	const Protocol::PlayerActions mine = Swing(1000);
	const auto                    packet = Protocol::Encode(mine);
	CHECK(Protocol::PeekType(packet) == MT::kPlayerActions);
	const auto back = Protocol::DecodePlayerActions(packet);
	REQUIRE(back);
	CHECK(*back == mine);
	const Protocol::RemoteActions theirs{ 3, mine.actions };
	const auto                    remote = Protocol::DecodeRemoteActions(Protocol::Encode(theirs));
	REQUIRE(remote);
	CHECK(*remote == theirs);
	// None at all is a message too (it says nothing).
	CHECK(Protocol::DecodePlayerActions(Protocol::Encode(Protocol::PlayerActions{})).has_value());
}

TEST("stand-in protocol: a cut, a longer or a packet with too many actions or an empty event is not read")
{
	const auto check = [](const std::vector<std::uint8_t>& a_packet, auto a_decode) {
		CHECK(a_decode(a_packet).has_value());
		for (std::size_t length = 0; length < a_packet.size(); ++length) {
			CHECK((!a_decode(std::span<const std::uint8_t>{ a_packet.data(), length }).has_value()));
		}
		auto longer = a_packet;
		longer.push_back(0);
		CHECK(!a_decode(longer).has_value());
	};
	check(Protocol::Encode(Swing(5)), [](std::span<const std::uint8_t> a_data) { return Protocol::DecodePlayerActions(a_data); });
	check(Protocol::Encode(Protocol::RemoteActions{ 2, Swing(5).actions }), [](std::span<const std::uint8_t> a_data) { return Protocol::DecodeRemoteActions(a_data); });

	// More than MAX_PLAYER_ACTIONS go as MAX_PLAYER_ACTIONS; a packet that claims more is not read.
	Protocol::PlayerActions many;
	for (std::size_t i = 0; i < Protocol::MAX_PLAYER_ACTIONS + 4; ++i) {
		many.actions.push_back({ static_cast<std::uint32_t>(i + 1), "attackStart" });
	}
	const auto cut = Protocol::DecodePlayerActions(Protocol::Encode(many));
	REQUIRE(cut);
	CHECK(cut->actions.size() == Protocol::MAX_PLAYER_ACTIONS);
	Protocol::Writer writer{ MT::kPlayerActions };
	writer.U8(static_cast<std::uint8_t>(Protocol::MAX_PLAYER_ACTIONS + 1));
	for (std::size_t i = 0; i <= Protocol::MAX_PLAYER_ACTIONS; ++i) {
		writer.U32(1);
		writer.Str("attackStart", Protocol::MAX_EVENT_NAME);
	}
	CHECK(!Protocol::DecodePlayerActions(writer.Data()).has_value());

	// An empty event, or one of control characters only (read as empty), names nothing a graph takes.
	for (const std::string_view name : { std::string_view{}, std::string_view{ "\x01\x02" } }) {
		Protocol::Writer w{ MT::kPlayerActions };
		w.U8(1);
		w.U32(1);
		w.Str(name, Protocol::MAX_EVENT_NAME);
		CHECK(!Protocol::DecodePlayerActions(w.Data()).has_value());
	}
	static_assert(Protocol::MAX_PLAYER_ACTIONS_SIZE <= Protocol::MAX_MESSAGE_SIZE);
}

TEST("stand-in protocol: looks carry each hand, the quiver and the voice, and an unnamed bit no version knows is not read")
{
	auto looks = Armed();
	looks.unnamed = Protocol::kLeftUnnamed | Protocol::kVoiceUnnamed;
	const auto back = Protocol::DecodePlayerLooks(Protocol::Encode(looks));
	REQUIRE(back);
	CHECK(*back == looks);
	const auto remote = Protocol::DecodeRemoteLooks(Protocol::Encode(Protocol::RemoteLooks{ 2, looks }));
	REQUIRE(remote);
	CHECK(remote->looks == looks);

	auto packet = Protocol::Encode(looks);
	packet.back() = 0x10;  // the unnamed byte is the last
	CHECK(!Protocol::DecodePlayerLooks(packet).has_value());
	packet.back() = Protocol::kAllLooksUnnamed;
	CHECK(Protocol::DecodePlayerLooks(packet).has_value());
}

TEST("stand-in protocol: actions carry no form, and this protocol is version 9 with them as types 39 and 40")
{
	CHECK(!FormTranslate::CarriesForms(MT::kPlayerActions));
	CHECK(!FormTranslate::CarriesForms(MT::kRemoteActions));
	static_assert(Protocol::VERSION >= 9);  // 10 added faces (tests/FaceTests.cpp)
	static_assert(static_cast<int>(MT::kPlayerActions) == 39 && static_cast<int>(MT::kRemoteActions) == 40);
	// Through a mapper that changes forms, an actions packet is left as it is.
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "A.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "B.esp", "A.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, guest);
	FormIds::Extend(session, host);
	const FormIds::Mapper edge(host, session);
	REQUIRE(!edge.Identity());
	const auto packet = Protocol::Encode(Protocol::RemoteActions{ 2, Swing(9).actions });
	CHECK(FormTranslate::Translate(edge, FormTranslate::Way::kToLocal, packet).kind == FormTranslate::Result::Kind::kSame);
}

TEST("stand-in forms: a hand, the quiver or the voice the other game lacks reads 0 with its unnamed bit; one it has is its own")
{
	// The host's A.esp is 01; the guest's game lacks A.esp. B.esp is 02 in the host's game, 01 in the guest's.
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "A.esp", "B.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "B.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper hostEdge(host, session);
	const FormIds::Mapper guestEdge(guest, session);

	Protocol::PlayerLooks looks = Armed();
	looks.right = 0x01000801;  // A.esp's sword: the guest lacks it
	looks.left = 0x02000802;   // B.esp's spell
	looks.ammo = 0x01000900;   // A.esp's arrows
	looks.voice = 0x00013E07;  // Skyrim.esm's shout

	auto       out = FormTranslate::Translate(hostEdge, FormTranslate::Way::kToSession, Protocol::Encode(looks));
	const auto wire = out.kind == FormTranslate::Result::Kind::kChanged ? out.packet : Protocol::Encode(looks);
	auto       in = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, wire);
	REQUIRE(in.kind == FormTranslate::Result::Kind::kChanged);
	const auto read = Protocol::DecodePlayerLooks(in.packet);
	REQUIRE(read);
	CHECK(read->right == 0u);
	CHECK(read->left == 0x01000802u);
	CHECK(read->ammo == 0u);
	CHECK(read->voice == 0x00013E07u);
	CHECK(read->unnamed == (Protocol::kRightUnnamed | Protocol::kAmmoUnnamed));
	CHECK(read->worn == looks.worn);

	// Nothing held is not unnamed.
	Protocol::PlayerLooks bare;
	bare.worn = { 0x00012E49 };
	auto bareIn = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(bare));
	const auto bareRead = Protocol::DecodePlayerLooks(bareIn.kind == FormTranslate::Result::Kind::kChanged ? bareIn.packet : Protocol::Encode(bare));
	REQUIRE(bareRead);
	CHECK(bareRead->unnamed == 0);
}

// ---- the server ----

TEST("server: a player's actions go to the others under that player's number, reliably, and not back to the sender")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u, 3u }) {
		(void)f.fake->Take(peer);
	}
	f.fake->Deliver(2, Protocol::Encode(Swing(77)));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 3u }) {
		const auto sent = SentActions(f, peer);
		REQUIRE(sent.size() == 1);
		CHECK(sent[0].second);
		CHECK(sent[0].first.playerId == 2);
		CHECK(sent[0].first.actions == Swing(77).actions);
	}
	CHECK(SentActions(f, 2).empty());

	// A message that claims to be another player's is not: the server names the sender.
	f.fake->Deliver(3, Protocol::Encode(Protocol::RemoteActions{ 1, Swing(5).actions }));
	REQUIRE(f.fake->Settle());
	CHECK(SentActions(f, 1).empty());
	CHECK(SentActions(f, 2).empty());
}

TEST("server: a player's actions past their budget are dropped, said once, and the next ones go once it refills")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	// Ten full messages at once: 320 events, a burst of 96 passed on (a few more as the budget refills).
	Protocol::PlayerActions full;
	for (std::size_t i = 0; i < Protocol::MAX_PLAYER_ACTIONS; ++i) {
		full.actions.push_back({ static_cast<std::uint32_t>(i + 1), "attackStart" });
	}
	for (int i = 0; i < 10; ++i) {
		f.fake->Deliver(2, Protocol::Encode(full));
	}
	REQUIRE(f.fake->Settle());
	std::size_t passed = 0;
	for (const auto& [msg, reliable] : SentActions(f, 1)) {
		passed += msg.actions.size();
	}
	CHECK(passed >= 96);
	CHECK(passed < 10 * Protocol::MAX_PLAYER_ACTIONS);
	CHECK(f.LoggedSoon("server: player 2 sends graph events faster than they are passed on: the rest are dropped (said once)"));

	std::this_thread::sleep_for(200ms);  // about ten back
	f.fake->Deliver(2, Protocol::Encode(Swing(1)));
	REQUIRE(f.fake->Settle());
	const auto later = SentActions(f, 1);
	REQUIRE(later.size() == 1);
	CHECK(later[0].first.actions.size() == 2);
}

// ---- what kind of action an event is ----

TEST("stand-in rules: each kind of action is known by its event, whatever the case, and anything else is other")
{
	const std::vector<std::pair<std::string_view, Act>> cases{
		{ "attackStart", Act::kAttack },
		{ "attackStartLeftHand", Act::kAttack },
		{ "attackStartDualWield", Act::kAttack },
		{ "attackStop", Act::kAttack },
		{ "attackPowerStartForward", Act::kPower },
		{ "AttackPowerStart_Sprint", Act::kPower },
		{ "blockStart", Act::kBlock },
		{ "blockStop", Act::kBlock },
		{ "bashStart", Act::kBash },
		{ "bashPowerStart", Act::kBash },
		{ "bowAttackStart", Act::kBow },
		{ "BowZoomStart", Act::kBow },
		{ "bowReset", Act::kBow },
		{ "BeginCastRight", Act::kCast },
		{ "BeginCastLeft", Act::kCast },
		{ "CastStop", Act::kCast },
		{ "MRh_SpellFire_Event", Act::kCast },
		{ "BeginCastVoice", Act::kShout },
		{ "shoutStart", Act::kShout },
		{ "shoutRelease", Act::kShout },
		{ "WeapEquip", Act::kDraw },
		{ "Unequip", Act::kDraw },
		{ "MagicForceEquipLeft", Act::kDraw },
		{ "KillMoveStart", Act::kKillMove },
		{ "PairedStop", Act::kKillMove },
		{ "IdleChairSitting", Act::kOther },
		{ "", Act::kOther },
	};
	for (const auto& [event, act] : cases) {
		if (StandInRules::ActOf(event) != act) {
			Test::Fail(__FILE__, __LINE__, std::format("{} read as {}, not {}", event, StandInRules::ActName(StandInRules::ActOf(event)), StandInRules::ActName(act)));
		}
	}
	CHECK(StandInRules::ActName(Act::kCount) == "?");
}

TEST("stand-in rules: actions are counted by kind and said in order, the empty kinds left out")
{
	StandInRules::ActCounts counts;
	CHECK(counts.Line() == "none");
	CHECK(counts.Total() == 0);
	counts.Add("attackStart");
	counts.Add("attackStop");
	counts.Add("attackPowerStartForward");
	counts.Add("BeginCastVoice");
	counts.Add(Act::kOther);
	CHECK(counts.Of(Act::kAttack) == 2);
	CHECK(counts.Total() == 5);
	CHECK(counts.Line() == "attack:2,power:1,shout:1,other:1");
}

// ---- our player's events, on their way out ----

TEST("stand-in rules: the outbox keeps what a stand-in plays, in order, with its time, and leaves the walk's own events out")
{
	StandInRules::Outbox box;
	const auto           t = Steady::now();
	CHECK(box.Heard("attackStart", 100, t));
	CHECK(!box.Heard("moveStart", 101, t));    // the walk: each game works it out
	CHECK(!box.Heard("SprintStart", 101, t));  // so is the sprint
	CHECK(!box.Heard("", 102, t));
	CHECK(!box.Heard(std::string(Protocol::MAX_EVENT_NAME + 1, 'a'), 102, t));
	CHECK(box.Heard("blockStart", 103, t));
	CHECK(box.heard == 2);
	CHECK(box.byAct.Line() == "attack:1,block:1");
	CHECK(box.Waiting() == 2);
	const auto taken = box.Take();
	REQUIRE(taken.size() == 2);
	CHECK((taken[0] == Protocol::PlayerAction{ 100, "attackStart" }));
	CHECK((taken[1] == Protocol::PlayerAction{ 103, "blockStart" }));
	CHECK(box.sent == 2);
	CHECK(box.Take().empty());
}

TEST("stand-in rules: the outbox gives at most a message's worth at a time and drops past its budget, counted")
{
	StandInRules::Outbox box;
	const auto           t = Steady::now();
	std::size_t          kept = 0;
	for (int i = 0; i < 100; ++i) {
		kept += box.Heard("attackStart", static_cast<std::uint32_t>(i + 1), t) ? 1 : 0;
	}
	CHECK(kept == static_cast<std::size_t>(StandInRules::Outbox::BURST));
	CHECK(box.dropped == 100 - kept);
	CHECK(box.heard == 100);
	CHECK(box.Take().size() == Protocol::MAX_PLAYER_ACTIONS);
	CHECK(box.Waiting() == kept - Protocol::MAX_PLAYER_ACTIONS);
	// A second later the budget has room for more.
	CHECK(box.Heard("attackStop", 200, t + 1s));
	// Out of a session: nothing waits.
	box.Clear();
	CHECK(box.Waiting() == 0);
	CHECK(box.Take().empty());
}

TEST("stand-in rules: the outbox never holds more than MAX_WAITING, whatever the budget")
{
	StandInRules::Outbox box;
	auto                 t = Steady::now();
	std::size_t          kept = 0;
	// Events at a pace the budget allows, never taken: the queue stops at MAX_WAITING.
	for (int i = 0; i < 2000; ++i) {
		t += 40ms;
		kept += box.Heard("attackStart", static_cast<std::uint32_t>(i + 1), t) ? 1 : 0;
	}
	CHECK(box.Waiting() == StandInRules::Outbox::MAX_WAITING);
	CHECK(kept == StandInRules::Outbox::MAX_WAITING);
}

// ---- what a stand-in holds ----

TEST("stand-in rules: a stand-in's gear is its friend's worn items once each, hands, quiver, and the unnamed hands kept")
{
	auto looks = Armed();
	looks.worn.push_back(looks.worn.front());  // a list that names one twice
	looks.unnamed = Protocol::kRightUnnamed | Protocol::kVoiceUnnamed;
	const auto gear = StandInRules::GearFor(looks);
	REQUIRE(gear.worn.size() == 2);
	CHECK(gear.worn[0].id == 0x00012E49u);
	CHECK(gear.worn[1].id == 0x00012E4Bu);
	CHECK(gear.worn[0].slots == 0u);
	CHECK(gear.right == looks.right);
	CHECK(gear.left == looks.left);
	CHECK(gear.ammo == looks.ammo);
	CHECK(gear.unnamed == Protocol::kRightUnnamed);  // the voice is not an NPC's gear
	CHECK(gear.unnamedSlots == 0u);
}

TEST("stand-in rules: a stand-in is dressed through the same plan as a spawn's copy: given what it lacks, the old taken off")
{
	const auto gear = StandInRules::GearFor(Armed());
	NpcRules::Wearing now;
	now.armor = { { 0x00012E4D, 0x00012E4D, 0x4 } };  // the preset's own boots
	now.right = 0x00013982;                         // a dagger in hand
	const auto plan = NpcRules::PlanGear(gear, now, {}, true);
	CHECK(plan.missing == 0u);
	CHECK(std::ranges::any_of(plan.unequip, [](const NpcRules::GearItem& a_i) { return a_i.id == 0x00012E4Du; }));
	CHECK(std::ranges::any_of(plan.unequip, [](const NpcRules::GearItem& a_i) { return a_i.id == 0x00013982u; }));
	for (const auto id : { 0x00012E49u, 0x00012E4Bu, 0x00012EB7u, 0x00012EB6u, 0x0001397Du }) {
		if (std::ranges::none_of(plan.equip, [&](const NpcRules::GearItem& a_i) { return a_i.id == id; })) {
			Test::Fail(__FILE__, __LINE__, std::format("{:08X} is not put on", id));
		}
		if (std::ranges::none_of(plan.add, [&](const NpcRules::Count& a_c) { return a_c.id == id; })) {
			Test::Fail(__FILE__, __LINE__, std::format("{:08X} is not given", id));
		}
	}
}

TEST("stand-in rules: another body remakes a stand-in, other gear re-dresses it, the same looks do nothing")
{
	using StandInRules::Change;
	const auto was = Armed();
	CHECK(StandInRules::Compare(was, was) == Change::kSame);
	for (const auto change : { +[](Protocol::PlayerLooks& a_l) { a_l.right = 0; }, +[](Protocol::PlayerLooks& a_l) { a_l.left = 0x00013982; },
			 +[](Protocol::PlayerLooks& a_l) { a_l.ammo = 0; }, +[](Protocol::PlayerLooks& a_l) { a_l.voice = 0x00013E08; },
			 +[](Protocol::PlayerLooks& a_l) { a_l.worn.pop_back(); }, +[](Protocol::PlayerLooks& a_l) { a_l.unnamed = Protocol::kLeftUnnamed; } }) {
		auto now = was;
		change(now);
		CHECK(StandInRules::Compare(was, now) == Change::kRedress);
	}
	for (const auto change : { +[](Protocol::PlayerLooks& a_l) { a_l.race = 0x00013741; }, +[](Protocol::PlayerLooks& a_l) { a_l.female = true; },
			 +[](Protocol::PlayerLooks& a_l) { a_l.weight = 51.0f; } }) {
		auto now = was;
		change(now);
		now.right = 0;  // and the gear with it: still a remake
		CHECK(StandInRules::Compare(was, now) == Change::kRemake);
	}
}

TEST("stand-in rules: a stand-in's quiver is filled when it runs low, never past the stock")
{
	CHECK(StandInRules::AmmoToAdd(0) == StandInRules::AMMO_STOCK);
	CHECK(StandInRules::AmmoToAdd(-3) == StandInRules::AMMO_STOCK);
	CHECK(StandInRules::AmmoToAdd(StandInRules::AMMO_LOW - 1) == StandInRules::AMMO_STOCK - (StandInRules::AMMO_LOW - 1));
	CHECK(StandInRules::AmmoToAdd(StandInRules::AMMO_LOW) == 0);
	CHECK(StandInRules::AmmoToAdd(500) == 0);
}

// ---- what its spells do here ----

TEST("stand-in rules: no effect of a stand-in's takes hold on anyone else, of any kind")
{
	for (std::int32_t archetype = -1; archetype <= 46; ++archetype) {
		if (StandInRules::MagicKept(false, archetype)) {
			Test::Fail(__FILE__, __LINE__, std::format("an effect of kind {} on another actor is kept", archetype));
		}
	}
}

TEST("stand-in rules: on itself, what only shows on it is kept; what makes, moves or scripts anything is refused")
{
	using namespace StandInRules;
	for (const auto kept : { kArchNone, kValueModifier, kDualValueModifier, kInvisibility, kLight, kDarkness, kNightEye, kDetectLife, kValueAndParts,
			 kAccumulateMagnitude, kPeakValueModifier, kCloak, kEtherealize }) {
		CHECK(MagicKept(true, kept));
	}
	for (const auto refused : { kScript, kCommandSummoned, kBoundWeapon, kSummonCreature, kReanimate, kSpawnHazard, kSpawnScriptedRef }) {
		CHECK(!MagicKept(true, refused));
	}
	// The kinds no list names (calm, frenzy, paralysis, telekinesis, a werewolf's change...) are refused too.
	for (const auto other : { 2, 4, 6, 7, 8, 9, 15, 16, 20, 21, 23, 33, 36, 37, 44, 45, 46 }) {
		CHECK(!MagicKept(true, other));
	}
}

// ---- drawn or put away ----

TEST("stand-in rules: a weapon state that disagrees is fixed after the grace, never while drawing, and not again too soon")
{
	StandInRules::DrawFix fix;
	const auto            t = Steady::now();
	CHECK(!fix.Due(true, true, false, t));   // agrees
	CHECK(!fix.Due(true, false, false, t));  // disagrees: the grace starts
	CHECK(!fix.Due(true, false, false, t + StandInRules::DrawFix::GRACE - 1ms));
	CHECK(fix.Due(true, false, false, t + StandInRules::DrawFix::GRACE));
	CHECK(fix.fixes == 1);
	// Still not drawn: not again before the cool-down, however long it disagrees.
	const auto after = t + StandInRules::DrawFix::GRACE;
	CHECK(!fix.Due(true, false, false, after + 1ms));
	CHECK(!fix.Due(true, false, false, after + StandInRules::DrawFix::GRACE + 1ms));
	CHECK(fix.Due(true, false, false, after + StandInRules::DrawFix::COOL + StandInRules::DrawFix::GRACE));
	CHECK(fix.fixes == 2);

	// Drawing or sheathing now: no fix, and the grace starts over after.
	StandInRules::DrawFix busy;
	CHECK(!busy.Due(false, true, false, t));
	CHECK(!busy.Due(false, true, true, t + 1s));
	CHECK(!busy.Due(false, true, false, t + 2s));  // the grace starts here
	CHECK(!busy.Due(false, true, false, t + 2s + StandInRules::DrawFix::GRACE - 1ms));
	CHECK(busy.Due(false, true, false, t + 2s + StandInRules::DrawFix::GRACE));

	// A new stand-in starts clean.
	fix.Reset();
	CHECK(!fix.Due(false, true, false, after + 10s));
	CHECK(fix.Due(false, true, false, after + 10s + StandInRules::DrawFix::GRACE));
}

// ---- aim ----

TEST("stand-in rules: a stand-in aims where its friend does while drawn, held within the most, level otherwise")
{
	Protocol::PlayerState state;
	state.pitch = 30;
	CHECK(StandInRules::AimPitch(state) == 0.0f);  // sheathed: level
	state.flags = Protocol::kWeaponDrawn;
	const auto radians = [](float a_degrees) { return a_degrees * std::numbers::pi_v<float> / 180.0f; };
	CHECK(std::fabs(StandInRules::AimPitch(state) - -radians(30.0f)) < 1e-5f);  // up is negative in the game's x
	state.pitch = -20;
	CHECK(std::fabs(StandInRules::AimPitch(state) - radians(20.0f)) < 1e-5f);
	state.pitch = 90;
	CHECK(std::fabs(StandInRules::AimPitch(state) - -radians(StandInRules::MAX_AIM_DEGREES)) < 1e-5f);
	state.pitch = -90;
	CHECK(std::fabs(StandInRules::AimPitch(state) - radians(StandInRules::MAX_AIM_DEGREES)) < 1e-5f);
}

TEST("stand-in rules: a friend swimming under water is pitched as it swims, weapon out or not; at the surface it is level")
{
	const auto            radians = [](float a_degrees) { return a_degrees * std::numbers::pi_v<float> / 180.0f; };
	Protocol::PlayerState state;
	state.pitch = -70;  // diving steeply
	state.flags = Protocol::kSwimming | Protocol::kUnderwater;
	CHECK(std::fabs(StandInRules::AimPitch(state) - radians(70.0f)) < 1e-5f);
	state.flags = Protocol::kSwimming | Protocol::kUnderwater | Protocol::kWeaponDrawn;
	CHECK(std::fabs(StandInRules::AimPitch(state) - radians(70.0f)) < 1e-5f);  // the dive's hold (75), not the aim's (60)
	state.pitch = -89;
	CHECK(std::fabs(StandInRules::AimPitch(state) - radians(Gait::MAX_DIVE_DEGREES)) < 1e-5f);
	state.flags = Protocol::kSwimming;  // at the surface, sheathed
	CHECK(StandInRules::AimPitch(state) == 0.0f);
}

// ---- what the game shows our player doing (the rig's proof a press did something) ----

TEST("stand-in rules: each kind of action our player begins is seen once, by the state the game shows")
{
	using StandInRules::Doing;
	StandInRules::Seen seen;
	Doing              idle;
	idle.stamina = 100.0f;
	idle.magicka = 100.0f;
	seen.Sample(idle);
	seen.Sample(idle);
	CHECK(seen.byAct.Total() == 0);  // nothing begun

	// A swing: the attack states from draw to follow-through are one attack, however many readings it lasts.
	auto swing = idle;
	for (const std::uint32_t state : { 1u, 2u, 3u, 5u }) {
		swing.attackState = state;
		seen.Sample(swing);
	}
	seen.Sample(idle);
	CHECK(seen.byAct.Of(Act::kAttack) == 1);
	CHECK(seen.byAct.Of(Act::kPower) == 0);

	// A power attack: its attack data says so; its stamina spent is added up.
	auto power = idle;
	power.attackState = 2;
	power.power = true;
	power.stamina = 70.0f;
	seen.Sample(power);
	seen.Sample(idle);  // stamina back up: nothing spent by a rise
	CHECK(seen.byAct.Of(Act::kPower) == 1);
	CHECK(std::fabs(seen.staminaSpent - 30.0f) < 1e-4f);

	// A bash, a bow drawn and let go (one bow action through all its states), a block, a cast in each hand, a shout
	// (its caster, then its recovery: one shout), a weapon drawn and put away.
	auto bash = idle;
	bash.attackState = 6;
	seen.Sample(bash);
	seen.Sample(idle);
	auto bow = idle;
	for (const std::uint32_t state : { 8u, 9u, 10u, 11u, 12u }) {
		bow.attackState = state;
		seen.Sample(bow);
	}
	seen.Sample(idle);
	auto block = idle;
	block.blocking = true;
	seen.Sample(block);
	seen.Sample(block);
	seen.Sample(idle);
	auto cast = idle;
	cast.castRight = true;
	cast.magicka = 90.0f;
	seen.Sample(cast);
	cast.castLeft = true;  // the left hand begins while the right goes on: a second cast
	cast.magicka = 80.0f;
	seen.Sample(cast);
	seen.Sample(idle);
	auto shout = idle;
	shout.castVoice = true;
	seen.Sample(shout);
	shout.castVoice = false;
	shout.voiceRecovery = 5.0f;
	seen.Sample(shout);
	seen.Sample(idle);
	auto drawn = idle;
	drawn.drawn = true;
	seen.Sample(drawn);
	seen.Sample(idle);
	CHECK(seen.byAct.Of(Act::kBash) == 1);
	CHECK(seen.byAct.Of(Act::kBow) == 1);
	CHECK(seen.byAct.Of(Act::kBlock) == 1);
	CHECK(seen.byAct.Of(Act::kCast) == 2);
	CHECK(seen.byAct.Of(Act::kShout) == 1);
	CHECK(seen.byAct.Of(Act::kDraw) == 2);
	CHECK(std::fabs(seen.magickaSpent - 20.0f) < 1e-4f);
	CHECK(seen.byAct.Of(Act::kAttack) == 1);  // none of those was a swing

	// After a while unread (a load), the first reading begins nothing: a state already under way is not counted.
	seen.Forget();
	seen.Sample(swing);
	CHECK(seen.byAct.Of(Act::kAttack) == 1);
}

// ---- what a stand-in may hold ----

TEST("stand-in rules: a hand keeps playable and conjured weapons, spells, scrolls, torches and playable shields")
{
	using StandInRules::Holdable;
	using K = Holdable::Kind;
	CHECK(StandInRules::KeptInHand({ K::kWeapon, true, false }));
	CHECK(StandInRules::KeptInHand({ K::kWeapon, false, true }));  // Bound Sword, Bound Bow, a mod's conjured weapon
	CHECK(!StandInRules::KeptInHand({ K::kWeapon, false, false }));  // a creature's or a non-playable weapon
	CHECK(StandInRules::KeptInHand({ K::kSpell, true, false }));
	CHECK(StandInRules::KeptInHand({ K::kScroll, true, false }));
	CHECK(StandInRules::KeptInHand({ K::kTorch, true, false }));
	CHECK(StandInRules::KeptInHand({ K::kShield, true, false }));
	CHECK(!StandInRules::KeptInHand({ K::kShield, false, false }));
	for (const auto kind : { K::kNothing, K::kAmmo, K::kShout, K::kPower, K::kOther }) {
		CHECK(!StandInRules::KeptInHand({ kind, true, false }));
	}
}

TEST("stand-in rules: the quiver keeps playable ammunition, and a conjured weapon's own while one is in a hand")
{
	using StandInRules::Holdable;
	using K = Holdable::Kind;
	CHECK(StandInRules::KeptInQuiver({ K::kAmmo, true, false }, false));
	CHECK(!StandInRules::KeptInQuiver({ K::kAmmo, false, false }, false));  // a non-playable arrow, no bound bow
	CHECK(StandInRules::KeptInQuiver({ K::kAmmo, false, false }, true));    // a conjured bow's own non-playable arrows
	CHECK(!StandInRules::KeptInQuiver({ K::kWeapon, true, false }, true));
	CHECK(!StandInRules::KeptInQuiver({ K::kNothing, true, false }, true));
}

TEST("stand-in rules: the voice keeps a shout or a power, nothing else")
{
	using StandInRules::Holdable;
	using K = Holdable::Kind;
	CHECK(StandInRules::KeptAsVoice({ K::kShout, true, false }));
	CHECK(StandInRules::KeptAsVoice({ K::kPower, true, false }));
	for (const auto kind : { K::kNothing, K::kWeapon, K::kSpell, K::kScroll, K::kTorch, K::kShield, K::kAmmo, K::kOther }) {
		CHECK(!StandInRules::KeptAsVoice({ kind, true, false }));
	}
}

TEST("stand-in rules: a voice that is not its friend's is drift, none included; an unnamed one is not judged")
{
	CHECK(!StandInRules::VoiceDrifted(0x00013E07, false, 0x00013E07));
	CHECK(StandInRules::VoiceDrifted(0x00013E07, false, 0));           // it did not take, or the game cleared it
	CHECK(StandInRules::VoiceDrifted(0x00013E07, false, 0x00013E08));  // another one
	CHECK(StandInRules::VoiceDrifted(0, false, 0x00013E07));           // its friend has none ready now
	CHECK(!StandInRules::VoiceDrifted(0, false, 0));
	CHECK(!StandInRules::VoiceDrifted(0, true, 0x00013E07));  // their game could not name it: kept as it is
}
