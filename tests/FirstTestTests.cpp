#include "Test.h"

#include "AddressLibraryFile.h"
#include "DevLine.h"
#include "FirstTest.h"
#include "MainQueue.h"
#include "net/RelayProbe.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

// The game-free parts of the first test in a game: the Address Library's file name, the developer channel's lines,
// the main thread's queue, the relay probe and the log lines of the lobby test (docs/GETTING-STARTED.md).

using namespace std::chrono_literals;

TEST("address library: the tested game reads versionlib-1-7-104-0.bin, the old line version-*.bin")
{
	CHECK(AddressLibraryFile::Name(1, 7, 104, 0) == "versionlib-1-7-104-0.bin");
	CHECK(AddressLibraryFile::Path(1, 7, 104, 0) == "Data/SKSE/Plugins/versionlib-1-7-104-0.bin");
	CHECK(AddressLibraryFile::Name(1, 6, 1170, 0) == "versionlib-1-6-1170-0.bin");
	CHECK(AddressLibraryFile::Name(1, 6, 317, 0) == "versionlib-1-6-317-0.bin");
	CHECK(AddressLibraryFile::Name(1, 5, 97, 0) == "version-1-5-97-0.bin");
	CHECK(AddressLibraryFile::Name(2, 0, 0, 0) == "versionlib-2-0-0-0.bin");
}

TEST("dev line: bytes in any pieces come out as whole lines, without line ends or empty lines")
{
	DevLine::LineBuffer buffer;
	buffer.Append("auth ab");
	CHECK(!buffer.Next());
	buffer.Append("cd\r\n\r\n\nsteam ho");
	CHECK(buffer.Next() == std::optional<std::string>{ "auth abcd" });
	CHECK(!buffer.Next());
	buffer.Append("st\nstatus\n");
	CHECK(buffer.Next() == std::optional<std::string>{ "steam host" });
	CHECK(buffer.Next() == std::optional<std::string>{ "status" });
	CHECK(!buffer.Next());
	CHECK(!buffer.Overflowed());
}

TEST("dev line: a line with no end that outgrows the limit overflows; a long line that ends does not")
{
	DevLine::LineBuffer endless;
	endless.Append(std::string(DevLine::MAX_LINE + 1, 'x'));
	CHECK(endless.Overflowed());

	DevLine::LineBuffer ended;
	ended.Append(std::string(DevLine::MAX_LINE + 1, 'x') + "\n");
	CHECK(!ended.Overflowed());
	CHECK(ended.Next()->size() == DevLine::MAX_LINE + 1);
}

TEST("dev line: a command is split from its arguments, and the arguments into words")
{
	const auto join = DevLine::Split("steam join 109775241234567890");
	CHECK(join.name == "steam");
	CHECK(join.args == "join 109775241234567890");
	const auto words = DevLine::Words("  join   109775241234567890 ");
	REQUIRE(words.size() == 2);
	CHECK(words[0] == "join");
	CHECK(words[1] == "109775241234567890");
	CHECK(DevLine::Split("status").name == "status");
	CHECK(DevLine::Split("status").args.empty());
	CHECK(DevLine::Words("").empty());
}

TEST("dev line: only the exact token authorises; an empty token never does")
{
	CHECK(DevLine::Authorises("auth 0123abcd", "0123abcd"));
	CHECK(!DevLine::Authorises("auth 0123abce", "0123abcd"));
	CHECK(!DevLine::Authorises("auth 0123abc", "0123abcd"));
	CHECK(!DevLine::Authorises("auth 0123abcdd", "0123abcd"));
	CHECK(!DevLine::Authorises("0123abcd", "0123abcd"));
	CHECK(!DevLine::Authorises("auth ", ""));
	CHECK(!DevLine::Authorises("auth x", ""));
}

TEST("dev line: numbers are whole decimal numbers or nothing")
{
	CHECK(DevLine::Number("109775241234567890") == std::optional<std::uint64_t>{ 109775241234567890ull });
	CHECK(DevLine::Number("5") == std::optional<std::uint64_t>{ 5 });
	CHECK(!DevLine::Number(""));
	CHECK(!DevLine::Number("5x"));
	CHECK(!DevLine::Number("-5"));
	CHECK(!DevLine::Number("99999999999999999999999"));
}

