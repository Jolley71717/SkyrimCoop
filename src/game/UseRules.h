#pragma once

#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// What a friend's use of a mechanism did, made in every game (docs/DESIGN.md, "Doors, locks and mechanisms", "What a use
// did"). Kept free of the game so every rule is tested (tests/UseTests.cpp); src/game/WorldObjects.cpp reads the game
// and makes what these decide.
//
// A friend's use is made in each other game by the friend's stand-in (RefRules, WorldObjects::Used), so the mechanism's
// own script runs there as it ran in the user's game. That is not always the same run: a script may check what its user
// carries (a claw door wants its claw, a keyhole its key, an altar its gem), and the stand-in carries none of it; or it
// may do nothing for anyone but the player. Two things close that, each by what the engine keeps, never by a script's
// name, so a mod's mechanism is covered as the base game's are:
// - the items the mechanism's own scripts name (their properties: an item, a list of items, an alias's item) that the
//   user holds go with the use, and are lent to the stand-in while it makes it (Loans), then taken back;
// - the user's game says what the use did near the mechanism for 10 seconds after it (Capture): the animation events
//   played on activators and movable statics (a claw door's rings and its fall, a pillar's turn, a gate built as an
//   activator), and the references enabled or disabled (a wall that opens a hidden way, a bridge put across). Each other
//   game makes what its own stand-in's use did not (Mirror): an animation no event here matched, a reference not yet so.
// A door's open state and a lock a use moves are shared as every door's are (RefRules), and are not effects here.
namespace UseRules
{
	using Clock = std::chrono::steady_clock;
	using Protocol::LentItem;
	using Protocol::UseEffect;

	// ---- the items lent with a use --------------------------------------------------------------------------------

	// An item's kind, from its base's form type (WorldObjects). A kind an NPC equips, drinks or reads on its own
	// (weapons, armor, ammunition, torches, scrolls, potions) is never lent: a stand-in given one could use it.
	enum class ItemKind : std::uint8_t
	{
		kMisc,        // a claw, a gem, a Dwemer part, a lexicon, a sphere: what mechanisms ask for most
		kKey,
		kSoulGem,
		kBook,        // a note, a journal, an Elder Scroll
		kIngredient,  // last of the kinds lent
		kPotion,
		kEquippable,
		kNotItem,
	};

	constexpr bool Lendable(ItemKind a_kind) { return a_kind <= ItemKind::kIngredient; }

	// A form a mechanism's script names, and how many of it our player holds now.
	struct Named
	{
		std::uint32_t base = 0;
		ItemKind      kind = ItemKind::kNotItem;
		std::int64_t  held = 0;
	};

	struct LendPick
	{
		std::vector<LentItem> lent;
		std::uint32_t         kept = 0;  // held and named, of a kind never lent
		std::uint32_t         cut = 0;   // past MAX_LENT
	};

	// What a use lends: each named form our player holds, of a kind that can be lent, once (named twice: once), with as
	// many as our player holds (at most 0xFFFF), at most Protocol::MAX_LENT. What is not an item is passed over (a
	// script names sounds, messages, quests and references as well).
	inline LendPick PickLent(const std::vector<Named>& a_named)
	{
		LendPick                   out;
		std::vector<std::uint32_t> seen;
		for (const auto& named : a_named) {
			if (named.base == 0 || named.held <= 0 || named.kind == ItemKind::kNotItem || std::ranges::find(seen, named.base) != seen.end()) {
				continue;
			}
			seen.push_back(named.base);
			if (!Lendable(named.kind)) {
				++out.kept;
			} else if (out.lent.size() >= Protocol::MAX_LENT) {
				++out.cut;
			} else {
				out.lent.push_back({ named.base, static_cast<std::uint16_t>((std::min<std::int64_t>)(named.held, 0xFFFF)) });
			}
		}
		return out;
	}

