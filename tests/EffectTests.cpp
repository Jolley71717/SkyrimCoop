#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/EffectRules.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A player's magic on NPCs other games run, dealt in their runners' games, and Slow Time shared (docs/DESIGN.md, "Magic
// on NPCs"): the messages, the server routing them, and the rules a game sorts its effects by.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Why = Protocol::ClaimWhy;
	using EffectRules::Route;

	constexpr Protocol::NpcKey BANDIT{ 0x00012345, 0 };
	constexpr Protocol::NpcKey DRAGON{ 0x0001CA05, 0 };
	constexpr Protocol::NpcKey WOLF_OF_1{ 0xFF000801, 1 };  // a spawn, the key its spawner's game gave it

	// Unrelenting Force's first word's push on a bandit.
	Protocol::NpcEffect Push(Protocol::NpcKey a_key = BANDIT)
	{
		return Protocol::NpcEffect{ .key = a_key, .item = 0x00013F39, .index = 1, .magnitude = 1.0f, .power = 1.0f, .castingSource = 2, .flags = Protocol::kEffectArea };
	}

	template <class T, class Decode>
	std::vector<std::pair<T, bool>> SentOf(Fixture& a_fixture, PeerId a_peer, MT a_type, Decode a_decode)
	{
		std::vector<std::pair<T, bool>> out;
		for (auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == a_type) {
				if (auto msg = a_decode(sent.data)) {
					out.emplace_back(std::move(*msg), sent.reliable);
				}
			}
		}
		return out;
	}

	void AllIn(Fixture& a_f, PeerId a_count)
	{
		const char* names[] = { "Ada", "Bo", "Cy", "Di" };
		for (PeerId peer = 1; peer <= a_count; ++peer) {
			REQUIRE(a_f.Join(peer, Fixture::MakeHello(names[peer - 1])));
		}
		REQUIRE(a_f.fake->Settle());
		for (PeerId peer = 1; peer <= a_count; ++peer) {
			(void)a_f.fake->Take(peer);
		}
	}

	void Forget(Fixture& a_f, std::initializer_list<PeerId> a_peers)
	{
		REQUIRE(a_f.fake->Settle());
		for (const auto peer : a_peers) {
			(void)a_f.fake->Take(peer);
		}
	}

	using Names = std::vector<std::string>;

	// The host's and a guest's load orders: B.esp is 03 in the host's game and 01 in the guest's; Extra.esp is the
	// host's alone.
	struct Edges
	{
		Names           host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "A.esp", "B.esp", "Extra.esp" }, {});
		Names           guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "B.esp", "Update.esm", "A.esp" }, {});
		Names           session;
		FormIds::Mapper hostEdge;
		FormIds::Mapper guestEdge;

		Edges()
		{
			FormIds::Extend(session, host);
			FormIds::Extend(session, guest);
			hostEdge = FormIds::Mapper(host, session);
			guestEdge = FormIds::Mapper(guest, session);
		}

		std::vector<std::uint8_t> HostToGuest(const std::vector<std::uint8_t>& a_packet) const
		{
			auto       out = FormTranslate::Translate(hostEdge, FormTranslate::Way::kToSession, a_packet);
			const auto wire = out.kind == FormTranslate::Result::Kind::kChanged ? out.packet : a_packet;
			auto       in = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, wire);
			return in.kind == FormTranslate::Result::Kind::kChanged ? in.packet : wire;
		}
	};
}

// ---- the messages ----

TEST("effect protocol: effects and Slow Times read back as written, as messages 81 and 82 of protocol 29, and carry forms")
{
	static_assert(Protocol::VERSION >= 29);
	static_assert(static_cast<std::uint8_t>(MT::kNpcEffects) == 81);
	static_assert(static_cast<std::uint8_t>(MT::kSlowTimes) == 82);
	Protocol::NpcEffect poison{ .key = WOLF_OF_1, .item = 0x0003A5A4, .index = 0, .source = 0x00012EB7, .magnitude = 25.0f, .power = 1.0f, .castingSource = 1 };
	Protocol::NpcEffect dual = Push(DRAGON);
	dual.flags = Protocol::kEffectDual;
	dual.castingSource = 0;
	const Protocol::NpcEffects effects{ .caster = 2, .effects = { Push(), poison, dual } };
	CHECK(Protocol::DecodeNpcEffects(Protocol::Encode(effects)) == effects);
	CHECK(Protocol::PeekType(Protocol::Encode(effects)) == MT::kNpcEffects);
	CHECK(Protocol::DecodeNpcEffects(Protocol::Encode(Protocol::NpcEffects{})) == Protocol::NpcEffects{});

	const Protocol::SlowTime slow{ .player = 3, .item = 0x00048AC9, .index = 0, .magnitude = 0.3f, .power = 1.0f };
	CHECK(Protocol::DecodeSlowTime(Protocol::Encode(slow)) == slow);
	CHECK(Protocol::PeekType(Protocol::Encode(slow)) == MT::kSlowTimes);

	CHECK(FormTranslate::CarriesForms(MT::kNpcEffects));
	CHECK(FormTranslate::CarriesForms(MT::kSlowTimes));
}

