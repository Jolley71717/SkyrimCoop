#include "Test.h"

#include "FormTranslate.h"
#include "NpcFates.h"
#include "ServerFixture.h"
#include "game/CopyHealth.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// NPCs' health and deaths (docs/DESIGN.md, "NPC health and deaths"): their messages, the server sending each hit to its
// NPC's runner alone and taking one death of each NPC, and what a copy of an NPC shows of its runner's word.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Why = Protocol::ClaimWhy;
	using Verdict = Protocol::HitVerdict;
	using Do = CopyHealth::Do;
	using Steady = std::chrono::steady_clock;

	constexpr Protocol::NpcKey GUARD{ 0x00012345, 0 };
	constexpr Protocol::NpcKey WOLF{ 0x00023456, 0 };
	constexpr Protocol::NpcKey ATRONACH_OF_1{ 0xFF000801, 1 };

	bool Near(float a_a, float a_b) { return std::fabs(a_a - a_b) < 1e-4f; }

	Protocol::NpcClaims Claims(std::initializer_list<Protocol::NpcClaim> a_claims) { return Protocol::NpcClaims{ .claims = a_claims }; }

	// Every message of type T the server sent a_peer so far, decoded, with whether it went reliably.
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

	// Players 1 to a_count in a session, everything they were sent so far forgotten.
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

	// One copy's health, looked at as a game would, its clock moved by hand.
	struct Run
	{
		CopyHealth::Copy   copy;
		CopyHealth::Counts counts;
		std::uint32_t      seq = 0;
		Steady::time_point t = Steady::now();

		CopyHealth::Step Look(const CopyHealth::Seen& a_seen, std::optional<CopyHealth::OurHit> a_hit = std::nullopt, bool a_mayRevive = true, bool a_hostsSave = false)
		{
			return copy.Look(GUARD, a_seen, a_hit, [this] { return ++seq; }, a_mayRevive, a_hostsSave, t, counts);
		}

		CopyHealth::OurHit Hit(std::uint8_t a_flags = 0) const { return { t, a_flags }; }
	};
}

// ---- the messages ----

TEST("npc combat protocol: hits, answers, healths and deaths read back as they were written")
{
	const Protocol::NpcHits hits{ .hitter = 2,
		.hits = { { GUARD, 1, 0.25f, Protocol::kHitPower | Protocol::kHitSneak }, { ATRONACH_OF_1, 2, 1.0f, Protocol::kHitKilling | Protocol::kHitKillMove }, { WOLF, 3, 0.5f, Protocol::kHitScript } } };
	CHECK(Protocol::DecodeNpcHits(Protocol::Encode(hits)) == hits);
	CHECK(Protocol::PeekType(Protocol::Encode(hits)) == MT::kNpcHits);

	const Protocol::NpcHitAnswers answers{ .answers = { { GUARD, 2, 1, Verdict::kApplied, 0.5f }, { WOLF, 2, 3, Verdict::kBusy, 0.0f } } };
	CHECK(Protocol::DecodeNpcHitAnswers(Protocol::Encode(answers)) == answers);
	CHECK(Protocol::PeekType(Protocol::Encode(answers)) == MT::kNpcHitAnswers);

	const Protocol::NpcHealths healths{ .runner = 1, .npcs = { { GUARD, 0.5f }, { WOLF, 1.0f } } };
	CHECK(Protocol::DecodeNpcHealths(Protocol::Encode(healths)) == healths);
	CHECK(Protocol::PeekType(Protocol::Encode(healths)) == MT::kNpcHealths);

	const Protocol::NpcDeaths deaths{ .runner = 1, .full = true, .deaths = { { GUARD, 2, Protocol::kDeathKillMove }, { WOLF, 0, 0 } } };
	CHECK(Protocol::DecodeNpcDeaths(Protocol::Encode(deaths)) == deaths);
	CHECK(Protocol::PeekType(Protocol::Encode(deaths)) == MT::kNpcDeaths);

	// Empty lists read (a translation may leave every entry out).
	CHECK(Protocol::DecodeNpcHits(Protocol::Encode(Protocol::NpcHits{})));
	CHECK(Protocol::DecodeNpcDeaths(Protocol::Encode(Protocol::NpcDeaths{})));
}

TEST("npc combat protocol: the longest of each fits, and one past a limit, cut short, or with a value out of range is refused")
{
	Protocol::NpcHits manyHits;
	Protocol::NpcHitAnswers manyAnswers;
	for (std::uint32_t i = 0; i < Protocol::MAX_NPC_HITS; ++i) {
		manyHits.hits.push_back({ { 0x00100000 + i, 0 }, i + 1, 0.5f, Protocol::kHitAllFlags });
		manyAnswers.answers.push_back({ { 0x00100000 + i, 0 }, 2, i + 1, Verdict::kApplied, 0.5f });
	}
	Protocol::NpcHealths manyHealths;
	Protocol::NpcDeaths  manyDeaths{ .full = true };
	for (std::uint32_t i = 0; i < Protocol::MAX_NPCS; ++i) {
		manyHealths.npcs.push_back({ { 0x00100000 + i, 0 }, 0.5f });
		manyDeaths.deaths.push_back({ { 0x00100000 + i, 0 }, 2, Protocol::kDeathAllFlags });
	}
	CHECK(Protocol::Encode(manyHits).size() == Protocol::MAX_NPC_HITS_SIZE);
	CHECK(Protocol::Encode(manyAnswers).size() == Protocol::MAX_NPC_HIT_ANSWERS_SIZE);
	CHECK(Protocol::Encode(manyHealths).size() == Protocol::MAX_NPC_HEALTHS_SIZE);
	CHECK(Protocol::Encode(manyDeaths).size() == Protocol::MAX_NPC_DEATHS_SIZE);
	CHECK(Protocol::DecodeNpcHits(Protocol::Encode(manyHits)) == manyHits);
	CHECK(Protocol::DecodeNpcHitAnswers(Protocol::Encode(manyAnswers)) == manyAnswers);
	CHECK(Protocol::DecodeNpcHealths(Protocol::Encode(manyHealths)) == manyHealths);
	CHECK(Protocol::DecodeNpcDeaths(Protocol::Encode(manyDeaths)) == manyDeaths);

	// Encoding stops at the limit; a count past it is refused.
	auto tooMany = manyHits;
	tooMany.hits.push_back({ GUARD, 999, 0.5f, 0 });
	CHECK(Protocol::DecodeNpcHits(Protocol::Encode(tooMany))->hits.size() == Protocol::MAX_NPC_HITS);
	auto countPast = Protocol::Encode(manyHits);
	countPast[5] = static_cast<std::uint8_t>(Protocol::MAX_NPC_HITS + 1);  // type, hitter, then the count
	CHECK(!Protocol::DecodeNpcHits(countPast));
	auto answersPast = Protocol::Encode(manyAnswers);
	answersPast[1] = static_cast<std::uint8_t>(Protocol::MAX_NPC_HITS + 1);
	CHECK(!Protocol::DecodeNpcHitAnswers(answersPast));

	// Cut short, or something after the end.
	for (auto bytes : { Protocol::Encode(manyHits), Protocol::Encode(manyAnswers), Protocol::Encode(manyHealths), Protocol::Encode(manyDeaths) }) {
		auto shorter = bytes;
		shorter.pop_back();
		auto longer = bytes;
		longer.push_back(0);
		for (const auto& changed : { shorter, longer }) {
			const auto type = Protocol::PeekType(changed);
			CHECK(type.has_value());
			switch (type.value_or(MT::kNpcDeaths)) {
			case MT::kNpcHits:
				CHECK(!Protocol::DecodeNpcHits(changed));
				break;
			case MT::kNpcHitAnswers:
				CHECK(!Protocol::DecodeNpcHitAnswers(changed));
				break;
			case MT::kNpcHealths:
				CHECK(!Protocol::DecodeNpcHealths(changed));
				break;
			default:
				CHECK(!Protocol::DecodeNpcDeaths(changed));
				break;
			}
		}
	}

	// A hit of no NPC, numbered 0, of no share, or of a flag unknown.
	const auto hitOf = [](Protocol::NpcHit a_hit) { return Protocol::DecodeNpcHits(Protocol::Encode(Protocol::NpcHits{ .hits = { a_hit } })); };
	CHECK(hitOf({ GUARD, 1, 0.0f, 0 }));
	CHECK(!hitOf({ { 0, 0 }, 1, 0.5f, 0 }));
	CHECK(!hitOf({ GUARD, 0, 0.5f, 0 }));
	CHECK(!hitOf({ GUARD, 1, 1.5f, 0 }));
	CHECK(!hitOf({ GUARD, 1, -0.1f, 0 }));
	CHECK(!hitOf({ GUARD, 1, std::nanf(""), 0 }));
	CHECK(!hitOf({ GUARD, 1, 0.5f, 0x40 }));

	// An answer of no hitter, no hit, an unknown verdict or no share.
	const auto answerOf = [](Protocol::NpcHitAnswer a_answer) {
		return Protocol::DecodeNpcHitAnswers(Protocol::Encode(Protocol::NpcHitAnswers{ .answers = { a_answer } }));
	};
	CHECK(answerOf({ GUARD, 2, 1, Verdict::kNotHere, 0.0f }));
	CHECK(!answerOf({ GUARD, 0, 1, Verdict::kApplied, 0.5f }));
	CHECK(!answerOf({ GUARD, 2, 0, Verdict::kApplied, 0.5f }));
	CHECK(!answerOf({ GUARD, 2, 1, static_cast<Verdict>(Protocol::HIT_VERDICT_COUNT), 0.5f }));
	CHECK(!answerOf({ GUARD, 2, 1, Verdict::kApplied, 2.0f }));

	// A health of none is a death, said as one; more than all is none.
	const auto healthOf = [](float a_share) { return Protocol::DecodeNpcHealths(Protocol::Encode(Protocol::NpcHealths{ .npcs = { { GUARD, a_share } } })); };
	CHECK(healthOf(0.001f));
	CHECK(healthOf(1.0f));
	CHECK(!healthOf(0.0f));
	CHECK(!healthOf(1.01f));
	CHECK(!healthOf(std::nanf("")));

	// A death of no NPC or of a flag unknown, and a full flag that is no flag.
	CHECK(!Protocol::DecodeNpcDeaths(Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { { 0, 0 }, 1, 0 } } })));
	CHECK(!Protocol::DecodeNpcDeaths(Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { GUARD, 1, 0x02 } } })));
	auto fullTwo = Protocol::Encode(Protocol::NpcDeaths{ .full = true, .deaths = { { GUARD, 1, 0 } } });
	fullTwo[5] = 2;  // type, runner, then full
	CHECK(!Protocol::DecodeNpcDeaths(fullTwo));
}

