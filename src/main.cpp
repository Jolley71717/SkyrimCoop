#include "Config.h"
#include "Log.h"
#include "Platform.h"
#include "Protocol.h"
#include "RuntimeSupport.h"
#include "game/GameThread.h"
#include "game/LoadOrder.h"
#include "game/RemotePlayers.h"
#include "steam/Steam.h"

// The plugin's entry: SKSE loads the DLL and calls SKSEPlugin_Load once, on the game's main thread, before the game's
// data is read.
//
// What is here is the start-up only: the log, the settings, the game version check and the SKSE messages. What the
// mod does in the game (reading the player, showing the other players, the session itself) is behind the interfaces
// in src/game and not written yet; docs/PORTING.md says what each needs.

namespace
{
	// The four bytes that mark our records in SKSE's co-save: "SKCO".
	constexpr std::uint32_t COSAVE_ID = 0x4F434B53;

	bool steamSaid = false;

	// The text an SKSE message carries: a save's name; empty with none.
	std::string_view MessageText(const SKSE::MessagingInterface::Message* a_msg)
	{
		constexpr std::uint32_t LONGEST = 4096;  // more is no name
		if (!a_msg->data || a_msg->dataLen == 0 || a_msg->dataLen > LONGEST) {
			return {};
		}
		return { static_cast<const char*>(a_msg->data), a_msg->dataLen };
	}

	// Whether Steam's lobbies, invites and relay can be used from inside this game: the first thing the project has
	// to find out about Skyrim (docs/GETTING-STARTED.md). Said once in the log; asked again at the next load while
	// the answer is no (Steam may only be late).
	void CheckSteam()
	{
		if (steamSaid || !Config::Get().steam) {
			return;
		}
		if (Steam::Available()) {
			steamSaid = true;
			logs::info("Steam: the game's own Steam library has everything the mod needs");
			return;
		}
		logs::warn("Steam: cannot be used yet: {}", Steam::Unavailable());
	}

	// Once a frame on the main thread, when GameThread can call it.
	void OnFrame()
	{
		if (Config::Get().steam) {
			Steam::Frame();
		}
	}

	void OnDataLoaded()
	{
		const auto plugins = LoadOrder::Read();
		logs::info("Game data loaded ({} plugins read)", plugins.full.size() + plugins.light.size());
		if (Config::Get().steam) {
			CheckSteam();
		} else {
			logs::info("Steam: left alone (bSteam = false)");
		}
		GameThread::Install(OnFrame);
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			logs::debug("SKSE: every plugin is loaded");
			break;
		case SKSE::MessagingInterface::kPostPostLoad:
			logs::debug("SKSE: every plugin has seen the others");
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			logs::debug("SKSE: the game's input is ready");
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			// Which save is loaded: the line a report of "an old save" is read by.
			logs::info("Load: about to load \"{}\"", MessageText(a_msg));
			RemotePlayers::Clear();  // the load takes the world they stand in
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			// data is not a pointer here: it is whether the load worked.
			logs::info("Load: {}", a_msg->data ? "done" : "failed");
			CheckSteam();
			break;
		case SKSE::MessagingInterface::kNewGame:
			logs::info("Load: a new game started");
			CheckSteam();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			logs::info("Save: saving \"{}\"", MessageText(a_msg));
			break;
		case SKSE::MessagingInterface::kDeleteGame:
			logs::debug("Save: \"{}\" was deleted", MessageText(a_msg));
			break;
		default:
			break;
		}
	}

	// The co-save: nothing of ours is kept in a save yet. The callbacks are registered so the first record has a
	// place to go, and so a load into another save is seen (Revert).
	void OnSave(SKSE::SerializationInterface*)
	{
		logs::debug("Co-save: nothing to write");
	}

	void OnLoad(SKSE::SerializationInterface*)
	{
		logs::debug("Co-save: nothing to read");
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		RemotePlayers::Clear();
	}

	// False when the plugin must not load into this game.
	bool CheckRuntime(const REL::Version& a_runtime)
	{
		const auto shown = a_runtime.string(".");
		switch (RuntimeSupport::Judge(a_runtime.major(), a_runtime.minor(), a_runtime.patch())) {
		case RuntimeSupport::Verdict::kRefused:
			logs::error("This is game version {}: SkyrimCoop needs Skyrim Special Edition {}.{}.{} or later (Skyrim VR and the 2011 game are not supported). Not loaded.",
				shown, RuntimeSupport::OLDEST[0], RuntimeSupport::OLDEST[1], RuntimeSupport::OLDEST[2]);
			return false;
		case RuntimeSupport::Verdict::kUntested:
			logs::warn("Game version {}: older than {}.{}.{}, which is where testing starts. It may work; nothing is promised.", shown,
				RuntimeSupport::TESTED_FROM[0], RuntimeSupport::TESTED_FROM[1], RuntimeSupport::TESTED_FROM[2]);
			return true;
		case RuntimeSupport::Verdict::kSupported:
			break;
		}
		logs::info("Game version {}", shown);
		return true;
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	// Before SKSE::Init opens (and empties) the log: the last launches' logs are kept.
	const auto notKept = Log::RotateBeforeInit();

	SKSE::Init(a_skse, SKSE::InitInfo{ .logLevel = spdlog::level::info, .logName = Log::NAME });
	Log::InstallScrubber();
	GameThread::NoteLoadThread();

	if (notKept) {
		logs::warn("Log: could not keep the previous log ({}); it was overwritten", notKept.message());
	}
	// The first line of the log names the build: what a bug report is read by.
	if (const auto plugin = SKSE::PluginDeclaration::GetSingleton()) {
		const auto version = plugin->GetVersion();
		logs::info("SkyrimCoop {}.{}.{} (protocol {})", version.major(), version.minor(), version.patch(), Protocol::VERSION);
	} else {
		logs::info("SkyrimCoop (protocol {})", Protocol::VERSION);
	}
	if constexpr (sizeof(SKYRIMCOOP_BUILD_LABEL) > 1) {
		logs::info("SkyrimCoop build {}", SKYRIMCOOP_BUILD_LABEL);
	}
	if (a_skse->IsEditor()) {
		logs::info("This is the Creation Kit, not the game. Not loaded.");
		return false;
	}
	if (!CheckRuntime(a_skse->RuntimeVersion())) {
		return false;
	}

	Platform::LogStartup();
	Config::Load();

	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		logs::error("SKSE: could not register for its messages. Not loaded.");
		return false;
	}

	if (const auto serialization = SKSE::GetSerializationInterface()) {
		serialization->SetUniqueID(COSAVE_ID);
		serialization->SetSaveCallback(OnSave);
		serialization->SetLoadCallback(OnLoad);
		serialization->SetRevertCallback(OnRevert);
	} else {
		logs::warn("SKSE: no co-save interface; nothing of the mod will be kept in saves");
	}

	return true;
}
