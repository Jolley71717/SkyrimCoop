#include "DevCommands.h"

#include "Config.h"
#include "DevLine.h"
#include "FirstTest.h"
#include "Protocol.h"
#include "Redact.h"
#include "game/Console.h"
#include "game/Face.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/NpcSync.h"
#include "game/PlayerActions.h"
#include "game/RemotePlayers.h"
#include "game/VirtualPad.h"
#include "net/Session.h"
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

		// status: one line of key=value, never a name or a Steam id (the rig's logs keep it). callbacks= tells a game that
		// never runs Steam's callbacks (stays 0) from Steam not answering (it counts up, the lobby still doesn't come).
		std::string Status(std::string_view)
		{
			const auto steam = Steam::Available();
			return std::format("skyrimcoop protocol={} game={} addrlib={} ticks={} steam={} loggedon={} app={} relay={} hosting={} lobby={} members={} callbacks={}",
				Protocol::VERSION, game, addressLibrary, GameThread::Ticks(), steam ? "ready" : "unavailable", Steam::LoggedOn(), Steam::AppId(),
				Steam::RelayStatus(), Steam::Hosting() ? 1 : 0, Steam::Lobby(), Steam::LobbyMembers(), Steam::CallbacksSeen());
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

		// session [host | leave]: this game's part in the session (net/Session.h), one line, no names or Steam ids.
		std::string SessionCommand(std::string_view a_args)
		{
			const auto words = DevLine::Words(a_args);
			const auto what = words.empty() ? std::string_view{} : words[0];
			if (what == "host") {
				std::string why;
				return Session::Host(why) ? "hosting " + Session::Describe() : "error: " + why;
			}
			if (what == "leave") {
				Session::Leave("asked on the developer channel");
				return "left " + Session::Describe();
			}
			if (what.empty() || what == "status") {
				return Session::Describe();
			}
			return "error: usage: session [host | leave | status]";
		}

		// where: the player's place and movement as they are sent (LocalPlayer::Describe).
		std::string Where(std::string_view)
		{
			return LocalPlayer::Describe();
		}

		// forms [id <hex> | find <plugin> <hex> | place <plugin> | armor <plugin>]: the session's form IDs (net/FormsCommand.h).
		std::string Forms(std::string_view a_args)
		{
			return Session::Forms(a_args);
		}

		std::string StandIns(std::string_view)
		{
			return RemotePlayers::Describe();
		}

		// actions [equip <right|left|ammo|voice> <hex form id>]: what our player's graph did and was sent (PlayerActions),
		// and what each friend's stand-in played of theirs, holds, and did here (RemotePlayers::Actions); equip: the rig's
		// set-up of our player's hands.
		std::string ActionsCommand(std::string_view a_args)
		{
			const auto words = DevLine::Words(a_args);
			if (!words.empty() && words[0] == "equip") {
				return PlayerActions::Equip(a_args.substr(a_args.find("equip") + 5));
			}
			if (!words.empty()) {
				return "error: usage: actions [equip <right|left|ammo|voice> <hex form id>]";
			}
			return PlayerActions::Describe() + " | " + RemotePlayers::Actions();
		}

		// faces [nudge [undo]]: our player's face as it is read and was sent (Face), and each friend's on their stand-in
		// (RemotePlayers::Faces); nudge: the rig changes our face (a slider and the hair colour), undo puts it back.
		std::string FacesCommand(std::string_view a_args)
		{
			const auto words = DevLine::Words(a_args);
			if (!words.empty() && words[0] == "nudge") {
				return Face::Nudge(words.size() > 1 ? words[1] : std::string_view{});
			}
			if (!words.empty()) {
				return "error: usage: faces [nudge [undo]]";
			}
			return Face::Describe() + " | " + RemotePlayers::Faces();
		}

		std::string Npcs(std::string_view a_args)
		{
			return NpcSync::Describe(a_args);
		}

		std::string Strays(std::string_view)
		{
			return RemotePlayers::Strays();
		}

		std::string Graph(std::string_view a_args)
		{
			return RemotePlayers::Graph(a_args);
		}

		std::string Pad(std::string_view a_args)
		{
			return VirtualPad::Command(a_args);
		}

		std::string WalkTo(std::string_view a_args)
		{
			return VirtualPad::WalkTo(a_args);
		}

		// console <command>: runs a console command in this game (the rig places players with coc and saves with save).
		std::string ConsoleCommand(std::string_view a_args)
		{
			if (a_args.empty()) {
				return "error: usage: console <command>";
			}
			logs::info("Console: running a command from the developer channel: {}", Redact::OneLine(a_args));
			return Console::Run(a_args) ? "ran" : "error: the game could not run it";
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
			{ "session", SessionCommand },
			{ "forms", Forms },
			{ "where", Where },
			{ "standins", StandIns },
			{ "actions", ActionsCommand },
			{ "faces", FacesCommand },
			{ "strays", Strays },
			{ "npcs", Npcs },
			{ "graph", Graph },
			{ "pad", Pad },
			{ "walkto", WalkTo },
			{ "console", ConsoleCommand },
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
		return "ping | help | status | steam [host | stophost | join <lobby id> | expectjoin | leave | joinurl | why] | probe [send [count] | report | log | reset] | session [host | leave | status] | forms [id <hex> | find <plugin> <hex> | place <plugin> | armor <plugin>] | where | standins | actions [equip <hand> <id>] | faces [nudge [undo]] | npcs [list | <form id>] | strays | graph <player> [...] | pad ... | walkto ... | console <command>";
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