TEST("npc combat protocol: hits, answers, healths and deaths carry forms, and came in protocol version 8")
{
	for (const auto type : { MT::kNpcHits, MT::kNpcHitAnswers, MT::kNpcHealths, MT::kNpcDeaths }) {
		CHECK(FormTranslate::CarriesForms(type));
	}
	static_assert(Protocol::VERSION >= 8);
	static_assert(static_cast<int>(MT::kNpcHits) == 35 && static_cast<int>(MT::kNpcDeaths) == 38);
}

// ---- each game's edge ----

namespace
{
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

		// What the guest reads of a_packet the host sent (the server passes it on as it is).
		std::vector<std::uint8_t> HostToGuest(const std::vector<std::uint8_t>& a_packet) const
		{
			auto       out = FormTranslate::Translate(hostEdge, FormTranslate::Way::kToSession, a_packet);
			const auto wire = out.kind == FormTranslate::Result::Kind::kChanged ? out.packet : a_packet;
			auto       in = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, wire);
			return in.kind == FormTranslate::Result::Kind::kChanged ? in.packet : wire;
		}
	};
}

TEST("npc combat form translate: each NPC is named as each game's own, and one only the sender has is left out")
{
	const Edges            e;
	const Protocol::NpcKey ofB{ 0x03000ABC, 0 };      // B.esp's reference in the host's numbering
	const Protocol::NpcKey ofExtra{ 0x04000ABC, 0 };  // Extra.esp's: the guest lacks it
	const Protocol::NpcKey ofSkyrim{ 0x00013BBF, 0 };
	const Protocol::NpcKey inGuest{ 0x01000ABC, 0 };

	const auto hits = Protocol::DecodeNpcHits(e.HostToGuest(Protocol::Encode(Protocol::NpcHits{
		.hitter = 1, .hits = { { ofB, 4, 0.25f, Protocol::kHitPower }, { ofExtra, 5, 0.5f, 0 }, { ATRONACH_OF_1, 6, 1.0f, Protocol::kHitKilling } } })));
	REQUIRE(hits);
	CHECK(hits->hitter == 1u);
	CHECK(hits->hits == std::vector<Protocol::NpcHit>({ { inGuest, 4, 0.25f, Protocol::kHitPower }, { ATRONACH_OF_1, 6, 1.0f, Protocol::kHitKilling } }));

	const auto answers = Protocol::DecodeNpcHitAnswers(e.HostToGuest(Protocol::Encode(Protocol::NpcHitAnswers{
		.answers = { { ofExtra, 2, 1, Verdict::kApplied, 0.5f }, { ofB, 2, 2, Verdict::kDead, 0.0f }, { ofSkyrim, 2, 3, Verdict::kApplied, 0.25f } } })));
	REQUIRE(answers);
	CHECK(answers->answers == std::vector<Protocol::NpcHitAnswer>({ { inGuest, 2, 2, Verdict::kDead, 0.0f }, { ofSkyrim, 2, 3, Verdict::kApplied, 0.25f } }));

	const auto healths = Protocol::DecodeNpcHealths(e.HostToGuest(Protocol::Encode(Protocol::NpcHealths{ .npcs = { { ofB, 0.5f }, { ofExtra, 0.25f } } })));
	REQUIRE(healths);
	CHECK(healths->npcs == std::vector<Protocol::NpcHealth>({ { inGuest, 0.5f } }));

	const auto deaths = Protocol::DecodeNpcDeaths(
		e.HostToGuest(Protocol::Encode(Protocol::NpcDeaths{ .full = true, .deaths = { { ofExtra, 1, 0 }, { ofB, 1, Protocol::kDeathKillMove }, { ofSkyrim, 0, 0 } } })));
	REQUIRE(deaths);
	CHECK(deaths->full);
	CHECK(deaths->deaths == std::vector<Protocol::NpcDeath>({ { inGuest, 1, Protocol::kDeathKillMove }, { ofSkyrim, 0, 0 } }));
}

// ---- the server's book of health and deaths ----

TEST("npc fates: a hit goes to its NPC's runner; one that can go nowhere is answered: nobody runs it, the hitter does, it is dead")
{
	NpcFates::Book         book;
	const auto             now = NpcFates::Clock::now();
	const Protocol::NpcHit hit{ GUARD, 1, 0.2f, 0 };
	const auto             toRunner = book.RouteHit(2, 1, hit);
	CHECK(toRunner.runner == 1u);
	CHECK(!toRunner.refused);
	CHECK(book.RouteHit(2, 0, hit).refused == Verdict::kNobody);
	CHECK(book.RouteHit(1, 1, hit).refused == Verdict::kYours);
	CHECK(book.Died(1, 1, { GUARD, 2, 0 }, now) == NpcFates::Death::kNew);
	CHECK(book.RouteHit(2, 1, hit).refused == Verdict::kDead);
	CHECK(book.RouteHit(2, 0, hit).refused == Verdict::kDead);  // dead before anything else
}

