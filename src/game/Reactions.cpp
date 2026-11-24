#include "game/Reactions.h"

#include "game/Furniture.h"
#include "game/NpcSync.h"
#include "game/Pairs.h"
#include "game/ReactionRules.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>

namespace Reactions
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Kind = ReactionRules::Kind;
		constexpr std::size_t KINDS = static_cast<std::size_t>(Kind::kCount);

		std::atomic<bool> active{ false };
		std::uint32_t     ourId = 0;

		// Set while we play an event on a stand-in or a copy ourselves (Play): the hook on that thread hears it as ours.
		thread_local bool playing = false;

		std::mutex                    lock;  // judge and the counts below it
		ReactionRules::Judge          judge;
		std::atomic<std::uint64_t>    playedHere{ 0 };  // reactions our own play put on a copy or a stand-in (never sent)

		// Main thread.
		ReactionRules::Outbox outbox;
		ReactionRules::Echoes echoes;
		ReactionRules::Gate   gate;
		std::array<std::uint64_t, KINDS> sentByKind{};
		std::uint64_t notCopies = 0;     // judged ours, but on an actor that is no copy of another game's NPC
		std::uint64_t in = 0;            // a friend's reactions that came
		std::array<std::uint64_t, KINDS> playedByKind{};
		std::uint64_t refused = 0;       // the graph turned it away
		std::uint64_t tooSoon = 0;       // the same kind on that NPC within RUNNER_GAP
		std::uint64_t notReactions = 0;  // a name that is no reaction
		std::uint64_t notRun = 0;        // an NPC we do not run (any more)
		std::uint64_t notHere = 0;       // run by us, but not loaded
		std::uint64_t busy = 0;          // dead, in a kill move, or in furniture the engine plays
		std::array<bool, KINDS> toldPlayed{};

		std::size_t Index(Kind a_kind) { return static_cast<std::size_t>(a_kind); }

		std::string ByKind(const std::array<std::uint64_t, KINDS>& a_counts)
		{
			std::string out;
			for (std::size_t i = 1; i < KINDS; ++i) {
				if (a_counts[i] != 0) {
					out += std::format("{}{}:{}", out.empty() ? "" : ",", ReactionRules::KindName(static_cast<Kind>(i)), a_counts[i]);
				}
			}
			return out.empty() ? "none" : out;
		}

		const RE::BSFixedString& MagnitudeName()
		{
			static const RE::BSFixedString name{ "staggerMagnitude" };
			return name;
		}

		const RE::BSFixedString& DirectionName()
		{
			static const RE::BSFixedString name{ "staggerDirection" };
			return name;
		}

		// Our player's hits, either way: what makes a copy's reaction ours (ReactionRules::Judge).
		struct HitSink final : RE::BSTEventSink<RE::TESHitEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !active.load(std::memory_order_relaxed)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto target = a_event->target.get();
				const auto cause = a_event->cause.get();
				if (!target || !cause) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const RE::TESObjectREFR* other = cause->IsPlayerRef() ? target : target->IsPlayerRef() ? cause : nullptr;
				if (other && other->IsActor()) {
					std::scoped_lock l{ lock };
					judge.Touched(other->GetFormID(), Clock::now());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		HitSink hitSink;

		bool Busy(RE::Actor* a_actor)
		{
			return a_actor->IsDead() || Pairs::EventsStay(a_actor) || Furniture::EngineOwnsEvents(a_actor);
		}
	}

	void Install()
	{
		if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(&hitSink);
		}
	}

	void Started(std::uint32_t a_ourId)
	{
		Clear();
		ourId = a_ourId;
		active.store(true, std::memory_order_relaxed);
	}

	void HeardGraph(RE::Actor* a_actor, std::string_view a_event)
	{
		if (!a_actor || !active.load(std::memory_order_relaxed) || !ReactionRules::IsReaction(a_event)) {
			return;
		}
		if (playing) {
			playedHere.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (a_actor->IsPlayerRef()) {
			return;  // our player's own go as its actions (PlayerActions)
		}
		ReactionRules::Heard heard{ a_actor->GetFormID(), std::string{ a_event }, StaggerOf(a_actor, a_event), Clock::now() };
		std::scoped_lock l{ lock };
		judge.Heard(std::move(heard));
	}

	std::optional<Protocol::Stagger> StaggerOf(RE::Actor* a_actor, std::string_view a_event)
	{
		if (!a_actor || ReactionRules::KindOf(a_event) != Kind::kStagger) {
			return std::nullopt;
		}
		float      magnitude = 0.0f;
		float      direction = 0.0f;
		const bool hasMagnitude = a_actor->GetGraphVariableFloat(MagnitudeName(), magnitude);
		const bool hasDirection = a_actor->GetGraphVariableFloat(DirectionName(), direction);
		if (!hasMagnitude && !hasDirection) {
			return std::nullopt;  // a graph with neither (a creature's may lack them): played as it comes
		}
		return ReactionRules::Pack(magnitude, direction);
	}

	bool Play(RE::Actor* a_actor, const NpcRules::Event& a_event)
	{
		if (!a_actor) {
			return false;
		}
		const bool was = playing;
		playing = true;
		if (a_event.stagger) {
			const auto values = ReactionRules::Unpack(*a_event.stagger);
			a_actor->SetGraphVariableFloat(MagnitudeName(), values.magnitude);
			a_actor->SetGraphVariableFloat(DirectionName(), values.direction);
		}
		const bool taken = a_actor->NotifyAnimationGraph(RE::BSFixedString{ std::string_view{ a_event.name } });
		playing = was;
		return taken;
	}

	bool Echo(const Protocol::NpcKey& a_key, std::string_view a_event, Clock::time_point a_now)
	{
		const auto kind = ReactionRules::KindOf(a_event);
		return kind != Kind::kNone && echoes.Echo(a_key, kind, a_now);
	}

	void Frame(const SendFn& a_send)
	{
		if (!active.load(std::memory_order_relaxed) || !a_send) {
			return;
		}
		const auto                        now = Clock::now();
		std::vector<ReactionRules::Heard> ours;
		{
			std::scoped_lock l{ lock };
			ours = judge.Ours(now);
		}
		for (auto& reaction : ours) {
			const auto actor = RE::TESForm::LookupByID<RE::Actor>(reaction.id);
			const auto key = actor ? NpcSync::KeyOf(actor) : std::nullopt;
			const auto runner = key ? NpcSync::RunnerOf(*key) : 0;
			if (!key || runner == 0 || runner == ourId) {
				// Not a copy: an NPC we run sends its own reactions as its actions; one nobody runs is its own game's.
				++notCopies;
				continue;
			}
			const auto kind = ReactionRules::KindOf(reaction.event);
			if (outbox.Add({ *key, reaction.event, reaction.stagger }, now)) {
				echoes.Ours(*key, kind, now);
				++sentByKind[Index(kind)];
				if (sentByKind[Index(kind)] == 1) {
					logs::info("Reactions: our player's first {} on a copy ({:08X}, {}) sent to its runner, player {}", ReactionRules::KindName(kind), reaction.id,
						reaction.event, runner);
				}
			}
		}
		if (auto list = outbox.Take(); !list.empty()) {
			a_send(Protocol::Encode(Protocol::NpcReactions{ .player = 0, .reactions = std::move(list) }), true);
		}
		echoes.Forget(now);
		gate.Forget(now);
	}

	void Heard(const Protocol::NpcReactions& a_msg)
	{
		if (!active.load(std::memory_order_relaxed) || a_msg.player == ourId) {
			return;
		}
		const auto now = Clock::now();
		std::vector<std::pair<Protocol::NpcKey, RE::NiPointer<RE::Actor>>> run;
		bool                                                                 listed = false;
		for (const auto& reaction : a_msg.reactions) {
			++in;
			if (NpcSync::RunnerOf(reaction.key) != ourId) {
				++notRun;
				continue;
			}
			if (!listed) {
				run = NpcSync::RunActors();
				listed = true;
			}
			const auto it = std::ranges::find_if(run, [&](const auto& a_pair) { return a_pair.first == reaction.key; });
			if (it == run.end() || !it->second) {
				++notHere;
				continue;
			}
			const auto actor = it->second.get();
			if (Busy(actor)) {
				++busy;
				continue;
			}
			switch (gate.Judge(reaction.key, reaction.event, now)) {
			case ReactionRules::Play::kNotReaction:
				++notReactions;
				continue;
			case ReactionRules::Play::kTooSoon:
				++tooSoon;
				continue;
			case ReactionRules::Play::kPlay:
				break;
			}
			// Played as the NPC's own: its graph's event goes to its copies as its others do (NpcSync), with the stagger's
			// size and way set here, and the friend whose blow it was skips it there (Echo).
			if (reaction.stagger) {
				const auto values = ReactionRules::Unpack(*reaction.stagger);
				actor->SetGraphVariableFloat(MagnitudeName(), values.magnitude);
				actor->SetGraphVariableFloat(DirectionName(), values.direction);
			}
			const auto kind = ReactionRules::KindOf(reaction.event);
			if (actor->NotifyAnimationGraph(RE::BSFixedString{ std::string_view{ reaction.event } })) {
				++playedByKind[Index(kind)];
				if (!toldPlayed[Index(kind)]) {
					toldPlayed[Index(kind)] = true;
					logs::info("Reactions: player {}'s first {} played on our NPC {:08X} ({})", a_msg.player, ReactionRules::KindName(kind), actor->GetFormID(),
						reaction.event);
				}
			} else {
				++refused;
			}
		}
	}

	void Clear()
	{
		active.store(false, std::memory_order_relaxed);
		{
			std::scoped_lock l{ lock };
			judge.Clear();
		}
		outbox.Clear();
		echoes.Clear();
		gate.Clear();
	}

	std::string Describe()
	{
		std::scoped_lock l{ lock };
		return std::format(
			"active={} ours: sent={} byKind={} judgedOurs={} notOurs={} notCopies={} heardDropped={} dropped={} waiting={} touching={} playedHere={} echoesSkipped={}"
			" | theirs: in={} played={} refused={} tooSoon={} notReactions={} notRun={} notHere={} busy={}",
			active.load() ? 1 : 0, outbox.sent, ByKind(sentByKind), judge.ours, judge.notOurs, notCopies, judge.dropped, outbox.dropped, judge.Waiting(),
			judge.Touching(), playedHere.load(), echoes.echoes, in, ByKind(playedByKind), refused, tooSoon, notReactions, notRun, notHere, busy);
	}
}
