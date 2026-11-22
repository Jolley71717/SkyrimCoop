#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The developer channel's text protocol (DevChannel.cpp), without the sockets: lines in, a command and its
// arguments out, the token compared. No game and no Windows here, so it is unit-tested.
//
// Carried from F4Multiplayer's src/DevChannel.cpp (commit 1d29178b1), where these pieces sit inside the socket loop.
namespace DevLine
{
	inline constexpr std::size_t MAX_LINE = 4096;

	// Bytes as they arrive (a recv's worth at a time) in, whole lines out: without the "\n", a "\r" before it
	// dropped, empty lines skipped. A line longer than MAX_LINE with no end in sight makes the buffer give up
	// (Overflowed), and the channel then closes that client.
	class LineBuffer
	{
	public:
		void Append(std::string_view a_bytes) { buffer.append(a_bytes); }

		// The next whole line, if one is there.
		std::optional<std::string> Next()
		{
			for (;;) {
				const auto newline = buffer.find('\n');
				if (newline == std::string::npos) {
					return std::nullopt;
				}
				auto line = buffer.substr(0, newline);
				buffer.erase(0, newline + 1);
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (!line.empty()) {
					return line;
				}
			}
		}

		[[nodiscard]] bool Overflowed() const { return buffer.size() > MAX_LINE && buffer.find('\n') == std::string::npos; }

	private:
		std::string buffer;
	};

	struct Command
	{
		std::string_view name;
		std::string_view args;  // everything after the first space ("" with none)
	};

	[[nodiscard]] inline Command Split(std::string_view a_line)
	{
		const auto space = a_line.find(' ');
		if (space == std::string_view::npos) {
			return { a_line, {} };
		}
		return { a_line.substr(0, space), a_line.substr(space + 1) };
	}

	// The arguments, split on spaces (runs of spaces count as one).
	[[nodiscard]] inline std::vector<std::string_view> Words(std::string_view a_args)
	{
		std::vector<std::string_view> words;
		std::size_t                   at = 0;
		while (at < a_args.size()) {
			const auto start = a_args.find_first_not_of(' ', at);
			if (start == std::string_view::npos) {
				break;
			}
			const auto end = a_args.find(' ', start);
			words.push_back(a_args.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
			at = end == std::string_view::npos ? a_args.size() : end;
		}
		return words;
	}

	// "auth <token>" with the right token. The comparison takes as long whatever the candidate holds, so the time
	// a reply takes says nothing about how much of a guess was right.
	[[nodiscard]] inline bool Authorises(std::string_view a_line, std::string_view a_token)
	{
		constexpr std::string_view PREFIX = "auth ";
		if (a_token.empty() || !a_line.starts_with(PREFIX)) {
			return false;
		}
		const auto candidate = a_line.substr(PREFIX.size());
		if (candidate.size() != a_token.size()) {
			return false;
		}
		unsigned char diff = 0;
		for (std::size_t i = 0; i < a_token.size(); ++i) {
			diff |= static_cast<unsigned char>(a_token[i] ^ candidate[i]);
		}
		return diff == 0;
	}

	// A whole decimal number (a lobby id, a count), or nothing.
	[[nodiscard]] inline std::optional<std::uint64_t> Number(std::string_view a_text)
	{
		std::uint64_t value = 0;
		const auto [end, error] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
		if (a_text.empty() || error != std::errc{} || end != a_text.data() + a_text.size()) {
			return std::nullopt;
		}
		return value;
	}
}
