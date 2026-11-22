#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "DevLine.h"

// The first test's messages over Steam's relay (src/steam/SteamProbe.cpp sends and receives them): one game sends a
// numbered ping, the other answers it with a pong of the same number, and the sender times the round trip with its own
// clock, so the two machines' clocks never have to agree. Each side pings the other, which is the "both ways" of the
// test. No game and no Steam here, so it is unit-tested.
//
// The messages are short text, so a capture or a debug log reads as what it is:
//   "skyrimcoop ping <n>"   "skyrimcoop pong <n>"
class RelayProbe
{
public:
	using Clock = std::chrono::steady_clock;

	static constexpr std::string_view PREFIX = "skyrimcoop ";

	enum class Kind
	{
		kNotOurs,      // something else on the channel: left alone
		kPing,         // the other side's ping: answer it with Reply
		kPong,         // the answer to one of ours: Rtt holds the round trip
		kStrayPong,    // a pong to no ping of ours (a repeat, or one from before a Reset)
	};

	struct Heard
	{
		Kind                                     kind = Kind::kNotOurs;
		std::uint32_t                            number = 0;
		std::string                              reply;  // kPing: the pong to send back
		std::optional<std::chrono::microseconds> rtt;    // kPong
	};

	// The ping to send now.
	std::string Ping(Clock::time_point a_now)
	{
		const auto number = ++lastSent;
		sentAt[number] = a_now;
		return std::format("{}ping {}", PREFIX, number);
	}

	// What a message that arrived is, and what to do with it.
	Heard Hear(std::string_view a_message, Clock::time_point a_now)
	{
		Heard heard;
		if (!a_message.starts_with(PREFIX)) {
			return heard;
		}
		const auto words = DevLine::Words(a_message.substr(PREFIX.size()));
		if (words.size() != 2) {
			return heard;
		}
		const auto number = DevLine::Number(words[1]);
		if (!number || *number == 0 || *number > UINT32_MAX) {
			return heard;
		}
		heard.number = static_cast<std::uint32_t>(*number);
		if (words[0] == "ping") {
			heard.kind = Kind::kPing;
			heard.reply = std::format("{}pong {}", PREFIX, heard.number);
			++pingsAnswered;
			return heard;
		}
		if (words[0] != "pong") {
			return heard;
		}
		const auto sent = sentAt.find(heard.number);
		if (sent == sentAt.end()) {
			heard.kind = Kind::kStrayPong;
			++strayPongs;
			return heard;
		}
		heard.kind = Kind::kPong;
		heard.rtt = std::chrono::duration_cast<std::chrono::microseconds>(a_now - sent->second);
		rtts.push_back(*heard.rtt);
		sentAt.erase(sent);
		return heard;
	}

	struct Summary
	{
		std::uint32_t sent = 0;
		std::uint32_t answered = 0;
		std::uint32_t waiting = 0;   // sent, not answered, younger than the timeout
		std::uint32_t lost = 0;      // sent, not answered, older than the timeout
		std::uint32_t pingsAnswered = 0;  // the other side's pings this side answered
		std::uint32_t strayPongs = 0;
		double        minMs = 0, medianMs = 0, maxMs = 0;  // round trips of the answered ones
	};

	[[nodiscard]] Summary Summarise(Clock::time_point a_now, Clock::duration a_timeout) const
	{
		Summary summary;
		summary.sent = lastSent - firstNumber;
		summary.answered = static_cast<std::uint32_t>(rtts.size());
		for (const auto& [number, at] : sentAt) {
			(a_now - at >= a_timeout ? summary.lost : summary.waiting) += 1;
		}
		summary.pingsAnswered = pingsAnswered;
		summary.strayPongs = strayPongs;
		if (!rtts.empty()) {
			auto sorted = rtts;
			std::ranges::sort(sorted);
			const auto ms = [](std::chrono::microseconds a_us) { return a_us.count() / 1000.0; };
			summary.minMs = ms(sorted.front());
			summary.maxMs = ms(sorted.back());
			// the lower middle of an even count: a value that was measured, not an average of two
			summary.medianMs = ms(sorted[(sorted.size() - 1) / 2]);
		}
		return summary;
	}

	// One line, for the log and the developer channel.
	[[nodiscard]] static std::string Describe(const Summary& a_summary)
	{
		return std::format("sent={} answered={} waiting={} lost={} pingsAnswered={} strayPongs={} rttMs min={:.1f} median={:.1f} max={:.1f}",
			a_summary.sent, a_summary.answered, a_summary.waiting, a_summary.lost, a_summary.pingsAnswered, a_summary.strayPongs, a_summary.minMs,
			a_summary.medianMs, a_summary.maxMs);
	}

	// Forgets every ping and answer. Numbers go on from where they were, so a late pong to a ping from before the
	// reset is a stray, never taken for the answer to a new ping.
	void Reset()
	{
		const auto last = lastSent;
		*this = RelayProbe{};
		firstNumber = last;
		lastSent = last;
	}

private:
	std::uint32_t                                  firstNumber = 0;  // the last number sent before the last Reset
	std::uint32_t                                  lastSent = 0;
	std::map<std::uint32_t, Clock::time_point>     sentAt;
	std::vector<std::chrono::microseconds>         rtts;
	std::uint32_t                                  pingsAnswered = 0;
	std::uint32_t                                  strayPongs = 0;
};
