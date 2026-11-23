#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/FaceRules.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// A friend's face on their stand-in (docs/DESIGN.md, "Players": "Its face"): the face on the wire and through the
// server, its forms between two load orders, and the rules free of the game (src/game/FaceRules.h): which tint mask a
// layer is, which head parts a stand-in gets, whether a face goes on, when ours is sent, how many copies a game run
// makes, how a stand-in's head is watched, and how what it shows is compared with what came.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using FaceRules::Fit;
	using Step = FaceRules::Watch::Step;

	constexpr std::uint32_t NORD = 0x00013746;
	constexpr std::uint32_t IMPERIAL = 0x00013744;

	Protocol::PlayerFace FaceOf(std::uint32_t a_race = NORD, bool a_female = false)
	{
		Protocol::PlayerFace face;
		face.race = a_race;
		face.female = a_female;
		face.headParts = { 0x00051622, 0x0006A0CE, 0x000E0EC7 };  // a head, eyes, a hair
		face.partTypes = static_cast<std::uint8_t>(FaceRules::TypeBit(1) | FaceRules::TypeBit(2) | FaceRules::TypeBit(3));
		face.hairColor = 0x000A0435;
		face.hairRgb = Protocol::PackColor(40, 30, 20, 255);
		face.faceDetails = 0x0001A28B;
		face.skin = 0;
		face.height = 1.03f;
		for (std::size_t i = 0; i < Protocol::FACE_MORPHS; ++i) {
			face.morphs[i] = 0.05f * static_cast<float>(i) - 0.4f;
		}
		face.morphs[5] = std::bit_cast<float>(Protocol::FACE_UNSET);  // a slider never set
		face.presets = { 2, Protocol::FACE_UNSET, 0xFFFFFFFF, 7 };
		face.tints = {
			{ .index = 0, .path = FaceRules::PathHash("Actors\\Character\\Character Assets\\TintMasks\\SkinTone.dds"), .color = Protocol::PackColor(200, 160, 140, 255), .value = 100, .type = 6 },
			{ .index = 9, .path = FaceRules::PathHash("Actors\\Character\\Character Assets\\TintMasks\\MaleHeadWarPaint_01.dds"), .color = Protocol::PackColor(10, 10, 80, 255), .value = 60, .type = 9 },
		};
		face.bodyTint = Protocol::PackColor(200, 160, 140, 255);
		return face;
	}

	// Where the tint count is in a face packet with a_parts head parts.
	constexpr std::size_t TintCountAt(std::size_t a_parts)
	{
		return 1 + 4 + 1 + 1 + a_parts * 4 + 1 + 4 * 4 + 4 + Protocol::FACE_MORPHS * 4 + Protocol::FACE_PRESETS * 4;
	}

	std::vector<std::pair<Protocol::RemoteFace, bool>> SentFaces(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<std::pair<Protocol::RemoteFace, bool>> out;
		for (const auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kRemoteFace) {
				if (const auto face = Protocol::DecodeRemoteFace(sent.data)) {
					out.emplace_back(*face, sent.reliable);
				}
			}
		}
		return out;
	}
}

// ---- the wire ----

TEST("faces: this protocol is version 10 with faces as message types 41 and 42, and both carry forms")
{
	static_assert(Protocol::VERSION >= 10);
	static_assert(static_cast<int>(MT::kPlayerFace) == 41 && static_cast<int>(MT::kRemoteFace) == 42);
	CHECK(FormTranslate::CarriesForms(MT::kPlayerFace));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteFace));
}

TEST("faces: a face and a friend's face go over the wire and come back the same")
{
	const auto face = FaceOf(IMPERIAL, true);
	const auto packet = Protocol::Encode(face);
	CHECK(Protocol::PeekType(packet) == MT::kPlayerFace);
	const auto back = Protocol::DecodePlayerFace(packet);
	REQUIRE(back);
	CHECK(*back == face);
	CHECK(std::bit_cast<std::uint32_t>(back->morphs[5]) == Protocol::FACE_UNSET);

	const Protocol::RemoteFace remote{ 3, face };
	const auto remoteBack = Protocol::DecodeRemoteFace(Protocol::Encode(remote));
	REQUIRE(remoteBack);
	CHECK(*remoteBack == remote);
	// Each decoder reads only its own message.
	CHECK(!Protocol::DecodeRemoteFace(packet).has_value());

	// A face with nothing (no parts, no tints) is a face too.
	const Protocol::PlayerFace bare{ .race = NORD };
	const auto bareBack = Protocol::DecodePlayerFace(Protocol::Encode(bare));
	REQUIRE(bareBack);
	CHECK(*bareBack == bare);
}

