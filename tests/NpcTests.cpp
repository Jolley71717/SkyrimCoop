#include "Test.h"

#include "FormTranslate.h"
#include "NpcBook.h"
#include "ServerFixture.h"
#include "game/NpcRules.h"
#include "game/Prediction.h"

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

	const Protocol::NpcGear gear{ .runner = 2, .key = GUARD, .worn = { 0x00012E49, 0x00012E4B }, .right = 0x00012EB7, .left = 0x00012EB6, .ammo = 0x0001397D };
	CHECK(Protocol::DecodeNpcGear(Protocol::Encode(gear)) == gear);
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
			 Protocol::Encode(Protocol::NpcGear{ .key = GUARD, .worn = { 1, 2 } }) }) {
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
}

TEST("npc protocol: every NPC message carries forms, and this protocol is version 5")
{
	for (const auto type : { MT::kNpcClaims, MT::kNpcOwners, MT::kNpcStates, MT::kNpcActions, MT::kNpcGear }) {
		CHECK(FormTranslate::CarriesForms(type));
	}
	CHECK(Protocol::VERSION == 5);
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

	// Gear: worn items and held objects the guest lacks go; gear of an NPC it lacks names none.
	const auto gear = Protocol::DecodeNpcGear(e.HostToGuest(Protocol::Encode(Protocol::NpcGear{ .key = ofB, .worn = { 0x00012E49, 0x04000901 }, .right = 0x04000902, .left = 0x03000903 })));
	REQUIRE(gear);
	CHECK(gear->key.id == 0x01000ABCu);
	CHECK(gear->worn == std::vector<std::uint32_t>{ 0x00012E49 });
	CHECK(gear->right == 0u);
	CHECK(gear->left == 0x01000903u);
	const auto lacked = Protocol::DecodeNpcGear(e.HostToGuest(Protocol::Encode(Protocol::NpcGear{ .key = ofExtra, .worn = { 0x00012E49 } })));
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
	// It follows player 2: player 1's follower claim and fights don't take it.
	CHECK(book.Claim(1, { GUARD, Why::kFollower }, t + 1min) == NpcBook::Outcome::kRefused);
	CHECK(book.Claim(1, { GUARD, Why::kFight }, t + 1min) == NpcBook::Outcome::kRefused);
	CHECK(book.Owner(GUARD) == 2u);
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
	CHECK(Decide(follower, t) == Ask::kNone);
	auto fought = behind;
	fought.fighting = true;
	CHECK(Decide(fought, t) == Ask::kFight);  // its runner's own fight keeps it where it is
	CHECK(WhyOf(Ask::kRelease) == Why::kRelease);
	CHECK(WhyOf(Ask::kHave) == Why::kHave);
	CHECK(WhyOf(Ask::kFight) == Why::kFight);
	CHECK(WhyOf(Ask::kFollower) == Why::kFollower);
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

TEST("npc rules: a copy equips what it carries of its runner's gear; only a spawn's copy is given what it lacks")
{
	using namespace NpcRules;
	const Protocol::NpcGear gear{ .key = GUARD, .worn = { 0x00012E49, 0x00012E4B, 0xFF000900, 0x00012E49 }, .right = 0x00012EB7, .left = 0, .ammo = 0x0001397D };
	// It wears the cuirass already and carries the boots and the sword; the arrows it lacks.
	const std::vector<std::uint32_t> worn{ 0x00012E49 };
	const std::vector<std::uint32_t> carried{ 0x00012E49, 0x00012E4B, 0x00012EB7 };
	const auto                       real = PlanGear(gear, worn, carried, false);
	CHECK(real.equip == std::vector<std::uint32_t>({ 0x00012E4B, 0x00012EB7 }));
	CHECK(real.add.empty());
	CHECK(real.missing == 1u);
	const auto spawn = PlanGear(gear, worn, carried, true);
	CHECK(spawn.equip == std::vector<std::uint32_t>({ 0x00012E4B, 0x00012EB7, 0x0001397D }));
	CHECK(spawn.add == std::vector<std::uint32_t>({ 0x0001397D }));
	CHECK(spawn.missing == 0u);
	// A weapon in both hands is equipped once.
	const Protocol::NpcGear twin{ .key = GUARD, .right = 0x00012EB7, .left = 0x00012EB7 };
	CHECK(PlanGear(twin, {}, carried, false).equip == std::vector<std::uint32_t>({ 0x00012EB7 }));
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
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcGear{ .key = GUARD, .worn = { 0x00012E49 } }));
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
	const Protocol::NpcGear gear{ .key = GUARD, .worn = { 0x00012E49 }, .right = 0x00012EB7 };
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
	const Protocol::NpcGear gear{ .key = GUARD, .worn = { 0x00012E49 } };
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
