#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "TalkBook.h"
#include "game/TalkRules.h"

#include <chrono>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Conversations (docs/DESIGN.md, "Conversations"): the messages (common/Protocol.h), their forms on the wire
// (common/FormTranslate.h), the server's book of who talks with which NPC and the lines it passes (common/TalkBook.h,
// common/Server.cpp), and each game's rules (src/game/TalkRules.h). What reads the game's dialogue and shows and plays a
// friend's line (src/game/Conversations.cpp) is tested in a game only: tools/rig/steps/talk.sh.

using Protocol::Line;
using Protocol::Talk;
using Protocol::TalkAnswer;
using namespace TalkRules;

namespace
{
	using MT = Protocol::MessageType;
	using namespace std::chrono_literals;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	constexpr std::uint32_t HULDA = 0x00013BBF;  // a reference of the base game (any form ID of Skyrim.esm will do)
	constexpr std::uint32_t INFO = 0x000D2A15;

	Line Said(std::uint32_t a_speaker, std::string a_text, std::uint32_t a_seq = 1)
	{
		Line line;
		line.kind = Line::kSay;
		line.speakerKind = Line::kRef;
		line.speaker = a_speaker;
		line.info = INFO;
		line.response = 1;
		line.seq = a_seq;
		line.name = "Hulda";
		line.text = std::move(a_text);
		line.voice = "sound\\voice\\skyrim.esm\\femaleevenToned\\dialoguegen_000d2a15_1";
		return line;
	}

	Line Picked(std::string a_text, std::uint32_t a_seq = 1)
	{
		Line line;
		line.speakerKind = Line::kTalker;
		line.seq = a_seq;
		line.text = std::move(a_text);
		return line;
	}

	std::vector<Line> LinesIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<Line> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemoteLine) {
				if (const auto line = Protocol::DecodeLine(packet)) {
					out.push_back(*line);
				}
			}
		}
		return out;
	}

	std::optional<TalkAnswer> AnswerTo(Fixture& a_f, PeerId a_peer)
	{
		const auto packet = a_f.fake->WaitFor(a_peer, Type(MT::kTalkAnswer));
		return packet ? Protocol::DecodeTalkAnswer(*packet) : std::nullopt;
	}
}

// ---- the messages ----------------------------------------------------------------------------------------------------

TEST("talk: a line goes there and back as it was: an NPC's, the talker's own topic, and an end")
{
	const auto npc = Said(HULDA, "What'll it be?", 7);
	const auto sent = Protocol::DecodeLine(Protocol::Encode(npc));
	REQUIRE(sent);
	CHECK(*sent == npc);
	CHECK(Protocol::PeekType(Protocol::Encode(npc)) == MT::kLine);

	auto remote = npc;
	remote.playerId = 3;
	const auto heard = Protocol::DecodeLine(Protocol::Encode(remote, MT::kRemoteLine));
	REQUIRE(heard);
	CHECK(*heard == remote);

	const auto own = Picked("I'd like something to eat.", 8);
	const auto ownBack = Protocol::DecodeLine(Protocol::Encode(own));
	REQUIRE(ownBack);
	CHECK(*ownBack == own);

	Line end;
	end.kind = Line::kEnd;
	end.speaker = HULDA;
	end.seq = 7;
	const auto endBack = Protocol::DecodeLine(Protocol::Encode(end));
	REQUIRE(endBack);
	CHECK(*endBack == end);

	// A text past MAX_LINE_TEXT is cut at a whole character, as every string is.
	auto lengthy = Said(HULDA, std::string(Protocol::MAX_LINE_TEXT - 1, 'a') + "\xC3\xA9", 9);
	const auto cut = Protocol::DecodeLine(Protocol::Encode(lengthy));
	REQUIRE(cut);
	CHECK(cut->text.size() == Protocol::MAX_LINE_TEXT - 1);
}

TEST("talk: a line no game could say is none: no number, a blank line, an end with words, the talker's own naming an NPC")
{
	const auto refused = [](const Line& a_line) { return !Protocol::DecodeLine(Protocol::Encode(a_line)).has_value(); };
	auto line = Said(HULDA, "Hello.", 0);
	CHECK(refused(line));  // no number
	line = Said(HULDA, "  \t ");
	CHECK(refused(line));  // nothing to read
	line = Said(HULDA, "Hello.");
	line.kind = 2;
	CHECK(refused(line));
	line = Said(HULDA, "Hello.");
	line.speakerKind = 2;
	CHECK(refused(line));
	line = Said(HULDA, "Hello.");
	line.kind = Line::kEnd;
	CHECK(refused(line));  // an end with a text
	line.text.clear();
	CHECK(refused(line));  // ... or a recording
	line.voice.clear();
	CHECK(!refused(line));
	line = Said(HULDA, "Hello.");
	line.info = 0;
	CHECK(refused(line));  // a response of no info
	line.response = 0;
	CHECK(!refused(line));

	for (int field = 0; field < 5; ++field) {
		auto own = Picked("Goodbye.");
		switch (field) {
		case 0: own.speaker = HULDA; break;
		case 1: own.info = INFO; break;
		case 2: own.info = INFO, own.response = 1; break;
		case 3: own.name = "Hulda"; break;
		default: own.voice = "sound\\voice\\x\\y\\z"; break;
		}
		CHECK(refused(own));
	}

	// Trailing bytes, and a kLine read as kRemoteLine's shape, are none.
	auto bytes = Protocol::Encode(Said(HULDA, "Hello."));
	bytes.push_back(0);
	CHECK(!Protocol::DecodeLine(bytes));
	CHECK(!Protocol::DecodeLine(Protocol::Encode(Talk{ HULDA, true })));
}