TEST("npc fates: only a runner's word counts, a death once; a runner that says a dead NPC lives brings it back")
{
	NpcFates::Book book;
	const auto     now = NpcFates::Clock::now();
	CHECK(book.Said(2, 1, { GUARD, 0.5f }, now) == NpcFates::Health::kRefused);
	CHECK(book.Said(0, 0, { GUARD, 0.5f }, now) == NpcFates::Health::kRefused);
	CHECK(book.Healths().empty());
	CHECK(book.Said(1, 1, { GUARD, 0.5f }, now) == NpcFates::Health::kTaken);
	CHECK(book.Said(1, 1, { WOLF, 1.0f }, now) == NpcFates::Health::kTaken);
	CHECK(book.Healths() == std::vector<Protocol::NpcHealth>({ { GUARD, 0.5f }, { WOLF, 1.0f } }));

	CHECK(book.Died(2, 1, { GUARD, 2, 0 }, now) == NpcFates::Death::kRefused);
	CHECK(!book.IsDead(GUARD));
	const Protocol::NpcDeath death{ GUARD, 2, Protocol::kDeathKillMove };
	CHECK(book.Died(1, 1, death, now) == NpcFates::Death::kNew);
	CHECK(book.Died(1, 1, { GUARD, 1, 0 }, now) == NpcFates::Death::kKnown);
	CHECK(book.IsDead(GUARD));
	CHECK(book.Dead() == std::vector<Protocol::NpcDeath>({ death }));
	CHECK(book.Healths() == std::vector<Protocol::NpcHealth>({ { WOLF, 1.0f } }));

	// Raised in its runner's game: alive again, and it may die again.
	CHECK(book.Said(1, 1, { GUARD, 0.3f }, now) == NpcFates::Health::kRevived);
	CHECK(!book.IsDead(GUARD));
	CHECK(book.Dead().empty());
	CHECK(book.Died(1, 1, death, now) == NpcFates::Death::kNew);
}

TEST("npc fates: an NPC let go of keeps no word, a dead one stays dead; a player who leaves takes their spawns with them")
{
	NpcFates::Book book;
	const auto     now = NpcFates::Clock::now();
	CHECK(book.Said(1, 1, { GUARD, 0.5f }, now) == NpcFates::Health::kTaken);
	book.Released(GUARD);
	CHECK(book.Healths().empty());
	CHECK(book.Died(1, 1, { WOLF, 0, 0 }, now) == NpcFates::Death::kNew);
	book.Released(WOLF);
	CHECK(book.IsDead(WOLF));

	CHECK(book.Said(1, 1, { ATRONACH_OF_1, 0.5f }, now) == NpcFates::Health::kTaken);
	CHECK(book.Died(1, 1, { { 0xFF000802, 1 }, 0, 0 }, now) == NpcFates::Death::kNew);
	book.Left(1);
	CHECK(book.Healths().empty());
	CHECK(book.Dead() == std::vector<Protocol::NpcDeath>({ { WOLF, 0, 0 } }));  // a load-order NPC's death stands
	book.Clear();
	CHECK(book.Dead().empty());
}

TEST("npc fates: a death from the game that ran it a moment ago is taken, told to all; the new runner's word of it alive then is dropped")
{
	NpcFates::Book book;
	const auto     now = NpcFates::Clock::now();
	// Ada (1) ran the guard; Bo's (2) fight claim won it. Ada's game says it died there before it heard.
	book.HandedOver(GUARD, 1, now);
	CHECK(book.Died(3, 2, { GUARD, 0, 0 }, now) == NpcFates::Death::kRefused);  // Cy never ran it
	CHECK(book.Died(1, 2, { GUARD, 1, 0 }, now + 1s) == NpcFates::Death::kLate);
	CHECK(book.IsDead(GUARD));
	CHECK(book.Dead() == std::vector<Protocol::NpcDeath>({ { GUARD, 1, 0 } }));
	// Bo's game said it alive before it heard: dropped, it stays dead. Bo's own death of it is known.
	CHECK(book.Said(2, 2, { GUARD, 0.8f }, now + 2s) == NpcFates::Health::kStale);
	CHECK(book.IsDead(GUARD));
	CHECK(book.Died(2, 2, { GUARD, 0, 0 }, now + 2s) == NpcFates::Death::kKnown);
	// Past the grace, Bo's word of it alive is a raise.
	CHECK(book.Said(2, 2, { GUARD, 0.8f }, now + 1s + NpcFates::DEATH_GRACE) == NpcFates::Health::kRevived);
	CHECK(!book.IsDead(GUARD));

	// Ada let the wolf go (nobody runs it): her death of it, a moment later, is taken; long after, it is not.
	book.HandedOver(WOLF, 1, now);
	CHECK(book.Died(1, 0, { WOLF, 0, 0 }, now + NpcFates::DEATH_GRACE + 1ms) == NpcFates::Death::kRefused);
	CHECK(book.Died(1, 0, { WOLF, 0, 0 }, now + NpcFates::DEATH_GRACE) == NpcFates::Death::kLate);
	// The same runner's death and raise are as ever: no grace in its own word.
	CHECK(book.Died(1, 1, { ATRONACH_OF_1, 0, 0 }, now) == NpcFates::Death::kNew);
	CHECK(book.Said(1, 1, { ATRONACH_OF_1, 0.4f }, now) == NpcFates::Health::kRevived);
	// A player who leaves is nobody's last runner.
	book.HandedOver({ 0x00034567, 0 }, 3, now);
	book.Left(3);
	CHECK(book.Died(3, 0, { { 0x00034567, 0 }, 0, 0 }, now) == NpcFates::Death::kRefused);
}

// ---- a copy's health (CopyHealth.h) ----

TEST("copy health: a copy shows its runner's word less our hits not answered yet, never healed for a hit on its way")
{
	Run r;
	CHECK(r.Look({ .share = 1.0f }).what == Do::kNothing);  // no word yet: where it starts
	REQUIRE(r.copy.RunnersWord());
	CHECK(*r.copy.RunnersWord() == 1.0f);

	const auto hit = r.Hit(Protocol::kHitPower);
	const auto sent = r.Look({ .share = 0.7f }, hit);
	CHECK(sent.what == Do::kSend);
	CHECK(sent.hit.key == GUARD);
	CHECK(sent.hit.seq == 1u);
	CHECK(Near(sent.hit.share, 0.3f));
	CHECK(sent.hit.flags == Protocol::kHitPower);
	CHECK(r.copy.InFlight() == 1u);

	// The runner's word from before the hit landed: what the copy shows stands (no heal for a hit on its way), and the
	// same hit's event is no second hit.
	r.copy.Word(1.0f);
	r.t += 50ms;
	CHECK(r.Look({ .share = 0.7f }, hit).what == Do::kNothing);
	CHECK(r.copy.InFlight() == 1u);

	// Applied: the runner's health after it is what the copy shows, at once.
	r.copy.Answer(1, Verdict::kApplied, 0.6f, r.counts);
	CHECK(r.copy.InFlight() == 0u);
	const auto set = r.Look({ .share = 0.7f }, hit);
	CHECK(set.what == Do::kSet);
	CHECK(Near(set.share, 0.6f));
	CHECK(r.Look({ .share = 0.6f }, hit).what == Do::kNothing);
	CHECK(r.counts.sent == 1u);
	CHECK(r.counts.applied == 1u);
	CHECK(r.counts.refused == 0u);
}

