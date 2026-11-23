#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Containers and loot as one class (docs/DESIGN.md, "Containers and loot"): which of a player's takes and puts the
// session shares, the ledger a game keeps of the session's numbered moves (what its world holds, kept in its co-save),
// the moves that wait for their holder, and the emptied marker. Kept free of the game so every rule is tested
// (tests/LootTests.cpp); src/game/LootSync.cpp reads the game and makes what these decide.
//
// The class is every holder a player loots, judged by what the engine keeps, never by a script's or a record's name, so
// a mod's chest is shared as the base game's are: every container (chests, barrels, sacks, urns, satchels, wardrobes,
// coffins, Dwemer and mod containers), and the body of every actor kind once it is dead (people, creatures, dragons,
// horses, a follower or a summon that died; an ash pile's or a dragon's remains are its actor's inventory). What stays
// each player's own:
// - a living actor's inventory (a follower's or a horse's pack, a pickpocket's take, a stand-in): never reported;
// - a merchant's chest (any faction's vendor container), and anything moved while the barter menu is open or just shut;
// - keys, books (notes, journals, skill books and spell tomes: each player reads their own), quest items, and an
//   instance with its own extras (tempered, enchanted or poisoned by a player, renamed, a soul gem a player filled): a
//   move carries the item's base form only, so another game would get a plain copy;
// - a runtime holder or item (FF: made by one game as it ran, a placed chest, a summoned body, a player's potion).
// Weapons and armor that are plain base forms (all of Skyrim's loot, enchanted loot included: its enchantment is the
// form's own) are shared like gold, food, ingredients, ammo, potions, soul gems, scrolls and torches.
namespace LootRules
{
	using Clock = std::chrono::steady_clock;
	using LootEntry = Protocol::LootEntry;

	enum class ItemKind : std::uint8_t
	{
		kOther,  // not an inventory item (a form a container can't hold), or one this class doesn't know
		kWeapon,
		kArmor,
		kAmmo,
		kPotion,  // AlchemyItem: potions, poisons, food, drink
		kIngredient,
		kMisc,  // gold, gems, ore and ingots, pelts, clutter, lockpicks
		kBook,  // books, notes, journals, skill books, spell tomes, recipes
		kKey,
		kSoulGem,
		kScroll,
		kLight,  // torches
		kApparatus,
	};

	enum class HolderKind : std::uint8_t
	{
		kOther,      // neither a container nor an actor (a loose item, a door, a player)
		kContainer,  // TESObjectCONT
		kActor,      // any actor: a person, a creature, a dragon, a horse, a follower, a summon
	};

	// What a game knows of the holder of a move, read by LootSync.
	struct Holder
	{
		HolderKind kind = HolderKind::kOther;
		bool       dead = false;     // an actor that is dead here
		bool       vendor = false;   // some faction's merchant container
		bool       storage = false;  // the player's own (owned by the player or the player's faction, or in a cell they own)
		bool       runtime = false;  // a runtime reference (FF)
	};

	// What a game knows of the item of a move.
	struct Item
	{
		ItemKind kind = ItemKind::kOther;
		bool     quest = false;     // a quest item (held in a quest alias as a quest object)
		bool     instance = false;  // the stack moved has its own extras (tempered, a player's enchantment or poison, a name, a filled soul)
		bool     runtime = false;   // a runtime form (FF: a player's own potion or enchanted item)
	};

	// Why a move is or isn't the session's (counted by name in `loot`).
	enum class Scope : std::uint8_t
	{
		kShared,
		kNotHolder,      // neither a container nor an actor
		kRuntimeHolder,  // a runtime reference
		kLiving,         // a living actor's inventory
		kVendor,         // a merchant's chest
		kBarter,         // the barter menu open, or shut less than BARTER_GRACE ago
		kNotItem,        // a form this class doesn't share
		kRuntimeItem,    // a runtime form
		kKey,
		kBook,
		kQuest,
		kInstance,
	};
	inline constexpr std::size_t SCOPE_COUNT = static_cast<std::size_t>(Scope::kInstance) + 1;