TEST("talk: a talk and its answer go there and back; no NPC, no holder or a bad flag is none")
{
	for (const bool on : { true, false }) {
		const auto talk = Protocol::DecodeTalk(Protocol::Encode(Talk{ HULDA, on }));
		REQUIRE(talk);
		CHECK(talk->ref == HULDA);
		CHECK(talk->on == on);
	}
	CHECK(!Protocol::DecodeTalk(Protocol::Encode(Talk{ 0, true })));
	auto bad = Protocol::Encode(Talk{ HULDA, true });
	bad.back() = 2;
	CHECK(!Protocol::DecodeTalk(bad));

	const auto answer = Protocol::DecodeTalkAnswer(Protocol::Encode(TalkAnswer{ HULDA, 4 }));
	REQUIRE(answer);
	CHECK(*answer == (TalkAnswer{ HULDA, 4 }));
	CHECK(!Protocol::DecodeTalkAnswer(Protocol::Encode(TalkAnswer{ 0, 4 })));
	CHECK(!Protocol::DecodeTalkAnswer(Protocol::Encode(TalkAnswer{ HULDA, 0 })));
}

TEST("talk: the speaker and the info cross by plugin name; one a game lacks is 0 (shown by name), and a talk of it is not sent")
{
	using FormIds::Mapper;
	using FormTranslate::Result;
	using FormTranslate::Way;
	for (const auto type : { MT::kLine, MT::kRemoteLine, MT::kTalk, MT::kTalkAnswer }) {
		CHECK(FormTranslate::CarriesForms(type));
	}

	// The host has Inns.esp before Shared.esp; the guest has Shared.esp only (01 there, 02 in the session's table).
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Inns.esp", "Shared.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const Mapper guestEdge(guest, session);
	const auto   read = [&](const Line& a_line, Result& a_result) {
		const auto packet = Protocol::Encode(a_line, MT::kRemoteLine);
		a_result = FormTranslate::Translate(guestEdge, Way::kToLocal, packet);
		return Protocol::DecodeLine(a_result.kind == Result::Kind::kChanged ? a_result.packet : packet);
	};

	Result result;
	auto   line = Said(HULDA, "Hello.");
	line.playerId = 1;
	auto in = read(line, result);
	REQUIRE(in);
	CHECK(in->speaker == HULDA);
	CHECK(in->info == INFO);
	CHECK(in->response == 1u);

	// An NPC and a line of Shared.esp: the guest's own IDs.
	line.speaker = 0x02000D01;
	line.info = 0x02000E02;
	in = read(line, result);
	REQUIRE(in);
	CHECK(in->speaker == 0x01000D01u);
	CHECK(in->info == 0x01000E02u);
	CHECK(in->response == 1u);
	CHECK(in->text == "Hello.");

	// An NPC and a line of Inns.esp, which the guest lacks: no speaker, no info and so no response; the name, the text
	// and the recording's path still come (it is read as text, by name).
	line.speaker = 0x01000D01;
	line.info = 0x01000E02;
	in = read(line, result);
	REQUIRE(in);
	CHECK(in->speaker == 0u);
	CHECK(in->info == 0u);
	CHECK(in->response == 0u);
	CHECK(in->name == "Hulda");
	CHECK(in->text == "Hello.");
	CHECK(result.fields == 2u);

	// A talk with an NPC the guest lacks is not sent on to it (it can't talk with it), nor its answer.
	const auto talkTo = FormTranslate::Translate(guestEdge, Way::kToLocal, Protocol::Encode(TalkAnswer{ 0x01000D01, 2 }));
	CHECK(talkTo.kind == Result::Kind::kDrop);
	const auto shared = FormTranslate::Translate(guestEdge, Way::kToLocal, Protocol::Encode(TalkAnswer{ 0x02000D01, 2 }));
	REQUIRE(shared.kind == Result::Kind::kChanged);
	CHECK(Protocol::DecodeTalkAnswer(shared.packet)->ref == 0x01000D01u);

	// A runtime NPC (FF: a courier the game made) is no game's but the talker's: its line goes by name, its talk is not sent.
	const Mapper same(host, session);
	auto         runtime = Said(0xFF000A12, "I've got a letter for you.");
	runtime.info = 0;
	runtime.response = 0;
	const auto out = FormTranslate::Translate(same, Way::kToSession, Protocol::Encode(runtime));
	REQUIRE(out.kind == Result::Kind::kChanged);
	const auto sent = Protocol::DecodeLine(out.packet);
	REQUIRE(sent);
	CHECK(sent->speaker == 0u);
	CHECK(sent->text == "I've got a letter for you.");
	CHECK(out.runtime == 1u);
	CHECK(FormTranslate::Translate(same, Way::kToSession, Protocol::Encode(Talk{ 0xFF000A12, true })).kind == Result::Kind::kDrop);
	// The talker's own line names no form: as it is.
	CHECK(FormTranslate::Translate(same, Way::kToSession, Protocol::Encode(Picked("Goodbye."))).kind == Result::Kind::kSame);
}

