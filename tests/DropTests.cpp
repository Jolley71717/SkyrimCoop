#include "Test.h"

#include "DropStore.h"
#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/DropRules.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Items given by dropping them (docs/DESIGN.md, "Dropped items"): the messages (common/Protocol.h), their form IDs on the
// wire (common/FormTranslate.h), the server's book (common/DropStore.h) and its handling (common/Server.cpp), and each
// game's rules (src/game/DropRules.h). What reads and changes the game itself (src/game/DropSync.cpp) is tested in a game
// only: tools/rig/steps/drops.sh.

using Protocol::ClaimDrop;
using Protocol::DropClaim;
using Protocol::DropGone;
using Protocol::DropList;
using Protocol::DroppedItem;
using Protocol::DropRecord;
using namespace DropRules;

namespace
{
	using MT = Protocol::MessageType;
	using namespace std::chrono_literals;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	// An iron dagger at rest in an interior cell, as its dropper's game reports it.
	DroppedItem Item(std::uint64_t a_nonce, std::uint32_t a_base = 0x1397E, std::uint32_t a_count = 1)
	{
		DroppedItem item;
		item.nonce = a_nonce;
		item.base = a_base;
		item.count = a_count;
		item.cell = 0x0001A332;
		item.x = 120.0f;
		item.y = -40.5f;
		item.z = 8.0f;
		item.angle = 1.25f;
		return item;
	}

	// A tempered, enchanted, poisoned, renamed one, outdoors.
	DroppedItem Fancy(std::uint64_t a_nonce)
	{
		auto item = Item(a_nonce, 0x0001359D);
		item.health = 1.6f;
		item.enchantment = 0x0004605A;
		item.enchantmentMax = 800;
		item.charge = 412.5f;
		item.poison = 0x0003A5A4;
		item.poisonCount = 3;
		item.name = "Old Friend";
		item.cell = 0;
		item.worldspace = 0x3C;
		item.x = 20000.0f;
		item.y = -60000.0f;
		item.z = -3000.0f;
		return item;
	}

	DroppedItem Numbered(DroppedItem a_item, std::uint32_t a_id, std::uint32_t a_owner)
	{
		a_item.dropId = a_id;
		a_item.ownerId = a_owner;
		return a_item;
	}

	std::vector<DroppedItem> PlacedIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<DroppedItem> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kDropPlaced) {
				if (const auto drop = Protocol::DecodeDroppedItem(packet)) {
					out.push_back(*drop);
				}
			}
		}
		return out;
	}

	std::vector<DropGone> GoneIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<DropGone> out;
		for (const auto& packet : a_packets) {
			if (const auto gone = Protocol::DecodeDropGone(packet)) {
				out.push_back(*gone);
			}
		}
		return out;
	}

	std::vector<DropList> ListsIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<DropList> out;
		for (const auto& packet : a_packets) {
			if (const auto list = Protocol::DecodeDropList(packet)) {
				out.push_back(*list);
			}
		}
		return out;
	}

	std::vector<std::uint8_t> Claim(std::uint32_t a_id, DropClaim a_op) { return Protocol::Encode(ClaimDrop{ a_id, a_op }); }

	const Clock::time_point T0 = Clock::time_point{} + 1h;
}

// ---- the messages -------------------------------------------------------------------------------------------------

TEST("drops: a dropped item goes there and back as it was, as a report and as placed; its extras and its place with it")
{
	CHECK(Protocol::VERSION >= 11);
	for (const auto type : { MT::kReportDrop, MT::kDropPlaced }) {
		for (const auto& item : { Numbered(Item(7, 0x1397E, 25), 3, 2), Numbered(Fancy(0xFFFFFFFFFFFFFFFFull), 9, 1) }) {
			const auto packet = Protocol::Encode(item, type);
			CHECK(Protocol::PeekType(packet) == type);
			const auto back = Protocol::DecodeDroppedItem(packet);
			REQUIRE(back);
			CHECK(*back == item);
		}
	}
	auto soul = Item(5, 0x0002E4E2);
	soul.soul = Protocol::MAX_SOUL;
	CHECK(Protocol::DecodeDroppedItem(Protocol::Encode(soul, MT::kReportDrop)) == soul);
	// The server's own field never goes on the wire.
	auto taken = Numbered(Item(5), 1, 1);
	taken.takerId = 4;
	CHECK(Protocol::DecodeDroppedItem(Protocol::Encode(taken, MT::kDropPlaced))->takerId == 0);
	// Not a drop's type.
	CHECK(!Protocol::DecodeDroppedItem(Protocol::Encode(ClaimDrop{ 1, DropClaim::kTake })));
}

