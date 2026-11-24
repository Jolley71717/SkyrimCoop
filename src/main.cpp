#include "AddressLibraryFile.h"
#include "Background.h"
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
#include "game/LocalPlayer.h"
#include "game/RemotePlayers.h"
#include "game/VirtualPad.h"
#include "game/WorldClock.h"
#include "game/Conversations.h"
#include "game/DropSync.h"
#include "game/Pickups.h"
#include "game/Traps.h"
#include "game/LootSync.h"
#include "game/QuestSync.h"
#include "game/WorldObjects.h"
#include "net/Session.h"
#include "steam/Steam.h"
#include "steam/SteamProbe.h"

// The plugin's entry: SKSE loads the DLL and calls SKSEPlugin_Load once, on the game's main thread, before the game's
// data is read.
//
// What is here is the start-up (the log, the settings, the game version check), the SKSE messages and the co-save, and
// the tick that runs the rest: the session (net/Session.h), the stand-ins of the other players (game/RemotePlayers.h)
// and the developer channel.

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

	// bRunInBackground (Background.h): the game's bAlwaysActive on while in a session, and whenever the developer channel
	// is on (the rig: a script must not stall because someone clicked elsewhere), the player's value back after.
	Background::Switch background;

	RE::Setting* AlwaysActive()
	{
		static RE::Setting* const found = [] {
			const auto prefs = RE::INIPrefSettingCollection::GetSingleton();
			const auto inPrefs = prefs ? prefs->GetSetting("bAlwaysActive:General") : nullptr;
			const auto ini = RE::INISettingCollection::GetSingleton();
			const auto setting = inPrefs ? inPrefs : ini ? ini->GetSetting("bAlwaysActive:General") : nullptr;
			if (!setting || setting->GetType() != RE::Setting::Type::kBool) {
				logs::warn("Config: the game's bAlwaysActive setting was not found; the game pauses while it is not focused");
				return static_cast<RE::Setting*>(nullptr);
			}
			logs::info("Config: the game's bAlwaysActive is in its {} settings, {} now", inPrefs ? "Prefs ini" : "ini", setting->GetBool());
			return setting;
		}();
		return found;
	}

	void RunInBackground()
	{
		const auto setting = AlwaysActive();
		if (!setting) {
			return;
		}
		const auto& config = Config::Get();
		const bool  forced = config.devChannel || Session::Active();
		if (const auto value = background.Update(config.runInBackground || config.devChannel, forced, setting->GetBool())) {
			setting->SetBool(*value);
			logs::info("Config: {}", *value ? (config.devChannel ? "the game runs on while it is not focused (the developer channel is on)" :
			                                                       "in a session, the game runs on while it is not focused (bRunInBackground)") :
			                                  "out of the session, the game's bAlwaysActive is back as it was");
		}
	}

	// The rig only (developer channel on): the questions a new game asks once, before the player can move, are taken
	// as answered "no", so a game the rig starts with coc from the main menu never waits on a box nobody will click.
	// Each is a global its quest sets once it has asked; set here before the quest looks, every frame until it holds (a
	// new game may set it back). A box that is up anyway shows in `where` (menus=...), and the rig's start stops on it.
	//   Survival_PlayerHasBeenPrompted  "Enable Survival Mode? Yes / No" (the bundled Survival Mode, ccQDRSSE001)
	void NoFirstRunPrompts()
	{
		if (!Config::Get().devChannel) {
			return;
		}
		static constexpr std::array ASKED_ONCE{ "Survival_PlayerHasBeenPrompted" };
		static std::array<RE::TESGlobal*, ASKED_ONCE.size()> globals = [] {
			std::array<RE::TESGlobal*, ASKED_ONCE.size()> found{};
			for (std::size_t i = 0; i < ASKED_ONCE.size(); ++i) {
				found[i] = RE::TESForm::LookupByEditorID<RE::TESGlobal>(ASKED_ONCE[i]);
				logs::info("Rig: {} {}", ASKED_ONCE[i], found[i] ? "found: the game will not ask it" : "not in this load order");
			}
			return found;
		}();
		for (const auto global : globals) {
			if (global && global->value != 1.0f) {
				global->value = 1.0f;
			}
		}
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
		NoFirstRunPrompts();
		Session::Frame();
		RemotePlayers::TendGraveyard();  // after the session's frame: a stand-in it took away is disabled in this tick
		RunInBackground();
		VirtualPad::Frame();
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
		if (!QuestSync::Install()) {
			logs::warn("Quests: quests are not shared in this game; a session still runs");
		}
		if (!WorldObjects::Install()) {
			logs::warn("World: doors, locks and mechanisms are not shared in this game; a session still runs");
		}
		if (!LootSync::Install()) {
			logs::warn("Loot: containers and loot are not shared in this game; a session still runs");
		}
		if (!DropSync::Install()) {
			logs::warn("Drops: dropped items are not given in this game; a session still runs");
		}
		if (!Pickups::Install()) {
			logs::warn("Pickups: what players take from the world is not shared in this game; a session still runs");
		}
		if (!Traps::Install()) {
			logs::warn("Traps: world traps are not shared in this game; a session still runs");
		}
		if (!Conversations::Install()) {
			logs::warn("Talk: conversations are not shared in this game; a session still runs");
		}
		if (!RemotePlayers::Install()) {
			logs::warn("Players: no stand-ins in this game (the Character update is not hooked); a session still runs");
		}
		LocalPlayer::NoteDefaultName();
		NoFirstRunPrompts();
		RunInBackground();  // before the first tick: the main menu must not stall either
		// Whether the game's sound is on: the rig's VM, which has no audio device, runs with it off (tools/rig/start.ps1).
		const auto ini = RE::INISettingCollection::GetSingleton();
		const auto audio = ini ? ini->GetSetting("bEnableAudio:Audio") : nullptr;
		logs::info("Config: the game's sound is {} ([Audio] bEnableAudio)", !audio ? "unknown" : audio->GetBool() ? "on" : "off");
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
			RemotePlayers::ForgetStandIns();  // the load takes the world they stand in; their players stay
			QuestSync::BeforeLoad();          // no quest alias record is left rewritten for a run into the load
			Conversations::BeforeLoad();      // friends' lines' sounds and subtitles go with the world
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			// data is not a pointer here: it is whether the load worked.
			logs::info("Load: {}", a_msg->data ? "done" : "failed");
			RemotePlayers::AfterLoad();  // any stand-in the save brought back goes
			WorldObjects::AfterLoad();   // the loaded world's doors are the save's: the session's words are asked for again
			QuestSync::AfterLoad();      // ... and its quests: the host says them again, a friend asks for the session's
			WorldClock::AfterLoad();     // ... and its clock: the save's day count and timescale, lined up with the owner's again
			Conversations::AfterLoad();  // ... and our conversation, read afresh
			LootSync::AfterLoad();       // ... and its loot: the save's ledger, the session's moves it lacks asked for
			DropSync::AfterLoad();       // ... and its dropped items: the save's record, told to the session
			Pickups::AfterLoad();        // ... and what players took from the world: the session's asked for again
			Traps::AfterLoad();          // ... and its traps: the session's words asked for again
			CheckSteam();
			break;
		case SKSE::MessagingInterface::kNewGame:
			logs::info("Load: a new game started");
			RemotePlayers::ForgetStandIns();
			Conversations::BeforeLoad();
			WorldObjects::AfterLoad();
			QuestSync::AfterLoad();
			WorldClock::AfterLoad();
			Conversations::AfterLoad();
			LootSync::AfterLoad();
			DropSync::NewGame();
			Pickups::NewGame();
			Traps::NewGame();
			CheckSteam();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			// Sent before the game writes the save: no stand-in is in the world while it does.
			logs::info("Save: saving \"{}\"", MessageText(a_msg));
			WorldObjects::BeforeSave();  // what was lent to a stand-in for a friend's use is taken back first
			RemotePlayers::BeforeSave();
			WorldClock::BeforeSave();  // ... and our own timescale goes into it, not the session's
			DropSync::BeforeSave();    // ... and no copy of a dropped item is in it
			break;
		case SKSE::MessagingInterface::kDeleteGame:
			logs::debug("Save: \"{}\" was deleted", MessageText(a_msg));
			break;
		default:
			break;
		}
	}

	// The co-save: the list of stand-ins taken out of the world for the save (RemotePlayers), so a load can remove any
	// that the save brought back, and the loot ledger (LootSync): which of the session's moves of loot this world holds,
	// and the ones still waiting for their holder; and the dropped items' record (DropSync): which of the session's drops
	// left this world and which it took in, and the copies taken out for the save; and the radiant jobs that ran for the
	// session's runs (QuestSync), so a load stops one no run claims. Nothing else of a session is kept.
	void OnSave(SKSE::SerializationInterface* a_intf)
	{
		RemotePlayers::WriteCoSave(a_intf);
		LootSync::WriteCoSave(a_intf);
		DropSync::WriteCoSave(a_intf);
		QuestSync::WriteCoSave(a_intf);
	}

	void OnLoad(SKSE::SerializationInterface* a_intf)
	{
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intf->GetNextRecordInfo(type, version, length)) {
			if (type == RemotePlayers::COSAVE_STANDINS) {
				RemotePlayers::ReadCoSave(a_intf, version, length);
			} else if (type == LootSync::COSAVE_LOOT) {
				LootSync::ReadCoSave(a_intf, version, length);
			} else if (type == DropSync::COSAVE_DROPS) {
				DropSync::ReadCoSave(a_intf, version, length);
			} else if (type == QuestSync::COSAVE_RUNS) {
				QuestSync::ReadCoSave(a_intf, version, length);
			} else {
				logs::warn("Co-save: a record of a type this version does not know ({:08X}); skipped", type);
			}
		}
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		RemotePlayers::ForgetStandIns();
		LootSync::RevertCoSave();
		DropSync::RevertCoSave();
		QuestSync::RevertCoSave();
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

	// Last, once nothing can fail the load: a plugin that is not loaded leaves no listener thread and no patched import.
	DevChannel::Start();
	VirtualPad::Install();  // developer channel only; before the game makes its gamepad

	return true;
}