	[[nodiscard]] constexpr const char* ScopeName(Scope a_scope)
	{
		switch (a_scope) {
		case Scope::kShared:
			return "shared";
		case Scope::kNotHolder:
			return "notHolder";
		case Scope::kRuntimeHolder:
			return "runtimeHolder";
		case Scope::kLiving:
			return "living";
		case Scope::kVendor:
			return "vendor";
		case Scope::kBarter:
			return "barter";
		case Scope::kNotItem:
			return "notItem";
		case Scope::kRuntimeItem:
			return "runtimeItem";
		case Scope::kKey:
			return "key";
		case Scope::kBook:
			return "book";
		case Scope::kQuest:
			return "quest";
		case Scope::kInstance:
			return "instance";
		}
		return "?";
	}

	// How long after the barter menu shuts a move still counts as a trade (its last items land as it closes).
	inline constexpr auto BARTER_GRACE = std::chrono::seconds(1);

	// The holder's part of the scope: whether moves at it are the session's at all (the emptied marker asks only this).
	[[nodiscard]] constexpr Scope HolderScope(const Holder& a_holder, bool a_barter)
	{
		if (a_holder.kind == HolderKind::kOther) {
			return Scope::kNotHolder;
		}
		if (a_holder.runtime) {
			return Scope::kRuntimeHolder;
		}
		if (a_barter) {
			return Scope::kBarter;
		}
		if (a_holder.vendor) {
			return Scope::kVendor;
		}
		if (a_holder.kind == HolderKind::kActor && !a_holder.dead) {
			return Scope::kLiving;
		}
		return Scope::kShared;
	}

	// The item's part.
	[[nodiscard]] constexpr Scope ItemScope(const Item& a_item)
	{
		if (a_item.kind == ItemKind::kOther) {
			return Scope::kNotItem;
		}
		if (a_item.runtime) {
			return Scope::kRuntimeItem;
		}
		if (a_item.kind == ItemKind::kKey) {
			return Scope::kKey;
		}
		if (a_item.kind == ItemKind::kBook) {
			return Scope::kBook;
		}
		if (a_item.quest) {
			return Scope::kQuest;
		}
		if (a_item.instance) {
			return Scope::kInstance;
		}
		return Scope::kShared;
	}

	// Whether our player's move of a_item at a_holder is the session's: the holder first (what it is decides for all its
	// items), then the item.
	[[nodiscard]] constexpr Scope Of(const Holder& a_holder, const Item& a_item, bool a_barter)
	{
		const auto holder = HolderScope(a_holder, a_barter);
		return holder != Scope::kShared ? holder : ItemScope(a_item);
	}

	// What a game does with a move of the session's (another player's, or its own from a catch-up), by its own facts: the
	// sender judged it shared in its game; this game judges again, so what an older build or another game's mods report
	// anyway stays out.
	enum class Verdict : std::uint8_t
	{
		kMake,  // make it here now
		kWait,  // the holder is not loaded here (or its inventory not made yet): wait for it
		kDead,  // the holder is an actor alive here: wait for its death (up to LIVING_WAIT)
		kSkip,  // not this game's to make (Judge's scope says why)
	};
	struct Judged
	{
		Verdict verdict = Verdict::kSkip;
		Scope   scope = Scope::kShared;
	};
	[[nodiscard]] constexpr Judged Judge(bool a_loaded, const Holder& a_holder, const Item& a_item, bool a_emptied)
	{
		if (!a_loaded) {
			return { Verdict::kWait, Scope::kShared };
		}
		auto holder = HolderScope(a_holder, false);
		if (holder == Scope::kLiving) {
			return { Verdict::kDead, holder };
		}
		if (holder != Scope::kShared) {
			return { Verdict::kSkip, holder };
		}
		if (a_emptied) {
			// Storage keeps what each player put there: an emptied marker never takes from it (its moves are shared).
			return a_holder.storage ? Judged{ Verdict::kSkip, Scope::kShared } : Judged{ Verdict::kMake, Scope::kShared };
		}
		const auto item = ItemScope(a_item);
		return { item == Scope::kShared ? Verdict::kMake : Verdict::kSkip, item };
	}

	// ---- the ledger ---------------------------------------------------------------------------------------------------