TEST("drops: an item no game could place is none: no nonce, no count or too many, a soul past the grand, a bad amount, out of the world")
{
	const auto refused = [](auto a_change) {
		auto item = Item(1);
		a_change(item);
		return !Protocol::DecodeDroppedItem(Protocol::Encode(item, MT::kReportDrop));
	};
	CHECK(refused([](DroppedItem& a_i) { a_i.nonce = 0; }));
	CHECK(refused([](DroppedItem& a_i) { a_i.count = 0; }));
	CHECK(refused([](DroppedItem& a_i) { a_i.soul = Protocol::MAX_SOUL + 1; }));
	CHECK(refused([](DroppedItem& a_i) { a_i.health = -2.0f; }));
	CHECK(refused([](DroppedItem& a_i) { a_i.charge = std::numeric_limits<float>::quiet_NaN(); }));
	CHECK(refused([](DroppedItem& a_i) { a_i.x = 1.0e9f; }));
	CHECK(refused([](DroppedItem& a_i) { a_i.angle = std::numeric_limits<float>::infinity(); }));
	CHECK(refused([](DroppedItem& a_i) { a_i.angle = 500.0f; }));
	// A count past the most is written as the most; a base of 0 is read (a game that lacks the item).
	auto many = Item(1);
	many.count = Protocol::MAX_DROP_COUNT + 10;
	CHECK(Protocol::DecodeDroppedItem(Protocol::Encode(many, MT::kReportDrop))->count == Protocol::MAX_DROP_COUNT);
	CHECK(!refused([](DroppedItem& a_i) { a_i.base = 0; }));
	// Cut short, or a byte too many.
	auto packet = Protocol::Encode(Item(1), MT::kReportDrop);
	packet.pop_back();
	CHECK(!Protocol::DecodeDroppedItem(packet));
	packet = Protocol::Encode(Item(1), MT::kReportDrop);
	packet.push_back(0);
	CHECK(!Protocol::DecodeDroppedItem(packet));
}

TEST("drops: claims, gone and records go there and back; a claim of no drop or no known kind, and gone of no drop, are none")
{
	for (const auto op : { DropClaim::kTake, DropClaim::kGiveBack, DropClaim::kPlaced, DropClaim::kKept }) {
		CHECK(Protocol::DecodeClaimDrop(Protocol::Encode(ClaimDrop{ 77, op })) == ClaimDrop{ 77, op });
	}
	CHECK(!Protocol::DecodeClaimDrop(Protocol::Encode(ClaimDrop{ 0, DropClaim::kTake })));
	auto bad = Protocol::Encode(ClaimDrop{ 3, DropClaim::kTake });
	bad.back() = 4;
	CHECK(!Protocol::DecodeClaimDrop(bad));

	CHECK(Protocol::DecodeDropGone(Protocol::Encode(DropGone{ 5, 2 })) == DropGone{ 5, 2 });
	CHECK(Protocol::DecodeDropGone(Protocol::Encode(DropGone{ 5, 0 })) == DropGone{ 5, 0 });
	CHECK(!Protocol::DecodeDropGone(Protocol::Encode(DropGone{ 0, 2 })));

	const DropRecord record{ .sessionId = 0x1122334455667788ull, .made = { 1, 0xFFFFFFFFFFFFFFFFull }, .taken = { 4, 9, 12 } };
	CHECK(Protocol::DecodeDropRecord(Protocol::Encode(record)) == record);
	CHECK(Protocol::DecodeDropRecord(Protocol::Encode(DropRecord{})) == DropRecord{});
	// Past the most, the newest are kept.
	DropRecord big;
	for (std::uint32_t i = 1; i <= Protocol::MAX_DROP_RECORD + 5; ++i) {
		big.made.push_back(i);
		big.taken.push_back(i);
	}
	const auto back = Protocol::DecodeDropRecord(Protocol::Encode(big));
	REQUIRE(back);
	REQUIRE(back->made.size() == Protocol::MAX_DROP_RECORD);
	CHECK(back->made.front() == 6);
	CHECK(back->taken.back() == Protocol::MAX_DROP_RECORD + 5);
}

TEST("drops: a list goes in packets of MAX_DROPS_PER_PACKET, the last marked; none is one empty last packet; a drop with no number is none")
{
	CHECK(ListsIn(Protocol::DropLists({})).size() == 1);
	CHECK(ListsIn(Protocol::DropLists({})).front() == DropList{ true, {} });
	std::vector<DroppedItem> drops;
	for (std::uint32_t i = 1; i <= Protocol::MAX_DROPS_PER_PACKET + 1; ++i) {
		drops.push_back(Numbered(Item(i), i, 1));
	}
	auto lists = ListsIn(Protocol::DropLists(drops));
	REQUIRE(lists.size() == 2);
	CHECK(!lists[0].last);
	CHECK(lists[0].drops.size() == Protocol::MAX_DROPS_PER_PACKET);
	CHECK(lists[1].last);
	REQUIRE(lists[1].drops.size() == 1);
	CHECK(lists[1].drops[0] == drops.back());
	drops.pop_back();
	lists = ListsIn(Protocol::DropLists(drops));
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].last);
	CHECK(!Protocol::DecodeDropList(Protocol::Encode(DropList{ true, { Item(1) } })));  // no drop ID
}