TEST("faces: a face at every limit is the most bytes a face can be, and more parts or tints than that are cut, not sent")
{
	auto face = FaceOf();
	face.headParts.clear();
	for (std::uint32_t i = 0; i < Protocol::MAX_FACE_HEAD_PARTS + 8; ++i) {
		face.headParts.push_back(0x00050000 + i);
	}
	face.tints.clear();
	for (std::uint16_t i = 0; i < Protocol::MAX_FACE_TINTS + 8; ++i) {
		face.tints.push_back({ .index = i, .path = 0x1000u + i, .color = 0xFF00FF00, .value = 100, .type = 14 });
	}
	const auto packet = Protocol::Encode(face);
	CHECK(packet.size() == Protocol::MAX_FACE_SIZE);
	CHECK(Protocol::Encode(Protocol::RemoteFace{ 9, face }).size() == Protocol::MAX_FACE_SIZE + 4);
	const auto back = Protocol::DecodePlayerFace(packet);
	REQUIRE(back);
	CHECK(back->headParts.size() == Protocol::MAX_FACE_HEAD_PARTS);
	CHECK(back->tints.size() == Protocol::MAX_FACE_TINTS);
	CHECK(back->headParts.back() == 0x00050000 + Protocol::MAX_FACE_HEAD_PARTS - 1);
}

TEST("faces: a face no game could have is not read")
{
	const auto good = Protocol::Encode(FaceOf());
	REQUIRE(Protocol::DecodePlayerFace(good));
	const auto refused = [](const Protocol::PlayerFace& a_face) { return !Protocol::DecodePlayerFace(Protocol::Encode(a_face)).has_value(); };

	// Cut short, or a byte too many.
	auto shorter = good;
	shorter.pop_back();
	CHECK(!Protocol::DecodePlayerFace(shorter));
	auto longer = good;
	longer.push_back(0);
	CHECK(!Protocol::DecodePlayerFace(longer));

	// A sex past female, a part count or a tint count past the limit.
	auto sex = good;
	sex[5] = 2;
	CHECK(!Protocol::DecodePlayerFace(sex));
	auto parts = good;
	parts[6] = static_cast<std::uint8_t>(Protocol::MAX_FACE_HEAD_PARTS + 1);
	CHECK(!Protocol::DecodePlayerFace(parts));
	auto tints = good;
	REQUIRE(tints[TintCountAt(3)] == 2);  // where the test thinks the count is
	tints[TintCountAt(3)] = static_cast<std::uint8_t>(Protocol::MAX_FACE_TINTS + 1);
	CHECK(!Protocol::DecodePlayerFace(tints));

	// A kind of head part no game knows.
	auto kinds = FaceOf();
	kinds.partTypes = 0x80;
	CHECK(refused(kinds));

	// A height that is no number or no race's, a slider that is no number.
	for (const float height : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.1f, 5.0f }) {
		auto tall = FaceOf();
		tall.height = height;
		CHECK(refused(tall));
	}
	for (const float morph : { std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity() }) {
		auto odd = FaceOf();
		odd.morphs[3] = morph;
		CHECK(refused(odd));
	}

	// A preset part the engine could not index; none (-1) and not set are fine.
	auto preset = FaceOf();
	preset.presets[0] = 64;
	CHECK(refused(preset));
	preset.presets[0] = 0xFFFFFFFE;  // -2
	CHECK(refused(preset));
	preset.presets[0] = 63;
	CHECK(!refused(preset));
	CHECK(Protocol::FacePresetOk(0xFFFFFFFF));
	CHECK(Protocol::FacePresetOk(Protocol::FACE_UNSET));

	// A tint laid on past full, or of a kind no game knows.
	auto strong = FaceOf();
	strong.tints[1].value = 101;
	CHECK(refused(strong));
	auto kind = FaceOf();
	kind.tints[1].type = Protocol::TINT_TYPES;
	CHECK(refused(kind));
}

// ---- forms ----

