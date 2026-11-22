#include "Test.h"

#include "Protocol.h"

#include <limits>
#include <string>
#include <vector>

// The wire format (common/Protocol.h): every message comes back as it went, and nothing malformed is read as
// a message.

namespace
{
	using MT = Protocol::MessageType;

	Protocol::PlayerState SampleState()
	{
		Protocol::PlayerState state;
		state.sequence = 0xDEADBEEF;
		state.cell = 0x00016BD0;
		state.worldspace = 0;
		state.x = -1234.5f;
		state.y = 98765.25f;
		state.z = 0.125f;
		state.heading = 3.1f;
		state.speed = 370.0f;
		state.flags = Protocol::kSneaking | Protocol::kWeaponDrawn;
		state.sentMs = 123456789;
		state.direction = 192;
		state.pitch = -45;
		return state;
	}

	Protocol::Hello SampleHello()
	{
		Protocol::Hello hello;
		hello.contentHash = 0xCAFEF00D;
		hello.name = "Lydia";
		hello.password = "secret";
		hello.identity = 0x0123456789ABCDEFull;
		hello.plugins = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "Dawnguard.esm" }, { "ccBGSSSE001-Fish.esm" });
		return hello;
	}

	// Every prefix of a_packet shorter than the whole, and the packet with one byte too many, must not decode.
	template <class Decode>
	void CheckStrict(const std::vector<std::uint8_t>& a_packet, Decode a_decode)
	{
		CHECK(a_decode(a_packet).has_value());
		for (std::size_t length = 0; length < a_packet.size(); ++length) {
			const std::span<const std::uint8_t> cut{ a_packet.data(), length };
			if (a_decode(cut).has_value()) {
				Test::Fail(__FILE__, __LINE__, std::format("a packet cut to {} of {} bytes was read", length, a_packet.size()));
			}
		}
		auto longer = a_packet;
		longer.push_back(0);
		CHECK(!a_decode(longer).has_value());
	}
}

TEST("protocol: a hello comes back as it went")
{
	const auto hello = SampleHello();
	const auto packet = Protocol::Encode(hello);
	CHECK(Protocol::PeekType(packet) == MT::kHello);
	const auto back = Protocol::DecodeHello(packet);
	REQUIRE(back);
	CHECK(*back == hello);
}

TEST("protocol: a hello cut short or with bytes left over is not read")
{
	const auto packet = Protocol::Encode(SampleHello());
	// A hello is read up to its version before anything else is asked of it, so the first seven bytes
	// (type, magic, version) are the least a readable one has.
	CHECK(Protocol::DecodeHello(packet).has_value());
	for (std::size_t length = 0; length < packet.size(); ++length) {
		const std::span<const std::uint8_t> cut{ packet.data(), length };
		if (Protocol::DecodeHello(cut).has_value()) {
			Test::Fail(__FILE__, __LINE__, std::format("a hello cut to {} of {} bytes was read", length, packet.size()));
		}
	}
	auto longer = packet;
	longer.push_back(0);
	CHECK(!Protocol::DecodeHello(longer).has_value());
}

TEST("protocol: a hello of another version is read up to its version only")
{
	auto hello = SampleHello();
	hello.version = static_cast<std::uint16_t>(Protocol::VERSION + 7);
	auto packet = Protocol::Encode(hello);
	// Whatever follows the version may be laid out another way: cut it off and put junk there.
	packet.resize(7);
	packet.insert(packet.end(), { 0xFF, 0xFF, 0xFF });
	const auto back = Protocol::DecodeHello(packet);
	REQUIRE(back);
	CHECK(back->version == Protocol::VERSION + 7);
	CHECK(back->magic == Protocol::MAGIC);
	CHECK(back->name.empty());
	CHECK(back->plugins.empty());

	auto other = SampleHello();
	other.magic = 0x11223344;
	const auto otherBack = Protocol::DecodeHello(Protocol::Encode(other));
	REQUIRE(otherBack);
	CHECK(otherBack->magic == 0x11223344);
	CHECK(otherBack->name.empty());
}

TEST("protocol: the magic spells the mod's four letters")
{
	const auto packet = Protocol::Encode(Protocol::Hello{});
	REQUIRE(packet.size() > 5);
	CHECK(std::string(packet.begin() + 1, packet.begin() + 5) == "SKCO");
}

