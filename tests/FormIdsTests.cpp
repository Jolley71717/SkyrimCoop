#include "Test.h"

#include "FormIds.h"

#include <string>
#include <vector>

using FormIds::Compose;
using FormIds::Extend;
using FormIds::Mapper;
using FormIds::Place;
using FormIds::PlaceOf;
using FormIds::Table;
using Names = std::vector<std::string>;

namespace
{
	// A load order as a hello carries it: the full plugins, the light mark, the light ones.
	Names Order(const Names& a_full, const Names& a_light = {})
	{
		return Protocol::ComparedLoadOrder(a_full, a_light);
	}

	const Names BASE{ "Skyrim.esm", "Dawnguard.esm", "Dragonborn.esm" };

	constexpr Place Full(std::uint16_t a_index) { return { false, a_index }; }
	constexpr Place Light(std::uint16_t a_index) { return { true, a_index }; }
}

TEST("form ids: a form ID's place is its plugin's among its kind; 0 and runtime forms have none")
{
	CHECK(PlaceOf(0x00012345) == Full(0));
	CHECK(PlaceOf(0x09000804) == Full(9));
	CHECK(PlaceOf(0xFE012804) == Light(0x012));
	CHECK(PlaceOf(0xFEFFF001) == Light(0xFFF));
	CHECK(!PlaceOf(0));
	CHECK(!PlaceOf(0xFF000123));
	CHECK(FormIds::LocalPart(0x09000804) == 0x000804);
	CHECK(FormIds::LocalPart(0xFE012804) == 0x804);
}

TEST("form ids: composing an ID gives back what was taken apart, and none where no ID can say it")
{
	for (const std::uint32_t id : { 0x00000014u, 0x0A0B0C0Du, 0xFD123456u, 0xFE000801u, 0xFE123FFFu }) {
		CHECK(Compose(*PlaceOf(id), FormIds::LocalPart(id)) == id);
	}
	CHECK(!Compose(Place{ true, 3 }, 0x1000));    // a full plugin's form past the 12 bits a light one numbers
	CHECK(!Compose(Place{ true, 0x1000 }, 1));    // past the light places
	CHECK(!Compose(Place{ false, 0xFE }, 1));     // FE and FF are not full places
	CHECK(Compose(Place{ false, 0xFD }, 0x00FFFFFF) == 0xFDFFFFFFu);
}

TEST("form ids: a table splits a hello's list into its kinds, names in any case, each once")
{
	const auto table = Table::Of(Order({ "Skyrim.esm", "A.esp", "a.ESP" }, { "L1.esl", "L2.esl" }));
	CHECK(table.full == Names({ "Skyrim.esm", "A.esp" }));
	CHECK(table.light == Names({ "L1.esl", "L2.esl" }));
	CHECK(table.Find("a.esp") == Full(1));
	CHECK(table.Find("l2.ESL") == Light(1));
	CHECK(!table.Find("B.esp"));
	REQUIRE(table.At(Place{ true, 0 }));
	CHECK(*table.At(Place{ true, 0 }) == "L1.esl");
	CHECK(!table.At(Place{ false, 2 }));
	CHECK(Table::Of(table.Order()).Same(table));
	// A list without the mark (an older layout) is all full.
	CHECK(Table::Of({ "Skyrim.esm", "X.esl" }).full.size() == 2);
}

TEST("form ids: the same plugins in the same places translate nothing")
{
	const Mapper same(Order(BASE, { "L.esl" }), Order({ "skyrim.ESM", "Dawnguard.esm", "Dragonborn.esm" }, { "l.esl" }));
	CHECK(same.Identity());
	CHECK(Mapper().Identity());
	// Without either list (a peer that sent none, the session's not come yet) as before protocol 4.
	CHECK(Mapper({}, Order(BASE)).Identity());
	CHECK(Mapper(Order(BASE), {}).Identity());
	CHECK(Mapper(Order(BASE), {}).ToSession(0x05000801) == 0x05000801u);
}