TEST("main queue: work posted from other threads runs on Drain, once each, in the order posted")
{
	MainQueue          queue;
	std::vector<int>   ran;
	std::atomic<int>   posted{ 0 };
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&, t] {
			for (int i = 0; i < 100; ++i) {
				queue.Post([&ran, t, i] { ran.push_back(t * 1000 + i); });
				++posted;
			}
		});
	}
	for (auto& thread : threads) {
		thread.join();
	}
	CHECK(queue.Waiting() == 400);
	CHECK(queue.Drain() == 400);
	CHECK(ran.size() == 400);
	CHECK(queue.Waiting() == 0);
	CHECK(queue.Drain() == 0);
	// within one thread, the order it posted in
	for (int t = 0; t < 4; ++t) {
		int last = -1;
		for (const int each : ran) {
			if (each / 1000 == t) {
				CHECK(each % 1000 == last + 1);
				last = each % 1000;
			}
		}
		CHECK(last == 99);
	}
	CHECK(queue.Most() == 400);
}

TEST("main queue: work that posts more runs that at the next Drain, not in the same one")
{
	MainQueue queue;
	int       second = 0;
	queue.Post([&] { queue.Post([&] { ++second; }); });
	CHECK(queue.Drain() == 1);
	CHECK(second == 0);
	CHECK(queue.Drain() == 1);
	CHECK(second == 1);
}

TEST("relay probe: a ping is answered with a pong of its number, and the pong gives the round trip")
{
	const RelayProbe::Clock::time_point t0{};
	RelayProbe                          host, friendGame;

	const auto ping = friendGame.Ping(t0);
	CHECK(ping == "skyrimcoop ping 1");

	const auto heard = host.Hear(ping, t0 + 20ms);
	CHECK(heard.kind == RelayProbe::Kind::kPing);
	CHECK(heard.number == 1);
	CHECK(heard.reply == "skyrimcoop pong 1");

	const auto answer = friendGame.Hear(heard.reply, t0 + 45ms);
	CHECK(answer.kind == RelayProbe::Kind::kPong);
	REQUIRE(answer.rtt.has_value());
	CHECK(*answer.rtt == 45ms);

	const auto summary = friendGame.Summarise(t0 + 1s, 5s);
	CHECK(summary.sent == 1);
	CHECK(summary.answered == 1);
	CHECK(summary.lost == 0);
	CHECK(summary.minMs == 45.0);
	CHECK(summary.maxMs == 45.0);
	CHECK(host.Summarise(t0 + 1s, 5s).pingsAnswered == 1);
}

TEST("relay probe: an answer that comes twice counts once, and other traffic is not ours")
{
	const RelayProbe::Clock::time_point t0{};
	RelayProbe                          probe;
	probe.Ping(t0);
	CHECK(probe.Hear("skyrimcoop pong 1", t0 + 10ms).kind == RelayProbe::Kind::kPong);
	CHECK(probe.Hear("skyrimcoop pong 1", t0 + 11ms).kind == RelayProbe::Kind::kStrayPong);
	CHECK(probe.Hear("skyrimcoop pong 7", t0 + 11ms).kind == RelayProbe::Kind::kStrayPong);
	CHECK(probe.Hear("skyrimcoop self test", t0).kind == RelayProbe::Kind::kNotOurs);
	CHECK(probe.Hear("skyrimcoop ping 0", t0).kind == RelayProbe::Kind::kNotOurs);
	CHECK(probe.Hear("skyrimcoop ping x", t0).kind == RelayProbe::Kind::kNotOurs);
	CHECK(probe.Hear("hello", t0).kind == RelayProbe::Kind::kNotOurs);
	CHECK(probe.Hear("", t0).kind == RelayProbe::Kind::kNotOurs);
	const auto summary = probe.Summarise(t0 + 1s, 5s);
	CHECK(summary.answered == 1);
	CHECK(summary.strayPongs == 2);
	CHECK(summary.pingsAnswered == 0);
}