TEST("copy health: a runner's answer that differs from what the copy took is set, never sent again as more hits")
{
	Run r;
	r.Look({ .share = 1.0f });
	const auto hit = r.Hit();
	CHECK(r.Look({ .share = 0.7f }, hit).what == Do::kSend);
	r.copy.Answer(1, Verdict::kApplied, 0.75f, r.counts);  // the runner's NPC took less (its own armor's roll)
	r.t += 16ms;
	const auto set = r.Look({ .share = 0.7f }, hit);
	CHECK(set.what == Do::kSet);
	CHECK(Near(set.share, 0.75f));
	CHECK(r.counts.sent == 1u);
	// A new blow is a new hit.
	r.t += 300ms;
	const auto next = r.Look({ .share = 0.6f }, r.Hit());
	CHECK(next.what == Do::kSend);
	CHECK(next.hit.seq == 2u);
	CHECK(Near(next.hit.share, 0.15f));
}

TEST("copy health: a refused hit is given back at once, one never answered after LOST_AFTER, and an unknown answer changes nothing")
{
	Run r;
	r.Look({ .share = 1.0f });
	const auto hit = r.Hit();
	CHECK(r.Look({ .share = 0.8f }, hit).what == Do::kSend);
	r.copy.Answer(1, Verdict::kBusy, 0.0f, r.counts);
	CHECK(r.counts.refused == 1u);
	const auto back = r.Look({ .share = 0.8f }, hit);
	CHECK(back.what == Do::kSet);
	CHECK(Near(back.share, 1.0f));
	CHECK(r.Look({ .share = 1.0f }, hit).what == Do::kNothing);

	// An answer to no hit in flight (given up on, or not ours): nothing.
	r.copy.Answer(7, Verdict::kApplied, 0.1f, r.counts);
	CHECK(r.counts.applied == 0u);
	CHECK(*r.copy.RunnersWord() == 1.0f);

	r.t += 2s;
	CHECK(r.Look({ .share = 0.9f }, r.Hit()).what == Do::kSend);
	r.t += CopyHealth::LOST_AFTER - 1ms;
	CHECK(r.Look({ .share = 0.9f }).what == Do::kNothing);
	r.t += 1ms;
	const auto lost = r.Look({ .share = 0.9f });
	CHECK(lost.what == Do::kSet);
	CHECK(Near(lost.share, 1.0f));
	CHECK(r.counts.lost == 1u);
	CHECK(r.copy.InFlight() == 0u);
}

TEST("copy health: kYours: our game runs the NPC now, and what the copy took stands")
{
	Run r;
	r.Look({ .share = 1.0f });
	CHECK(r.Look({ .share = 0.7f }, r.Hit()).what == Do::kSend);
	r.copy.Answer(1, Verdict::kYours, 0.0f, r.counts);
	CHECK(Near(*r.copy.RunnersWord(), 0.7f));
	CHECK(r.counts.refused == 0u);
	CHECK(r.copy.InFlight() == 0u);
}

TEST("copy health: what no hit of ours explains is set back after JUDGE_AFTER, our hit's late event is ours, the runner's word is set at once")
{
	Run r;
	r.Look({ .share = 1.0f });
	// A trap, a fall or an NPC's blow on the copy: no hit of ours. The runner's game deals those.
	CHECK(r.Look({ .share = 0.8f }).what == Do::kNothing);
	r.t += CopyHealth::JUDGE_AFTER - 1ms;
	CHECK(r.Look({ .share = 0.8f }).what == Do::kNothing);
	r.t += 1ms;
	const auto back = r.Look({ .share = 0.8f });
	CHECK(back.what == Do::kSet);
	CHECK(Near(back.share, 1.0f));
	CHECK(r.counts.givenBack == 1u);

	// Our hit's event a frame after its drop: booked, not given back.
	r.t += 1s;
	CHECK(r.Look({ .share = 0.5f }).what == Do::kNothing);
	r.t += 16ms;
	const auto late = r.Look({ .share = 0.5f }, r.Hit(Protocol::kHitSneak));
	CHECK(late.what == Do::kSend);
	CHECK(Near(late.hit.share, 0.5f));
	CHECK(late.hit.flags == Protocol::kHitSneak);
	r.copy.Answer(late.hit.seq, Verdict::kApplied, 0.5f, r.counts);
	CHECK(r.Look({ .share = 0.5f }).what == Do::kNothing);

	// Its own regeneration: taken back.
	r.t += 1s;
	CHECK(r.Look({ .share = 0.6f }).what == Do::kNothing);
	r.t += CopyHealth::JUDGE_AFTER;
	const auto regen = r.Look({ .share = 0.6f });
	CHECK(regen.what == Do::kSet);
	CHECK(Near(regen.share, 0.5f));
	CHECK(r.counts.takenBack == 1u);

	// A change under EPSILON is left until it adds up.
	CHECK(r.Look({ .share = 0.502f }).what == Do::kNothing);
	r.t += 1s;
	CHECK(r.Look({ .share = 0.502f }).what == Do::kNothing);

	// The runner's word (its NPC healed, or hit by its own NPCs): set at once.
	r.copy.Word(0.9f);
	const auto word = r.Look({ .share = 0.5f });
	CHECK(word.what == Do::kSet);
	CHECK(Near(word.share, 0.9f));
}

TEST("copy health: a spell's hit event explains one drop, as a blow's; a later drop is not ours for it (a follower's axe)")
{
	Run r;
	r.Look({ .share = 1.0f });
	const auto fire = r.Hit(Protocol::kHitSpell);
	const auto first = r.Look({ .share = 0.9f }, fire);
	CHECK(first.what == Do::kSend);
	CHECK(first.hit.flags == Protocol::kHitSpell);
	// The VM's follower's axe, inside the spell's second and after it: never sent as our player's hit, set back.
	r.t += 300ms;
	CHECK(r.Look({ .share = 0.8f }, fire).what == Do::kNothing);
	r.t += CopyHealth::JUDGE_AFTER;
	const auto back = r.Look({ .share = 0.8f }, fire);
	CHECK(back.what == Do::kSet);
	CHECK(Near(back.share, 0.9f));
	CHECK(r.counts.sent == 1u);
	CHECK(r.counts.givenBack == 1u);

	Run b;
	b.Look({ .share = 1.0f });
	const auto blow = b.Hit();
	b.t += CopyHealth::HIT_REACH + 1ms;
	CHECK(b.Look({ .share = 0.9f }, blow).what == Do::kNothing);
	CHECK(b.counts.sent == 0u);
}

namespace
{
	// A copy looked at every 20 ms for a_for, our player's lasting effects on it taking a_drain a second and a_alsoOnce
	// more on the second look (another's blow): what was sent as ours, and the copy as the game would show it after.
	struct Ticks
	{
		float sent = 0;
		float shown = 1;
		bool  allSpell = true;
	};
}
static Ticks Tick(Run& a_r, float a_drain, std::chrono::milliseconds a_for, float a_alsoOnce = 0)
{
	Ticks out;
	a_r.Look({ .share = 1.0f, .ourDrain = a_drain });
	for (auto at = 20ms, i = 0ms; at <= a_for; at += 20ms, i += 20ms) {
		a_r.t += 20ms;
		out.shown -= a_drain * 0.02f + (i == 20ms ? a_alsoOnce : 0.0f);
		const auto step = a_r.Look({ .share = out.shown, .ourDrain = a_drain });
		if (step.what == Do::kSend) {
			out.sent += step.hit.share;
			out.allSpell = out.allSpell && step.hit.flags == Protocol::kHitSpell;
		} else if (step.what == Do::kSet) {
			out.shown = step.share;
		}
	}
	return out;
}

