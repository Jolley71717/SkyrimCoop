#pragma once

#include "RideBook.h"

// Who is in what furniture, as the server keeps it (docs/DESIGN.md, "Furniture"): the riding book's rules (RideBook.h),
// over sits. A game says its own player's and those of the NPCs it runs (kSits) whenever one changes; the server passes
// on a player's only from that player's game and an NPC's only from its runner's, each change once, gives a joiner every
// one the session has, owes a change past the sender's budget, and ends every one a leaver's game said. No game and no
// network here: tests/SitTests.cpp.
namespace SitBook
{
	using Book = RideBook::BookOf<Protocol::Sit>;
	using RideBook::RiderOf;
	[[nodiscard]] inline Protocol::Sit Up(const RideBook::Rider& a_rider) { return RideBook::EndOf<Protocol::Sit>(a_rider); }
}