	// Items lent here to stand-ins for a friend's use: taken back LEND_FOR after (the script has checked by then), before
	// a save (no stand-in is in a save, nor what it holds) and, past MAX_LOANS, the oldest at once.
	inline constexpr auto        LEND_FOR = std::chrono::seconds(10);
	inline constexpr std::size_t MAX_LOANS = 128;

	struct Loan
	{
		std::uint32_t     standIn = 0;
		std::uint32_t     base = 0;
		std::uint16_t     count = 0;
		Clock::time_point until{};
	};

	class Loans
	{
	public:
		void Lent(std::uint32_t a_standIn, const std::vector<LentItem>& a_items, Clock::time_point a_now)
		{
			for (const auto& item : a_items) {
				if (item.base != 0 && item.count != 0) {
					loans.push_back({ a_standIn, item.base, item.count, a_now + LEND_FOR });
				}
			}
			while (loans.size() > MAX_LOANS) {
				loans.front().until = a_now;  // due at once
				early.push_back(loans.front());
				loans.erase(loans.begin());
			}
		}

		// The loans due now, out of the book: the game takes them back.
		[[nodiscard]] std::vector<Loan> Due(Clock::time_point a_now)
		{
			auto out = std::exchange(early, {});
			const auto due = std::ranges::partition(loans, [&](const Loan& a_loan) { return a_loan.until > a_now; });
			out.insert(out.end(), due.begin(), due.end());
			loans.erase(due.begin(), due.end());
			return out;
		}

		// Every loan, out of the book (a save).
		[[nodiscard]] std::vector<Loan> All()
		{
			auto out = std::exchange(early, {});
			out.insert(out.end(), loans.begin(), loans.end());
			loans.clear();
			return out;
		}

		// Whether a_standIn holds a loan of a_base now (the container event's sink asks, for each move from a stand-in).
		[[nodiscard]] bool Lends(std::uint32_t a_standIn, std::uint32_t a_base) const
		{
			const auto of = [&](const Loan& a_loan) { return a_loan.standIn == a_standIn && a_loan.base == a_base; };
			return std::ranges::any_of(loans, of) || std::ranges::any_of(early, of);
		}

		// The stand-in's own run of the use moved a_count of a_base out of it (a script handed it on: into a holder, onto
		// the ground, or used it up): how many of those were lent, out of the book (oldest loan first), so they are
		// neither taken back from it later nor counted as gone. Its own items of that base (a stand-in may carry some)
		// are the rest.
		[[nodiscard]] std::uint32_t Moved(std::uint32_t a_standIn, std::uint32_t a_base, std::uint32_t a_count)
		{
			std::uint32_t lent = 0;
			for (auto* book : { &early, &loans }) {
				for (auto& loan : *book) {
					if (lent >= a_count) {
						break;
					}
					if (loan.standIn == a_standIn && loan.base == a_base) {
						const auto n = (std::min<std::uint32_t>)(loan.count, a_count - lent);
						loan.count = static_cast<std::uint16_t>(loan.count - n);
						lent += n;
					}
				}
				std::erase_if(*book, [](const Loan& a_loan) { return a_loan.count == 0; });
			}
			return lent;
		}

		[[nodiscard]] std::size_t Out() const { return loans.size() + early.size(); }
		void                      Clear()
		{
			loans.clear();
			early.clear();
		}

		// How many to take back: what was lent, at most what the stand-in holds now (the script may have taken it).
		[[nodiscard]] static std::int32_t Back(const Loan& a_loan, std::int64_t a_held) { return static_cast<std::int32_t>(std::clamp<std::int64_t>(a_held, 0, a_loan.count)); }

	private:
		std::vector<Loan> loans;
		std::vector<Loan> early;
	};