TEST("copy health: our player's lasting effects on a copy are ours as they tick, their share alone; another's blow then is set back")
{
	// A burn of 2 % a second for two seconds: about 4 % sent, nothing set back.
	Run burn;
	const auto alone = Tick(burn, 0.02f, 2000ms);
	CHECK(alone.sent > 0.04f - 2 * CopyHealth::EPSILON);
	CHECK(alone.sent <= 0.0401f);
	CHECK(alone.allSpell);
	CHECK(burn.counts.givenBack == 0u);
	CHECK(burn.counts.takenBack == 0u);

	// The same burn while a follower's axe takes 20 %: the axe is never sent as ours (at most the burn's own share and its
	// margin), and it is set back once it stood.
	Run axe;
	const auto mixed = Tick(axe, 0.02f, 2000ms, 0.2f);
	CHECK(mixed.sent <= 0.02f * 2 * CopyHealth::DRAIN_MARGIN + CopyHealth::EPSILON);
	CHECK(mixed.allSpell);
	CHECK(axe.counts.givenBack >= 1u);
	CHECK(mixed.shown > 0.9f);  // the axe's drop given back: only the burn's ticks stand (sent, in flight)

	// No effect of ours on it (a follower's burn, a trap's poison): nothing is ours, every drop is set back.
	Run theirs;
	const auto none = Tick(theirs, 0.0f, 500ms, 0.1f);
	CHECK(none.sent == 0.0f);
	CHECK(theirs.counts.givenBack >= 1u);
}

TEST("copy health: a copy our player killed sends its killing blow once, as all it had left, a kill move said so")
{
	Run r;
	r.Look({ .share = 1.0f });
	CHECK(r.Look({ .share = 0.4f }, r.Hit()).what == Do::kSend);
	r.copy.Answer(1, Verdict::kApplied, 0.4f, r.counts);
	CHECK(r.Look({ .share = 0.4f }).what == Do::kNothing);
	r.t += 2s;
	const auto blow = r.Hit(Protocol::kHitPower);
	const auto kill = r.Look({ .share = 0.0f, .dead = true }, blow);
	CHECK(kill.what == Do::kSend);
	CHECK(Near(kill.hit.share, 0.4f));
	CHECK(kill.hit.flags == (Protocol::kHitPower | Protocol::kHitKilling));
	r.t += 16ms;
	CHECK(r.Look({ .share = 0.0f, .dead = true }, blow).what == Do::kNothing);
	// Its runner's word of its death: dead here already.
	r.copy.Died();
	r.t += 10s;
	CHECK(r.Look({ .share = 0.0f, .dead = true }).what == Do::kNothing);
	CHECK(r.counts.kills == 0u);

	Run m;
	m.Look({ .share = 1.0f });
	const auto move = m.Look({ .share = 0.0f, .dead = true, .inKillMove = true }, m.Hit(Protocol::kHitKillMove));
	CHECK(move.what == Do::kSend);
	CHECK(Near(move.hit.share, 1.0f));
	CHECK(move.hit.flags == (Protocol::kHitKilling | Protocol::kHitKillMove));
}

TEST("copy health: a copy dies here only on its runner's word, or in our player's kill move with it")
{
	CHECK(CopyHealth::DiesHere(true, false));
	CHECK(CopyHealth::DiesHere(false, true));
	CHECK(!CopyHealth::DiesHere(false, false));
}

TEST("copy health: our player's blow that would have killed a copy is held: a killing blow goes, and it dies on its runner's word")
{
	Run r;
	const float floor = 0.01f;
	r.Look({ .share = 1.0f, .floor = floor });
	r.copy.Word(0.4f);
	r.Look({ .share = 0.4f, .floor = floor });
	// Our blow took it to nothing; it was held at one point, alive.
	const auto blow = r.Hit(Protocol::kHitPower);
	const auto kill = r.Look({ .share = floor, .floor = floor, .heldByUs = true }, blow);
	CHECK(kill.what == Do::kSend);
	CHECK(Near(kill.hit.share, 0.4f));
	CHECK(kill.hit.flags == (Protocol::kHitPower | Protocol::kHitKilling));
	CHECK(r.counts.heldKills == 1u);
	// Held at its floor while the blow is on its way: no second blow (another held one of ours, a frame later), no set back.
	r.t += 16ms;
	CHECK(r.Look({ .share = floor, .floor = floor, .heldByUs = true }, blow).what == Do::kNothing);
	r.t += CopyHealth::JUDGE_AFTER;
	CHECK(r.Look({ .share = floor, .floor = floor }).what == Do::kNothing);
	CHECK(r.counts.sent == 1u);
	// Applied: its runner's NPC died. The death word kills it here, never before.
	r.copy.Answer(kill.hit.seq, Verdict::kApplied, 0.0f, r.counts);
	r.copy.Died();
	CHECK(r.Look({ .share = floor, .floor = floor }).what == Do::kKill);
	CHECK(r.counts.kills == 1u);
	CHECK(r.counts.revived == 0u);
}

TEST("copy health: a held killing blow refused is given back, and the next held blow of ours is sent again; another's held blow is set back")
{
	Run r;
	const float floor = 0.01f;
	r.Look({ .share = 1.0f, .floor = floor });
	r.copy.Word(0.5f);
	r.Look({ .share = 0.5f, .floor = floor });
	const auto kill = r.Look({ .share = floor, .floor = floor, .heldByUs = true }, r.Hit());
	CHECK(kill.what == Do::kSend);
	r.copy.Answer(kill.hit.seq, Verdict::kNotHere, 0.0f, r.counts);
	const auto back = r.Look({ .share = floor, .floor = floor });
	CHECK(back.what == Do::kSet);
	CHECK(Near(back.share, 0.5f));
	r.t += 2s;
	const auto again = r.Look({ .share = floor, .floor = floor, .heldByUs = true }, r.Hit());
	CHECK(again.what == Do::kSend);
	CHECK((again.hit.flags & Protocol::kHitKilling) != 0);
	CHECK(r.counts.heldKills == 2u);
	// Never answered: given back after LOST_AFTER.
	r.t += CopyHealth::LOST_AFTER;
	CHECK(r.Look({ .share = floor, .floor = floor }).what == Do::kSet);

	// A guard's blow (an NPC this game runs) held it at one point: not ours, so set back once it has stood; nothing sent.
	Run g;
	g.Look({ .share = 1.0f, .floor = floor });
	g.copy.Word(0.7f);
	g.Look({ .share = 0.7f, .floor = floor });
	CHECK(g.Look({ .share = floor, .floor = floor }).what == Do::kNothing);
	g.t += CopyHealth::JUDGE_AFTER;
	const auto setBack = g.Look({ .share = floor, .floor = floor });
	CHECK(setBack.what == Do::kSet);
	CHECK(Near(setBack.share, 0.7f));
	CHECK(g.counts.sent == 0u);
}

TEST("copy health: a script's held kill of a copy goes to its runner as a death, essential or not, our player its killer only if named")
{
	Run r;
	const float floor = 0.01f;
	r.Look({ .share = 1.0f, .floor = floor });
	r.copy.Word(0.8f);
	r.Look({ .share = 0.8f, .floor = floor });
	// A quest's Kill() on it here: held, alive at what it had.
	const auto kill = r.Look({ .share = 0.8f, .floor = floor, .heldScript = true });
	CHECK(kill.what == Do::kSend);
	CHECK(Near(kill.hit.share, 0.8f));
	CHECK(kill.hit.flags == Protocol::kHitScript);  // no killer named: nobody's kill
	CHECK(r.counts.scriptKills == 1u);
	CHECK(r.counts.heldKills == 0u);
	// On its way: not sent again.
	r.t += 16ms;
	CHECK(r.Look({ .share = 0.8f, .floor = floor, .heldScript = true }).what == Do::kNothing);
	// Applied: the runner's death kills it here.
	r.copy.Answer(kill.hit.seq, Verdict::kApplied, 0.0f, r.counts);
	r.copy.Died();
	CHECK(r.Look({ .share = 0.8f, .floor = floor }).what == Do::kKill);

	// Its runner's NPC is down at none (an essential one): a KillEssential still goes, at least its floor, and kills it.
	Run e;
	e.Look({ .share = 1.0f, .floor = floor });
	e.copy.Word(0.0f);
	e.Look({ .share = floor, .floor = floor });
	const auto down = e.Look({ .share = floor, .floor = floor, .heldByUs = true, .heldScript = true });
	CHECK(down.what == Do::kSend);
	CHECK(Near(down.hit.share, floor));
	CHECK(down.hit.flags == (Protocol::kHitScript | Protocol::kHitKilling));  // Kill(player): our player its killer
}

