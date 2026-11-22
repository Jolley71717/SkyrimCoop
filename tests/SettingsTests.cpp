#include "Test.h"

#include "RuntimeSupport.h"
#include "Settings.h"

#include <string>
#include <vector>

// The reading of SkyrimCoop.ini (src/Settings.h) and the game version rule (src/RuntimeSupport.h).

TEST("settings: an empty file gives the defaults")
{
	std::vector<std::string> notes;
	const auto               values = Settings::Parse("", &notes);
	CHECK(values == Settings::Values{});
	CHECK(notes.empty());
	CHECK(values.playerName == "character");
	CHECK(!values.host);
	CHECK(values.transport == "auto");
	CHECK(values.port == Protocol::DEFAULT_PORT);
	CHECK(values.maxPlayers == 4);
	CHECK(values.steam);
	CHECK(values.logLevel == "info");
}

TEST("settings: every key is read, with comments, blank lines and either line ending around it")
{
	const std::string text =
		"# SkyrimCoop settings\r\n"
		"\r\n"
		"sPlayerName = Lydia\r\n"
		"; a comment\r\n"
		"bHost = true\n"
		"sTransport=enet\n"
		"  sServerAddress =  host.example:7781  \n"
		"iPort = 7781\n"
		"iMaxPlayers = 8\n"
		"sPassword = sweetroll\n"
		"bSteam = false\n"
		"sLanguage = pt-br\n"
		"sLogLevel = debug\n"
		"bDevChannel = true\n"
		"iDevChannelPort = 7795\n";
	std::vector<std::string> notes;
	const auto               values = Settings::Parse(text, &notes);
	CHECK(notes.empty());
	CHECK(values.playerName == "Lydia");
	CHECK(values.host);
	CHECK(values.transport == "enet");
	CHECK(values.serverAddress == "host.example:7781");
	CHECK(values.port == 7781);
	CHECK(values.maxPlayers == 8);
	CHECK(values.password == "sweetroll");
	CHECK(!values.steam);
	CHECK(values.language == "pt-br");
	CHECK(values.logLevel == "debug");
	CHECK(values.devChannel);
	CHECK(values.devChannelPort == 7795);
}

TEST("settings: a value that can't be used keeps the default and is said, without the password")
{
	const std::string text =
		"sTransport = carrier pigeon\n"
		"iPort = 70000\n"
		"iMaxPlayers = 1\n"
		"sPassword = " + std::string(Protocol::MAX_PASSWORD_LENGTH + 1, 'x') + "\n"
		"sLogLevel = loud\n"
		"bHost = maybe\n";
	std::vector<std::string> notes;
	const auto               values = Settings::Parse(text, &notes);
	CHECK(values == Settings::Values{});
	REQUIRE(notes.size() == 5);
	CHECK(notes[0] == "sTransport = carrier pigeon is not auto, steam or enet; it is auto");
	CHECK(notes[1] == "iPort = 70000 is not a port (1 to 65535); it is 7780");
	CHECK(notes[2] == "iMaxPlayers = 1 is not 2 to 8; it is 4");
	CHECK(notes[3] == "sPassword is longer than 64 characters; no password is used");
	CHECK(notes[4] == "sLogLevel = loud is not info or debug; it is info");
	for (const auto& note : notes) {
		CHECK(note.find("xxxx") == std::string::npos);
	}
}

TEST("settings: numbers with anything after them are not numbers")
{
	CHECK(Settings::Parse("iPort = 7781x\n").port == Protocol::DEFAULT_PORT);
	CHECK(Settings::Parse("iPort = -1\n").port == Protocol::DEFAULT_PORT);
	CHECK(Settings::Parse("iPort = 0\n").port == Protocol::DEFAULT_PORT);
	CHECK(Settings::Parse("iPort = 65535\n").port == 65535);
	CHECK(Settings::Parse("iMaxPlayers = 9\n").maxPlayers == 4);
	CHECK(Settings::Parse("iMaxPlayers = 2\n").maxPlayers == 2);
}

TEST("settings: a long player name is cut at the protocol's limit, and an empty one stays empty")
{
	const auto values = Settings::Parse("sPlayerName = " + std::string(300, 'a') + "\n");
	CHECK(values.playerName.size() == Protocol::MAX_NAME_LENGTH);
	// Empty means "my Steam name" (NameChoice.h).
	CHECK(Settings::Parse("sPlayerName =\n").playerName.empty());
}


TEST("runtime support: 1.7.104 is the tested build, the rest of the Anniversary line warns, 1.5.97 and older are refused")
{
	using RuntimeSupport::Judge;
	using RuntimeSupport::Verdict;
	// Skyrim VR reports 1.4.15.
	CHECK(Judge(1, 4, 15) == Verdict::kRefused);
	CHECK(Judge(1, 5, 80) == Verdict::kRefused);
	CHECK(Judge(1, 5, 97) == Verdict::kRefused);
	CHECK(Judge(1, 6, 316) == Verdict::kRefused);
	CHECK(Judge(0, 9, 9999) == Verdict::kRefused);
	CHECK(Judge(1, 6, 317) == Verdict::kOlder);
	CHECK(Judge(1, 6, 640) == Verdict::kOlder);
	CHECK(Judge(1, 6, 1170) == Verdict::kOlder);
	CHECK(Judge(1, 7, 99) == Verdict::kOlder);
	CHECK(Judge(1, 7, 104) == Verdict::kSupported);
	CHECK(Judge(1, 7, 105) == Verdict::kNewer);
	CHECK(Judge(1, 8, 0) == Verdict::kNewer);
	CHECK(Judge(2, 0, 0) == Verdict::kNewer);
}

TEST("settings: the developer channel is off by default, on its own port, and refuses ports below 1024")
{
	const auto defaults = Settings::Parse("");
	CHECK(!defaults.devChannel);
	CHECK(defaults.devChannelPort == 7791);
	CHECK(defaults.devChannelPort != 7790);  // F4Multiplayer's: the rig's tools for the two games never cross

	std::vector<std::string> notes;
	const auto               values = Settings::Parse("bDevChannel = 1\niDevChannelPort = 80\n", &notes);
	CHECK(values.devChannel);
	CHECK(values.devChannelPort == 7791);
	REQUIRE(notes.size() == 1);
	CHECK(notes[0] == "iDevChannelPort = 80 is not a port (1024 to 65535); it is 7791");
}