TEST("faces: a face's race, parts, hair colour, complexion and skin reach the other game as its own IDs; those it lacks are left out or none")
{
	// The guest lacks Mine.esp and MyLight.esl; the session's table has them (the host brought them).
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Mine.esp", "Shared.esp" }, { "MyLight.esl" });
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper guestEdge(guest, session);

	auto face = FaceOf();
	face.race = 0x02000D62;                                              // Shared.esp's race: 01 in the guest's game
	face.headParts = { 0x00051622, 0x01000900, 0x02000901, 0xFE000801 };  // Skyrim's, Mine's, Shared's, MyLight's
	face.hairColor = 0x02000A00;
	face.faceDetails = 0x01000B00;  // Mine.esp's complexion
	face.skin = 0x00013000;
	auto translated = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(Protocol::RemoteFace{ 4, face }));
	REQUIRE(translated.kind == FormTranslate::Result::Kind::kChanged);
	const auto in = Protocol::DecodeRemoteFace(translated.packet);
	REQUIRE(in);
	CHECK(in->playerId == 4u);
	CHECK(in->face.race == 0x01000D62u);
	CHECK(in->face.headParts == std::vector<std::uint32_t>({ 0x00051622, 0x01000901 }));
	CHECK(in->face.partTypes == face.partTypes);  // the kinds the friend has: the stand-in keeps its preset's where one is left out
	CHECK(in->face.hairColor == 0x01000A00u);
	CHECK(in->face.hairRgb == face.hairRgb);  // the colour itself, whatever came of its form
	CHECK(in->face.faceDetails == 0u);
	CHECK(in->face.skin == 0x00013000u);
	CHECK(in->face.morphs == face.morphs);
	CHECK(in->face.tints == face.tints);
	CHECK(translated.fields == 1u);
	CHECK(translated.entries == 2u);

	// A race the other game lacks reads none: that face goes on no stand-in (FaceRules::Fits).
	auto foreign = face;
	foreign.race = 0x01000D62;
	const auto lost = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(Protocol::RemoteFace{ 4, foreign }));
	const auto lostIn = Protocol::DecodeRemoteFace(lost.kind == FormTranslate::Result::Kind::kChanged ? lost.packet : std::vector<std::uint8_t>{});
	REQUIRE(lostIn);
	CHECK(lostIn->face.race == 0u);
	CHECK(FaceRules::Fits(lostIn->face, NORD, false, false, false) == Fit::kNoRace);

	// Going out, a runtime form (a colour a barber made) is none, the rest mapped.
	const FormIds::Mapper hostEdge(host, Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {}));
	auto ours = FaceOf();
	ours.hairColor = 0xFF000812;
	ours.headParts = { 0x00051622, 0x02000901, 0xFF000813 };
	const auto out = FormTranslate::Translate(hostEdge, FormTranslate::Way::kToSession, Protocol::Encode(ours));
	REQUIRE(out.kind == FormTranslate::Result::Kind::kChanged);
	const auto sent = Protocol::DecodePlayerFace(out.packet);
	REQUIRE(sent);
	CHECK(sent->hairColor == 0u);
	CHECK(sent->headParts == std::vector<std::uint32_t>({ 0x00051622, 0x01000901 }));
	CHECK(out.runtime == 2u);
}

// ---- the server ----

TEST("server: a player's face is passed on to the others, reliably, not back to its sender; the same again is not")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u, 3u }) {
		(void)f.fake->Take(peer);
	}

	const auto face = FaceOf();
	f.fake->Deliver(2, Protocol::Encode(face));
	f.fake->Deliver(2, Protocol::Encode(face));
	auto changed = face;
	changed.morphs[0] = 0.75f;
	f.fake->Deliver(2, Protocol::Encode(changed));
	auto bad = Protocol::Encode(changed);
	bad.pop_back();
	f.fake->Deliver(2, bad);
	REQUIRE(f.fake->Settle());

	for (const PeerId peer : { 1u, 3u }) {
		const auto sent = SentFaces(f, peer);
		REQUIRE(sent.size() == 2);
		CHECK(sent[0].second);
		CHECK(sent[0].first.playerId == 2);
		CHECK(sent[0].first.face == face);
		CHECK(sent[1].first.face == changed);
	}
	CHECK(SentFaces(f, 2).empty());
}

TEST("server: a player who joins later is told each player's newest face, after that player's join")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	auto face = FaceOf();
	f.fake->Deliver(1, Protocol::Encode(face));
	face.hairRgb = Protocol::PackColor(1, 2, 3, 255);
	f.fake->Deliver(1, Protocol::Encode(face));
	REQUIRE(f.fake->Settle());
	// Bo sends none: nothing is made up for them.

	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto sent = f.fake->TakeSent(3);
	std::optional<std::size_t> joinedAt;
	std::optional<std::size_t> faceAt;
	std::size_t                faces = 0;
	for (std::size_t i = 0; i < sent.size(); ++i) {
		const auto type = Protocol::PeekType(sent[i].data);
		if (const auto joined = type == MT::kPlayerJoined ? Protocol::DecodePlayerJoined(sent[i].data) : std::nullopt; joined && joined->playerId == 1) {
			joinedAt = i;
		}
		if (const auto told = type == MT::kRemoteFace ? Protocol::DecodeRemoteFace(sent[i].data) : std::nullopt) {
			++faces;
			faceAt = i;
			CHECK(told->playerId == 1);
			CHECK(told->face == face);
			CHECK(sent[i].reliable);
		}
	}
	REQUIRE(joinedAt);
	REQUIRE(faceAt);
	CHECK(*faceAt > *joinedAt);
	CHECK(faces == 1u);
}

TEST("server: a burst of faces is held back past a few, and the newest still reaches everyone")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	constexpr int BURST = 12;
	auto          last = FaceOf();
	for (int i = 0; i < BURST; ++i) {
		last.morphs[1] = 0.01f * static_cast<float>(i);
		f.fake->Deliver(2, Protocol::Encode(last));
	}
	REQUIRE(f.fake->Settle());
	auto sent = SentFaces(f, 1);
	CHECK(sent.size() >= 4u);
	CHECK(sent.size() < static_cast<std::size_t>(BURST));
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while ((sent.empty() || sent.back().first.face != last) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(20ms);
		for (auto& more : SentFaces(f, 1)) {
			sent.push_back(std::move(more));
		}
	}
	REQUIRE(!sent.empty());
	CHECK(sent.back().first.face == last);
	CHECK(sent.back().second);
	CHECK(sent.size() <= 6u);
}

