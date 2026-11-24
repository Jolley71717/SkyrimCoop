#pragma once

#include "Protocol.h"
#include "game/LootRules.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The game-free half of giving items by dropping them (docs/DESIGN.md, "Dropped items"; the game side is
// src/game/DropSync, the server's book common/DropStore.h): what may be given, when a dropped item has come to rest,
// where each game shows its copy, our player's claims and takes, our drops the server has not confirmed, and what our
// world holds of the session's drops (the co-save record the server makes the session agree with). Ported from the
// sister project's DropBook, with Skyrim's items. Kept free of the game so it can be tested (tests/DropTests.cpp).
//
// The class is every item a player can drop, judged by its form type and its extras, never by a name, so a mod's item
// is given as the base game's are: weapons and armor (tempered, enchanted, poisoned, renamed: each travels with it),
// ammo, potions and food, ingredients, misc items and gold, soul gems (with their soul), scrolls, torches.
namespace DropRules
{
	using Clock = std::chrono::steady_clock;
	using ItemKind = LootRules::ItemKind;

	// ---- what may be given ------------------------------------------------------------------------------------------

	struct ItemFacts
	{
		ItemKind      kind = ItemKind::kOther;
		std::uint32_t count = 0;
		bool          named = false;               // the engine's event named the reference the item became
		bool          created = false;             // ... made at run time (FF): a drop, not a plugin's reference
		bool          runtimeBase = false;         // its base form was made at run time (a potion a player brewed)
		bool          runtimeEnchantment = false;  // an enchantment a player made (a runtime form no other game has)
		bool          runtimePoison = false;       // a poison a player brewed
		bool          quest = false;               // a quest alias holds it
		bool          scripted = false;            // no drop menu was open and the dev channel did not drop it: a script's
		                                           // DropObject (a weapon rack, a mannequin, a mod placing our gear)
	};

	enum class Givable : std::uint8_t
	{
		kYes,
		kNoReference,  // the engine's event named no reference: nothing to take out of our world for the copy
		kScripted,     // a script dropped it (DropObject with no drop menu open): the script keeps the reference (a rack
		               // holds it, a mod placed it) and would be left holding a deleted one; it stays an ordinary item
		kPlaced,       // a plugin's own reference (picked up and dropped again): its pickup is not shared yet, so a
		               // friend's world still has it where the plugin placed it, and a copy would be a second
		kNotItem,      // not an item a player carries (or a kind this class doesn't know)
		kOwn,          // keys and books: each player's own, as in containers (LootRules::ItemScope)
		kQuest,        // a quest's item
		kRuntime,      // its base, its enchantment or its poison was made at run time: no other game has it
		kCount,        // no count, or more than one reference holds
	};
	inline constexpr std::size_t GIVABLE_COUNT = static_cast<std::size_t>(Givable::kCount) + 1;

	[[nodiscard]] constexpr Givable Judge(const ItemFacts& a_item)
	{
		if (!a_item.named) {
			return Givable::kNoReference;
		}
		if (a_item.scripted) {
			return Givable::kScripted;
		}
		if (!a_item.created) {
			return Givable::kPlaced;
		}
		if (a_item.kind == ItemKind::kOther) {
			return Givable::kNotItem;
		}
		if (a_item.kind == ItemKind::kKey || a_item.kind == ItemKind::kBook) {
			return Givable::kOwn;
		}
		if (a_item.quest) {
			return Givable::kQuest;
		}
		if (a_item.runtimeBase || a_item.runtimeEnchantment || a_item.runtimePoison) {
			return Givable::kRuntime;
		}
		if (a_item.count == 0 || a_item.count > Protocol::MAX_DROP_COUNT) {
			return Givable::kCount;
		}
		return Givable::kYes;
	}

