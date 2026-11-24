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
// - an entry of a list is left out: a worn item, a head part of a face;
// - a detail becomes 0, which the message reads as none: a race (the stand-in falls back to a race every game has), a
//   face's hair colour, complexion or skin (the stand-in keeps its preset's);
// - an NPC the other side lacks is left out of the NPC messages' lists, and so is an NPC's state in a place it lacks.
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
		case MT::kNpcClaims:
		case MT::kNpcOwners:
		case MT::kNpcStates:
		case MT::kNpcActions:
		case MT::kNpcGear:
		case MT::kNpcHits:
		case MT::kNpcHitAnswers:
		case MT::kNpcHealths:
		case MT::kNpcDeaths:
		case MT::kPlayerFace:
		case MT::kRemoteFace:
		case MT::kRides:
		case MT::kShots:
		case MT::kSits:
		case MT::kPairs:
		case MT::kPlayerStatus:
		case MT::kRemoteStatus:
			return true;
		case MT::kHello:  // a load order is plugin names, not form IDs
		case MT::kWelcome:
		case MT::kReject:
		case MT::kPlayerJoined:
		case MT::kPlayerLeft:
		case MT::kHeartbeat:
		case MT::kHeartbeatAck:
		case MT::kPluginTable:
		case MT::kPlayerActions:  // a player's graph events: names, no form
		case MT::kRemoteActions:
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

			// An NPC (Protocol::NpcKey): a load-order reference goes by its plugin, false when it has no ID on the other
			// side; a spawn's id is its spawner's game's own runtime reference, never translated (no other game has it).
			bool Npc(Protocol::NpcKey& a_key)
			{
				return a_key.IsSpawn() || (a_key.id != 0 && Key(a_key.id));
			}

			// A place that must be had whole (an NPC's): false when either half has no ID on the other side.
			bool WholePlace(std::uint32_t& a_cell, std::uint32_t& a_worldspace)
			{
				auto cell = a_cell;
				auto worldspace = a_worldspace;
				if (!Key(cell) || !Key(worldspace)) {
					return false;
				}
				a_cell = cell;
				a_worldspace = worldspace;
				return true;
			}

			// The entries of a list a_keep says have IDs on the other side (mapping them as it goes); the rest are left out.
			template <class T, class Keep>
			void Entries(std::vector<T>& a_list, Keep a_keep)
			{
				std::vector<T> kept;
				kept.reserve(a_list.size());
				for (auto& entry : a_list) {
					if (a_keep(entry)) {
						kept.push_back(std::move(entry));
					}
				}
				const auto left = a_list.size() - kept.size();
				a_list = std::move(kept);
				out.entries += static_cast<std::uint32_t>(left);
				changed = changed || left != 0;
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

		// A race the other side lacks reads 0 (a Nord there), a worn item it lacks is left out, and a held object (each
		// hand's, the ammunition, the voice) it lacks reads 0 with its unnamed bit set: the stand-in keeps what it has
		// there rather than show an empty hand.
		inline void MapLooks(Context& a_ctx, Protocol::PlayerLooks& a_looks)
		{
			a_ctx.Detail(a_looks.race);
			a_ctx.Ids(a_looks.worn);
			const auto held = [&](std::uint32_t& a_id, std::uint8_t a_bit) {
				const bool had = a_id != 0;
				a_ctx.Detail(a_id);
				if (had && a_id == 0) {
					a_looks.unnamed = static_cast<std::uint8_t>(a_looks.unnamed | a_bit);
				}
			};
			held(a_looks.right, Protocol::kRightUnnamed);
			held(a_looks.left, Protocol::kLeftUnnamed);
			held(a_looks.ammo, Protocol::kAmmoUnnamed);
			held(a_looks.voice, Protocol::kVoiceUnnamed);
		}

		// A face: a race the other side lacks reads 0 (the face is then put on no stand-in: it is for that race), a head
		// part it lacks is left out (partTypes still says the friend has one of that kind: the stand-in keeps its preset's
		// there), and a hair colour, complexion or skin it lacks reads 0 (the colour itself travels beside its form; the
		// preset's complexion and skin stay).
		inline void MapFace(Context& a_ctx, Protocol::PlayerFace& a_face)
		{
			a_ctx.Detail(a_face.race);
			a_ctx.Ids(a_face.headParts);
			a_ctx.Detail(a_face.hairColor);
			a_ctx.Detail(a_face.faceDetails);
			a_ctx.Detail(a_face.skin);
		}

		inline void MapStatus(Context& a_ctx, Protocol::PlayerStatus& a_status)
		{
			a_ctx.Detail(a_status.location);
			if (a_status.mapWorld != 0 && !a_ctx.Key(a_status.mapWorld)) {
				a_status.mapWorld = 0;
				a_status.mapX = a_status.mapY = a_status.mapZ = 0.0f;
				++a_ctx.out.fields;
				a_ctx.changed = true;
			}
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
		case MT::kPlayerFace:
			detail::Apply<Protocol::PlayerFace>(a_packet, ctx, Protocol::DecodePlayerFace, [&](Protocol::PlayerFace& a_face) { detail::MapFace(ctx, a_face); });
			break;
		case MT::kRemoteFace:
			detail::Apply<Protocol::RemoteFace>(a_packet, ctx, Protocol::DecodeRemoteFace, [&](Protocol::RemoteFace& a_msg) { detail::MapFace(ctx, a_msg.face); });
			break;
		// NPCs: one the other side lacks (a mod's) is left out of every list; a state somewhere the other side lacks is
		// left out too (no copy can stand there); a base it lacks reads 0 (a spawn of it can't be placed); gear of an NPC it
		// lacks names no NPC (id 0, which the reader skips), and worn items or held objects it lacks are left out or 0.
		case MT::kNpcClaims:
			detail::Apply<Protocol::NpcClaims>(a_packet, ctx, Protocol::DecodeNpcClaims, [&](Protocol::NpcClaims& a_msg) {
				ctx.Entries(a_msg.claims, [&](Protocol::NpcClaim& a_claim) { return ctx.Npc(a_claim.key); });
			});
			break;
		case MT::kNpcOwners:
			detail::Apply<Protocol::NpcOwners>(a_packet, ctx, Protocol::DecodeNpcOwners, [&](Protocol::NpcOwners& a_msg) {
				ctx.Entries(a_msg.owners, [&](Protocol::NpcOwner& a_owner) { return ctx.Npc(a_owner.key); });
			});
			break;
		case MT::kNpcStates:
			detail::Apply<Protocol::NpcStates>(a_packet, ctx, Protocol::DecodeNpcStates, [&](Protocol::NpcStates& a_msg) {
				ctx.Entries(a_msg.npcs, [&](Protocol::NpcState& a_npc) {
					if (!ctx.Npc(a_npc.key) || !ctx.WholePlace(a_npc.state.cell, a_npc.state.worldspace)) {
						return false;
					}
					ctx.Detail(a_npc.base);
					return true;
				});
			});
			break;
		case MT::kNpcActions:
			detail::Apply<Protocol::NpcActions>(a_packet, ctx, Protocol::DecodeNpcActions, [&](Protocol::NpcActions& a_msg) {
				ctx.Entries(a_msg.actions, [&](Protocol::NpcAction& a_action) { return ctx.Npc(a_action.key); });
			});
			break;
		case MT::kNpcGear:
			detail::Apply<Protocol::NpcGear>(a_packet, ctx, Protocol::DecodeNpcGear, [&](Protocol::NpcGear& a_msg) {
				if (!ctx.Npc(a_msg.key)) {
					a_msg.key = {};
					++ctx.out.fields;
					ctx.changed = true;
				}
				// A worn item with no ID on the other side is left out, its body slots said (the copy keeps its own
				// there); a held one is 0, the hand said to hold something unnamed (not an empty hand).
				std::vector<Protocol::WornItem> kept;
				kept.reserve(a_msg.worn.size());
				for (auto item : a_msg.worn) {
					if (ctx.Key(item.id)) {
						kept.push_back(item);
					} else {
						a_msg.unnamedSlots |= item.slots;
						++ctx.out.entries;
						ctx.changed = true;
					}
				}
				a_msg.worn = std::move(kept);
				const auto held = [&](std::uint32_t& a_id, std::uint8_t a_bit) {
					const bool had = a_id != 0;
					ctx.Detail(a_id);
					if (had && a_id == 0) {
						a_msg.unnamed |= a_bit;
					}
				};
				held(a_msg.right, Protocol::kRightUnnamed);
				held(a_msg.left, Protocol::kLeftUnnamed);
				held(a_msg.ammo, Protocol::kAmmoUnnamed);
				held(a_msg.voice, Protocol::kVoiceUnnamed);
			});
			break;
		// NPC health and deaths: an NPC the other side lacks is left out of each list (it has no copy to hurt or kill).
		case MT::kNpcHits:
			detail::Apply<Protocol::NpcHits>(a_packet, ctx, Protocol::DecodeNpcHits, [&](Protocol::NpcHits& a_msg) {
				ctx.Entries(a_msg.hits, [&](Protocol::NpcHit& a_hit) { return ctx.Npc(a_hit.key); });
			});
			break;
		case MT::kNpcHitAnswers:
			detail::Apply<Protocol::NpcHitAnswers>(a_packet, ctx, Protocol::DecodeNpcHitAnswers, [&](Protocol::NpcHitAnswers& a_msg) {
				ctx.Entries(a_msg.answers, [&](Protocol::NpcHitAnswer& a_answer) { return ctx.Npc(a_answer.key); });
			});
			break;
		case MT::kNpcHealths:
			detail::Apply<Protocol::NpcHealths>(a_packet, ctx, Protocol::DecodeNpcHealths, [&](Protocol::NpcHealths& a_msg) {
				ctx.Entries(a_msg.npcs, [&](Protocol::NpcHealth& a_npc) { return ctx.Npc(a_npc.key); });
			});
			break;
		case MT::kNpcDeaths:
			detail::Apply<Protocol::NpcDeaths>(a_packet, ctx, Protocol::DecodeNpcDeaths, [&](Protocol::NpcDeaths& a_msg) {
				ctx.Entries(a_msg.deaths, [&](Protocol::NpcDeath& a_death) { return ctx.Npc(a_death.key); });
			});
			break;
		// Rides: an NPC rider the other side lacks is left out (it has no copy to seat); a mount it lacks becomes none (id
		// 0), the ride kept, so the rider is shown on foot rather than not at all.
		case MT::kRides:
			detail::Apply<Protocol::Rides>(a_packet, ctx, Protocol::DecodeRides, [&](Protocol::Rides& a_msg) {
				ctx.Entries(a_msg.rides, [&](Protocol::Ride& a_ride) { return a_ride.player != 0 || ctx.Npc(a_ride.rider); });
				for (auto& ride : a_msg.rides) {
					if (ride.mount.id != 0 && !ctx.Npc(ride.mount)) {
						ride.mount = {};
						++ctx.out.fields;
						ctx.changed = true;
					}
				}
			});
			break;
		// Shots: one whose record or NPC the other side lacks is left out (nothing to launch, or no copy to launch it
		// from); a source it lacks (a mod's bow, a player-made poison) becomes none, and the shot still flies.
		case MT::kShots:
			detail::Apply<Protocol::Shots>(a_packet, ctx, Protocol::DecodeShots, [&](Protocol::Shots& a_msg) {
				ctx.Entries(a_msg.shots, [&](Protocol::Shot& a_shot) { return (a_shot.player != 0 || ctx.Npc(a_shot.npc)) && ctx.Key(a_shot.projectile); });
				for (auto& shot : a_msg.shots) {
					for (auto* id : { &shot.weapon, &shot.ammo, &shot.spell, &shot.enchant, &shot.poison }) {
						ctx.Detail(*id);
					}
				}
			});
			break;
		// Furniture: an NPC the other side lacks is left out (it has no copy to sit down); a reference it lacks (a mod's,
		// or one made as the game ran) becomes none and its base, if it has that, finds it by place (SitRules::Pick); a
		// base it lacks becomes none too, and the friend is shown standing beside it.
		case MT::kSits:
			detail::Apply<Protocol::Sits>(a_packet, ctx, Protocol::DecodeSits, [&](Protocol::Sits& a_msg) {
				ctx.Entries(a_msg.sits, [&](Protocol::Sit& a_sit) { return a_sit.player != 0 || ctx.Npc(a_sit.rider); });
				for (auto& sit : a_msg.sits) {
					ctx.Detail(sit.furniture);
					ctx.Detail(sit.base);
				}
			});
			break;
		// Paired animations: one whose attacker or victim is an NPC the other side lacks is left out (no copy to play it
		// on); an idle record it lacks (a mod's kill move) becomes none, and the reader finds the idle by its event.
		case MT::kPairs:
			detail::Apply<Protocol::Pairs>(a_packet, ctx, Protocol::DecodePairs, [&](Protocol::Pairs& a_msg) {
				const auto side = [&](Protocol::PairSide& a_side) { return a_side.player != 0 || ctx.Npc(a_side.npc); };
				ctx.Entries(a_msg.pairs, [&](Protocol::Pair& a_pair) { return side(a_pair.attacker) && side(a_pair.victim); });
				for (auto& pair : a_msg.pairs) {
					ctx.Detail(pair.idle);
				}
			});
			break;
		// A status: a place it lacks (a mod's town) reads none, and a map world it lacks takes the map spot with it (none:
		// the friend is not drawn on a map this game has).
		case MT::kPlayerStatus:
			detail::Apply<Protocol::PlayerStatus>(a_packet, ctx, Protocol::DecodePlayerStatus, [&](Protocol::PlayerStatus& a_status) { detail::MapStatus(ctx, a_status); });
			break;
		case MT::kRemoteStatus:
			detail::Apply<Protocol::RemoteStatus>(a_packet, ctx, Protocol::DecodeRemoteStatus, [&](Protocol::RemoteStatus& a_msg) { detail::MapStatus(ctx, a_msg.status); });
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
