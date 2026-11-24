#include "Test.h"

#include "FormTranslate.h"
#include "NpcBook.h"
#include "ServerFixture.h"
#include "game/NpcRules.h"
#include "game/Prediction.h"
#include "game/StateBuffer.h"

#include <string>
#include <vector>

// Shared NPCs (docs/DESIGN.md, "NPCs"): their messages, the server's book of who runs each, what each game decides
// about the NPCs near its player, and the server passing on only a runner's word.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Why = Protocol::ClaimWhy;
	using Steady = std::chrono::steady_clock;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	constexpr Protocol::NpcKey GUARD{ 0x00012345, 0 };
	constexpr Protocol::NpcKey WOLF{ 0x00023456, 0 };
	constexpr Protocol::NpcKey ATRONACH_OF_1{ 0xFF000801, 1 };

	Protocol::PlayerState StateAt(std::uint32_t a_sequence, float a_x)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.worldspace = 0x3C;
		state.x = a_x;
		state.y = 20.0f;
		state.z = 30.0f;
		state.heading = 1.5f;
		state.speed = 150.0f;
		state.sentMs = 77;
		state.direction = 32;
		state.pitch = -10;
		state.flags = Protocol::kWeaponDrawn | Protocol::kFlying;
		return state;
	}

	Protocol::NpcClaims Claims(std::initializer_list<Protocol::NpcClaim> a_claims)
	{
		return Protocol::NpcClaims{ .claims = a_claims };
	}

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

	std::vector<std::pair<Protocol::NpcOwners, bool>> OwnersSent(Fixture& a_fixture, PeerId a_peer)
	{
		return SentOf<Protocol::NpcOwners>(a_fixture, a_peer, MT::kNpcOwners, Protocol::DecodeNpcOwners);
	}

	// Two players in a session, everything they were sent so far forgotten.
	void TwoIn(Fixture& a_f)
	{
		REQUIRE(a_f.Join(1, Fixture::MakeHello("Ada")));
		REQUIRE(a_f.Join(2, Fixture::MakeHello("Bo")));
		REQUIRE(a_f.fake->Settle());
		(void)a_f.fake->Take(1);
		(void)a_f.fake->Take(2);
	}
}

// ---- the messages ----

TEST("npc protocol: claims, owners, states, actions and gear read back as they were written")
{
	const auto claims = Claims({ { GUARD, Why::kHave }, { WOLF, Why::kFight }, { ATRONACH_OF_1, Why::kFollower }, { GUARD, Why::kRelease } });
	CHECK(Protocol::DecodeNpcClaims(Protocol::Encode(claims)) == claims);
	CHECK(Protocol::PeekType(Protocol::Encode(claims)) == MT::kNpcClaims);

	const Protocol::NpcOwners owners{ .full = true, .owners = { { GUARD, 1 }, { WOLF, 0 }, { ATRONACH_OF_1, 1 } } };
	CHECK(Protocol::DecodeNpcOwners(Protocol::Encode(owners)) == owners);

	const Protocol::NpcStates states{ .runner = 2, .npcs = { { GUARD, 0x00013BBF, StateAt(5, 10.0f) }, { ATRONACH_OF_1, 0x000204C0, StateAt(9, -40.0f) } } };
	CHECK(Protocol::DecodeNpcStates(Protocol::Encode(states)) == states);

	const Protocol::NpcActions actions{ .runner = 2, .actions = { { GUARD, 1234, "attackStart" }, { WOLF, 0, "FlyStartCruise" } } };
	CHECK(Protocol::DecodeNpcActions(Protocol::Encode(actions)) == actions);

	const Protocol::NpcGear gear{ .runner = 2, .key = GUARD, .worn = { { 0x00012E49, 0x4 }, { 0x00012E4B, 0x80 } }, .right = 0x00012EB7, .left = 0x00012EB6, .ammo = 0x0001397D, .unnamed = Protocol::kAmmoUnnamed, .unnamedSlots = 0x2 };
	CHECK(Protocol::DecodeNpcGear(Protocol::Encode(gear)) == gear);
	// A mage's: a spell in each hand and a shout ready (protocol 21), the voice unnamed or not.
	const Protocol::NpcGear mage{ .runner = 2, .key = GUARD, .right = 0x00012FCD, .left = 0x0002F3B8, .voice = 0x00013E07 };
	CHECK(Protocol::DecodeNpcGear(Protocol::Encode(mage)) == mage);
	const Protocol::NpcGear unnamedVoice{ .runner = 2, .key = GUARD, .right = 0x00012FCD, .unnamed = Protocol::kVoiceUnnamed };
	CHECK(Protocol::DecodeNpcGear(Protocol::Encode(unnamedVoice)) == unnamedVoice);
}

TEST("npc protocol: the longest of each message fits its limit, and one past a limit, or cut short, is refused")
{
	Protocol::NpcOwners all{ .full = true };
	for (std::uint32_t i = 0; i < Protocol::MAX_NPCS; ++i) {
		all.owners.push_back({ { 0x00100000 + i, 0 }, 1 });
	}
	const auto allBytes = Protocol::Encode(all);
	CHECK(allBytes.size() <= Protocol::MAX_NPC_OWNERS_SIZE);
	CHECK(Protocol::DecodeNpcOwners(allBytes) == all);

	Protocol::NpcStates states;
	for (std::uint32_t i = 0; i < Protocol::MAX_NPC_STATES; ++i) {
		states.npcs.push_back({ { 0x00100000 + i, 0 }, 0x00013BBF, StateAt(i + 1, 10.0f) });
	}
	const auto stateBytes = Protocol::Encode(states);
	CHECK(stateBytes.size() <= Protocol::MAX_NPC_STATES_SIZE);
	CHECK(stateBytes.size() <= 1200u);
	CHECK(Protocol::DecodeNpcStates(stateBytes) == states);

	Protocol::NpcActions actions;
	for (std::uint32_t i = 0; i < Protocol::MAX_NPC_ACTIONS; ++i) {
		actions.actions.push_back({ { 0x00100000 + i, 0 }, i, std::string(Protocol::MAX_EVENT_NAME, 'a') });
	}
	const auto actionBytes = Protocol::Encode(actions);
	CHECK(actionBytes.size() <= Protocol::MAX_NPC_ACTIONS_SIZE);
	CHECK(Protocol::DecodeNpcActions(actionBytes) == actions);

	// One past a count: the encoder writes no more than the limit, so the count byte is forged.
	auto tooMany = Protocol::Encode(Claims({ { GUARD, Why::kHave } }));
	tooMany[1] = static_cast<std::uint8_t>((Protocol::MAX_NPC_CLAIMS + 1) & 0xFF);
	tooMany[2] = static_cast<std::uint8_t>((Protocol::MAX_NPC_CLAIMS + 1) >> 8);
	CHECK(!Protocol::DecodeNpcClaims(tooMany));

	for (auto bytes : { Protocol::Encode(Claims({ { GUARD, Why::kHave } })), allBytes, stateBytes, actionBytes,
			 Protocol::Encode(Protocol::NpcGear{ .key = GUARD, .worn = { { 1, 4 }, { 2, 8 } } }) }) {
		bytes.pop_back();
		const auto type = Protocol::PeekType(bytes);
		REQUIRE(type);
		switch (*type) {
		case MT::kNpcClaims:
			CHECK(!Protocol::DecodeNpcClaims(bytes));
			break;
		case MT::kNpcOwners:
			CHECK(!Protocol::DecodeNpcOwners(bytes));
			break;
		case MT::kNpcStates:
			CHECK(!Protocol::DecodeNpcStates(bytes));
			break;
		case MT::kNpcActions:
			CHECK(!Protocol::DecodeNpcActions(bytes));
			break;
		default:
			CHECK(!Protocol::DecodeNpcGear(bytes));
			break;
		}
	}
}