TEST("protocol: a welcome, a refusal, a join and a leave come back as they went")
{
	const Protocol::Welcome welcome{ 3, 0xFEDCBA9876543210ull, 1 };
	const auto              welcomeBack = Protocol::DecodeWelcome(Protocol::Encode(welcome));
	REQUIRE(welcomeBack);
	CHECK(*welcomeBack == welcome);
	CheckStrict(Protocol::Encode(welcome), [](std::span<const std::uint8_t> a_data) { return Protocol::DecodeWelcome(a_data); });

	const auto rejectBack = Protocol::DecodeReject(Protocol::Encode(Protocol::Reject{ "Wrong password." }));
	REQUIRE(rejectBack);
	CHECK(rejectBack->reason == "Wrong password.");
	CheckStrict(Protocol::Encode(Protocol::Reject{ "Wrong password." }), [](std::span<const std::uint8_t> a_data) { return Protocol::DecodeReject(a_data); });

	const Protocol::PlayerJoined joined{ 2, "J'zargo" };
	const auto                   joinedBack = Protocol::DecodePlayerJoined(Protocol::Encode(joined));
	REQUIRE(joinedBack);
	CHECK(*joinedBack == joined);
	CheckStrict(Protocol::Encode(joined), [](std::span<const std::uint8_t> a_data) { return Protocol::DecodePlayerJoined(a_data); });

	const auto leftBack = Protocol::DecodePlayerLeft(Protocol::Encode(Protocol::PlayerLeft{ 7 }));
	REQUIRE(leftBack);
	CHECK(leftBack->playerId == 7);
	CheckStrict(Protocol::Encode(Protocol::PlayerLeft{ 7 }), [](std::span<const std::uint8_t> a_data) { return Protocol::DecodePlayerLeft(a_data); });
}

TEST("protocol: a state comes back as it went, alone and as another player's")
{
	const auto state = SampleState();
	const auto packet = Protocol::Encode(state);
	CHECK(Protocol::PeekType(packet) == MT::kPlayerState);
	const auto back = Protocol::DecodePlayerState(packet);
	REQUIRE(back);
	CHECK(*back == state);
	CheckStrict(packet, [](std::span<const std::uint8_t> a_data) { return Protocol::DecodePlayerState(a_data); });

	const Protocol::RemoteState remote{ 5, state };
	const auto                  remotePacket = Protocol::Encode(remote);
	CHECK(Protocol::PeekType(remotePacket) == MT::kRemoteState);
	const auto remoteBack = Protocol::DecodeRemoteState(remotePacket);
	REQUIRE(remoteBack);
	CHECK(*remoteBack == remote);
	CheckStrict(remotePacket, [](std::span<const std::uint8_t> a_data) { return Protocol::DecodeRemoteState(a_data); });
}

TEST("protocol: a state with a position no game could be at is not read")
{
	const float bad[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 16777216.0f, -1.0e20f };
	for (const float value : bad) {
		for (int field = 0; field < 3; ++field) {
			auto state = SampleState();
			(field == 0 ? state.x : field == 1 ? state.y : state.z) = value;
			CHECK(!state.IsFinite());
			CHECK(!Protocol::DecodePlayerState(Protocol::Encode(state)).has_value());
			CHECK(!Protocol::DecodeRemoteState(Protocol::Encode(Protocol::RemoteState{ 1, state })).has_value());
		}
	}
	for (const float value : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() }) {
		auto state = SampleState();
		state.heading = value;
		CHECK(!Protocol::DecodePlayerState(Protocol::Encode(state)).has_value());
		state = SampleState();
		state.speed = value;
		CHECK(!Protocol::DecodePlayerState(Protocol::Encode(state)).has_value());
	}
	auto edge = SampleState();
	edge.x = 16777215.0f;
	edge.y = -16777215.0f;
	CHECK(Protocol::DecodePlayerState(Protocol::Encode(edge)).has_value());
}