	// What this game's world holds of the session's numbered moves: the log's id and the first number it does not hold
	// yet. Kept in the co-save, so a save loaded later asks for the moves from there (its own among them: the load took
	// them from its world). Its own moves still out (sent, no echo or answer yet) are kept here too, each by the tag it
	// was sent with: the server answers each move once, with a numbered echo or a kLootAnswer, and carries the tag back as
	// it was (the forms may come back as 0).
	//
	// The world is replaced at a welcome and at a load (Begin): from that moment, not from when the ask goes out, every
	// move passed on is held until the catch-up of this world's newest ask is done. So a move heard on the loading screen,
	// before the ask can go out, never moves the ledger past moves this world lacks: the catch-up brings it with them. Each
	// ask has its own number, carried by its catch-up and its end: the answers to an older ask (asked for a world replaced
	// since) are void, and an older ask the server never answered (it owes a game one at most, the newest) leaves nothing
	// waiting for it.
	class Ledger
	{
	public:
		struct Mark
		{
			std::uint64_t logId = 0;  // 0: no log (a world that never was in a session)
			std::uint32_t next = 1;
			bool operator==(const Mark&) const = default;
		};

		// What a move heard from the session is to this game.
		enum class Heard : std::uint8_t
		{
			kMake,  // make it here
			kOwn,   // ours, made in this world as we made it: nothing to make (its number is the ledger's now)
			kHeld,  // heard while this world's catch-up is coming: not made now (the catch-up brings it, if this world lacks it)
			kHad,   // numbered before what this world holds already
			kVoid,  // the catch-up of an ask older than this world's newest (a load or a welcome since): not this world's
		};

		// Whose a move echoed or answered was.
		enum class Mine : std::uint8_t
		{
			kNone,    // no move of ours out by that tag (another player's, a stray, or one given up on)
			kNow,     // ours, made in this world
			kBefore,  // ours, made in a world a load has replaced since: this world never made it
		};

		[[nodiscard]] Mark Saved() const { return mark; }

		// A load (from its co-save; {0, 1} when it held none) or a new game: the world is that one now. A catch-up still
		// coming was asked for the world before: void. Our moves still out were made in the world before: this world
		// lacks them, so their echoes are made here as anyone's are.
		void Loaded(Mark a_mark)
		{
			mark = a_mark.next == 0 ? Mark{ a_mark.logId, 1 } : a_mark;
			asking = false;
			askDue = false;
			for (auto& o : out) {
				o.before = true;
			}
		}

		// This world asks the session for the moves it lacks (a welcome, or a load in a session). From now until the
		// catch-up of this ask is done, moves passed on are held, whenever the ask itself can go out (AskDue: not from a
		// loading screen). Any older ask's catch-up is void from now.
		void Begin()
		{
			asking = true;
			askDue = true;
			if (++ask == 0) {
				ask = 1;
			}
		}

		// The ask to send now: once after each Begin, none otherwise. From the ledger as it is then: a move heard since
		// Begin was held, so the ledger never moved past one this world lacks.
		[[nodiscard]] std::optional<Protocol::LootAsk> AskDue()
		{
			if (!std::exchange(askDue, false)) {
				return std::nullopt;
			}
			return Protocol::LootAsk{ .ask = ask, .logId = mark.logId, .from = mark.next };
		}

		// A move the session passed on (kRemoteLoot): what to do with it. a_ask is the ask its batch answers as a catch-up
		// (0: a move passed on as it came). Our own, matched by its tag, is no longer out: made in this world, it is
		// kOwn; made in a world a load replaced since, this world lacks it and it is made (kMake), in a live echo and in a
		// catch-up alike. A move held is not matched: the catch-up brings it again, and matches it then.
		Heard Take(const LootEntry& a_move, std::uint32_t a_ask, std::uint32_t a_ourId)
		{
			if (a_ask != 0) {
				if (!asking || askDue || a_ask != ask) {
					return Heard::kVoid;
				}
			} else if (asking) {
				return Heard::kHeld;
			}
			const auto mine = a_ourId != 0 && a_move.playerId == a_ourId ? Match(a_move.tag, nullptr) : Mine::kNone;
			if (a_move.number < mark.next) {
				return Heard::kHad;
			}
			mark.next = a_move.number + 1;
			return mine == Mine::kNow ? Heard::kOwn : Heard::kMake;
		}