TEST("drops: the item, its enchantment, its poison and its place are named by their plugin on the wire; one a game lacks is base 0, a place it lacks is nowhere")
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
	// The drop a packet is once through an edge (as it was when nothing changed).
	const auto through = [](const FormIds::Mapper& a_edge, Way a_way, const std::vector<std::uint8_t>& a_packet) -> std::optional<DroppedItem> {
		const auto out = FormTranslate::Translate(a_edge, a_way, a_packet);
		switch (out.kind) {
		case Result::Kind::kSame:
			return Protocol::DecodeDroppedItem(a_packet);
		case Result::Kind::kChanged:
			return Protocol::DecodeDroppedItem(out.packet);
		case Result::Kind::kDrop:
			break;
		}
		return std::nullopt;
	};

	CHECK(FormTranslate::CarriesForms(MT::kReportDrop));
	CHECK(FormTranslate::CarriesForms(MT::kDropPlaced));
	CHECK(FormTranslate::CarriesForms(MT::kDropList));
	CHECK(!FormTranslate::CarriesForms(MT::kClaimDrop));
	CHECK(!FormTranslate::CarriesForms(MT::kDropGone));
	CHECK(!FormTranslate::CarriesForms(MT::kDropRecord));

	// The guest drops a sword of B.esp (its 01), enchanted by A.esp's enchantment (its 03), in a cell of B.esp.
	auto sword = Item(11, 0x01000800);
	sword.enchantment = 0x03000801;
	sword.poison = 0x0003A5A4;
	sword.cell = 0x01000D62;
	const auto out = through(guestEdge, Way::kToSession, Protocol::Encode(sword, MT::kReportDrop));
	REQUIRE(out);
	CHECK(out->base == 0x03000800);         // B.esp is the session's 03
	CHECK(out->enchantment == 0x02000801);  // A.esp its 02
	CHECK(out->poison == 0x0003A5A4);
	CHECK(out->cell == 0x03000D62);
	CHECK(out->nonce == 11);
	const auto atHost = through(hostEdge, Way::kToLocal, Protocol::Encode(Numbered(*out, 4, 2), MT::kDropPlaced));
	REQUIRE(atHost);
	CHECK(atHost->base == 0x03000800);
	CHECK(atHost->enchantment == 0x02000801);
	CHECK(atHost->cell == 0x03000D62);
	CHECK(atHost->dropId == 4);

	// An item of Extra.esp, or one enchanted by it: the host lacks it, so its base is 0 there (no copy), the drop still
	// known (it hears who takes it).
	const auto extra = through(guestEdge, Way::kToSession, Protocol::Encode(Item(12, 0x04000800), MT::kReportDrop));
	REQUIRE(extra);
	CHECK(extra->base == 0x04000800);
	auto heard = through(hostEdge, Way::kToLocal, Protocol::Encode(Numbered(*extra, 5, 2), MT::kDropPlaced));
	REQUIRE(heard);
	CHECK(heard->base == 0);
	CHECK(heard->dropId == 5);
	auto charmed = Item(13, 0x01000800);
	charmed.enchantment = 0x04000801;
	const auto charmedOut = through(guestEdge, Way::kToSession, Protocol::Encode(charmed, MT::kReportDrop));
	REQUIRE(charmedOut);
	CHECK(charmedOut->base == 0x03000800);
	heard = through(hostEdge, Way::kToLocal, Protocol::Encode(Numbered(*charmedOut, 6, 2), MT::kDropPlaced));
	REQUIRE(heard);
	CHECK(heard->base == 0);

	// A runtime base, enchantment or poison has no ID anywhere: base 0 on its way out (the server refuses it).
	for (const auto& runtime : { Item(14, 0xFF000123), [] {
			 auto i = Item(15);
			 i.poison = 0xFF000124;
			 return i;
		 }() }) {
		const auto told = through(guestEdge, Way::kToSession, Protocol::Encode(runtime, MT::kReportDrop));
		REQUIRE(told);
		CHECK(told->base == 0);
	}

	// A cell the host lacks: nowhere (placed by none).
	auto lost = *out;
	lost.cell = 0x04000D62;
	const auto nowhere = through(hostEdge, Way::kToLocal, Protocol::Encode(Numbered(lost, 7, 2), MT::kDropPlaced));
	REQUIRE(nowhere);
	CHECK(nowhere->cell == 0);
	CHECK(nowhere->worldspace == 0);

	// A list keeps every drop, each mapped.
	const auto list = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(DropList{ true, { Numbered(lost, 7, 2), Numbered(*extra, 8, 2) } }));
	REQUIRE(list.kind == Result::Kind::kChanged);
	const auto listed = Protocol::DecodeDropList(list.packet);
	REQUIRE(listed);
	REQUIRE(listed->drops.size() == 2);
	CHECK(listed->drops[0].cell == 0);
	CHECK(listed->drops[0].base == 0x03000800);
	CHECK(listed->drops[1].base == 0);
	CHECK(listed->drops[1].dropId == 8);
	CHECK(listed->last);
}

// ---- the server's book --------------------------------------------------------------------------------------------

