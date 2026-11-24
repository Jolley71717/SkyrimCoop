#include "Test.h"

#include "FormTranslate.h"
#include "MadeBook.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/MadeRules.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

// Run-time NPCs (docs/DESIGN.md, "Run-time NPCs"): what made an NPC a game made as it ran, on the wire; the server's
// book of them (common/MadeBook.h: shared, twin of another game's, merges); and what a game does with the answers and
// with a friend's made NPC its maker lets go of (src/game/MadeRules.h).

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Steady = std::chrono::steady_clock;
	using Protocol::MadeSource;
	using Protocol::MadeVerdict;
	using Protocol::NpcKey;
	using Protocol::NpcMade;

	constexpr std::uint32_t TAMRIEL = 0x0000003C;
	constexpr std::uint32_t DRAGON_RACE = 0x00012E82;
	constexpr std::uint32_t COURIER_QUEST = 0x00039F82;
	constexpr std::uint32_t LVL_BANDIT = 0x0003DF08;
	constexpr std::uint32_t BANDIT_ROLL = 0x00037C00;

	// A thing a script placed outside, at (x, 0, 0).
	MadeSource Placed(std::uint32_t a_made, std::uint32_t a_body, float a_x)
	{
		MadeSource s;
		s.made = a_made;
		s.pick = a_made;
		s.body = a_body;
		s.worldspace = TAMRIEL;
		s.x = a_x;
		return s;
	}

	// A quest's created alias's NPC, at (x, 0, 0).
	MadeSource ByAlias(std::uint32_t a_quest, std::uint32_t a_alias, float a_x)
	{
		MadeSource s = Placed(0x000E5F1C, Protocol::BODY_PERSON, a_x);
		s.flags = static_cast<std::uint8_t>(Protocol::kMadeByAlias);
		s.quest = a_quest;
		s.alias = a_alias;
		return s;
	}

	NpcMade Made(std::uint32_t a_id, std::uint32_t a_spawner, MadeSource a_source)
	{
		return NpcMade{ NpcKey{ a_id, a_spawner }, a_source };
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

	struct Heard
	{
		std::vector<Protocol::NpcMades>       mades;
		std::vector<Protocol::NpcMadeAnswers> answers;
		std::vector<Protocol::NpcOwners>      owners;
		std::vector<MT>                       order;  // the types in the order sent
	};

	Heard HeardBy(Fixture& a_f, PeerId a_peer)
	{
		Heard out;
		for (auto& sent : a_f.fake->TakeSent(a_peer)) {
			const auto peeked = Protocol::PeekType(sent.data);
			if (!peeked) {
				continue;
			}
			const auto type = *peeked;
			if (type == MT::kNpcMades) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodeNpcMades(sent.data)) {
					out.mades.push_back(*msg);
					out.order.push_back(type);
				}
			} else if (type == MT::kNpcMadeAnswers) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodeNpcMadeAnswers(sent.data)) {
					out.answers.push_back(*msg);
					out.order.push_back(type);
				}
			} else if (type == MT::kNpcOwners) {
				if (auto msg = Protocol::DecodeNpcOwners(sent.data)) {
					out.owners.push_back(*msg);
					out.order.push_back(type);
				}
			}
		}
		return out;
	}

	Protocol::NpcStates StateOf(NpcKey a_key, float a_x)
	{
		Protocol::PlayerState state;
		state.sequence = 1;
		state.worldspace = TAMRIEL;
		state.x = a_x;
		return Protocol::NpcStates{ .npcs = { { a_key, 0x000E5F1C, state } } };
	}
}

// ---- the messages ----