TEST("copy health: a held kill no blow explains is owed until the NPC dies: sent again, waits for the word, put on here if this game runs it")
{
	using Held = CopyHealth::HeldKills;
	const Steady::time_point t0{ 100s };
	Held held;
	// Held before any word of its runner: waits for it.
	held.Script(0x1234, false);
	CHECK(!held.Look(0x1234, false, t0).script);
	CHECK(held.Owes(0x1234));
	const auto first = held.Look(0x1234, true, t0);
	CHECK(first.script);
	CHECK(!first.blow);
	CHECK(!first.byUs);
	// Due again only SCRIPT_RESEND later (a runner that could not apply it: not loaded there, handed on).
	CHECK(!held.Look(0x1234, true, t0 + 100ms).script);
	CHECK(held.Look(0x1234, true, t0 + Held::SCRIPT_RESEND).script);
	// Never forgotten while it lives, however long.
	held.Forget(t0 + 1h);
	CHECK(held.Owes(0x1234));
	// This game comes to run it: put on here, nobody its killer; then owed no more.
	CHECK(held.Run(0x1234) == std::optional<bool>{ false });
	CHECK(!held.Owes(0x1234));
	CHECK(!held.Run(0x1234));
	// A Kill(player) on another: our player its killer; it died: owed no more.
	held.Script(0x5678, true);
	CHECK(held.Look(0x5678, true, t0).byUs);
	held.Dead(0x5678);
	CHECK(!held.Owes(0x5678));
	CHECK(held.Size() == 0u);
}

TEST("copy health: our player's held blow is owed only a moment: one never looked at in time is never sent later")
{
	using Held = CopyHealth::HeldKills;
	const Steady::time_point t0{ 100s };
	Held held;
	held.Blow(0x1234, t0);
	const auto due = held.Look(0x1234, true, t0 + 16ms);
	CHECK(due.blow);
	CHECK(due.byUs);
	CHECK(!due.script);
	CHECK(!held.Owes(0x1234));  // taken
	// A copy that unloaded (or that this game came to run) before a look: the blow lapses.
	held.Blow(0x2345, t0);
	held.Forget(t0 + Held::OUR_BLOW_FRESH);
	CHECK(!held.Owes(0x2345));
	held.Blow(0x3456, t0);
	CHECK(!held.Look(0x3456, true, t0 + Held::OUR_BLOW_FRESH).blow);
	// Held before the word: waits for it, a moment only.
	held.Blow(0x4567, t0);
	CHECK(!held.Look(0x4567, false, t0 + 100ms).blow);
	CHECK(held.Look(0x4567, true, t0 + 200ms).blow);
	// A blow is never put on here when this game comes to run it (only a script's kill is).
	held.Blow(0x5678, t0);
	CHECK(!held.Run(0x5678));
	CHECK(held.Size() == 0u);
}

TEST("copy health: our player's kill counts however late its hit event; a kill by anyone else is not ours, and is raised")
{
	// Our player's arrow killed it, its hit event long gone (or never seen): its killer says it is ours.
	Run late;
	late.Look({ .share = 1.0f });
	late.t += 5s;
	const auto kill = late.Look({ .share = 0.0f, .dead = true, .killedByUs = true });
	CHECK(kill.what == Do::kSend);
	CHECK(Near(kill.hit.share, 1.0f));
	CHECK(kill.hit.flags == Protocol::kHitKilling);
	CHECK(late.counts.saveKills == 0u);
	late.t += CopyHealth::REVIVE_AFTER;
	CHECK(late.Look({ .share = 0.0f, .dead = true, .killedByUs = true }).what == Do::kNothing);  // its killing blow is on its way

	// Killed in a kill move of our player's, paired: said so.
	Run move;
	move.Look({ .share = 1.0f });
	const auto moved = move.Look({ .share = 0.0f, .dead = true, .inKillMove = true, .killedByUs = true });
	CHECK(moved.what == Do::kSend);
	CHECK(moved.hit.flags == (Protocol::kHitKilling | Protocol::kHitKillMove));

	// Killed by a follower (its own kill move too): nothing sent, raised after REVIVE_AFTER.
	Run other;
	other.Look({ .share = 1.0f });
	CHECK(other.Look({ .share = 0.0f, .dead = true, .inKillMove = true }).what == Do::kNothing);
	other.t += CopyHealth::REVIVE_AFTER;
	CHECK(other.Look({ .share = 0.0f, .dead = true }).what == Do::kRevive);
	CHECK(other.counts.sent == 0u);
}

TEST("copy health: its runner's death kills a copy, after any kill move it is in, and again if the game did not take it")
{
	Run r;
	r.Look({ .share = 1.0f });
	r.copy.Died();
	CHECK(r.copy.RunnerDead());
	CHECK(r.Look({ .share = 0.5f, .inKillMove = true }).what == Do::kNothing);
	CHECK(r.Look({ .share = 0.5f }).what == Do::kKill);
	CHECK(r.counts.kills == 1u);
	r.t += CopyHealth::KILL_AGAIN - 1ms;
	CHECK(r.Look({ .share = 0.5f }).what == Do::kNothing);
	r.t += 1ms;
	CHECK(r.Look({ .share = 0.5f }).what == Do::kKill);
	CHECK(r.Look({ .share = 0.0f, .dead = true }).what == Do::kNothing);
	CHECK(r.counts.kills == 2u);
	// Its runner's word that it lives (raised there): alive again here.
	r.copy.Word(0.5f);
	CHECK(!r.copy.RunnerDead());
}

TEST("copy health: a flyer shot down in its runner's game dies here once its body lies still, not in the air")
{
	Run r;
	r.Look({ .share = 1.0f });
	r.copy.Died();
	// Its runner's body still falls and slides (NpcRules::Crash): the copy follows it, alive.
	CHECK(r.Look({ .share = 0.5f, .crashing = true }).what == Do::kNothing);
	r.t += 5s;
	CHECK(r.Look({ .share = 0.5f, .crashing = true }).what == Do::kNothing);
	CHECK(r.counts.kills == 0u);
	// It lies still: killed there.
	CHECK(r.Look({ .share = 0.5f }).what == Do::kKill);
	CHECK(r.counts.kills == 1u);
}

TEST("copy health: a copy dead here while its runner's NPC lives is raised after REVIVE_AFTER, unless it may not be or is down at none")
{
	Run r;
	r.Look({ .share = 1.0f });
	r.copy.Word(0.6f);
	CHECK(r.Look({ .share = 0.0f, .dead = true }).what == Do::kNothing);  // a fall, a trap: not ours
	r.t += CopyHealth::REVIVE_AFTER - 1ms;
	CHECK(r.Look({ .share = 0.0f, .dead = true }).what == Do::kNothing);
	r.t += 1ms;
	const auto raised = r.Look({ .share = 0.0f, .dead = true });
	CHECK(raised.what == Do::kRevive);
	CHECK(Near(raised.share, 0.6f));
	CHECK(r.counts.revived == 1u);
	CHECK(r.Look({ .share = 0.6f }).what == Do::kNothing);

	Run no;
	no.Look({ .share = 1.0f });
	no.copy.Word(0.6f);
	no.Look({ .share = 0.0f, .dead = true }, std::nullopt, false);
	no.t += 10s;
	CHECK(no.Look({ .share = 0.0f, .dead = true }, std::nullopt, false).what == Do::kNothing);

	Run down;
	down.Look({ .share = 1.0f });
	down.copy.Word(0.001f);  // an essential NPC down in its runner's game
	down.Look({ .share = 0.0f, .dead = true });
	down.t += 10s;
	CHECK(down.Look({ .share = 0.0f, .dead = true }).what == Do::kNothing);
}

