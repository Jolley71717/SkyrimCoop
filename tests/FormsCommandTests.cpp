#include "Test.h"

#include "net/FormsCommand.h"

#include <string>
#include <vector>

// The developer channel's `forms` command without the game (src/net/FormsCommand.h): the rig names a form by its
// plugin's file name and its number within the plugin, and reads each game's form ID and place for it.

using FormsCommand::Kind;
using FormsCommand::Parse;

namespace
{
	std::vector<std::string> Order(const std::vector<std::string>& a_full, const std::vector<std::string>& a_light = {})
	{
		return Protocol::ComparedLoadOrder(a_full, a_light);
	}
}

TEST("forms command: each form of the command is read, plugin names with spaces in them too")
{
	CHECK(Parse("").kind == Kind::kStatus);
	CHECK(Parse(" status ").kind == Kind::kStatus);
	const auto id = Parse("id 0x0300ABCD");
	CHECK(id.kind == Kind::kId);
	CHECK(id.number == 0x0300ABCDu);
	CHECK(Parse("id fe000801").number == 0xFE000801u);

	const auto find = Parse("find Unofficial Skyrim Special Edition Patch.esp 1A2B");
	CHECK(find.kind == Kind::kFind);
	CHECK(find.plugin == "Unofficial Skyrim Special Edition Patch.esp");
	CHECK(find.number == 0x1A2Bu);
	const auto place = Parse("place  ccBGSSSE025-AdvDSGS.esm ");
	CHECK(place.kind == Kind::kPlace);
	CHECK(place.plugin == "ccBGSSSE025-AdvDSGS.esm");
	CHECK(Parse("armor My Armor.esp").plugin == "My Armor.esp");
	CHECK(Parse("armor My Armor.esp").kind == Kind::kArmor);
}

TEST("forms command: what can't be read is a usage error, never a guess")
{
	for (const auto* bad : { "id", "id xyz", "id 123456789", "id 12 34", "find A.esp", "find 800", "find A.esp 80g", "place", "armor", "status now", "nonsense" }) {
		if (Parse(bad).kind != Kind::kBad) {
			Test::Fail(__FILE__, __LINE__, std::format("'{}' was read as a command", bad));
		}
	}
}

TEST("forms command: a form found by plugin and number, a plugin's place, and an ID through the session and back")
{
	const auto ours = FormIds::Table::Of(Order({ "Skyrim.esm", "Update.esm", "A.esp" }, { "L.esl" }));
	CHECK(FormsCommand::DescribeFind(ours, "a.esp", 0x900) == "id=02000900 plugin=a.esp local=900");
	CHECK(FormsCommand::DescribeFind(ours, "L.esl", 0x801) == "id=FE000801 plugin=L.esl local=801");
	CHECK(FormsCommand::DescribeFind(ours, "Missing.esp", 0x801) == "id=none plugin=Missing.esp local=801");
	CHECK(FormsCommand::DescribePlace(ours, "A.esp") == "place=full:2 plugin=A.esp");
	CHECK(FormsCommand::DescribePlace(ours, "l.ESL") == "place=light:0 plugin=l.ESL");
	CHECK(FormsCommand::DescribePlace(ours, "Missing.esp") == "place=none plugin=Missing.esp");

	const FormIds::Mapper mapper(Order({ "Skyrim.esm", "Update.esm", "A.esp" }, { "L.esl" }), Order({ "Skyrim.esm", "A.esp", "Update.esm" }, { "L.esl" }));
	CHECK(FormsCommand::DescribeId(mapper, ours, 0x02000900) == "id=02000900 plugin=A.esp local=900 session=01000900 back=02000900");
	CHECK(FormsCommand::DescribeId(mapper, ours, 0xFF000123) == "id=FF000123 plugin=none local=123 session=FF000123 back=FF000123");
	const FormIds::Mapper lacking(Order({ "Skyrim.esm", "A.esp" }), Order({ "Skyrim.esm" }));
	CHECK(FormsCommand::DescribeId(lacking, FormIds::Table::Of(Order({ "Skyrim.esm", "A.esp" })), 0x01000900) == "id=01000900 plugin=A.esp local=900 session=none back=none");
}

TEST("forms command: a plugin's name in a reply is one line, with no account number in it")
{
	// An individual account's SteamID64, made here so the number itself is written nowhere in the repository.
	const auto        account = std::to_string(0x0110000100000000ull + 52079950);
	const std::string named = "Mine" + account + "\r\nforged.esp";
	const auto        reply = FormsCommand::DescribePlace(FormIds::Table{}, named);
	CHECK(reply.find('\n') == std::string::npos);
	CHECK(reply.find(account) == std::string::npos);
}
