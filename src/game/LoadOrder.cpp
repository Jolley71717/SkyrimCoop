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
		// The game's compiled lists: the full plugins by their load index, the light ones by theirs (FE xxx).
		const auto  full = data->GetLoadedMods();
		const auto  fullCount = data->GetLoadedModCount();
		for (std::uint32_t i = 0; full && i < fullCount; ++i) {
			if (full[i]) {
				out.full.emplace_back(full[i]->GetFilename());
			}
		}
		const auto light = data->GetLoadedLightMods();
		const auto lightCount = data->GetLoadedLightModCount();
		for (std::uint32_t i = 0; light && i < lightCount; ++i) {
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