TEST("effect protocol: the most fit; more, an effect of no NPC or no item, an amount that is none, an unknown hand or flag, or a byte off are refused")
{
	Protocol::NpcEffects most{ .caster = 1 };
	for (std::size_t i = 0; i < Protocol::MAX_NPC_EFFECTS; ++i) {
		most.effects.push_back(Push());
	}
	auto bytes = Protocol::Encode(most);
	CHECK(bytes.size() == Protocol::MAX_NPC_EFFECTS_SIZE);
	CHECK(bytes.size() <= 1200u);
	CHECK(Protocol::DecodeNpcEffects(bytes) == most);
	bytes[5] = static_cast<std::uint8_t>(Protocol::MAX_NPC_EFFECTS + 1);
	CHECK(!Protocol::DecodeNpcEffects(bytes));
	most.effects.push_back(Push());
	CHECK(Protocol::DecodeNpcEffects(Protocol::Encode(most))->effects.size() == Protocol::MAX_NPC_EFFECTS);

	const auto refused = [](Protocol::NpcEffect a_effect) {
		return !Protocol::DecodeNpcEffects(Protocol::Encode(Protocol::NpcEffects{ .effects = { Push(), a_effect } }));
	};
	auto bad = Push({ 0, 0 });
	CHECK(refused(bad));
	bad = Push();
	bad.item = 0;
	CHECK(refused(bad));
	bad = Push();
	bad.magnitude = std::numeric_limits<float>::quiet_NaN();
	CHECK(refused(bad));
	bad = Push();
	bad.magnitude = -1.0f;
	CHECK(refused(bad));
	bad = Push();
	bad.power = std::numeric_limits<float>::infinity();
	CHECK(refused(bad));
	bad = Push();
	bad.magnitude = 2.0e6f;
	CHECK(refused(bad));
	bad = Push();
	bad.castingSource = Protocol::MAX_CASTING_SOURCE + 1;
	CHECK(refused(bad));
	bad = Push();
	bad.flags = 4;
	CHECK(refused(bad));
	auto extra = Protocol::Encode(Protocol::NpcEffects{ .effects = { Push() } });
	extra.push_back(0);
	CHECK(!Protocol::DecodeNpcEffects(extra));
	auto shorter = Protocol::Encode(Protocol::NpcEffects{ .effects = { Push() } });
	shorter.pop_back();
	CHECK(!Protocol::DecodeNpcEffects(shorter));

	CHECK(!Protocol::DecodeSlowTime(Protocol::Encode(Protocol::SlowTime{ .item = 0, .magnitude = 0.3f })));
	CHECK(!Protocol::DecodeSlowTime(Protocol::Encode(Protocol::SlowTime{ .item = 0x00048AC9, .magnitude = std::nanf("") })));
	auto slowExtra = Protocol::Encode(Protocol::SlowTime{ .item = 0x00048AC9, .magnitude = 0.3f });
	slowExtra.push_back(0);
	CHECK(!Protocol::DecodeSlowTime(slowExtra));
}

TEST("effect form translate: an effect on an NPC the reader lacks, or of an item it lacks, is left out; a source it lacks is none")
{
	const Edges            e;
	const Protocol::NpcKey ofExtra{ 0x04000ABC, 0 };  // Extra.esp's: the guest lacks it
	const Protocol::NpcKey ofB{ 0x03000ABC, 0 };
	const Protocol::NpcKey inGuest{ 0x01000ABC, 0 };

	auto onB = Push(ofB);
	onB.item = 0x03000123;    // B.esp's spell: renumbered
	onB.source = 0x04000777;  // Extra.esp's weapon: none
	auto extraItem = Push();
	extraItem.item = 0x04000456;  // Extra.esp's spell: left out
	const auto onExtra = Push(ofExtra);
	const auto spawn = Push(WOLF_OF_1);

	const auto effects = Protocol::DecodeNpcEffects(e.HostToGuest(Protocol::Encode(Protocol::NpcEffects{ .caster = 1, .effects = { onB, extraItem, onExtra, spawn } })));
	REQUIRE(effects);
	REQUIRE(effects->effects.size() == 2u);
	CHECK((effects->effects[0].key == inGuest));
	CHECK(effects->effects[0].item == 0x01000123u);
	CHECK(effects->effects[0].source == 0u);
	CHECK((effects->effects[1].key == WOLF_OF_1));

	// A Slow Time of an item the reader lacks reads none, which its reader refuses.
	CHECK(!Protocol::DecodeSlowTime(e.HostToGuest(Protocol::Encode(Protocol::SlowTime{ .player = 1, .item = 0x04000456, .magnitude = 0.3f }))));
	const auto kept = Protocol::DecodeSlowTime(e.HostToGuest(Protocol::Encode(Protocol::SlowTime{ .player = 1, .item = 0x00048AC9, .magnitude = 0.3f })));
	REQUIRE(kept);
	CHECK(kept->item == 0x00048AC9u);
}