TEST("made protocol: made NPCs and answers read back as they were written, as messages 89 and 90 of protocol 34; only the first carry forms")
{
	static_assert(Protocol::VERSION >= 34);
	static_assert(static_cast<std::uint8_t>(MT::kNpcMades) == 89);
	static_assert(static_cast<std::uint8_t>(MT::kNpcMadeAnswers) == 90);
	MadeSource gone = Placed(LVL_BANDIT, Protocol::BODY_PERSON, -5.0f);
	gone.flags = static_cast<std::uint8_t>(Protocol::kMadeGone);
	const Protocol::NpcMades mades{ .sender = 2, .full = true,
		.mades = { Made(0xFF000801, 2, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 100.0f)), Made(0xFF000802, 2, ByAlias(COURIER_QUEST, 3, 50.0f)), Made(0xFF000803, 2, gone) } };
	CHECK(Protocol::DecodeNpcMades(Protocol::Encode(mades)) == mades);
	CHECK(Protocol::PeekType(Protocol::Encode(mades)) == MT::kNpcMades);
	CHECK(Protocol::DecodeNpcMades(Protocol::Encode(Protocol::NpcMades{})) == Protocol::NpcMades{});

	const Protocol::NpcMadeAnswers answers{ .answers = { { NpcKey{ 0xFF000801, 2 }, MadeVerdict::kShared, {} },
		{ NpcKey{ 0xFF000802, 2 }, MadeVerdict::kTwin, NpcKey{ 0xFF000F00, 1 } }, { NpcKey{ 0xFF000803, 2 }, MadeVerdict::kFull, {} } } };
	CHECK(Protocol::DecodeNpcMadeAnswers(Protocol::Encode(answers)) == answers);
	CHECK(Protocol::PeekType(Protocol::Encode(answers)) == MT::kNpcMadeAnswers);

	CHECK(FormTranslate::CarriesForms(MT::kNpcMades));
	CHECK(!FormTranslate::CarriesForms(MT::kNpcMadeAnswers));
}

TEST("made protocol: the most fit; more, a load-order NPC, an unknown flag, an alias without its flag, a place out of the world or a byte too many are refused")
{
	Protocol::NpcMades most;
	for (std::uint32_t i = 0; i < Protocol::MAX_NPC_MADES; ++i) {
		most.mades.push_back(Made(0xFF000800 + i, 1, ByAlias(COURIER_QUEST, 3, 10.0f)));
	}
	const auto encoded = Protocol::Encode(most);
	CHECK(encoded.size() <= 2048u);
	CHECK(Protocol::DecodeNpcMades(encoded) == most);

	auto tooMany = encoded;
	tooMany[6] = static_cast<std::uint8_t>(Protocol::MAX_NPC_MADES + 1);  // type, sender (4), full, count
	CHECK(!Protocol::DecodeNpcMades(tooMany));

	const auto refuses = [](NpcMade a_made) { return !Protocol::DecodeNpcMades(Protocol::Encode(Protocol::NpcMades{ .mades = { a_made } })); };
	CHECK(refuses(Made(0x00012345, 0, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f))));  // a load-order NPC
	MadeSource badFlag = Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f);
	badFlag.flags = 0x80;
	CHECK(refuses(Made(0xFF000801, 1, badFlag)));
	MadeSource aliasNoFlag = Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f);
	aliasNoFlag.quest = COURIER_QUEST;
	CHECK(refuses(Made(0xFF000801, 1, aliasNoFlag)));
	CHECK(refuses(Made(0xFF000801, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 1e12f))));

	auto extra = Protocol::Encode(Protocol::NpcMades{ .mades = { Made(0xFF000801, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f)) } });
	extra.push_back(0);
	CHECK(!Protocol::DecodeNpcMades(extra));

	// Answers: of a load-order NPC, an unknown verdict, a twin of the same maker or none, a twin key on another verdict.
	const auto answerRefused = [](std::vector<std::uint8_t> a_packet) { return !Protocol::DecodeNpcMadeAnswers(a_packet); };
	CHECK(answerRefused(Protocol::Encode(Protocol::NpcMadeAnswers{ .answers = { { NpcKey{ 0x00012345, 0 }, MadeVerdict::kShared, {} } } })));
	CHECK(answerRefused(Protocol::Encode(Protocol::NpcMadeAnswers{ .answers = { { NpcKey{ 0xFF000801, 2 }, MadeVerdict::kTwin, NpcKey{ 0xFF000900, 2 } } } })));
	CHECK(answerRefused(Protocol::Encode(Protocol::NpcMadeAnswers{ .answers = { { NpcKey{ 0xFF000801, 2 }, MadeVerdict::kTwin, {} } } })));
	auto badVerdict = Protocol::Encode(Protocol::NpcMadeAnswers{ .answers = { { NpcKey{ 0xFF000801, 2 }, MadeVerdict::kShared, {} } } });
	badVerdict[2 + 8] = Protocol::MADE_VERDICT_COUNT;  // type, count, key (8)
	CHECK(answerRefused(badVerdict));
	auto twinOnShared = Protocol::Encode(Protocol::NpcMadeAnswers{ .answers = { { NpcKey{ 0xFF000801, 2 }, MadeVerdict::kShared, {} } } });
	twinOnShared[2 + 8 + 1] = 0x01;  // a twin id on a shared answer
	CHECK(answerRefused(twinOnShared));
}

