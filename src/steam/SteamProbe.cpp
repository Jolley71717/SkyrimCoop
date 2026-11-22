#include "steam/SteamProbe.h"

#include "Redact.h"
#include "net/RelayProbe.h"
#include "steam/Steam.h"

namespace SteamProbe
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto        LOST_AFTER = 10s;
		constexpr int         BATCH = 16;
		constexpr std::size_t LONGEST = 64;  // our messages are about 20 bytes: anything longer is not one

		RelayProbe        probe;
		std::uint32_t     toSend = 0;
		SteamApi::SteamId target = 0;
		bool              firstPingTold = false;
		bool              firstPongTold = false;

		bool SendTo(const SteamApi::Api* a_api, SteamApi::SteamId a_to, const std::string& a_text)
		{
			const auto identity = SteamApi::IdentityOf(a_to);
			// Reliable: the test is whether a message gets across at all; Steam resends a lost one, and the round trip shows it.
			const int result = a_api->SendMessageToUser(a_api->messages, &identity, a_text.data(), static_cast<std::uint32_t>(a_text.size()),
				SteamApi::SEND_RELIABLE | SteamApi::SEND_AUTO_RESTART_BROKEN_SESSION, CHANNEL);
			if (result != 1) {  // k_EResultOK
				logs::warn("First test: relay: sending to {} failed (result {})", Redact::Alias(a_to), result);
				return false;
			}
			return true;
		}
	}

	void Frame()
	{
		const auto api = SteamApi::Loaded();
		if (!api || !Steam::Available()) {
			return;
		}
		const auto now = Clock::now();

		SteamApi::NetworkingMessage* messages[BATCH];
		const int                    count = api->ReceiveMessagesOnChannel(api->messages, CHANNEL, messages, BATCH);
		for (int i = 0; i < count; ++i) {
			const auto& message = *messages[i];
			const auto  from = SteamApi::SteamIdOf(message.peer);
			if (message.data && message.size > 0 && static_cast<std::size_t>(message.size) <= LONGEST) {
				const std::string_view text{ static_cast<const char*>(message.data), static_cast<std::size_t>(message.size) };
				const auto             heard = probe.Hear(text, now);
				switch (heard.kind) {
				case RelayProbe::Kind::kPing:
					if (!firstPingTold) {
						firstPingTold = true;
						logs::info("First test: relay: a ping from {} arrived (ping {}); answering", Redact::Alias(from), heard.number);
					} else {
						logs::debug("First test: relay: ping {} from {}", heard.number, Redact::Alias(from));
					}
					SendTo(api, from, heard.reply);
					break;
				case RelayProbe::Kind::kPong:
					if (!firstPongTold) {
						firstPongTold = true;
						logs::info("First test: relay: the answer from {} arrived (pong {}), round trip {:.1f} ms", Redact::Alias(from), heard.number,
							heard.rtt->count() / 1000.0);
					} else {
						logs::debug("First test: relay: pong {} from {}, round trip {:.1f} ms", heard.number, Redact::Alias(from), heard.rtt->count() / 1000.0);
					}
					break;
				case RelayProbe::Kind::kStrayPong:
					logs::debug("First test: relay: pong {} from {} answers no ping of ours", heard.number, Redact::Alias(from));
					break;
				case RelayProbe::Kind::kNotOurs:
					logs::debug("First test: relay: {} bytes from {} that are not a probe", message.size, Redact::Alias(from));
					break;
				}
			}
			api->ReleaseMessage(messages[i]);
		}

		if (toSend > 0 && target != 0) {
			--toSend;
			SendTo(api, target, probe.Ping(now));
			if (toSend == 0) {
				logs::info("First test: relay: every ping asked for is sent to {}", Redact::Alias(target));
			}
		}
	}

	std::string Send(std::uint32_t a_count)
	{
		if (!Steam::Available()) {
			return "error: Steam is not available";
		}
		const auto other = Steam::OtherPlayer();
		if (other == 0) {
			return "error: no other player in our lobby";
		}
		target = other;
		toSend = std::clamp<std::uint32_t>(a_count, 1, 1000);
		logs::info("First test: relay: sending {} pings to {}", toSend, Redact::Alias(other));
		return std::format("sending {}", toSend);
	}

	std::string Report(bool a_log)
	{
		auto line = RelayProbe::Describe(probe.Summarise(Clock::now(), LOST_AFTER));
		if (a_log) {
			logs::info("First test: relay report: {}", line);
		}
		return line;
	}

	void Reset()
	{
		probe.Reset();
		toSend = 0;
		firstPingTold = false;
		firstPongTold = false;
	}
}
