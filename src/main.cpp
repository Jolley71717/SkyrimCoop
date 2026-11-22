#include "AddressLibraryFile.h"
#include "Config.h"
#include "DevChannel.h"
#include "DevCommands.h"
#include "Log.h"
#include "Platform.h"
#include "Protocol.h"
#include "RuntimeSupport.h"
#include "Text.h"
#include "game/GameThread.h"
#include "game/LoadOrder.h"
#include "game/RemotePlayers.h"
#include "steam/Steam.h"
#include "steam/SteamProbe.h"

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

	// About once a frame on the main thread (GameThread). Steam first, so a friend's first message is let in
	// (SessionRequested) before the probe reads its channel.
	void OnFrame()
	{
		if (Config::Get().steam) {
			Steam::Frame();
			SteamProbe::Frame();
			DevCommands::Frame();
		}
		DevChannel::Frame();
	}

	// Whether the Address Library has the file for this game version, and an address is found through it: the first
	// thing the first test reads. CommonLib's own answer to a missing file is a message box and the end of the game,
	// so the file is looked for first and the plugin stays out without it. The id asked for is the game's main object
	// (RE::Main), which CommonLib itself looks up the same way; nothing at that address is read.
	bool CheckAddressLibrary(const REL::Version& a_runtime)
	{
		const auto relative = AddressLibraryFile::Path(a_runtime.major(), a_runtime.minor(), a_runtime.patch(), a_runtime.build());
		std::error_code error;
		const auto      size = std::filesystem::file_size(Platform::GamePath(relative), error);
		if (error) {
			logs::error("Address Library: {} is not there. Install \"Address Library for SKSE Plugins\" for game {} (docs/GETTING-STARTED.md). Not loaded.",
				relative, a_runtime.string("."));
			DevCommands::NoteStart(a_runtime.string("."), "missing");
			return false;
		}
		const auto offset = REL::RelocationID(516943, 403449).offset();
		logs::info("Address Library: {} ({} bytes) resolved an address (the game's main object at offset {:#x})", AddressLibraryFile::Name(a_runtime.major(),
			a_runtime.minor(), a_runtime.patch(), a_runtime.build()), size, offset);
		DevCommands::NoteStart(a_runtime.string("."), "ok");
		return true;
	}

	void OnDataLoaded()
	{
		Text::Load();  // the game's own sLanguage is known by now
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
		case RuntimeSupport::Verdict::kOlder:
			logs::warn("Game version {}: older than {}.{}.{}, the version the mod is tested on. It may work; nothing is promised.", shown,
				RuntimeSupport::TESTED[0], RuntimeSupport::TESTED[1], RuntimeSupport::TESTED[2]);
			return true;
		case RuntimeSupport::Verdict::kNewer:
			logs::warn("Game version {}: newer than {}.{}.{}, the version the mod is tested on. It may work; nothing is promised.", shown,
				RuntimeSupport::TESTED[0], RuntimeSupport::TESTED[1], RuntimeSupport::TESTED[2]);
			return true;
		case RuntimeSupport::Verdict::kSupported:
			break;
		}
		logs::info("Game version {} (the tested one)", shown);
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
	if (!CheckAddressLibrary(a_skse->RuntimeVersion())) {
		return false;
	}

	Platform::LogStartup();
	Config::Load();
	DevChannel::Start();

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