TEST("drop store: a drop is an offer to its dropper until they confirm it; then it lies there, and the first to take it has it")
{
	DropStore store;
	const auto added = store.Add(1, Item(50));
	REQUIRE(added.outcome == DropStore::Outcome::kAdded);
	REQUIRE(added.drop);
	const auto id = added.drop->dropId;
	CHECK(id == 1);
	CHECK(added.drop->ownerId == 1);
	CHECK(store.OfferCount() == 1);
	CHECK(store.Lying().empty());
	CHECK(!store.Take(2, id));          // an offer is never taken
	CHECK(!store.Confirm(2, id));       // nor confirmed by another
	const auto lying = store.Confirm(1, id);
	REQUIRE(lying);
	CHECK(lying->dropId == id);
	CHECK(store.Lying().size() == 1);
	CHECK(store.Take(2, id) == DropGone{ id, 2 });
	CHECK(!store.Take(1, id));          // too late
	CHECK(store.TakerOf(id) == 2);
	CHECK(store.Lying().empty());
	CHECK(!store.GiveBack(1, id));      // not theirs to give back
	const auto again = store.GiveBack(2, id);
	REQUIRE(again);
	CHECK(again->takerId == 0);
	CHECK(store.Find(id));
	CHECK(store.Take(1, id) == DropGone{ id, 1 });
}

TEST("drop store: the same report again is the one kept; a drop picked up again is no offer; none with no nonce, base, owner or place")
{
	DropStore store;
	const auto first = store.Add(1, Item(5));
	const auto repeat = store.Add(1, Item(5));
	CHECK(repeat.outcome == DropStore::Outcome::kRepeat);
	REQUIRE(repeat.drop);
	CHECK(repeat.drop->dropId == first.drop->dropId);
	CHECK(!repeat.lying);
	CHECK(store.Add(2, Item(5)).outcome == DropStore::Outcome::kAdded);  // another player's own number
	CHECK(store.Keep(1, first.drop->dropId));
	CHECK(!store.Keep(1, first.drop->dropId));
	CHECK(store.OfferCount() == 1);
	auto nowhere = Item(6);
	nowhere.cell = 0;
	CHECK(store.Add(1, nowhere).outcome == DropStore::Outcome::kInvalid);
	CHECK(store.Add(1, Item(0)).outcome == DropStore::Outcome::kInvalid);
	CHECK(store.Add(1, Item(7, 0)).outcome == DropStore::Outcome::kInvalid);
	CHECK(store.Add(0, Item(7)).outcome == DropStore::Outcome::kInvalid);
	// Lying already, or taken: a repeat, with nothing to answer for a taken one.
	const auto second = store.Add(1, Item(8));
	(void)store.Confirm(1, second.drop->dropId);
	CHECK(store.Add(1, Item(8)).lying);
	(void)store.Take(2, second.drop->dropId);
	const auto gone = store.Add(1, Item(8));
	CHECK(gone.outcome == DropStore::Outcome::kRepeat);
	CHECK(!gone.drop);
}

TEST("drop store: a session holds at most Limits::lying, a player Limits::perOwner; past those a drop is refused, never an old one dropped")
{
	DropStore store({ .lying = 4, .taken = 2, .perOwner = 3 });
	for (std::uint64_t i = 1; i <= 3; ++i) {
		CHECK(store.Add(1, Item(i)).outcome == DropStore::Outcome::kAdded);
	}
	CHECK(store.Add(1, Item(4)).outcome == DropStore::Outcome::kOwnerFull);
	CHECK(store.Add(2, Item(4)).outcome == DropStore::Outcome::kAdded);
	CHECK(store.Add(2, Item(5)).outcome == DropStore::Outcome::kFull);
	// Taken ones are remembered up to Limits::taken, the oldest going first.
	for (std::uint32_t id = 1; id <= 4; ++id) {
		(void)store.Confirm(id == 4 ? 2 : 1, id);
		CHECK(store.Take(3, id));
	}
	CHECK(store.TakenCount() == 2);
	CHECK(store.TakerOf(1) == 0);
	CHECK(store.TakerOf(4) == 3);
}


TEST("drop store: a player who leaves takes their offers and their drops lying with them; others' drops, and what they took, stay")
{
	DropStore  store;
	const auto offer = store.Add(1, Item(1)).drop->dropId;
	const auto mine = store.Add(1, Item(2)).drop->dropId;
	const auto theirs = store.Add(2, Item(3)).drop->dropId;
	const auto taken = store.Add(2, Item(4)).drop->dropId;
	(void)store.Confirm(1, mine);
	(void)store.Confirm(2, theirs);
	(void)store.Confirm(2, taken);
	(void)store.Take(1, taken);
	const auto gone = store.Left(1);
	REQUIRE(gone.size() == 1);
	CHECK(gone[0] == DropGone{ mine, 0 });
	CHECK(store.OfferCount() == 0);
	CHECK(!store.Find(offer));
	CHECK(store.Find(theirs));
	CHECK(store.TakerOf(taken) == 1);
	CHECK(store.TakerOf(mine) == 1);  // back with them: their record says so when they come back
	// They come back with a world that never had it back (their game crashed first): it lies there again.
	const auto back = store.Reconcile(1, DropRecord{ .sessionId = 9, .made = { 2 }, .taken = { taken } }, 9);
	REQUIRE(back.returned.size() == 1);
	CHECK(back.returned[0].dropId == mine);
	CHECK(store.Find(mine));
	CHECK(store.TakerOf(taken) == 1);
}