TEST("relay probe: an unanswered ping is waiting until the timeout, then lost; the median is a measured value")
{
	const RelayProbe::Clock::time_point t0{};
	RelayProbe                          probe;
	for (int i = 0; i < 5; ++i) {
		probe.Ping(t0);
	}
	probe.Hear("skyrimcoop pong 1", t0 + 30ms);
	probe.Hear("skyrimcoop pong 2", t0 + 10ms);
	probe.Hear("skyrimcoop pong 3", t0 + 20ms);
	probe.Hear("skyrimcoop pong 4", t0 + 40ms);

	const auto early = probe.Summarise(t0 + 1s, 5s);
	CHECK(early.sent == 5);
	CHECK(early.answered == 4);
	CHECK(early.waiting == 1);
	CHECK(early.lost == 0);
	CHECK(early.minMs == 10.0);
	CHECK(early.medianMs == 20.0);
	CHECK(early.maxMs == 40.0);

	const auto late = probe.Summarise(t0 + 6s, 5s);
	CHECK(late.waiting == 0);
	CHECK(late.lost == 1);
	CHECK(RelayProbe::Describe(late) == "sent=5 answered=4 waiting=0 lost=1 pingsAnswered=0 strayPongs=0 rttMs min=10.0 median=20.0 max=40.0");
}

TEST("relay probe: after a reset the numbers go on, so a late answer to an old ping is a stray")
{
	const RelayProbe::Clock::time_point t0{};
	RelayProbe                          probe;
	probe.Ping(t0);
	probe.Reset();
	CHECK(probe.Ping(t0) == "skyrimcoop ping 2");
	CHECK(probe.Hear("skyrimcoop pong 1", t0 + 5ms).kind == RelayProbe::Kind::kStrayPong);
	CHECK(probe.Hear("skyrimcoop pong 2", t0 + 5ms).kind == RelayProbe::Kind::kPong);
	CHECK(probe.Summarise(t0, 5s).sent == 1);
}

TEST("first test: the host's log says when its lobby opened and when a player joined, with times")
{
	const FirstTest::Clock::time_point t0{};
	FirstTest                          test;
	CHECK(test.Look({}, t0).empty());
	test.AskedToHost(t0);
	CHECK(test.Look({ .hosting = true }, t0 + 100ms).empty());  // asked, no lobby yet: nothing to say

	auto lines = test.Look({ .hosting = true, .lobby = 109775241234567890ull, .members = 1 }, t0 + 850ms);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0] == "First test: lobby open (lobby 109775241234567890) 850 ms after it was asked for");

	CHECK(test.Look({ .hosting = true, .lobby = 109775241234567890ull, .members = 1 }, t0 + 2s).empty());
	lines = test.Look({ .hosting = true, .lobby = 109775241234567890ull, .members = 2 }, t0 + 10850ms);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0] == "First test: a player joined our lobby (2 members) 10000 ms after it opened");

	lines = test.Look({ .hosting = true, .lobby = 109775241234567890ull, .members = 1 }, t0 + 20s);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0] == "First test: a player left our lobby (1 members)");

	lines = test.Look({}, t0 + 21s);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0] == "First test: left lobby 109775241234567890");
}

TEST("first test: the friend's log says when it joined, timed from the ask, or that nobody asked through the channel")
{
	const FirstTest::Clock::time_point t0{};
	FirstTest                          test;
	test.AskedToJoin(t0);
	auto lines = test.Look({ .lobby = 109775241234567890ull, .members = 2 }, t0 + 1200ms);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0] == "First test: joined lobby 109775241234567890 (2 members) 1200 ms after it was asked for");

	FirstTest invited;  // a real invite: the channel asked nothing
	lines = invited.Look({ .lobby = 109775241234567891ull, .members = 2 }, t0 + 1s);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].starts_with("First test: joined lobby 109775241234567891 (2 members) (not asked for"));
}

TEST("first test: a lobby changed for another in one look says both, the old one first")
{
	const FirstTest::Clock::time_point t0{};
	FirstTest                          test;
	test.Look({ .lobby = 109775241234567890ull, .members = 2 }, t0);
	const auto lines = test.Look({ .lobby = 109775241234567891ull, .members = 3 }, t0 + 1s);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0] == "First test: left lobby 109775241234567890");
	CHECK(lines[1].starts_with("First test: joined lobby 109775241234567891 (3 members)"));
}