TEST("npc protocol: a claim of no NPC or of an unknown reason, a state of no NPC or nowhere, and an empty event are refused")
{
	CHECK(!Protocol::DecodeNpcClaims(Protocol::Encode(Claims({ { { 0, 0 }, Why::kHave } }))));
	auto unknownWhy = Protocol::Encode(Claims({ { GUARD, Why::kHave } }));
	unknownWhy.back() = Protocol::CLAIM_WHY_COUNT;
	CHECK(!Protocol::DecodeNpcClaims(unknownWhy));

	CHECK(!Protocol::DecodeNpcStates(Protocol::Encode(Protocol::NpcStates{ .npcs = { { { 0, 0 }, 0, StateAt(1, 1.0f) } } })));
	auto nowhere = StateAt(1, 1.0f);
	nowhere.x = std::numeric_limits<float>::quiet_NaN();
	CHECK(!Protocol::DecodeNpcStates(Protocol::Encode(Protocol::NpcStates{ .npcs = { { GUARD, 0, nowhere } } })));

	CHECK(!Protocol::DecodeNpcActions(Protocol::Encode(Protocol::NpcActions{ .actions = { { GUARD, 1, "" } } })));
	CHECK(!Protocol::DecodeNpcActions(Protocol::Encode(Protocol::NpcActions{ .actions = { { { 0, 0 }, 1, "attackStart" } } })));
	// Gear of no NPC reads (the translation leaves that for an NPC the other game lacks); its reader skips it.
	CHECK(Protocol::DecodeNpcGear(Protocol::Encode(Protocol::NpcGear{ .key = {} })));
	// An unnamed bit this version does not know is refused.
	auto unknownBit = Protocol::Encode(Protocol::NpcGear{ .key = GUARD });
	unknownBit[unknownBit.size() - 5] = 0x10;
	CHECK(!Protocol::DecodeNpcGear(unknownBit));
	unknownBit[unknownBit.size() - 5] = Protocol::kAllUnnamed;
	CHECK(Protocol::DecodeNpcGear(unknownBit));
	unknownBit[unknownBit.size() - 5] = Protocol::kAllLooksUnnamed;  // the voice's bit too (protocol 21)
	CHECK(Protocol::DecodeNpcGear(unknownBit));
}

TEST("npc protocol: every NPC message carries forms")
{
	for (const auto type : { MT::kNpcClaims, MT::kNpcOwners, MT::kNpcStates, MT::kNpcActions, MT::kNpcGear }) {
		CHECK(FormTranslate::CarriesForms(type));
	}
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
		std::vector<std::uint8_t> HostToGuest(const std::vector<std::uint8_t>& a_packet, FormTranslate::Result* a_in = nullptr) const
		{
			auto out = FormTranslate::Translate(hostEdge, FormTranslate::Way::kToSession, a_packet);
			const auto wire = out.kind == FormTranslate::Result::Kind::kChanged ? out.packet : a_packet;
			auto       in = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, wire);
			auto       read = in.kind == FormTranslate::Result::Kind::kChanged ? in.packet : wire;
			if (a_in) {
				*a_in = std::move(in);
			}
			return read;
		}
	};
}

TEST("npc form translate: an NPC of a plugin both games have is named as each game's own, and one only the sender has is left out")
{
	const Edges e;
	const Protocol::NpcKey ofB{ 0x03000ABC, 0 };      // B.esp's reference in the host's numbering
	const Protocol::NpcKey ofExtra{ 0x04000ABC, 0 };  // Extra.esp's: the guest lacks it
	const Protocol::NpcKey ofSkyrim{ 0x00013BBF, 0 };
	const Protocol::NpcKey spawn{ 0xFF000801, 1 };    // the host's game's own runtime reference: never translated

	const auto claims = Protocol::DecodeNpcClaims(e.HostToGuest(Protocol::Encode(Claims({ { ofB, Why::kHave }, { ofExtra, Why::kHave }, { ofSkyrim, Why::kFight }, { spawn, Why::kFollower } }))));
	REQUIRE(claims);
	REQUIRE(claims->claims.size() == 3u);
	CHECK((claims->claims[0].key == Protocol::NpcKey{ 0x01000ABC, 0 }));
	CHECK(claims->claims[1].key == ofSkyrim);
	CHECK(claims->claims[1].why == Why::kFight);
	CHECK(claims->claims[2].key == spawn);

	const auto owners = Protocol::DecodeNpcOwners(e.HostToGuest(Protocol::Encode(Protocol::NpcOwners{ .full = true, .owners = { { ofExtra, 1 }, { ofB, 1 } } })));
	REQUIRE(owners);
	CHECK(owners->full);
	REQUIRE(owners->owners.size() == 1u);
	CHECK(owners->owners[0].key.id == 0x01000ABCu);

	// A state: its NPC, its place and its base as the guest's; one somewhere the guest lacks is left out (no copy can
	// stand there), and a base the guest lacks reads 0.
	auto inB = StateAt(3, 5.0f);
	inB.worldspace = 0x03000D74;
	auto inExtra = StateAt(4, 5.0f);
	inExtra.worldspace = 0x04000D74;
	FormTranslate::Result in;
	const auto states = Protocol::DecodeNpcStates(e.HostToGuest(
		Protocol::Encode(Protocol::NpcStates{ .npcs = { { ofB, 0x03000900, inB }, { ofSkyrim, 0x04000900, inB }, { ofSkyrim, 0x00013BBF, inExtra } } }), &in));
	REQUIRE(states);
	REQUIRE(states->npcs.size() == 2u);
	CHECK(states->npcs[0].key.id == 0x01000ABCu);
	CHECK(states->npcs[0].base == 0x01000900u);
	CHECK(states->npcs[0].state.worldspace == 0x01000D74u);
	CHECK(states->npcs[1].key == ofSkyrim);
	CHECK(states->npcs[1].base == 0u);

	const auto actions = Protocol::DecodeNpcActions(e.HostToGuest(Protocol::Encode(Protocol::NpcActions{ .actions = { { ofExtra, 5, "attackStart" }, { ofB, 6, "blockStart" } } })));
	REQUIRE(actions);
	REQUIRE(actions->actions.size() == 1u);
	CHECK(actions->actions[0].key.id == 0x01000ABCu);
	CHECK(actions->actions[0].event == "blockStart");

	// Gear: worn items and held objects the guest lacks go, said as unnamed (their body slots, the hand), never as
	// nothing worn or an empty hand; gear of an NPC it lacks names none.
	const auto gear = Protocol::DecodeNpcGear(e.HostToGuest(Protocol::Encode(
		Protocol::NpcGear{ .key = ofB, .worn = { { 0x00012E49, 0x4 }, { 0x04000901, 0x1 | 0x2 } }, .right = 0x04000902, .left = 0x03000903, .ammo = 0 })));
	REQUIRE(gear);
	CHECK(gear->key.id == 0x01000ABCu);
	CHECK(gear->worn == std::vector<Protocol::WornItem>({ { 0x00012E49, 0x4 } }));
	CHECK(gear->unnamedSlots == 0x3u);
	CHECK(gear->right == 0u);
	CHECK(gear->left == 0x01000903u);
	CHECK(gear->unnamed == Protocol::kRightUnnamed);  // the left's is known, and no ammunition is none, not unnamed
	CHECK(gear->voice == 0u);
	// The voice the same way (protocol 21): a shout or power of B.esp is renumbered; one of Extra.esp, which the guest
	// lacks, is 0 said as unnamed (the copy keeps its own ready), never as none.
	const auto voiced = Protocol::DecodeNpcGear(e.HostToGuest(Protocol::Encode(Protocol::NpcGear{ .key = ofB, .voice = 0x03000904 })));
	REQUIRE(voiced);
	CHECK(voiced->voice == 0x01000904u);
	CHECK(voiced->unnamed == 0u);
	const auto unvoiced = Protocol::DecodeNpcGear(e.HostToGuest(Protocol::Encode(Protocol::NpcGear{ .key = ofB, .voice = 0x04000905 })));
	REQUIRE(unvoiced);
	CHECK(unvoiced->voice == 0u);
	CHECK(unvoiced->unnamed == Protocol::kVoiceUnnamed);
	const auto lacked = Protocol::DecodeNpcGear(e.HostToGuest(Protocol::Encode(Protocol::NpcGear{ .key = ofExtra, .worn = { { 0x00012E49, 0x4 } } })));
	REQUIRE(lacked);
	CHECK(lacked->key.id == 0u);
}

// ---- who runs each (the server's book) ----