TEST("copy health: in the host's game a body of its save never alive this session is killed in its runner's game, not raised")
{
	Run h;  // never seen alive here
	h.copy.Word(0.8f);
	const auto blow = h.Look({ .share = 0.0f, .dead = true }, std::nullopt, false, true);
	CHECK(blow.what == Do::kSend);
	CHECK(Near(blow.hit.share, 0.8f));
	CHECK(blow.hit.flags == Protocol::kHitKilling);
	CHECK(h.counts.saveKills == 1u);
	h.t += 10s;
	CHECK(h.Look({ .share = 0.0f, .dead = true }, std::nullopt, false, true).what == Do::kNothing);

	// No word of it yet: nothing to send.
	Run n;
	CHECK(n.Look({ .share = 0.0f, .dead = true }, std::nullopt, false, true).what == Do::kNothing);
	CHECK(n.counts.sent == 0u);

	// One seen alive this session is no body of the save: raised as anywhere.
	Run g;
	g.Look({ .share = 1.0f }, std::nullopt, true, true);
	g.copy.Word(0.5f);
	CHECK(g.Look({ .share = 0.0f, .dead = true }, std::nullopt, true, true).what == Do::kNothing);
	g.t += CopyHealth::REVIVE_AFTER;
	CHECK(g.Look({ .share = 0.0f, .dead = true }, std::nullopt, true, true).what == Do::kRevive);
}

TEST("copy health: a living copy is never set below one point, so a runner's NPC down at none is set once, not every frame")
{
	Run r;
	r.Look({ .share = 1.0f });
	r.copy.Word(0.0f);
	const auto set = r.Look({ .share = 1.0f, .floor = 0.01f });
	CHECK(set.what == Do::kSet);
	CHECK(Near(set.share, 0.01f));
	CHECK(r.Look({ .share = 0.01f, .floor = 0.01f }).what == Do::kNothing);
	r.t += 1s;
	CHECK(r.Look({ .share = 0.01f, .floor = 0.01f }).what == Do::kNothing);
}

TEST("copy health: the runner says an NPC's health first, after a friend's hit at once, else on a change at most every REPORT_EVERY")
{
	CopyHealth::Reporter rep;
	const auto           t = Steady::now();
	CHECK(rep.Due(1.0f, false, t));
	rep.Sent(1.0f, t);
	CHECK(!rep.Due(1.0f, false, t + 1s));
	CHECK(!rep.Due(0.998f, false, t + 1s));
	CHECK(!rep.Due(0.9f, false, t + CopyHealth::REPORT_EVERY - 1ms));
	CHECK(rep.Due(0.9f, false, t + CopyHealth::REPORT_EVERY));
	CHECK(rep.Due(1.0f, true, t + 1ms));
	rep.Reset();
	CHECK(rep.Due(1.0f, false, t));
}

TEST("copy health: a hit takes the same share of the most in every game; changes and shares never pass their bounds")
{
	CHECK(CopyHealth::DamageOf(0.25f, 200.0f) == 50.0f);
	CHECK(CopyHealth::DamageOf(2.0f, 100.0f) == 100.0f);
	CHECK(CopyHealth::DamageOf(std::nanf(""), 200.0f) == 0.0f);
	CHECK(CopyHealth::DamageOf(0.5f, 0.0f) == 0.0f);
	CHECK(CopyHealth::ChangeTo(0.5f, 200.0f, 150.0f) == -50.0f);
	CHECK(CopyHealth::ChangeTo(1.0f, 200.0f, 150.0f) == 50.0f);
	CHECK(CopyHealth::ChangeTo(0.5f, std::nanf(""), 150.0f) == 0.0f);
	CHECK(CopyHealth::ShareOf(50.0f, 200.0f) == 0.25f);
	CHECK(CopyHealth::ShareOf(300.0f, 200.0f) == 1.0f);
	CHECK(CopyHealth::ShareOf(-5.0f, 200.0f) == 0.0f);
	CHECK(CopyHealth::ShareOf(5.0f, 0.0f) == 0.0f);
	// The host's game keeps its own save's dead; every other game kills what the session has dead.
	static_assert(!CopyHealth::KillsOnLoad(true, true));
	static_assert(CopyHealth::KillsOnLoad(true, false));
	static_assert(CopyHealth::KillsOnLoad(false, true));
	static_assert(CopyHealth::KillsOnLoad(false, false));
}

// ---- the server ----

TEST("npc combat server: a hit on a copy goes to its runner alone, the hitter named; the answer to the hitter alone")
{
	Fixture f;
	AllIn(f, 3);
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	Forget(f, { 1, 2, 3 });

	const Protocol::NpcHit hit{ GUARD, 1, 0.25f, Protocol::kHitPower };
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcHits{ .hitter = 99, .hits = { hit } }));
	REQUIRE(f.fake->Settle());
	const auto toRunner = SentOf<Protocol::NpcHits>(f, 1, MT::kNpcHits, Protocol::DecodeNpcHits);
	REQUIRE(toRunner.size() == 1u);
	CHECK(toRunner[0].second);
	CHECK(toRunner[0].first.hitter == 2u);
	CHECK(toRunner[0].first.hits == std::vector<Protocol::NpcHit>({ hit }));
	CHECK(f.fake->Take(2).empty());
	CHECK(f.fake->Take(3).empty());

	// The runner's answers: each to its hitter alone; one naming the runner itself goes nowhere.
	const Protocol::NpcHitAnswer applied{ GUARD, 2, 1, Verdict::kApplied, 0.75f };
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcHitAnswers{ .answers = { applied, { GUARD, 1, 5, Verdict::kApplied, 0.5f }, { GUARD, 3, 9, Verdict::kDead, 0.0f } } }));
	REQUIRE(f.fake->Settle());
	const auto toBo = SentOf<Protocol::NpcHitAnswers>(f, 2, MT::kNpcHitAnswers, Protocol::DecodeNpcHitAnswers);
	REQUIRE(toBo.size() == 1u);
	CHECK(toBo[0].second);
	CHECK(toBo[0].first.answers == std::vector<Protocol::NpcHitAnswer>({ applied }));
	const auto toCy = SentOf<Protocol::NpcHitAnswers>(f, 3, MT::kNpcHitAnswers, Protocol::DecodeNpcHitAnswers);
	REQUIRE(toCy.size() == 1u);
	CHECK(toCy[0].first.answers == std::vector<Protocol::NpcHitAnswer>({ { GUARD, 3, 9, Verdict::kDead, 0.0f } }));
	CHECK(f.fake->Take(1).empty());
}

