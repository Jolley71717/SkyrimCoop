#include "game/RemotePlayers.h"

namespace RemotePlayers
{
	namespace
	{
		struct Remote
		{
			std::string                         name;
			std::optional<Protocol::PlayerState> state;
		};

		std::unordered_map<std::uint32_t, Remote> players;
	}

	void Joined(std::uint32_t a_playerId, std::string_view a_name)
	{
		players[a_playerId] = Remote{ std::string{ a_name }, std::nullopt };
		logs::info("Players: player {} joined ({} here now); stand-ins are not written yet, so nothing is shown", a_playerId, players.size());
	}

	void Left(std::uint32_t a_playerId)
	{
		if (players.erase(a_playerId) > 0) {
			logs::info("Players: player {} left ({} here now)", a_playerId, players.size());
		}
	}

	void Moved(std::uint32_t a_playerId, const Protocol::PlayerState& a_state)
	{
		if (const auto it = players.find(a_playerId); it != players.end()) {
			it->second.state = a_state;
		}
	}

	void Clear()
	{
		players.clear();
	}

	std::size_t Count()
	{
		return players.size();
	}
}
