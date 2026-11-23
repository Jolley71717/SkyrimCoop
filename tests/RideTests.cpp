#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "RideBook.h"
#include "ServerFixture.h"
#include "game/NpcRules.h"
#include "game/RideRules.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

// Riding (docs/DESIGN.md, "Riding"): who rides what on the wire, the server's book of it, how a game seats a friend's
// stand-in or a copy on its mount, what it says of its own riders, and how a mount goes with its rider's game.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Ride = Protocol::Ride;
	using Act = RideRules::Act;
	using Steady = std::chrono::steady_clock;

	constexpr Protocol::NpcKey HORSE{ 0x00068D07, 0 };
	constexpr Protocol::NpcKey OTHER_HORSE{ 0x00068D08, 0 };
	constexpr Protocol::NpcKey SOLDIER{ 0x00012345, 0 };
	constexpr Protocol::NpcKey PLACED_HORSE_OF_1{ 0xFF000801, 1 };

	Ride PlayerOn(std::uint32_t a_player, Protocol::NpcKey a_mount) { return Ride{ .player = a_player, .rider = {}, .mount = a_mount, .on = true }; }
	Ride PlayerOff(std::uint32_t a_player) { return Ride{ .player = a_player, .rider = {}, .mount = {}, .on = false }; }
	Ride NpcOn(Protocol::NpcKey a_rider, Protocol::NpcKey a_mount) { return Ride{ .player = 0, .rider = a_rider, .mount = a_mount, .on = true }; }
	Ride NpcOff(Protocol::NpcKey a_rider) { return Ride{ .player = 0, .rider = a_rider, .mount = {}, .on = false }; }

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

	template <class T, class Decode>
	std::vector<T> SentOf(Fixture& a_fixture, PeerId a_peer, MT a_type, Decode a_decode)
	{
		std::vector<T> out;
		for (auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == a_type) {
				CHECK(sent.reliable);
				if (auto msg = a_decode(sent.data)) {
					out.push_back(std::move(*msg));
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

	auto RunsNone() { return [](const Protocol::NpcKey&) { return false; }; }
	auto RunsSoldier() { return [](const Protocol::NpcKey& a_key) { return a_key == SOLDIER; }; }
}

// ---- the message ----

TEST("ride protocol: rides read back as they were written, as message 55 of protocol 12, and carry forms")
{
	const Protocol::Rides rides{ .sender = 2, .full = true, .rides = { PlayerOn(2, HORSE), NpcOn(SOLDIER, PLACED_HORSE_OF_1), NpcOff(PLACED_HORSE_OF_1), PlayerOff(3) } };
	CHECK(Protocol::DecodeRides(Protocol::Encode(rides)) == rides);
	CHECK(Protocol::PeekType(Protocol::Encode(rides)) == MT::kRides);
	CHECK(Protocol::DecodeRides(Protocol::Encode(Protocol::Rides{})) == Protocol::Rides{});
	static_assert(static_cast<int>(MT::kRides) == 55);
	static_assert(Protocol::VERSION >= 12);
	CHECK(FormTranslate::CarriesForms(MT::kRides));
}

TEST("ride protocol: the longest fits; a ride of nobody, a player's naming an NPC, a bad on, a mount of a spawner and no id, or too many are refused")
{
	Protocol::Rides many{ .full = true };
	for (std::uint32_t i = 0; i < Protocol::MAX_RIDES; ++i) {
		many.rides.push_back(NpcOn({ 0x00100000 + i, 0 }, { 0xFF000100 + i, 1 }));
	}
	const auto bytes = Protocol::Encode(many);
	CHECK(bytes.size() == Protocol::MAX_RIDES_SIZE);
	CHECK(Protocol::DecodeRides(bytes) == many);
	// One more is cut at the limit by the writer.
	many.rides.push_back(NpcOn(SOLDIER, HORSE));
	CHECK(Protocol::DecodeRides(Protocol::Encode(many))->rides.size() == Protocol::MAX_RIDES);

	CHECK(!Protocol::DecodeRides(Protocol::Encode(Protocol::Rides{ .rides = { NpcOn({ 0, 0 }, HORSE) } })));
	CHECK(!Protocol::DecodeRides(Protocol::Encode(Protocol::Rides{ .rides = { NpcOn(SOLDIER, { 0, 1 }) } })));
	// The writer leaves a player's NPC out; written by hand, a player's ride that names one is refused.
	auto withNpc = Protocol::Encode(Protocol::Rides{ .rides = { PlayerOn(1, HORSE) } });
	withNpc[1 + 4 + 1 + 1 + 4] = 0x01;
	CHECK(!Protocol::DecodeRides(withNpc));
	auto badOn = Protocol::Encode(Protocol::Rides{ .rides = { PlayerOn(1, HORSE) } });
	badOn.back() = 2;
	CHECK(!Protocol::DecodeRides(badOn));
	auto badFull = Protocol::Encode(Protocol::Rides{ .rides = { PlayerOn(1, HORSE) } });
	badFull[5] = 2;
	CHECK(!Protocol::DecodeRides(badFull));
	auto tooMany = Protocol::Encode(Protocol::Rides{});
	tooMany[6] = static_cast<std::uint8_t>(Protocol::MAX_RIDES + 1);
	CHECK(!Protocol::DecodeRides(tooMany));
	auto shortOne = Protocol::Encode(Protocol::Rides{ .rides = { PlayerOn(1, HORSE) } });
	shortOne.pop_back();
	CHECK(!Protocol::DecodeRides(shortOne));
	auto longOne = Protocol::Encode(Protocol::Rides{ .rides = { PlayerOn(1, HORSE) } });
	longOne.push_back(0);
	CHECK(!Protocol::DecodeRides(longOne));
	// A player on foot, a mount none: a ride that holds.
	CHECK(Protocol::RideHolds(PlayerOff(1)));
	CHECK(!Protocol::RideHolds(Ride{}));
}

TEST("ride form translate: an NPC rider the reader lacks is left out; a mount it lacks becomes none, the ride kept")
{
	const Edges            e;
	const Protocol::NpcKey ofB{ 0x03000ABC, 0 };      // B.esp's reference in the host's numbering
	const Protocol::NpcKey ofExtra{ 0x04000ABC, 0 };  // Extra.esp's: the guest lacks it
	const Protocol::NpcKey inGuest{ 0x01000ABC, 0 };
	const Protocol::NpcKey ofSkyrim{ 0x00013BBF, 0 };

	const auto rides = Protocol::DecodeRides(e.HostToGuest(Protocol::Encode(Protocol::Rides{ .sender = 1,
		.rides = { PlayerOn(1, ofB), PlayerOn(2, ofExtra), NpcOn(ofExtra, ofSkyrim), NpcOn(ofSkyrim, ofExtra), NpcOn(ofB, PLACED_HORSE_OF_1), NpcOff(ofSkyrim) } })));
	REQUIRE(rides);
	CHECK(rides->sender == 1u);
	CHECK(rides->rides == std::vector<Ride>({ PlayerOn(1, inGuest), PlayerOn(2, {}), NpcOn(ofSkyrim, {}), NpcOn(inGuest, PLACED_HORSE_OF_1), NpcOff(ofSkyrim) }));
}

// ---- the server's book ----

TEST("ride book: a player says only their own ride, an NPC's only its runner; each change once; an end of nothing is nothing")
{
	RideBook::Book book;
	CHECK(book.Take(1, { PlayerOn(1, HORSE) }, RunsNone()) == std::vector<Ride>({ PlayerOn(1, HORSE) }));
	CHECK(book.Take(1, { PlayerOn(1, HORSE) }, RunsNone()).empty());  // the same again
	CHECK(book.Take(2, { PlayerOn(1, OTHER_HORSE) }, RunsNone()).empty());  // not Bo's to say
	CHECK(book.Take(1, { PlayerOn(1, OTHER_HORSE) }, RunsNone()) == std::vector<Ride>({ PlayerOn(1, OTHER_HORSE) }));
	CHECK(book.Take(1, { NpcOn(SOLDIER, HORSE) }, RunsNone()).empty());  // Ada does not run the soldier
	CHECK(book.Take(2, { NpcOn(SOLDIER, HORSE) }, RunsSoldier()) == std::vector<Ride>({ NpcOn(SOLDIER, HORSE) }));
	CHECK(book.Take(0, { PlayerOn(0, HORSE) }, RunsNone()).empty());
	CHECK(book.Refused() == 3u);
	CHECK(book.Count() == 2u);
	CHECK(book.Take(2, { PlayerOff(2) }, RunsNone()).empty());  // Bo was riding nothing
	// An end is passed on as an end, whatever mount it names.
	CHECK(book.Take(1, { Ride{ .player = 1, .rider = {}, .mount = HORSE, .on = false } }, RunsNone()) == std::vector<Ride>({ PlayerOff(1) }));
	CHECK(book.All() == std::vector<Ride>({ NpcOn(SOLDIER, HORSE) }));
}

TEST("ride book: a player who leaves takes their ride and their NPCs' with them; owed changes go as their newest state")
{
	RideBook::Book book;
	(void)book.Take(1, { PlayerOn(1, HORSE), NpcOn(SOLDIER, OTHER_HORSE) }, RunsSoldier());
	(void)book.Take(2, { PlayerOn(2, PLACED_HORSE_OF_1) }, RunsNone());
	const auto ended = book.Leave(1);
	CHECK(ended.size() == 2u);
	CHECK(std::ranges::find(ended, PlayerOff(1)) != ended.end());
	CHECK(std::ranges::find(ended, NpcOff(SOLDIER)) != ended.end());
	CHECK(book.All() == std::vector<Ride>({ PlayerOn(2, PLACED_HORSE_OF_1) }));

	// Bo's change past the budget is owed; the newest state goes, an end as an end.
	book.Owe(2, PlayerOn(2, PLACED_HORSE_OF_1));
	CHECK(book.Owes(2));
	(void)book.Take(2, { PlayerOff(2) }, RunsNone());
	Ride next;
	CHECK(book.NextOwed(2, next));
	CHECK(next == PlayerOff(2));
	CHECK(!book.Owes(2));
	CHECK(!book.NextOwed(2, next));
	book.Owe(3, PlayerOn(3, HORSE));
	book.Clear();
	CHECK(!book.Owes(3));
	CHECK(book.Count() == 0u);
}

TEST("ride server: a ride goes to the others with its sender named, only from whoever may say it; a joiner hears them all; a leaver's end")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { SOLDIER, Protocol::ClaimWhy::kHave } } }));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// Bo's word of Ada's ride and of Ada's soldier is nobody's.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Rides{ .rides = { PlayerOn(1, HORSE), NpcOn(SOLDIER, HORSE) } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.fake->Take(2).empty());

	f.fake->Deliver(1, Protocol::Encode(Protocol::Rides{ .sender = 99, .rides = { PlayerOn(1, HORSE), NpcOn(SOLDIER, OTHER_HORSE), PlayerOn(2, HORSE) } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	const auto toBo = SentOf<Protocol::Rides>(f, 2, MT::kRides, Protocol::DecodeRides);
	REQUIRE(toBo.size() == 1u);
	CHECK(toBo[0].sender == 1u);
	CHECK(!toBo[0].full);
	CHECK(toBo[0].rides == std::vector<Ride>({ PlayerOn(1, HORSE), NpcOn(SOLDIER, OTHER_HORSE) }));

	// The same again is not passed on.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Rides{ .rides = { PlayerOn(1, HORSE) } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(2).empty());

	// Cy joins: every ride, as a full list.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto toCy = SentOf<Protocol::Rides>(f, 3, MT::kRides, Protocol::DecodeRides);
	REQUIRE(toCy.size() == 1u);
	CHECK(toCy[0].full);
	CHECK(toCy[0].rides.size() == 2u);
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// Ada leaves: Bo and Cy get her and her soldier off.
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto ends = SentOf<Protocol::Rides>(f, peer, MT::kRides, Protocol::DecodeRides);
		REQUIRE(ends.size() == 1u);
		CHECK(ends[0].rides.size() == 2u);
		CHECK(std::ranges::find(ends[0].rides, PlayerOff(1)) != ends[0].rides.end());
		CHECK(std::ranges::find(ends[0].rides, NpcOff(SOLDIER)) != ends[0].rides.end());
	}
}

// ---- seating a rider ----

TEST("ride seat: the mount activated, then the console, then the quick put-on, in rounds 5 s apart; given up after 3")
{
	RideRules::Seat      seat;
	auto                 t = Steady::now();
	RideRules::SeatFacts facts{ .wanted = true, .ready = false };
	CHECK(seat.Step(facts, t) == Act::kNone);  // not ready: rider or mount still loading
	facts.ready = true;
	facts.busy = true;
	CHECK(seat.Step(facts, t) == Act::kNone);  // the engine is getting it on or off
	facts.busy = false;
	for (int round = 0; round < RideRules::MAX_ROUNDS; ++round) {
		CHECK(seat.Step(facts, t) == Act::kActivate);
		CHECK(seat.Step(facts, t + 1s) == Act::kNone);
		CHECK(seat.Step(facts, t + RideRules::CONSOLE_AFTER) == Act::kConsole);
		CHECK(seat.Step(facts, t + 3s) == Act::kNone);
		CHECK(seat.Step(facts, t + RideRules::QUICK_AFTER) == Act::kQuick);
		CHECK(seat.Step(facts, t + RideRules::ROUND_LENGTH) == Act::kNone);
		CHECK(seat.Rounds() == round + 1);
		CHECK(seat.Step(facts, t + RideRules::ROUND_LENGTH + 1s) == Act::kNone);  // the gap between rounds
		t += RideRules::ROUND_LENGTH + RideRules::ROUND_GAP;
	}
	CHECK(seat.GivenUp());
	CHECK(seat.Step(facts, t + 1h) == Act::kNone);
	CHECK(!seat.Seated());
	// Its player gets off and on again: tried afresh.
	CHECK(seat.Step(RideRules::SeatFacts{}, t) == Act::kNone);
	CHECK(!seat.GivenUp());
	CHECK(seat.Step(facts, t) == Act::kActivate);
}

TEST("ride seat: seated once the engine has it on its mount, by whichever try it was; off when it should not ride, or rides another")
{
	RideRules::Seat      seat;
	const auto           t = Steady::now();
	RideRules::SeatFacts facts{ .wanted = true, .ready = true };
	CHECK(seat.Step(facts, t) == Act::kActivate);
	CHECK(seat.Step(facts, t + RideRules::CONSOLE_AFTER) == Act::kConsole);
	facts.onWanted = true;
	CHECK(seat.Step(facts, t + 3s) == Act::kNone);
	CHECK(seat.Seated());
	CHECK(seat.SeatedBy() == Act::kConsole);
	CHECK(seat.Rounds() == 0);
	// Its player got off: it gets off, once a second at most, not while the engine is busy with it.
	facts.wanted = false;
	CHECK(seat.Step(facts, t + 4s) == Act::kDismount);
	CHECK(!seat.Seated());
	CHECK(seat.Step(facts, t + 4s + 500ms) == Act::kNone);
	facts.busy = true;
	CHECK(seat.Step(facts, t + 6s) == Act::kNone);
	facts.busy = false;
	CHECK(seat.Step(facts, t + 6s) == Act::kDismount);
	CHECK(seat.Dismounts() == 2u);
	// On another mount than its player's: off it first, then on the right one.
	facts = { .wanted = true, .ready = true, .onWanted = false, .onOther = true };
	CHECK(seat.Step(facts, t + 8s) == Act::kDismount);
	facts.onOther = false;
	CHECK(seat.Step(facts, t + 8s + 100ms) == Act::kActivate);
	// Already on when first looked at (a joiner's list, a world loaded): seated, nothing tried.
	RideRules::Seat already;
	CHECK(already.Step({ .wanted = true, .ready = true, .onWanted = true }, t) == Act::kNone);
	CHECK(already.Seated());
	CHECK(already.SeatedBy() == Act::kNone);
}

TEST("ride saddle: a seated rider far from its mount for a second is put back on it; near, or not long, it is not")
{
	RideRules::SaddleWatch watch;
	const auto             t = Steady::now();
	CHECK(!watch.Step(20.0f, t));
	CHECK(!watch.Step(RideRules::SADDLE_FAR + 1.0f, t));
	CHECK(!watch.Step(RideRules::SADDLE_FAR + 1.0f, t + 500ms));
	CHECK(watch.Step(RideRules::SADDLE_FAR + 1.0f, t + RideRules::SADDLE_HOLD_AFTER));
	CHECK(!watch.Step(10.0f, t + 2s));
	CHECK(!watch.Step(RideRules::SADDLE_FAR + 1.0f, t + 2s));  // far again: the second starts over
	CHECK(watch.Holds() == 1u);
	CHECK(!watch.Step(-1.0f, t + 5s));  // not seated
}

// ---- what a game says of its own riders ----

TEST("ride said: each ride of ours that begins or changes mount is said once, and the end of each said before")
{
	using Rider = RideRules::Said::Rider;
	RideRules::Said said;
	const Rider     us{ 1, {} };
	const Rider     soldier{ 0, SOLDIER };
	CHECK(said.Changes({ { us, {} } }).empty());  // on foot: nothing to say
	CHECK(said.Changes({ { us, HORSE } }) == std::vector<Ride>({ PlayerOn(1, HORSE) }));
	CHECK(said.Changes({ { us, HORSE } }).empty());
	CHECK(said.Changes({ { us, OTHER_HORSE }, { soldier, PLACED_HORSE_OF_1 } }) == std::vector<Ride>({ NpcOn(SOLDIER, PLACED_HORSE_OF_1), PlayerOn(1, OTHER_HORSE) }));
	// Our player got off; the soldier is no longer ours (or got off): both end.
	CHECK(said.Changes({ { us, {} } }) == std::vector<Ride>({ NpcOff(SOLDIER), PlayerOff(1) }));
	CHECK(said.Count() == 0u);
	CHECK(said.Changes({}).empty());
}

TEST("ride believed: a player's ride only from that player, an NPC's only from its runner; a joiner's list from the server")
{
	CHECK(RideRules::Believed(PlayerOn(2, HORSE), 2, 0));
	CHECK(!RideRules::Believed(PlayerOn(2, HORSE), 3, 0));
	CHECK(RideRules::Believed(PlayerOn(2, HORSE), 0, 0));
	CHECK(RideRules::Believed(NpcOn(SOLDIER, HORSE), 2, 2));
	CHECK(!RideRules::Believed(NpcOn(SOLDIER, HORSE), 2, 3));
	CHECK(!RideRules::Believed(NpcOn(SOLDIER, HORSE), 2, 0));
	CHECK(RideRules::Believed(NpcOn(SOLDIER, HORSE), 0, 0));
}

// ---- the mount and the rider's graph ----

TEST("ride events: getting on and off and riding are each game's own, never played again on a stand-in or a copy")
{
	for (const auto name : { "HorseEnter", "horseExit", "HORSELOCOMOTION", "MountedCombatStart", "dismount", "DragonRidingStart", "DragonMount" }) {
		CHECK(NpcRules::MountEvent(name));
		CHECK(!NpcRules::Replayed(name));
	}
	for (const auto name : { "attackStart", "bowAttackStart", "FlyStartCruise", "BeginCastRight", "shoutStart" }) {
		CHECK(!NpcRules::MountEvent(name));
		CHECK(NpcRules::Replayed(name));
	}
}

TEST("ride claims: a mount our rider sits on is asked for at the claim rate; one another game's NPC rides is let go of")
{
	using NpcRules::Ask;
	using NpcRules::Owner;
	const auto t = Steady::now();

	NpcRules::Facts theirs{ .owner = Owner::kOther, .followsUs = true, .ridden = true };
	theirs.lastClaim = t - NpcRules::CLAIM_EVERY;
	CHECK(NpcRules::Decide(theirs, t) == Ask::kFollower);
	theirs.lastClaim = t - 500ms;
	CHECK(NpcRules::Decide(theirs, t) == Ask::kNone);
	// A follower that is no mount of ours waits the follower's time.
	NpcRules::Facts follower{ .owner = Owner::kOther, .followsUs = true };
	follower.lastClaim = t - NpcRules::CLAIM_EVERY;
	CHECK(NpcRules::Decide(follower, t) == Ask::kNone);

	NpcRules::Facts ours{ .owner = Owner::kUs, .riddenThere = true };
	CHECK(NpcRules::Decide(ours, t) == Ask::kRelease);
	ours.lastClaim = t - 500ms;
	CHECK(NpcRules::Decide(ours, t) == Ask::kNone);
	// Ours and ridden by ours (two riders' word at once): kept, as our follower.
	NpcRules::Facts both{ .owner = Owner::kUs, .followsUs = true, .ridden = true, .riddenThere = true };
	CHECK(NpcRules::Decide(both, t) == Ask::kFollower);
	// A spawn of ours is never let go of.
	NpcRules::Facts spawn{ .owner = Owner::kUs, .spawn = true, .riddenThere = true };
	CHECK(NpcRules::Decide(spawn, t) == Ask::kNone);
}

TEST("ride share: a horse made as the game ran is shared as our spawn while ours ride it or it is our player's last")
{
	using NpcRules::Commander;
	CHECK(NpcRules::SharedRuntime(Commander::kNobody, true));
	CHECK(!NpcRules::SharedRuntime(Commander::kNobody, false));
	CHECK(!NpcRules::SharedRuntime(Commander::kOther, false));
	CHECK(NpcRules::SharedRuntime(Commander::kOurPlayer, false));
	CHECK(NpcRules::SharedRuntime(Commander::kOurNpc, false));
}

TEST("ride acts: each try has a name for the developer channel")
{
	for (const auto act : { Act::kNone, Act::kActivate, Act::kConsole, Act::kQuick, Act::kDismount }) {
		CHECK(RideRules::ActName(act) != "?");
	}
}