TEST("protocol: a heartbeat and its answer carry the token")
{
	for (const auto type : { MT::kHeartbeat, MT::kHeartbeatAck }) {
		const auto packet = Protocol::Encode(Protocol::Heartbeat{ 0xA1B2C3D4 }, type);
		CHECK(Protocol::PeekType(packet) == type);
		const auto back = Protocol::DecodeHeartbeat(packet);
		REQUIRE(back);
		CHECK(back->token == 0xA1B2C3D4);
		CheckStrict(packet, [](std::span<const std::uint8_t> a_data) { return Protocol::DecodeHeartbeat(a_data); });
	}
}

TEST("protocol: an empty packet has no type")
{
	CHECK(!Protocol::PeekType({}).has_value());
}

TEST("protocol: a name too long is cut at a whole character, never inside one")
{
	// 127 letters, then a two-byte character that would straddle the 128-byte limit.
	const std::string name = std::string(127, 'a') + "\xC3\xA9" + "tail";
	auto              hello = SampleHello();
	hello.name = name;
	const auto back = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(back);
	CHECK(back->name == std::string(127, 'a'));

	CHECK(Protocol::Utf8Prefix("abc", 10) == "abc");
	CHECK(Protocol::Utf8Prefix("abc", 2) == "ab");
	CHECK(Protocol::Utf8Prefix("a\xE5\x86\x85" "b", 3) == "a");
	CHECK(Protocol::Utf8Prefix("a\xE5\x86\x85" "b", 4) == "a\xE5\x86\x85");
	CHECK(Protocol::Utf8Prefix("\xF0\x9F\x98\x80", 3).empty());
	CHECK(Protocol::Utf8Prefix("abc", 0).empty());
}

TEST("protocol: control characters in a string are dropped when it is read")
{
	Protocol::PlayerJoined joined{ 1, "Na\nme\x7F\twith\x01 junk" };
	const auto             back = Protocol::DecodePlayerJoined(Protocol::Encode(joined));
	REQUIRE(back);
	CHECK(back->name == "Namewith junk");
}

TEST("protocol: a string that claims more bytes than the packet holds, or than it may have, is not read")
{
	// A refusal whose length byte says 50 with 3 bytes behind it.
	const std::vector<std::uint8_t> shortPacket{ static_cast<std::uint8_t>(MT::kReject), 50, 'a', 'b', 'c' };
	CHECK(!Protocol::DecodeReject(shortPacket).has_value());

	// A reason of 201 bytes: one more than a reason may have.
	std::vector<std::uint8_t> longPacket{ static_cast<std::uint8_t>(MT::kReject), 201 };
	longPacket.insert(longPacket.end(), 201, 'x');
	CHECK(!Protocol::DecodeReject(longPacket).has_value());
	longPacket[1] = 200;
	longPacket.pop_back();
	CHECK(Protocol::DecodeReject(longPacket).has_value());
}

namespace
{
	// The longest load order a game can have: 254 full plugins and 4096 light ones, each name a_length bytes (at
	// least 24), laid out as a hello carries it.
	std::vector<std::string> WholeLoadOrder(std::size_t a_length = 40)
	{
		const auto named = [&](std::string a_name) {
			a_name.insert(a_name.size() - 4, a_length - a_name.size(), '_');
			return a_name;
		};
		std::vector<std::string> full;
		for (int i = 0; i < 254; ++i) {
			full.push_back(named(std::format("Full plugin {:03}.esp", i)));
		}
		std::vector<std::string> light;
		for (int i = 0; i < 4096; ++i) {
			light.push_back(named(std::format("Light plugin {:04}.esl", i)));
		}
		return Protocol::ComparedLoadOrder(full, light);
	}

	// A hello's fields as the server reads them, then a_count and the names as given (not as the writer would cut them).
	std::vector<std::uint8_t> HandWrittenHello(std::size_t a_count, const std::vector<std::string>& a_names)
	{
		Protocol::Writer writer{ MT::kHello };
		writer.U32(Protocol::MAGIC);
		writer.U16(Protocol::VERSION);
		writer.U32(0);
		writer.Str("name", Protocol::MAX_NAME_LENGTH);
		writer.Str("", Protocol::MAX_PASSWORD_LENGTH);
		writer.U64(0);
		writer.U16(static_cast<std::uint16_t>(a_count));
		for (const auto& name : a_names) {
			writer.LongStr(name, 0xFFFF);
		}
		return writer.Data();
	}
}

