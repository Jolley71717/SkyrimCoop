#include "Test.h"

#include "net/LaunchArgs.h"

// The lobby a friend's invite started the game for, read from its command line (src/net/LaunchArgs.h).

namespace
{
	constexpr std::uint64_t LOBBY = 109775241234567890ull;  // a chat account in the public universe: a lobby
	constexpr std::uint64_t USER = 76561197960287930ull;    // an individual account: a player, not a lobby
}

TEST("launch: Steam's own +connect_lobby and our +skyrimcoop_lobby both name the lobby")
{
	CHECK(LaunchArgs::LooksLikeLobby(LOBBY));
	CHECK(!LaunchArgs::LooksLikeLobby(USER));
	CHECK(LaunchArgs::Lobby("\"F:\\Games\\SkyrimSE.exe\" +connect_lobby 109775241234567890") == LOBBY);
	CHECK(LaunchArgs::Lobby("skse64_loader.exe +skyrimcoop_lobby 109775241234567890 -something") == LOBBY);
	CHECK(LaunchArgs::Lobby("game.exe   +connect_lobby\t \"109775241234567890\"") == LOBBY);
}

TEST("launch: no switch, no number, or a number that is no lobby: nothing to join")
{
	CHECK(!LaunchArgs::Lobby("").has_value());
	CHECK(!LaunchArgs::Lobby("SkyrimSE.exe").has_value());
	CHECK(!LaunchArgs::Lobby("game.exe +connect_lobby").has_value());
	CHECK(!LaunchArgs::Lobby("game.exe +connect_lobby abc").has_value());
	CHECK(!LaunchArgs::Lobby("game.exe +connect_lobby 76561197960287930").has_value());
	// Another word that starts the same is not the switch.
	CHECK(!LaunchArgs::Lobby("game.exe +connect_lobbyX 109775241234567890").has_value());
	// But a later real one still counts.
	CHECK(LaunchArgs::Lobby("game.exe +connect_lobbyX 1 +connect_lobby 109775241234567890") == LOBBY);
}
