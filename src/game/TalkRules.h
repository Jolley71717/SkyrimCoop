#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/SubtitleTime.h"

// The rules of conversations (docs/DESIGN.md, "Conversations"), free of the game so tests/TalkTests.cpp can run them:
// which of the speaker's subtitles are new lines (LineWatch), the talk with an NPC as the session hears of it (Claimer),
// which heard lines are shown and how (Hearing, HowToShow), a line's end heard from the talker (CutOnEnd) and a
// recording's path (VoiceBase). src/game/Conversations.cpp reads and writes the game with them.
namespace TalkRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// How close a speaker must be to be heard: the game's own range for subtitles (fMaxSubtitleDistance, 1500 units)
	// less a little, so a line we show is one the game would show too. About 20 m.
	inline constexpr float HEAR_RANGE = 1400.0f;

	// ---- a recording's path ------------------------------------------------------------------------------------------

	// The longest path kept (Windows' 260 characters, as Protocol::MAX_VOICE_PATH).
	inline constexpr std::size_t MAX_VOICE_PATH = 260;

	namespace detail
	{
		inline char Lower(char a_c) { return a_c >= 'A' && a_c <= 'Z' ? static_cast<char>(a_c - 'A' + 'a') : a_c; }
	}

	// A recording's path as a game reads it, from what the engine holds for a response (DialogueResponse::voice) or what
	// a friend's game sent: under Data, in lower case, with back slashes, "sound\voice\<plugin>\<voice type>\<file>"
	// without its extension (the game keeps a line's recording as a .fuz, or a loose .xwm or .wav: VoiceFiles). A path
	// that is not one (anything outside Sound\Voice, a step up "..", a drive or a stream ":", an empty part, a control
	// character, an extension that is no recording's, or too long) is none: a friend's game can name no other file.
	[[nodiscard]] inline std::optional<std::string> VoiceBase(std::string_view a_raw)
	{
		if (a_raw.empty() || a_raw.size() > MAX_VOICE_PATH) {
			return std::nullopt;
		}
		std::string path;
		path.reserve(a_raw.size() + 6);
		for (const char c : a_raw) {
			if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || c == ':') {
				return std::nullopt;
			}
			path += c == '/' ? '\\' : detail::Lower(c);
		}
		if (path.starts_with("data\\")) {
			path.erase(0, 5);
		}
		if (path.starts_with("voice\\")) {
			path.insert(0, "sound\\");
		}
		constexpr std::string_view ROOT = "sound\\voice\\";
		if (!path.starts_with(ROOT) || path.find("..") != std::string::npos || path.find("\\\\") != std::string::npos || path.ends_with('\\')) {
			return std::nullopt;
		}
		// The plugin, the voice type and the file: at least three parts under the root (a mod may keep its files deeper).
		if (std::ranges::count(std::string_view{ path }.substr(ROOT.size()), '\\') < 2) {
			return std::nullopt;
		}
		const auto file = path.rfind('\\') + 1;
		if (path[file] == '.') {
			return std::nullopt;  // no name: the extension alone, or a hidden file
		}
		if (const auto dot = path.rfind('.'); dot != std::string::npos && dot > file) {
			const auto ext = std::string_view{ path }.substr(dot);
			if (ext != ".fuz" && ext != ".xwm" && ext != ".wav") {
				return std::nullopt;
			}
			path.erase(dot);
		}
		if (path.size() <= file || path.size() + 4 > MAX_VOICE_PATH) {
			return std::nullopt;
		}
		return path;
	}

	// The files a recording may be, in the order a game looks for them: the game's own .fuz, then a mod's loose .xwm or
	// .wav.
	[[nodiscard]] inline std::array<std::string, 3> VoiceFiles(const std::string& a_base)
	{
		return { a_base + ".fuz", a_base + ".xwm", a_base + ".wav" };
	}

	// ---- the talker's side: which subtitles are lines ------------------------------------------------------------------

	// The subtitle of the NPC our player talks with, read each tick (the engine's HighProcessData::voiceSubtitle, which it
	// keeps after the line ends): a line to send each time it changes to something to read. A line said again with the
	// same text (a greeting said twice) is the same subtitle: after the engine says a new line begins (Begin, from its
	// topic event), the same text read SAME_AGAIN later is that line again; read sooner, it is the old one still up.
	class LineWatch
	{
	public:
		static constexpr auto SAME_AGAIN = 300ms;

		// The engine began a line of this speaker.
		void Begin(Clock::time_point a_now)
		{
			armed = true;
			armedAt = a_now;
		}

		// The subtitle read now: true when it is a new line to send.
		bool Read(std::string_view a_text, Clock::time_point a_now)
		{
			if (SubtitleTime::Blank(a_text)) {
				return false;
			}
			if (a_text != last) {
				last = std::string{ a_text };
				armed = false;
				return true;
			}
			if (armed && a_now - armedAt >= SAME_AGAIN) {
				armed = false;
				return true;
			}
			return false;
		}

		// Another speaker, or none: the next subtitle read is a new line whatever it says.
		void Clear()
		{
			last.clear();
			armed = false;
		}

	private:
		std::string       last;
		bool              armed = false;
		Clock::time_point armedAt{};
	};

	// ---- the talker's side: the talk as the session hears of it ------------------------------------------------------

	// Our player's talk with an NPC (its reference; 0: none), told to the session (Protocol::Talk): "on" when it starts
	// and again every REFRESH while it lasts, "off" LINGER after it ends (the NPC's goodbye is still said, and its lines
	// still pass), at once when another starts. The server's answer names who talks with it: another player's is busy,
	// once a talk (Heard: kBusy), and our game ends its own talk; nothing of it is said again, and its lines are not sent.
	class Claimer
	{
	public:
		static constexpr auto REFRESH = 5s;
		static constexpr auto LINGER = 2s;

		struct Say
		{
			std::uint32_t ref = 0;
			bool          on = true;
			bool operator==(const Say&) const = default;
		};

		enum class Heard
		{
			kIgnored,  // not about our talk now, or busy said already
			kGranted,  // ours
			kBusy,     // another player talks with it: end ours (once a talk)
		};

		// Once a tick, with the NPC our player talks with now: what to tell the session.
		std::vector<Say> Frame(std::uint32_t a_partner, Clock::time_point a_now)
		{
			std::vector<Say> out;
			if (a_partner != current) {
				if (current != 0 && !busy) {  // a busy talk was never ours: nothing of it lingers, no off is said
					if (lingering != 0 && lingering != current) {
						out.push_back({ lingering, false });  // the one before that: no linger left for it
					}
					lingering = current;
					lingerUntil = a_now + LINGER;
				}
				current = a_partner;
				busy = false;
				granted = false;
				if (current != 0) {
					if (lingering == current) {
						lingering = 0;  // back to the same NPC before its "off": still ours
					}
					out.push_back({ current, true });
					saidAt = a_now;
				}
			} else if (current != 0 && !busy && a_now - saidAt >= REFRESH) {
				out.push_back({ current, true });
				saidAt = a_now;
			}
			if (lingering != 0 && (a_now >= lingerUntil || lingering == current)) {
				if (lingering != current) {
					out.push_back({ lingering, false });
				}
				lingering = 0;
			}
			return out;
		}

		// The session's answer: who talks with a_ref (a_holder), we being a_us.
		Heard Answer(std::uint32_t a_ref, std::uint32_t a_holder, std::uint32_t a_us)
		{
			if (a_ref == 0 || a_ref != current || busy) {
				return Heard::kIgnored;
			}
			if (a_holder == a_us) {
				granted = true;
				return Heard::kGranted;
			}
			busy = true;
			holder = a_holder;
			return Heard::kBusy;
		}

		// Whether a line of a_speaker is ours to send: the NPC we talk with (not refused as busy) or the one whose goodbye
		// is still said.
		[[nodiscard]] bool Shares(std::uint32_t a_speaker) const
		{
			return a_speaker != 0 && ((a_speaker == current && !busy) || a_speaker == lingering);
		}

		// Whether our own lines (the topics our player picks) are sent: while we talk with an NPC that is not busy.
		[[nodiscard]] bool Talking() const { return current != 0 && !busy; }

		[[nodiscard]] std::uint32_t Current() const { return current; }
		[[nodiscard]] std::uint32_t Lingering() const { return lingering; }
		[[nodiscard]] bool          Busy() const { return busy; }
		[[nodiscard]] bool          Granted() const { return granted; }
		[[nodiscard]] std::uint32_t Holder() const { return busy ? holder : 0; }

		// The session ended (or a load took the world): nothing is ours, nothing is said.
		void Clear()
		{
			current = lingering = holder = 0;
			busy = granted = false;
		}

	private:
		std::uint32_t     current = 0;
		std::uint32_t     lingering = 0;
		std::uint32_t     holder = 0;
		bool              busy = false;
		bool              granted = false;
		Clock::time_point saidAt{};
		Clock::time_point lingerUntil{};
	};

	// ---- the hearer's side ---------------------------------------------------------------------------------------------

	// Which heard lines are shown: the same speaker saying the same thing again within REPEAT_WINDOW is skipped, and at
	// most MAX_SHOWN lines in SHOW_WINDOW (a talker skipping through an NPC's lines says them faster than anyone reads).
	// The key names the talker and the speaker (KeyOf).
	class Hearing
	{
	public:
		static constexpr auto        REPEAT_WINDOW = 5s;
		static constexpr auto        SHOW_WINDOW = 3s;
		static constexpr std::size_t MAX_SHOWN = 3;

		enum class Verdict
		{
			kShow,
			kRepeat,   // said just now by the same speaker
			kTooMany,  // MAX_SHOWN in SHOW_WINDOW already
		};

		Verdict Judge(std::uint64_t a_key, std::string_view a_text, Clock::time_point a_now)
		{
			while (!shown.empty() && a_now - shown.front().at >= REPEAT_WINDOW) {
				shown.pop_front();
			}
			if (std::ranges::any_of(shown, [&](const Shown& a_s) { return a_s.key == a_key && a_s.text == a_text; })) {
				return Verdict::kRepeat;
			}
			const auto recent = std::ranges::count_if(shown, [&](const Shown& a_s) { return a_now - a_s.at < SHOW_WINDOW; });
			if (static_cast<std::size_t>(recent) >= MAX_SHOWN) {
				return Verdict::kTooMany;
			}
			shown.push_back({ a_key, std::string{ a_text }, a_now });
			return Verdict::kShow;
		}

		void Clear() { shown.clear(); }

	private:
		struct Shown
		{
			std::uint64_t     key = 0;
			std::string       text;
			Clock::time_point at{};
		};
		std::deque<Shown> shown;
	};

	// A line's key: the talker and the speaker (0 for the talker themselves).
	[[nodiscard]] constexpr std::uint64_t KeyOf(std::uint32_t a_player, std::uint32_t a_speaker)
	{
		return (std::uint64_t{ a_player } << 32) | a_speaker;
	}

	// Where a heard line's speaker is from our player: the speaker itself in our world (the NPC, or the talker's
	// stand-in for their own line), and the talker's stand-in. A distance is of the same space (cell or worldspace) only.
	struct Where
	{
		std::optional<float> speaker;  // none: not in our world, not loaded, or in another space
		std::optional<float> talker;
	};

	enum class Show
	{
		kSubtitle,  // in the game's subtitle area, over the speaker, as the game shows a nearby NPC's line
		kNotice,    // as a notice in the corner, "Name: line"
		kFar,       // nowhere: neither the speaker nor the talker is near our player
	};

	// How a heard line is shown. Near (HEAR_RANGE) is the speaker's distance when it is in our world, else the talker's
	// (a line of an NPC our game lacks is told by its name). The subtitle area shows it only over a speaker in our world,
	// and only as the game's own settings allow a nearby NPC's line: General Subtitles on, and our player in no
	// conversation of their own (the game then shows that conversation's lines alone); a notice otherwise.
	[[nodiscard]] inline Show HowToShow(const Where& a_where, bool a_generalSubtitles, bool a_inConversation)
	{
		const auto near = [](const std::optional<float>& a_d) { return a_d && std::isfinite(*a_d) && *a_d <= HEAR_RANGE; };
		if (a_where.speaker ? !near(a_where.speaker) : !near(a_where.talker)) {
			return Show::kFar;
		}
		if (!a_where.speaker || !a_generalSubtitles || a_inConversation) {
			return Show::kNotice;
		}
		return Show::kSubtitle;
	}

	// A line's end heard from its talker (Protocol::Line::kEnd): its sound here is ended only when it has more than
	// END_KEEP still to play (the talker skipped it, or left the conversation), or its length is not known; one about
	// to end on its own plays out, since the end comes as late as the line did.
	inline constexpr auto END_KEEP = 750ms;
	[[nodiscard]] inline bool CutOnEnd(Clock::time_point a_started, std::optional<double> a_seconds, Clock::time_point a_now)
	{
		if (!a_seconds || !std::isfinite(*a_seconds) || *a_seconds <= 0.0) {
			return true;
		}
		const auto length = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::min(*a_seconds, 600.0)));
		return a_now + END_KEEP < a_started + length;
	}
}
