#include "game/LoadOrder.h"

#include "Protocol.h"

namespace LoadOrder
{
	Plugins Read()
	{
		Plugins    out;
		const auto data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return out;
		}
		// The game's compiled lists: the full plugins by their load index, the light ones by theirs (FE xxx). A name's
		// place in the list is the index its forms carry (FormIds::Table), so a gap (a null entry, or a file whose own
		// compile index is not its place) would shift every later plugin and give friends other forms than ours. The
		// game is not known to leave one; if it ever does, it is said loudly, once, rather than mapped wrong in silence.
		const auto gap = [](const char* a_kind, std::uint32_t a_place, const RE::TESFile* a_file, std::uint32_t a_index) {
			logs::error("LoadOrder: the {} plugin at place {} {} (its compile index {}): forms of later {} plugins may be taken for other plugins' forms in a session whose load orders differ",
				a_kind, a_place, a_file ? "is out of place" : "is missing", a_index, a_kind);
		};
		const auto full = data->GetLoadedMods();
		const auto fullCount = data->GetLoadedModCount();
		bool       saidFull = false;
		for (std::uint32_t i = 0; full && i < fullCount; ++i) {
			if (!saidFull && (!full[i] || full[i]->compileIndex != i)) {
				gap("full", i, full[i], full[i] ? full[i]->compileIndex : 0);
				saidFull = true;
			}
			if (full[i]) {
				out.full.emplace_back(full[i]->GetFilename());
			}
		}
		const auto light = data->GetLoadedLightMods();
		const auto lightCount = data->GetLoadedLightModCount();
		bool       saidLight = false;
		for (std::uint32_t i = 0; light && i < lightCount; ++i) {
			if (!saidLight && (!light[i] || light[i]->smallFileCompileIndex != i)) {
				gap("light", i, light[i], light[i] ? light[i]->smallFileCompileIndex : 0);
				saidLight = true;
			}
			if (light[i]) {
				out.light.emplace_back(light[i]->GetFilename());
			}
		}
		return out;
	}

	std::optional<std::uint32_t> FirstPlayableArmor(std::string_view a_plugin)
	{
		const auto data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return std::nullopt;
		}
		for (const auto armor : data->GetFormArray<RE::TESObjectARMO>()) {
			// Defined there, not only changed there: the plugin that made it is the first file of its form.
			const auto file = armor ? armor->GetFile(0) : nullptr;
			if (file && armor->GetPlayable() && Protocol::SamePluginName(file->GetFilename(), a_plugin)) {
				return armor->GetFormID();
			}
		}
		return std::nullopt;
	}
}