TEST("made form translate: a quest, a made NPC, a roll or a body the reader lacks become none; a place it lacks becomes nowhere; the maker's own id is kept")
{
	const Edges e;
	MadeSource  ofB = ByAlias(0x03000ABC, 2, 10.0f);  // B.esp's quest in the host's numbering
	ofB.made = 0x03000DEF;
	ofB.pick = 0x00012345;
	MadeSource ofExtra = Placed(0x04000456, 0x04000777, 20.0f);  // Extra.esp's NPC and race
	ofExtra.cell = 0x04000999;
	ofExtra.worldspace = 0;
	const auto got = Protocol::DecodeNpcMades(e.HostToGuest(Protocol::Encode(Protocol::NpcMades{ .sender = 1, .mades = { Made(0xFF000801, 1, ofB), Made(0xFF000802, 1, ofExtra) } })));
	REQUIRE(got);
	REQUIRE(got->mades.size() == 2u);
	CHECK((got->mades[0].key == NpcKey{ 0xFF000801, 1 }));
	CHECK(got->mades[0].source.quest == 0x01000ABCu);
	CHECK(got->mades[0].source.alias == 2u);  // an alias's number, no form
	CHECK(got->mades[0].source.made == 0x01000DEFu);
	CHECK(got->mades[0].source.pick == 0x00012345u);
	CHECK(got->mades[0].source.worldspace == TAMRIEL);
	CHECK((got->mades[1].key == NpcKey{ 0xFF000802, 1 }));
	CHECK(got->mades[1].source.made == 0u);
	CHECK(got->mades[1].source.body == 0u);
	CHECK(got->mades[1].source.cell == 0u);
	CHECK(got->mades[1].source.worldspace == 0u);
}

// ---- the server's book ----

TEST("made book: twins are one place, one body and either one quest's alias near, or the same NPC or roll near, or any roll at the very spot")
{
	using MadeBook::Twins;
	CHECK(Twins(ByAlias(COURIER_QUEST, 3, 0.0f), ByAlias(COURIER_QUEST, 3, MadeBook::PAIR_RANGE)));
	CHECK(!Twins(ByAlias(COURIER_QUEST, 3, 0.0f), ByAlias(COURIER_QUEST, 3, MadeBook::PAIR_RANGE + 1.0f)));  // two runs apart
	CHECK(!Twins(ByAlias(COURIER_QUEST, 3, 0.0f), ByAlias(COURIER_QUEST, 4, 0.0f)));
	CHECK(!Twins(ByAlias(0, 3, 0.0f), ByAlias(0, 3, 0.0f)));  // a quest the reader lacks: any two
	CHECK(!Twins(ByAlias(COURIER_QUEST, 3, 0.0f), Placed(0x000E5F1C, Protocol::BODY_PERSON, 0.0f)));  // an alias's and a script's

	CHECK(Twins(Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f), Placed(LVL_BANDIT, Protocol::BODY_PERSON, MadeBook::SAME_SPOT)));
	CHECK(!Twins(Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f), Placed(LVL_BANDIT, Protocol::BODY_PERSON, MadeBook::SAME_SPOT + 1.0f)));
	MadeSource rolled = Placed(0x00099999, Protocol::BODY_PERSON, 300.0f);
	rolled.pick = LVL_BANDIT;
	CHECK(Twins(Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f), rolled));  // the same roll of another list
	// Different rolls of a person: only at the very spot.
	CHECK(Twins(Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f), Placed(BANDIT_ROLL, Protocol::BODY_PERSON, MadeBook::BODY_SPOT)));
	CHECK(!Twins(Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f), Placed(BANDIT_ROLL, Protocol::BODY_PERSON, MadeBook::BODY_SPOT + 1.0f)));
	// Another body, another place, or nowhere: never.
	CHECK(!Twins(Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f), Placed(LVL_BANDIT, DRAGON_RACE, 0.0f)));
	MadeSource inside = Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f);
	inside.worldspace = 0;
	inside.cell = 0x00016BD7;
	CHECK(!Twins(Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f), inside));
	MadeSource nowhere = Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f);
	nowhere.worldspace = 0;
	CHECK(!Twins(nowhere, nowhere));
	MadeSource noBody = Placed(LVL_BANDIT, 0, 0.0f);
	CHECK(!Twins(noBody, noBody));
}