TEST("drop store: a record makes the book agree with a world: what it holds is its player's, what the book had them hold but it lacks lies there again")
{
	constexpr std::uint64_t S = 77;
	DropStore               store;
	const auto              id = store.Add(1, Item(10)).drop->dropId;
	(void)store.Confirm(1, id);
	// The world after the drop: nothing changes.
	auto result = store.Reconcile(1, DropRecord{ S, { 10 }, {} }, S);
	CHECK(result.withdrawn.empty());
	CHECK(result.returned.empty());
	CHECK(store.Find(id));
	// A save from before the drop: the item is in that world again, so it lies nowhere.
	result = store.Reconcile(1, DropRecord{ S, {}, {} }, S);
	REQUIRE(result.withdrawn.size() == 1);
	CHECK(result.withdrawn[0] == DropGone{ id, 0 });
	CHECK(!store.Find(id));
	// The later save again: it lies there again.
	result = store.Reconcile(1, DropRecord{ S, { 10 }, {} }, S);
	REQUIRE(result.returned.size() == 1);
	CHECK(store.Find(id));
	// Bo takes it, then loads a save from before taking it: it lies there again; from after, it is theirs.
	(void)store.Take(2, id);
	result = store.Reconcile(2, DropRecord{ S, {}, {} }, S);
	REQUIRE(result.returned.size() == 1);
	result = store.Reconcile(2, DropRecord{ S, {}, { id } }, S);
	REQUIRE(result.withdrawn.size() == 1);
	CHECK(result.withdrawn[0] == DropGone{ id, 2 });
	CHECK(store.TakerOf(id) == 2);
	// Ada loads a save from before dropping it: both worlds have it (counted), and the book leaves it with Bo.
	result = store.Reconcile(1, DropRecord{ S, {}, {} }, S);
	CHECK(result.dupRisk == 1);
	CHECK(store.TakerOf(id) == 2);
	// A record of another session (or none) holds none of this one's transfers: Ada's own drop is in that world.
	const auto other = store.Add(1, Item(11)).drop->dropId;
	(void)store.Confirm(1, other);
	result = store.Reconcile(1, DropRecord{ S + 1, { 11 }, {} }, S);
	REQUIRE(result.withdrawn.size() == 1);
	CHECK(result.withdrawn[0].dropId == other);
}

TEST("drop store: a record settles a player's offers: one their world took out lies there now, one it still has goes, to them alone")
{
	constexpr std::uint64_t S = 5;
	DropStore               store;
	const auto              out = store.Add(1, Item(20)).drop->dropId;
	const auto              in = store.Add(1, Item(21)).drop->dropId;
	const auto              bo = store.Add(2, Item(22)).drop->dropId;
	const auto              result = store.Reconcile(1, DropRecord{ S, { 20 }, {} }, S);
	REQUIRE(result.confirmed.size() == 1);
	CHECK(result.confirmed[0].dropId == out);
	REQUIRE(result.unoffered.size() == 1);
	CHECK(result.unoffered[0] == DropGone{ in, 0 });
	CHECK(store.Find(out));
	CHECK(!store.Find(in));
	CHECK(store.OfferCount() == 1);  // Bo's: not Ada's to settle
	CHECK(!store.Find(bo));
}

TEST("drop store: a new session empties the book, and its numbers go on from where they were")
{
	DropStore  store;
	const auto first = store.Add(1, Item(1)).drop->dropId;
	(void)store.Confirm(1, first);
	store.Clear();
	CHECK(store.Lying().empty());
	CHECK(store.TakerOf(first) == 0);
	CHECK(store.Add(1, Item(1)).drop->dropId == first + 1);
}

// ---- the server ---------------------------------------------------------------------------------------------------

TEST("server: a drop is offered to its dropper alone, lies there for all once confirmed, and two takes at once give it to one")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	f.fake->Deliver(1, Protocol::Encode(Item(31, 0x1397E, 3), MT::kReportDrop));
	const auto offer = PlacedIn(f.fake->TakeThrough(1, Type(MT::kDropPlaced)));
	REQUIRE(offer.size() == 1);
	const auto id = offer[0].dropId;
	CHECK(id != 0);
	CHECK(offer[0].ownerId == ada->playerId);
	CHECK(offer[0].nonce == 31);
	CHECK(offer[0].count == 3);
	REQUIRE(f.fake->Settle());
	CHECK(PlacedIn(f.fake->Take(2)).empty());

	f.fake->Deliver(1, Claim(id, DropClaim::kPlaced));
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 2 } }) {
		const auto placed = PlacedIn(f.fake->TakeThrough(peer, Type(MT::kDropPlaced)));
		REQUIRE(placed.size() == 1);
		CHECK(placed[0].dropId == id);
		CHECK(placed[0].x == offer[0].x);
	}

	f.fake->Deliver(2, Claim(id, DropClaim::kTake));
	f.fake->Deliver(1, Claim(id, DropClaim::kTake));
	REQUIRE(f.fake->Settle());
	const auto toAda = GoneIn(f.fake->Take(1));
	const auto toBo = GoneIn(f.fake->Take(2));
	REQUIRE(toBo.size() == 1);
	CHECK(toBo[0] == DropGone{ id, bo->playerId });
	REQUIRE(toAda.size() == 2);  // everyone's, and the answer to Ada's own claim
	for (const auto& gone : toAda) {
		CHECK(gone == DropGone{ id, bo->playerId });
	}

	// Bo's game could not finish it: it lies there again for anyone.
	f.fake->Deliver(2, Claim(id, DropClaim::kGiveBack));
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 2 } }) {
		CHECK(PlacedIn(f.fake->TakeThrough(peer, Type(MT::kDropPlaced))).size() == 1);
	}

	// Ada leaves with it lying there: it goes with her (her game puts it back in her world).
	f.fake->Drop(1);
	const auto gone = GoneIn(f.fake->TakeThrough(2, Type(MT::kDropGone)));
	REQUIRE(gone.size() == 1);
	CHECK(gone[0] == DropGone{ id, 0 });
	CHECK(f.LoggedSoon("went with them"));
}