// ---- who talks with whom: the server's book --------------------------------------------------------------------------

TEST("talk book: one player at a time talks with an NPC; a new talk ends their last, and a talk not said again lapses")
{
	TalkBook   book;
	const auto t0 = std::chrono::steady_clock::now();
	auto       claim = book.Claim(1, HULDA, t0);
	CHECK(claim.granted);
	CHECK(claim.holder == 1u);
	CHECK(book.Holds(1, HULDA, t0));

	claim = book.Claim(2, HULDA, t0 + 1s);
	CHECK(!claim.granted);
	CHECK(claim.holder == 1u);
	CHECK(!book.Holds(2, HULDA, t0 + 1s));
	CHECK(!book.Release(2, HULDA));  // not theirs to let go
	CHECK(book.HolderOf(HULDA, t0 + 1s) == 1u);

	// Said again: kept past the first HOLD.
	CHECK(book.Claim(1, HULDA, t0 + 15s).granted);
	CHECK(book.Holds(1, HULDA, t0 + 30s));
	CHECK(!book.Claim(2, HULDA, t0 + 30s).granted);

	// Player 1 talks with another NPC: the first is kept until their game says it off (its goodbye), then free.
	CHECK(book.Claim(1, 0x00013BBD, t0 + 31s).granted);
	CHECK(book.Holds(1, HULDA, t0 + 31s));
	CHECK(!book.Claim(2, HULDA, t0 + 31s).granted);
	CHECK(book.Release(1, HULDA));
	CHECK(!book.Holds(1, HULDA, t0 + 31s));
	CHECK(book.Claim(2, HULDA, t0 + 31s).granted);
	CHECK(book.Size() == 2u);

	// Not said again within HOLD: lapsed, and anyone's.
	const auto late = t0 + 31s + TalkBook::HOLD;
	CHECK(!book.Holds(1, 0x00013BBD, late));
	CHECK(book.HolderOf(0x00013BBD, late) == 0u);
	CHECK(book.Claim(3, 0x00013BBD, late).granted);

	// Let go, and leaving.
	CHECK(book.Claim(2, HULDA, late).granted);
	CHECK(book.Release(2, HULDA));
	CHECK(book.HolderOf(HULDA, late) == 0u);
	CHECK(book.Claim(2, HULDA, late).granted);
	const auto freed = book.Left(2);
	REQUIRE(freed.size() == 1u);
	CHECK(freed[0] == HULDA);
	CHECK(book.Claim(1, HULDA, late).granted);
}

TEST("talk book: the NPC a player left for another is kept until said off (its goodbye passes), the last one only")
{
	TalkBook                book;
	const auto              t0 = std::chrono::steady_clock::now();
	constexpr std::uint32_t B = 0x00013BBD;
	constexpr std::uint32_t C = 0x00013BBE;
	CHECK(book.Claim(1, HULDA, t0).granted);
	CHECK(book.Claim(1, B, t0 + 1s).granted);
	CHECK(book.Holds(1, HULDA, t0 + 1s));  // left 1 s ago: its goodbye is still said
	CHECK(book.Holds(1, B, t0 + 1s));
	// A third: the one before it (B) is kept, the older one (Hulda) is over.
	CHECK(book.Claim(1, C, t0 + 2s).granted);
	CHECK(!book.Holds(1, HULDA, t0 + 2s));
	CHECK(book.Holds(1, B, t0 + 2s));
	CHECK(book.Holds(1, C, t0 + 2s));
	CHECK(book.Size() == 2u);
	// Said again: the one lingering stays until said off.
	CHECK(book.Claim(1, C, t0 + 3s).granted);
	CHECK(book.Holds(1, B, t0 + 3s));
	CHECK(!book.Claim(2, B, t0 + 3s).granted);
	CHECK(book.Release(1, B));
	CHECK(book.Claim(2, B, t0 + 3s).granted);
	CHECK(book.Claim(2, HULDA, t0 + 3s).granted);
	// Never said off (the word lost): over HOLD after it was last said, as any talk.
	CHECK(book.HolderOf(C, t0 + 3s + TalkBook::HOLD) == 0u);
	CHECK(book.Claim(2, C, t0 + 3s + TalkBook::HOLD).granted);
}