	[[nodiscard]] constexpr const char* GivableName(Givable a_why)
	{
		switch (a_why) {
		case Givable::kYes:
			return "given";
		case Givable::kNoReference:
			return "noref";
		case Givable::kScripted:
			return "scripted";
		case Givable::kPlaced:
			return "placed";
		case Givable::kNotItem:
			return "notitem";
		case Givable::kOwn:
			return "own";
		case Givable::kQuest:
			return "quest";
		case Givable::kRuntime:
			return "runtime";
		case Givable::kCount:
			return "count";
		}
		return "?";
	}

	// ---- when it has come to rest -------------------------------------------------------------------------------------

	// A dropped item falls and rolls: it is reported where it comes to rest, so every copy lies where the dropper sees
	// it. At rest: still for STILL_FOR, and no sooner than MIN_WAIT after the drop; or MAX_WAIT after it, wherever it is
	// (an item rocking on a slope).
	inline constexpr auto MIN_WAIT = std::chrono::milliseconds(1500);
	inline constexpr auto STILL_FOR = std::chrono::milliseconds(500);
	inline constexpr auto MAX_WAIT = std::chrono::seconds(5);
	// Moved: by more than this since the last look (units).
	inline constexpr float STILL_UNITS = 2.0f;

	[[nodiscard]] constexpr bool Settled(Clock::duration a_sinceDrop, Clock::duration a_sinceMoved)
	{
		return (a_sinceDrop >= MIN_WAIT && a_sinceMoved >= STILL_FOR) || a_sinceDrop >= MAX_WAIT;
	}

	// ---- where each game shows its copy -------------------------------------------------------------------------------

	// A copy is placed while our player is in the same place (the same interior cell, or the same worldspace) within
	// PLACE_WITHIN, and removed once they are past REMOVE_PAST or elsewhere: the gap keeps a player standing at the edge
	// from placing and removing it every frame.
	inline constexpr float PLACE_WITHIN = 5000.0f;
	inline constexpr float REMOVE_PAST = 7000.0f;

	enum class CopyWant : std::uint8_t
	{
		kKeep,    // as it is (placed or not)
		kPlace,   // place it now
		kRemove,  // remove it now
	};

	// a_sameSpace: our player is where the item lies; a_distance: how far from it; a_placed: our copy is in the world;
	// a_placeable: this game has every form of the item (FormTranslate left its base) and knows the place.
	[[nodiscard]] constexpr CopyWant Want(bool a_placeable, bool a_sameSpace, float a_distance, bool a_placed)
	{
		if (!a_placeable) {
			return a_placed ? CopyWant::kRemove : CopyWant::kKeep;
		}
		if (a_placed) {
			return !a_sameSpace || a_distance > REMOVE_PAST ? CopyWant::kRemove : CopyWant::kKeep;
		}
		return a_sameSpace && a_distance <= PLACE_WITHIN ? CopyWant::kPlace : CopyWant::kKeep;
	}

	// ---- our player's claims -------------------------------------------------------------------------------------------

	// Our player pressed Take (activated) a copy: one claim per item until the server answers, at most MAX_OUT at once
	// (a held key doesn't send one a frame). An unanswered claim may go again after CLAIM_RETRY (an answer lost with a
	// reconnect).
	class Claims
	{
	public:
		static constexpr auto        CLAIM_RETRY = std::chrono::seconds(5);
		static constexpr std::size_t MAX_OUT = 8;

		// Whether to claim a_dropId now.
		bool Start(std::uint32_t a_dropId, Clock::time_point a_now)
		{
			if (a_dropId == 0) {
				return false;
			}
			if (const auto it = out.find(a_dropId); it != out.end()) {
				if (a_now - it->second < CLAIM_RETRY) {
					return false;
				}
				it->second = a_now;
				return true;
			}
			if (out.size() >= MAX_OUT) {
				return false;
			}
			out.emplace(a_dropId, a_now);
			return true;
		}

		// The server answered (the item went, to us or another). True when it was ours to wait for.
		bool Answered(std::uint32_t a_dropId) { return out.erase(a_dropId) != 0; }