		// The end of a catch-up. True when it ends this world's newest ask (the ledger is the log's from now); an older
		// ask's, or one nobody asked for, is false and changes nothing.
		bool CaughtUp(const Protocol::LootCaughtUp& a_msg)
		{
			if (!asking || askDue || a_msg.ask != ask) {
				return false;
			}
			asking = false;
			mark.logId = a_msg.logId;
			mark.next = a_msg.next;
			return true;
		}

		[[nodiscard]] bool Asking() const { return asking; }

		// Our move goes out: tagged, and out until its echo or answer. The move as it is sent (with its tag).
		LootEntry Sent(LootEntry a_move, Clock::time_point a_now)
		{
			if (out.size() >= MAX_OUT) {
				out.pop_front();
				++lost;
			}
			a_move.tag = nextTag++;
			if (nextTag == 0) {
				nextTag = 1;
			}
			out.push_back({ a_move, a_now, false });
			return a_move;
		}

		// The first tag Sent gives (a random one per run of the game, so a catch-up that holds an earlier run's moves of
		// ours never meets a tag still out in this one).
		void FirstTag(std::uint32_t a_tag) { nextTag = a_tag != 0 ? a_tag : 1; }

		// Our move answered (kLootAnswer): no longer out. Whose it was (kNone: a stray, or one given up on), and a_sent
		// the move as we sent it (its forms ours, whatever the answer's came back as), to send again when it was busy.
		Mine Answered(const LootEntry& a_move, LootEntry& a_sent) { return Match(a_move.tag, &a_sent); }

		// Moves out longer than OUT_LOST are given up on (a connection that dropped them): counted as lost.
		void Expire(Clock::time_point a_now)
		{
			while (!out.empty() && a_now - out.front().at > OUT_LOST) {
				out.pop_front();
				++lost;
			}
		}

		// How many of a_item our player put into a_holder in this world that the session has not numbered yet.
		[[nodiscard]] std::uint32_t PutsOut(std::uint32_t a_holder, std::uint32_t a_item) const
		{
			std::uint64_t n = 0;
			for (const auto& o : out) {
				if (!o.before && o.move.kind == LootEntry::kPut && o.move.holderId == a_holder && o.move.itemId == a_item) {
					n += o.move.count;
				}
			}
			return static_cast<std::uint32_t>((std::min<std::uint64_t>)(n, Protocol::MAX_LOOT_COUNT));
		}

		[[nodiscard]] std::size_t   Out() const { return out.size(); }
		[[nodiscard]] std::uint64_t Lost() const { return lost; }

		// A session's end, or a welcome before its Begin: no catch-up is coming. The mark stays (the world's), and so do
		// our moves out: a rejoin's catch-up may hold their echoes (ours, made here), and the rest are lost in OUT_LOST.
		void Disconnected()
		{
			asking = false;
			askDue = false;
		}

		static constexpr std::size_t MAX_OUT = 4096;
		static constexpr auto        OUT_LOST = std::chrono::seconds(60);

	private:
		Mine Match(std::uint32_t a_tag, LootEntry* a_sent)
		{
			if (a_tag == 0) {
				return Mine::kNone;
			}
			const auto it = std::ranges::find_if(out, [&](const OutMove& a_out) { return a_out.move.tag == a_tag; });
			if (it == out.end()) {
				return Mine::kNone;
			}
			const auto mine = it->before ? Mine::kBefore : Mine::kNow;
			if (a_sent) {
				*a_sent = it->move;
			}
			out.erase(it);

			return mine;
		}

		struct OutMove
		{
			LootEntry         move;
			Clock::time_point at;
			bool              before = false;  // made in a world a load has replaced since
		};
		Mark                mark;
		std::deque<OutMove> out;
		std::uint32_t       ask = 0;         // this world's newest ask (0: none yet)
		bool                asking = false;  // ... its catch-up not done: moves passed on are held
		bool                askDue = false;  // ... and not sent yet
		std::uint32_t       nextTag = 1;
		std::uint64_t       lost = 0;
	};

	// ---- moves that wait ----------------------------------------------------------------------------------------------

	// The session's moves this game can't make yet: their holder is not loaded here (or a body still alive here). Each
	// holder's in the order heard: a put that waits holds back the takes after it, or a take would find nothing and the
	// put would leave more than the session has. A move for a living body is given up on after LIVING_WAIT (the session
	// said it died; this game never saw it: another game's own fight). Past MAX_WAITING the oldest is dropped.
	class Waiting
	{
	public:
		static constexpr std::size_t MAX_WAITING = 4096;
		static constexpr auto        LIVING_WAIT = std::chrono::minutes(10);