TEST("talk book: an NPC the session can't name (a runtime one) or none is nobody's: granted to all, and never kept")
{
	TalkBook   book;
	const auto now = std::chrono::steady_clock::now();
	CHECK(book.Claim(1, 0xFF000A12, now).granted);
	CHECK(book.Claim(2, 0xFF000A12, now).granted);
	CHECK(book.Claim(2, 0, now).granted);
	CHECK(book.Size() == 0u);
	CHECK(!book.Holds(1, 0xFF000A12, now));

	// Never more than MAX kept: a claim past it is granted and not kept, never refused.
	for (std::uint32_t i = 0; i < TalkBook::MAX + 4; ++i) {
		CHECK(book.Claim(100 + i, 0x00010000 + i, now).granted);
	}
	CHECK(book.Size() == TalkBook::MAX);
}

TEST("talk server: each talk is answered to the asker alone; another player's talk with that NPC is told busy, and it frees on off or leaving")
{
	Fixture    f;
	const auto a = f.Join(1, Fixture::MakeHello("A", 0x1111));
	const auto b = f.Join(2, Fixture::MakeHello("B", 0x2222));
	REQUIRE(a);
	REQUIRE(b);

	f.fake->Deliver(1, Protocol::Encode(Talk{ HULDA, true }));
	auto answer = AnswerTo(f, 1);
	REQUIRE(answer);
	CHECK(*answer == (TalkAnswer{ HULDA, a->playerId }));

	f.fake->Deliver(2, Protocol::Encode(Talk{ HULDA, true }));
	answer = AnswerTo(f, 2);
	REQUIRE(answer);
	CHECK(answer->holderId == a->playerId);
	REQUIRE(f.LoggedSoon(std::format("player {} would talk with {:08X}, which player {} talks with: told it is busy", b->playerId, HULDA, a->playerId)));
	REQUIRE(f.fake->Settle());
	for (const auto& packet : f.fake->Take(1)) {
		CHECK(Protocol::PeekType(packet) != MT::kTalkAnswer);  // nothing of B's to A
	}

	// A says the talk is over: B has it.
	f.fake->Deliver(1, Protocol::Encode(Talk{ HULDA, false }));
	f.fake->Deliver(2, Protocol::Encode(Talk{ HULDA, true }));
	answer = AnswerTo(f, 2);
	REQUIRE(answer);
	CHECK(answer->holderId == b->playerId);

	// B leaves without a word: A has it again.
	f.fake->Drop(2);
	REQUIRE(f.LoggedSoon(std::format("player {} left", b->playerId)));
	f.fake->Deliver(1, Protocol::Encode(Talk{ HULDA, true }));
	answer = AnswerTo(f, 1);
	REQUIRE(answer);
	CHECK(answer->holderId == a->playerId);
}

TEST("talk server: lines go to the others with whose they are; an NPC's only from who talks with it, an NPC nobody can name from anyone")
{
	Fixture    f;
	const auto a = f.Join(1, Fixture::MakeHello("A", 0x1111));
	const auto b = f.Join(2, Fixture::MakeHello("B", 0x2222));
	const auto c = f.Join(3, Fixture::MakeHello("C", 0x3333));
	REQUIRE(a);
	REQUIRE(b);
	REQUIRE(c);
	f.fake->Deliver(1, Protocol::Encode(Talk{ HULDA, true }));
	REQUIRE(AnswerTo(f, 1));

	// A's line of Hulda, and A's own topic: to B and C, as A's, never back to A.
	f.fake->Deliver(1, Protocol::Encode(Picked("What's for sale?", 1)));
	f.fake->Deliver(1, Protocol::Encode(Said(HULDA, "Take a look.", 2)));
	REQUIRE(f.fake->Settle());
	const auto toB = LinesIn(f.fake->Take(2));
	REQUIRE(toB.size() == 2u);
	CHECK(toB[0].playerId == a->playerId);
	CHECK(toB[0].speakerKind == Line::kTalker);
	CHECK(toB[0].text == "What's for sale?");
	CHECK(toB[1].speaker == HULDA);
	CHECK(toB[1].text == "Take a look.");
	CHECK(toB[1].voice == Said(HULDA, "").voice);
	CHECK(LinesIn(f.fake->Take(3)).size() == 2u);
	CHECK(LinesIn(f.fake->Take(1)).empty());

	// B's line of Hulda, whom A talks with: not passed on, said once.
	f.fake->Deliver(2, Protocol::Encode(Said(HULDA, "Not yours.", 1)));
	f.fake->Deliver(2, Protocol::Encode(Said(HULDA, "Still not yours.", 2)));
	REQUIRE(f.LoggedSoon(std::format("player {} sent a line of {:08X}, whom they do not talk with (player {} does)", b->playerId, HULDA, a->playerId)));
	// ... while B's line of an NPC nobody can name (0: a runtime one), its own topic and an end pass.
	f.fake->Deliver(2, Protocol::Encode(Said(0, "A letter for you.", 3)));
	REQUIRE(f.fake->Settle());
	const auto toA = LinesIn(f.fake->Take(1));
	REQUIRE(toA.size() == 1u);
	CHECK(toA[0].playerId == b->playerId);
	CHECK(toA[0].speaker == 0u);
	CHECK(toA[0].text == "A letter for you.");
	Line end;
	end.kind = Line::kEnd;
	end.speaker = HULDA;
	end.seq = 2;
	f.fake->Deliver(1, Protocol::Encode(end));
	REQUIRE(f.fake->Settle());
	const auto ends = LinesIn(f.fake->Take(2));
	REQUIRE(!ends.empty());
	CHECK(ends.back().kind == Line::kEnd);
	CHECK(std::ranges::count_if(f.LogLines(), [](const std::string& a_line) { return a_line.find("whom they do not talk with") != std::string::npos; }) == 1);
}

