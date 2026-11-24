#include "game/Conversations.h"

#include "DevLine.h"
#include "FormIds.h"
#include "Text.h"
#include "game/GameThread.h"
#include "game/LineSounds.h"
#include "game/LocalPlayer.h"
#include "game/RemotePlayers.h"
#include "game/SubtitleTime.h"
#include "game/TalkRules.h"

// windows.h names GetObject a macro (GetObjectA); BGSDefaultObjectManager::GetObject is the engine's own.
#ifdef GetObject
#	undef GetObject
#endif

namespace Conversations
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Sounds = LineSounds<RE::BSSoundHandle>;
		using Protocol::Line;

		constexpr std::uint32_t VOICE_FLAGS = 0x10;     // a 3D sound, heard from where it is (as F4Multiplayer's lines)
		constexpr std::uint32_t VOICE_PRIORITY = 128;
		constexpr std::uint16_t VOICE_CUT_MS = 100;     // a speaker's next line fades the one before out this fast
		constexpr std::size_t   MAX_VOICE_READ = 4u << 20;
		constexpr auto          CUT_LOG_EVERY = 30s;
		constexpr auto          CHECK_LISTED = 200ms;   // a subtitle we added is looked for again this much later
		constexpr std::uint64_t MAX_SUBTITLE_DROPS = 3;  // the engine dropped this many of ours: notices for the session
		constexpr auto          PICK_AGAIN = 1s;         // the same topic picked again, at the earliest
		constexpr std::size_t   MAX_QUEUED = 256;
		constexpr float         PICK_RANGE = 1500.0f;    // `talk pick`: NPCs this near our player

		// ---- the topic events (the sink only queues; the tick reads them) -----------------------------------------------

		struct TopicEvent
		{
			std::uint32_t speaker = 0;
			std::uint32_t info = 0;
			bool          begin = true;
		};

		std::mutex              queueLock;
		std::vector<TopicEvent> queue;
		std::uint64_t           eventsDropped = 0;  // under queueLock
		std::atomic<std::uint64_t> eventsSeen{ 0 };

		void Queue(const TopicEvent& a_event)
		{
			std::lock_guard lock{ queueLock };
			if (queue.size() >= MAX_QUEUED) {
				++eventsDropped;
				return;
			}
			queue.push_back(a_event);
		}

		std::vector<TopicEvent> TakeQueue()
		{
			std::lock_guard lock{ queueLock };
			return std::exchange(queue, {});
		}

		class Sink : public RE::BSTEventSink<RE::TESTopicInfoEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESTopicInfoEvent* a_event, RE::BSTEventSource<RE::TESTopicInfoEvent>*) override
			{
				if (a_event && a_event->speakerRef) {
					++eventsSeen;
					Queue({ a_event->speakerRef->GetFormID(), a_event->topicInfoFormID, a_event->type.get() == RE::TESTopicInfoEvent::TopicInfoEventType::kTopicBegin });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sink sink;
		bool installed = false;

		// ---- what is counted (the dev line `talk`) -----------------------------------------------------------------------

		struct Counts
		{
			// our talk
			std::uint64_t said = 0;         // NPC lines sent
			std::uint64_t playerLines = 0;  // our player's topics sent
			std::uint64_t ends = 0;
			std::uint64_t talks = 0;        // talk words said to the session
			std::uint64_t busyRefused = 0;  // a greeting of an NPC another player talks with: left to run, kept home
			std::uint64_t busyClosed = 0;   // our talk with an NPC another player talks with, ended
			// friends' lines
			std::uint64_t heard = 0;
			std::uint64_t subtitled = 0;  // shown in the subtitle area (still listed CHECK_LISTED after)
			std::uint64_t noticed = 0;
			std::uint64_t dropped = 0;    // added to the subtitle area and gone at once: shown as a notice instead
			std::uint64_t voiced = 0;
			std::uint64_t missing = 0;    // no recording of that path here
			std::uint64_t unplayed = 0;   // a recording here the audio manager would not play
			std::uint64_t silent = 0;     // this game's sound is off
			std::uint64_t farAway = 0;
			std::uint64_t noSpeaker = 0;  // an NPC named that this game has, not loaded here
			std::uint64_t repeats = 0;
			std::uint64_t tooMany = 0;
			std::uint64_t cutLong = 0;
			std::uint64_t cutCrowded = 0;
			std::uint64_t cutEnded = 0;
			std::uint64_t offMain = 0;
		};
		Counts counts;

		// ---- our talk ------------------------------------------------------------------------------------------------------

		struct OpenLine
		{
			std::uint32_t seq = 0;  // 0: none
			std::uint32_t speaker = 0;
		};

		std::uint32_t        ourId = 0;
		TalkRules::Claimer   claimer;
		TalkRules::LineWatch watch;
		std::uint32_t        watched = 0;  // whose subtitle the watch reads
		std::uint32_t        seq = 0;      // our lines' numbers, rising for the plugin's life
		OpenLine             openLine;     // our NPC's last line, until its end is said
		bool                 wasOpen = false;
		bool                 startedByPlayer = false;
		const void*          pickedDialogue = nullptr;  // the topic last picked (or there when the menu opened)
		std::string          pickedText;
		Clock::time_point    pickedAt{};
		bool                 pickAgain = false;
		std::uint32_t        lastSaid = 0;  // the text's hash, for the rig

		// ---- friends' lines ----------------------------------------------------------------------------------------------

		struct Up
		{
			RE::ObjectRefHandle speaker;
			std::string         text;
			std::string         notice;  // shown instead when the engine drops it
			std::uint64_t       key = 0;
			Clock::time_point   checkAt{};
			Clock::time_point   until{};
			bool                checked = false;
		};

		struct LastLine
		{
			std::uint32_t seq = 0;
			std::uint64_t key = 0;
		};

		Sounds                                      sounds;
		TalkRules::Hearing                          hearing;
		std::vector<Up>                             up;  // our lines in the engine's subtitle list
		std::unordered_map<std::uint32_t, LastLine> lastOf;  // each friend's last line, for its end
		std::uint64_t                               subtitleDrops = 0;
		bool                                        subtitlesOff = false;  // the engine drops ours: notices only, this session
		std::optional<Clock::time_point>            cutLoggedAt;
		std::uint64_t                               cutUnlogged = 0;
		std::uint32_t                               lastHeard = 0;  // the text's hash, for the rig
		std::uint32_t                               lastFrom = 0;

		std::uint32_t Hash(std::string_view a_text)
		{
			std::uint32_t hash = 2166136261u;
			for (const unsigned char c : a_text) {
				hash ^= c;
				hash *= 16777619u;
			}
			return hash;
		}

		// The game's writes are made from the tick, which until the frame hook runs on SKSE's task queue: counted when
		// off the game's main thread, said once.
		void NoteThread()
		{
			if (GameThread::IsLoadThread()) {
				return;
			}
			if (counts.offMain++ == 0) {
				logs::info("Talk: a subtitle, sound or menu written off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); "
						   "counted in `talk` as offmain");
			}
		}

		std::string NameOfRef(RE::TESObjectREFR* a_ref)
		{
			const auto name = a_ref ? a_ref->GetDisplayFullName() : nullptr;
			return name ? std::string{ name } : std::string{};
		}

		void Notice(const std::string& a_text)
		{
			NoteThread();
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false);
		}

		RE::Setting* FindSetting(const char* a_name)
		{
			const auto prefs = RE::INIPrefSettingCollection::GetSingleton();
			if (const auto setting = prefs ? prefs->GetSetting(a_name) : nullptr) {
				return setting;
			}
			const auto ini = RE::INISettingCollection::GetSingleton();
			return ini ? ini->GetSetting(a_name) : nullptr;
		}

		// The game's own: lines said near the player, not to them, shown (Interface, General Subtitles).
		bool GeneralSubtitles()
		{
			static RE::Setting* const setting = FindSetting("bGeneralSubtitles:Interface");
			return setting && setting->GetBool();
		}

		bool DialogueSubtitles()
		{
			static RE::Setting* const setting = FindSetting("bDialogueSubtitles:Interface");
			return setting && setting->GetBool();
		}

		// Whether this game plays sound at all (the rig's VM runs with it off: tools/rig/start.ps1).
		bool AudioOn()
		{
			static RE::Setting* const setting = [] {
				const auto ini = RE::INISettingCollection::GetSingleton();
				return ini ? ini->GetSetting("bEnableAudio:Audio") : nullptr;
			}();
			return !setting || setting->GetBool();
		}

		RE::MenuTopicManager* Topics()
		{
			return RE::MenuTopicManager::GetSingleton();
		}

		bool DialogueOpen()
		{
			const auto ui = RE::UI::GetSingleton();
			const auto topics = Topics();
			return ui && topics && topics->menuOpen && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		RE::TESObjectREFR* Partner()
		{
			const auto topics = Topics();
			if (!topics || !DialogueOpen()) {
				return nullptr;
			}
			return topics->speaker.get().get();
		}

		// The distance from our player to a_ref, in the same place only (one cell inside, one worldspace outside), loaded
		// and enabled: none otherwise.
		std::optional<float> DistanceTo(RE::TESObjectREFR* a_ref)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!a_ref || !player || a_ref->IsDisabled() || !a_ref->Is3DLoaded()) {
				return std::nullopt;
			}
			const auto theirs = a_ref->GetParentCell();
			const auto ours = player->GetParentCell();
			if (!theirs || !ours) {
				return std::nullopt;
			}
			if (theirs->IsInteriorCell() || ours->IsInteriorCell()) {
				if (theirs != ours) {
					return std::nullopt;
				}
			} else if (a_ref->GetWorldspace() != player->GetWorldspace()) {
				return std::nullopt;
			}
			return player->GetPosition().GetDistance(a_ref->GetPosition());
		}

		// ---- our talk: what is said --------------------------------------------------------------------------------------

		std::string SubtitleOf(RE::Actor* a_actor)
		{
			const auto high = a_actor ? a_actor->GetHighProcess() : nullptr;
			const auto text = high ? high->voiceSubtitle.c_str() : nullptr;
			return text ? std::string{ text } : std::string{};
		}

		// The response the engine is saying with that text: its info, its number (from 1) and its recording's path.
		struct Response
		{
			std::uint32_t info = 0;
			std::uint8_t  number = 0;
			std::string   voice;
		};

		std::optional<Response> InList(RE::BSSimpleList<RE::DialogueResponse*>& a_list, const RE::TESTopicInfo* a_info, std::string_view a_text)
		{
			std::uint32_t number = 0;
			for (const auto response : a_list) {
				++number;
				const auto text = response ? response->text.c_str() : nullptr;
				if (text && a_text == text) {
					Response found;
					found.info = a_info ? a_info->GetFormID() : 0;
					found.number = found.info != 0 && number <= 255 ? static_cast<std::uint8_t>(number) : 0;
					const auto voice = response->voice.c_str();
					found.voice = voice ? voice : "";
					return found;
				}
			}
			return std::nullopt;
		}

		// Where a line our NPC says comes from: what it greets with (its process's greeting), or the topic our player
		// picked. None: a line the engine said some other way (its recording is not known, the text still goes).
		std::optional<Response> FindResponse(RE::Actor* a_actor, std::string_view a_text)
		{
			if (const auto high = a_actor->GetHighProcess(); high && high->greetTopic) {
				auto& greeting = *high->greetTopic;  // not const: CommonLib's list has no const iteration
				if (auto found = InList(greeting.responses, greeting.info, a_text)) {
					return found;
				}
			}
			const auto topics = Topics();
			if (const auto dialogue = topics ? topics->lastSelectedDialogue : nullptr) {
				if (auto found = InList(dialogue->responses, dialogue->parentTopicInfo, a_text)) {
					return found;
				}
			}
			return std::nullopt;
		}

		void SayEnd(const SendFn& a_send)
		{
			if (openLine.seq == 0) {
				return;
			}
			Line end;
			end.kind = Line::kEnd;
			end.speakerKind = Line::kRef;
			end.speaker = openLine.speaker;
			end.seq = openLine.seq;
			a_send(Protocol::Encode(end));
			++counts.ends;
			openLine = {};
		}

		void SayNpcLine(RE::Actor* a_actor, const std::string& a_text, const SendFn& a_send)
		{
			Line line;
			line.kind = Line::kSay;
			line.speakerKind = Line::kRef;
			line.speaker = a_actor->GetFormID();
			line.seq = ++seq;
			line.name = NameOfRef(a_actor);
			line.text = a_text;
			if (const auto found = FindResponse(a_actor, a_text)) {
				line.info = found->info;
				line.response = found->number;
				line.voice = TalkRules::VoiceBase(found->voice).value_or(std::string{});
			}
			a_send(Protocol::Encode(line));
			++counts.said;
			lastSaid = Hash(a_text);
			openLine = { line.seq, line.speaker };
		}

		void SayPlayerLine(const std::string& a_text, const SendFn& a_send)
		{
			Line line;
			line.kind = Line::kSay;
			line.speakerKind = Line::kTalker;
			line.seq = ++seq;
			line.text = a_text;
			a_send(Protocol::Encode(line));
			++counts.playerLines;
		}

		void FrameTalk(const SendFn& a_send, Clock::time_point a_now)
		{
			const auto topics = Topics();
			const bool open = DialogueOpen();
			const auto partner = open ? Partner() : nullptr;
			const auto partnerId = partner ? partner->GetFormID() : 0;
			const auto dialogue = topics && open ? topics->lastSelectedDialogue : nullptr;
			if (open && !wasOpen) {
				// A talk begins: our player's (a greeting of the NPC's is not; Answered leaves that one running), and the
				// topic picked before it is no line of this talk.
				startedByPlayer = !topics->isGreetingPlayer;
				pickedDialogue = dialogue;
				pickedText = dialogue && dialogue->topicText.c_str() ? dialogue->topicText.c_str() : "";
				pickAgain = false;
			}
			wasOpen = open;

			for (const auto& say : claimer.Frame(partnerId, a_now)) {
				a_send(Protocol::Encode(Protocol::Talk{ say.ref, say.on }));
				++counts.talks;
			}

			for (const auto& event : TakeQueue()) {
				if (event.begin) {
					if (event.speaker != 0 && event.speaker == watched) {
						watch.Begin(a_now);
					}
					// The NPC begins its answer to the topic picked: picked again, when that topic's text is unchanged.
					const auto info = dialogue ? dialogue->parentTopicInfo : nullptr;
					if (info && info->GetFormID() == event.info && event.speaker == partnerId && a_now - pickedAt >= PICK_AGAIN) {
						pickAgain = true;
					}
				} else if (openLine.seq != 0 && event.speaker == openLine.speaker) {
					SayEnd(a_send);
				}
			}

			// Our player's own line first: it is said before the NPC answers it.
			if (open && dialogue && claimer.Talking()) {
				std::string text = dialogue->topicText.c_str() ? dialogue->topicText.c_str() : "";
				if ((dialogue != pickedDialogue || text != pickedText || pickAgain) && !SubtitleTime::Blank(text)) {
					SayPlayerLine(text, a_send);
					pickedAt = a_now;
				}
				pickedDialogue = dialogue;
				pickedText = std::move(text);
				pickAgain = false;
			}

			// The NPC's lines: the one we talk with, or the one whose goodbye is still said.
			const auto who = claimer.Current() != 0 ? claimer.Current() : claimer.Lingering();
			if (who != watched) {
				if (openLine.seq != 0 && openLine.speaker == watched) {
					SayEnd(a_send);  // the conversation was left: a line still said ends here
				}
				watch.Clear();
				watched = who;
			}
			if (who == 0 || !claimer.Shares(who)) {
				return;
			}
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(who);
			const auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor) {
				return;  // a talking activator, a thing: no process, no subtitle to read (docs/DESIGN.md, "Not yet")
			}
			const auto text = SubtitleOf(actor);
			if (watch.Read(text, a_now)) {
				SayNpcLine(actor, text, a_send);
			}
		}

		// ---- friends' lines: the sound -----------------------------------------------------------------------------------

		// The length of a recording, from its start (the lip sync, and the audio's header and packet table:
		// SubtitleTime::HeaderBytes), read through the game's own file system (loose or in an archive).
		std::optional<double> SecondsOf(RE::BSResourceNiBinaryStream& a_stream)
		{
			std::vector<std::uint8_t> bytes;
			bool                      ended = false;
			const auto                readTo = [&](std::size_t a_want) {
				a_want = (std::min)(a_want, MAX_VOICE_READ);
				while (!ended && bytes.size() < a_want) {
					const auto chunk = static_cast<std::uint32_t>((std::min<std::size_t>)(4096, a_want - bytes.size()));
					const auto at = bytes.size();
					const auto from = a_stream.tell();
					bytes.resize(at + chunk);
					if (!a_stream.read(bytes.data() + at, chunk)) {
						const auto to = a_stream.tell();
						bytes.resize(at + (to > from ? (std::min<std::size_t>)(chunk, to - from) : 0));
						ended = true;  // the file's end
					}
				}
			};
			readTo(SubtitleTime::HEADER_PROBE);
			readTo(SubtitleTime::HeaderBytes(bytes));
			return SubtitleTime::VoiceSeconds(bytes);
		}

		// Ends a friend's line's sound: faded out and let go, whether the audio manager still says it plays or not. One
		// still sounding at its hard end, or crowded out, is logged (now and then): it would have run on by itself.
		void EndSound(Sounds::Entry& a_entry, Sounds::Why a_why)
		{
			const bool sounding = a_entry.handle.IsPlaying();
			a_entry.handle.FadeOutAndRelease(VOICE_CUT_MS);
			if (!sounding || a_why == Sounds::Why::kNext) {
				return;
			}
			if (a_why == Sounds::Why::kEnded) {
				++counts.cutEnded;
				return;
			}
			const bool tooLong = a_why == Sounds::Why::kTooLong;
			++(tooLong ? counts.cutLong : counts.cutCrowded);
			const auto now = Clock::now();
			if (cutLoggedAt && now - *cutLoggedAt < CUT_LOG_EVERY) {
				++cutUnlogged;
				return;
			}
			const auto after = std::chrono::duration<double>(now - a_entry.started).count();
			const auto length = a_entry.seconds ? std::format("{:.1f} s", *a_entry.seconds) : std::string{ "unknown" };
			logs::info("Talk: the sound of player {}'s line (speaker {:08X}) still played {:.1f} s after it started (its recording: {}) {}: ended ({} at the end, {} crowded out so far, {} not logged)",
				static_cast<std::uint32_t>(a_entry.key >> 32), static_cast<std::uint32_t>(a_entry.key), after, length,
				tooLong ? std::string{ "at its end" } : std::format("with {} newer ones", Sounds::MAX_PLAYING), counts.cutLong, counts.cutCrowded, cutUnlogged);
			cutLoggedAt = now;
			cutUnlogged = 0;
		}

		// Plays a recording at a_speaker and follows it while it lasts: a sound like any other, nothing of the dialogue
		// system (no conversation, scene or lip sync), with a hard end (LineSounds).
		bool Play(RE::TESObjectREFR* a_speaker, const std::string& a_file, std::uint64_t a_key, std::optional<double> a_seconds)
		{
			const auto audio = RE::BSAudioManager::GetSingleton();
			if (!audio) {
				return false;
			}
			NoteThread();
			RE::BSSoundHandle handle;
			for (const auto& path : { a_file, "data\\" + a_file }) {
				RE::BSResource::ID id;
				id.GenerateFromPath(path.c_str());
				handle = RE::BSSoundHandle{};
				audio->GetSoundHandleByFile(handle, id, VOICE_FLAGS, VOICE_PRIORITY);
				if (handle.IsValid()) {
					break;
				}
			}
			if (!handle.IsValid()) {
				return false;
			}
			if (const auto defaults = RE::BGSDefaultObjectManager::GetSingleton()) {
				if (const auto model = defaults->GetObject<RE::BGSSoundOutput>(RE::DEFAULT_OBJECTS::kDialogueOutputModel3D)) {
					handle.SetOutputModel(model);  // how far an NPC's own lines carry, and how they fade
				}
			}
			handle.SetPosition(a_speaker->GetPosition());
			if (const auto root = a_speaker->Get3D()) {
				handle.SetObjectToFollow(root);
			}
			if (!handle.Play()) {
				handle.Stop();  // made and never played: let go
				return false;
			}
			sounds.Start(a_key, handle, Clock::now(), a_seconds, EndSound);
			return true;
		}

		// The recording of a friend's line, played at its speaker when this game has it: its length (for how long the
		// subtitle stays up), or none.
		std::optional<double> Speak(RE::TESObjectREFR* a_speaker, const Line& a_line, std::uint64_t a_key)
		{
			const auto base = TalkRules::VoiceBase(a_line.voice);
			if (!base) {
				++counts.missing;
				return std::nullopt;
			}
			for (const auto& file : TalkRules::VoiceFiles(*base)) {
				RE::BSResourceNiBinaryStream stream{ file };
				if (!stream.good()) {
					continue;
				}
				const auto seconds = SecondsOf(stream);
				if (!AudioOn()) {
					++counts.silent;
				} else if (Play(a_speaker, file, a_key, seconds)) {
					++counts.voiced;
				} else {
					++counts.unplayed;
					logs::debug("Talk: the audio manager made no sound of a recording player {} named ({})", a_line.playerId, file);
				}
				return seconds;
			}
			++counts.missing;
			logs::debug("Talk: no recording here of the one player {}'s line named", a_line.playerId);
			return std::nullopt;
		}

		// ---- friends' lines: the subtitle area ---------------------------------------------------------------------------

		bool Listed(const Up& a_up)
		{
			const auto manager = RE::SubtitleManager::GetSingleton();
			if (!manager) {
				return false;
			}
			RE::BSSpinLockGuard guard{ manager->lock };
			return std::ranges::any_of(manager->subtitles, [&](const RE::SubtitleInfo& a_entry) {
				const auto text = a_entry.subtitle.c_str();
				return a_entry.speaker == a_up.speaker && text && a_up.text == text;
			});
		}

		// Takes our line out of the engine's list (its speaker's own lines are the engine's to take).
		void TakeDown(const Up& a_up)
		{
			const auto manager = RE::SubtitleManager::GetSingleton();
			if (!manager) {
				return;
			}
			NoteThread();
			RE::BSSpinLockGuard guard{ manager->lock };
			auto&               list = manager->subtitles;
			for (auto it = list.begin(); it != list.end(); ++it) {
				const auto text = it->subtitle.c_str();
				if (it->speaker == a_up.speaker && text && a_up.text == text) {
					list.erase(it);
					return;
				}
			}
		}

		template <class Pred>
		void TakeDownWhere(Pred&& a_pred)
		{
			for (auto it = up.begin(); it != up.end();) {
				if (a_pred(*it)) {
					TakeDown(*it);
					it = up.erase(it);
				} else {
					++it;
				}
			}
		}

		// A friend's line in the game's subtitle area over a_speaker, as the engine lists a nearby NPC's line, for a_for.
		// A speaker says one line at a time: its line before goes. False when there is no list to add to.
		bool ShowSubtitle(RE::TESObjectREFR* a_speaker, const Line& a_line, std::uint64_t a_key, std::string a_notice, std::chrono::milliseconds a_for, float a_distance,
			Clock::time_point a_now)
		{
			const auto manager = RE::SubtitleManager::GetSingleton();
			if (!manager || !a_speaker) {
				return false;
			}
			const auto handle = a_speaker->GetHandle();
			if (!handle) {
				return false;
			}
			TakeDownWhere([&](const Up& a_up) { return a_up.key == a_key || a_up.speaker == handle; });
			NoteThread();
			{
				RE::BSSpinLockGuard guard{ manager->lock };
				RE::SubtitleInfo    entry{};
				entry.speaker = handle;
				entry.subtitle = a_line.text.c_str();
				entry.targetDistance = a_distance;
				entry.forceDisplay = false;
				manager->subtitles.push_back(std::move(entry));
			}
			up.push_back({ handle, a_line.text, std::move(a_notice), a_key, a_now + CHECK_LISTED, a_now + a_for, false });
			return true;
		}

		void FrameHear(Clock::time_point a_now)
		{
			sounds.EndDue(a_now, EndSound);
			for (auto it = up.begin(); it != up.end();) {
				if (!it->checked && a_now >= it->checkAt) {
					it->checked = true;
					if (!Listed(*it)) {
						// The engine took it out (or never shows a line it did not add): the notice instead.
						++counts.dropped;
						if (++subtitleDrops >= MAX_SUBTITLE_DROPS && !subtitlesOff) {
							subtitlesOff = true;
							logs::info("Talk: the game took {} of friends' lines out of its subtitle area at once: notices only for the rest of the session", subtitleDrops);
						}
						Notice(it->notice);
						++counts.noticed;
						it = up.erase(it);
						continue;
					}
					++counts.subtitled;
				}
				if (a_now >= it->until) {
					TakeDown(*it);
					it = up.erase(it);
					continue;
				}
				++it;
			}
		}

		void EndEverything()
		{
			sounds.EndAll([](Sounds::Entry& a_entry) { a_entry.handle.FadeOutAndRelease(VOICE_CUT_MS); });
			TakeDownWhere([](const Up&) { return true; });
			lastOf.clear();
		}

		// ---- the dev line ------------------------------------------------------------------------------------------------

		// `talk pick [n]`: the n-th NPC near our player who can talk with it (alive, enabled, loaded, not hostile, of a race
		// that talks with the player, not a stand-in or a runtime one), lowest form ID first.
		std::string Pick(std::uint64_t a_skip)
		{
			const auto lists = RE::ProcessLists::GetSingleton();
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!lists || !player || !LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			std::vector<std::pair<RE::Actor*, float>> found;
			for (auto& handle : lists->highActorHandles) {
				const auto pointer = handle.get();
				const auto actor = pointer.get();
				if (!actor || actor == player || actor->IsDead() || FormIds::IsRuntime(actor->GetFormID())) {
					continue;
				}
				const auto base = actor->GetActorBase();
				const auto race = actor->GetRace();
				if (!base || base->IsPreset() || !race || !race->data.flags.any(RE::RACE_DATA::Flag::kAllowPCDialogue) || actor->IsHostileToActor(player)) {
					continue;
				}
				const auto distance = DistanceTo(actor);
				if (distance && *distance <= PICK_RANGE) {
					found.emplace_back(actor, *distance);
				}
			}
			std::ranges::sort(found, {}, [](const auto& a_found) { return a_found.first->GetFormID(); });
			if (a_skip >= found.size()) {
				return std::format("error: {} NPCs to talk with near, none at {}", found.size(), a_skip);
			}
			return std::format("ref={:08X} dist={:.0f} of={}", found[a_skip].first->GetFormID(), found[a_skip].second, found.size());
		}

		std::string Subs()
		{
			const auto     manager = RE::SubtitleManager::GetSingleton();
			std::size_t    listed = 0;
			if (manager) {
				RE::BSSpinLockGuard guard{ manager->lock };
				listed = manager->subtitles.size();
			}
			return std::format("general={} dialogue={} manager={} listed={} ours={} off={} drops={}", GeneralSubtitles() ? 1 : 0, DialogueSubtitles() ? 1 : 0,
				manager ? 1 : 0, listed, up.size(), subtitlesOff ? 1 : 0, subtitleDrops);
		}

		std::string Describe()
		{
			std::uint64_t dropped = 0;
			{
				std::lock_guard lock{ queueLock };
				dropped = eventsDropped;
			}
			const auto partner = Partner();
			return std::format(
				"installed={} us={} menu={} partner={:08X} current={:08X} lingering={:08X} granted={} busy={} holder={} started={} said={} playerLines={} ends={} "
				"talks={} lastSaid={:08X} heard={} subtitled={} noticed={} dropped={} voiced={} missing={} unplayed={} silent={} far={} noSpeaker={} repeats={} "
				"tooMany={} busyRefused={} busyClosed={} sounds={} cutLong={} cutCrowded={} cutEnded={} subUp={} subsOff={} lastHeard={:08X} lastFrom={} audio={} "
				"offmain={} events={} eventsDropped={}",
				installed ? 1 : 0, ourId, DialogueOpen() ? 1 : 0, partner ? partner->GetFormID() : 0, claimer.Current(), claimer.Lingering(), claimer.Granted() ? 1 : 0,
				claimer.Busy() ? 1 : 0, claimer.Holder(), startedByPlayer ? 1 : 0, counts.said, counts.playerLines, counts.ends, counts.talks, lastSaid, counts.heard,
				counts.subtitled, counts.noticed, counts.dropped, counts.voiced, counts.missing, counts.unplayed, counts.silent, counts.farAway, counts.noSpeaker,
				counts.repeats, counts.tooMany, counts.busyRefused, counts.busyClosed, sounds.Kept(), counts.cutLong, counts.cutCrowded, counts.cutEnded, up.size(),
				subtitlesOff ? 1 : 0, lastHeard, lastFrom, AudioOn() ? 1 : 0, counts.offMain, eventsSeen.load(), dropped);
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::warn("Talk: the game's event source is not there; conversations are not shared");
			return false;
		}
		events->AddEventSink<RE::TESTopicInfoEvent>(&sink);
		installed = true;
		logs::info("Talk: watching the dialogue's topic events; conversations are read each tick");
		return true;
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		ourId = a_playerId;
		claimer.Clear();  // a talk under way is said again on the next tick
		watch.Clear();
		watched = 0;
		openLine = {};
		hearing.Clear();
		lastOf.clear();
		subtitleDrops = 0;
		subtitlesOff = false;
	}

	void Frame(const SendFn& a_send)
	{
		const auto now = Clock::now();
		FrameHear(now);
		if (!LocalPlayer::InWorld()) {
			(void)TakeQueue();
			return;
		}
		FrameTalk(a_send, now);
	}

	void Heard(const Line& a_line)
	{
		if (a_line.playerId == 0 || a_line.playerId == ourId) {
			return;
		}
		const auto now = Clock::now();
		// The speaker in the key: the talker themselves (0), the NPC, or one our game can't name (all of them as one).
		const auto speakerKey = a_line.speakerKind == Line::kTalker ? 0u : (a_line.speaker != 0 ? a_line.speaker : 0xFFFFFFFFu);
		const auto key = TalkRules::KeyOf(a_line.playerId, speakerKey);
		if (a_line.kind == Line::kEnd) {
			const auto it = lastOf.find(a_line.playerId);
			if (it != lastOf.end() && it->second.seq == a_line.seq && it->second.key == key) {
				const bool cut = sounds.End(key, [&](const Sounds::Entry& a_entry) { return TalkRules::CutOnEnd(a_entry.started, a_entry.seconds, now); }, EndSound);
				if (cut) {
					TakeDownWhere([&](const Up& a_up) { return a_up.key == key; });
				}
				lastOf.erase(it);
			}
			return;
		}
		++counts.heard;
		const auto talker = static_cast<RE::TESObjectREFR*>(RemotePlayers::StandInOf(a_line.playerId));
		RE::TESObjectREFR* speaker = talker;
		if (a_line.speakerKind == Line::kRef) {
			speaker = a_line.speaker != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_line.speaker) : nullptr;
		}
		TalkRules::Where where{ DistanceTo(speaker), DistanceTo(talker) };
		if (!where.speaker) {
			if (a_line.speakerKind == Line::kRef && a_line.speaker != 0) {
				++counts.noSpeaker;
			}
			speaker = nullptr;
		}
		const auto show = TalkRules::HowToShow(where, GeneralSubtitles() && !subtitlesOff, DialogueOpen());
		if (show == TalkRules::Show::kFar) {
			++counts.farAway;
			return;
		}
		switch (hearing.Judge(key, a_line.text, now)) {
		case TalkRules::Hearing::Verdict::kRepeat:
			++counts.repeats;
			return;
		case TalkRules::Hearing::Verdict::kTooMany:
			++counts.tooMany;
			return;
		case TalkRules::Hearing::Verdict::kShow:
			break;
		}
		lastHeard = Hash(a_line.text);
		lastFrom = a_line.playerId;
		lastOf[a_line.playerId] = { a_line.seq, key };

		std::string name = a_line.speakerKind == Line::kTalker ? NameOfRef(talker) : a_line.name;
		if (name.empty()) {
			name = NameOfRef(speaker);
		}
		auto notice = Text::Format("talk.line", { { "speaker", name }, { "line", a_line.text } });

		std::optional<double> seconds;
		if (speaker && a_line.speakerKind == Line::kRef && !a_line.voice.empty()) {
			seconds = Speak(speaker, a_line, key);
		}
		if (show == TalkRules::Show::kSubtitle && ShowSubtitle(speaker, a_line, key, notice, SubtitleTime::ShowFor(seconds, a_line.text), where.speaker.value_or(0.0f), now)) {
			return;  // counted as subtitled once still listed (FrameHear)
		}
		Notice(notice);
		++counts.noticed;
	}

	void Answered(const Protocol::TalkAnswer& a_answer)
	{
		if (claimer.Answer(a_answer.ref, a_answer.holderId, ourId) != TalkRules::Claimer::Heard::kBusy) {
			return;
		}
		if (openLine.speaker == a_answer.ref) {
			openLine = {};  // the server passed none of its lines on: no end to say
		}
		if (!startedByPlayer) {
			// A greeting of the NPC's (a forcegreet): ending it would have the NPC greet again. It runs, its lines kept home.
			++counts.busyRefused;
			logs::info("Talk: {:08X} greets our player while player {} talks with it: its lines are not shared", a_answer.ref, a_answer.holderId);
			return;
		}
		++counts.busyClosed;
		logs::info("Talk: player {} talks with {:08X}: our talk with it is ended", a_answer.holderId, a_answer.ref);
		NoteThread();
		if (const auto queueOfUI = RE::UIMessageQueue::GetSingleton()) {
			queueOfUI->AddMessage(RE::BSFixedString{ RE::DialogueMenu::MENU_NAME }, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		}
		const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_answer.ref);
		std::string holder = NameOfRef(RemotePlayers::StandInOf(a_answer.holderId));
		if (holder.empty()) {
			holder = Text::Get("talk.another");
		}
		Notice(Text::Format("talk.busy", { { "name", NameOfRef(ref) }, { "player", holder } }));
	}

	void Left(std::uint32_t a_playerId)
	{
		lastOf.erase(a_playerId);
	}

	void BeforeLoad()
	{
		EndEverything();
		openLine = {};
		watch.Clear();
	}

	void AfterLoad()
	{
		watch.Clear();
		watched = 0;
		wasOpen = false;
		openLine = {};
		(void)TakeQueue();
	}

	void Clear()
	{
		EndEverything();
		hearing.Clear();
		claimer.Clear();
		watch.Clear();
		watched = 0;
		openLine = {};
		wasOpen = false;
		ourId = 0;
		(void)TakeQueue();
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.empty() || words[0] == "status") {
			return Describe();
		}
		if (words[0] == "pick") {
			const auto skip = words.size() >= 2 ? DevLine::Number(words[1]) : std::optional<std::uint64_t>{ 0 };
			if (!skip) {
				return "error: usage: talk pick [n]";
			}
			return Pick(*skip);
		}
		if (words[0] == "subs") {
			return Subs();
		}
		return "error: usage: talk [status] | talk pick [n] | talk subs";
	}
}
