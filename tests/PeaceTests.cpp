#include "Test.h"

#include "game/PeaceRules.h"
#include "game/StandInRules.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

// The world leaves a friend's stand-in alone (docs/DESIGN.md, "Players"; src/game/PeaceRules.h): who is a stand-in for
// the engine's hooks, what the engine's asking about one is turned away, an actor seen fighting one told to stop and
// given up on, and the effects put on one.

using namespace std::chrono_literals;

namespace
{
	using Ask = PeaceRules::Ask;
	using Calm = PeaceRules::Calm;
	using Magic = PeaceRules::Magic;
	using Steady = std::chrono::steady_clock;

	constexpr std::uint32_t HOSTS = 0xFF000801;   // the host's stand-in, in the VM's game
	constexpr std::uint32_t THIRDS = 0xFF000802;  // a third player's
	constexpr std::uint32_t LEAVING = 0xFF0007F0;  // one on its way out
	constexpr std::uint32_t GUARD = 0x0001A68A;
	constexpr std::uint32_t WOLF = 0xFF000900;
}

TEST("peace ids: the set is what it was given, zero and repeats left out; empty finds nothing")
{
	PeaceRules::IdSet<8> set;
	CHECK(!set.Contains(HOSTS));
	CHECK(!set.Contains(0));
	CHECK(set.Count() == 0);
	const std::vector<std::uint32_t> ids{ HOSTS, 0, LEAVING, HOSTS };
	CHECK(set.Assign(ids));
	CHECK(set.Count() == 2);
	CHECK(set.Contains(HOSTS));
	CHECK(set.Contains(LEAVING));
	CHECK(!set.Contains(THIRDS));
	CHECK(!set.Contains(0));  // an empty slot is never an id

	// The one on its way out is gone, a third player's stand-in comes.
	const std::vector<std::uint32_t> next{ HOSTS, THIRDS };
	CHECK(set.Assign(next));
	CHECK(set.Count() == 2);
	CHECK(set.Contains(HOSTS));
	CHECK(set.Contains(THIRDS));
	CHECK(!set.Contains(LEAVING));

	CHECK(set.Assign({}));
	CHECK(set.Count() == 0);
	CHECK(!set.Contains(HOSTS));
}

TEST("peace ids: more than the set holds is said (false), and as many as fit are kept")
{
	PeaceRules::IdSet<2> set;
	const std::vector<std::uint32_t> three{ HOSTS, THIRDS, LEAVING };
	CHECK(!set.Assign(three));
	CHECK(set.Count() == 2);
	CHECK(set.Contains(HOSTS));
	CHECK(set.Contains(THIRDS));
	CHECK(!set.Contains(LEAVING));
	// The overflow went: room again.
	const std::vector<std::uint32_t> two{ HOSTS, LEAVING };
	CHECK(set.Assign(two));
	CHECK(set.Contains(LEAVING));
	CHECK(!set.Contains(THIRDS));
	CHECK(PeaceRules::MAX_WATCHED >= 16);
}

TEST("peace ids: an id kept from one set to the next is never missing while the set is written (a hook reads with no lock)")
{
	PeaceRules::IdSet<PeaceRules::MAX_WATCHED> set;
	const std::vector<std::uint32_t> first{ HOSTS, THIRDS };
	const std::vector<std::uint32_t> second{ LEAVING, HOSTS };
	set.Assign(first);
	std::atomic<bool>          done{ false };
	std::atomic<std::uint64_t> misses{ 0 };
	std::atomic<std::uint64_t> reads{ 0 };
	std::thread                reader([&] {
		while (!done.load()) {
			if (!set.Contains(HOSTS)) {
				misses.fetch_add(1);
			}
			reads.fetch_add(1);
		}
	});
	for (int i = 0; i < 20000; ++i) {
		set.Assign(i % 2 == 0 ? second : first);
	}
	done = true;
	reader.join();
	CHECK(misses.load() == 0);
	CHECK(reads.load() > 0);
	CHECK(set.Contains(HOSTS));
}

TEST("peace asks: a stand-in is no one's target, talker, crowd or threat, and asks none of it; everyone else as the engine says")
{
	CHECK(PeaceRules::Refused(false, true));   // a guard asked about the stand-in
	CHECK(PeaceRules::Refused(true, false));   // the stand-in asked about anyone (getting into furniture, its own update runs)
	CHECK(PeaceRules::Refused(true, true));    // one stand-in about another
	CHECK(!PeaceRules::Refused(false, false));  // a wolf about our player, a guard about a bandit: untouched
	CHECK(PeaceRules::Name(Ask::kTarget) == "target");
	CHECK(PeaceRules::Name(Ask::kTalk) == "talk");
	CHECK(PeaceRules::Name(Ask::kWatch) == "watch");
	CHECK(PeaceRules::Name(Ask::kFlee) == "flee");
	CHECK(static_cast<int>(Ask::kCount) == 4);
}

TEST("peace calm: an actor fighting a stand-in is told to stop, again only after the cool-down")
{
	PeaceRules::CalmBook book;
	const auto           t0 = Steady::now();
	CHECK(book.Seen(WOLF, t0) == Calm::kStop);
	CHECK(book.Seen(WOLF, t0 + 500ms) == Calm::kWait);  // the next sweep: told already
	CHECK(book.Seen(WOLF, t0 + PeaceRules::CALM_COOL - 1ms) == Calm::kWait);
	CHECK(book.Seen(WOLF, t0 + PeaceRules::CALM_COOL) == Calm::kStop);
	CHECK(book.Seen(GUARD, t0 + PeaceRules::CALM_COOL) == Calm::kStop);  // each actor on its own
	CHECK(book.Size() == 2);
	CHECK(book.GivenUp() == 0);
}