TEST("talk server: a player who leaves an NPC for another still passes its goodbye, until their game says it off")
{
	Fixture    f;
	const auto a = f.Join(1, Fixture::MakeHello("A", 0x1111));
	REQUIRE(a);
	REQUIRE(f.Join(2, Fixture::MakeHello("B", 0x2222)));
	f.fake->Deliver(1, Protocol::Encode(Talk{ HULDA, true }));
	REQUIRE(AnswerTo(f, 1));
	f.fake->Deliver(1, Protocol::Encode(Talk{ 0x00013BBD, true }));  // within the linger, another NPC
	REQUIRE(AnswerTo(f, 1));
	f.fake->Deliver(1, Protocol::Encode(Said(HULDA, "Come back soon.", 3)));
	REQUIRE(f.fake->Settle());
	auto toB = LinesIn(f.fake->Take(2));
	REQUIRE(toB.size() == 1u);
	CHECK(toB[0].text == "Come back soon.");

	// Said off: a line of it now is not theirs.
	f.fake->Deliver(1, Protocol::Encode(Talk{ HULDA, false }));
	f.fake->Deliver(1, Protocol::Encode(Said(HULDA, "Too late.", 4)));
	REQUIRE(f.LoggedSoon(std::format("player {} sent a line of {:08X}, whom they do not talk with", a->playerId, HULDA)));
	REQUIRE(f.fake->Settle());
	CHECK(LinesIn(f.fake->Take(2)).empty());
}

TEST("talk server: lines past the budget are dropped (a line is of the moment), said once")
{
	Fixture    f;
	const auto a = f.Join(1, Fixture::MakeHello("A", 0x1111));
	REQUIRE(a);
	REQUIRE(f.Join(2, Fixture::MakeHello("B", 0x2222)));
	for (std::uint32_t i = 1; i <= 40; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Picked(std::format("Line {}.", i), i)));
	}
	REQUIRE(f.LoggedSoon(std::format("player {} sends lines faster than the server passes them on", a->playerId)));
	REQUIRE(f.fake->Settle());
	const auto heard = LinesIn(f.fake->Take(2));
	CHECK(heard.size() >= 16u);
	CHECK(heard.size() < 40u);
}

// ---- the game's rules ------------------------------------------------------------------------------------------------

TEST("talk rules: a recording's path is one under Sound\\Voice, as the game reads it; any other file is none")
{
	CHECK(VoiceBase("Sound\\Voice\\Skyrim.esm\\FemaleEvenToned\\DialogueGe_000D2A15_1.fuz") == "sound\\voice\\skyrim.esm\\femaleeventoned\\dialoguege_000d2a15_1");
	CHECK(VoiceBase("Data\\Sound\\Voice\\Skyrim.esm\\MaleNord\\x_1.xwm") == "sound\\voice\\skyrim.esm\\malenord\\x_1");
	CHECK(VoiceBase("data/sound/voice/Dawnguard.esm/MaleNord/x_1.wav") == "sound\\voice\\dawnguard.esm\\malenord\\x_1");
	CHECK(VoiceBase("Voice\\Skyrim.esm\\MaleNord\\x_1") == "sound\\voice\\skyrim.esm\\malenord\\x_1");  // the extension left off, the root's Sound too
	CHECK(VoiceBase("sound\\voice\\mod.esp\\a\\b\\c_1.fuz") == "sound\\voice\\mod.esp\\a\\b\\c_1");      // a mod's deeper folders
	for (const std::string_view bad : { "", "sound\\voice\\skyrim.esm\\x_1.fuz", "sound\\voice\\..\\..\\skyrim.ini", "sound\\voice\\a\\..\\b\\c.fuz",
			 "c:\\sound\\voice\\a\\b\\c.fuz", "sound\\voice\\a\\b\\c.fuz:stream", "sound\\fx\\a\\b\\c.wav", "meshes\\a\\b\\c.nif", "sound\\voice\\a\\b\\c.dll",
			 "sound\\voice\\a\\\\b\\c.fuz", "sound\\voice\\a\\b\\", "sound\\voice\\a\\b\\.fuz", "sound\\voice\\a\\b\\c\x01.fuz" }) {
		CHECK(!VoiceBase(bad));
	}
	CHECK(!VoiceBase("sound\\voice\\a\\b\\" + std::string(260, 'c')));
	const auto files = VoiceFiles("sound\\voice\\a\\b\\c");
	CHECK(files[0] == "sound\\voice\\a\\b\\c.fuz");
	CHECK(files[1] == "sound\\voice\\a\\b\\c.xwm");
	CHECK(files[2] == "sound\\voice\\a\\b\\c.wav");
}