	// Where a lent item the stand-in's run handed on went, and what this game does about it. The user's game made the
	// same move with the user's own item and says it as that class says our player's moves: a put into a holder the
	// session shares goes through loot sync (LootSync, LootRules::Of), so the holder here gets it from there, and the
	// stand-in's copy is taken back out of it (else the holder would hold two). A put that loot sync leaves each game's
	// own (a key, a book, a living actor, a merchant's chest), a drop onto the ground (a script's drop is no player's
	// drop: DropSync says only those) and an item used up stay as the script left them: this game's run of the script is
	// the only one that makes that move here, as the user's made it there.
	enum class LentWent : std::uint8_t
	{
		kUsedUp,        // to nobody, nothing placed: the script used it up
		kDropped,       // to nobody, placed in the world
		kSharedHolder,  // into a holder whose moves the session shares
		kOwnHolder,     // into a holder each game keeps its own
	};

	[[nodiscard]] constexpr bool TakeOutOfHolder(LentWent a_went) { return a_went == LentWent::kSharedHolder; }

	[[nodiscard]] constexpr const char* LentWentName(LentWent a_went)
	{
		switch (a_went) {
		case LentWent::kUsedUp:
			return "used up";
		case LentWent::kDropped:
			return "dropped";
		case LentWent::kSharedHolder:
			return "put in a holder the session shares";
		case LentWent::kOwnHolder:
			return "put in a holder each game keeps";
		}
		return "?";
	}

	// ---- what our player's use did, in our game -------------------------------------------------------------------

	inline constexpr auto        WATCH_FOR = std::chrono::seconds(10);      // after our player's use
	inline constexpr float       EFFECT_REACH = 2000.0f;                    // u from the mechanism
	inline constexpr auto        SEND_EVERY = std::chrono::milliseconds(500);
	inline constexpr std::size_t MAX_PER_USE = 64;  // effects one use says (past it, counted, not said)
	inline constexpr std::size_t MAX_WATCHED = 4;   // uses watched at once (a fifth ends the oldest's watch)

	// The uses our player made, each watched WATCH_FOR: what happens near its mechanism is its effect (the newest use's,
	// when two are watched; the game decides by reach). Said in batches every SEND_EVERY, the rest when the watch ends.
	class Capture
	{
	public:
		struct Watching
		{
			std::uint32_t seq = 0;
			std::uint32_t mechanism = 0;
		};

		void Begin(std::uint32_t a_seq, std::uint32_t a_mechanism, Clock::time_point a_now)
		{
			if (watches.size() >= MAX_WATCHED) {
				watches.front().until = a_now;  // ends now: what it holds still goes
			}
			watches.push_back({ .seq = a_seq, .mechanism = a_mechanism, .start = a_now, .until = a_now + WATCH_FOR, .lastSent = a_now });
		}

		[[nodiscard]] bool Active(Clock::time_point a_now) const
		{
			return std::ranges::any_of(watches, [&](const Watch& a_watch) { return a_now < a_watch.until; });
		}

		// The uses watched now, newest first.
		[[nodiscard]] std::vector<Watching> Watched(Clock::time_point a_now) const
		{
			std::vector<Watching> out;
			for (auto it = watches.rbegin(); it != watches.rend(); ++it) {
				if (a_now < it->until) {
					out.push_back({ it->seq, it->mechanism });
				}
			}
			return out;
		}