TEST("protocol: a hello carries a whole Skyrim load order, 254 full plugins and 4096 light ones")
{
	auto hello = SampleHello();
	hello.plugins = WholeLoadOrder();
	REQUIRE(hello.plugins.size() == 254 + 1 + 4096);
	CHECK(hello.plugins.size() <= Protocol::MAX_PLUGINS);
	CHECK(Protocol::PluginsFit(hello.plugins));
	const auto back = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(back);
	CHECK(back->plugins == hello.plugins);

	// Names of 110 bytes on average still go whole; that is what MAX_PLUGIN_LIST_BYTES promises.
	hello.plugins = WholeLoadOrder(110);
	CHECK(Protocol::PluginsFit(hello.plugins));
	const auto longNames = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(longNames);
	CHECK(longNames->plugins == hello.plugins);
}

TEST("protocol: a hello with more plugins than the limit leaves the rest off, and one that claims more is not read")
{
	auto hello = SampleHello();
	hello.plugins.clear();
	for (std::size_t i = 0; i < Protocol::MAX_PLUGINS; ++i) {
		hello.plugins.push_back(std::format("P{}.esl", i));
	}
	CHECK(Protocol::PluginsFit(hello.plugins));
	const auto atLimit = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(atLimit);
	CHECK(atLimit->plugins == hello.plugins);

	// One more than the limit: the writer leaves the rest off.
	hello.plugins.push_back("One too many.esp");
	CHECK(!Protocol::PluginsFit(hello.plugins));
	const auto cut = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(cut);
	CHECK(cut->plugins.size() == Protocol::MAX_PLUGINS);
	CHECK(cut->plugins.back() != "One too many.esp");

	// A packet that claims more plugins than the limit is not read at all.
	CHECK(!Protocol::DecodeHello(HandWrittenHello(Protocol::MAX_PLUGINS + 1, std::vector<std::string>(Protocol::MAX_PLUGINS + 1, "a.esp"))).has_value());
	CHECK(Protocol::DecodeHello(HandWrittenHello(Protocol::MAX_PLUGINS, std::vector<std::string>(Protocol::MAX_PLUGINS, "a.esp"))).has_value());
}

TEST("protocol: the longest hello fits in one message on every transport, its list cut at whole names")
{
	// Every field at its longest, and as many plugins as the limit allows, each with a name past the longest.
	Protocol::Hello hello;
	hello.contentHash = 0xFFFFFFFF;
	hello.name = std::string(Protocol::MAX_NAME_LENGTH + 10, 'n');
	hello.password = std::string(Protocol::MAX_PASSWORD_LENGTH + 10, 'p');
	hello.identity = ~std::uint64_t{ 0 };
	hello.plugins = std::vector<std::string>(Protocol::MAX_PLUGINS, std::string(Protocol::MAX_PLUGIN_NAME_LENGTH + 10, 'x'));
	CHECK(!Protocol::PluginsFit(hello.plugins));

	const auto packet = Protocol::Encode(hello);
	CHECK(packet.size() <= Protocol::MAX_HELLO_SIZE);
	CHECK(packet.size() <= Protocol::MAX_MESSAGE_SIZE);
	// The list fills its bytes to within one name.
	CHECK(packet.size() + Protocol::PluginNameBytes(hello.plugins[0]) > Protocol::MAX_HELLO_SIZE);

	const auto back = Protocol::DecodeHello(packet);
	REQUIRE(back);
	CHECK(back->plugins.size() == Protocol::PluginsCarried(hello.plugins));
	CHECK(back->plugins.size() < Protocol::MAX_PLUGINS);
	CHECK(std::ranges::all_of(back->plugins, [](const std::string& a_name) { return a_name.size() == Protocol::MAX_PLUGIN_NAME_LENGTH; }));
}

TEST("protocol: a plugin list past its bytes is not read, even with fewer names than the limit")
{
	const std::string longest(Protocol::MAX_PLUGIN_NAME_LENGTH, 'x');
	const auto        fits = Protocol::PluginsCarried(std::vector<std::string>(Protocol::MAX_PLUGINS, longest));
	REQUIRE(fits < Protocol::MAX_PLUGINS);
	CHECK(Protocol::DecodeHello(HandWrittenHello(fits, std::vector<std::string>(fits, longest))).has_value());
	CHECK(!Protocol::DecodeHello(HandWrittenHello(fits + 1, std::vector<std::string>(fits + 1, longest))).has_value());
}