TEST("made book: the first said is shared; the same one of another game is its twin, the first heard of several; one's own are never twins; past the caps: full")
{
	MadeBook::Book book;
	const auto     dragon = Placed(0x000FEA9B, DRAGON_RACE, 0.0f);
	CHECK(book.Report(1, Made(0xFF000801, 1, dragon)).verdict == MadeVerdict::kShared);
	CHECK(book.Report(3, Made(0xFF000A01, 3, Placed(0x000FEA9B, DRAGON_RACE, 100.0f))).verdict == MadeVerdict::kTwin);  // twins are not kept
	CHECK(book.Report(2, Made(0xFF000901, 2, Placed(0x000FEA9B, DRAGON_RACE, 50.0f))) == (MadeBook::Answer{ MadeVerdict::kTwin, NpcKey{ 0xFF000801, 1 } }));
	// The same game's two at one spot are two (two bandits of one camp).
	CHECK(book.Report(1, Made(0xFF000802, 1, dragon)).verdict == MadeVerdict::kShared);
	// A later one of player 3 near both: the first heard of the two.
	CHECK(book.Report(3, Made(0xFF000A02, 3, dragon)) == (MadeBook::Answer{ MadeVerdict::kTwin, NpcKey{ 0xFF000801, 1 } }));
	// Said again (loaded again, moved): shared still, where it says now.
	CHECK(book.Report(1, Made(0xFF000801, 1, Placed(0x000FEA9B, DRAGON_RACE, 9000.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Find(NpcKey{ 0xFF000801, 1 })->x == 9000.0f);
	CHECK(book.Report(3, Made(0xFF000A03, 3, dragon)) == (MadeBook::Answer{ MadeVerdict::kTwin, NpcKey{ 0xFF000802, 1 } }));

	// Not one's own, a load-order NPC, one said gone, player 0: refused.
	CHECK(book.Report(2, Made(0xFF000801, 1, dragon)).verdict == MadeVerdict::kRefused);
	CHECK(book.Report(1, Made(0x00012345, 0, dragon)).verdict == MadeVerdict::kRefused);
	MadeSource gone = dragon;
	gone.flags = static_cast<std::uint8_t>(Protocol::kMadeGone);
	CHECK(book.Report(1, Made(0xFF000803, 1, gone)).verdict == MadeVerdict::kRefused);
	CHECK(book.Report(0, Made(0xFF000803, 0, dragon)).verdict == MadeVerdict::kRefused);
	CHECK(book.Size() == 2u);

	MadeBook::Book full;
	for (std::uint32_t i = 0; i < MadeBook::MAX_PER_PLAYER; ++i) {
		CHECK(full.Report(1, Made(0xFF000100 + i, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 1000.0f * static_cast<float>(i)))).verdict == MadeVerdict::kShared);
	}
	CHECK(full.Report(1, Made(0xFF000200, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, -50000.0f))).verdict == MadeVerdict::kFull);
	CHECK(full.Report(2, Made(0xFF000200, 2, Placed(LVL_BANDIT, Protocol::BODY_PERSON, -50000.0f))).verdict == MadeVerdict::kShared);
	MadeBook::Book session;
	std::uint32_t  id = 0xFF000100;
	for (std::uint32_t player = 1; session.Size() < MadeBook::MAX_SESSION; ++player) {
		for (std::size_t i = 0; i < MadeBook::MAX_PER_PLAYER && session.Size() < MadeBook::MAX_SESSION; ++i, ++id) {
			CHECK(session.Report(player, Made(id, player, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 1000.0f * static_cast<float>(id - 0xFF000000)))).verdict == MadeVerdict::kShared);
		}
	}
	CHECK(session.Report(99, Made(0xFF0FFFFF, 99, Placed(LVL_BANDIT, Protocol::BODY_PERSON, -50000.0f))).verdict == MadeVerdict::kFull);
}

