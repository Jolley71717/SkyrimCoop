#include "DevCommands.h"

#include "Config.h"
#include "DevLine.h"
#include "FirstTest.h"
#include "Protocol.h"
#include "game/GameThread.h"
#include "steam/Steam.h"
#include "steam/SteamProbe.h"

namespace DevCommands
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::string game = "?";
		std::string addressLibrary = "?";
		FirstTest   firstTest;

		// status: one line of key=value, never a name or a Steam id (the rig's logs keep it).
		std::string Status(std::string_view)
		{
			const auto steam = Steam::Available();
			return std::format("skyrimcoop protocol={} game={} addrlib={} ticks={} steam={} loggedon={} app={} relay={} hosting={} lobby={} members={}",
				Protocol::VERSION, game, addressLibrary, GameThread::Ticks(), steam ? "ready" : "unavailable", Steam::LoggedOn(), Steam::AppId(),
				Steam::RelayStatus(), Steam::Hosting() ? 1 : 0, Steam::Lobby(), Steam::LobbyMembers());
		}

		// steam [host | stophost | join <lobby id> | expectjoin | leave | joinurl | why]
		std::string SteamCommand(std::string_view a_args)
		{
			const auto words = DevLine::Words(a_args);
			const auto what = words.empty() ? std::string_view{} : words[0];
			if (what == "why") {
				const auto why = Steam::Unavailable();
				return why.empty() ? "available" : why;
			}
			if (!Steam::Available()) {
				return "error: Steam is not available: " + Steam::Unavailable();
			}
			if (what == "host") {
				firstTest.AskedToHost(Clock::now());
				Steam::StartHosting(Config::Get().maxPlayers);
				logs::info("First test: asked Steam for a friends-only lobby");
				return "hosting";
			}
			if (what == "stophost") {
				Steam::StopHosting();
				return "stopped hosting";
			}
			if (what == "join" && words.size() == 2) {
				const auto id = DevLine::Number(words[1]);
				if (!id) {
					return "error: usage: steam join <lobby id>";
				}
				firstTest.AskedToJoin(Clock::now());
				logs::info("First test: asked to join lobby {}", *id);
				Steam::Join(*id);
				return "joining";
			}
			// A join that Steam itself starts (steam://joinlobby, an invite) is timed from here.
			if (what == "expectjoin") {
				firstTest.AskedToJoin(Clock::now());
				logs::info("First test: a join through Steam is expected");
				return "expecting";
			}
			if (what == "leave") {
				Steam::Leave();
				return "left";
			}
			if (what == "joinurl") {
				// Names our Steam id: an answer for the rig's script only, never logged.
				const auto url = Steam::JoinUrl();
				return url.empty() ? "error: no lobby of ours" : url;
			}
			return "error: usage: steam [host | stophost | join <lobby id> | expectjoin | leave | joinurl | why]";
		}

		// probe [send [count] | report | log | reset]
		std::string Probe(std::string_view a_args)
		{
			const auto words = DevLine::Words(a_args);
			if (!words.empty() && words[0] == "send") {
				const auto count = words.size() > 1 ? DevLine::Number(words[1]) : std::optional<std::uint64_t>{ 5 };
				if (!count || *count == 0 || *count > 1000) {
					return "error: usage: probe send [1-1000]";
				}
				return SteamProbe::Send(static_cast<std::uint32_t>(*count));
			}
			if (!words.empty() && words[0] == "reset") {
				SteamProbe::Reset();
				return "reset";
			}
			if (words.empty() || words[0] == "report") {
				return SteamProbe::Report(false);
			}
			if (words[0] == "log") {
				return SteamProbe::Report(true);  // the same, written in the log as well
			}
			return "error: usage: probe [send [count] | report | log | reset]";
		}

		struct Entry
		{
			std::string_view name;
			Handler          handler;
		};
		constexpr Entry COMMANDS[] = {
			{ "status", Status },
			{ "steam", SteamCommand },
			{ "probe", Probe },
		};
	}

	Handler Find(std::string_view a_name)
	{
		for (const auto& entry : COMMANDS) {
			if (entry.name == a_name) {
				return entry.handler;
			}
		}
		return nullptr;
	}

	std::string Help()
	{
		return "ping | help | status | steam [host | stophost | join <lobby id> | expectjoin | leave | joinurl | why] | probe [send [count] | report | log | reset]";
	}

	void NoteStart(std::string a_game, std::string a_addressLibrary)
	{
		game = std::move(a_game);
		addressLibrary = std::move(a_addressLibrary);
	}

	void Frame()
	{
		if (!Steam::Available()) {
			return;
		}
		for (const auto& line : firstTest.Look({ .hosting = Steam::Hosting(), .lobby = Steam::Lobby(), .members = Steam::LobbyMembers() }, Clock::now())) {
			logs::info("{}", line);
		}
	}
}