TEST("talk rules: a subtitle is a new line when it changes, or when the same words begin again after a moment")
{
	LineWatch  watch;
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(!watch.Read("", t0));
	CHECK(!watch.Read(" ", t0));  // an idle's blank line
	CHECK(watch.Read("Welcome.", t0));
	CHECK(!watch.Read("Welcome.", t0 + 16ms));  // still up
	CHECK(!watch.Read("Welcome.", t0 + 5s));    // the engine keeps the last one after it ends
	CHECK(watch.Read("Anything else?", t0 + 6s));

	// The same line said again: the engine says a line begins; read at once it may be the old one still up.
	watch.Begin(t0 + 10s);
	CHECK(!watch.Read("Anything else?", t0 + 10s + 100ms));
	CHECK(watch.Read("Anything else?", t0 + 10s + LineWatch::SAME_AGAIN));
	CHECK(!watch.Read("Anything else?", t0 + 11s));  // once
	// A begin then a new text: the new text is the line, and the begin is used up by it.
	watch.Begin(t0 + 12s);
	CHECK(watch.Read("Farewell.", t0 + 12s + 50ms));
	CHECK(!watch.Read("Farewell.", t0 + 13s));

	watch.Clear();  // another speaker
	CHECK(watch.Read("Farewell.", t0 + 14s));
}

TEST("talk rules: a talk's start never sends the subtitle left up from before (a goodbye, an ambient line); its greeting goes")
{
	LineWatch  watch;
	const auto t0 = std::chrono::steady_clock::now();

	// The NPC's goodbye of the talk before is still its subtitle: primed, not sent, however long it stays up.
	watch.Prime("Come back soon.", std::nullopt);
	CHECK(!watch.Read("Come back soon.", t0));
	CHECK(!watch.Read("Come back soon.", t0 + 10s));
	// The greeting begins: a new text is a new line at once.
	watch.Begin(t0 + 11s);
	CHECK(watch.Read("Welcome to the Bannered Mare.", t0 + 11s + 20ms));

	// A greeting with the same words as the line left up: its begin arms the watch; sent once SAME_AGAIN has passed.
	watch.Prime("What do you need?", std::nullopt);
	CHECK(!watch.Read("What do you need?", t0 + 20s));
	watch.Begin(t0 + 21s);
	CHECK(!watch.Read("What do you need?", t0 + 21s + 100ms));
	CHECK(watch.Read("What do you need?", t0 + 21s + LineWatch::SAME_AGAIN));
	CHECK(!watch.Read("What do you need?", t0 + 23s));

	// The greeting began a moment before the watch turned to its speaker (the menu opened a frame or two after):
	// read SAME_AGAIN after that begin, it goes at once; read sooner, it waits.
	watch.Prime("Hm?", t0 + 30s);
	CHECK(watch.Read("Hm?", t0 + 30s + 400ms));
	watch.Prime("Hm?", t0 + 40s);
	CHECK(!watch.Read("Hm?", t0 + 40s + 50ms));
	CHECK(watch.Read("Hm?", t0 + 40s + LineWatch::SAME_AGAIN));

	// Nothing up (or a blank idle line): the first line is new, as after Clear.
	watch.Prime(" ", std::nullopt);
	CHECK(watch.Read("Hello.", t0 + 50s));
	watch.Prime("", std::nullopt);
	CHECK(watch.Read("Hello.", t0 + 51s));
}

TEST("talk rules: each speaker's last begun line is kept a while; a line ends only by its own info's end")
{
	Begins     begins;
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(!begins.Of(HULDA, t0, Begins::BEGUN_RECENT));
	begins.Note(HULDA, INFO, t0);
	begins.Note(0, INFO, t0);  // no speaker: not kept
	auto begun = begins.Of(HULDA, t0 + 500ms, Begins::BEGUN_RECENT);
	REQUIRE(begun);
	CHECK(begun->info == INFO);
	CHECK(begun->at == t0);
	CHECK(!begins.Of(HULDA, t0 + 2s, Begins::BEGUN_RECENT));
	CHECK(begins.Of(HULDA, t0 + 2s, 60s));
	CHECK(!begins.Of(0x1234, t0, 60s));
	begins.Note(HULDA, INFO + 1, t0 + 3s);  // its next line replaces it
	begun = begins.Of(HULDA, t0 + 3s, 60s);
	REQUIRE(begun);
	CHECK(begun->info == INFO + 1);
	for (std::uint32_t i = 1; i <= Begins::MAX + 4; ++i) {
		begins.Note(0x00010000 + i, i, t0 + 4s);
	}
	CHECK(!begins.Of(HULDA, t0 + 4s, 60s));  // the oldest went
	CHECK(begins.Of(0x00010000 + Begins::MAX + 4, t0 + 4s, 60s));
	begins.Clear();
	CHECK(!begins.Of(0x00010000 + Begins::MAX + 4, t0 + 4s, 60s));

	// A line's end: its own info's, or one not known; the greeting's end fired after the answer is up is not the answer's.
	CHECK(EndsLine(INFO, INFO));
	CHECK(!EndsLine(INFO, INFO + 1));
	CHECK(EndsLine(0, INFO));
	CHECK(EndsLine(INFO, 0));
}