TEST("protocol: a plugin's name longer than a file name may be is not read")
{
	Protocol::Writer writer{ MT::kHello };
	writer.U32(Protocol::MAGIC);
	writer.U16(Protocol::VERSION);
	writer.U32(0);
	writer.Str("name", Protocol::MAX_NAME_LENGTH);
	writer.Str("", Protocol::MAX_PASSWORD_LENGTH);
	writer.U64(0);
	writer.U16(1);
	writer.LongStr(std::string(Protocol::MAX_PLUGIN_NAME_LENGTH + 1, 'p'), 0xFFFF);
	CHECK(!Protocol::DecodeHello(writer.Data()).has_value());

	// The writer itself cuts a name that long.
	auto hello = SampleHello();
	hello.plugins = { std::string(Protocol::MAX_PLUGIN_NAME_LENGTH + 40, 'p') };
	const auto back = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(back);
	REQUIRE(back->plugins.size() == 1);
	CHECK(back->plugins[0].size() == Protocol::MAX_PLUGIN_NAME_LENGTH);
}

TEST("protocol: the load order's hash is the same in any case, and differs with the order and the kind")
{
	const auto base = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "A.esp" }, { "L.esl" });
	const auto shouted = Protocol::ComparedLoadOrder({ "SKYRIM.ESM", "update.ESM", "a.esp" }, { "l.ESL" });
	CHECK(Protocol::LoadOrderHash(base) == Protocol::LoadOrderHash(shouted));

	const auto swapped = Protocol::ComparedLoadOrder({ "Update.esm", "Skyrim.esm", "A.esp" }, { "L.esl" });
	CHECK(Protocol::LoadOrderHash(base) != Protocol::LoadOrderHash(swapped));

	// The same files, one of them counted as light on one side: the form IDs differ, so the hash must.
	const auto otherKind = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm" }, { "A.esp", "L.esl" });
	CHECK(Protocol::LoadOrderHash(base) != Protocol::LoadOrderHash(otherKind));

	// Two names run together are not one name.
	CHECK(Protocol::LoadOrderHash({ "ab", "c" }) != Protocol::LoadOrderHash({ "a", "bc" }));

	REQUIRE(base.size() == 5);
	CHECK(base[3] == Protocol::LIGHT_PLUGINS);
	CHECK(Protocol::SamePluginName("Skyrim.esm", "SKYRIM.ESM"));
	CHECK(!Protocol::SamePluginName("Skyrim.esm", "Skyrim.esp"));
	CHECK(!Protocol::SamePluginName("Skyrim.esm", "Skyrim.es"));
}