TEST("form ids: a reordered load order maps each plugin's forms to its place in the other")
{
	// Ours: A before B; the session's: B before A.
	const Mapper m(Order(Names{ "Skyrim.esm", "A.esp", "B.esp" }), Order(Names{ "Skyrim.esm", "B.esp", "A.esp" }));
	REQUIRE(!m.Identity());
	CHECK(m.ToSession(0x01000801) == 0x02000801u);
	CHECK(m.ToSession(0x02000802) == 0x01000802u);
	CHECK(m.ToLocal(0x02000801) == 0x01000801u);
	CHECK(m.ToSession(0x0001F00D) == 0x0001F00Du);  // Skyrim.esm in both
	CHECK(m.ToLocal(*m.ToSession(0x01ABCDEF)) == 0x01ABCDEFu);
	CHECK(m.LocalPlugin(0x01000801) == "A.esp");
	CHECK(m.SessionPlugin(0x01000801) == "B.esp");
	CHECK(m.OursOnly() == 0);
	CHECK(m.SessionsOnly() == 0);
}

TEST("form ids: light plugins map among the light ones, and a plugin light in one game and full in the other")
{
	const Mapper m(Order(BASE, { "L1.esl", "L2.esl", "Both.esp" }), Order(Names{ "Skyrim.esm", "Dawnguard.esm", "Dragonborn.esm", "Both.esp" }, { "L2.esl", "L1.esl" }));
	CHECK(m.ToSession(0xFE000801) == 0xFE001801u);  // L1: light 0 here, light 1 there
	CHECK(m.ToSession(0xFE001ABC) == 0xFE000ABCu);
	CHECK(m.ToSession(0xFE002801) == 0x03000801u);  // Both.esp: light here, full there
	CHECK(m.ToLocal(0x03000801) == 0xFE002801u);
	// A form of it past what a light plugin numbers has no ID here.
	CHECK(!m.ToLocal(0x03001000));
}

TEST("form ids: a plugin the other side lacks has no IDs there; nor has a runtime form, in any mapper; 0 stays 0")
{
	const Mapper m(Order(Names{ "Skyrim.esm", "Mine.esp", "Shared.esp" }, { "MyLight.esl" }), Order(Names{ "Skyrim.esm", "Shared.esp", "Theirs.esp" }));
	CHECK(!m.ToSession(0x01000801));  // Mine.esp
	CHECK(!m.ToSession(0xFE000801));  // MyLight.esl
	CHECK(m.ToSession(0x02000801) == 0x01000801u);
	CHECK(!m.ToLocal(0x02000801));  // Theirs.esp
	CHECK(!m.ToLocal(0x07000801));  // a place the session's table has no plugin at
	CHECK(m.ToSession(0) == 0u);
	CHECK(!m.ToSession(0xFF001234));  // a runtime form: no other game has it
	CHECK(!m.ToLocal(0xFF001234));
	CHECK(m.OursOnly() == 2);
	CHECK(m.SessionsOnly() == 1);
	CHECK(m.LocalPlugin(0xFF001234).empty());
	// The same plugins in the same places: every plugin's form is itself, a runtime form still has none.
	const Mapper same(Order(Names{ "Skyrim.esm" }), Order(Names{ "Skyrim.esm" }));
	REQUIRE(same.Identity());
	CHECK(same.ToSession(0x00012E46) == 0x00012E46u);
	CHECK(!same.ToSession(0xFF000D2A));
	CHECK(!same.ToLocal(0xFF000D2A));
	CHECK(!Mapper().ToSession(0xFF000D2A));  // before the session's table has come
}

TEST("form ids: a game sends a plugin form for a runtime one: its template, followed a few steps, or nothing")
{
	using FormIds::IsRuntime;
	using FormIds::Shareable;
	CHECK(IsRuntime(0xFF000D2A));
	CHECK(!IsRuntime(0xFE000801));
	CHECK(!IsRuntime(0));
	const auto none = [](std::uint32_t) -> std::uint32_t { return 0; };
	// A plugin's form is itself; 0 is nothing.
	CHECK(Shareable(0x00012E46, none) == 0x00012E46u);
	CHECK(Shareable(0xFE000801, none) == 0xFE000801u);
	CHECK(!Shareable(0, none));
	// A runtime copy (an armor a mod's script made) goes as the plugin armor it was made from, through a copy of a copy.
	const auto templates = [](std::uint32_t a_id) -> std::uint32_t {
		switch (a_id) {
		case 0xFF000D2A: return 0x00012E46;  // Iron Armor
		case 0xFF000D30: return 0xFF000D2A;
		case 0xFF000E00: return 0xFF000E01;  // two runtime forms naming each other: broken data
		case 0xFF000E01: return 0xFF000E00;
		default: return 0;
		}
	};
	CHECK(Shareable(0xFF000D2A, templates) == 0x00012E46u);
	CHECK(Shareable(0xFF000D30, templates) == 0x00012E46u);
	// One with no template, or only runtime ones, goes as nothing (left out), never as its own ID.
	CHECK(!Shareable(0xFF000D40, templates));
	CHECK(!Shareable(0xFF000E00, templates));
}

