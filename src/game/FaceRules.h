#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A friend's face on their stand-in (docs/DESIGN.md, "Players": "Its face"): the rules free of the game. Which tint mask
// of this game's race a friend's tint layer is, which head parts a stand-in gets when some of its friend's are from a mod
// this game lacks, whether a face goes on a stand-in at all, when our own changed face is sent, how many copies of a
// preset a game run makes, how a stand-in's head is watched once it is placed, and how what a stand-in shows is compared
// with what its friend sent. The game's side is src/game/Face.cpp and src/game/RemotePlayers.cpp. No game here:
// tests/FaceTests.cpp.
namespace FaceRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// ---- tint masks ----

	// A texture's path as every game names it: case and slash kind ignored, a leading "textures\" or slash dropped (the
	// game keeps paths relative to Data\Textures, a mod's record may name it from Data). FNV-1a of that.
	[[nodiscard]] inline std::uint32_t PathHash(std::string_view a_path)
	{
		std::string path;
		path.reserve(a_path.size());
		for (const char c : a_path) {
			const auto lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
			path.push_back(lower == '/' ? '\\' : lower);
		}
		std::string_view view{ path };
		while (!view.empty() && view.front() == '\\') {
			view.remove_prefix(1);
		}
		if (view.starts_with("textures\\")) {
			view.remove_prefix(9);
		}
		if (view.empty()) {
			return 0;
		}
		std::uint32_t hash = 2166136261u;
		for (const char c : view) {
			hash ^= static_cast<std::uint8_t>(c);
			hash *= 16777619u;
		}
		return hash;
	}

	// One of this game's race's tint masks (for the face's sex): its index and its texture's PathHash.
	struct RaceTint
	{
		std::uint16_t index = 0;
		std::uint32_t path = 0;
	};

	// The index of this game's race's mask that a friend's tint layer is: the same index with the same texture (the
	// usual case), else the mask with that texture under another index (a race a mod renumbered); none when this race has
	// no mask of that texture (a tint mod the friend has): the layer is left out, never laid on another mask.
	[[nodiscard]] inline std::optional<std::uint16_t> TintIndexFor(const Protocol::FaceTint& a_tint, std::span<const RaceTint> a_race)
	{
		if (a_tint.path == 0) {
			return std::nullopt;
		}
		for (const auto& mask : a_race) {
			if (mask.index == a_tint.index && mask.path == a_tint.path) {
				return mask.index;
			}
		}
		for (const auto& mask : a_race) {
			if (mask.path == a_tint.path) {
				return mask.index;
			}
		}
		return std::nullopt;
	}

	// ---- head parts ----

	// A head part as this game has it: its id and its type (BGSHeadPart::HeadPartType, below Protocol::HEAD_PART_TYPES).
	struct Part
	{
		std::uint32_t id = 0;
		std::uint8_t  type = 0;
		bool operator==(const Part&) const = default;
	};

	[[nodiscard]] constexpr std::uint8_t TypeBit(std::uint8_t a_type)
	{
		return a_type < Protocol::HEAD_PART_TYPES ? static_cast<std::uint8_t>(1u << a_type) : std::uint8_t{ 0 };
	}

	// The kinds of head part a list holds (Protocol::PlayerFace::partTypes).
	[[nodiscard]] inline std::uint8_t TypesOf(std::span<const Part> a_parts)
	{
		std::uint8_t types = 0;
		for (const auto& part : a_parts) {
			types = static_cast<std::uint8_t>(types | TypeBit(part.type));
		}
		return types;
	}

	struct PartPlan
	{
		std::vector<std::uint32_t> ids;  // what the stand-in's head is built from
		std::uint8_t               fellBack = 0;  // kinds its friend has that came with none this game has: the preset's
	};

	// A stand-in's head parts: each of its friend's that this game has (a_friend: those the face named, looked up here;
	// one of no kind a game knows is left out), then for each kind its friend has (a_types) that came with none (a hair
	// or a pair of eyes from a mod this game lacks), the preset's of that kind, so it never shows no eyes or no head. A
	// kind its friend has none of (no beard, bald) stays without, whatever the preset has. At most
	// Protocol::MAX_FACE_HEAD_PARTS, none twice.
	[[nodiscard]] inline PartPlan PlanParts(std::span<const Part> a_friend, std::uint8_t a_types, std::span<const Part> a_preset)
	{
		PartPlan plan;
		const auto add = [&](const Part& a_part) {
			if (TypeBit(a_part.type) != 0 && a_part.id != 0 && plan.ids.size() < Protocol::MAX_FACE_HEAD_PARTS && std::ranges::find(plan.ids, a_part.id) == plan.ids.end()) {
				plan.ids.push_back(a_part.id);
			}
		};
		for (const auto& part : a_friend) {
			add(part);
		}
		const auto came = TypesOf(a_friend);
		for (std::uint8_t type = 0; type < Protocol::HEAD_PART_TYPES; ++type) {
			const auto bit = TypeBit(type);
			if ((a_types & bit) == 0 || (came & bit) != 0) {
				continue;
			}
			plan.fellBack = static_cast<std::uint8_t>(plan.fellBack | bit);
			for (const auto& part : a_preset) {
				if (part.type == type) {
					add(part);
				}
			}
		}
		return plan;
	}

	// ---- whether a face goes on a stand-in ----

	enum class Fit : std::uint8_t
	{
		kOwn,         // its friend's face, on a copy of the preset
		kNoFace,      // none came yet: the preset's face
		kNoRace,      // the face's race is one this game lacks (it read 0), or none: the preset's
		kOtherRace,   // the face is for another race than the stand-in's (its looks changed race, its face not yet)
		kOtherSex,    // or another sex
		kGivenUp,     // its own face showed no head here: the preset's for the rest of the session
		kFull,        // this game run made all the copies it may: the preset's
		kCount,
	};

	inline constexpr std::string_view FIT_NAMES[] = { "own", "noface", "norace", "otherrace", "othersex", "givenup", "full" };
	static_assert(std::size(FIT_NAMES) == static_cast<std::size_t>(Fit::kCount));

	[[nodiscard]] constexpr std::string_view FitName(Fit a_fit)
	{
		const auto i = static_cast<std::size_t>(a_fit);
		return i < std::size(FIT_NAMES) ? FIT_NAMES[i] : "?";
	}

	// a_presetRace / a_presetFemale: the stand-in's preset, as this game picked it from the looks (a Nord for a race this
	// game lacks). The face goes on only when it is for exactly that race and sex.
	[[nodiscard]] inline Fit Fits(const std::optional<Protocol::PlayerFace>& a_face, std::uint32_t a_presetRace, bool a_presetFemale, bool a_givenUp, bool a_full)
	{
		if (!a_face) {
			return Fit::kNoFace;
		}
		if (a_face->race == 0 || FormIds::IsRuntime(a_face->race)) {
			return Fit::kNoRace;
		}
		if (a_face->race != a_presetRace) {
			return Fit::kOtherRace;
		}
		if (a_face->female != a_presetFemale) {
			return Fit::kOtherSex;
		}
		if (a_givenUp) {
			return Fit::kGivenUp;
		}
		return a_full ? Fit::kFull : Fit::kOwn;
	}

	// A face's fingerprint (FNV-1a of its encoding): the same face, the same number, in every game with the same plugins.
	[[nodiscard]] inline std::uint32_t Hash(const Protocol::PlayerFace& a_face)
	{
		const auto    bytes = Protocol::Encode(a_face);
		std::uint32_t hash = 2166136261u;
		for (std::size_t i = 1; i < bytes.size(); ++i) {
			hash ^= bytes[i];
			hash *= 16777619u;
		}
		return hash == 0 ? 1 : hash;  // 0 is "no face of its own"
	}

	// ---- sending our own ----

	// Our face is read each second, on a timer of its own (Session: nothing else makes a read come early). The first read
	// of a session goes at once (the others have none of ours yet); after that, a face that changed goes once two reads in
	// a row agree, so a slider dragged in a barber's chair sends where it stops, not every step of the way. While a menu
	// that edits faces is open (the race menu, which face editor mods use too, or any menu that pauses the game), nothing
	// goes: a face being edited is not sent at every slider stop, and goes once, at the first read after the menu closes.
	class Settle
	{
	public:
		// The face to send now, if any (a_sent: what this session already has of ours; a_editing: a face editor is open).
		[[nodiscard]] std::optional<Protocol::PlayerFace> Next(const Protocol::PlayerFace& a_read, const std::optional<Protocol::PlayerFace>& a_sent, bool a_editing = false)
		{
			if (a_editing) {
				editing = true;
				last.reset();
				return std::nullopt;
			}
			const bool closed = std::exchange(editing, false);
			const bool steady = last && *last == a_read;
			last = a_read;
			if (a_sent && *a_sent == a_read) {
				return std::nullopt;
			}
			if (!a_sent || steady || closed) {
				return a_read;
			}
			return std::nullopt;
		}

		void Reset()
		{
			last.reset();
			editing = false;
		}

		[[nodiscard]] bool Editing() const { return editing; }

	private:
		std::optional<Protocol::PlayerFace> last;
		bool                                editing = false;
	};

	// ---- copies of presets ----

	// A stand-in with its friend's face stands on a copy of its preset made at run time (a form the game never frees).
	// Each copy holds one face at a time, that of one friend. A stand-in placed again for the same face goes on the copy
	// that holds it. A new face is written into a copy of the same preset that no actor stands on any more (a_inUse: no
	// stand-in here, and none on its way out, has it as its base), the one written longest ago; only when every copy of
	// that preset is in use is a new one made. So the copies made follow how many stand-ins show a face at once, not how
	// often faces change (a friend at a barber's, an editor's slider stops). At most MAX_MADE are made in a game run, after
	// which a stand-in that needs a new copy gets its preset's face. None is kept across a load: a load may free forms
	// made before it, so the game forgets them all then (Forget) and makes new ones.
	class CopyBook
	{
	public:
		static constexpr std::size_t MAX_MADE = 64;

		using InUse = std::function<bool(std::uintptr_t)>;

		// The copy that holds a_player's face of that hash now.
		[[nodiscard]] std::optional<std::uintptr_t> Find(std::uint32_t a_player, std::uint32_t a_hash) const
		{
			for (const auto& entry : entries) {
				if (entry.player == a_player && entry.hash == a_hash) {
					return entry.copy;
				}
			}
			return std::nullopt;
		}

		// A copy of a_preset no actor stands on, to write a new face into: the one written longest ago.
		[[nodiscard]] std::optional<std::uintptr_t> Free(std::uintptr_t a_preset, const InUse& a_inUse) const
		{
			const Entry* best = nullptr;
			for (const auto& entry : entries) {
				if (entry.preset == a_preset && (!a_inUse || !a_inUse(entry.copy)) && (!best || entry.written < best->written)) {
					best = &entry;
				}
			}
			return best ? std::optional{ best->copy } : std::nullopt;
		}

		[[nodiscard]] bool CanMake() const { return made < MAX_MADE; }

		// A new copy of a_preset, holding a_player's face a_hash.
		void Made(std::uint32_t a_player, std::uint32_t a_hash, std::uintptr_t a_preset, std::uintptr_t a_copy)
		{
			++made;
			entries.push_back({ a_player, a_hash, a_preset, a_copy, ++writes });
		}

		// A copy this book has (Free) now holds a_player's face a_hash instead of what it held.
		void Rewritten(std::uintptr_t a_copy, std::uint32_t a_player, std::uint32_t a_hash)
		{
			for (auto& entry : entries) {
				if (entry.copy == a_copy) {
					entry.player = a_player;
					entry.hash = a_hash;
					entry.written = ++writes;
					++rewrites;
					return;
				}
			}
		}

		// Whether a base is one of the copies made (since the last load).
		[[nodiscard]] bool IsCopy(std::uintptr_t a_base) const
		{
			return a_base != 0 && std::ranges::any_of(entries, [&](const Entry& a_entry) { return a_entry.copy == a_base; });
		}

		// Before a load: none is placed again or written into. What was made still counts towards MAX_MADE.
		void Forget() { entries.clear(); }

		[[nodiscard]] std::size_t Made() const { return made; }
		[[nodiscard]] std::size_t Kept() const { return entries.size(); }
		[[nodiscard]] std::size_t Rewrites() const { return rewrites; }

	private:
		struct Entry
		{
			std::uint32_t  player = 0;
			std::uint32_t  hash = 0;
			std::uintptr_t preset = 0;
			std::uintptr_t copy = 0;
			std::uint64_t  written = 0;
		};
		std::vector<Entry> entries;
		std::size_t        made = 0;
		std::size_t        rewrites = 0;
		std::uint64_t      writes = 0;
	};

	// ---- watching a stand-in's own head ----

	// The sister project's FaceWatch: a stand-in placed with its friend's face, its 3D loaded and no face node on it for
	// NO_HEAD_FOR, is asked to build its 3D again once (kRebuild); with still none NO_HEAD_FOR after that, its friend's face
	// is given up for the session (kGiveUp: placed again with the preset's). And our own player's face node gone for
	// OURS_GONE_FOR while it is loaded (seen once by the sister project: a copy and the player seemed to share one face)
	// gives it up too (kGiveUpOurs), and ours is asked back. Ours is judged only while it can have a face node at all
	// (oursJudged): read from its third-person 3D, in the race it last had a head in (not a werewolf's or a vampire lord's
	// form, which have no face; the sister project skipped power armor the same way), not in first person, and not while
	// a face editor rebuilds it.
	class Watch
	{
	public:
		static constexpr auto NO_HEAD_FOR = 3s;
		static constexpr auto OURS_GONE_FOR = 2s;

		enum class Step : std::uint8_t
		{
			kNone,
			kBuilt,  // its head is there (said once)
			kRebuild,
			kGiveUp,
			kGiveUpOurs,
		};

		struct Facts
		{
			bool loaded = false;   // its 3D is loaded
			bool head = false;     // a face node on it
			bool oursLoaded = false;
			bool oursHead = false;
			bool oursJudged = false;  // ours can have a face node now (above): only then is its going counted
		};

		Step Next(const Facts& a_facts, Clock::time_point a_now)
		{
			if (done) {
				return Step::kNone;
			}
			if (!a_facts.loaded) {
				noHeadSince.reset();
				oursGoneSince.reset();
				return Step::kNone;
			}
			if (a_facts.oursLoaded && a_facts.oursJudged && !a_facts.oursHead) {
				if (!oursGoneSince) {
					oursGoneSince = a_now;
				} else if (a_now - *oursGoneSince >= OURS_GONE_FOR) {
					done = true;
					return Step::kGiveUpOurs;
				}
			} else {
				oursGoneSince.reset();
			}
			if (a_facts.head) {
				noHeadSince.reset();
				if (!built) {
					built = true;
					return Step::kBuilt;
				}
				return Step::kNone;
			}
			if (!noHeadSince) {
				noHeadSince = a_now;
				return Step::kNone;
			}
			if (a_now - *noHeadSince < NO_HEAD_FOR) {
				return Step::kNone;
			}
			if (!rebuilt) {
				rebuilt = true;
				noHeadSince = a_now;
				return Step::kRebuild;
			}
			done = true;
			return Step::kGiveUp;
		}

		[[nodiscard]] bool Built() const { return built; }
		[[nodiscard]] bool Rebuilt() const { return rebuilt; }
		[[nodiscard]] std::string_view State() const { return done ? "done" : built ? "built" : rebuilt ? "rebuilt" : noHeadSince ? "nohead" : "waiting"; }

	private:
		std::optional<Clock::time_point> noHeadSince;
		std::optional<Clock::time_point> oursGoneSince;
		bool                             built = false;
		bool                             rebuilt = false;
		bool                             done = false;
	};

	// ---- what a stand-in's head shows ----

	// A hair tint as the engine set it on a head's hair (a colour 0-1 a channel, or 0-2 at the scale the library's
	// UpdateHairColor uses) against the colour a face asked for (Protocol::PackColor, its alpha left out): the same at
	// either scale, within two steps of 255 a channel.
	[[nodiscard]] inline bool HairTintSame(std::uint32_t a_rgb, float a_red, float a_green, float a_blue)
	{
		const float want[3] = { static_cast<float>(a_rgb & 0xFF), static_cast<float>((a_rgb >> 8) & 0xFF), static_cast<float>((a_rgb >> 16) & 0xFF) };
		const float got[3] = { a_red, a_green, a_blue };
		for (const float scale : { 255.0f, 128.0f }) {
			bool same = true;
			for (std::size_t i = 0; i < 3; ++i) {
				same = same && std::isfinite(got[i]) && std::abs(got[i] * scale - want[i]) <= 2.01f;
			}
			if (same) {
				return true;
			}
		}
		return false;
	}

	// What a stand-in's built head shows, read from its 3D (Face::Shown3D): under its face node, a node named by each
	// head part of the copy it stands on (the engine names each part's geometry by the part's editor id, which the
	// library's Actor::GetHeadPartObject relies on), none of its preset's parts the copy left out (the preset's face would
	// show those), and its hair tinted the colour that came.
	struct Head3D
	{
		bool          face = false;        // a face node at all
		std::size_t   parts = 0;           // the copy's head parts
		std::size_t   partsFound = 0;      // of those, a node named by it
		std::size_t   presetLeft = 0;      // the preset's parts the copy does not have
		std::size_t   presetFound = 0;     // of those, a node named by it: the preset's face
		bool          hairWanted = false;  // a hair part and a colour to look for
		bool          hairSeen = false;    // a hair-tinted geometry under the hair's node
		bool          hairSame = false;    // its tint the colour that came (HairTintSame)
		std::uint32_t hairWant = 0;
		float         hairGot[3] = { 0.0f, 0.0f, 0.0f };
		std::size_t   others = 0;          // the face node's children named by no part of the copy (said, not judged)
		std::string   othersNames;

		[[nodiscard]] bool All() const
		{
			return face && parts > 0 && partsFound == parts && presetFound == 0 && (!hairWanted || (hairSeen && hairSame));
		}
	};

	// As the developer channel says it: "shown3d=1 face3d=1 parts3d=7/7 presetShown=0/2 hairTint=1 others3d=0".
	[[nodiscard]] inline std::string Line(const Head3D& a_head)
	{
		std::string hair = !a_head.hairWanted ? "none" : !a_head.hairSeen ? "unseen" : a_head.hairSame ? "1" : "0";
		if (a_head.hairWanted && a_head.hairSeen && !a_head.hairSame) {
			char got[64];
			std::snprintf(got, sizeof(got), "(want=%06X,got=%.3f/%.3f/%.3f)", static_cast<unsigned>(a_head.hairWant & 0x00FFFFFF), a_head.hairGot[0], a_head.hairGot[1], a_head.hairGot[2]);
			hair += got;
		}
		return std::string{ "shown3d=" } + (a_head.All() ? "1" : "0") + " face3d=" + (a_head.face ? "1" : "0") + " parts3d=" + std::to_string(a_head.partsFound) + "/" +
		       std::to_string(a_head.parts) + " presetShown=" + std::to_string(a_head.presetFound) + "/" + std::to_string(a_head.presetLeft) + " hairTint=" + hair +
		       " others3d=" + std::to_string(a_head.others) + (a_head.othersNames.empty() ? std::string{} : "(" + a_head.othersNames + ")");
	}

	// ---- what a stand-in shows against what came ----

	// How much of a friend's face (a_sent, as it came to this game) the stand-in's base holds (a_shown, read off it): the
	// rig's check, and the developer channel's.
	struct Agreement
	{
		std::size_t partsSent = 0;
		std::size_t partsShown = 0;   // of the parts sent, those the base has
		std::size_t morphsSame = 0;   // of Protocol::FACE_MORPHS
		std::size_t presetsSame = 0;  // of Protocol::FACE_PRESETS
		std::size_t tintsSent = 0;
		std::size_t tintsShown = 0;   // of the tints sent, those the base has (same texture, colour and strength)
		bool        hair = false;     // the same hair colour
		bool        height = false;
		bool        bodyTint = false;
		bool        details = false;  // the complexion: the same, or none sent
		bool        skin = false;     // the skin: the same, or none sent

		[[nodiscard]] bool All() const
		{
			return partsShown == partsSent && morphsSame == Protocol::FACE_MORPHS && presetsSame == Protocol::FACE_PRESETS && tintsShown == tintsSent && hair && height &&
			       bodyTint && details && skin;
		}
	};

	[[nodiscard]] inline Agreement Compare(const Protocol::PlayerFace& a_sent, const Protocol::PlayerFace& a_shown)
	{
		Agreement out;
		out.partsSent = a_sent.headParts.size();
		out.partsShown = static_cast<std::size_t>(std::ranges::count_if(a_sent.headParts, [&](std::uint32_t a_id) { return std::ranges::find(a_shown.headParts, a_id) != a_shown.headParts.end(); }));
		for (std::size_t i = 0; i < Protocol::FACE_MORPHS; ++i) {
			out.morphsSame += std::bit_cast<std::uint32_t>(a_sent.morphs[i]) == std::bit_cast<std::uint32_t>(a_shown.morphs[i]) ? 1 : 0;
		}
		for (std::size_t i = 0; i < Protocol::FACE_PRESETS; ++i) {
			out.presetsSame += a_sent.presets[i] == a_shown.presets[i] ? 1 : 0;
		}
		out.tintsSent = a_sent.tints.size();
		out.tintsShown = static_cast<std::size_t>(std::ranges::count_if(a_sent.tints, [&](const Protocol::FaceTint& a_tint) {
			return std::ranges::any_of(a_shown.tints, [&](const Protocol::FaceTint& a_has) { return a_has.path == a_tint.path && a_has.color == a_tint.color && a_has.value == a_tint.value; });
		}));
		out.hair = a_sent.hairRgb == a_shown.hairRgb;
		out.height = std::bit_cast<std::uint32_t>(a_sent.height) == std::bit_cast<std::uint32_t>(a_shown.height);
		out.bodyTint = a_sent.bodyTint == a_shown.bodyTint;
		out.details = a_sent.faceDetails == 0 || a_sent.faceDetails == a_shown.faceDetails;
		out.skin = a_sent.skin == 0 || a_sent.skin == a_shown.skin;
		return out;
	}

	// As the developer channel says it: "parts=7/7 morphs=19 presets=4 tints=12/12 hair=1 height=1 body=1 details=1 skin=1".
	[[nodiscard]] inline std::string Line(const Agreement& a_agreement)
	{
		return "parts=" + std::to_string(a_agreement.partsShown) + "/" + std::to_string(a_agreement.partsSent) + " morphs=" + std::to_string(a_agreement.morphsSame) +
		       " presets=" + std::to_string(a_agreement.presetsSame) + " tints=" + std::to_string(a_agreement.tintsShown) + "/" + std::to_string(a_agreement.tintsSent) +
		       " hair=" + (a_agreement.hair ? "1" : "0") + " height=" + (a_agreement.height ? "1" : "0") + " body=" + (a_agreement.bodyTint ? "1" : "0") +
		       " details=" + (a_agreement.details ? "1" : "0") + " skin=" + (a_agreement.skin ? "1" : "0");
	}
}
