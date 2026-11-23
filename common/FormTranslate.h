#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// Each game's edge between its own form IDs and the session's (FormIds.h, Protocol::PluginTable): every packet a game
// sends goes through Translate(kToSession) and every packet it gets through Translate(kToLocal), before anything reads
// it, so the rest of the game only ever sees its own IDs, and the server only the session's. A form is named on the
// wire by its plugin's place in the session's table, which is its plugin's file name: never by a game's load order.
// With the same plugins in the same places (the usual case) no ID changes, and a packet is written again only when it
// held a runtime form.
//
// What has no ID on the other side (a form of a plugin that game lacks) is skipped, never guessed, and the message it
// was in still goes, for every message carrying forms today:
// - a place (a player state's cell and worldspace, as a pair) becomes "nowhere this game has" (both 0), so the reader
//   shows no one there rather than someone in the wrong cell;
// - an entry of a list is left out: a worn item;
// - a detail becomes 0, which the message reads as none: a race (the stand-in falls back to a race every game has); a
//   clock's worldspace and weather (a weather of a worldspace with none goes with it: nothing is forced from it);
// - the reference a door's state or a mechanism's use is about becomes 0, which no game applies and the server keeps
//   nothing of: a door of a plugin some game lacks is that game's own;
// - an entry of a quest batch whose quest has no ID is left out (a mod's quest some game lacks is that game's own); a
//   batch left with no entry is not sent at all (kDrop), as it could not be read;
// - a move of loot the session passes on (kRemoteLoot) whose holder or item has no ID is left out (a mod's chest or
//   item some game lacks is that game's own), a batch left with none dropped; in a game's own moves going out (kLoot) and
//   in the answers to them (kLootAnswer) each becomes 0 instead, so every move is still answered once (the server
//   refuses a move of no holder);
// - a dropped item (kReportDrop, kDropPlaced, each of a kDropList) whose base, enchantment or poison has no ID on the
//   other side has its base made 0: going out, the server refuses it (the session can't name that item, so it stays its
//   dropper's own); coming in, this game places no copy of it (it would be another item), and still knows the drop, so
//   it hears who takes it. Its place is a place or nowhere (both 0), as a player's state's is: nowhere is placed by none.
// A runtime form (FF, FormIds::IsRuntime: made by one game as it ran) has no ID in any other game, so it is skipped the
// same way, both ways, whatever the two load orders; the sender sends the plugin form it was made from instead where it
// has one (FormIds::Shareable), and a receiver never looks one up.
// Result says what was skipped and which IDs had none; SkipLog names each plugin skipped once. Free of the game:
// tests/FormTranslateTests.cpp. Each new message type is put in CarriesForms (its switch names every type, so the
// compiler points at a type left out), and one that carries a form ID gets its case in Translate.
namespace FormTranslate
{
	enum class Way
	{
		kToSession,  // ours, going out
		kToLocal,    // the session's, coming in
	};

	struct Result
	{
		enum class Kind
		{
			kSame,     // send or read the packet as it is
			kChanged,  // send or read `packet` instead
			kDrop,     // nothing of it is the other side's: neither send nor read it
		};
		Kind                       kind = Kind::kSame;
		std::vector<std::uint8_t>  packet;
		std::uint32_t              entries = 0;      // list entries left out
		std::uint32_t              fields = 0;       // details set to none
		bool                       placeless = false;  // a place set to nowhere this game has
		std::vector<std::uint32_t> unmapped;         // the IDs that had none, in the numbering they came in (the first few)
		std::uint32_t              runtime = 0;      // of the IDs that had none, the runtime forms

		[[nodiscard]] bool Skipped() const { return entries != 0 || fields != 0 || placeless; }
	};

	inline constexpr std::size_t MAX_UNMAPPED_KEPT = 8;

	// Whether a message type carries form IDs: every type is named, so a new one left out is a compiler warning here.
	[[nodiscard]] constexpr bool CarriesForms(Protocol::MessageType a_type)
	{
		using MT = Protocol::MessageType;
		switch (a_type) {
		case MT::kPlayerState:
		case MT::kRemoteState:
		case MT::kPlayerLooks:
		case MT::kRemoteLooks:
		case MT::kRefState:
		case MT::kRemoteRefState:
		case MT::kActivate:
		case MT::kRemoteActivate:
		case MT::kRefRefused:
		case MT::kQuests:
		case MT::kRemoteQuests:
		case MT::kQuestAnswer:
		case MT::kClock:
		case MT::kRemoteClock:
		case MT::kLoot:
		case MT::kRemoteLoot:
		case MT::kLootAnswer:
		case MT::kReportDrop:
		case MT::kDropPlaced:
		case MT::kDropList:
			return true;
		case MT::kClaimDrop:  // drop numbers, no forms
		case MT::kDropGone:
		case MT::kDropRecord:
		case MT::kAskLoot:  // a ledger: numbers, no forms
		case MT::kLootCaughtUp:
		case MT::kHello:  // a load order is plugin names, not form IDs
		case MT::kWelcome:
		case MT::kReject:
		case MT::kPlayerJoined:
		case MT::kPlayerLeft:
		case MT::kHeartbeat:
		case MT::kHeartbeatAck:
		case MT::kPluginTable:
		case MT::kAskRefStates:
		case MT::kAskQuests:
		case MT::kQuestReset:
			return false;
		}
		return false;  // a type this version does not know
	}