		[[nodiscard]] bool        Waiting(std::uint32_t a_dropId) const { return out.contains(a_dropId); }
		[[nodiscard]] std::size_t Size() const { return out.size(); }
		void                      Clear() { out.clear(); }

	private:
		std::map<std::uint32_t, Clock::time_point> out;
	};

	// ---- a take being finished ----------------------------------------------------------------------------------------

	// A take the server granted us: our game picks its copy up as our player's own take, and checks both that the copy
	// left the world and that our player's count of its base rose by the drop's count. Picked up again every RETRY_EVERY
	// while the copy still lies there, at most TRIES times; then it is given back to the session (kGiveBack).
	inline constexpr auto          TAKE_RETRY_EVERY = std::chrono::milliseconds(500);
	inline constexpr std::uint32_t TAKE_TRIES = 4;
	// How long after the copy left the world the count may take to rise (the engine adds it in its own task).
	inline constexpr auto TAKE_SETTLE = std::chrono::seconds(2);

	enum class TakeState : std::uint8_t
	{
		kDone,      // the copy left the world and the count rose: ours
		kWait,      // not yet: look again next frame
		kRetry,     // the copy still lies there: pick it up again now
		kGiveBack,  // tried TAKE_TRIES times and it still lies there: the session gets it back
		kUnsure,    // the copy left the world, but the count did not rise in TAKE_SETTLE: kept as ours, counted
	};

	struct TakeFacts
	{
		bool            copyInWorld = true;
		std::int64_t    before = 0;  // our player's count of the base when the take began
		std::int64_t    now = 0;
		std::uint32_t   count = 0;
		std::uint32_t   tries = 1;                // pick-ups made so far
		Clock::duration sinceTry{};               // since the last one
		std::optional<Clock::duration> sinceGone;  // since the copy was first seen gone
	};

	[[nodiscard]] constexpr TakeState JudgeTake(const TakeFacts& a_take)
	{
		if (!a_take.copyInWorld) {
			if (a_take.now >= a_take.before + static_cast<std::int64_t>(a_take.count)) {
				return TakeState::kDone;
			}
			return a_take.sinceGone && *a_take.sinceGone >= TAKE_SETTLE ? TakeState::kUnsure : TakeState::kWait;
		}
		if (a_take.sinceTry < TAKE_RETRY_EVERY) {
			return TakeState::kWait;
		}
		return a_take.tries >= TAKE_TRIES ? TakeState::kGiveBack : TakeState::kRetry;
	}

	// ---- our drops the server hasn't confirmed ------------------------------------------------------------------------

	// Our drops our game took out of its world (kPlaced sent) that the server hasn't yet said lie there. The server forgets
	// an offer whose dropper leaves before their kPlaced reaches it, so if we leave first the item would be in no world:
	// our game gives each one left here back to our world instead (and our record no longer says it left). The server
	// tells us once it lies there (kDropPlaced, as everyone), or by its list or someone's take of it.
	class Unconfirmed
	{
	public:
		static constexpr std::size_t MAX = 64;

		// False when full (the drop is not taken out of our world then: kKept).
		bool Swapped(const Protocol::DroppedItem& a_drop)
		{
			if (out.size() >= MAX && !out.contains(a_drop.dropId)) {
				return false;
			}
			out[a_drop.dropId] = a_drop;
			return true;
		}

		// The server says it lies there (or lay there: someone took it).
		bool Heard(std::uint32_t a_dropId) { return out.erase(a_dropId) != 0; }

		// We left: the ones never heard of, in the order they were dropped, to be given back to our world.
		std::vector<Protocol::DroppedItem> TakeAll()
		{
			std::vector<Protocol::DroppedItem> all;
			for (auto& [id, drop] : out) {
				all.push_back(std::move(drop));
			}
			out.clear();
			return all;
		}

		[[nodiscard]] bool        Waiting(std::uint32_t a_dropId) const { return out.contains(a_dropId); }
		[[nodiscard]] std::size_t Size() const { return out.size(); }
		void                      Clear() { out.clear(); }