TEST("peace calm: one that fights a stand-in again each time it is told is given up on once, rests, then is told again")
{
	PeaceRules::CalmBook book;
	auto                 t = Steady::now();
	for (std::size_t i = 0; i < PeaceRules::CALM_TRIES; ++i) {
		CHECK(book.Seen(WOLF, t) == Calm::kStop);
		t += PeaceRules::CALM_COOL;
	}
	CHECK(book.Seen(WOLF, t) == Calm::kGiveUp);  // said once
	CHECK(book.GivenUp() == 1);
	CHECK(book.Seen(WOLF, t + 1s) == Calm::kWait);
	CHECK(book.Seen(WOLF, t + PeaceRules::CALM_REST - 1ms) == Calm::kWait);
	CHECK(book.Seen(WOLF, t + PeaceRules::CALM_REST) == Calm::kStop);  // its rest is over: told again, counted afresh
	CHECK(book.GivenUp() == 0);
}

TEST("peace calm: stops spread wider than the window never give an actor up")
{
	PeaceRules::CalmBook book;
	auto                 t = Steady::now();
	const auto           apart = PeaceRules::CALM_WINDOW / static_cast<int>(PeaceRules::CALM_TRIES - 1);
	for (int i = 0; i < 12; ++i) {
		CHECK(book.Seen(WOLF, t) == Calm::kStop);
		t += apart;
	}
	CHECK(book.GivenUp() == 0);
}

TEST("peace calm: an actor told nothing for the window is forgotten; one given up on is kept until its rest is over")
{
	PeaceRules::CalmBook book;
	const auto           t0 = Steady::now();
	book.Seen(GUARD, t0);
	auto t = t0;
	for (std::size_t i = 0; i <= PeaceRules::CALM_TRIES; ++i) {
		book.Seen(WOLF, t);
		t += PeaceRules::CALM_COOL;
	}
	CHECK(book.GivenUp() == 1);
	book.Age(t0 + PeaceRules::CALM_WINDOW);
	CHECK(book.Size() == 1);  // the guard went, the wolf rests
	const auto restEnd = t - PeaceRules::CALM_COOL + PeaceRules::CALM_REST;
	book.Age(restEnd - 1ms);
	CHECK(book.Size() == 1);
	book.Age(restEnd);
	CHECK(book.Size() == 0);
	book.Seen(GUARD, restEnd);
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("peace calm: a full book makes room by forgetting old entries, and a new actor waits while it is full of recent ones")
{
	PeaceRules::CalmBook book;
	const auto           t0 = Steady::now();
	for (std::uint32_t i = 0; i < PeaceRules::CALM_MAX; ++i) {
		CHECK(book.Seen(0x00100000 + i, t0) == Calm::kStop);
	}
	CHECK(book.Size() == PeaceRules::CALM_MAX);
	CHECK(book.Seen(WOLF, t0 + 1s) == Calm::kWait);  // all told within the window: no room
	CHECK(book.Size() == PeaceRules::CALM_MAX);
	CHECK(book.Seen(WOLF, t0 + PeaceRules::CALM_WINDOW) == Calm::kStop);  // the old ones forgotten
	CHECK(book.Size() == 1);
}

TEST("peace magic: anyone's effect on a stand-in is refused; a stand-in's on another refused, on itself as its kind says")
{
	// Anyone's on a stand-in: a guard's spell, our player's heal, a trap's or a hazard's (no caster).
	CHECK(PeaceRules::JudgeMagic(false, true, false, false) == Magic::kRefusedOn);
	CHECK(PeaceRules::JudgeMagic(false, true, false, true) == Magic::kRefusedOn);  // even a kind a stand-in shows on itself
	// One stand-in's on another: refused on the target (the caster's friend's game deals it).
	CHECK(PeaceRules::JudgeMagic(true, true, false, false) == Magic::kRefusedOn);
	// A stand-in's on someone else: its friend's game deals it.
	CHECK(PeaceRules::JudgeMagic(true, false, false, false) == Magic::kRefused);
	// Its own on itself: what it shows (a light, a cloak, invisibility) held; the rest refused.
	CHECK(PeaceRules::JudgeMagic(true, true, true, true) == Magic::kKept);
	CHECK(PeaceRules::JudgeMagic(true, true, true, false) == Magic::kRefused);
	CHECK(PeaceRules::JudgeMagic(true, false, true, true) == Magic::kKept);
	// Neither a stand-in: the engine's.
	CHECK(PeaceRules::JudgeMagic(false, false, false, false) == Magic::kLet);
	CHECK(PeaceRules::JudgeMagic(false, false, true, true) == Magic::kLet);
}

TEST("peace magic: what a stand-in casts is judged as before (StandInRules::MagicKept), now with the target's side")
{
	const std::array kinds{ StandInRules::kArchNone, StandInRules::kValueModifier, StandInRules::kScript, StandInRules::kCloak, StandInRules::kLight,
		StandInRules::kSummonCreature, StandInRules::kBoundWeapon, StandInRules::kInvisibility };
	for (const auto kind : kinds) {
		for (const bool onItself : { false, true }) {
			const bool kept = StandInRules::MagicKept(onItself, kind);
			const auto verdict = PeaceRules::JudgeMagic(true, onItself, onItself, kept);
			CHECK((verdict == Magic::kKept) == kept);
			CHECK(verdict != Magic::kLet);
		}
	}
}