		// An effect of the use a_seq, which happened at a_at: false when that use is not watched then, or has said its
		// most (counted as cut).
		bool Add(std::uint32_t a_seq, std::uint32_t a_ref, std::uint8_t a_what, std::string_view a_event, Clock::time_point a_at)
		{
			const auto it = std::ranges::find_if(watches, [&](const Watch& a_watch) { return a_watch.seq == a_seq && a_at < a_watch.until; });
			if (it == watches.end() || a_ref == 0 || a_what > Protocol::USE_EFFECT_MAX || (a_what == UseEffect::kAnim) == a_event.empty()) {
				return false;
			}
			if (it->said + it->pending.size() >= MAX_PER_USE) {
				++cut;
				return false;
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(a_at - it->start).count();
			UseEffect effect;
			effect.ref = a_ref;
			effect.what = a_what;
			effect.atMs = static_cast<std::uint16_t>(std::clamp<long long>(ms, 0, 0xFFFF));
			if (a_what == UseEffect::kAnim) {
				effect.event = std::string{ a_event.substr(0, Protocol::MAX_ANIM_EVENT) };
			}
			it->pending.push_back(std::move(effect));
			++captured;
			return true;
		}

		// The batches to send now: a watch's new effects once SEND_EVERY has passed since its last, and all it holds
		// when its watch has ended (then it is forgotten). At most Protocol::MAX_USE_EFFECTS a batch.
		[[nodiscard]] std::vector<Protocol::UseEffects> Due(Clock::time_point a_now)
		{
			std::vector<Protocol::UseEffects> out;
			for (auto& watch : watches) {
				const bool ended = a_now >= watch.until;
				if (watch.pending.empty() || (!ended && a_now - watch.lastSent < SEND_EVERY)) {
					continue;
				}
				for (std::size_t i = 0; i < watch.pending.size(); i += Protocol::MAX_USE_EFFECTS) {
					Protocol::UseEffects batch;
					batch.seq = watch.seq;
					batch.mechanism = watch.mechanism;
					const auto end = (std::min)(watch.pending.size(), i + Protocol::MAX_USE_EFFECTS);
					batch.effects.assign(watch.pending.begin() + static_cast<std::ptrdiff_t>(i), watch.pending.begin() + static_cast<std::ptrdiff_t>(end));
					out.push_back(std::move(batch));
				}
				watch.said += watch.pending.size();
				watch.pending.clear();
				watch.lastSent = a_now;
			}
			std::erase_if(watches, [&](const Watch& a_watch) { return a_now >= a_watch.until && a_watch.pending.empty(); });
			batches += out.size();
			return out;
		}

		void Clear() { watches.clear(); }

		std::uint64_t captured = 0;
		std::uint64_t cut = 0;
		std::uint64_t batches = 0;

	private:
		struct Watch
		{
			std::uint32_t          seq = 0;
			std::uint32_t          mechanism = 0;
			Clock::time_point      start{};
			Clock::time_point      until{};
			Clock::time_point      lastSent{};
			std::vector<UseEffect> pending;
			std::size_t            said = 0;
		};
		std::vector<Watch> watches;
	};

	// What a placed reference is, for its enabling or disabling as a use's effect.
	enum class Placed : std::uint8_t
	{
		kNot,        // never an effect: an actor, our player, a reference made as the game ran, a deleted one
		kFixed,      // a wall, a bridge, a light that can't be carried, a static, an activator: both ways
		kCarryable,  // an item (a weapon, a gem, a book, a light that can be carried): its enabling only
	};

	// Whether a reference of that kind going enabled (a_disabled false) or disabled is a use's effect. A carryable item
	// disabled is most often one picked up, which loot and drops own, so only its enabling is said: a script putting out
	// a reward (a gem on a pedestal, a key on a table) is.
	[[nodiscard]] constexpr bool EnableSays(Placed a_kind, bool a_disabled)
	{
		return a_kind == Placed::kFixed || (a_kind == Placed::kCarryable && !a_disabled);
	}

	// Enabled or not, of the references near a watched use, read every scan: a change between two reads in a row is an
	// effect; a reference first read (it came within reach, or the watch began) says nothing. The first read is made as
	// the use is made, in the activate event's sink, before the mechanism's script has run (Baseline).
	class EnableWatch
	{
	public:
		void BeginScan() { ++generation; }

		// A reference read as the use was made: taken as read in the last scan, so the next scan's read of it is compared
		// with it. One read already keeps its read (older, and as good a baseline).
		void Baseline(std::uint32_t a_ref, bool a_disabled)
		{
			if (a_ref != 0) {
				last.try_emplace(a_ref, Entry{ a_disabled, generation });
			}
		}