TEST("talk rules: the talk is said on start, every REFRESH, off LINGER after; busy ends ours once and keeps its lines home")
{
	Claimer    claimer;
	const auto t0 = std::chrono::steady_clock::now();
	using Say = Claimer::Say;
	CHECK(claimer.Frame(0, t0).empty());
	auto said = claimer.Frame(HULDA, t0);
	REQUIRE(said.size() == 1u);
	CHECK(said[0] == (Say{ HULDA, true }));
	CHECK(claimer.Talking());
	CHECK(claimer.Shares(HULDA));
	CHECK(!claimer.Shares(0x1234));
	CHECK(claimer.Frame(HULDA, t0 + 1s).empty());
	said = claimer.Frame(HULDA, t0 + Claimer::REFRESH);
	REQUIRE(said.size() == 1u);
	CHECK(said[0] == (Say{ HULDA, true }));
	CHECK(claimer.Answer(HULDA, 5, 5) == Claimer::Heard::kGranted);
	CHECK(claimer.Granted());

	// The talk ends: its lines still pass for LINGER (the goodbye), then it is said off.
	CHECK(claimer.Frame(0, t0 + 6s).empty());
	CHECK(!claimer.Talking());
	CHECK(claimer.Shares(HULDA));
	CHECK(claimer.Frame(0, t0 + 7s).empty());
	said = claimer.Frame(0, t0 + 6s + Claimer::LINGER);
	REQUIRE(said.size() == 1u);
	CHECK(said[0] == (Say{ HULDA, false }));
	CHECK(!claimer.Shares(HULDA));

	// Back to the same NPC within LINGER: still ours, no off said.
	CHECK(claimer.Frame(HULDA, t0 + 10s).size() == 1u);
	CHECK(claimer.Frame(0, t0 + 11s).empty());
	said = claimer.Frame(HULDA, t0 + 12s);
	REQUIRE(said.size() == 1u);
	CHECK(said[0] == (Say{ HULDA, true }));
	CHECK(claimer.Frame(HULDA, t0 + 12s + Claimer::LINGER).empty());

	// Straight to another NPC: the first is said off after LINGER, the new one on at once.
	said = claimer.Frame(0x00013BBD, t0 + 20s);
	REQUIRE(said.size() == 1u);
	CHECK(said[0] == (Say{ 0x00013BBD, true }));
	said = claimer.Frame(0x00013BBD, t0 + 20s + Claimer::LINGER);
	REQUIRE(said.size() == 1u);
	CHECK(said[0] == (Say{ HULDA, false }));

	// Busy: once a talk, and then nothing of it is said or shared until it ends; an answer of another NPC is ignored.
	CHECK(claimer.Answer(HULDA, 7, 5) == Claimer::Heard::kIgnored);
	CHECK(claimer.Answer(0x00013BBD, 7, 5) == Claimer::Heard::kBusy);
	CHECK(claimer.Busy());
	CHECK(claimer.Holder() == 7u);
	CHECK(claimer.Answer(0x00013BBD, 7, 5) == Claimer::Heard::kIgnored);
	CHECK(!claimer.Talking());
	CHECK(!claimer.Shares(0x00013BBD));
	CHECK(claimer.Frame(0x00013BBD, t0 + 40s).empty());  // no refresh while busy
	// A busy talk ends: never ours, so nothing of it lingers and no off is said (the server would refuse its lines).
	CHECK(claimer.Frame(0, t0 + 41s).empty());
	CHECK(!claimer.Shares(0x00013BBD));
	CHECK(claimer.Frame(0, t0 + 41s + Claimer::LINGER).empty());
	said = claimer.Frame(0x00013BBD, t0 + 50s);  // a new talk: asked again
	REQUIRE(said.size() == 1u);
	CHECK(!claimer.Busy());

	claimer.Clear();
	CHECK(claimer.Current() == 0u);
	CHECK(claimer.Lingering() == 0u);
	CHECK(!claimer.Talking());
}

