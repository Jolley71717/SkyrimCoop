#include "game/Effects.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/EffectRules.h"
#include "game/NpcSync.h"
#include "game/RemotePlayers.h"

#include <array>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

// The rules' numbers are the game's.
static_assert(EffectRules::kSlowTime == std::to_underlying(RE::EffectArchetypes::ArchetypeID::kSlowTime));
static_assert(EffectRules::kStagger == std::to_underlying(RE::EffectArchetypes::ArchetypeID::kStagger));
static_assert(EffectRules::kParalysis == std::to_underlying(RE::EffectArchetypes::ArchetypeID::kParalysis));
static_assert(EffectRules::kVampireLord == std::to_underlying(RE::EffectArchetypes::ArchetypeID::kVampireLord));
static_assert(EffectRules::kAbsorb == std::to_underlying(RE::EffectArchetypes::ArchetypeID::kAbsorb));
static_assert(EffectRules::AV_HEALTH == std::to_underlying(RE::ActorValue::kHealth));
static_assert(Protocol::MAX_CASTING_SOURCE == std::to_underlying(RE::MagicSystem::CastingSource::kInstant));

namespace Effects
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::atomic<bool> active{ false };
		std::uint32_t     ourId = 0;
		thread_local bool applying = false;

		// What the hook noted (any thread), for the next tick to send.
		struct Noted
		{
			RE::FormID    target = 0;  // the copy's actor here
			std::uint32_t item = 0;
			std::uint8_t  index = 0;
			std::uint32_t source = 0;
			float         magnitude = 0;
			float         power = 1;
			std::uint8_t  castingSource = 0;
			std::uint8_t  flags = 0;
			std::int32_t  archetype = EffectRules::kArchNone;
			EffectRules::Route route = EffectRules::Route::kRunner;
		};
		std::mutex                       notedLock;
		std::vector<Noted>               noted;
		std::optional<Protocol::SlowTime> slowNoted;
		constexpr std::size_t            MAX_NOTED = 128;

		EffectRules::Repeats<RE::FormID> repeats;  // main thread

		struct Counts
		{
			// Ours: held back (refused here) and sent, by route; noted but not sent (no key, not another game's now,
			// repeats); refused by the server's budget is the server's to say.
			std::uint64_t heldBack = 0, both = 0, sent = 0, noKey = 0, notOthers = 0, repeats = 0, overflow = 0, noIndex = 0;
			// Theirs: put on, refused by the game, and why not put on.
			std::uint64_t heard = 0, applied = 0, refusedByGame = 0, notOurs = 0, noActor = 0, dead = 0, noItem = 0, noStandIn = 0;
			// Slow Time: ours sent; theirs shared, too far, elsewhere, refused.
			std::uint64_t slowSent = 0, slowHeard = 0, slowShared = 0, slowFar = 0, slowElsewhere = 0, slowRefused = 0;
		};
		Counts counts;
		std::array<std::atomic<std::uint64_t>, 48> heldByKind{};  // our effects held back here, by archetype (0-46)
		std::array<std::uint64_t, 48>              appliedByKind{};
		std::string                                lastSent;
		std::string                                lastApplied;

		std::size_t KindSlot(std::int32_t a_archetype)
		{
			return a_archetype >= 0 && a_archetype < 47 ? static_cast<std::size_t>(a_archetype) : 47;
		}

		std::string Hex(std::uint32_t a_id) { return std::format("{:08X}", a_id); }

		// Which of its item's effects a_effect is (none: not one of them).
		std::optional<std::uint8_t> IndexOf(const RE::MagicItem* a_item, const RE::Effect* a_effect)
		{
			if (!a_item || !a_effect) {
				return std::nullopt;
			}
			for (std::uint32_t i = 0; i < a_item->effects.size() && i < 0xFF; ++i) {
				if (a_item->effects[i] == a_effect) {
					return static_cast<std::uint8_t>(i);
				}
			}
			return std::nullopt;
		}

		std::uint8_t FlagsOf(const RE::MagicTarget::AddTargetData& a_data)
		{
			return static_cast<std::uint8_t>((a_data.dualCasted ? Protocol::kEffectDual : 0) | (a_data.areaTarget ? Protocol::kEffectArea : 0));
		}

		// The effect a message names, in this game (none: an item or an index it lacks).
		std::pair<RE::MagicItem*, RE::Effect*> EffectOf(std::uint32_t a_item, std::uint8_t a_index)
		{
			const auto form = RE::TESForm::LookupByID(a_item);
			const auto item = form ? form->As<RE::MagicItem>() : nullptr;
			if (!item || a_index >= item->effects.size() || !item->effects[a_index] || !item->effects[a_index]->baseEffect) {
				return { nullptr, nullptr };
			}
			return { item, item->effects[a_index] };
		}

		// Puts an effect on a_target as the game would have, the hook letting it through (Applying).
		bool Apply(RE::Actor* a_target, RE::Actor* a_caster, RE::MagicItem* a_item, RE::Effect* a_effect, std::uint32_t a_source, float a_magnitude, float a_power,
			std::uint8_t a_castingSource, std::uint8_t a_flags)
		{
			const auto target = a_target->AsMagicTarget();
			if (!target) {
				return false;
			}
			RE::MagicTarget::AddTargetData data{};
			data.caster = a_caster;
			data.magicItem = a_item;
			data.effect = a_effect;
			const auto sourceForm = a_source != 0 ? RE::TESForm::LookupByID(a_source) : nullptr;
			data.source = sourceForm ? sourceForm->As<RE::TESBoundObject>() : nullptr;
			data.postCreationCallback = nullptr;
			data.resultsCollector = nullptr;
			// An area's effect pushes from where it went off: the caster here (a cone's, a shout's, from its mouth).
			data.explosionPoint = a_caster ? a_caster->GetPosition() : a_target->GetPosition();
			data.magnitude = a_magnitude;
			data.power = a_power;
			data.castingSource = static_cast<RE::MagicSystem::CastingSource>(a_castingSource);
			data.areaTarget = (a_flags & Protocol::kEffectArea) != 0;
			data.dualCasted = (a_flags & Protocol::kEffectDual) != 0;
			applying = true;
			const bool took = target->AddTarget(data);
			applying = false;
			return took;
		}

		bool SameSpace(RE::Actor* a_a, RE::Actor* a_b)
		{
			const auto worldA = a_a->GetWorldspace();
			const auto worldB = a_b->GetWorldspace();
			if (worldA || worldB) {
				return worldA == worldB;
			}
			return a_a->GetParentCell() && a_a->GetParentCell() == a_b->GetParentCell();
		}
	}

	void Started(std::uint32_t a_ourId)
	{
		ourId = a_ourId;
		active = true;
	}

	void Clear()
	{
		active = false;
		ourId = 0;
		{
			std::scoped_lock l{ notedLock };
			noted.clear();
			slowNoted.reset();
		}
		repeats.Clear();
	}

	bool Applying()
	{
		return applying;
	}

	bool Forwards(RE::Actor* a_caster, RE::Actor* a_target, const RE::MagicTarget::AddTargetData& a_data, std::int32_t a_archetype)
	{
		if (applying || !active.load(std::memory_order_relaxed) || !a_caster || !a_target || a_caster == a_target || !a_caster->IsPlayerRef() || !NpcSync::OthersCopy(a_target)) {
			return false;
		}
		const auto base = a_data.effect ? a_data.effect->baseEffect : nullptr;
		const auto av = base ? static_cast<std::int32_t>(std::to_underlying(base->data.primaryAV)) : -1;
		// A spell, enchantment or poison our player made (a run-time form): no other game has it, taken here as before.
		const bool madeHere = a_data.magicItem && FormIds::IsRuntime(a_data.magicItem->GetFormID());
		const auto route = EffectRules::OnCopy(a_archetype, av, madeHere);
		if (route == EffectRules::Route::kHere) {
			return false;
		}
		const auto index = IndexOf(a_data.magicItem, a_data.effect);
		std::scoped_lock l{ notedLock };
		if (!index) {
			++counts.noIndex;  // an effect not of the item it came with: taken here, as before
			return false;
		}
		if (noted.size() >= MAX_NOTED) {
			++counts.overflow;
			return false;  // a flood in one tick: the rest are taken here, as before
		}
		noted.push_back({ .target = a_target->GetFormID(),
			.item = a_data.magicItem->GetFormID(),
			.index = *index,
			.source = a_data.source ? a_data.source->GetFormID() : 0,
			.magnitude = a_data.magnitude,
			.power = a_data.power,
			.castingSource = static_cast<std::uint8_t>(std::to_underlying(a_data.castingSource)),
			.flags = FlagsOf(a_data),
			.archetype = a_archetype,
			.route = route });
		if (route == EffectRules::Route::kRunner) {
			heldByKind[KindSlot(a_archetype)].fetch_add(1, std::memory_order_relaxed);
			++counts.heldBack;
			return true;
		}
		++counts.both;
		return false;
	}

	void NoteSelf(RE::Actor* a_caster, const RE::MagicTarget::AddTargetData& a_data, std::int32_t a_archetype)
	{
		// The shout's own Slow Time alone (a voice power's), not a perk's, a potion's or a mod's (EffectRules::SharedWithPlayers).
		const auto spell = a_data.magicItem ? a_data.magicItem->As<RE::SpellItem>() : nullptr;
		const bool fromShout = spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower;
		if (applying || !active.load(std::memory_order_relaxed) || !a_caster || !a_caster->IsPlayerRef() || !EffectRules::SharedWithPlayers(a_archetype, fromShout)) {
			return;
		}
		const auto index = IndexOf(a_data.magicItem, a_data.effect);
		if (!index) {
			return;
		}
		std::scoped_lock l{ notedLock };
		slowNoted = Protocol::SlowTime{ .item = a_data.magicItem->GetFormID(), .index = *index, .magnitude = a_data.magnitude, .power = a_data.power };
	}

	void Frame(const SendFn& a_send)
	{
		if (!active) {
			return;
		}
		std::vector<Noted>               ours;
		std::optional<Protocol::SlowTime> slow;
		{
			std::scoped_lock l{ notedLock };
			ours.swap(noted);
			slow.swap(slowNoted);
		}
		const auto now = Clock::now();
		if (slow && !FormIds::IsRuntime(slow->item)) {
			a_send(Protocol::Encode(*slow), true);
			++counts.slowSent;
			logs::info("Effects: our Slow Time ({:08X} #{}, rate {:.2f}) said, for the players near to share", slow->item, slow->index, slow->magnitude);
		}
		Protocol::NpcEffects msg;
		const auto           flush = [&] {
			if (!msg.effects.empty()) {
				counts.sent += msg.effects.size();
				a_send(Protocol::Encode(msg), true);
				msg.effects.clear();
			}
		};
		for (const auto& one : ours) {
			const auto actor = RE::TESForm::LookupByID<RE::Actor>(one.target);
			const auto key = actor ? NpcSync::KeyOf(actor) : std::nullopt;
			if (!key) {
				++counts.noKey;
				continue;
			}
			const auto runner = NpcSync::RunnerOf(*key);
			if (runner == 0 || runner == ourId) {
				++counts.notOthers;  // ours, or nobody's, since the hook asked: what was refused here is lost (said in the counts)
				continue;
			}
			if (!repeats.Due(one.target, one.item, one.index, now)) {
				++counts.repeats;
				continue;
			}
			msg.effects.push_back({ .key = *key,
				.item = one.item,
				.index = one.index,
				.source = one.source,
				.magnitude = Protocol::IsEffectAmount(one.magnitude) ? one.magnitude : 0.0f,
				.power = Protocol::IsEffectAmount(one.power) ? one.power : 1.0f,
				.castingSource = (std::min)(one.castingSource, Protocol::MAX_CASTING_SOURCE),
				.flags = one.flags });
			lastSent = std::format("{:08X} #{} (kind {}, {}) on {:08X} (runner player {})", one.item, one.index, one.archetype, EffectRules::RouteName(one.route), key->id, runner);
			if (msg.effects.size() >= Protocol::MAX_NPC_EFFECTS) {
				flush();
			}
		}
		flush();
	}

	void Heard(const Protocol::NpcEffects& a_msg)
	{
		if (!active || a_msg.caster == ourId) {
			return;
		}
		const auto standIn = RemotePlayers::StandInOf(a_msg.caster);
		for (const auto& effect : a_msg.effects) {
			++counts.heard;
			if (NpcSync::RunnerOf(effect.key) != ourId) {
				++counts.notOurs;  // handed over meanwhile: its new runner's copy was hit, not its NPC
				continue;
			}
			const auto actor = NpcSync::ActorOf(effect.key);
			if (!actor || actor->IsDisabled() || !actor->Is3DLoaded()) {
				++counts.noActor;
				continue;
			}
			if (actor->IsDead()) {
				++counts.dead;
				continue;
			}
			const auto [item, one] = EffectOf(effect.item, effect.index);
			if (!item) {
				++counts.noItem;
				continue;
			}
			if (!standIn) {
				++counts.noStandIn;  // no caster to blame here (its player's stand-in is not placed): not put on
				continue;
			}
			if (Apply(actor.get(), standIn.get(), item, one, effect.source, effect.magnitude, effect.power, effect.castingSource, effect.flags)) {
				++counts.applied;
				const auto kind = static_cast<std::int32_t>(std::to_underlying(one->baseEffect->GetArchetype()));
				++appliedByKind[KindSlot(kind)];
				lastApplied = std::format("{:08X} #{} (kind {}) on {:08X} from player {}", effect.item, effect.index, kind, actor->GetFormID(), a_msg.caster);
			} else {
				++counts.refusedByGame;  // resisted, absorbed, a ward, an immune NPC: the game's own say
			}
		}
	}

	void Heard(const Protocol::SlowTime& a_msg)
	{
		if (!active || a_msg.player == ourId) {
			return;
		}
		++counts.slowHeard;
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto standIn = RemotePlayers::StandInOf(a_msg.player);
		if (!player || !standIn) {
			++counts.slowElsewhere;
			return;
		}
		const bool same = SameSpace(player, standIn.get());
		const auto distance = same ? std::optional{ player->GetPosition().GetDistance(standIn->GetPosition()) } : std::nullopt;
		if (!EffectRules::SharesSlowTime(same, distance)) {
			++(same ? counts.slowFar : counts.slowElsewhere);
			logs::info("Effects: player {}'s Slow Time not shared: {}", a_msg.player, same ? std::format("{:.0f} u away", *distance) : std::string{ "elsewhere" });
			return;
		}
		const auto [item, one] = EffectOf(a_msg.item, a_msg.index);
		if (!item || static_cast<std::int32_t>(std::to_underlying(one->baseEffect->GetArchetype())) != EffectRules::kSlowTime) {
			++counts.slowRefused;
			return;
		}
		if (Apply(player, player, item, one, 0, a_msg.magnitude, a_msg.power, static_cast<std::uint8_t>(std::to_underlying(RE::MagicSystem::CastingSource::kOther)), 0)) {
			++counts.slowShared;
			logs::info("Effects: player {}'s Slow Time shared: the world slows here too (rate {:.2f})", a_msg.player, a_msg.magnitude);
		} else {
			++counts.slowRefused;
		}
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "last") {
			return std::format("sent: {} | put on: {}", lastSent.empty() ? "none" : lastSent, lastApplied.empty() ? "none" : lastApplied);
		}
		std::string held;
		std::string applied;
		for (std::size_t i = 0; i < heldByKind.size(); ++i) {
			if (const auto n = heldByKind[i].load(std::memory_order_relaxed)) {
				held += std::format("{}{}:{}", held.empty() ? "" : ",", i == 47 ? std::string{ "other" } : std::to_string(i), n);
			}
			if (appliedByKind[i]) {
				applied += std::format("{}{}:{}", applied.empty() ? "" : ",", i == 47 ? std::string{ "other" } : std::to_string(i), appliedByKind[i]);
			}
		}
		// The hook's counts are written under notedLock (any thread), the rest on the main thread, as this runs.
		std::size_t waiting = 0;
		Counts      c;
		{
			std::scoped_lock l{ notedLock };
			waiting = noted.size();
			c = counts;
		}
		return std::format(
			"active={} ours: heldBack={} both={} sent={} noKey={} notOthers={} repeats={} overflow={} noIndex={} waiting={} byKind={} | theirs: heard={} applied={} refusedByGame={} "
			"notOurs={} noActor={} dead={} noItem={} noStandIn={} byKind={} | slowTime: sent={} heard={} shared={} far={} elsewhere={} refused={}",
			active ? 1 : 0, c.heldBack, c.both, c.sent, c.noKey, c.notOthers, c.repeats, c.overflow, c.noIndex, waiting, held.empty() ? "none" : held, c.heard, c.applied,
			c.refusedByGame, c.notOurs, c.noActor, c.dead, c.noItem, c.noStandIn, applied.empty() ? "none" : applied, c.slowSent, c.slowHeard, c.slowShared, c.slowFar, c.slowElsewhere,
			c.slowRefused);
	}
}