// ---- the rules ----

TEST("face rules: a tint mask's texture is named the same whatever its case, slashes or a leading textures folder")
{
	const auto plain = FaceRules::PathHash("Actors\\Character\\Character Assets\\TintMasks\\SkinTone.dds");
	CHECK(plain != 0u);
	CHECK(FaceRules::PathHash("actors/character/character assets/tintmasks/skintone.dds") == plain);
	CHECK(FaceRules::PathHash("Textures\\Actors\\Character\\Character Assets\\TintMasks\\SkinTone.dds") == plain);
	CHECK(FaceRules::PathHash("\\textures/Actors\\Character\\Character Assets\\TintMasks\\SKINTONE.DDS") == plain);
	CHECK(FaceRules::PathHash("Actors\\Character\\Character Assets\\TintMasks\\SkinTone2.dds") != plain);
	CHECK(FaceRules::PathHash("") == 0u);
	CHECK(FaceRules::PathHash("Textures\\") == 0u);
}

TEST("face rules: a tint layer goes on this race's mask of the same texture, at its own index first, and never on another texture's")
{
	const std::vector<FaceRules::RaceTint> race{ { 0, 0xA }, { 1, 0xB }, { 2, 0xC }, { 3, 0xD }, { 4, 0xD } };
	const auto                             maskOf = [&](std::uint16_t a_index, std::uint32_t a_path) {
        return FaceRules::TintIndexFor({ .index = a_index, .path = a_path }, race);
	};
	CHECK(maskOf(1, 0xB) == std::optional<std::uint16_t>{ 1 });
	CHECK(maskOf(7, 0xC) == std::optional<std::uint16_t>{ 2 });  // a race a mod renumbered
	CHECK(maskOf(1, 0xC) == std::optional<std::uint16_t>{ 2 });  // index 1 is another texture here
	CHECK(maskOf(4, 0xD) == std::optional<std::uint16_t>{ 4 });  // two masks of one texture: its own index
	CHECK(maskOf(9, 0xD) == std::optional<std::uint16_t>{ 3 });
	CHECK(!maskOf(1, 0xE));  // a tint mod's mask this game lacks: left out
	CHECK(!maskOf(0, 0));
}

TEST("face rules: a stand-in's head has its friend's parts, and its preset's only for a kind whose part this game lacks")
{
	using Part = FaceRules::Part;
	// The preset: a head (1), eyes (2), a hair (3), a beard (4), brows (6).
	const std::vector<Part> preset{ { 100, 1 }, { 101, 2 }, { 102, 3 }, { 104, 4 }, { 106, 6 } };
	// The friend has a head, eyes, a hair and brows; only their eyes and hair are in this game (a mod's head and brows).
	const std::vector<Part> came{ { 10, 2 }, { 20, 3 } };
	const auto types = static_cast<std::uint8_t>(FaceRules::TypeBit(1) | FaceRules::TypeBit(2) | FaceRules::TypeBit(3) | FaceRules::TypeBit(6));
	const auto plan = FaceRules::PlanParts(came, types, preset);
	CHECK(plan.ids == std::vector<std::uint32_t>({ 10, 20, 100, 106 }));  // no beard: the friend has none
	CHECK(plan.fellBack == static_cast<std::uint8_t>(FaceRules::TypeBit(1) | FaceRules::TypeBit(6)));

	// All of them here: nothing of the preset's.
	const auto whole = FaceRules::PlanParts(std::vector<Part>{ { 10, 2 }, { 20, 3 } }, static_cast<std::uint8_t>(FaceRules::TypeBit(2) | FaceRules::TypeBit(3)), preset);
	CHECK(whole.ids == std::vector<std::uint32_t>({ 10, 20 }));
	CHECK(whole.fellBack == 0);

	// None twice, none of a kind no game knows, none that is no form, and no more than a face carries.
	const auto odd = FaceRules::PlanParts(std::vector<Part>{ { 101, 2 }, { 101, 2 }, { 55, 9 }, { 0, 3 } }, static_cast<std::uint8_t>(FaceRules::TypeBit(2) | FaceRules::TypeBit(1)), preset);
	CHECK(odd.ids == std::vector<std::uint32_t>({ 101, 100 }));
	std::vector<Part> many;
	for (std::uint32_t i = 0; i < 40; ++i) {
		many.push_back({ 1000 + i, 0 });
	}
	CHECK(FaceRules::PlanParts(many, FaceRules::TypeBit(0), preset).ids.size() == Protocol::MAX_FACE_HEAD_PARTS);
	CHECK(FaceRules::TypesOf(preset) == static_cast<std::uint8_t>(0b1011110));
}