TEST("talk rules: a heard line is shown once (a repeat within 5 s is skipped) and at most 3 of one speaker in 3 s")
{
	Hearing    hearing;
	const auto t0 = std::chrono::steady_clock::now();
	const auto key = KeyOf(2, HULDA);
	CHECK(KeyOf(2, HULDA) != KeyOf(3, HULDA));
	CHECK(KeyOf(2, 0) != KeyOf(2, HULDA));
	CHECK(hearing.Judge(key, "Hello.", t0) == Hearing::Verdict::kShow);
	CHECK(hearing.Judge(key, "Hello.", t0 + 1s) == Hearing::Verdict::kRepeat);
	CHECK(hearing.Judge(KeyOf(3, HULDA), "Hello.", t0 + 1s) == Hearing::Verdict::kShow);  // another talker's
	CHECK(hearing.Judge(key, "Anything else?", t0 + 1500ms) == Hearing::Verdict::kShow);
	CHECK(hearing.Judge(key, "And more?", t0 + 2s) == Hearing::Verdict::kShow);
	CHECK(hearing.Judge(key, "Still more?", t0 + 2500ms) == Hearing::Verdict::kTooMany);
	CHECK(hearing.Judge(key, "Still more?", t0 + 3s + 1ms) == Hearing::Verdict::kShow);  // the first out of the window
	CHECK(hearing.Judge(key, "Hello.", t0 + Hearing::REPEAT_WINDOW) == Hearing::Verdict::kShow);  // its repeat window is over
	hearing.Clear();
	CHECK(hearing.Judge(key, "Hello.", t0 + 6s) == Hearing::Verdict::kShow);
}

TEST("talk rules: two conversations near us, and a talker's own topics, each have their own budget of lines")
{
	Hearing    hearing;
	const auto t0 = std::chrono::steady_clock::now();
	const auto first = KeyOf(2, HULDA);        // player 2 talks with Hulda
	const auto second = KeyOf(3, 0x00013BBD);  // player 3, at the same time, with another NPC
	const auto own = KeyOf(2, 0);              // player 2's own topics
	for (int i = 0; i < 3; ++i) {
		CHECK(hearing.Judge(first, std::format("Hulda {}.", i), t0 + i * 200ms) == Hearing::Verdict::kShow);
		CHECK(hearing.Judge(second, std::format("Other {}.", i), t0 + i * 200ms + 100ms) == Hearing::Verdict::kShow);
		CHECK(hearing.Judge(own, std::format("Topic {}.", i), t0 + i * 200ms + 150ms) == Hearing::Verdict::kShow);
	}
	CHECK(hearing.Judge(first, "Hulda 3.", t0 + 1s) == Hearing::Verdict::kTooMany);
	CHECK(hearing.Judge(second, "Other 3.", t0 + 1s) == Hearing::Verdict::kTooMany);
	CHECK(hearing.Judge(own, "Topic 3.", t0 + 1s) == Hearing::Verdict::kTooMany);

	// Never more than MAX_KEPT kept, the oldest first.
	for (std::size_t i = 0; i < Hearing::MAX_KEPT + 8; ++i) {
		CHECK(hearing.Judge(KeyOf(10, static_cast<std::uint32_t>(0x00020000 + i)), "Hi.", t0 + 2s) == Hearing::Verdict::kShow);
	}
	CHECK(hearing.Judge(first, "Hulda 0.", t0 + 2s) == Hearing::Verdict::kShow);  // its line at 0 is gone: not a repeat
}

TEST("talk rules: a line near us shows over its speaker as the game's own settings allow, as a notice otherwise, and not at all far away")
{
	const Where nearby{ 300.0f, 200.0f };
	CHECK(HowToShow(nearby, true, false) == Show::kSubtitle);
	CHECK(HowToShow(nearby, false, false) == Show::kNotice);  // General Subtitles off
	CHECK(HowToShow(nearby, true, true) == Show::kNotice);    // in our own conversation
	CHECK(HowToShow(Where{ HEAR_RANGE + 1.0f, 100.0f }, true, false) == Show::kFar);  // the speaker's distance counts
	CHECK(HowToShow(Where{ std::nullopt, 500.0f }, true, false) == Show::kNotice);    // a speaker our game lacks: by name
	CHECK(HowToShow(Where{ std::nullopt, HEAR_RANGE * 2 }, true, false) == Show::kFar);
	CHECK(HowToShow(Where{}, true, false) == Show::kFar);
	CHECK(HowToShow(Where{ std::numeric_limits<float>::quiet_NaN(), 1.0f }, true, false) == Show::kFar);
}

TEST("talk rules: a line's end cuts its sound only when much of it is left, or its length is not known")
{
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(CutOnEnd(t0, 4.0, t0 + 1s));            // skipped early
	CHECK(!CutOnEnd(t0, 4.0, t0 + 3500ms));       // about to end: plays out
	CHECK(!CutOnEnd(t0, 4.0, t0 + 10s));          // over already
	CHECK(CutOnEnd(t0, std::nullopt, t0 + 10s));  // not known
	CHECK(CutOnEnd(t0, -1.0, t0));
}