TEST("server: a record is answered with what lies there; a world that has its drop again takes it back from everyone")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	CHECK(ada->sessionId == bo->sessionId);
	f.fake->Deliver(1, Protocol::Encode(Item(41), MT::kReportDrop));
	const auto id = PlacedIn(f.fake->TakeThrough(1, Type(MT::kDropPlaced))).at(0).dropId;
	f.fake->Deliver(1, Claim(id, DropClaim::kPlaced));
	(void)f.fake->TakeThrough(1, Type(MT::kDropPlaced));
	(void)f.fake->TakeThrough(2, Type(MT::kDropPlaced));

	f.fake->Deliver(2, Protocol::Encode(DropRecord{ bo->sessionId, {}, {} }));
	const auto lists = ListsIn(f.fake->TakeThrough(2, Type(MT::kDropList)));
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].last);
	REQUIRE(lists[0].drops.size() == 1);
	CHECK(lists[0].drops[0].dropId == id);

	// Ada loads a save from before the drop.
	f.fake->Deliver(1, Protocol::Encode(DropRecord{ ada->sessionId, {}, {} }));
	const auto packets = f.fake->TakeThrough(1, Type(MT::kDropList));
	CHECK(GoneIn(packets) == std::vector<DropGone>{ DropGone{ id, 0 } });
	REQUIRE(ListsIn(packets).size() == 1);
	CHECK(ListsIn(packets)[0].drops.empty());
	CHECK(GoneIn(f.fake->TakeThrough(2, Type(MT::kDropGone))) == std::vector<DropGone>{ DropGone{ id, 0 } });
}

TEST("server: a drop it can't take is answered with no number, its dropper's own still: one it can't name, and one past the budget")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	f.fake->Deliver(1, Protocol::Encode(Item(51, 0), MT::kReportDrop));
	const auto refused = PlacedIn(f.fake->TakeThrough(1, Type(MT::kDropPlaced)));
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].dropId == 0);
	CHECK(refused[0].ownerId == ada->playerId);
	CHECK(refused[0].nonce == 51);
	CHECK(f.LoggedSoon("an item or a place the session can't name"));

	constexpr std::uint64_t BURST = 60;  // the budget holds 32 at once, and 4 more a second
	for (std::uint64_t i = 1; i <= BURST; ++i) {
		f.fake->Deliver(2, Protocol::Encode(Item(100 + i), MT::kReportDrop));
	}
	REQUIRE(f.fake->Settle());
	const auto answers = PlacedIn(f.fake->Take(2));
	CHECK(answers.size() == BURST);
	const auto none = std::ranges::count_if(answers, [](const DroppedItem& a_d) { return a_d.dropId == 0; });
	CHECK(none > 0);
	CHECK(static_cast<std::uint64_t>(none) <= BURST - 32);
	CHECK(f.LoggedSoon("dropped faster than the server takes them"));
	CHECK(PlacedIn(f.fake->Take(1)).empty());  // offers and refusals are the dropper's alone
}

TEST("server: a new session starts with nothing lying there, its drops numbered on from the last")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	REQUIRE(ada);
	f.fake->Deliver(1, Protocol::Encode(Item(61), MT::kReportDrop));
	const auto first = PlacedIn(f.fake->TakeThrough(1, Type(MT::kDropPlaced))).at(0).dropId;
	f.fake->Deliver(1, Claim(first, DropClaim::kPlaced));
	(void)f.fake->TakeThrough(1, Type(MT::kDropPlaced));
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));  // the first player in again: a new session
	REQUIRE(bo);
	f.fake->Deliver(2, Protocol::Encode(DropRecord{ bo->sessionId, {}, {} }));
	const auto lists = ListsIn(f.fake->TakeThrough(2, Type(MT::kDropList)));
	REQUIRE(lists.size() == 1);
	CHECK(lists[0].drops.empty());
	f.fake->Deliver(2, Protocol::Encode(Item(61), MT::kReportDrop));
	CHECK(PlacedIn(f.fake->TakeThrough(2, Type(MT::kDropPlaced))).at(0).dropId == first + 1);
}

// ---- each game's rules --------------------------------------------------------------------------------------------