	namespace detail
	{
		struct Context
		{
			const FormIds::Mapper& mapper;
			Way                    way;
			Result&                out;
			bool                   changed = false;

			std::optional<std::uint32_t> Map(std::uint32_t a_id)
			{
				auto mapped = way == Way::kToSession ? mapper.ToSession(a_id) : mapper.ToLocal(a_id);
				if (!mapped && FormIds::IsRuntime(a_id)) {
					++out.runtime;
				}
				if (!mapped && out.unmapped.size() < MAX_UNMAPPED_KEPT && std::ranges::find(out.unmapped, a_id) == out.unmapped.end()) {
					out.unmapped.push_back(a_id);
				}
				return mapped;
			}

			// A form the message names (0 stays 0): false when it has no ID on the other side, a_id untouched then.
			bool Key(std::uint32_t& a_id)
			{
				if (a_id == 0) {
					return true;
				}
				const auto mapped = Map(a_id);
				if (!mapped) {
					return false;
				}
				changed = changed || *mapped != a_id;
				a_id = *mapped;
				return true;
			}

			// A detail: none (0) when it has no ID on the other side.
			void Detail(std::uint32_t& a_id)
			{
				if (!Key(a_id)) {
					a_id = 0;
					++out.fields;
					changed = true;
				}
			}

			// The IDs of a list that have one on the other side; the rest are left out.
			void Ids(std::vector<std::uint32_t>& a_list)
			{
				std::vector<std::uint32_t> kept;
				kept.reserve(a_list.size());
				for (auto id : a_list) {
					if (Key(id)) {
						kept.push_back(id);
					}
				}
				const auto left = a_list.size() - kept.size();
				out.entries += static_cast<std::uint32_t>(left);
				changed = changed || left != 0;
				a_list = std::move(kept);
			}

			// A cell and a worldspace, one place: both, or nowhere (both 0) when either has no ID on the other side.
			void Place(std::uint32_t& a_cell, std::uint32_t& a_worldspace)
			{
				auto cell = a_cell;
				auto worldspace = a_worldspace;
				if (Key(cell) && Key(worldspace)) {
					a_cell = cell;
					a_worldspace = worldspace;
					return;
				}
				a_cell = 0;
				a_worldspace = 0;
				out.placeless = true;
				changed = true;
			}
		};

		template <class T, class Decode, class MapFn>
		void Apply(std::span<const std::uint8_t> a_packet, Context& a_ctx, Decode a_decode, MapFn a_map)
		{
			auto msg = a_decode(a_packet);
			if (!msg) {
				return;  // as it is: the reader refuses it as it would have
			}
			T& value = *msg;
			a_map(value);
			if (a_ctx.changed) {
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = Protocol::Encode(static_cast<const T&>(value));
			}
		}

		// A message about one reference, written back with its own type (kRefState or kRemoteRefState, kActivate or
		// kRemoteActivate, kRefRefused): the reference, or 0 when it has none on the other side.
		template <class T, class Decode>
		void ApplyRef(std::span<const std::uint8_t> a_packet, Context& a_ctx, Protocol::MessageType a_type, Decode a_decode)
		{
			auto msg = a_decode(a_packet);
			if (!msg) {
				return;
			}
			a_ctx.Detail(msg->refId);
			if (a_ctx.changed) {
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = Protocol::Encode(static_cast<const T&>(*msg), a_type);
			}
		}

		// A quest batch, written back with its own type: the entries whose quest has an ID on the other side; none left
		// is kDrop (an empty batch is no message).
		inline void ApplyQuests(std::span<const std::uint8_t> a_packet, Context& a_ctx, Protocol::MessageType a_type)
		{
			auto msg = Protocol::DecodeQuestBatch(a_packet);
			if (!msg) {
				return;
			}
			std::vector<Protocol::QuestEntry> kept;
			kept.reserve(msg->entries.size());
			for (auto entry : msg->entries) {
				if (a_ctx.Key(entry.questId)) {
					kept.push_back(entry);
				}
			}
			const auto left = msg->entries.size() - kept.size();
			a_ctx.out.entries += static_cast<std::uint32_t>(left);
			if (kept.empty()) {
				a_ctx.out.kind = Result::Kind::kDrop;
				return;
			}
			if (a_ctx.changed || left != 0) {
				msg->entries = std::move(kept);
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = Protocol::Encode(*msg, a_type);
			}
		}