TEST("face rules: a face goes on a stand-in only when it is for that preset's race and sex, until given up or the copies run out")
{
	const auto face = FaceOf(NORD, true);
	CHECK(FaceRules::Fits(std::nullopt, NORD, true, false, false) == Fit::kNoFace);
	CHECK(FaceRules::Fits(face, NORD, true, false, false) == Fit::kOwn);
	CHECK(FaceRules::Fits(face, IMPERIAL, true, false, false) == Fit::kOtherRace);  // their looks changed race, their face not yet
	CHECK(FaceRules::Fits(face, NORD, false, false, false) == Fit::kOtherSex);
	CHECK(FaceRules::Fits(face, NORD, true, true, false) == Fit::kGivenUp);
	CHECK(FaceRules::Fits(face, NORD, true, false, true) == Fit::kFull);
	auto none = face;
	none.race = 0;
	CHECK(FaceRules::Fits(none, NORD, true, false, false) == Fit::kNoRace);
	none.race = 0xFF000801;  // a runtime race (one that got through): another game's number
	CHECK(FaceRules::Fits(none, 0xFF000801, true, false, false) == Fit::kNoRace);
	CHECK(FaceRules::FitName(Fit::kOwn) == "own");
	CHECK(FaceRules::FitName(Fit::kFull) == "full");
	CHECK(FaceRules::FitName(Fit::kCount) == "?");
}

TEST("face rules: a face's fingerprint is the same for the same face, and another for any change")
{
	const auto face = FaceOf();
	CHECK(FaceRules::Hash(face) == FaceRules::Hash(FaceOf()));
	CHECK(FaceRules::Hash(face) != 0u);
	auto slider = face;
	slider.morphs[18] += 0.01f;
	CHECK(FaceRules::Hash(slider) != FaceRules::Hash(face));
	auto tint = face;
	tint.tints[1].value = 59;
	CHECK(FaceRules::Hash(tint) != FaceRules::Hash(face));
	auto hair = face;
	hair.hairRgb ^= 1;
	CHECK(FaceRules::Hash(hair) != FaceRules::Hash(face));
}

TEST("face rules: our face goes at once the first time, and a change only once it holds still for two reads")
{
	FaceRules::Settle                   settle;
	std::optional<Protocol::PlayerFace> sent;
	const auto                          a = FaceOf();
	auto                                b = a;
	b.morphs[0] = 0.3f;
	auto c = a;
	c.morphs[0] = 0.6f;

	const auto first = settle.Next(a, sent);
	REQUIRE(first);
	CHECK(*first == a);
	sent = first;
	CHECK(!settle.Next(a, sent));  // nothing new
	CHECK(!settle.Next(b, sent));  // a slider moving
	CHECK(!settle.Next(c, sent));  // still moving
	const auto stopped = settle.Next(c, sent);
	REQUIRE(stopped);
	CHECK(*stopped == c);
	sent = stopped;
	CHECK(!settle.Next(c, sent));

	// Back where it was sent, mid-drag: nothing goes.
	CHECK(!settle.Next(b, sent));
	CHECK(!settle.Next(c, sent));

	// A new connection: the first read goes at once again.
	settle.Reset();
	CHECK(settle.Next(b, std::nullopt).has_value());
}

TEST("face rules: nothing goes while a face editor is open, and the face as it was left goes once it closes")
{
	FaceRules::Settle                   settle;
	std::optional<Protocol::PlayerFace> sent = FaceOf();
	CHECK(!settle.Next(*sent, sent));

	// Two minutes in a race menu, a slider stopping on a new value every few reads: not one goes.
	std::size_t gone = 0;
	auto        face = *sent;
	for (int i = 0; i < 120; ++i) {
		face.morphs[i % Protocol::FACE_MORPHS] = static_cast<float>(i % 7) * 0.1f;
		for (int read = 0; read < 3; ++read) {
			gone += settle.Next(face, sent, true).has_value() ? 1 : 0;
		}
	}
	CHECK(gone == 0u);
	CHECK(settle.Editing());

	// Closed: the first read sends the face as it was left, at once (no second read waited for), and only once.
	const auto closed = settle.Next(face, sent, false);
	REQUIRE(closed);
	CHECK(*closed == face);
	CHECK(!settle.Editing());
	sent = closed;
	CHECK(!settle.Next(face, sent, false));
	CHECK(!settle.Next(face, sent, false));

	// Opened and closed with nothing changed: nothing goes.
	CHECK(!settle.Next(face, sent, true));
	CHECK(!settle.Next(face, sent, false));

	// A first face of the session while an editor is open waits for it to close too.
	FaceRules::Settle fresh;
	CHECK(!fresh.Next(face, std::nullopt, true));
	CHECK(fresh.Next(face, std::nullopt, false).has_value());
}