TEST("made book: two of one alias shared apart that come near merge, the later giving way to the earliest once; script-made ones never merge")
{
	MadeBook::Book book;
	CHECK(book.Report(1, Made(0xFF000801, 1, ByAlias(COURIER_QUEST, 3, 0.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Report(2, Made(0xFF000901, 2, ByAlias(COURIER_QUEST, 3, 20000.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Report(3, Made(0xFF000A01, 3, ByAlias(COURIER_QUEST, 3, 40000.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Report(1, Made(0xFF000802, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Report(2, Made(0xFF000902, 2, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 20000.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Merges().empty());

	Protocol::PlayerState near;
	near.worldspace = TAMRIEL;
	near.x = 100.0f;
	book.Moved(NpcKey{ 0xFF000901, 2 }, near);
	book.Moved(NpcKey{ 0xFF000A01, 3 }, near);
	book.Moved(NpcKey{ 0xFF000902, 2 }, near);
	Protocol::PlayerState nowhere;
	book.Moved(NpcKey{ 0xFF000801, 1 }, nowhere);  // a state of nowhere moves nothing
	const auto merges = book.Merges();
	REQUIRE(merges.size() == 2u);
	CHECK((merges[0] == MadeBook::Merge{ NpcKey{ 0xFF000901, 2 }, NpcKey{ 0xFF000801, 1 } }));
	CHECK((merges[1] == MadeBook::Merge{ NpcKey{ 0xFF000A01, 3 }, NpcKey{ 0xFF000801, 1 } }));
	CHECK(!book.Find(NpcKey{ 0xFF000901, 2 }));
	CHECK(!book.Find(NpcKey{ 0xFF000A01, 3 }));
	CHECK(book.Find(NpcKey{ 0xFF000902, 2 }));  // a script's: never merged
	CHECK(book.Merges().empty());
}

TEST("made book: one goes when its maker says it gone, it dies or is let go, or its maker leaves; a joiner gets all in the order heard")
{
	MadeBook::Book book;
	CHECK(book.Report(2, Made(0xFF000901, 2, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Report(1, Made(0xFF000801, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 5000.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Report(1, Made(0xFF000802, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 9000.0f))).verdict == MadeVerdict::kShared);
	CHECK(book.Report(2, Made(0xFF000902, 2, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 15000.0f))).verdict == MadeVerdict::kShared);
	const auto all = book.All();
	REQUIRE(all.size() == 4u);
	CHECK((all[0].key == NpcKey{ 0xFF000901, 2 }));
	CHECK((all[1].key == NpcKey{ 0xFF000801, 1 }));
	CHECK((all[2].key == NpcKey{ 0xFF000802, 1 }));
	CHECK((all[3].key == NpcKey{ 0xFF000902, 2 }));

	MadeSource gone = Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f);
	gone.flags = static_cast<std::uint8_t>(Protocol::kMadeGone);
	CHECK(!book.Gone(1, Made(0xFF000901, 2, gone)));  // not its maker
	CHECK(book.Gone(2, Made(0xFF000901, 2, gone)));
	CHECK(!book.Gone(2, Made(0xFF000901, 2, gone)));  // once
	CHECK(book.Erase(NpcKey{ 0xFF000802, 1 }));
	CHECK(book.Of(1) == 1u);
	CHECK(book.Leave(1) == std::vector<NpcKey>({ NpcKey{ 0xFF000801, 1 } }));
	CHECK(book.Size() == 1u);
	// The spot is free: a later one there is shared, not a twin of one gone.
	CHECK(book.Report(3, Made(0xFF000A01, 3, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 5000.0f))).verdict == MadeVerdict::kShared);
}

// ---- the game's rules ----

TEST("made rules: a new living one is said, asked again after a lost answer, a full one after its wait; a body never")
{
	const auto        t0 = Steady::now();
	MadeRules::Ours   ours;
	const MadeSource  source = Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f);
	CHECK(MadeRules::SayNow(ours, true, t0));
	CHECK(!MadeRules::SayNow(ours, false, t0));
	CHECK(MadeRules::SeenAs(ours) == MadeRules::Seen::kWait);
	MadeRules::Said(ours, source, t0);
	CHECK(ours.phase == MadeRules::Phase::kAsked);
	CHECK(!MadeRules::SayNow(ours, true, t0 + MadeRules::ASK_AGAIN - 1ms));
	CHECK(MadeRules::SayNow(ours, true, t0 + MadeRules::ASK_AGAIN));
	CHECK(MadeRules::SeenAs(ours) == MadeRules::Seen::kWait);

	CHECK(MadeRules::Answered(ours, { NpcKey{ 0xFF000801, 1 }, MadeVerdict::kFull, {} }, t0));
	CHECK(ours.phase == MadeRules::Phase::kWaiting);
	CHECK(!MadeRules::SayNow(ours, true, t0 + MadeRules::FULL_AGAIN - 1ms));
	CHECK(MadeRules::SayNow(ours, true, t0 + MadeRules::FULL_AGAIN));
	CHECK(!MadeRules::Answered(ours, { NpcKey{ 0xFF000801, 1 }, MadeVerdict::kShared, {} }, t0));  // no answer to an ask: left waiting
	CHECK(!MadeRules::Answered(ours, { NpcKey{ 0xFF000801, 1 }, MadeVerdict::kFull, {} }, t0));

	MadeRules::Said(ours, source, t0);
	CHECK(MadeRules::Answered(ours, { NpcKey{ 0xFF000801, 1 }, MadeVerdict::kShared, {} }, t0));
	CHECK(MadeRules::SeenAs(ours) == MadeRules::Seen::kShare);
	CHECK(!MadeRules::SayNow(ours, true, t0 + 1h));
	CHECK(MadeRules::SayGone(ours));
	ours.goneSaid = true;
	CHECK(!MadeRules::SayGone(ours));
	CHECK(!MadeRules::Answered(ours, { NpcKey{ 0xFF000801, 1 }, MadeVerdict::kFull, {} }, t0));  // an old answer: still shared

	// A merge: a shared one becomes a twin; said again when its twin's maker lets it go.
	CHECK(MadeRules::Answered(ours, { NpcKey{ 0xFF000801, 1 }, MadeVerdict::kTwin, NpcKey{ 0xFF000901, 2 } }, t0));
	CHECK(ours.phase == MadeRules::Phase::kTwin);
	CHECK((ours.twin == NpcKey{ 0xFF000901, 2 }));
	CHECK(MadeRules::SeenAs(ours) == MadeRules::Seen::kTwin);
	CHECK(!MadeRules::SayGone(MadeRules::Ours{ .phase = MadeRules::Phase::kTwin }));
	CHECK(!MadeRules::Answered(ours, { NpcKey{ 0xFF000801, 1 }, MadeVerdict::kShared, {} }, t0));
	MadeRules::Again(ours);
	CHECK(ours.phase == MadeRules::Phase::kNew);
	CHECK((ours.twin == NpcKey{}));
	CHECK(MadeRules::SayNow(ours, true, t0));
}

TEST("made rules: a friend's let go of: our own twin is ours again; a copy we placed is kept only when its maker said what made it and it stands here")
{
	using MadeRules::LetGo;
	using MadeRules::OnLetGo;
	CHECK(OnLetGo({ .placed = false, .twin = true, .known = true, .there = true }) == LetGo::kUnpair);
	CHECK(OnLetGo({ .placed = false, .twin = true }) == LetGo::kUnpair);
	CHECK(OnLetGo({ .placed = true, .known = true, .there = true }) == LetGo::kAdopt);
	CHECK(OnLetGo({ .placed = true, .known = false, .there = true }) == LetGo::kTakeAway);  // a summon, a mount: goes as before
	CHECK(OnLetGo({ .placed = true, .known = true, .there = false }) == LetGo::kTakeAway);  // not loaded here
	CHECK(OnLetGo({ .placed = false, .known = true, .there = true }) == LetGo::kTakeAway);

	MadeSource theirs = ByAlias(COURIER_QUEST, 3, 0.0f);
	theirs.flags = static_cast<std::uint8_t>(theirs.flags | Protocol::kMadeGone);
	Protocol::PlayerState here;
	here.cell = 0x00016BD7;
	here.x = 1.0f;
	here.y = 2.0f;
	here.z = 3.0f;
	const auto said = MadeRules::Adopted(theirs, here, BANDIT_ROLL);
	CHECK(said.ByAlias());
	CHECK(!said.Gone());
	CHECK(said.quest == COURIER_QUEST);
	CHECK(said.alias == 3u);
	CHECK(said.cell == 0x00016BD7u);
	CHECK(said.worldspace == 0u);
	CHECK(said.z == 3.0f);
	CHECK(said.pick == BANDIT_ROLL);
	CHECK(MadeRules::Adopted(theirs, here, 0).pick == theirs.pick);
}

// ---- the server ----

TEST("made server: a shared one is granted to its maker before its answer, and passed on to the others with the maker named; a twin is answered alone")
{
	Fixture f;
	AllIn(f, 3);
	const auto bandit = Made(0xFF000801, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f));
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcMades{ .sender = 77, .mades = { bandit } }));
	REQUIRE(f.fake->Settle());
	const auto maker = HeardBy(f, 1);
	REQUIRE(maker.order.size() == 2u);
	CHECK(maker.order[0] == MT::kNpcOwners);
	CHECK(maker.order[1] == MT::kNpcMadeAnswers);
	CHECK(maker.owners[0].owners == std::vector<Protocol::NpcOwner>({ { bandit.key, 1 } }));
	CHECK(maker.answers[0].answers == std::vector<Protocol::NpcMadeAnswer>({ { bandit.key, MadeVerdict::kShared, {} } }));
	CHECK(maker.mades.empty());  // not back to its maker
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto other = HeardBy(f, peer);
		REQUIRE(other.mades.size() == 1u);
		CHECK(other.mades[0].sender == 1u);
		CHECK(other.mades[0].mades == std::vector<NpcMade>({ bandit }));
		REQUIRE(other.owners.size() == 1u);
		CHECK(other.answers.empty());
	}

	// Bo's game made the same bandit a moment later: its own shows Ada's. Nobody else hears a thing.
	const auto bosBandit = Made(0xFF000901, 2, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 40.0f));
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcMades{ .mades = { bosBandit } }));
	REQUIRE(f.fake->Settle());
	const auto twin = HeardBy(f, 2);
	REQUIRE(twin.answers.size() == 1u);
	CHECK(twin.answers[0].answers == std::vector<Protocol::NpcMadeAnswer>({ { bosBandit.key, MadeVerdict::kTwin, bandit.key } }));
	CHECK(twin.owners.empty());
	CHECK(HeardBy(f, 1).order.empty());
	CHECK(HeardBy(f, 3).order.empty());
	CHECK(f.LoggedSoon("is the twin of player 1's FF000801"));

	// One of another player's: refused, nothing passed on.
	f.fake->Deliver(3, Protocol::Encode(Protocol::NpcMades{ .mades = { Made(0xFF000802, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 9000.0f)) } }));
	REQUIRE(f.fake->Settle());
	const auto refused = HeardBy(f, 3);
	REQUIRE(refused.answers.size() == 1u);
	CHECK(refused.answers[0].answers[0].verdict == MadeVerdict::kRefused);
	CHECK(HeardBy(f, 1).order.empty());
}

TEST("made server: a joiner hears every shared one first, then who runs them; one said gone, dead or whose maker left is no longer told")
{
	Fixture f;
	AllIn(f, 2);
	const auto first = Made(0xFF000801, 1, Placed(LVL_BANDIT, Protocol::BODY_PERSON, 0.0f));
	const auto second = Made(0xFF000901, 2, ByAlias(COURIER_QUEST, 3, 9000.0f));
	const auto third = Made(0xFF000802, 1, Placed(0x000FEA9B, DRAGON_RACE, 30000.0f));
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcMades{ .mades = { first } }));
	REQUIRE(f.fake->Settle());
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcMades{ .mades = { second } }));
	REQUIRE(f.fake->Settle());
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcMades{ .mades = { third } }));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto joiner = HeardBy(f, 3);
	REQUIRE(joiner.mades.size() == 1u);
	CHECK(joiner.mades[0].full);
	CHECK(joiner.mades[0].mades == std::vector<NpcMade>({ first, second, third }));
	REQUIRE(joiner.order.size() >= 2u);
	CHECK(joiner.order[0] == MT::kNpcMades);
	CHECK(joiner.order[1] == MT::kNpcOwners);

	// Its maker says the dragon gone: the others hear it, and a later joiner is not told of it.
	MadeSource gone = third.source;
	gone.flags = static_cast<std::uint8_t>(Protocol::kMadeGone);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcMades{ .mades = { Made(third.key.id, 1, gone) } }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto other = HeardBy(f, peer);
		REQUIRE(other.mades.size() == 1u);
		CHECK(other.mades[0].mades[0].source.Gone());
	}
	CHECK(HeardBy(f, 1).answers.empty());  // nothing to answer
	// Ada leaves: hers go from the list.
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	REQUIRE(f.Join(4, Fixture::MakeHello("Di")));
	REQUIRE(f.fake->Settle());
	const auto late = HeardBy(f, 4);
	REQUIRE(late.mades.size() == 1u);
	CHECK(late.mades[0].mades == std::vector<NpcMade>({ second }));
}