		// A clock, written back with its own type (kClock or kRemoteClock): its worldspace and its weather, each none (0)
		// when it has no ID on the other side. A weather is said only for a worldspace: with the worldspace gone, the
		// weather goes too (a mod's worldspace another game lacks says nothing of that game's skies).
		inline void ApplyClock(std::span<const std::uint8_t> a_packet, Context& a_ctx, Protocol::MessageType a_type)
		{
			auto msg = Protocol::DecodeClockState(a_packet);
			if (!msg) {
				return;
			}
			a_ctx.Detail(msg->worldspace);
			if (msg->worldspace == 0 && msg->weather != 0) {
				msg->weather = 0;
				++a_ctx.out.fields;
				a_ctx.changed = true;
			} else {
				a_ctx.Detail(msg->weather);
			}
			if (a_ctx.changed) {
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = Protocol::Encode(*msg, a_type);
			}
		}

		// A batch of loot moves, written back with its own type. kRemoteLoot: the moves whose holder and item (an emptied
		// marker has none) have IDs on the other side; none left is kDrop. kLoot and kLootAnswer: every move kept, a form
		// with no ID made 0, so each of a game's moves is answered once.
		inline void ApplyLoot(std::span<const std::uint8_t> a_packet, Context& a_ctx, Protocol::MessageType a_type)
		{
			auto msg = Protocol::DecodeLootBatch(a_packet);
			if (!msg) {
				return;
			}
			if (a_type == Protocol::MessageType::kRemoteLoot) {
				std::vector<Protocol::LootEntry> kept;
				kept.reserve(msg->entries.size());
				for (auto entry : msg->entries) {
					if (a_ctx.Key(entry.holderId) && a_ctx.Key(entry.itemId)) {
						kept.push_back(entry);
					}
				}
				const auto left = msg->entries.size() - kept.size();
				a_ctx.out.entries += static_cast<std::uint32_t>(left);
				if (kept.empty()) {
					a_ctx.out.kind = Result::Kind::kDrop;
					return;
				}
				if (a_ctx.changed || left != 0) {
					msg->entries = std::move(kept);
					a_ctx.out.kind = Result::Kind::kChanged;
					a_ctx.out.packet = Protocol::Encode(*msg, a_type);
				}
				return;
			}
			for (auto& entry : msg->entries) {
				a_ctx.Detail(entry.holderId);
				a_ctx.Detail(entry.itemId);
			}
			if (a_ctx.changed) {
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = Protocol::Encode(*msg, a_type);
			}
		}

		// A dropped item's forms: its base, enchantment and poison all, or its base 0 (this side lacks one of them: another
		// item); its place, or nowhere.
		inline void MapDrop(Context& a_ctx, Protocol::DroppedItem& a_drop)
		{
			auto base = a_drop.base;
			auto enchantment = a_drop.enchantment;
			auto poison = a_drop.poison;
			if (a_ctx.Key(base) && a_ctx.Key(enchantment) && a_ctx.Key(poison)) {
				a_drop.base = base;
				a_drop.enchantment = enchantment;
				a_drop.poison = poison;
			} else if (a_drop.base != 0) {
				a_drop.base = 0;
				++a_ctx.out.fields;
				a_ctx.changed = true;
			}
			a_ctx.Place(a_drop.cell, a_drop.worldspace);
		}

		inline void ApplyDrop(std::span<const std::uint8_t> a_packet, Context& a_ctx, Protocol::MessageType a_type)
		{
			auto msg = Protocol::DecodeDroppedItem(a_packet);
			if (!msg) {
				return;
			}
			MapDrop(a_ctx, *msg);
			if (a_ctx.changed) {
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = Protocol::Encode(*msg, a_type);
			}
		}

		// Every drop of a list is kept (a game removes its copies of the ones a list lacks), each mapped as one.
		inline void ApplyDropList(std::span<const std::uint8_t> a_packet, Context& a_ctx)
		{
			auto msg = Protocol::DecodeDropList(a_packet);
			if (!msg) {
				return;
			}
			for (auto& drop : msg->drops) {
				MapDrop(a_ctx, drop);
			}
			if (a_ctx.changed) {
				a_ctx.out.kind = Result::Kind::kChanged;
				a_ctx.out.packet = Protocol::Encode(*msg);
			}
		}

		inline void MapLooks(Context& a_ctx, Protocol::PlayerLooks& a_looks)
		{
			a_ctx.Detail(a_looks.race);
			a_ctx.Ids(a_looks.worn);
		}
	}