		bool Seen(std::uint32_t a_ref, bool a_disabled)
		{
			auto [it, added] = last.try_emplace(a_ref, Entry{ a_disabled, generation });
			const bool changed = !added && it->second.generation + 1 == generation && it->second.disabled != a_disabled;
			if (!added && it->second.generation == generation) {
				return false;  // read twice in one scan (two watched uses' reaches meet)
			}
			it->second = Entry{ a_disabled, generation };
			return changed;
		}

		// What this scan did not read is forgotten: read again later, it is a first read.
		void EndScan()
		{
			std::erase_if(last, [&](const auto& a_entry) { return a_entry.second.generation != generation; });
		}

		void                      Clear() { last.clear(); }
		[[nodiscard]] std::size_t Size() const { return last.size(); }

	private:
		struct Entry
		{
			bool          disabled = false;
			std::uint64_t generation = 0;
		};
		std::unordered_map<std::uint32_t, Entry> last;
		std::uint64_t                            generation = 0;
	};

	// ---- what a friend's use did, made here ------------------------------------------------------------------------

	inline constexpr auto        MATCH_WAIT = std::chrono::milliseconds(1500);  // after an effect's moment, for ours to match it
	inline constexpr auto        MATCH_WAIT_MADE = std::chrono::seconds(5);      // ... when our stand-in made the use here
	inline constexpr auto        OWN_SLACK = std::chrono::seconds(1);           // an event of ours this much before the use still matches
	inline constexpr auto        OWN_KEEP = std::chrono::seconds(20);
	inline constexpr auto        DOUBLE_WINDOW = std::chrono::seconds(10);
	inline constexpr auto        USE_KEEP = std::chrono::seconds(60);
	inline constexpr std::size_t MAX_PENDING_EFFECTS = 256;
	inline constexpr std::size_t MAX_OWN = 256;
	inline constexpr std::size_t MAX_USES_KEPT = 64;

	// A friend's uses and what they did: each effect waits until MATCH_WAIT after the moment it happened in the user's
	// game (timed from when the use reached this game, where the stand-in made it), then is made unless this game's own
	// run of the use did it already (an animation event played here on that reference since the use, which it takes).
	// Enables and disables are judged by the game: a reference already so is left alone. When our stand-in made the use
	// here, an effect waits MATCH_WAIT_MADE instead: its own run of the script does it all as a rule, and a modded game's
	// script lag or a slow frame rate makes it seconds late. An animation made here and then sent again by this game's
	// own run within DOUBLE_WINDOW, with no effect waiting for it, is a repeat: the animation hook does not pass it to
	// the graph (Repeat), so a late run never turns a pillar twice. Our player's own use of a mechanism ends that
	// (OurUse): what follows it is our player's doing.
	class Mirror
	{
	public:
		struct Make
		{
			std::uint32_t player = 0;
			std::uint32_t seq = 0;
			UseEffect     effect;
		};

		// A friend's use reached this game now (its stand-in made it, or not): its effects are timed from now here.
		// a_madeHere: our stand-in of them made it (its own run of the script is under way), so its effects wait longer.
		void UseHeard(std::uint32_t a_player, std::uint32_t a_seq, Clock::time_point a_now, bool a_madeHere = false)
		{
			if (uses.size() >= MAX_USES_KEPT) {
				uses.erase(uses.begin());
			}
			uses.push_back({ a_player, a_seq, a_now, a_madeHere });
		}

		// ... our stand-in of them has made it since (the game took the stand-in's activation).
		void MadeHere(std::uint32_t a_player, std::uint32_t a_seq)
		{
			const auto use = std::ranges::find_if(uses, [&](const Use& a_use) { return a_use.player == a_player && a_use.seq == a_seq; });
			if (use != uses.end()) {
				use->madeHere = true;
			}
		}