TEST("face rules: a copy holds one friend's face; the same face is placed on it again, a new one goes into a copy no actor stands on")
{
	constexpr std::uintptr_t NORD_M = 0x10;
	constexpr std::uintptr_t NORD_F = 0x20;
	std::vector<std::uintptr_t> used;
	const FaceRules::CopyBook::InUse inUse = [&](std::uintptr_t a_copy) { return std::ranges::find(used, a_copy) != used.end(); };

	FaceRules::CopyBook book;
	CHECK(!book.Find(1, 0xAA));
	CHECK(!book.Free(NORD_M, inUse));
	CHECK(book.CanMake());
	book.Made(1, 0xAA, NORD_M, 0x1000);
	book.Made(2, 0xAA, NORD_M, 0x2000);
	used = { 0x1000, 0x2000 };
	CHECK(book.Find(1, 0xAA) == std::optional<std::uintptr_t>{ 0x1000 });
	CHECK(book.Find(2, 0xAA) == std::optional<std::uintptr_t>{ 0x2000 });
	CHECK(!book.Find(1, 0xBB));
	CHECK(book.IsCopy(0x1000));
	CHECK(!book.IsCopy(0x3000));
	CHECK(!book.IsCopy(0));
	CHECK(!book.Free(NORD_M, inUse));  // both stood on

	// Player 1's face changed: their old stand-in is still on its way out (its copy in use), so a new copy is made.
	CHECK(!book.Free(NORD_M, inUse));
	book.Made(1, 0xBB, NORD_M, 0x1100);
	used = { 0x1000, 0x1100, 0x2000 };
	CHECK(book.Find(1, 0xBB) == std::optional<std::uintptr_t>{ 0x1100 });
	CHECK(book.Find(1, 0xAA) == std::optional<std::uintptr_t>{ 0x1000 });  // still holds it, until written again
	CHECK(book.Made() == 3u);

	// The old stand-in gone: its copy is free, for a copy of the same preset only.
	used = { 0x1100, 0x2000 };
	CHECK(!book.Free(NORD_F, inUse));
	REQUIRE(book.Free(NORD_M, inUse) == std::optional<std::uintptr_t>{ 0x1000 });
	book.Rewritten(0x1000, 1, 0xCC);
	CHECK(book.Find(1, 0xCC) == std::optional<std::uintptr_t>{ 0x1000 });
	CHECK(!book.Find(1, 0xAA));  // written over
	CHECK(book.Made() == 3u);
	CHECK(book.Rewrites() == 1u);
	CHECK(book.Kept() == 3u);

	// Of two free copies, the one written longest ago.
	used = {};
	CHECK(book.Free(NORD_M, inUse) == std::optional<std::uintptr_t>{ 0x2000 });
	// None in use given: any copy of the preset is free.
	CHECK(book.Free(NORD_M, nullptr).has_value());

	// A load: none placed again or written into; what was made still counts.
	book.Forget();
	CHECK(!book.Find(1, 0xCC));
	CHECK(!book.IsCopy(0x2000));
	CHECK(!book.Free(NORD_M, inUse));
	CHECK(book.Kept() == 0u);
	CHECK(book.Made() == 3u);
}

TEST("face rules: a friend editing their face for a long while makes a few copies, never the game run's 64")
{
	// The game's way, as RemotePlayers and Face do it: each new face places the stand-in again; the one taken away stays
	// in use until its 3D is gone and it is deleted (about two seconds), so it overlaps the new one by a face or two.
	constexpr std::uintptr_t PRESET = 0x10;
	FaceRules::CopyBook      book;
	std::vector<std::uintptr_t> standing;  // the stand-in's copy, then those of the stand-ins on their way out, newest first
	std::uintptr_t           next = 0x1000;
	const FaceRules::CopyBook::InUse inUse = [&](std::uintptr_t a_copy) { return std::ranges::find(standing, a_copy) != standing.end(); };
	for (std::uint32_t change = 1; change <= 500; ++change) {
		std::uintptr_t copy = 0;
		if (const auto found = book.Find(7, change)) {
			copy = *found;
		} else if (const auto free = book.Free(PRESET, inUse)) {
			copy = *free;
			book.Rewritten(copy, 7, change);
		} else {
			REQUIRE(book.CanMake());
			copy = next++;
			book.Made(7, change, PRESET, copy);
		}
		standing.insert(standing.begin(), copy);
		if (standing.size() > 3) {
			standing.pop_back();  // the oldest one on its way out is deleted
		}
	}
	CHECK(book.Made() <= 4u);
	CHECK(book.CanMake());
	CHECK(book.Rewrites() >= 496u);

	// A second friend editing at the same time: a few more, still far from the limit.
	for (std::uint32_t change = 1; change <= 500; ++change) {
		if (!book.Find(8, change)) {
			if (const auto free = book.Free(PRESET, inUse)) {
				book.Rewritten(*free, 8, change);
				standing.insert(standing.begin(), *free);
			} else {
				book.Made(8, change, PRESET, next);
				standing.insert(standing.begin(), next++);
			}
		}
		if (standing.size() > 6) {
			standing.pop_back();
		}
	}
	CHECK(book.Made() <= 7u);
	CHECK(book.CanMake());
}