	private:
		std::map<std::uint32_t, Protocol::DroppedItem> out;
	};

	// ---- what our world holds of the session's drops ------------------------------------------------------------------

	// Per session (a save may have played in more than one), what the drops did to our world: the drops whose item left it
	// (by our game's nonce) and the drops it took in (by drop ID). Saved in the co-save with each save, read back with it,
	// and sent to the server after every welcome and load (Protocol::DropRecord). The oldest session past MAX_SESSIONS,
	// and the oldest entry of a session past Protocol::MAX_DROP_RECORD, are forgotten.
	class Record
	{
	public:
		static constexpr std::size_t   MAX_SESSIONS = 8;
		static constexpr std::uint16_t VERSION = 1;

		// We are in a_session now (its welcome): its entry is the one changed and sent, the newest.
		void Bind(std::uint64_t a_session)
		{
			current = a_session;
			if (a_session == 0) {
				return;
			}
			const auto it = std::ranges::find(sessions, a_session, &Session::id);
			if (it != sessions.end()) {
				auto session = std::move(*it);
				sessions.erase(it);
				sessions.push_back(std::move(session));
				return;
			}
			sessions.push_back(Session{ a_session, {}, {} });
			while (sessions.size() > MAX_SESSIONS) {
				sessions.pop_front();
			}
		}
		[[nodiscard]] std::uint64_t Current() const { return current; }

		// An item we dropped left our world (our copy of it is the session's now).
		void Made(std::uint64_t a_nonce) { Add(Entry()->made, a_nonce); }
		// An item we dropped is back in our world after all (we left before the server confirmed it, or it went back
		// with us as we left).
		void Unmade(std::uint64_t a_nonce) { Erase(Entry()->made, a_nonce); }
		// ... in a_session's entry, bound or not: an item given back to our world after we left a_session (the record is
		// bound to no session by then, and that entry is the one a rejoin or a later save sends). Nothing if not kept.
		void Unmade(std::uint64_t a_session, std::uint64_t a_nonce)
		{
			if (const auto s = Find(a_session)) {
				Erase(s->made, a_nonce);
			}
		}
		// We took a drop: it is in our world.
		void Took(std::uint32_t a_dropId) { Add(Entry()->taken, a_dropId); }
		// A take we could not finish: it is not in our world.
		void Untook(std::uint32_t a_dropId) { Erase(Entry()->taken, a_dropId); }

		[[nodiscard]] bool HasMade(std::uint64_t a_nonce) const
		{
			const auto s = Find(current);
			return s && std::ranges::find(s->made, a_nonce) != s->made.end();
		}
		[[nodiscard]] bool HasTaken(std::uint32_t a_dropId) const
		{
			const auto s = Find(current);
			return s && std::ranges::find(s->taken, a_dropId) != s->taken.end();
		}

		// The record of the session we are in, for the server.
		[[nodiscard]] Protocol::DropRecord ForSession() const
		{
			Protocol::DropRecord record{ .sessionId = current, .made = {}, .taken = {} };
			if (const auto s = Find(current)) {
				record.made.assign(s->made.begin(), s->made.end());
				record.taken.assign(s->taken.begin(), s->taken.end());
			}
			return record;
		}

		[[nodiscard]] bool Empty() const { return sessions.empty(); }
		[[nodiscard]] std::size_t Sessions() const { return sessions.size(); }

		// The co-save's bytes: the version, the sessions (oldest first), each its id and its lists.
		[[nodiscard]] std::vector<std::uint8_t> Save() const
		{
			std::vector<std::uint8_t> out;
			Put(out, VERSION);
			Put(out, static_cast<std::uint16_t>(sessions.size()));
			for (const auto& s : sessions) {
				Put(out, s.id);
				Put(out, static_cast<std::uint16_t>(s.made.size()));
				for (const auto nonce : s.made) {
					Put(out, nonce);
				}
				Put(out, static_cast<std::uint16_t>(s.taken.size()));
				for (const auto id : s.taken) {
					Put(out, id);
				}
			}
			return out;
		}