TEST("protocol: direction and pitch pack into a byte each")
{
	CHECK(Protocol::PackDirection(0.0f) == 0);
	CHECK(Protocol::PackDirection(0.25f) == 64);
	CHECK(Protocol::PackDirection(0.5f) == 128);
	CHECK(Protocol::PackDirection(-0.25f) == 192);
	CHECK(Protocol::PackDirection(1.0f) == 0);
	CHECK(Protocol::PackDirection(std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(Protocol::UnpackDirection(64) == 0.25f);

	CHECK(Protocol::PackPitch(0.0f) == 0);
	CHECK(Protocol::PackPitch(44.6f) == 45);
	CHECK(Protocol::PackPitch(-200.0f) == -90);
	CHECK(Protocol::PackPitch(200.0f) == 90);
	CHECK(Protocol::PackPitch(std::numeric_limits<float>::infinity()) == 0);
}

TEST("protocol: the time a state was sampled is never 0, which means not known")
{
	using Clock = std::chrono::steady_clock;
	CHECK(Protocol::SentMs(Clock::time_point{}) == 1);
	CHECK(Protocol::SentMs(Clock::time_point{ std::chrono::milliseconds(5000) }) == 5000);
	// It wraps after 49 days rather than stopping.
	CHECK(Protocol::SentMs(Clock::time_point{ std::chrono::milliseconds(0x100000000ll + 42) }) == 42);
}

namespace
{
	Protocol::PlayerLooks SampleLooks()
	{
		Protocol::PlayerLooks looks;
		looks.race = 0x00013746;  // a race's form id, as Skyrim.esm numbers them
		looks.female = true;
		looks.weight = 62.5f;
		looks.worn = { 0x00012E49, 0x00012E4B, 0x00012E46, 0x00012E4D };
		return looks;
	}
}

TEST("protocol: a player's looks come back as they went, alone and as another player's")
{
	const auto looks = SampleLooks();
	const auto packet = Protocol::Encode(looks);
	CHECK(Protocol::PeekType(packet) == MT::kPlayerLooks);
	const auto back = Protocol::DecodePlayerLooks(packet);
	REQUIRE(back);
	CHECK(*back == looks);
	CheckStrict(packet, [](std::span<const std::uint8_t> a_data) { return Protocol::DecodePlayerLooks(a_data); });

	const Protocol::RemoteLooks remote{ 4, looks };
	const auto                  remotePacket = Protocol::Encode(remote);
	CHECK(Protocol::PeekType(remotePacket) == MT::kRemoteLooks);
	const auto remoteBack = Protocol::DecodeRemoteLooks(remotePacket);
	REQUIRE(remoteBack);
	CHECK(*remoteBack == remote);
	CheckStrict(remotePacket, [](std::span<const std::uint8_t> a_data) { return Protocol::DecodeRemoteLooks(a_data); });

	// Nothing worn is a look too (a character in nothing but the body).
	Protocol::PlayerLooks bare;
	bare.race = 0x00013741;
	const auto bareBack = Protocol::DecodePlayerLooks(Protocol::Encode(bare));
	REQUIRE(bareBack);
	CHECK(*bareBack == bare);
}

TEST("protocol: looks are sent with at most MAX_WORN items, and read with no more")
{
	auto many = SampleLooks();
	many.worn.clear();
	for (std::uint32_t i = 0; i < Protocol::MAX_WORN + 5; ++i) {
		many.worn.push_back(0x00010000 + i);
	}
	const auto back = Protocol::DecodePlayerLooks(Protocol::Encode(many));
	REQUIRE(back);
	CHECK(back->worn.size() == Protocol::MAX_WORN);
	CHECK(back->worn.front() == 0x00010000);

	// A packet that claims more than MAX_WORN, with all of them there, is not read.
	Protocol::Writer writer{ MT::kPlayerLooks };
	writer.U32(0x00013746);
	writer.U8(0);
	writer.F32(50.0f);
	writer.U8(static_cast<std::uint8_t>(Protocol::MAX_WORN + 1));
	for (std::size_t i = 0; i <= Protocol::MAX_WORN; ++i) {
		writer.U32(0x00012E49);
	}
	CHECK(!Protocol::DecodePlayerLooks(writer.Data()).has_value());
}

TEST("protocol: looks with a sex that is neither, or a weight that is no number, are not read")
{
	auto packet = Protocol::Encode(SampleLooks());
	packet[1 + 4] = 2;  // after the type and the race: the sex byte
	CHECK(!Protocol::DecodePlayerLooks(packet).has_value());

	for (const float value : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() }) {
		auto looks = SampleLooks();
		looks.weight = value;
		CHECK(!looks.IsFinite());
		CHECK(!Protocol::DecodePlayerLooks(Protocol::Encode(looks)).has_value());
		CHECK(!Protocol::DecodeRemoteLooks(Protocol::Encode(Protocol::RemoteLooks{ 1, looks })).has_value());
	}
}

TEST("protocol: looks with a body weight outside 0 to 100 are not read; the ends are")
{
	for (const float value : { -0.5f, 100.5f, 1e30f, -1e30f }) {
		auto looks = SampleLooks();
		looks.weight = value;
		CHECK(!Protocol::DecodePlayerLooks(Protocol::Encode(looks)).has_value());
		CHECK(!Protocol::DecodeRemoteLooks(Protocol::Encode(Protocol::RemoteLooks{ 1, looks })).has_value());
	}
	for (const float value : { 0.0f, 100.0f }) {
		auto looks = SampleLooks();
		looks.weight = value;
		const auto read = Protocol::DecodePlayerLooks(Protocol::Encode(looks));
		REQUIRE(read);
		CHECK(read->weight == value);
	}
}