// ---- the server ----

TEST("effect server: each effect goes to its NPC's runner alone, the caster named; one on an NPC nobody runs, or the caster's own, goes nowhere")
{
	Fixture f;
	AllIn(f, 3);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { BANDIT, Why::kHave } } }));
	f.fake->Deliver(3, Protocol::Encode(Protocol::NpcClaims{ .claims = { { DRAGON, Why::kHave } } }));
	Forget(f, { 1, 2, 3 });

	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcEffects{ .caster = 99, .effects = { Push(BANDIT), Push(DRAGON), Push(WOLF_OF_1) } }));
	REQUIRE(f.fake->Settle());
	const auto toAda = SentOf<Protocol::NpcEffects>(f, 1, MT::kNpcEffects, Protocol::DecodeNpcEffects);
	REQUIRE(toAda.size() == 1u);
	CHECK(toAda[0].second);
	CHECK(toAda[0].first.caster == 2u);
	CHECK(toAda[0].first.effects == std::vector<Protocol::NpcEffect>({ Push(BANDIT) }));
	const auto toCy = SentOf<Protocol::NpcEffects>(f, 3, MT::kNpcEffects, Protocol::DecodeNpcEffects);
	REQUIRE(toCy.size() == 1u);
	CHECK(toCy[0].first.effects == std::vector<Protocol::NpcEffect>({ Push(DRAGON) }));
	CHECK(f.fake->Take(2).empty());

	// The runner's own on its NPC (a copy it held a moment ago): nowhere.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcEffects{ .effects = { Push(BANDIT) } }));
	REQUIRE(f.fake->Settle());
	for (PeerId peer = 1; peer <= 3; ++peer) {
		CHECK(SentOf<Protocol::NpcEffects>(f, peer, MT::kNpcEffects, Protocol::DecodeNpcEffects).empty());
	}
}

TEST("effect server: a caster's effects past its budget are dropped")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { BANDIT, Why::kHave } } }));
	Forget(f, { 1, 2 });
	std::size_t passed = 0;
	for (int i = 0; i < 10; ++i) {
		Protocol::NpcEffects many;
		for (std::size_t k = 0; k < Protocol::MAX_NPC_EFFECTS; ++k) {
			many.effects.push_back(Push());
		}
		f.fake->Deliver(2, Protocol::Encode(many));
	}
	REQUIRE(f.fake->Settle());
	for (const auto& [msg, reliable] : SentOf<Protocol::NpcEffects>(f, 1, MT::kNpcEffects, Protocol::DecodeNpcEffects)) {
		passed += msg.effects.size();
	}
	CHECK(passed >= 64u);
	CHECK(passed < 10u * Protocol::MAX_NPC_EFFECTS);
}

TEST("effect server: a player's Slow Time goes to the others, the player named, within its budget")
{
	Fixture f;
	AllIn(f, 3);
	f.fake->Deliver(2, Protocol::Encode(Protocol::SlowTime{ .player = 7, .item = 0x00048AC9, .magnitude = 0.3f }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 3 } }) {
		const auto got = SentOf<Protocol::SlowTime>(f, peer, MT::kSlowTimes, Protocol::DecodeSlowTime);
		REQUIRE(got.size() == 1u);
		CHECK(got[0].second);
		CHECK(got[0].first.player == 2u);
		CHECK(got[0].first.item == 0x00048AC9u);
	}
	CHECK(SentOf<Protocol::SlowTime>(f, 2, MT::kSlowTimes, Protocol::DecodeSlowTime).empty());
	for (int i = 0; i < 20; ++i) {
		f.fake->Deliver(2, Protocol::Encode(Protocol::SlowTime{ .item = 0x00048AC9, .magnitude = 0.3f }));
	}
	REQUIRE(f.fake->Settle());
	CHECK(SentOf<Protocol::SlowTime>(f, 1, MT::kSlowTimes, Protocol::DecodeSlowTime).size() < 20u);
}

// ---- the rules ----