		void Add(const LootEntry& a_move, Clock::time_point a_now)
		{
			if (held.size() >= MAX_WAITING) {
				held.pop_front();
				++dropped;
			}
			held.push_back({ a_move, a_now, {} });
		}

		// Whether a move for a_holder waits already: a new one for it waits behind it.
		[[nodiscard]] bool Has(std::uint32_t a_holder) const
		{
			return std::ranges::any_of(held, [&](const Held& a_held) { return a_held.move.holderId == a_holder; });
		}

		// Tries each, in order: a_try(move) says what became of it. A holder whose move still waits has its later moves
		// left to wait too, untried this time.
		template <class Try>
		void Retry(Clock::time_point a_now, Try a_try)
		{
			std::unordered_set<std::uint32_t> blocked;
			for (auto it = held.begin(); it != held.end();) {
				if (blocked.contains(it->move.holderId)) {
					++it;
					continue;
				}
				switch (a_try(it->move)) {
				case Verdict::kMake:
				case Verdict::kSkip:
					it = held.erase(it);
					continue;
				case Verdict::kDead:
					if (!it->livingSince) {
						it->livingSince = a_now;
					}
					if (a_now - *it->livingSince > LIVING_WAIT) {
						++expired;
						it = held.erase(it);
						continue;
					}
					break;
				case Verdict::kWait:
					it->livingSince.reset();
					break;
				}
				blocked.insert(it->move.holderId);
				++it;
			}
		}

		// Each move that waits, in order (for the co-save).
		template <class Fn>
		void ForEach(Fn a_fn) const
		{
			for (const auto& h : held) {
				a_fn(h.move);
			}
		}

		[[nodiscard]] std::size_t   Size() const { return held.size(); }
		[[nodiscard]] std::uint64_t Dropped() const { return dropped; }
		[[nodiscard]] std::uint64_t Expired() const { return expired; }
		void                        Clear() { held.clear(); }

	private:
		struct Held
		{
			LootEntry                        move;
			Clock::time_point                since;
			std::optional<Clock::time_point> livingSince;  // waiting for a death since
		};
		std::list<Held> held;
		std::uint64_t   dropped = 0;
		std::uint64_t   expired = 0;
	};

	// ---- the emptied marker -------------------------------------------------------------------------------------------

	// Each game rolls a chest's and a body's leveled loot on its own, so the same holder holds different things in two
	// games. When a player's take leaves their copy holding nothing the session shares, their game says so (kEmptied,
	// with its ledger as of then); every other game then takes out of its own copy what is left of each item another
	// player took from that holder since it was last emptied here (its roll had more of it: the takes went in before),
	// and leaves the rest: what only its own roll holds, what its own player alone took (that says nothing of the other
	// copy), and the session's puts the sender had not seen yet (numbered at or after its ledger, or ours still out).
	class TakenBook
	{
	public:
		static constexpr std::size_t MAX_HOLDERS = 4096;

		// A take made here: another player's (counted for a marker), or ours (not).
		void Took(std::uint32_t a_holder, std::uint32_t a_item, bool a_byOther)
		{
			if (a_byOther) {
				Of(a_holder).taken.insert(a_item);
			}
		}

		// A put made here (anyone's), numbered a_number by the session.
		void Put(std::uint32_t a_holder, std::uint32_t a_item, std::uint32_t a_count, std::uint32_t a_number)
		{
			auto& puts = Of(a_holder).puts;
			if (puts.size() < MAX_PUTS) {
				puts.push_back({ a_item, a_count, a_number });
			}
		}

