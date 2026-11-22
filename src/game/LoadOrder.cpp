#include "game/LoadOrder.h"

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
}