TEST("face rules: a game run makes at most 64 copies, then a stand-in that needs a new one gets its preset's face")
{
	FaceRules::CopyBook book;
	const FaceRules::CopyBook::InUse all = [](std::uintptr_t) { return true; };  // every one stood on
	for (std::size_t i = 0; i < FaceRules::CopyBook::MAX_MADE; ++i) {
		CHECK(book.CanMake());
		CHECK(!book.Free(0x10, all));
		book.Made(static_cast<std::uint32_t>(i), 0xAA, 0x10, 0x9000 + i);
	}
	CHECK(!book.CanMake());
	CHECK(Fit::kFull == FaceRules::Fits(FaceOf(), NORD, false, false, !book.CanMake()));
}

TEST("face rules: a stand-in with no head is built again once, then its friend's face is given up; one with a head is left")
{
	using Facts = FaceRules::Watch::Facts;
	const auto t0 = FaceRules::Clock::now();
	const Facts headless{ .loaded = true, .head = false, .oursLoaded = true, .oursHead = true, .oursJudged = true };
	const Facts headed{ .loaded = true, .head = true, .oursLoaded = true, .oursHead = true, .oursJudged = true };
	const Facts unloaded{};

	FaceRules::Watch good;
	CHECK(good.Next(unloaded, t0) == Step::kNone);
	CHECK(good.State() == "waiting");
	CHECK(good.Next(headed, t0 + 1s) == Step::kBuilt);
	CHECK(good.Next(headed, t0 + 10s) == Step::kNone);
	CHECK(good.Built());
	CHECK(good.State() == "built");

	FaceRules::Watch bad;
	CHECK(bad.Next(headless, t0) == Step::kNone);
	CHECK(bad.State() == "nohead");
	CHECK(bad.Next(headless, t0 + 2s) == Step::kNone);
	CHECK(bad.Next(headless, t0 + 3s) == Step::kRebuild);
	CHECK(bad.Rebuilt());
	CHECK(bad.Next(headless, t0 + 5s) == Step::kNone);
	CHECK(bad.Next(headless, t0 + 6s) == Step::kGiveUp);
	CHECK(bad.Next(headless, t0 + 60s) == Step::kNone);
	CHECK(bad.State() == "done");

	// Built again and then a head: kept.
	FaceRules::Watch late;
	(void)late.Next(headless, t0);
	CHECK(late.Next(headless, t0 + 3s) == Step::kRebuild);
	CHECK(late.Next(headed, t0 + 4s) == Step::kBuilt);
	CHECK(late.Next(headless, t0 + 20s) == Step::kNone);  // a head lost later (a cell's 3D going) starts a new wait
	CHECK(late.Next(headless, t0 + 23s) == Step::kGiveUp);  // already built again once

	// Its 3D unloaded (far away, a loading screen) restarts the wait.
	FaceRules::Watch away;
	(void)away.Next(headless, t0);
	CHECK(away.Next(unloaded, t0 + 2s) == Step::kNone);
	CHECK(away.Next(headless, t0 + 4s) == Step::kNone);
	CHECK(away.Next(headless, t0 + 6900ms) == Step::kNone);
	CHECK(away.Next(headless, t0 + 7s) == Step::kRebuild);
}

TEST("face rules: our own head gone while a stand-in stands on a copy gives its face up; ours away loading does not")
{
	using Facts = FaceRules::Watch::Facts;
	const auto t0 = FaceRules::Clock::now();
	const Facts oursGone{ .loaded = true, .head = true, .oursLoaded = true, .oursHead = false, .oursJudged = true };
	const Facts oursAway{ .loaded = true, .head = true, .oursLoaded = false, .oursHead = false, .oursJudged = true };
	const Facts fine{ .loaded = true, .head = true, .oursLoaded = true, .oursHead = true, .oursJudged = true };

	FaceRules::Watch watch;
	CHECK(watch.Next(oursGone, t0) == Step::kBuilt);
	CHECK(watch.Next(oursGone, t0 + 1s) == Step::kNone);
	CHECK(watch.Next(oursGone, t0 + 2s) == Step::kGiveUpOurs);
	CHECK(watch.Next(oursGone, t0 + 3s) == Step::kNone);

	FaceRules::Watch blink;  // gone for a moment (our 3D rebuilt by an armour change): kept
	(void)blink.Next(fine, t0);
	CHECK(blink.Next(oursGone, t0 + 1s) == Step::kNone);
	CHECK(blink.Next(fine, t0 + 2s) == Step::kNone);
	CHECK(blink.Next(oursGone, t0 + 3s) == Step::kNone);
	CHECK(blink.Next(oursGone, t0 + 4500ms) == Step::kNone);

	FaceRules::Watch loading;
	for (int i = 0; i < 10; ++i) {
		CHECK(loading.Next(oursAway, t0 + std::chrono::seconds(i)) != Step::kGiveUpOurs);
	}
}