TEST("effect rules: what a shout or a spell does to an NPC goes to its runner; its health stays here, and what makes or moves something here")
{
	using namespace EffectRules;
	constexpr std::int32_t STAMINA = 26;
	constexpr std::int32_t SPEED_MULT = 30;
	// Unrelenting Force: its push to the runner, its health damage here.
	CHECK(OnCopy(kStagger, -1) == Route::kRunner);
	CHECK(OnCopy(kValueModifier, AV_HEALTH) == Route::kHere);
	// Fire and Frost Breath: the fire's and the frost's on health here; frost's slow, its push and paralysis to the runner.
	CHECK(OnCopy(kDualValueModifier, AV_HEALTH) == Route::kHere);
	CHECK(OnCopy(kPeakValueModifier, SPEED_MULT) == Route::kRunner);
	CHECK(OnCopy(kParalysis, -1) == Route::kRunner);
	// Dismay, Kyne's Peace, Disarm, Dragonrend's and Soul Tear's scripts, Marked for Death's armor, a stamina drain.
	CHECK(OnCopy(kDemoralize, -1) == Route::kRunner);
	CHECK(OnCopy(kCalm, -1) == Route::kRunner);
	CHECK(OnCopy(kFrenzy, -1) == Route::kRunner);
	CHECK(OnCopy(kDisarm, -1) == Route::kRunner);
	CHECK(OnCopy(kScript, -1) == Route::kRunner);
	CHECK(OnCopy(kValueModifier, STAMINA) == Route::kRunner);
	CHECK(OnCopy(kAccumulateMagnitude, AV_HEALTH) == Route::kHere);
	CHECK(OnCopy(kTurnUndead, -1) == Route::kRunner);
	CHECK(OnCopy(kBanish, -1) == Route::kRunner);
	CHECK(OnCopy(kDispel, -1) == Route::kRunner);
	// Ours: an absorb gives to our player; a soul trap fills our gem; a raised corpse, a summon, a grab are this world's.
	CHECK(OnCopy(kAbsorb, STAMINA) == Route::kHere);
	CHECK(OnCopy(kSoulTrap, -1) == Route::kHere);
	CHECK(OnCopy(kReanimate, -1) == Route::kHere);
	CHECK(OnCopy(kCommandSummoned, -1) == Route::kHere);
	CHECK(OnCopy(kGrabActor, -1) == Route::kHere);
	CHECK(OnCopy(kTelekinesis, -1) == Route::kHere);
	CHECK(OnCopy(kSpawnHazard, -1) == Route::kHere);
	CHECK(OnCopy(kArchNone, -1) == Route::kHere);
	// What only shows: both.
	CHECK(OnCopy(kInvisibility, -1) == Route::kBoth);
	CHECK(OnCopy(kEtherealize, -1) == Route::kBoth);
	CHECK(RouteName(Route::kRunner) == "runner");
	// Every kind is sorted: a kind the game has but the list names not goes to the runner.
	for (std::int32_t kind = kValueModifier; kind <= kVampireLord; ++kind) {
		const auto route = OnCopy(kind, -1);
		CHECK((route == Route::kHere || route == Route::kRunner || route == Route::kBoth));
	}
}

TEST("effect rules: Slow Time alone is shared, with a player in the same space within reach of the shouter")
{
	using namespace EffectRules;
	CHECK(SharedWithPlayers(kSlowTime));
	CHECK(!SharedWithPlayers(kEtherealize));
	CHECK(!SharedWithPlayers(kStagger));
	CHECK(SharesSlowTime(true, 100.0f));
	CHECK(SharesSlowTime(true, SLOW_TIME_WITHIN));
	CHECK(!SharesSlowTime(true, SLOW_TIME_WITHIN + 1.0f));
	CHECK(!SharesSlowTime(false, std::nullopt));
	CHECK(!SharesSlowTime(true, std::nullopt));
}

TEST("effect rules: the same effect on the same NPC goes once per RESEND_AFTER; another effect, NPC or item goes at once")
{
	using namespace EffectRules;
	const Clock::time_point t0{ 100s };
	Repeats<std::uint32_t> r;
	CHECK(r.Due(1, 0x100, 0, t0));
	CHECK(!r.Due(1, 0x100, 0, t0 + 100ms));
	CHECK(r.Due(1, 0x100, 1, t0 + 100ms));
	CHECK(r.Due(2, 0x100, 0, t0 + 100ms));
	CHECK(r.Due(1, 0x200, 0, t0 + 100ms));
	CHECK(r.Due(1, 0x100, 0, t0 + RESEND_AFTER));
	// It forgets old ones past MAX_KEPT.
	for (std::uint32_t i = 0; i < 300; ++i) {
		r.Due(1000 + i, 0x100, 0, t0 + 10s);
	}
	CHECK(r.Due(1000, 0x100, 0, t0 + 11s));
	CHECK(r.Size() <= 304u);
	r.Clear();
	CHECK(r.Size() == 0u);
}