TEST("made server: two of one alias shared apart that come near: the later's maker hears it is a twin before anyone hears it is nobody's")
{
	Fixture f;
	AllIn(f, 2);
	const auto adas = Made(0xFF000801, 1, ByAlias(COURIER_QUEST, 3, 0.0f));
	const auto bos = Made(0xFF000901, 2, ByAlias(COURIER_QUEST, 3, 30000.0f));
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcMades{ .mades = { adas } }));
	REQUIRE(f.fake->Settle());
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcMades{ .mades = { bos } }));
	REQUIRE(f.fake->Settle());
	CHECK(HeardBy(f, 2).answers.at(0).answers.at(0).verdict == MadeVerdict::kShared);
	(void)f.fake->Take(1);

	// Bo's courier walks to Ada's.
	f.fake->Deliver(2, Protocol::Encode(StateOf(bos.key, 200.0f)));
	REQUIRE(f.fake->Settle());
	const auto later = HeardBy(f, 2);
	REQUIRE(later.order.size() == 2u);
	CHECK(later.order[0] == MT::kNpcMadeAnswers);
	CHECK(later.order[1] == MT::kNpcOwners);
	CHECK(later.answers[0].answers == std::vector<Protocol::NpcMadeAnswer>({ { bos.key, MadeVerdict::kTwin, adas.key } }));
	CHECK(later.owners[0].owners == std::vector<Protocol::NpcOwner>({ { bos.key, 0 } }));
	const auto earlier = HeardBy(f, 1);
	REQUIRE(earlier.owners.size() == 1u);
	CHECK(earlier.owners[0].owners == std::vector<Protocol::NpcOwner>({ { bos.key, 0 } }));
	CHECK(f.LoggedSoon("stands as the twin of player 1's FF000801"));
}
