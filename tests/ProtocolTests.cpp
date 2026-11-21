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

TEST("protocol: a hello carries a whole Skyrim load order, and no more plugins than the limit")
{
	std::vector<std::string> full;
	for (int i = 0; i < 254; ++i) {
		full.push_back(std::format("Full plugin {:03}.esp", i));
	}
	std::vector<std::string> light;
	for (std::size_t i = full.size() + 1; i < Protocol::MAX_PLUGINS; ++i) {
		light.push_back(std::format("Light plugin {:04}.esl", i));
	}
	auto hello = SampleHello();
	hello.plugins = Protocol::ComparedLoadOrder(full, light);
	REQUIRE(hello.plugins.size() == Protocol::MAX_PLUGINS);
	const auto back = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(back);
	CHECK(back->plugins == hello.plugins);

	// One more than the limit: the writer leaves the rest off.
	hello.plugins.push_back("One too many.esp");
	const auto cut = Protocol::DecodeHello(Protocol::Encode(hello));
	REQUIRE(cut);
	CHECK(cut->plugins.size() == Protocol::MAX_PLUGINS);
	CHECK(cut->plugins.back() != "One too many.esp");

	// A packet that claims more plugins than the limit is not read at all.
	Protocol::Writer writer{ MT::kHello };
	writer.U32(Protocol::MAGIC);
	writer.U16(Protocol::VERSION);
	writer.U32(0);
	writer.Str("name", Protocol::MAX_NAME_LENGTH);
	writer.Str("", Protocol::MAX_PASSWORD_LENGTH);
	writer.U64(0);
	writer.U16(static_cast<std::uint16_t>(Protocol::MAX_PLUGINS + 1));
	for (std::size_t i = 0; i <= Protocol::MAX_PLUGINS; ++i) {
		writer.LongStr("a.esp", Protocol::MAX_PLUGIN_NAME_LENGTH);
	}
	CHECK(!Protocol::DecodeHello(writer.Data()).has_value());
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