TEST("npc combat server: a hit that can go nowhere is answered by the server: nobody runs it, the hitter does, it is dead, past the budget")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	f.fake->Deliver(2, Protocol::Encode(Claims({ { WOLF, Why::kHave } })));
	Forget(f, { 1, 2 });

	// Ada hits an NPC nobody runs and her own guard.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcHits{ .hits = { { ATRONACH_OF_1, 1, 0.1f, 0 }, { GUARD, 2, 0.1f, 0 } } }));
	REQUIRE(f.fake->Settle());
	const auto toAda = SentOf<Protocol::NpcHitAnswers>(f, 1, MT::kNpcHitAnswers, Protocol::DecodeNpcHitAnswers);
	REQUIRE(toAda.size() == 1u);
	CHECK(toAda[0].first.answers ==
		  std::vector<Protocol::NpcHitAnswer>({ { ATRONACH_OF_1, 1, 1, Verdict::kNobody, 0.0f }, { GUARD, 1, 2, Verdict::kYours, 0.0f } }));
	CHECK(f.fake->Take(2).empty());

	// Ada says her guard died: Bo's hit on it is answered dead, and goes nowhere.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { GUARD, 2, 0 } } }));
	Forget(f, { 1, 2 });
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcHits{ .hits = { { GUARD, 3, 0.1f, 0 } } }));
	REQUIRE(f.fake->Settle());
	const auto dead = SentOf<Protocol::NpcHitAnswers>(f, 2, MT::kNpcHitAnswers, Protocol::DecodeNpcHitAnswers);
	REQUIRE(dead.size() == 1u);
	CHECK(dead[0].first.answers == std::vector<Protocol::NpcHitAnswer>({ { GUARD, 2, 3, Verdict::kDead, 0.0f } }));
	CHECK(f.fake->Take(1).empty());

	// A flurry on Bo's wolf past Ada's budget: what is past it is answered busy, never dropped.
	for (std::uint32_t round = 0; round < 2; ++round) {
		Protocol::NpcHits many;
		for (std::uint32_t i = 0; i < Protocol::MAX_NPC_HITS; ++i) {
			many.hits.push_back({ WOLF, 100 + round * 100 + i, 0.01f, 0 });
		}
		f.fake->Deliver(1, Protocol::Encode(many));
	}
	REQUIRE(f.fake->Settle());
	std::size_t forwarded = 0;
	for (const auto& sent : SentOf<Protocol::NpcHits>(f, 2, MT::kNpcHits, Protocol::DecodeNpcHits)) {
		CHECK(sent.first.hitter == 1u);
		forwarded += sent.first.hits.size();
	}
	std::size_t busy = 0;
	for (const auto& sent : SentOf<Protocol::NpcHitAnswers>(f, 1, MT::kNpcHitAnswers, Protocol::DecodeNpcHitAnswers)) {
		busy += static_cast<std::size_t>(std::ranges::count_if(sent.first.answers, [](const Protocol::NpcHitAnswer& a_a) { return a_a.verdict == Verdict::kBusy; }));
	}
	CHECK(forwarded >= Protocol::MAX_NPC_HITS);
	CHECK(busy >= Protocol::MAX_NPC_HITS / 2);
	CHECK(forwarded + busy == 2 * Protocol::MAX_NPC_HITS);
}

TEST("npc combat server: only a runner's word of health and deaths is passed on, a death once; a joiner hears both")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave }, { WOLF, Why::kHave } })));
	Forget(f, { 1, 2 });

	// Bo's word on Ada's NPCs is nobody's.
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcHealths{ .npcs = { { GUARD, 0.5f } } }));
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { GUARD, 2, 0 } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.fake->Take(2).empty());

	// Ada's: to Bo, named as hers; the one she does not run is left out.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcHealths{ .runner = 77, .npcs = { { GUARD, 0.5f }, { WOLF, 1.0f }, { ATRONACH_OF_1, 0.3f } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	const auto healths = SentOf<Protocol::NpcHealths>(f, 2, MT::kNpcHealths, Protocol::DecodeNpcHealths);
	REQUIRE(healths.size() == 1u);
	CHECK(healths[0].second);
	CHECK(healths[0].first.runner == 1u);
	CHECK(healths[0].first.npcs == std::vector<Protocol::NpcHealth>({ { GUARD, 0.5f }, { WOLF, 1.0f } }));

	const Protocol::NpcDeath wolf{ WOLF, 2, Protocol::kDeathKillMove };
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { wolf } }));
	REQUIRE(f.fake->Settle());
	const auto deaths = SentOf<Protocol::NpcDeaths>(f, 2, MT::kNpcDeaths, Protocol::DecodeNpcDeaths);
	REQUIRE(deaths.size() == 1u);
	CHECK(deaths[0].first.runner == 1u);
	CHECK(!deaths[0].first.full);
	CHECK(deaths[0].first.deaths == std::vector<Protocol::NpcDeath>({ wolf }));
	// Said again, or a full list from a game: dropped.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { wolf } }));
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .full = true, .deaths = { { GUARD, 0, 0 } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(2).empty());

	// Cy joins: the guard's health and the wolf's death.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	std::optional<Protocol::NpcHealths> joinHealths;
	std::optional<Protocol::NpcDeaths>  joinDeaths;
	for (const auto& packet : f.fake->TakeSent(3)) {
		if (Protocol::PeekType(packet.data) == MT::kNpcHealths) {
			joinHealths = Protocol::DecodeNpcHealths(packet.data);
		} else if (Protocol::PeekType(packet.data) == MT::kNpcDeaths) {
			joinDeaths = Protocol::DecodeNpcDeaths(packet.data);
		}
	}
	REQUIRE(joinHealths);
	CHECK(joinHealths->npcs == std::vector<Protocol::NpcHealth>({ { GUARD, 0.5f } }));
	REQUIRE(joinDeaths);
	CHECK(joinDeaths->full);
	CHECK(joinDeaths->deaths == std::vector<Protocol::NpcDeath>({ wolf }));
	Forget(f, { 1, 2, 3 });

	// Ada says the wolf lives (raised): everyone hears it, and the session has it dead no more.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcHealths{ .npcs = { { WOLF, 0.2f } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.LoggedSoon("server: player 1 says 1 NPC(s) the session had dead live again"));
	for (const PeerId peer : { 2u, 3u }) {
		const auto told = SentOf<Protocol::NpcHealths>(f, peer, MT::kNpcHealths, Protocol::DecodeNpcHealths);
		REQUIRE(told.size() == 1u);
		CHECK(told[0].first.npcs == std::vector<Protocol::NpcHealth>({ { WOLF, 0.2f } }));
	}

	// Ada leaves: her NPCs' health goes with them. Di, joining after, hears no health and no death.
	f.fake->Drop(1);
	Forget(f, { 2, 3 });
	REQUIRE(f.Join(4, Fixture::MakeHello("Di")));
	REQUIRE(f.fake->Settle());
	for (const auto& packet : f.fake->TakeSent(4)) {
		CHECK(Protocol::PeekType(packet.data) != MT::kNpcHealths);
		CHECK(Protocol::PeekType(packet.data) != MT::kNpcDeaths);
	}
}

TEST("npc combat server: a death from the game that let go of its NPC a moment ago reaches every game, that one too; the next runner's word of it alive then is dropped")
{
	Fixture f;
	AllIn(f, 2);
	// Ada ran the guard and let it go; Bo's game has it now. Ada's game says it died there before it heard.
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kRelease } })));
	f.fake->Deliver(2, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	Forget(f, { 1, 2 });
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { GUARD, 1, 0 } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.LoggedSoon("server: player 1 says 1 NPC(s) it ran a moment ago died there: everyone is told"));
	for (const PeerId peer : { 1u, 2u }) {
		const auto told = SentOf<Protocol::NpcDeaths>(f, peer, MT::kNpcDeaths, Protocol::DecodeNpcDeaths);
		REQUIRE(told.size() == 1u);
		CHECK(told[0].second);
		CHECK(told[0].first.runner == 0u);
		CHECK(!told[0].first.full);
		CHECK(told[0].first.deaths == std::vector<Protocol::NpcDeath>({ { GUARD, 1, 0 } }));
	}

	// Bo's game said it alive before it heard: nobody hears that. Its own death of it, after, is known: nobody hears it again.
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcHealths{ .npcs = { { GUARD, 0.9f } } }));
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { GUARD, 0, 0 } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());

	// A game that never ran an NPC is refused as ever.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { WOLF, 1, 0 } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(2).empty());
}