		// What a friend's use did in its user's game.
		void Heard(const Protocol::UseEffects& a_msg, Clock::time_point a_now)
		{
			const auto use = std::ranges::find_if(uses, [&](const Use& a_use) { return a_use.player == a_msg.playerId && a_use.seq == a_msg.seq; });
			for (const auto& effect : a_msg.effects) {
				++heard;
				if (pending.size() >= MAX_PENDING_EFFECTS) {
					++cut;
					continue;
				}
				const auto at = std::chrono::milliseconds(effect.atMs);
				// a use that never reached this game (its message lost to a budget, or sent before we joined): timed as if
				// it had, when this effect is heard
				const auto base = use != uses.end() ? use->at : a_now - at;
				const Clock::duration wait = use != uses.end() && use->madeHere ? Clock::duration{ MATCH_WAIT_MADE } : Clock::duration{ MATCH_WAIT };
				pending.push_back({ Make{ a_msg.playerId, a_msg.seq, effect }, base, (std::max)(a_now, base + at) + wait });
			}
		}

		// An animation event this game is about to send to a reference's graph, not by our Make: true when it is this
		// game's own repeat of one we made (within DOUBLE_WINDOW, no effect waiting for it), which is then not sent. Each
		// make holds back one repeat (counted as doubled).
		[[nodiscard]] bool Repeat(std::uint32_t a_ref, std::string_view a_event, Clock::time_point a_at)
		{
			const bool awaited = std::ranges::any_of(pending, [&](const Pending& a_p) { return a_p.make.effect.what == UseEffect::kAnim && a_p.make.effect.ref == a_ref && a_p.make.effect.event == a_event; });
			if (awaited) {
				return false;
			}
			const auto made = std::ranges::find_if(madeEvents, [&](const Event& a_e) { return !a_e.used && a_e.ref == a_ref && a_e.event == a_event && a_at >= a_e.at && a_at - a_e.at <= DOUBLE_WINDOW; });
			if (made == madeEvents.end()) {
				return false;
			}
			made->used = true;
			++doubled;
			return true;
		}

		// An animation event played here on a reference (its graph took it), not by our Make: what matches an effect.
		void Played(std::uint32_t a_ref, std::string_view a_event, Clock::time_point a_at)
		{
			if (own.size() >= MAX_OWN) {
				own.erase(own.begin());
			}
			own.push_back({ a_ref, std::string{ a_event }, a_at, false });
		}

		// Both at once: a repeat is held back (true), anything else is played.
		bool Own(std::uint32_t a_ref, std::string_view a_event, Clock::time_point a_at)
		{
			if (Repeat(a_ref, a_event, a_at)) {
				return true;
			}
			Played(a_ref, a_event, a_at);
			return false;
		}

		// Our player used a mechanism: what is sent after it is our player's doing, never a repeat to hold back.
		void OurUse() { madeEvents.clear(); }

		// Due now, in the order they happened: the animations no event of ours matched, and every enable and disable.
		[[nodiscard]] std::vector<Make> Due(Clock::time_point a_now)
		{
			std::erase_if(own, [&](const Event& a_e) { return a_now - a_e.at > OWN_KEEP; });
			std::erase_if(madeEvents, [&](const Event& a_e) { return a_now - a_e.at > DOUBLE_WINDOW; });
			std::erase_if(uses, [&](const Use& a_use) { return a_now - a_use.at > USE_KEEP; });
			const auto due = std::ranges::stable_partition(pending, [&](const Pending& a_p) { return a_p.due > a_now; });
			std::vector<Pending> ready(due.begin(), due.end());
			pending.erase(due.begin(), due.end());
			std::ranges::stable_sort(ready, [](const Pending& a_l, const Pending& a_r) { return a_l.base + std::chrono::milliseconds(a_l.make.effect.atMs) < a_r.base + std::chrono::milliseconds(a_r.make.effect.atMs); });
			std::vector<Make> out;
			for (auto& p : ready) {
				if (p.make.effect.what == UseEffect::kAnim) {
					const auto match = std::ranges::find_if(own, [&](const Event& a_e) {
						return !a_e.used && a_e.ref == p.make.effect.ref && a_e.event == p.make.effect.event && a_e.at + OWN_SLACK >= p.base;
					});
					if (match != own.end()) {
						match->used = true;
						++matched;
						continue;
					}
				}
				++toMake;
				out.push_back(std::move(p.make));
			}
			return out;
		}

