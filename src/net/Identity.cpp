#include "net/Identity.h"

#include <random>

namespace Identity
{
	std::optional<std::filesystem::path> DataFolder()
	{
		return SKSE::log::log_directory();
	}

	namespace
	{
		std::optional<std::filesystem::path> FilePath()
		{
			const auto folder = DataFolder();
			return folder ? std::optional{ *folder / "SkyrimCoop_player.id" } : std::nullopt;
		}

		std::uint64_t Load()
		{
			const auto path = FilePath();
			if (!path) {
				return 0;
			}
			std::uint64_t id = 0;
			if (std::ifstream in{ *path }; in && (in >> std::hex >> id) && id != 0) {
				return id;
			}
			std::random_device rd;
			while (id == 0) {
				id = (static_cast<std::uint64_t>(rd()) << 32) | rd();
			}
			std::ofstream out{ *path, std::ios::trunc };
			out << std::format("{:016x}", id);
			if (!out) {
				logs::warn("Identity: couldn't write {}", path->filename().string());  // not the folder: it holds the Windows user name
			}
			return id;
		}
	}

	std::uint64_t Get()
	{
		static const std::uint64_t id = Load();
		return id;
	}
}
