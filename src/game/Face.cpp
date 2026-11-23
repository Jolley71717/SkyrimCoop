#include "game/Face.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/FaceRules.h"

namespace Face
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		FaceRules::CopyBook                                book;
		std::unordered_map<const RE::TESNPC*, RE::TESNPC*> presets;  // copy -> the preset it was made from (until a load)
		// Hair colours made for friends whose colour this game has no form of (a face editor's own colour, or a mod's):
		// one per colour, kept for the game run (made by the factory, never given a form id, so no load frees them).
		std::unordered_map<std::uint32_t, RE::BGSColorForm*> colors;
		constexpr std::size_t                                MAX_COLORS = 64;

		std::uint64_t refused = 0;    // copies the game did not make
		std::uint64_t rebuilds = 0;   // stand-ins' 3D built again for a head that did not come
		std::uint64_t repairs = 0;    // our own head asked back
		std::uint64_t tintsLeft = 0;  // friends' tint layers no mask of this game's race has (put on no copy)
		std::uint64_t ourTintsLeft = 0;  // our tint masks our race has no mask of (not sent)
		std::uint64_t facesSent = 0;
		std::uint32_t sentHash = 0;
		bool          toldRefused = false;

		// What `faces nudge` changed on our player, to put back.
		struct Nudged
		{
			float              morph = 0.0f;
			RE::BGSColorForm*  hair = nullptr;
		};
		std::optional<Nudged> nudged;

		std::uint32_t IdOf(const RE::TESForm* a_form)
		{
			return a_form ? a_form->GetFormID() : 0;
		}

		std::uint32_t Pack(const RE::Color& a_color)
		{
			return Protocol::PackColor(a_color.red, a_color.green, a_color.blue, a_color.alpha);
		}

		RE::Color Unpack(std::uint32_t a_color)
		{
			RE::Color color;
			color.red = static_cast<std::uint8_t>(a_color & 0xFF);
			color.green = static_cast<std::uint8_t>((a_color >> 8) & 0xFF);
			color.blue = static_cast<std::uint8_t>((a_color >> 16) & 0xFF);
			color.alpha = static_cast<std::uint8_t>((a_color >> 24) & 0xFF);
			return color;
		}

		RE::TESRace::FaceRelatedData* FaceDataOf(const RE::TESRace* a_race, bool a_female)
		{
			return a_race ? a_race->faceRelatedData[a_female ? RE::SEXES::kFemale : RE::SEXES::kMale] : nullptr;
		}

		// The race's tint masks for a sex, as FaceRules names them (index, texture path).
		std::vector<FaceRules::RaceTint> RaceTints(const RE::TESRace* a_race, bool a_female)
		{
			std::vector<FaceRules::RaceTint> out;
			const auto data = FaceDataOf(a_race, a_female);
			if (!data || !data->tintMasks) {
				return out;
			}
			for (const auto asset : *data->tintMasks) {
				if (asset) {
					out.push_back({ asset->texture.index, FaceRules::PathHash(asset->texture.file.textureName.c_str()) });
				}
			}
			return out;
		}

		const RE::TESRace::FaceRelatedData::TintAsset* RaceTint(const RE::TESRace* a_race, bool a_female, std::uint16_t a_index)
		{
			const auto data = FaceDataOf(a_race, a_female);
			if (!data || !data->tintMasks) {
				return nullptr;
			}
			for (const auto asset : *data->tintMasks) {
				if (asset && asset->texture.index == a_index) {
					return asset;
				}
			}
			return nullptr;
		}

		std::uint8_t TintType(const RE::TESRace::FaceRelatedData::TintAsset* a_asset)
		{
			const auto type = a_asset ? static_cast<std::uint32_t>(a_asset->texture.skinTone.underlying()) : 0u;
			return static_cast<std::uint8_t>(type < Protocol::TINT_TYPES ? type : 0u);
		}

		float Unset()
		{
			return std::bit_cast<float>(Protocol::FACE_UNSET);
		}

		// Everything but the tints, which differ for the player (its own masks) and an NPC (its layers).
		Protocol::PlayerFace ReadCommon(const RE::TESNPC* a_npc, const RE::TESRace* a_race)
		{
			Protocol::PlayerFace face;
			face.race = IdOf(a_race);
			face.female = a_npc->GetSex() == RE::SEX::kFemale;
			for (std::int8_t i = 0; a_npc->headParts && i < a_npc->numHeadParts; ++i) {
				const auto part = a_npc->headParts[i];
				if (!part || face.headParts.size() >= Protocol::MAX_FACE_HEAD_PARTS) {
					continue;
				}
				face.headParts.push_back(part->GetFormID());
				face.partTypes = static_cast<std::uint8_t>(face.partTypes | FaceRules::TypeBit(static_cast<std::uint8_t>(part->type.underlying())));
			}
			if (const auto head = a_npc->headRelatedData) {
				if (head->hairColor) {
					face.hairColor = head->hairColor->GetFormID();
					face.hairRgb = Pack(head->hairColor->color);
				}
				face.faceDetails = IdOf(head->faceDetails);
			}
			face.skin = IdOf(a_npc->skin);
			face.height = std::isfinite(a_npc->height) ? std::clamp(a_npc->height, 0.25f, 4.0f) : 1.0f;
			for (std::size_t i = 0; i < Protocol::FACE_MORPHS; ++i) {
				const float v = a_npc->faceData ? a_npc->faceData->morphs[i] : Unset();
				face.morphs[i] = std::isfinite(v) ? v : Unset();
			}
			for (std::size_t i = 0; i < Protocol::FACE_PRESETS; ++i) {
				const auto v = a_npc->faceData ? static_cast<std::uint32_t>(a_npc->faceData->parts[i]) : Protocol::FACE_UNSET;
				face.presets[i] = Protocol::FacePresetOk(v) ? v : Protocol::FACE_UNSET;
			}
			face.bodyTint = Pack(a_npc->bodyTintColor);
			return face;
		}

		// An NPC's tint layers (TESNPC's TINI/TINC/TINV), each named by its race's mask.
		void ReadLayers(const RE::TESNPC* a_npc, const RE::TESRace* a_race, Protocol::PlayerFace& a_face)
		{
			if (!a_npc->tintLayers) {
				return;
			}
			for (const auto layer : *a_npc->tintLayers) {
				if (!layer || a_face.tints.size() >= Protocol::MAX_FACE_TINTS) {
					continue;
				}
				const auto asset = RaceTint(a_race, a_face.female, layer->tintIndex);
				if (!asset) {
					continue;
				}
				a_face.tints.push_back({ .index = layer->tintIndex,
					.path = FaceRules::PathHash(asset->texture.file.textureName.c_str()),
					.color = Pack(layer->tintColor),
					.value = static_cast<std::uint8_t>((std::min)(layer->interpolationValue, std::uint16_t{ 100 })),
					.type = TintType(asset) });
			}
		}

		// A colour form of exactly that colour: the game's when it has one (the race's own hair colours), else one made.
		RE::BGSColorForm* ColorFor(std::uint32_t a_form, std::uint32_t a_rgb)
		{
			const auto form = a_form != 0 && !FormIds::IsRuntime(a_form) ? RE::TESForm::LookupByID<RE::BGSColorForm>(a_form) : nullptr;
			if (form && Pack(form->color) == a_rgb) {
				return form;
			}
			if (const auto it = colors.find(a_rgb); it != colors.end()) {
				return it->second;
			}
			if (colors.size() >= MAX_COLORS) {
				return form;  // the form's own colour, or none (the preset's)
			}
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSColorForm>();
			const auto made = factory ? factory->Create() : nullptr;
			if (!made) {
				return form;
			}
			made->color = Unpack(a_rgb);
			colors.emplace(a_rgb, made);
			return made;
		}

		// Of the race's presets for that tint mask, the one of this colour (0: none is; the layer's own colour is laid on).
		std::uint16_t PresetFor(const RE::TESRace* a_race, bool a_female, std::uint16_t a_index, std::uint32_t a_color)
		{
			const auto asset = RaceTint(a_race, a_female, a_index);
			if (!asset) {
				return 0;
			}
			const auto& colorsOf = asset->presets.colors;
			for (std::uint32_t i = 0; i < colorsOf.size(); ++i) {
				if (colorsOf[i] && (Pack(colorsOf[i]->color) & 0x00FFFFFF) == (a_color & 0x00FFFFFF)) {
					return static_cast<std::uint16_t>(i);
				}
			}
			return 0;
		}

		// a_face on the copy: what the engine builds a character's head and body from. The preset's own arrays are never
		// written to: the copy gets its own (a duplicate may share the preset's).
		void Apply(RE::TESNPC* a_copy, const RE::TESNPC* a_preset, const Protocol::PlayerFace& a_face, float a_weight)
		{
			const auto race = a_preset->race;

			std::vector<FaceRules::Part> theirs;
			for (const auto id : a_face.headParts) {
				if (const auto part = id != 0 && !FormIds::IsRuntime(id) ? RE::TESForm::LookupByID<RE::BGSHeadPart>(id) : nullptr) {
					theirs.push_back({ id, static_cast<std::uint8_t>(part->type.underlying()) });
				}
			}
			std::vector<FaceRules::Part> its;
			for (std::int8_t i = 0; a_preset->headParts && i < a_preset->numHeadParts; ++i) {
				if (const auto part = a_preset->headParts[i]) {
					its.push_back({ part->GetFormID(), static_cast<std::uint8_t>(part->type.underlying()) });
				}
			}
			const auto plan = FaceRules::PlanParts(theirs, a_face.partTypes, its);
			if (!plan.ids.empty()) {
				const auto parts = RE::calloc<RE::BGSHeadPart*>(plan.ids.size());
				if (parts) {
					for (std::size_t i = 0; i < plan.ids.size(); ++i) {
						parts[i] = RE::TESForm::LookupByID<RE::BGSHeadPart>(plan.ids[i]);
					}
					a_copy->headParts = parts;
					a_copy->numHeadParts = static_cast<std::int8_t>(plan.ids.size());
				}
			}

			if (const auto head = RE::calloc<RE::TESNPC::HeadRelatedData>(1)) {
				if (a_preset->headRelatedData) {
					*head = *a_preset->headRelatedData;
				}
				if (a_face.hairRgb != 0 || a_face.hairColor != 0) {
					if (const auto color = ColorFor(a_face.hairColor, a_face.hairRgb)) {
						head->hairColor = color;
					}
				}
				if (const auto details = a_face.faceDetails != 0 ? RE::TESForm::LookupByID<RE::BGSTextureSet>(a_face.faceDetails) : nullptr) {
					head->faceDetails = details;
				}
				a_copy->headRelatedData = head;
			}

			if (const auto skin = a_face.skin != 0 ? RE::TESForm::LookupByID<RE::TESObjectARMO>(a_face.skin) : nullptr) {
				a_copy->skin = skin;
			}
			a_copy->height = a_face.height;
			a_copy->weight = std::isfinite(a_weight) ? std::clamp(a_weight, 0.0f, 100.0f) : a_copy->weight;

			if (const auto data = RE::calloc<RE::TESNPC::FaceData>(1)) {
				if (a_preset->faceData) {
					*data = *a_preset->faceData;
				}
				for (std::size_t i = 0; i < Protocol::FACE_MORPHS; ++i) {
					data->morphs[i] = a_face.morphs[i];
				}
				for (std::size_t i = 0; i < Protocol::FACE_PRESETS; ++i) {
					data->parts[i] = static_cast<std::int32_t>(a_face.presets[i]);
				}
				a_copy->faceData = data;
			}

			const auto raceTints = RaceTints(race, a_face.female);
			const auto layers = new RE::BSTArray<RE::TESNPC::Layer*>();
			for (const auto& tint : a_face.tints) {
				const auto index = FaceRules::TintIndexFor(tint, raceTints);
				const auto layer = index ? RE::calloc<RE::TESNPC::Layer>(1) : nullptr;
				if (!layer) {
					++tintsLeft;
					continue;
				}
				layer->tintColor = Unpack(tint.color);
				layer->tintIndex = *index;
				layer->preset = PresetFor(race, a_face.female, *index, tint.color);
				layer->interpolationValue = tint.value;
				layers->push_back(layer);
			}
			a_copy->tintLayers = layers;
			a_copy->bodyTintColor = Unpack(a_face.bodyTint);

			// Its own face, never another NPC's: no face template, and its traits its own.
			a_copy->faceNPC = nullptr;
			a_copy->actorData.templateUseFlags.reset(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kTraits);
			if (plan.fellBack != 0) {
				logs::info("Face: copy {:08X}: head parts of kinds {:#04x} came from a mod this game lacks; the preset's kept there", a_copy->GetFormID(), plan.fellBack);
			}
		}
	}

	std::optional<Protocol::PlayerFace> Read()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto base = player ? player->GetActorBase() : nullptr;
		const auto race = player ? player->GetRace() : nullptr;
		if (!base || !race) {
			return std::nullopt;
		}
		auto face = ReadCommon(base, race);
		// The player's tints are its own masks (what character creation and a face editor set), not its base's layers;
		// each named by the race's mask of the same texture. Its skin tone mask is the colour of its skin.
		const auto& masks = player->GetPlayerRuntimeData().tintMasks;
		if (masks.empty()) {
			ReadLayers(base, race, face);
			return face;
		}
		const auto  raceTints = RaceTints(race, face.female);
		std::size_t left = 0;
		for (const auto mask : masks) {
			if (!mask || !mask->texture || face.tints.size() >= Protocol::MAX_FACE_TINTS) {
				continue;
			}
			const auto path = FaceRules::PathHash(mask->texture->textureName.c_str());
			const auto known = std::ranges::find(raceTints, path, &FaceRules::RaceTint::path);
			if (known == raceTints.end()) {
				++left;  // a mask the race does not list (a face editor's own): no other game could find it
				continue;
			}
			const float alpha = std::isfinite(mask->alpha) ? std::clamp(mask->alpha, 0.0f, 1.0f) : 0.0f;
			const auto  type = static_cast<std::uint32_t>(mask->type.underlying());
			face.tints.push_back({ .index = known->index,
				.path = path,
				.color = Pack(mask->color),
				.value = static_cast<std::uint8_t>(std::lround(alpha * 100.0f)),
				.type = static_cast<std::uint8_t>(type < Protocol::TINT_TYPES ? type : 0u) });
			if (type == std::to_underlying(RE::TintMask::Type::kSkinTone)) {
				face.bodyTint = Protocol::PackColor(mask->color.red, mask->color.green, mask->color.blue, 255);
			}
		}
		if (left != ourTintsLeft) {
			ourTintsLeft = left;
			if (left > 0) {
				logs::info("Face: {} of our tint masks are of no texture our race lists: not sent", left);
			}
		}
		return face;
	}

	std::optional<Protocol::PlayerFace> ReadBase(const RE::TESNPC* a_npc)
	{
		const auto race = a_npc ? a_npc->race : nullptr;
		if (!race) {
			return std::nullopt;
		}
		auto face = ReadCommon(a_npc, race);
		ReadLayers(a_npc, race, face);
		return face;
	}

	Copy CopyFor(std::uint32_t a_player, RE::TESNPC* a_preset, const Protocol::PlayerFace& a_face, float a_weight)
	{
		if (!a_preset) {
			return {};
		}
		const auto key = FaceRules::Hash(a_face) ^ (std::bit_cast<std::uint32_t>(a_weight) * 2654435761u);
		if (const auto found = book.Find(a_player, key)) {
			return { .base = reinterpret_cast<RE::TESNPC*>(*found) };
		}
		if (!book.CanMake()) {
			return { .full = true };
		}
		const auto form = a_preset->CreateDuplicateForm(false, nullptr);
		const auto copy = form ? form->As<RE::TESNPC>() : nullptr;
		if (!copy) {
			++refused;
			if (!toldRefused) {
				toldRefused = true;
				logs::warn("Face: the game made no copy of preset {:08X}; friends get the preset's face", a_preset->GetFormID());
			}
			return {};
		}
		book.Made(a_player, key, reinterpret_cast<std::uintptr_t>(copy));
		book.Track(reinterpret_cast<std::uintptr_t>(copy));
		presets[copy] = a_preset;
		Apply(copy, a_preset, a_face, a_weight);
		logs::info("Face: player {}'s face on copy {:08X} of preset {:08X} ({} head parts, {} tints, face {:08X}; {} copies made this run)", a_player, copy->GetFormID(),
			a_preset->GetFormID(), copy->numHeadParts, copy->tintLayers ? copy->tintLayers->size() : 0u, FaceRules::Hash(a_face), book.Made());
		return { .base = copy, .made = true };
	}

	bool Full()
	{
		return !book.CanMake();
	}

	RE::TESNPC* PresetOf(const RE::TESBoundObject* a_base)
	{
		const auto npc = a_base ? a_base->As<RE::TESNPC>() : nullptr;
		const auto it = npc ? presets.find(npc) : presets.end();
		return it != presets.end() ? it->second : nullptr;
	}

	void ForgetCopies()
	{
		if (book.Kept() > 0) {
			logs::info("Face: {} copies of presets not placed again after this load (new ones are made)", book.Kept());
		}
		book.Forget();
		presets.clear();
	}

	bool HasHead(RE::Actor* a_actor)
	{
		return a_actor && a_actor->Get3D() && a_actor->GetFaceNodeSkinned() != nullptr;
	}

	bool OursLoaded()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		return player && player->Get3D(false) != nullptr;
	}

	bool OursHasHead()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		return player && player->Get3D(false) && player->GetFaceNodeSkinned() != nullptr;
	}

	void Rebuild(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		++rebuilds;
		// Built again on the engine's next update of it, head included (a script's QueueNiNodeUpdate).
		a_actor->DoReset3D(false);
	}

	void RepairOurs()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		++repairs;
		logs::warn("Face: our own player's head is still gone after the stand-in that seemed to take it went; asked the engine for it again");
		player->DoReset3D(false);
	}

	void Sent(const Protocol::PlayerFace& a_face)
	{
		++facesSent;
		sentHash = FaceRules::Hash(a_face);
		logs::info("Session: sent our face {:08X} ({} head parts, {} tints)", sentHash, a_face.headParts.size(), a_face.tints.size());
	}

	std::string Describe()
	{
		const auto face = Read();
		std::string out;
		if (!face) {
			out = "read=0";
		} else {
			const auto set = std::ranges::count_if(face->morphs, [](float a_v) { return std::bit_cast<std::uint32_t>(a_v) != Protocol::FACE_UNSET; });
			out = std::format("read=1 hash={:08X} race={:08X} female={} parts={} types={:#04x} hair={:08X}/{:08X} details={:08X} skin={:08X} height={:.3f} morphsSet={} tints={} body={:08X}",
				FaceRules::Hash(*face), face->race, face->female ? 1 : 0, face->headParts.size(), face->partTypes, face->hairColor, face->hairRgb, face->faceDetails, face->skin,
				face->height, set, face->tints.size(), face->bodyTint);
		}
		out += std::format(" ourTintsLeft={} sent={} sentHash={:08X} nudged={} copiesMade={} copiesKept={} full={} refused={} colors={} tintsLeft={} rebuilds={} repairs={}", ourTintsLeft,
			facesSent, sentHash, nudged ? 1 : 0, book.Made(), book.Kept(), book.CanMake() ? 0 : 1, refused, colors.size(), tintsLeft, rebuilds, repairs);
		return out;
	}

	std::string Nudge(std::string_view a_args)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto base = player ? player->GetActorBase() : nullptr;
		const auto race = player ? player->GetRace() : nullptr;
		if (!base || !race || !base->faceData || !base->headRelatedData) {
			return "error: our player has no face to change (no base, race, face data or head data)";
		}
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "undo") {
			if (!nudged) {
				return "error: nothing nudged";
			}
			base->faceData->morphs[0] = nudged->morph;
			base->headRelatedData->hairColor = nudged->hair;
			nudged.reset();
		} else if (!words.empty()) {
			return "error: usage: faces nudge [undo]";
		} else {
			if (nudged) {
				return "error: already nudged (faces nudge undo first)";
			}
			// Another of the race's hair colours than the one worn, and the nose slider moved half its range.
			const auto data = FaceDataOf(race, base->GetSex() == RE::SEX::kFemale);
			RE::BGSColorForm* other = nullptr;
			if (data && data->availableHairColors) {
				for (const auto color : *data->availableHairColors) {
					if (color && color != base->headRelatedData->hairColor && (!base->headRelatedData->hairColor || Pack(color->color) != Pack(base->headRelatedData->hairColor->color))) {
						other = color;
						break;
					}
				}
			}
			if (!other) {
				return "error: our race offers no other hair colour";
			}
			nudged = Nudged{ base->faceData->morphs[0], base->headRelatedData->hairColor };
			const float was = base->faceData->morphs[0];
			base->faceData->morphs[0] = std::isfinite(was) && std::bit_cast<std::uint32_t>(was) != Protocol::FACE_UNSET && was >= 0.0f ? was - 0.5f : 0.5f;
			base->headRelatedData->hairColor = other;
		}
		player->UpdateHairColor();
		player->DoReset3D(false);
		const auto face = Read();
		logs::info("Face: our face {} for the rig (dev faces nudge)", nudged ? "changed" : "put back");
		return std::format("{} hash={:08X}", nudged ? "nudged" : "undone", face ? FaceRules::Hash(*face) : 0u);
	}
}