TEST("drop judge: a dropped item made at run time is given; no reference, a plugin's own, not an item, keys and books, quest items and runtime forms are not")
{
	const ItemFacts sword{ .kind = ItemKind::kWeapon, .count = 1, .named = true, .created = true };
	CHECK(Judge(sword) == Givable::kYes);
	const auto with = [&](auto a_change) {
		auto facts = sword;
		a_change(facts);
		return Judge(facts);
	};
	CHECK(with([](ItemFacts& a_f) { a_f.named = false; }) == Givable::kNoReference);
	CHECK(with([](ItemFacts& a_f) { a_f.created = false; }) == Givable::kPlaced);
	CHECK(with([](ItemFacts& a_f) { a_f.kind = ItemKind::kOther; }) == Givable::kNotItem);
	CHECK(with([](ItemFacts& a_f) { a_f.kind = ItemKind::kKey; }) == Givable::kOwn);
	CHECK(with([](ItemFacts& a_f) { a_f.kind = ItemKind::kBook; }) == Givable::kOwn);
	CHECK(with([](ItemFacts& a_f) { a_f.quest = true; }) == Givable::kQuest);
	CHECK(with([](ItemFacts& a_f) { a_f.runtimeBase = true; }) == Givable::kRuntime);
	CHECK(with([](ItemFacts& a_f) { a_f.runtimeEnchantment = true; }) == Givable::kRuntime);
	CHECK(with([](ItemFacts& a_f) { a_f.runtimePoison = true; }) == Givable::kRuntime);
	CHECK(with([](ItemFacts& a_f) { a_f.count = 0; }) == Givable::kCount);
	CHECK(with([](ItemFacts& a_f) { a_f.count = Protocol::MAX_DROP_COUNT + 1; }) == Givable::kCount);
	// Every kind a player carries, judged by its type, a mod's as the game's.
	for (const auto kind : { ItemKind::kArmor, ItemKind::kAmmo, ItemKind::kPotion, ItemKind::kIngredient, ItemKind::kMisc, ItemKind::kSoulGem,
			 ItemKind::kScroll, ItemKind::kLight, ItemKind::kApparatus }) {
		CHECK(with([&](ItemFacts& a_f) { a_f.kind = kind; }) == Givable::kYes);
	}
	for (std::size_t i = 0; i < GIVABLE_COUNT; ++i) {
		CHECK(std::string{ GivableName(static_cast<Givable>(i)) } != "?");
	}
}

TEST("drop rest: an item is at rest once still for STILL_FOR, no sooner than MIN_WAIT, and at MAX_WAIT wherever it is")
{
	CHECK(!Settled(1s, 1s));
	CHECK(Settled(MIN_WAIT, STILL_FOR));
	CHECK(!Settled(MIN_WAIT, STILL_FOR - 1ms));
	CHECK(!Settled(4s, 0ms));
	CHECK(Settled(MAX_WAIT, 0ms));
}

TEST("drop copy: placed near our player in the same place, removed past REMOVE_PAST or elsewhere, never one this game lacks")
{
	CHECK(Want(true, true, 100.0f, false) == CopyWant::kPlace);
	CHECK(Want(true, true, PLACE_WITHIN, false) == CopyWant::kPlace);
	CHECK(Want(true, true, PLACE_WITHIN + 1.0f, false) == CopyWant::kKeep);
	CHECK(Want(true, false, 100.0f, false) == CopyWant::kKeep);
	CHECK(Want(true, true, REMOVE_PAST - 1.0f, true) == CopyWant::kKeep);  // the gap: no placing and removing every frame
	CHECK(Want(true, true, REMOVE_PAST + 1.0f, true) == CopyWant::kRemove);
	CHECK(Want(true, false, 0.0f, true) == CopyWant::kRemove);
	CHECK(Want(false, true, 0.0f, false) == CopyWant::kKeep);
	CHECK(Want(false, true, 0.0f, true) == CopyWant::kRemove);
}

TEST("drop claims: one per item until answered or CLAIM_RETRY passes, at most MAX_OUT at once")
{
	Claims claims;
	CHECK(!claims.Start(0, T0));
	CHECK(claims.Start(1, T0));
	CHECK(!claims.Start(1, T0 + 1s));
	CHECK(claims.Start(1, T0 + Claims::CLAIM_RETRY));
	CHECK(claims.Answered(1));
	CHECK(!claims.Answered(1));
	CHECK(claims.Start(1, T0));
	for (std::uint32_t id = 2; id <= Claims::MAX_OUT; ++id) {
		CHECK(claims.Start(id, T0));
	}
	CHECK(!claims.Start(100, T0));
	CHECK(claims.Size() == Claims::MAX_OUT);
	CHECK(claims.Waiting(2));
	claims.Clear();
	CHECK(claims.Size() == 0);
}

TEST("drop take: done once the copy left the world and the count rose; picked up again while it lies there, given back after TAKE_TRIES")
{
	TakeFacts take{ .copyInWorld = false, .before = 2, .now = 5, .count = 3, .tries = 1, .sinceTry = 0ms, .sinceGone = 0ms };
	CHECK(JudgeTake(take) == TakeState::kDone);
	take.now = 4;
	CHECK(JudgeTake(take) == TakeState::kWait);
	take.sinceGone = TAKE_SETTLE;
	CHECK(JudgeTake(take) == TakeState::kUnsure);
	take = TakeFacts{ .copyInWorld = true, .before = 0, .now = 0, .count = 1, .tries = 1, .sinceTry = 100ms, .sinceGone = std::nullopt };
	CHECK(JudgeTake(take) == TakeState::kWait);
	take.sinceTry = TAKE_RETRY_EVERY;
	CHECK(JudgeTake(take) == TakeState::kRetry);
	take.tries = TAKE_TRIES;
	CHECK(JudgeTake(take) == TakeState::kGiveBack);
}