TEST("face rules: our head that can't be judged (a werewolf, a vampire lord, first person, a face editor) never gives a face up")
{
	using Facts = FaceRules::Watch::Facts;
	const auto t0 = FaceRules::Clock::now();
	// Loaded, no face node, and not judged: what OurHead says in a beast form (a race it never had a head in), in first
	// person, or with the race menu open.
	const Facts unjudged{ .loaded = true, .head = true, .oursLoaded = true, .oursHead = false, .oursJudged = false };
	const Facts gone{ .loaded = true, .head = true, .oursLoaded = true, .oursHead = false, .oursJudged = true };

	FaceRules::Watch beast;
	for (int i = 0; i < 600; ++i) {
		CHECK(beast.Next(unjudged, t0 + std::chrono::seconds(i)) != Step::kGiveUpOurs);
	}
	CHECK(beast.State() == "built");

	// Back to its own race (or third person): the wait starts then, not from when the face node went.
	FaceRules::Watch back;
	(void)back.Next(unjudged, t0);
	CHECK(back.Next(unjudged, t0 + 30s) == Step::kNone);
	CHECK(back.Next(gone, t0 + 31s) == Step::kNone);
	CHECK(back.Next(gone, t0 + 32s) == Step::kNone);
	CHECK(back.Next(unjudged, t0 + 32500ms) == Step::kNone);  // first person again: the count stops
	CHECK(back.Next(gone, t0 + 34s) == Step::kNone);
	CHECK(back.Next(gone, t0 + 36s) == Step::kGiveUpOurs);
}

TEST("face rules: a stand-in's built head is judged from its 3D: the copy's parts there, none of the preset's, its hair's tint")
{
	FaceRules::Head3D head{ .face = true, .parts = 5, .partsFound = 5, .presetLeft = 2, .presetFound = 0, .hairWanted = true, .hairSeen = true, .hairSame = true };
	CHECK(head.All());
	CHECK(FaceRules::Line(head) == "shown3d=1 face3d=1 parts3d=5/5 presetShown=0/2 hairTint=1 others3d=0");

	auto preset = head;
	preset.presetFound = 2;  // the preset's face built
	CHECK(!preset.All());
	auto missing = head;
	missing.partsFound = 4;
	CHECK(!missing.All());
	auto unseen = head;
	unseen.hairSeen = false;
	unseen.hairSame = false;
	CHECK(!unseen.All());
	CHECK(FaceRules::Line(unseen).find("hairTint=unseen") != std::string::npos);
	auto bald = head;
	bald.hairWanted = false;
	bald.hairSeen = false;
	CHECK(bald.All());
	CHECK(!FaceRules::Head3D{}.All());  // no face node
	auto others = head;
	others.others = 2;
	others.othersNames = "Overlay0,Overlay1";
	CHECK(others.All());  // said, not judged
	CHECK(FaceRules::Line(others).ends_with("others3d=2(Overlay0,Overlay1)"));

	// The tint as the engine keeps it (0-1 a channel), or at the library's scale (0-2): the same colour either way.
	const auto rgb = Protocol::PackColor(0x40, 0x20, 0x10, 0xFF);
	CHECK(FaceRules::HairTintSame(rgb, 0x40 / 255.0f, 0x20 / 255.0f, 0x10 / 255.0f));
	CHECK(FaceRules::HairTintSame(rgb, 0x40 / 128.0f, 0x20 / 128.0f, 0x10 / 128.0f));
	CHECK(FaceRules::HairTintSame(rgb, 0x41 / 255.0f, 0x1F / 255.0f, 0x10 / 255.0f));
	CHECK(!FaceRules::HairTintSame(rgb, 0x60 / 255.0f, 0x20 / 255.0f, 0x10 / 255.0f));
	CHECK(!FaceRules::HairTintSame(rgb, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
}

TEST("face rules: what a stand-in's copy holds is compared with what came, piece by piece")
{
	const auto sent = FaceOf();
	const auto same = FaceRules::Compare(sent, sent);
	CHECK(same.All());
	CHECK(FaceRules::Line(same) == "parts=3/3 morphs=19 presets=4 tints=2/2 hair=1 height=1 body=1 details=1 skin=1");

	auto shown = sent;
	shown.headParts = { 0x00051622, 0x000E0EC7, 0x00099999 };  // the eyes are the preset's
	shown.morphs[2] = 0.9f;
	shown.tints[1].value = 30;
	shown.hairRgb = 0;
	const auto off = FaceRules::Compare(sent, shown);
	CHECK(!off.All());
	CHECK(FaceRules::Line(off) == "parts=2/3 morphs=18 presets=4 tints=1/2 hair=0 height=1 body=1 details=1 skin=1");

	// A complexion or skin the sender had none of (or that this game lacked) passes whatever the copy keeps.
	auto none = sent;
	none.faceDetails = 0;
	none.skin = 0;
	auto kept = sent;
	kept.faceDetails = 0x00011111;
	kept.skin = 0x00022222;
	CHECK(FaceRules::Compare(none, kept).All());
	CHECK(!FaceRules::Compare(sent, kept).All());
}