	// a_packet as the other side numbers its forms. A packet that can't be read is left as it is (its reader refuses
	// it as before), and so is every message that names no form. The identity mapper still reads each message that
	// carries forms, for the runtime forms in it (small messages: a state, a looks).
	[[nodiscard]] inline Result Translate(const FormIds::Mapper& a_mapper, Way a_way, std::span<const std::uint8_t> a_packet)
	{
		Result out;
		const auto type = Protocol::PeekType(a_packet);
		if (!type || !CarriesForms(*type)) {
			return out;
		}
		using MT = Protocol::MessageType;
		detail::Context ctx{ a_mapper, a_way, out };
		switch (*type) {
		case MT::kPlayerState:
			detail::Apply<Protocol::PlayerState>(a_packet, ctx, Protocol::DecodePlayerState, [&](Protocol::PlayerState& a_state) { ctx.Place(a_state.cell, a_state.worldspace); });
			break;
		case MT::kRemoteState:
			detail::Apply<Protocol::RemoteState>(a_packet, ctx, Protocol::DecodeRemoteState, [&](Protocol::RemoteState& a_msg) { ctx.Place(a_msg.state.cell, a_msg.state.worldspace); });
			break;
		case MT::kPlayerLooks:
			detail::Apply<Protocol::PlayerLooks>(a_packet, ctx, Protocol::DecodePlayerLooks, [&](Protocol::PlayerLooks& a_looks) { detail::MapLooks(ctx, a_looks); });
			break;
		case MT::kRemoteLooks:
			detail::Apply<Protocol::RemoteLooks>(a_packet, ctx, Protocol::DecodeRemoteLooks, [&](Protocol::RemoteLooks& a_msg) { detail::MapLooks(ctx, a_msg.looks); });
			break;
		case MT::kRefState:
		case MT::kRemoteRefState:
			detail::ApplyRef<Protocol::RefState>(a_packet, ctx, *type, Protocol::DecodeRefState);
			break;
		case MT::kActivate:
		case MT::kRemoteActivate:
			detail::ApplyRef<Protocol::Activation>(a_packet, ctx, *type, Protocol::DecodeActivation);
			break;
		case MT::kRefRefused:
			detail::ApplyRef<Protocol::RefRefusal>(a_packet, ctx, *type, Protocol::DecodeRefRefusal);
			break;
		case MT::kQuests:
		case MT::kRemoteQuests:
		case MT::kQuestAnswer:
			detail::ApplyQuests(a_packet, ctx, *type);
			break;
		case MT::kClock:
		case MT::kRemoteClock:
			detail::ApplyClock(a_packet, ctx, *type);
			break;
		case MT::kLoot:
		case MT::kRemoteLoot:
		case MT::kLootAnswer:
			detail::ApplyLoot(a_packet, ctx, *type);
			break;
		case MT::kReportDrop:
		case MT::kDropPlaced:
			detail::ApplyDrop(a_packet, ctx, *type);
			break;
		case MT::kDropList:
			detail::ApplyDropList(a_packet, ctx);
			break;
		default:
			break;
		}
		return out;
	}

	// The plugins whose forms a session's translations skipped, each named once (a log line per plugin, not per
	// packet: a friend in a mod's cell sends 20 states a second).
	class SkipLog
	{
	public:
		// The plugins a_result skipped forms of that were not named before: by file name, from our own table (kToSession:
		// IDs of ours) or the session's (kToLocal). An ID whose place names no plugin in that table (a table that grew
		// after it was sent, or a peer's mistake) is named as "". Runtime forms are no plugin's: FreshRuntime says them.
		[[nodiscard]] std::vector<std::string> Fresh(const FormIds::Mapper& a_mapper, Way a_way, const Result& a_result)
		{
			std::vector<std::string> out;
			for (const auto id : a_result.unmapped) {
				if (FormIds::IsRuntime(id)) {
					continue;
				}
				auto name = a_way == Way::kToSession ? a_mapper.LocalPlugin(id) : a_mapper.SessionPlugin(id);
				if (named.insert(Protocol::LowerPluginName(name)).second) {
					out.push_back(std::move(name));
				}
			}
			return out;
		}

		// Whether a_result is the first this session to skip a runtime form (said once in the log, as a plugin is).
		[[nodiscard]] bool FreshRuntime(const Result& a_result)
		{
			if (a_result.runtime == 0 || runtimeSaid) {
				return false;
			}
			runtimeSaid = true;
			return true;
		}

		void Clear()
		{
			named.clear();
			runtimeSaid = false;
		}
		[[nodiscard]] std::size_t Count() const { return named.size(); }

	private:
		std::unordered_set<std::string> named;  // lower case
		bool                            runtimeSaid = false;
	};
}