TEST("drop unconfirmed: ours taken out of our world wait for the server, at most MAX; those never heard of go back in drop order")
{
	Unconfirmed waiting;
	for (std::uint32_t id = 1; id <= Unconfirmed::MAX; ++id) {
		CHECK(waiting.Swapped(Numbered(Item(id), id, 1)));
	}
	CHECK(!waiting.Swapped(Numbered(Item(999), 999, 1)));
	CHECK(waiting.Swapped(Numbered(Item(5), 5, 1)));  // one it has already
	CHECK(waiting.Heard(3));
	CHECK(!waiting.Heard(3));
	CHECK(!waiting.Waiting(3));
	const auto back = waiting.TakeAll();
	CHECK(back.size() == Unconfirmed::MAX - 1);
	CHECK(back.front().dropId == 1);
	CHECK(back.back().dropId == Unconfirmed::MAX);
	CHECK(waiting.Size() == 0);
}

TEST("drop record: kept per session, the newest last; nothing kept outside a session; saved and read back; bytes that can't be read are none")
{
	Record record;
	record.Made(1);  // in no session: nothing kept
	CHECK(record.Empty());
	record.Bind(10);
	record.Made(100);
	record.Made(100);
	record.Took(7);
	CHECK(record.HasMade(100));
	CHECK(record.HasTaken(7));
	CHECK(record.ForSession() == DropRecord{ 10, { 100 }, { 7 } });
	record.Bind(20);
	record.Took(8);
	CHECK(record.ForSession() == DropRecord{ 20, {}, { 8 } });
	record.Bind(10);  // back to the first: it is the newest now
	record.Unmade(100);
	record.Untook(7);
	CHECK(record.ForSession() == DropRecord{ 10, {}, {} });
	record.Made(101);

	const auto bytes = record.Save();
	Record     loaded;
	CHECK(loaded.Load(bytes));
	CHECK(loaded.Sessions() == 2);
	CHECK(loaded.Current() == 0);  // bound again by its welcome
	loaded.Bind(10);
	CHECK(loaded.ForSession() == DropRecord{ 10, { 101 }, {} });
	loaded.Bind(20);
	CHECK(loaded.ForSession() == DropRecord{ 20, {}, { 8 } });
	CHECK(loaded.Save().size() == bytes.size());

	auto broken = bytes;
	broken.pop_back();
	CHECK(!loaded.Load(broken));
	CHECK(loaded.Empty());
	broken = bytes;
	broken.push_back(0);
	CHECK(!loaded.Load(broken));
	broken = bytes;
	broken[0] = 9;  // another version
	CHECK(!loaded.Load(broken));
	CHECK(!loaded.Load({}));

	// Past MAX_SESSIONS the oldest goes.
	Record many;
	for (std::uint64_t s = 1; s <= Record::MAX_SESSIONS + 2; ++s) {
		many.Bind(s);
		many.Took(static_cast<std::uint32_t>(s));
	}
	CHECK(many.Sessions() == Record::MAX_SESSIONS);
	many.Bind(1);
	CHECK(many.ForSession() == DropRecord{ 1, {}, {} });
}

TEST("drop lying: a list replaces what this game heard once its last packet comes; one placed while it comes stays")
{
	Lying lying;
	lying.Placed(Numbered(Item(1), 1, 1));
	lying.Placed(Numbered(Item(2), 2, 2));
	lying.Placed(Numbered(Item(3), 3, 1));
	CHECK(lying.Size() == 3);
	CHECK(lying.OwnedBy(1).size() == 2);
	CHECK(lying.Gone(2));
	CHECK(!lying.Gone(2));
	lying.Listed(Numbered(Item(1), 1, 1));
	lying.Placed(Numbered(Item(4), 4, 2));  // heard while the list comes
	const auto gone = lying.EndList();
	CHECK(gone == std::vector<std::uint32_t>{ 3 });
	CHECK(lying.Find(1));
	CHECK(lying.Find(4));
	CHECK(!lying.Find(3));
	lying.BeginList();
	CHECK(lying.EndList().size() == 2);  // an empty list: nothing lies there
	CHECK(lying.Size() == 0);
}

TEST("drop offer: ours waiting is swapped while it lies where it came to rest, kept when picked up first; refused stays ours; any other lies there")
{
	CHECK(HearPlaced(5, true, true, true) == Offer::kSwap);
	CHECK(HearPlaced(5, true, true, false) == Offer::kKept);
	CHECK(HearPlaced(0, true, true, true) == Offer::kRefused);
	CHECK(HearPlaced(5, true, false, true) == Offer::kLies);  // ours, confirmed: as everyone hears it
	CHECK(HearPlaced(5, false, false, false) == Offer::kLies);
	CHECK(HearPlaced(0, false, false, false) == Offer::kRefused);
}