		// The record a save held (Save's bytes). False, the record empty, for bytes that can't be read: the world is then
		// taken to hold none of any session's drops.
		bool Load(std::span<const std::uint8_t> a_bytes)
		{
			sessions.clear();
			current = 0;
			std::size_t   at = 0;
			std::uint16_t version = 0;
			std::uint16_t count = 0;
			if (!Get(a_bytes, at, version) || version != VERSION || !Get(a_bytes, at, count) || count > MAX_SESSIONS) {
				return false;
			}
			for (std::uint16_t i = 0; i < count; ++i) {
				Session       s;
				std::uint16_t n = 0;
				if (!Get(a_bytes, at, s.id) || !Get(a_bytes, at, n) || n > Protocol::MAX_DROP_RECORD) {
					sessions.clear();
					return false;
				}
				for (std::uint16_t j = 0; j < n; ++j) {
					std::uint64_t nonce = 0;
					if (!Get(a_bytes, at, nonce)) {
						sessions.clear();
						return false;
					}
					s.made.push_back(nonce);
				}
				if (!Get(a_bytes, at, n) || n > Protocol::MAX_DROP_RECORD) {
					sessions.clear();
					return false;
				}
				for (std::uint16_t j = 0; j < n; ++j) {
					std::uint32_t id = 0;
					if (!Get(a_bytes, at, id)) {
						sessions.clear();
						return false;
					}
					s.taken.push_back(id);
				}
				sessions.push_back(std::move(s));
			}
			if (at != a_bytes.size()) {
				sessions.clear();
				return false;
			}
			return true;
		}

		void Clear()
		{
			sessions.clear();
			current = 0;
		}

	private:
		struct Session
		{
			std::uint64_t             id = 0;
			std::deque<std::uint64_t> made;
			std::deque<std::uint32_t> taken;
		};

		Session* Entry()
		{
			if (current == 0) {
				return &unbound;  // no session: nothing kept
			}
			const auto it = std::ranges::find(sessions, current, &Session::id);
			if (it != sessions.end()) {
				return &*it;
			}
			Bind(current);
			return &sessions.back();
		}
		[[nodiscard]] const Session* Find(std::uint64_t a_id) const
		{
			if (a_id == 0) {
				return nullptr;
			}
			const auto it = std::ranges::find(sessions, a_id, &Session::id);
			return it != sessions.end() ? &*it : nullptr;
		}
		Session* Find(std::uint64_t a_id)
		{
			if (a_id == 0) {
				return nullptr;
			}
			const auto it = std::ranges::find(sessions, a_id, &Session::id);
			return it != sessions.end() ? &*it : nullptr;
		}

		template <class T>
		static void Add(std::deque<T>& a_list, T a_value)
		{
			if (std::ranges::find(a_list, a_value) != a_list.end()) {
				return;
			}
			a_list.push_back(a_value);
			while (a_list.size() > Protocol::MAX_DROP_RECORD) {
				a_list.pop_front();
			}
		}
		template <class T>
		static void Erase(std::deque<T>& a_list, T a_value)
		{
			std::erase(a_list, a_value);
		}