		// We are playing this animation (a Make; said before it is sent, so a repeat racing it is caught): one this game
		// itself sends after it, within DOUBLE_WINDOW and with no effect waiting for it, is a repeat (Repeat).
		void Made(std::uint32_t a_ref, std::string_view a_event, Clock::time_point a_now)
		{
			if (madeEvents.size() >= MAX_OWN) {
				madeEvents.erase(madeEvents.begin());
			}
			madeEvents.push_back({ a_ref, std::string{ a_event }, a_now, false });
		}

		// ... and its graph did not take it: nothing played, so no repeat of it is held back.
		void Unmade(std::uint32_t a_ref, std::string_view a_event)
		{
			for (auto it = madeEvents.end(); it != madeEvents.begin();) {
				--it;
				if (!it->used && it->ref == a_ref && it->event == a_event) {
					madeEvents.erase(it);
					return;
				}
			}
		}

		// Whether animation events played here are worth reading now: a friend's use was heard lately, an effect waits, or
		// one was made lately.
		[[nodiscard]] bool Listening(Clock::time_point a_now) const
		{
			return !pending.empty() || !madeEvents.empty() ||
			       std::ranges::any_of(uses, [&](const Use& a_use) { return a_now - a_use.at < WATCH_FOR + MATCH_WAIT + std::chrono::seconds(5); });
		}

		[[nodiscard]] std::size_t Waiting() const { return pending.size(); }

		void Clear()
		{
			uses.clear();
			pending.clear();
			own.clear();
			madeEvents.clear();
		}

		std::uint64_t heard = 0;    // effects heard
		std::uint64_t matched = 0;  // ... done here already by our own run of the use
		std::uint64_t toMake = 0;   // ... handed to the game to make
		std::uint64_t doubled = 0;  // animations made here, then sent again by our own run: held back (Repeat)
		std::uint64_t cut = 0;      // past MAX_PENDING_EFFECTS

	private:
		struct Use
		{
			std::uint32_t     player = 0;
			std::uint32_t     seq = 0;
			Clock::time_point at{};
			bool              madeHere = false;
		};
		struct Pending
		{
			Make              make;
			Clock::time_point base{};
			Clock::time_point due{};
		};
		struct Event
		{
			std::uint32_t     ref = 0;
			std::string       event;
			Clock::time_point at{};
			bool              used = false;
		};
		std::vector<Use>     uses;
		std::vector<Pending> pending;
		std::vector<Event>   own;
		std::vector<Event>   madeEvents;
	};

	// The last animation events read here (the dev command `anims`), whoever played them: ours made for a friend marked.
	inline constexpr std::size_t ANIM_RING = 64;

	class AnimRing
	{
	public:
		struct Entry
		{
			std::uint32_t     ref = 0;
			std::string       event;
			bool              made = false;
			Clock::time_point at{};
		};

		void Add(std::uint32_t a_ref, std::string_view a_event, bool a_made, Clock::time_point a_at)
		{
			if (ring.size() >= ANIM_RING) {
				ring.erase(ring.begin());
			}
			ring.push_back({ a_ref, std::string{ a_event }, a_made, a_at });
			++total;
		}

		// That reference's entries, oldest first.
		[[nodiscard]] std::vector<Entry> Of(std::uint32_t a_ref) const
		{
			std::vector<Entry> out;
			std::ranges::copy_if(ring, std::back_inserter(out), [&](const Entry& a_e) { return a_e.ref == a_ref; });
			return out;
		}

		void Clear() { ring.clear(); }

		std::uint64_t total = 0;

	private:
		std::vector<Entry> ring;
	};
}