TEST("npc book: the first to ask runs it; another asking is refused; its runner asking again keeps it")
{
	NpcBook::Book book;
	const auto    t = Steady::now();
	CHECK(book.Claim(1, { GUARD, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	CHECK(book.Owner(GUARD) == 1u);
	CHECK(book.Runs(1, GUARD));
	CHECK(!book.Runs(2, GUARD));
	CHECK(!book.Runs(0, WOLF));
	CHECK(book.Claim(2, { GUARD, Why::kHave }, t + 1min) == NpcBook::Outcome::kRefused);
	CHECK(book.Claim(1, { GUARD, Why::kHave }, t + 1min) == NpcBook::Outcome::kKept);
	CHECK(book.Owner(GUARD) == 1u);
}

TEST("npc book: a fight takes an NPC over, but not within the cooldown, nor from a runner who fought it lately")
{
	NpcBook::Book book;
	const auto    t = Steady::now();
	REQUIRE(book.Claim(1, { GUARD, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	// It just came to player 1: player 2's fight waits.
	CHECK(book.Claim(2, { GUARD, Why::kFight }, t + 1s) == NpcBook::Outcome::kRefused);
	CHECK(book.Claim(2, { GUARD, Why::kFight }, t + NpcBook::TAKEOVER_COOLDOWN) == NpcBook::Outcome::kGranted);
	CHECK(book.Owner(GUARD) == 2u);
	// Player 2 fights it; player 1 fights it too: it stays with player 2 while player 2 keeps fighting.
	const auto later = t + NpcBook::TAKEOVER_COOLDOWN * 2;
	CHECK(book.Claim(2, { GUARD, Why::kFight }, later) == NpcBook::Outcome::kKept);
	CHECK(book.Claim(1, { GUARD, Why::kFight }, later + 2s) == NpcBook::Outcome::kRefused);
	// Player 2 stopped fighting it: player 1's next blow takes it.
	CHECK(book.Claim(1, { GUARD, Why::kFight }, later + NpcBook::TAKEOVER_COOLDOWN) == NpcBook::Outcome::kGranted);
	CHECK(book.Owner(GUARD) == 1u);
	// A plain ask never takes one over.
	CHECK(book.Claim(2, { GUARD, Why::kHave }, later + 1h) == NpcBook::Outcome::kRefused);
}

TEST("npc book: a follower goes to its player's game, and stays with the first player it follows")
{
	NpcBook::Book book;
	const auto    t = Steady::now();
	REQUIRE(book.Claim(1, { GUARD, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	CHECK(book.Claim(2, { GUARD, Why::kFollower }, t + 10s) == NpcBook::Outcome::kGranted);
	CHECK(book.Owner(GUARD) == 2u);
	// It follows player 2 (player 2's game says so every FOLLOWER_EVERY): player 1's follower claim and fights don't take it.
	CHECK(book.Claim(2, { GUARD, Why::kFollower }, t + 25s) == NpcBook::Outcome::kKept);
	CHECK(book.Claim(2, { GUARD, Why::kFollower }, t + 40s) == NpcBook::Outcome::kKept);
	CHECK(book.Claim(2, { GUARD, Why::kFollower }, t + 55s) == NpcBook::Outcome::kKept);
	CHECK(book.Claim(1, { GUARD, Why::kFollower }, t + 1min) == NpcBook::Outcome::kRefused);
	CHECK(book.Claim(1, { GUARD, Why::kFight }, t + 1min) == NpcBook::Outcome::kRefused);
	CHECK(book.Owner(GUARD) == 2u);
}

// A runner says its NPC follows its player every FOLLOWER_EVERY; the book holds it as a follower a little over two of them.
static_assert(NpcBook::FOLLOWS_LAPSE > 2 * NpcRules::FOLLOWER_EVERY);

TEST("npc book: a follower its runner hired after it had it is held as one: a friend's stray blow or talk does not take it")
{
	NpcBook::Book book;
	const auto    t = Steady::now();
	// Player 1's game got Lydia because its player was nearest (a plain ask), then its player hired her.
	REQUIRE(book.Claim(1, { GUARD, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	CHECK(book.Claim(1, { GUARD, Why::kFollower }, t + 1min) == NpcBook::Outcome::kKept);
	// Player 2 hits her by accident in a fight, or talks to her: refused.
	CHECK(book.Claim(2, { GUARD, Why::kFight }, t + 1min + 1s) == NpcBook::Outcome::kRefused);
	// Her runner says so again every FOLLOWER_EVERY: still refused long after.
	CHECK(book.Claim(1, { GUARD, Why::kFollower }, t + 1min + NpcRules::FOLLOWER_EVERY) == NpcBook::Outcome::kKept);
	CHECK(book.Claim(1, { GUARD, Why::kFollower }, t + 1min + 2 * NpcRules::FOLLOWER_EVERY) == NpcBook::Outcome::kKept);
	CHECK(book.Claim(2, { GUARD, Why::kFight }, t + 1min + 2 * NpcRules::FOLLOWER_EVERY + 1s) == NpcBook::Outcome::kRefused);
	CHECK(book.Owner(GUARD) == 1u);
}

TEST("npc book: a follower dismissed by its runner's player is no longer held: a friend who hired her has her")
{
	const auto t = Steady::now();
	{
		// Player 1's game got her as a follower; its player dismissed her and its game says so (kHave).
		NpcBook::Book book;
		REQUIRE(book.Claim(1, { GUARD, Why::kFollower }, t) == NpcBook::Outcome::kGranted);
		CHECK(book.Claim(2, { GUARD, Why::kFollower }, t + 10s) == NpcBook::Outcome::kRefused);
		CHECK(book.Claim(1, { GUARD, Why::kHave }, t + 11s) == NpcBook::Outcome::kKept);
		// Player 2 hired her in their own game: theirs at the next ask.
		CHECK(book.Claim(2, { GUARD, Why::kFollower }, t + 12s) == NpcBook::Outcome::kGranted);
		CHECK(book.Owner(GUARD) == 2u);
	}
	{
		// Its runner never says either (its game lost track of her): she is held no longer than FOLLOWS_LAPSE.
		NpcBook::Book book;
		REQUIRE(book.Claim(1, { GUARD, Why::kFollower }, t) == NpcBook::Outcome::kGranted);
		CHECK(book.Claim(2, { GUARD, Why::kFight }, t + NpcBook::FOLLOWS_LAPSE - 1s) == NpcBook::Outcome::kRefused);
		CHECK(book.Claim(2, { GUARD, Why::kFight }, t + NpcBook::FOLLOWS_LAPSE) == NpcBook::Outcome::kGranted);
		CHECK(book.Owner(GUARD) == 2u);
	}
	{
		// A fight claim of its runner's says nothing of following: she stays held.
		NpcBook::Book book;
		REQUIRE(book.Claim(1, { GUARD, Why::kFollower }, t) == NpcBook::Outcome::kGranted);
		CHECK(book.Claim(1, { GUARD, Why::kFight }, t + 10s) == NpcBook::Outcome::kKept);
		CHECK(book.Claim(2, { GUARD, Why::kFollower }, t + 20s) == NpcBook::Outcome::kRefused);
	}
}

TEST("npc book: a spawn is only ever its spawner's")
{
	NpcBook::Book book;
	const auto    t = Steady::now();
	CHECK(book.Claim(2, { ATRONACH_OF_1, Why::kHave }, t) == NpcBook::Outcome::kRefused);
	CHECK(book.Owner(ATRONACH_OF_1) == 0u);
	CHECK(book.Claim(1, { ATRONACH_OF_1, Why::kFollower }, t) == NpcBook::Outcome::kGranted);
	CHECK(book.Claim(2, { ATRONACH_OF_1, Why::kFight }, t + 1h) == NpcBook::Outcome::kRefused);
	CHECK(book.Owner(ATRONACH_OF_1) == 1u);
}

TEST("npc book: only its runner lets an NPC go; a player who leaves lets go of all theirs")
{
	NpcBook::Book book;
	const auto    t = Steady::now();
	REQUIRE(book.Claim(1, { GUARD, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	REQUIRE(book.Claim(1, { ATRONACH_OF_1, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	REQUIRE(book.Claim(2, { WOLF, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	CHECK(book.Claim(2, { GUARD, Why::kRelease }, t) == NpcBook::Outcome::kIgnored);
	CHECK(book.Claim(2, { WOLF, Why::kRelease }, t) == NpcBook::Outcome::kReleased);
	CHECK(book.Owner(WOLF) == 0u);
	CHECK(book.Claim(1, { WOLF, Why::kHave }, t) == NpcBook::Outcome::kGranted);

	auto left = book.Leave(1);
	std::ranges::sort(left);
	CHECK((left == std::vector<Protocol::NpcKey>{ GUARD, WOLF, ATRONACH_OF_1 }));
	CHECK(book.Size() == 0u);
	CHECK(book.All().empty());
}

TEST("npc book: no more than MAX_NPCS are run at once")
{
	NpcBook::Book book;
	const auto    t = Steady::now();
	for (std::uint32_t i = 0; i < Protocol::MAX_NPCS; ++i) {
		REQUIRE(book.Claim(1, { { 0x00100000 + i, 0 }, Why::kHave }, t) == NpcBook::Outcome::kGranted);
	}
	CHECK(book.Claim(2, { GUARD, Why::kHave }, t) == NpcBook::Outcome::kIgnored);
	CHECK(book.Size() == Protocol::MAX_NPCS);
	CHECK(book.All().size() == Protocol::MAX_NPCS);
}

// ---- what each game decides ----

TEST("npc rules: nobody runs it: the nearest player's game asks first, any game after a wait, a fight or a follower at once")
{
	using namespace NpcRules;
	const auto t = Steady::now();
	Facts      nearer{ .owner = Owner::kNobody, .unownedSince = t, .ours = 300.0f, .nearestFriend = 2000.0f };
	CHECK(Decide(nearer, t) == Ask::kHave);
	Facts farther{ .owner = Owner::kNobody, .unownedSince = t, .ours = 3000.0f, .nearestFriend = 300.0f };
	CHECK(Decide(farther, t) == Ask::kNone);
	CHECK(Decide(farther, t + CLAIM_ANYWAY_AFTER) == Ask::kHave);
	// Within the margin counts as nearest: two players side by side both ask, and the server takes the first.
	Facts close{ .owner = Owner::kNobody, .unownedSince = t, .ours = 400.0f + CLAIM_MARGIN, .nearestFriend = 400.0f };
	CHECK(Decide(close, t) == Ask::kHave);
	// No friend in its space: ours to ask for.
	Facts alone{ .owner = Owner::kNobody, .unownedSince = t, .ours = 5000.0f };
	CHECK(Decide(alone, t) == Ask::kHave);
	auto fighting = farther;
	fighting.fighting = true;
	CHECK(Decide(fighting, t) == Ask::kFight);
	auto following = farther;
	following.followsUs = true;
	CHECK(Decide(following, t) == Ask::kFollower);
	// Not again within CLAIM_EVERY.
	nearer.lastClaim = t;
	CHECK(Decide(nearer, t + CLAIM_EVERY / 2) == Ask::kNone);
	CHECK(Decide(nearer, t + CLAIM_EVERY) == Ask::kHave);
}

TEST("npc rules: another game runs it: a fight asks every two seconds, a follower every fifteen, else nothing")
{
	using namespace NpcRules;
	const auto t = Steady::now();
	Facts      other{ .owner = Owner::kOther, .ours = 100.0f, .nearestFriend = 5000.0f };
	CHECK(Decide(other, t) == Ask::kNone);  // nearer than its runner's player, but it stays where it is
	other.fighting = true;
	CHECK(Decide(other, t) == Ask::kFight);
	other.lastClaim = t;
	CHECK(Decide(other, t + 1s) == Ask::kNone);
	CHECK(Decide(other, t + FIGHT_EVERY) == Ask::kFight);
	Facts follower{ .owner = Owner::kOther, .ours = 100.0f, .followsUs = true, .lastClaim = t };
	CHECK(Decide(follower, t + 10s) == Ask::kNone);
	CHECK(Decide(follower, t + FOLLOWER_EVERY) == Ask::kFollower);
}

TEST("npc rules: the runner lets go of an NPC its player left behind for a friend near it, but never a follower or one it fights")
{
	using namespace NpcRules;
	const auto t = Steady::now();
	Facts      near{ .owner = Owner::kUs, .ours = 500.0f, .nearestFriend = 100.0f };
	CHECK(Decide(near, t) == Ask::kNone);
	Facts behind{ .owner = Owner::kUs, .ours = HANDOFF_FAR + 1.0f, .nearestFriend = 200.0f };
	CHECK(Decide(behind, t) == Ask::kRelease);
	// Far, but the friend is no nearer by the margin: it stays.
	Facts both{ .owner = Owner::kUs, .ours = HANDOFF_FAR + 1.0f, .nearestFriend = HANDOFF_FAR };
	CHECK(Decide(both, t) == Ask::kNone);
	// Through a door our player didn't take, a friend there: theirs. Nobody there: it stays.
	Facts elsewhere{ .owner = Owner::kUs, .nearestFriend = 300.0f };
	CHECK(Decide(elsewhere, t) == Ask::kRelease);
	Facts lonely{ .owner = Owner::kUs };
	CHECK(Decide(lonely, t) == Ask::kNone);
	auto follower = behind;
	follower.followsUs = true;
	follower.saidFollows = t;  // the book has heard it follows us (the next test)
	CHECK(Decide(follower, t) == Ask::kNone);
	auto fought = behind;
	fought.fighting = true;
	CHECK(Decide(fought, t) == Ask::kFight);  // its runner's own fight keeps it where it is
	CHECK(WhyOf(Ask::kRelease) == Why::kRelease);
	CHECK(WhyOf(Ask::kHave) == Why::kHave);
	CHECK(WhyOf(Ask::kFight) == Why::kFight);
	CHECK(WhyOf(Ask::kFollower) == Why::kFollower);
}

TEST("npc rules: the runner says its NPC follows its player as it starts and every fifteen seconds, and says when it stopped")
{
	using namespace NpcRules;
	const auto t = Steady::now();
	// Hired after we got it with a plain ask: said at once, however we got it.
	Facts hired{ .owner = Owner::kUs, .ours = 200.0f, .nearestFriend = 300.0f, .followsUs = true, .lastClaim = t - 1s };
	CHECK(Decide(hired, t) == Ask::kFollower);
	// Said: not again within FOLLOWER_EVERY, again after it.
	hired.saidFollows = t;
	hired.lastClaim = t;
	CHECK(Decide(hired, t + FOLLOWER_EVERY - 1s) == Ask::kNone);
	CHECK(Decide(hired, t + FOLLOWER_EVERY) == Ask::kFollower);
	// Our player hits it by accident: the fight is said between the follower's turns, the follower's turn first.
	auto hit = hired;
	hit.fighting = true;
	CHECK(Decide(hit, t + 3s) == Ask::kFight);
	CHECK(Decide(hit, t + FOLLOWER_EVERY) == Ask::kFollower);
	// Dismissed: said once (kHave), then nothing (the caller forgets saidFollows after a kHave).
	auto dismissed = hired;
	dismissed.followsUs = false;
	CHECK(Decide(dismissed, t + 2s) == Ask::kHave);
	dismissed.saidFollows.reset();
	dismissed.lastClaim = t + 2s;
	CHECK(Decide(dismissed, t + 3s) == Ask::kNone);
	// One that never followed us is never said to: nothing changes for it.
	Facts plain{ .owner = Owner::kUs, .ours = 200.0f, .nearestFriend = 300.0f };
	CHECK(Decide(plain, t) == Ask::kNone);
}

TEST("npc rules: an NPC let go of for a friend whose game lacks it is not asked for again, so it never trades back and forth")
{
	using namespace NpcRules;
	const auto t = Steady::now();
	// The host runs a bandit its player left 6500 u behind; the VM's player stands 300 u from it, but the VM's game does
	// not list it (disabled in its save, or not among its loaded actors). The host lets go of it ...
	const Facts run{ .owner = Owner::kUs, .ours = 6500.0f, .nearestFriend = 300.0f };
	CHECK(Decide(run, t) == Ask::kRelease);
	// ... and nobody runs it two seconds on: the host's game does not ask for it again, now or later.
	const Facts after{ .owner = Owner::kNobody, .unownedSince = t - 2s, .ours = 6500.0f, .nearestFriend = 300.0f, .lastClaim = t - 1s };
	CHECK(Decide(after, t) == Ask::kNone);
	CHECK(Decide(after, t + 1min) == Ask::kNone);
	// The same through a door: our player in another space, a friend in its own.
	const Facts elsewhere{ .owner = Owner::kNobody, .unownedSince = t - 2s, .nearestFriend = 300.0f };
	CHECK(Decide(elsewhere, t) == Ask::kNone);
	// Once our player comes back near (or the friend walks off), it is ours to ask for again.
	auto back = after;
	back.ours = 400.0f;
	CHECK(Decide(back, t) == Ask::kHave);
	auto gone = after;
	gone.nearestFriend.reset();
	CHECK(Decide(gone, t) == Ask::kHave);
	// A fight still takes it at once.
	auto fought = after;
	fought.fighting = true;
	CHECK(Decide(fought, t) == Ask::kFight);
	// Far, but no friend nearer by the margin: the wait still lets any game that has it ask.
	const Facts farBoth{ .owner = Owner::kNobody, .unownedSince = t - 2s, .ours = 6500.0f, .nearestFriend = 5800.0f };
	CHECK(Decide(farBoth, t) == Ask::kHave);
}

TEST("npc rules: a spawn is kept by its spawner's game while it has it, and asked for at once when nobody runs it")
{
	using namespace NpcRules;
	const auto t = Steady::now();
	// A conjurer's atronach: its player far, a friend near. It is no other game's to run, so it is never let go of.
	const Facts ours{ .owner = Owner::kUs, .ours = 9000.0f, .nearestFriend = 100.0f, .spawn = true };
	CHECK(Decide(ours, t) == Ask::kNone);
	auto lost = ours;
	lost.ours.reset();
	CHECK(Decide(lost, t) == Ask::kNone);
	const Facts nobody{ .owner = Owner::kNobody, .unownedSince = t, .ours = 9000.0f, .nearestFriend = 100.0f, .spawn = true };
	CHECK(Decide(nobody, t) == Ask::kHave);
	auto asked = nobody;
	asked.lastClaim = t;
	CHECK(Decide(asked, t + CLAIM_EVERY / 2) == Ask::kNone);
	CHECK(Decide(asked, t + CLAIM_EVERY) == Ask::kHave);
}

TEST("npc rules: an actor made as the game ran is shared when our player or an NPC we run commands it")
{
	using namespace NpcRules;
	CHECK(SharedAsSpawn(Commander::kOurPlayer));  // our conjured atronach, our thrall
	CHECK(SharedAsSpawn(Commander::kOurNpc));     // a necromancer's raised corpse, a conjurer follower's familiar
	CHECK(!SharedAsSpawn(Commander::kOther));     // another game's NPC's summon: that game shares it
	CHECK(!SharedAsSpawn(Commander::kNobody));    // a quest's or an encounter's: each game makes its own
}

TEST("npc rules: states go 20 a second in a fight or near a player, else 10")
{
	using namespace NpcRules;
	CHECK(SendEvery(true, std::nullopt) == Clock::duration{ FAST_EVERY });
	CHECK(SendEvery(false, 1000.0f) == Clock::duration{ FAST_EVERY });
	CHECK(SendEvery(false, FAST_WITHIN + 1.0f) == Clock::duration{ SLOW_EVERY });
	CHECK(SendEvery(false, std::nullopt) == Clock::duration{ SLOW_EVERY });
}

TEST("npc rules: a copy follows its runner only with a recent state in our player's space and reach")
{
	using namespace NpcRules;
	CHECK(CopyMode(true, true, 1000.0f) == Mode::kMirror);
	CHECK(CopyMode(false, true, 1000.0f) == Mode::kSilent);
	CHECK(CopyMode(true, false, std::nullopt) == Mode::kElsewhere);
	CHECK(CopyMode(true, true, MIRROR_WITHIN + 1.0f) == Mode::kFar);
	CHECK(ModeName(Mode::kMirror) == "mirror");
	CHECK(ModeName(Mode::kFar) == "far");
}

TEST("npc rules: a copy plays its runner's events but not the walk's own, whatever their case, nor names no graph takes")
{
	using namespace NpcRules;
	CHECK(Replayed("attackStart"));
	CHECK(Replayed("bowAttackStart"));
	CHECK(Replayed("FlyStartCruise"));
	CHECK(Replayed("blockStart"));
	for (const auto local : LOCAL_EVENTS) {
		CHECK(!Replayed(local));
	}
	CHECK(!Replayed("MOVESTART"));
	CHECK(!Replayed("sprintstop"));
	CHECK(!Replayed(""));
	CHECK(!Replayed(std::string(Protocol::MAX_EVENT_NAME + 1, 'a')));
	CHECK(!Replayed("attack\nStart"));
}

TEST("npc rules: a copy's events play when it is drawn at their time, in order, and stale ones are dropped")
{
	using namespace NpcRules;
	const auto  t = Steady::now();
	ActionQueue queue;
	queue.Push("attackStart", t + 100ms, t);
	queue.Push("blockStart", t + 50ms, t);
	queue.Push("now", std::nullopt, t);  // no time: at once
	CHECK(queue.Size() == 3u);
	CHECK(queue.Due(t) == std::vector<std::string>{ "now" });
	CHECK(queue.Due(t + 60ms) == std::vector<std::string>{ "blockStart" });
	CHECK(queue.Due(t + 60ms).empty());
	CHECK(queue.Due(t + 100ms) == std::vector<std::string>{ "attackStart" });
	// A clock read far ahead is held to STALE; one that waits past STALE is dropped.
	queue.Push("far", t + 1h, t);
	CHECK(queue.Due(t + ActionQueue::STALE) == std::vector<std::string>{ "far" });
	queue.Push("old", t, t);
	CHECK(queue.Due(t + ActionQueue::STALE + 1ms).empty());
	CHECK(queue.Dropped() == 1u);
	// No more than MAX wait.
	for (std::size_t i = 0; i < ActionQueue::MAX + 5; ++i) {
		queue.Push("e", t, t);
	}
	CHECK(queue.Size() == ActionQueue::MAX);
	CHECK(queue.Dropped() == 6u);
	queue.Clear();
	CHECK(queue.Size() == 0u);
}

namespace
{
	// Skyrim.esm's guard gear, for the gear tests: a cuirass (body), boots (feet), a helmet (head), a shield, a sword, a
	// bow, a torch, arrows.
	constexpr std::uint32_t CUIRASS = 0x00012E49, BOOTS = 0x00012E4B, HELMET = 0x00012E4D, SHIELD = 0x00012EB6;
	constexpr std::uint32_t SWORD = 0x00012EB7, BOW = 0x0001397E, TORCH = 0x0001D4EC, ARROWS = 0x0001397D;
	constexpr std::uint32_t BODY = 0x4, FEET = 0x80, HEAD = 0x1, SHIELD_SLOT = 0x200;

	std::vector<NpcRules::GearItem> Items(std::initializer_list<NpcRules::GearItem> a_items) { return a_items; }
}

TEST("npc rules: a copy puts on what it carries of its runner's gear; only a spawn's copy is given what it lacks")
{
	using namespace NpcRules;
	const Protocol::NpcGear gear{ .key = GUARD, .worn = { { CUIRASS, BODY }, { BOOTS, FEET }, { 0xFF000900, HEAD }, { CUIRASS, BODY } }, .right = SWORD, .left = 0, .ammo = ARROWS };
	// It wears the cuirass already and carries the boots and the sword; the arrows it lacks.
	const Wearing              now{ .armor = { { CUIRASS, CUIRASS, BODY } } };
	const std::vector<Count>   carried{ { CUIRASS, 1 }, { BOOTS, 1 }, { SWORD, 1 } };
	const auto                 real = PlanGear(gear, now, carried, false);
	CHECK(real.equip == Items({ { BOOTS, Hand::kNone }, { SWORD, Hand::kRight } }));
	CHECK(real.add.empty());
	CHECK(real.unequip.empty());
	CHECK(real.missing == 1u);
	const auto spawn = PlanGear(gear, now, carried, true);
	CHECK(spawn.equip == Items({ { BOOTS, Hand::kNone }, { SWORD, Hand::kRight }, { ARROWS, Hand::kNone } }));
	CHECK(spawn.add == std::vector<Count>({ { ARROWS, 1 } }));
	CHECK(spawn.missing == 0u);
	// All on already: nothing to do.
	const Wearing dressed{ .armor = { { CUIRASS, CUIRASS, BODY }, { BOOTS, BOOTS, FEET } }, .right = SWORD, .ammo = ARROWS };
	const auto    none = PlanGear(gear, dressed, carried, false);
	CHECK(none.equip.empty());
	CHECK(none.unequip.empty());
	CHECK(none.missing == 0u);
}

TEST("npc rules: a copy takes off what its runner's NPC put away: a torch, a helmet, a shield, a bow swapped for a sword")
{
	using namespace NpcRules;
	const std::vector<Count> carried{ { CUIRASS, 1 }, { HELMET, 1 }, { SHIELD, 1 }, { SWORD, 1 }, { BOW, 1 }, { TORCH, 1 }, { ARROWS, 20 } };
	// Dusk: the guard lit a torch, and the copy holds it. Dawn: the runner's guard put it away (left 0).
	const Wearing night{ .armor = { { CUIRASS, CUIRASS, BODY } }, .right = SWORD, .left = TORCH };
	const auto    dawn = PlanGear({ .key = GUARD, .worn = { { CUIRASS, BODY } }, .right = SWORD }, night, carried, false);
	CHECK(dawn.unequip == Items({ { TORCH, Hand::kLeft } }));
	CHECK(dawn.equip.empty());
	// A helmet taken off, a shield put away: worn armor the runner's list no longer has.
	const Wearing armed{ .armor = { { CUIRASS, CUIRASS, BODY }, { HELMET, HELMET, HEAD }, { SHIELD, SHIELD, SHIELD_SLOT } }, .right = SWORD, .left = SHIELD };
	const auto    bare = PlanGear({ .key = GUARD, .worn = { { CUIRASS, BODY } }, .right = SWORD }, armed, carried, false);
	CHECK(bare.unequip == Items({ { HELMET, Hand::kNone }, { SHIELD, Hand::kNone } }));  // the shield once, as armor
	CHECK(bare.equip.empty());
	// A bow swapped for a sword: the bow comes off, the sword goes on; the arrows too.
	const Wearing archer{ .armor = { { CUIRASS, CUIRASS, BODY } }, .right = BOW, .ammo = ARROWS };
	const auto    melee = PlanGear({ .key = GUARD, .worn = { { CUIRASS, BODY } }, .right = SWORD }, archer, carried, false);
	CHECK(melee.unequip == Items({ { BOW, Hand::kRight }, { ARROWS, Hand::kNone } }));
	CHECK(melee.equip == Items({ { SWORD, Hand::kRight } }));
	// ... and with a sword the copy does not carry, it keeps its bow rather than fight barehanded; the arrows, which the
	// runner's NPC has none of, come off.
	const auto lacking = PlanGear({ .key = GUARD, .worn = { { CUIRASS, BODY } }, .right = 0x00099999 }, archer, carried, false);
	CHECK(lacking.unequip == Items({ { ARROWS, Hand::kNone } }));
	CHECK(lacking.equip.empty());
	CHECK(lacking.missing == 1u);
	// A spawn's copy is given the sword, so its bow comes off.
	const auto spawn = PlanGear({ .key = GUARD, .right = 0x00099999 }, archer, carried, true);
	CHECK(spawn.unequip == Items({ { CUIRASS, Hand::kNone }, { BOW, Hand::kRight }, { ARROWS, Hand::kNone } }));
	CHECK(spawn.add == std::vector<Count>({ { 0x00099999, 1 } }));
}

TEST("npc rules: a copy keeps what its runner's game could not name to it, never left bare for a mod it lacks")
{
	using namespace NpcRules;
	const std::vector<Count> carried{ { CUIRASS, 1 }, { HELMET, 1 }, { SWORD, 1 }, { BOW, 1 } };
	const Wearing            now{ .armor = { { CUIRASS, CUIRASS, BODY }, { HELMET, HELMET, HEAD } }, .right = SWORD, .left = BOW };
	// The runner's NPC wears a mod's cuirass and holds a weapon made as its game ran: the copy keeps its cuirass and sword.
	const auto keep = PlanGear({ .key = GUARD, .worn = { { HELMET, HEAD } }, .right = 0, .left = 0, .unnamed = Protocol::kRightUnnamed, .unnamedSlots = BODY }, now, carried, false);
	CHECK(keep.unequip == Items({ { BOW, Hand::kLeft } }));  // the left hand is empty there, not unnamed
	CHECK(keep.equip.empty());
	// A runtime armor of the copy's own counts as the plugin armor it was made from.
	const Wearing enchanted{ .armor = { { 0xFF000A00, CUIRASS, BODY } } };
	const auto    same = PlanGear({ .key = GUARD, .worn = { { CUIRASS, BODY } } }, enchanted, carried, false);
	CHECK(same.unequip.empty());
	CHECK(same.equip.empty());
	// One with no plugin armor behind it is never compared, so never taken off.
	const Wearing odd{ .armor = { { 0xFF000A01, 0, BODY } } };
	CHECK(PlanGear({ .key = GUARD }, odd, carried, false).unequip.empty());
}

TEST("npc rules: a weapon in both hands is put in both, which takes two of it")
{
	using namespace NpcRules;
	const Protocol::NpcGear twin{ .key = GUARD, .right = SWORD, .left = SWORD };
	// Two carried: both hands.
	CHECK(PlanGear(twin, {}, { { SWORD, 2 } }, false).equip == Items({ { SWORD, Hand::kRight }, { SWORD, Hand::kLeft } }));
	// One carried: the right only; the left is missing.
	const auto one = PlanGear(twin, {}, { { SWORD, 1 } }, false);
	CHECK(one.equip == Items({ { SWORD, Hand::kRight } }));
	CHECK(one.missing == 1u);
	// Held in the right already: the left only.
	CHECK(PlanGear(twin, { .right = SWORD }, { { SWORD, 2 } }, false).equip == Items({ { SWORD, Hand::kLeft } }));
	// A spawn's copy is given the two it lacks, no more.
	const auto spawn = PlanGear(twin, {}, {}, true);
	CHECK(spawn.equip == Items({ { SWORD, Hand::kRight }, { SWORD, Hand::kLeft } }));
	CHECK(spawn.add == std::vector<Count>({ { SWORD, 1 }, { SWORD, 1 } }));
	// Moved from the right hand to the left: off the right first, then into the left.
	const auto moved = PlanGear({ .key = GUARD, .left = SWORD }, { .right = SWORD }, { { SWORD, 1 } }, false);
	CHECK(moved.unequip == Items({ { SWORD, Hand::kRight } }));
	CHECK(moved.equip == Items({ { SWORD, Hand::kLeft } }));
}

TEST("npc rules: a two-handed weapon or spell is the right hand's alone, however the game reports it")
{
	using namespace NpcRules;
	constexpr std::uint32_t GREATSWORD = 0x0001359D, RITUAL = 0x0010F7EC;
	// Reported in both hands: the right only (a bow, a greatsword, a ritual spell).
	CHECK((OneReportEach(BOW, BOW, true, true) == Hands{ BOW, 0 }));
	CHECK((OneReportEach(GREATSWORD, GREATSWORD, true, true) == Hands{ GREATSWORD, 0 }));
	CHECK((OneReportEach(RITUAL, RITUAL, true, true) == Hands{ RITUAL, 0 }));
	// Reported in one hand: still the right's.
	CHECK((OneReportEach(BOW, 0, true, false) == Hands{ BOW, 0 }));
	CHECK((OneReportEach(0, BOW, false, true) == Hands{ BOW, 0 }));
	// One-handed in both hands (dual wielding, dual casting) stays in both; a sword and a shield or torch are untouched.
	CHECK((OneReportEach(SWORD, SWORD, false, false) == Hands{ SWORD, SWORD }));
	CHECK((OneReportEach(SWORD, SHIELD, false, false) == Hands{ SWORD, SHIELD }));
	CHECK((OneReportEach(SWORD, TORCH, false, false) == Hands{ SWORD, TORCH }));
	CHECK((OneReportEach(0, 0, false, false) == Hands{ 0, 0 }));
}

TEST("npc rules: sword to bow and bow to sword, each carried once, needs nothing more and lacks nothing")
{
	using namespace NpcRules;
	const std::vector<Count> carried{ { CUIRASS, 1 }, { SWORD, 1 }, { BOW, 1 }, { ARROWS, 20 } };
	// Both sides report a bow as the right hand's alone (OneReportEach).
	const auto bow = OneReportEach(BOW, BOW, true, true);
	const auto sword = OneReportEach(SWORD, 0, false, false);
	// A guard draws its bow: the sword comes off, the bow goes in the right, the arrows on. No second bow wanted.
	const auto draw = PlanGear({ .key = GUARD, .worn = { { CUIRASS, BODY } }, .right = bow.right, .left = bow.left, .ammo = ARROWS },
		{ .armor = { { CUIRASS, CUIRASS, BODY } }, .right = sword.right, .left = sword.left }, carried, false);
	CHECK(draw.unequip == Items({ { SWORD, Hand::kRight } }));
	CHECK(draw.equip == Items({ { BOW, Hand::kRight }, { ARROWS, Hand::kNone } }));
	CHECK(draw.add.empty());
	CHECK(draw.missing == 0u);
	// ... and back to the sword: the bow comes off once, the sword goes on.
	const auto back = PlanGear({ .key = GUARD, .worn = { { CUIRASS, BODY } }, .right = sword.right, .left = sword.left },
		{ .armor = { { CUIRASS, CUIRASS, BODY } }, .right = bow.right, .left = bow.left, .ammo = ARROWS }, carried, false);
	CHECK(back.unequip == Items({ { BOW, Hand::kRight }, { ARROWS, Hand::kNone } }));
	CHECK(back.equip == Items({ { SWORD, Hand::kRight } }));
	CHECK(back.missing == 0u);
	// A spawn's copy (a Dremora's greatsword) is given one, not two.
	constexpr std::uint32_t GREATSWORD = 0x0001359D;
	const auto              great = OneReportEach(GREATSWORD, GREATSWORD, true, true);
	const auto              spawn = PlanGear({ .key = GUARD, .right = great.right, .left = great.left }, {}, {}, true);
	CHECK(spawn.equip == Items({ { GREATSWORD, Hand::kRight } }));
	CHECK(spawn.add == std::vector<Count>({ { GREATSWORD, 1 } }));
	CHECK(spawn.missing == 0u);
}

TEST("npc rules: a copy's gear read back is complete only with all of it on but what it lacks, and nothing left on")
{
	using namespace NpcRules;
	const Protocol::NpcGear gear{ .key = GUARD, .worn = { { CUIRASS, BODY }, { HELMET, HEAD }, { SHIELD, SHIELD_SLOT } }, .right = SWORD, .left = SHIELD, .ammo = ARROWS };
	const Wearing           all{ .armor = { { CUIRASS, CUIRASS, BODY }, { HELMET, HELMET, HEAD }, { SHIELD, SHIELD, SHIELD_SLOT } }, .right = SWORD, .left = SHIELD, .ammo = ARROWS };
	const auto              ok = CheckGear(gear, all);
	CHECK(ok.named == 3u);
	CHECK(ok.matched == 3u);
	CHECK(ok.extra == 0u);
	CHECK(ok.Complete(0));
	// An equip the game refused: the helmet is not on. Short by one: complete only if the copy lacked it.
	auto noHelmet = all;
	noHelmet.armor.erase(noHelmet.armor.begin() + 1);
	CHECK(!CheckGear(gear, noHelmet).Complete(0));
	CHECK(CheckGear(gear, noHelmet).Complete(1));
	// The shield lacking: counted once (as armor), and the left hand holds nothing else: complete with one missing.
	Wearing noShield{ .armor = { { CUIRASS, CUIRASS, BODY }, { HELMET, HELMET, HEAD } }, .right = SWORD, .ammo = ARROWS };
	CHECK(CheckGear(gear, noShield).Short() == 1u);
	CHECK(CheckGear(gear, noShield).Complete(1));
	// A bow left in hand by an unequip that did nothing: the right hand differs, whatever is worn.
	auto bow = all;
	bow.right = BOW;
	CHECK(!CheckGear(gear, bow).right);
	CHECK(!CheckGear(gear, bow).Complete(0));
	// A torch in the left hand where the shield is not on: the copy keeps its own there when it lacks the shield, which
	// is short once already, so it is complete only if the shield was lacked.
	auto torch = noShield;
	torch.left = TORCH;
	CHECK(CheckGear(gear, torch).left);
	CHECK(!CheckGear(gear, torch).Complete(0));
	CHECK(CheckGear(gear, torch).Complete(1));
	// A torch still in the left hand with the shield on: the hand differs.
	auto both = all;
	both.left = TORCH;
	CHECK(!CheckGear(gear, both).left);
	// The helmet the runner's NPC took off, still on: left on, never complete, whatever it lacks.
	const Protocol::NpcGear bare{ .key = GUARD, .worn = { { CUIRASS, BODY }, { SHIELD, SHIELD_SLOT } }, .right = SWORD, .left = SHIELD, .ammo = ARROWS };
	CHECK(CheckGear(bare, all).extra == 1u);
	CHECK(!CheckGear(bare, all).Complete(5));
	// ... unless the runner's game could not name what it wears there (its own kept: PlanGear never takes it off).
	auto unnamedHead = bare;
	unnamedHead.unnamedSlots = HEAD;
	CHECK(CheckGear(unnamedHead, all).Complete(0));
	// A hand the runner's game could not name is not judged, nor the arrows.
	auto unnamedHands = gear;
	unnamedHands.unnamed = static_cast<std::uint8_t>(Protocol::kRightUnnamed | Protocol::kAmmoUnnamed);
	auto own = all;
	own.right = BOW;
	own.ammo = 0;
	CHECK(CheckGear(unnamedHands, own).Complete(0));
	// An enchanted copy of the cuirass (a runtime form made from it) counts as the cuirass.
	auto enchanted = all;
	enchanted.armor[0] = { 0xFF000A00, CUIRASS, BODY };
	CHECK(CheckGear(gear, enchanted).Complete(0));
}

TEST("npc rules: a copy whose outfit roll differs keeps its own where it lacks its runner's, never left bare, and is judged dressed")
{
	using namespace NpcRules;
	// A bandit's leveled outfit: hide armor in the runner's game, fur armor in this one.
	constexpr std::uint32_t HIDE = 0x00013911, FUR = 0x0006F393, ROBE = 0x0001BE1A, DAGGER = 0x0001397F;
	const Protocol::NpcGear hide{ .key = GUARD, .worn = { { HIDE, BODY } } };
	const Wearing           fur{ .armor = { { FUR, FUR, BODY } } };
	// It carries its fur only: the fur stays on, the hide armor is missing.
	const auto keep = PlanGear(hide, fur, { { FUR, 1 } }, false);
	CHECK(keep.unequip.empty());
	CHECK(keep.equip.empty());
	CHECK(keep.missing == 1u);
	CHECK(CheckGear(hide, fur).extra == 0u);
	CHECK(CheckGear(hide, fur).Complete(1));
	CHECK(!CheckGear(hide, fur).Complete(0));
	// A runner's NPC with no helmet still takes the copy's helmet off; the fur stays.
	const Wearing furHelmet{ .armor = { { FUR, FUR, BODY }, { HELMET, HELMET, HEAD } } };
	const auto    bareHead = PlanGear(hide, furHelmet, { { FUR, 1 }, { HELMET, 1 } }, false);
	CHECK(bareHead.unequip == Items({ { HELMET, Hand::kNone } }));
	CHECK(bareHead.missing == 1u);
	CHECK(CheckGear(hide, furHelmet).extra == 1u);  // the helmet left on is still caught
	// It carries the hide armor too: the fur comes off and the hide goes on; if the game refused the hide, it is short.
	const auto swap = PlanGear(hide, fur, { { FUR, 1 }, { HIDE, 1 } }, false);
	CHECK(swap.unequip == Items({ { FUR, Hand::kNone } }));
	CHECK(swap.equip == Items({ { HIDE, Hand::kNone } }));
	CHECK(swap.missing == 0u);
	CHECK(!CheckGear(hide, fur).Complete(swap.missing));
	// A lacked item over two slots (a robe with its boots) keeps the copy's own in both.
	const Wearing furBoots{ .armor = { { FUR, FUR, BODY }, { BOOTS, BOOTS, FEET } } };
	const Protocol::NpcGear robe{ .key = GUARD, .worn = { { ROBE, BODY | FEET } } };
	CHECK(PlanGear(robe, furBoots, { { FUR, 1 }, { BOOTS, 1 } }, false).unequip.empty());
	CHECK(CheckGear(robe, furBoots).Complete(1));
	// A spawn's copy is given the hide armor, so its fur comes off.
	const auto spawn = PlanGear(hide, fur, {}, true);
	CHECK(spawn.unequip == Items({ { FUR, Hand::kNone } }));
	CHECK(spawn.add == std::vector<Count>({ { HIDE, 1 } }));
	CHECK(spawn.equip == Items({ { HIDE, Hand::kNone } }));
	// The hands too: dual swords where the copy carries one sword and a dagger: the dagger stays in the left.
	const Protocol::NpcGear twin{ .key = GUARD, .right = SWORD, .left = SWORD };
	const Wearing           held{ .right = SWORD, .left = DAGGER };
	const auto              hands = PlanGear(twin, held, { { SWORD, 1 }, { DAGGER, 1 } }, false);
	CHECK(hands.unequip.empty());
	CHECK(hands.equip.empty());
	CHECK(hands.missing == 1u);
	CHECK(CheckGear(twin, held).Complete(1));
	// A shield the copy lacks: the torch in its left hand stays, counted once (the shield).
	const Protocol::NpcGear shield{ .key = GUARD, .worn = { { SHIELD, SHIELD_SLOT } }, .right = SWORD, .left = SHIELD };
	const Wearing           torch{ .right = SWORD, .left = TORCH };
	const auto              lit = PlanGear(shield, torch, { { SWORD, 1 }, { TORCH, 1 } }, false);
	CHECK(lit.unequip.empty());
	CHECK(lit.missing == 1u);
	CHECK(CheckGear(shield, torch).Complete(1));
}

TEST("npc rules: a copy follows its runner again after the NPC changed hands and came back, whatever the runner numbered it")
{
	using namespace NpcRules;
	const auto start = Steady::now();
	// The runner numbers each NPC's states for the whole session: having it again (its loaded entry made anew) goes on.
	Sequences numbers;
	for (std::uint32_t i = 1; i <= 5; ++i) {
		CHECK(numbers.Next(GUARD) == i);
	}
	CHECK(numbers.Next(WOLF) == 1u);  // each NPC its own
	CHECK(numbers.Next(GUARD) == 6u);
	numbers.Clear();  // a new session
	CHECK(numbers.Next(GUARD) == 1u);

	// The watcher's copy held states 1..N of runner 1; the NPC went to nobody and back to runner 1, whose numbers
	// started over (a runner built before the session-long count, or any other reason): the copy takes them.
	Feed                feed;
	StateBuffer::Buffer buffer;
	REQUIRE(feed.From(1));  // the first states: nothing held to drop
	for (std::uint32_t i = 1; i <= 50; ++i) {
		REQUIRE(buffer.Push(StateAt(i, static_cast<float>(i)), start + std::chrono::milliseconds(50 * i)));
	}
	CHECK(!feed.From(1));  // the same runner, no change of hands: kept
	CHECK(!buffer.Push(StateAt(1, 0.0f), start + 3s));  // what a copy that kept its states would do: refuse it
	feed.HandsChanged();   // to nobody
	feed.HandsChanged();   // and back to runner 1
	REQUIRE(feed.From(1));
	buffer.Clear();
	CHECK(buffer.Push(StateAt(1, 0.0f), start + 3s));
	CHECK(buffer.Push(StateAt(2, 1.0f), start + 3s + 50ms));
	CHECK(!feed.From(1));
	// Another runner: dropped as before.
	CHECK(feed.From(2));
}

TEST("npc rules: every kind of actor is told apart, the most telling first")
{
	using namespace NpcRules;
	CHECK(KindOf(false, false, false, false, true) == Kind::kHuman);
	CHECK(KindOf(false, false, false, false, false) == Kind::kCreature);
	CHECK(KindOf(false, false, true, false, false) == Kind::kDragon);
	CHECK(KindOf(false, false, false, true, false) == Kind::kHorse);
	CHECK(KindOf(false, true, false, true, true) == Kind::kFollower);
	CHECK(KindOf(true, true, true, false, true) == Kind::kSummon);
	std::set<std::string_view> names;
	for (std::size_t i = 0; i < static_cast<std::size_t>(Kind::kCount); ++i) {
		names.insert(KindName(static_cast<Kind>(i)));
	}
	CHECK((names == std::set<std::string_view>{ "human", "creature", "dragon", "horse", "follower", "summon" }));
}

TEST("npc rules: an event's time on its runner's clock falls among that runner's states' stamps")
{
	Prediction::SenderClock clock;
	CHECK(!clock.At(5));
	const auto t = Steady::now();
	const auto first = clock.Next(t, 1000);
	REQUIRE(clock.At(1000));
	CHECK(*clock.At(1000) == first);
	CHECK(*clock.At(1030) == first + 30ms);
	CHECK(*clock.At(970) == first - 30ms);
	// Across the wrap of the sender's millisecond count.
	Prediction::SenderClock wrapping;
	const auto              w = wrapping.Next(t, 0xFFFFFFF0u);
	CHECK(*wrapping.At(0x00000010u) == w + 32ms);
}

// ---- the server ----

TEST("npc server: the first to claim runs it and everyone hears; a refused claimant hears who runs it")
{
	Fixture f;
	TwoIn(f);
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave }, { ATRONACH_OF_1, Why::kFollower } })));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u }) {
		const auto told = OwnersSent(f, peer);
		REQUIRE(told.size() == 1u);
		CHECK(told[0].second);
		CHECK(!told[0].first.full);
		CHECK(told[0].first.owners == std::vector<Protocol::NpcOwner>({ { GUARD, 1 }, { ATRONACH_OF_1, 1 } }));
	}

	// Bo asks for both: the guard is Ada's, the atronach Ada's spawn. Only Bo is told.
	f.fake->Deliver(2, Protocol::Encode(Claims({ { GUARD, Why::kHave }, { ATRONACH_OF_1, Why::kFight } })));
	REQUIRE(f.fake->Settle());
	CHECK(OwnersSent(f, 1).empty());
	const auto refused = OwnersSent(f, 2);
	REQUIRE(refused.size() == 1u);
	CHECK(refused[0].first.owners == std::vector<Protocol::NpcOwner>({ { GUARD, 1 }, { ATRONACH_OF_1, 1 } }));

	// Ada asks again: nothing to tell.
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.fake->Take(2).empty());
}

TEST("npc server: only an NPC's runner's states, events and gear are passed on, named as theirs")
{
	Fixture f;
	TwoIn(f);
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// Ada runs the guard, not the wolf: the wolf's state is dropped, the guard's passed on to Bo only, unreliably.
	const Protocol::NpcStates states{ .runner = 99, .npcs = { { GUARD, 0x00013BBF, StateAt(1, 5.0f) }, { WOLF, 0, StateAt(1, 6.0f) } } };
	f.fake->Deliver(1, Protocol::Encode(states));
	// Bo's word on the guard is nobody's.
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcStates{ .npcs = { { GUARD, 0, StateAt(2, 7.0f) } } }));
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcActions{ .actions = { { GUARD, 1, "attackStart" } } }));
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcGear{ .key = GUARD, .worn = { { 0x00012E49, 0x4 } } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	const auto heard = SentOf<Protocol::NpcStates>(f, 2, MT::kNpcStates, Protocol::DecodeNpcStates);
	REQUIRE(heard.size() == 1u);
	CHECK(!heard[0].second);
	CHECK(heard[0].first.runner == 1u);
	REQUIRE(heard[0].first.npcs.size() == 1u);
	CHECK(heard[0].first.npcs[0] == states.npcs[0]);

	const Protocol::NpcActions actions{ .actions = { { GUARD, 40, "attackStart" }, { WOLF, 41, "attackStart" } } };
	f.fake->Deliver(1, Protocol::Encode(actions));
	const Protocol::NpcGear gear{ .key = GUARD, .worn = { { 0x00012E49, 0x4 } }, .right = 0x00012EB7 };
	f.fake->Deliver(1, Protocol::Encode(gear));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	const auto sent = f.fake->TakeSent(2);
	REQUIRE(sent.size() == 2u);
	const auto actionsOut = Protocol::DecodeNpcActions(sent[0].data);
	REQUIRE(actionsOut);
	CHECK(sent[0].reliable);
	CHECK(actionsOut->runner == 1u);
	CHECK(actionsOut->actions == std::vector<Protocol::NpcAction>({ { GUARD, 40, "attackStart" } }));
	const auto gearOut = Protocol::DecodeNpcGear(sent[1].data);
	REQUIRE(gearOut);
	CHECK(sent[1].reliable);
	auto expected = gear;
	expected.runner = 1;
	CHECK(*gearOut == expected);
}

TEST("npc server: one who joins later hears who runs each NPC and its gear; a player who leaves lets go of theirs")
{
	Fixture f;
	TwoIn(f);
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave }, { ATRONACH_OF_1, Why::kHave } })));
	f.fake->Deliver(2, Protocol::Encode(Claims({ { WOLF, Why::kHave } })));
	REQUIRE(f.fake->Settle());
	const Protocol::NpcGear gear{ .key = GUARD, .worn = { { 0x00012E49, 0x4 } } };
	f.fake->Deliver(1, Protocol::Encode(gear));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto sent = f.fake->TakeSent(3);
	std::optional<Protocol::NpcOwners> owners;
	std::vector<Protocol::NpcGear>     gears;
	for (const auto& packet : sent) {
		if (Protocol::PeekType(packet.data) == MT::kNpcOwners) {
			owners = Protocol::DecodeNpcOwners(packet.data);
		} else if (Protocol::PeekType(packet.data) == MT::kNpcGear) {
			gears.push_back(*Protocol::DecodeNpcGear(packet.data));
		}
	}
	REQUIRE(owners);
	CHECK(owners->full);
	CHECK(owners->owners == std::vector<Protocol::NpcOwner>({ { GUARD, 1 }, { WOLF, 2 }, { ATRONACH_OF_1, 1 } }));
	REQUIRE(gears.size() == 1u);
	CHECK(gears[0].key == GUARD);
	CHECK(gears[0].runner == 1u);
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// Ada leaves: her guard and her atronach are nobody's, and the others hear it.
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 2u, 3u }) {
		const auto told = OwnersSent(f, peer);
		REQUIRE(told.size() == 1u);
		CHECK(told[0].first.owners == std::vector<Protocol::NpcOwner>({ { GUARD, 0 }, { ATRONACH_OF_1, 0 } }));
	}
	CHECK(f.LoggedSoon("server: the 2 NPC(s) player 1 ran are nobody's now"));
	// Bo may take the guard now.
	f.fake->Deliver(2, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	REQUIRE(f.fake->Settle());
	const auto toBo = OwnersSent(f, 2);
	REQUIRE(toBo.size() == 1u);
	CHECK(toBo[0].first.owners == std::vector<Protocol::NpcOwner>({ { GUARD, 2 } }));
}

TEST("npc server: a runner lets go, and everyone hears the NPC is nobody's; another's release changes nothing")
{
	Fixture f;
	TwoIn(f);
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kHave } })));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);
	f.fake->Deliver(2, Protocol::Encode(Claims({ { GUARD, Why::kRelease } })));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.fake->Take(2).empty());
	f.fake->Deliver(1, Protocol::Encode(Claims({ { GUARD, Why::kRelease } })));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u }) {
		const auto told = OwnersSent(f, peer);
		REQUIRE(told.size() == 1u);
		CHECK(told[0].first.owners == std::vector<Protocol::NpcOwner>({ { GUARD, 0 } }));
	}
}