TEST("form ids: a joiner's other plugins are added after the rest of their kind, so the table's IDs stay")
{
	auto table = Order(BASE, { "L1.esl" });
	const auto before = table;
	CHECK(Extend(table, Order(BASE, { "L1.esl" })) == 0);
	CHECK(table == before);

	CHECK(Extend(table, Order(Names{ "Skyrim.esm", "New.esp", "Dragonborn.esm" }, { "L2.esl", "l1.ESL" })) == 2);
	CHECK(table == Order(Names{ "Skyrim.esm", "Dawnguard.esm", "Dragonborn.esm", "New.esp" }, { "L1.esl", "L2.esl" }));
	// The old IDs are what they were.
	const Mapper m(before, table);
	CHECK(m.ToSession(0x02000801) == 0x02000801u);
	CHECK(m.ToSession(0xFE000801) == 0xFE000801u);

	// An empty table becomes the first list.
	Names empty;
	CHECK(Extend(empty, Order(BASE)) == 3);
	CHECK(empty == Order(BASE));
}

TEST("form ids: the table stops growing where places or the wire's list run out")
{
	Names full;
	for (int i = 0; i < 300; ++i) {
		full.push_back(std::format("Mod{}.esp", i));
	}
	Names table;
	Extend(table, Order(full));
	CHECK(Table::Of(table).full.size() == FormIds::MAX_FULL);

	Names light;
	for (int i = 0; i < 600; ++i) {
		light.push_back(std::format("Light{}.esl", i));
	}
	Extend(table, Order({}, light));
	CHECK(table.size() < Protocol::MAX_PLUGINS);
	CHECK(Table::Of(table).Size() + 1 == table.size());
}

TEST("form ids: a whole ESL-heavy load order, 254 full plugins and 4096 light ones, makes a table that goes in a hello")
{
	Names full;
	for (int i = 0; i < 254; ++i) {
		full.push_back(std::format("Full{:03}.esp", i));
	}
	Names light;
	for (int i = 0; i < 4096; ++i) {
		light.push_back(std::format("Light{:04}.esl", i));
	}
	Names table;
	CHECK(Extend(table, Order(full, light)) == 254 + 4096);
	CHECK(table == Order(full, light));
	CHECK(table.size() <= Protocol::MAX_PLUGINS);
	CHECK(Protocol::PluginsFit(table));

	// No more places: another light plugin is left out.
	CHECK(Extend(table, Order({}, { "OneMore.esl" })) == 0);
	CHECK(!Table::Of(table).Find("OneMore.esl"));

	// The last light place and the last full one translate both ways, by name in any case.
	Names reversed(light.rbegin(), light.rend());
	for (auto& name : reversed) {
		std::ranges::transform(name, name.begin(), [](char a_c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(a_c))); });
	}
	const Mapper m(Order(full, reversed), table);
	CHECK(!m.Identity());
	CHECK(m.OursOnly() == 0);
	CHECK(m.SessionsOnly() == 0);
	CHECK(m.ToSession(0xFE000123) == 0xFEFFF123u);  // LIGHT4095: our light 0, the session's light FFF
	CHECK(m.ToLocal(0xFEFFF123) == 0xFE000123u);
	CHECK(m.ToSession(0xFD001234) == 0xFD001234u);
	CHECK(m.SessionPlugin(0xFEFFF001) == "Light4095.esl");
}

TEST("form ids: the table stops growing where its list would pass a hello's bytes")
{
	const auto named = [](std::string a_name) {
		a_name.insert(0, Protocol::MAX_PLUGIN_NAME_LENGTH - a_name.size(), 'x');
		return a_name;
	};
	Names light;
	for (int i = 0; i < 4096; ++i) {
		light.push_back(named(std::format("{:04}.esl", i)));
	}
	Names table;
	const auto added = Extend(table, Order({}, light));
	CHECK(added > 0);
	CHECK(added < light.size());
	CHECK(Protocol::PluginsFit(table));
	CHECK(Protocol::PluginsCarried(table) == table.size());
	// One more name of that length and it would not.
	auto more = table;
	more.push_back(light[added]);
	CHECK(!Protocol::PluginsFit(more));
}