		// A marker as of a_asOf for a_holder: each item another player took from it since it was last emptied here, with
		// how many of it to keep (the session's puts numbered a_asOf or later, plus a_putsOut(item): ours not numbered
		// yet). The holder's record starts again.
		template <class PutsOut>
		[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> Emptied(std::uint32_t a_holder, std::uint32_t a_asOf, PutsOut a_putsOut)
		{
			std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
			const auto                                           it = holders.find(a_holder);
			if (it == holders.end()) {
				return out;
			}
			for (const auto item : it->second.taken) {
				std::uint64_t keep = a_putsOut(item);
				for (const auto& put : it->second.puts) {
					if (put.item == item && put.number >= a_asOf) {
						keep += put.count;
					}
				}
				out.emplace_back(item, static_cast<std::uint32_t>((std::min<std::uint64_t>)(keep, Protocol::MAX_LOOT_COUNT)));
			}
			std::ranges::sort(out);
			Forget(a_holder);
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return holders.size(); }
		void                      Clear()
		{
			holders.clear();
			order.clear();
		}

	private:
		static constexpr std::size_t MAX_PUTS = 256;

		struct PutAt
		{
			std::uint32_t item = 0;
			std::uint32_t count = 0;
			std::uint32_t number = 0;
		};
		struct Record
		{
			std::unordered_set<std::uint32_t> taken;
			std::vector<PutAt>                puts;
		};

		Record& Of(std::uint32_t a_holder)
		{
			if (const auto it = holders.find(a_holder); it != holders.end()) {
				return it->second;
			}
			if (holders.size() >= MAX_HOLDERS && !order.empty()) {
				holders.erase(order.front());
				order.pop_front();
			}
			order.push_back(a_holder);
			return holders[a_holder];
		}

		void Forget(std::uint32_t a_holder)
		{
			holders.erase(a_holder);
			std::erase(order, a_holder);
		}

		std::unordered_map<std::uint32_t, Record> holders;
		std::deque<std::uint32_t>                 order;  // oldest first
	};

	// Whether our game says a holder empty: once each time its copy here comes to hold nothing the session shares after
	// our player's take, not again until it holds something shared again.
	class EmptyWatch
	{
	public:
		// After our player's take from a_holder: true when the marker is to be sent now.
		bool AfterTake(std::uint32_t a_holder, bool a_holdsShared)
		{
			if (a_holdsShared) {
				said.erase(a_holder);
				return false;
			}
			if (said.size() >= TakenBook::MAX_HOLDERS) {
				said.clear();
			}
			return said.insert(a_holder).second;
		}
		// Our player put something into it: it may be said empty again.
		void AfterPut(std::uint32_t a_holder) { said.erase(a_holder); }
		void Clear() { said.clear(); }

	private:
		std::unordered_set<std::uint32_t> said;
	};

	// ---- our moves going out ------------------------------------------------------------------------------------------

	// Moves the server answered busy go again after AGAIN_AFTER, as they were.
	class Again
	{
	public:
		static constexpr auto        AGAIN_AFTER = std::chrono::seconds(1);
		static constexpr std::size_t MAX_AGAIN = 4096;

		void Add(LootEntry a_move, Clock::time_point a_now)
		{
			if (held.size() >= MAX_AGAIN) {
				return;
			}
			a_move.answer = 0;
			held.push_back({ a_move, a_now + AGAIN_AFTER });
		}

		[[nodiscard]] std::vector<LootEntry> Due(Clock::time_point a_now)
		{
			std::vector<LootEntry> out;
			while (!held.empty() && held.front().second <= a_now) {
				out.push_back(held.front().first);
				held.pop_front();
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return held.size(); }
		void                      Clear() { held.clear(); }

	private:
		std::deque<std::pair<LootEntry, Clock::time_point>> held;
	};

	// How many of a stack a take makes here: as many as the move took, but never more than the copy holds plain (an
	// instance of the item here, tempered or enchanted by a player, is never taken for another game's plain one).
	[[nodiscard]] constexpr std::uint32_t TakeHere(std::uint32_t a_wanted, std::int64_t a_plainHere)
	{
		if (a_plainHere <= 0) {
			return 0;
		}
		return static_cast<std::uint32_t>((std::min<std::int64_t>)(a_wanted, a_plainHere));
	}

	// How many of an item an emptied marker takes out of the copy here: all it holds plain but what it keeps.
	[[nodiscard]] constexpr std::uint32_t EmptyHere(std::int64_t a_plainHere, std::uint32_t a_keep)
	{
		const auto n = a_plainHere - static_cast<std::int64_t>(a_keep);
		return n > 0 ? static_cast<std::uint32_t>((std::min<std::int64_t>)(n, Protocol::MAX_LOOT_COUNT)) : 0;
	}
}