		template <class T>
		static void Put(std::vector<std::uint8_t>& a_out, T a_value)
		{
			for (std::size_t i = 0; i < sizeof(T); ++i) {
				a_out.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(a_value) >> (8 * i)) & 0xFF));
			}
		}
		template <class T>
		static bool Get(std::span<const std::uint8_t> a_in, std::size_t& a_at, T& a_value)
		{
			if (a_at + sizeof(T) > a_in.size()) {
				return false;
			}
			std::uint64_t v = 0;
			for (std::size_t i = 0; i < sizeof(T); ++i) {
				v |= static_cast<std::uint64_t>(a_in[a_at + i]) << (8 * i);
			}
			a_value = static_cast<T>(v);
			a_at += sizeof(T);
			return true;
		}

		std::deque<Session> sessions;
		std::uint64_t       current = 0;
		Session             unbound;
	};

	// ---- what lies in the session, as this game heard it ------------------------------------------------------------

	// The drops lying in the session as this game heard them (kDropPlaced to all, a kDropList, gone with kDropGone), so a
	// copy is kept of each. A list being heard replaces the book once its last packet comes (BeginList, Listed, EndList):
	// the drops it lacks are gone. A drop heard placed while a list is coming is kept too (it may have been placed after
	// the server wrote the list).
	class Lying
	{
	public:
		void Placed(const Protocol::DroppedItem& a_drop)
		{
			drops[a_drop.dropId] = a_drop;
			if (listing) {
				listed.insert(a_drop.dropId);
			}
		}
		// Gone: true when it lay here.
		bool Gone(std::uint32_t a_dropId)
		{
			listed.erase(a_dropId);
			return drops.erase(a_dropId) != 0;
		}
		void BeginList()
		{
			if (!listing) {
				listing = true;
				listed.clear();
			}
		}
		void Listed(const Protocol::DroppedItem& a_drop)
		{
			BeginList();
			Placed(a_drop);
		}
		// The list's last packet came: the drops it did not name are gone, each returned.
		std::vector<std::uint32_t> EndList()
		{
			std::vector<std::uint32_t> gone;
			for (auto it = drops.begin(); it != drops.end();) {
				if (!listed.contains(it->first)) {
					gone.push_back(it->first);
					it = drops.erase(it);
				} else {
					++it;
				}
			}
			listing = false;
			listed.clear();
			return gone;
		}

		[[nodiscard]] const Protocol::DroppedItem* Find(std::uint32_t a_dropId) const
		{
			const auto it = drops.find(a_dropId);
			return it != drops.end() ? &it->second : nullptr;
		}
		// Our own drops lying there (ownerId a_us), for the give-back as we leave.
		[[nodiscard]] std::vector<Protocol::DroppedItem> OwnedBy(std::uint32_t a_us) const
		{
			std::vector<Protocol::DroppedItem> out;
			for (const auto& [id, drop] : drops) {
				if (drop.ownerId == a_us) {
					out.push_back(drop);
				}
			}
			return out;
		}
		[[nodiscard]] const std::map<std::uint32_t, Protocol::DroppedItem>& All() const { return drops; }
		[[nodiscard]] std::size_t Size() const { return drops.size(); }
		void Clear()
		{
			drops.clear();
			listed.clear();
			listing = false;
		}

	private:
		std::map<std::uint32_t, Protocol::DroppedItem> drops;
		std::unordered_set<std::uint32_t>              listed;
		bool                                           listing = false;
	};

	// ---- an offer heard ---------------------------------------------------------------------------------------------------

	// What our game does with a kDropPlaced (Offer): a_ours: its owner is us and its nonce one we reported and are still
	// waiting on (a_waiting); a_stillThere: our item still lies where it came to rest (not picked up again).
	enum class Offer : std::uint8_t
	{
		kRefused,  // dropId 0: not given; our item stays ours, an ordinary one
		kSwap,     // the offer: take our item out of our world, record it, say kPlaced, show the session's copy
		kKept,     // the offer, but our player picked it up again: kKept, it never lay anywhere
		kLies,     // it lies there for everyone (ours confirmed, or anyone's): keep a copy of it
	};

	[[nodiscard]] constexpr Offer HearPlaced(std::uint32_t a_dropId, bool a_ours, bool a_waiting, bool a_stillThere)
	{
		if (a_ours && a_waiting) {
			if (a_dropId == 0) {
				return Offer::kRefused;
			}
			return a_stillThere ? Offer::kSwap : Offer::kKept;
		}
		return a_dropId == 0 ? Offer::kRefused : Offer::kLies;
	}
}
