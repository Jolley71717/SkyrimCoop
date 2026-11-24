#include "game/Traps.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/RemotePlayers.h"
#include "game/TrapRules.h"
#include "net/FormsCommand.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Traps
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Trap = Protocol::Trap;
		using Kind = TrapRules::Kind;
		using Look = TrapRules::Look;
		using Verdict = TrapRules::Verdict;

		constexpr float       FIND_RADIUS = 6000.0f;  // traps further than this from our player are not read
		constexpr auto        FIND_EVERY = std::chrono::seconds(1);
		constexpr auto        READ_EVERY = std::chrono::milliseconds(250);
		constexpr auto        RECHECK_FOR = std::chrono::seconds(10);  // a reference with no trap script is looked at again
		                                                                // this long (its scripts bind as its cell loads)
		constexpr std::size_t MAX_LOOKS = 512;     // references looked at for trap scripts in one find
		constexpr std::size_t MAX_NOT_TRAPS = 32768;
		constexpr std::size_t MAX_OWNED = 32768;
		constexpr std::size_t MAX_TRACKED = 1024;
		constexpr std::size_t MAX_LOGGED = 32;

		// Whether a reference is a trap, by its scripts: any thread (WorldObjects asks), its own lock.
		std::mutex                     ownLock;
		std::unordered_set<RE::FormID> owned;

		// Everything below, between the tick and SKSE's callbacks on the main thread. Never held while sending.
		std::mutex stateLock;

		std::atomic<bool>     active{ false };
		bool                  installed = false;
		std::uint32_t         ourId = 0;
		bool                  askDue = false;
		TrapRules::Watch      watch;
		TrapRules::Outbox     outbox;
		TrapRules::Waiting    waiting;
		Clock::time_point     nextFind{};
		Clock::time_point     nextRead{};
		std::size_t           logged = 0;

		struct Tracked
		{
			Kind        kind = Kind::kNone;
			std::string script;            // the trap script's own name: what its state is set on
			float       distance = -1.0f;  // our player from it at the last read
			float       standIn = -1.0f;   // ... and the nearest friend's stand-in
		};
		std::unordered_map<RE::FormID, Tracked>           tracked;   // traps loaded near our player
		std::unordered_map<RE::FormID, Clock::time_point> notTraps;  // looked at, no trap script: since when

		struct Counts
		{
			std::uint64_t finds = 0;
			std::uint64_t found = 0;      // traps started being read
			std::uint64_t reads = 0;
			std::uint64_t left = 0;       // traps read once more as they left the world here (disabled, 3D gone)
			std::uint64_t leftTold = 0;   // ... whose last read was news (a soul gem taken)
			std::uint64_t sent = 0;       // our words told to the session
			std::uint64_t heard = 0;      // words heard
			std::uint64_t catchUps = 0;   // catch-up batches heard
			std::uint64_t asks = 0;
			std::uint64_t made = 0;       // words made here (each verdict below)
			std::uint64_t dispatchFailed = 0;
			std::uint64_t offMain = 0;
			std::array<std::uint64_t, TrapRules::VERDICT_COUNT> verdicts{};
		};
		Counts counts;

		std::uint32_t Counted(std::uint64_t a_n)
		{
			return static_cast<std::uint32_t>((std::min<std::uint64_t>)(a_n, 0xFFFFFFFF));
		}

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		void NoteThread(std::string_view a_what)
		{
			if (GameThread::IsLoadThread()) {
				return;
			}
			if (counts.offMain++ == 0) {
				logs::info("Traps: {} off the game's main thread (the tick runs on SKSE's task queue until the frame hook is written); counted in `traps` as offmain",
					a_what);
			}
		}

		const char* KindName(Kind a_kind)
		{
			switch (a_kind) {
			case Kind::kTripwire:
				return "tripwire";
			case Kind::kHinge:
				return "hinge";
			case Kind::kRockfall:
				return "rockfall";
			case Kind::kBreakaway:
				return "breakaway";
			case Kind::kBearTrap:
				return "bear";
			case Kind::kSoulGem:
				return "soulgem";
			case Kind::kNone:
				break;
			}
			return "none";
		}

		const char* PhaseName(std::uint8_t a_phase)
		{
			switch (a_phase) {
			case Trap::kFired:
				return "fired";
			case Trap::kDisarmed:
				return "disarmed";
			case Trap::kArmed:
				return "armed";
			default:
				return "?";
			}
		}

		float Distance(const RE::TESObjectREFR* a_a, const RE::TESObjectREFR* a_b)
		{
			const auto cellA = a_a ? a_a->GetParentCell() : nullptr;
			const auto cellB = a_b ? a_b->GetParentCell() : nullptr;
			if (!cellA || !cellB) {
				return -1.0f;
			}
			if ((cellA->IsInteriorCell() || cellB->IsInteriorCell()) ? cellA != cellB : a_a->GetWorldspace() != a_b->GetWorldspace()) {
				return -1.0f;
			}
			return a_a->GetPosition().GetDistance(a_b->GetPosition());
		}

		// The kinds of base a trap's reference can be (the records Papyrus scripts are bound to that a trap is placed as).
		bool MayBeTrap(const RE::TESForm* a_base)
		{
			if (!a_base) {
				return false;
			}
			switch (a_base->GetFormType()) {
			case RE::FormType::Activator:
			case RE::FormType::Door:
			case RE::FormType::Container:
			case RE::FormType::Furniture:
			case RE::FormType::MovableStatic:
			case RE::FormType::Misc:
			case RE::FormType::SoulGem:
				return true;
			default:
				return false;
			}
		}

		// What a reference's scripts say: the first bound script whose chain is a trap's, its name and its state.
		struct ScriptRead
		{
			Kind        kind = Kind::kNone;
			std::string script;
			std::string state;
		};

		ScriptRead ReadScripts(RE::TESObjectREFR* a_ref)
		{
			using namespace RE::BSScript;
			ScriptRead out;
			const auto vm = Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!a_ref || !policy) {
				return out;
			}
			const auto handle = policy->GetHandleForObject(a_ref->GetFormType(), a_ref);
			if (handle == policy->EmptyHandle()) {
				return out;
			}
			RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
			const auto          it = vm->attachedScripts.find(handle);
			if (it == vm->attachedScripts.end()) {
				return out;
			}
			for (const auto& script : it->second) {
				const auto object = script.get();
				const auto type = object ? object->GetTypeInfo() : nullptr;
				if (!type) {
					continue;
				}
				std::vector<std::string_view> chain;
				for (auto link = static_cast<const ObjectTypeInfo*>(type); link && chain.size() < 16; link = link->GetParent()) {
					const auto name = link->GetName();
					chain.emplace_back(name ? name : "");
				}
				if (const auto kind = TrapRules::Classify(chain); kind != Kind::kNone) {
					out.kind = kind;
					out.script = chain.front();
					out.state = object->currentState.empty() ? std::string{} : std::string{ object->currentState.c_str() };
					return out;
				}
			}
			return out;
		}

		void Own(RE::FormID a_ref)
		{
			std::lock_guard guard{ ownLock };
			if (owned.size() < MAX_OWNED) {
				owned.insert(a_ref);
			}
		}

		bool Loaded(RE::TESObjectREFR* a_ref)
		{
			const auto cell = a_ref ? a_ref->GetParentCell() : nullptr;
			return a_ref && !a_ref->IsDeleted() && !a_ref->IsDisabled() && cell && cell->IsAttached() && a_ref->Is3DLoaded();
		}

		TrapRules::Point PointOf(const RE::NiPoint3& a_at)
		{
			return { a_at.x, a_at.y, a_at.z };
		}

		// Someone who can set a trap off (our player, a friend's stand-in): where they stand now and stood at the last read.
		struct Mover
		{
			const RE::TESObjectREFR* ref = nullptr;
			TrapRules::Point         at;
			TrapRules::Point         was;
		};

		// Where our player and each stand-in stood at the last read (a load forgets them).
		std::optional<TrapRules::Point>                lastPlayerAt;
		std::unordered_map<RE::FormID, TrapRules::Point> lastStandInAt;

		// How near a_mover came to a_ref since the last read (below 0: not in its cell or space).
		float Reach(const Mover& a_mover, const RE::TESObjectREFR* a_ref)
		{
			if (!a_mover.ref || Distance(a_mover.ref, a_ref) < 0.0f) {
				return -1.0f;
			}
			return TrapRules::Closest(PointOf(a_ref->GetPosition()), a_mover.was, a_mover.at);
		}

		// The nearest any of a_standIns came to a_ref since the last read (below 0: none in its cell or space).
		float NearestStandIn(const RE::TESObjectREFR* a_ref, const std::vector<Mover>& a_standIns)
		{
			float nearest = TrapRules::NO_STAND_IN;
			for (const auto& mover : a_standIns) {
				const auto d = Reach(mover, a_ref);
				if (d >= 0.0f && (nearest < 0.0f || d < nearest)) {
					nearest = d;
				}
			}
			return nearest;
		}

		// One step of making a word, on the trap's own script.
		bool Do(RE::TESObjectREFR* a_ref, const std::string& a_script, const TrapRules::Step& a_step)
		{
			using namespace RE::BSScript;
			using Step = TrapRules::Step;
			const auto vm = Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!policy) {
				return false;
			}
			const auto handle = policy->GetHandleForObject(a_ref->GetFormType(), a_ref);
			if (handle == policy->EmptyHandle()) {
				return false;
			}
			RE::BSTSmartPointer<IStackCallbackFunctor> none;
			const RE::BSFixedString                    script{ a_script.c_str() };
			const std::string                          arg{ a_step.arg };
			switch (a_step.op) {
			case Step::kGoto:
				return vm->DispatchMethodCall2(handle, script, "GotoState", RE::MakeFunctionArguments(std::string{ arg }), none);
			case Step::kAnim:
				return a_ref->NotifyAnimationGraph(RE::BSFixedString{ arg.c_str() });
			case Step::kTriggerEnter:
				return vm->DispatchMethodCall2(handle, script, "OnTriggerEnter", RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(a_ref)), none);
			case Step::kDisable:
				a_ref->Disable();
				return true;
			}
			return false;
		}

		// The session's word on a trap, made here if it is loaded and reads otherwise; else it waits.
		void Try(TrapRules::Waiting::Entry& a_entry, Clock::time_point a_now)
		{
			const auto& word = a_entry.word;
			const auto  ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(word.ref);
			const auto  at = tracked.find(word.ref);
			TrapRules::Facts facts;
			ScriptRead       read;
			if (ref && at != tracked.end() && Loaded(ref)) {
				read = ReadScripts(ref);
				const auto base = ref->GetBaseObject();
				facts.loaded = true;
				facts.base = base ? base->GetFormID() : 0;
				facts.kind = read.kind;
				facts.look = TrapRules::LookOf(read.kind, read.state);
				facts.settled = watch.Settled(word.ref, a_now);
				facts.justMade = watch.RecentlyApplied(word.ref, a_now);
				facts.playerDistance = Distance(Player(), ref);
			}
			facts.attempts = a_entry.attempts;
			facts.catchUp = a_entry.catchUp;
			facts.fresh = a_now - a_entry.heardAt < TrapRules::FRESH;
			const auto verdict = TrapRules::Judge(word, facts);
			switch (verdict) {
			case Verdict::kWait:
				return;
			case Verdict::kNothing:
			case Verdict::kSkip:
				++counts.verdicts[static_cast<std::size_t>(verdict)];
				waiting.Remove(word.ref);
				return;
			case Verdict::kGiveUp:
				++counts.verdicts[static_cast<std::size_t>(verdict)];
				logs::warn("Traps: player {}'s word on {:08X} ({} {}) was made here {} times and it still reads {}; left as it is", word.playerId, word.ref,
					KindName(word.kind), PhaseName(word.phase), a_entry.attempts, TrapRules::LookName(facts.look));
				waiting.Remove(word.ref);
				return;
			case Verdict::kFireLive:
			case Verdict::kFireQuiet:
			case Verdict::kDisarm:
			case Verdict::kRearm:
				break;
			}
			NoteThread("a trap made for the session");
			++counts.verdicts[static_cast<std::size_t>(verdict)];
			++counts.made;
			for (const auto& step : TrapRules::StepsFor(word.kind, verdict)) {
				if (!Do(ref, read.script, step)) {
					++counts.dispatchFailed;
				}
			}
			watch.Applied(word.ref, a_now);
			++a_entry.attempts;  // checked again after APPLY_QUIET: done when it reads the word's way
			if (a_entry.attempts > 1 || !a_entry.catchUp || logged++ < MAX_LOGGED) {
				logs::info("Traps: {:08X} ({}) made {} here: player {} {} it{}", word.ref, KindName(word.kind), TrapRules::VerdictName(verdict), word.playerId,
					PhaseName(word.phase), a_entry.attempts > 1 ? std::format(" (try {})", a_entry.attempts) : "");
			}
		}

		// The traps loaded near our player, found: each new one read from now on.
		void Find(Clock::time_point a_now)
		{
			const auto player = Player();
			const auto tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return;
			}
			++counts.finds;
			std::vector<RE::TESObjectREFR*> look;
			tes->ForEachReferenceInRange(player, FIND_RADIUS, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref == player || look.size() >= MAX_LOOKS) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto id = a_ref->GetFormID();
				if (FormIds::IsRuntime(id) || tracked.contains(id) || !MayBeTrap(a_ref->GetBaseObject())) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (const auto it = notTraps.find(id); it != notTraps.end() && a_now - it->second >= RECHECK_FOR) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				look.push_back(a_ref);
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto ref : look) {
				const auto id = ref->GetFormID();
				const auto read = Loaded(ref) ? ReadScripts(ref) : ScriptRead{};
				if (read.kind == Kind::kNone || tracked.size() >= MAX_TRACKED) {
					if (notTraps.size() >= MAX_NOT_TRAPS) {
						notTraps.clear();  // looked at again: a few hundred reads
					}
					notTraps.try_emplace(id, a_now);
					continue;
				}
				notTraps.erase(id);
				tracked.emplace(id, Tracked{ read.kind, read.script });
				Own(id);
				++counts.found;
			}
		}

		// A read of a trap, told to the session if it is news.
		bool Tell(RE::TESObjectREFR* a_ref, Kind a_kind, Look a_look, float a_distance, float a_standIn, Clock::time_point a_now, bool a_leaving)
		{
			const auto id = a_ref->GetFormID();
			const auto phase = watch.Read(id, a_kind, a_look, a_distance, a_now, a_standIn);
			const auto base = phase ? a_ref->GetBaseObject() : nullptr;
			if (!base) {
				return false;
			}
			outbox.Add({ .ref = id, .base = base->GetFormID(), .kind = a_kind, .phase = *phase });
			++counts.sent;
			logs::info("Traps: {:08X} ({}) {} in our game{} (our player {:.0f} away); told to the session", id, KindName(a_kind), PhaseName(*phase),
				a_leaving ? " as it left the world" : "", a_distance);
			return true;
		}

		// The traps found, read: our changes told, the session's words made.
		void Read(Clock::time_point a_now)
		{
			const auto player = Player();
			if (!player) {
				return;
			}
			// Who could have set a trap off since the last read, each with the way they came (who crossed a tripwire is
			// told by that way, not by where they stand now).
			const auto playerAt = PointOf(player->GetPosition());
			const Mover us{ .ref = player, .at = playerAt, .was = lastPlayerAt.value_or(playerAt) };
			std::vector<Mover> standIns;
			std::unordered_map<RE::FormID, TrapRules::Point> standInsAt;
			for (const auto actor : RemotePlayers::StandIns()) {
				const auto at = PointOf(actor->GetPosition());
				const auto was = lastStandInAt.find(actor->GetFormID());
				standIns.push_back({ .ref = actor, .at = at, .was = was != lastStandInAt.end() ? was->second : at });
				standInsAt.emplace(actor->GetFormID(), at);
			}
			lastPlayerAt = playerAt;
			lastStandInAt = std::move(standInsAt);

			for (auto it = tracked.begin(); it != tracked.end();) {
				const auto id = it->first;
				const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				const auto distance = ref ? Distance(player, ref) : -1.0f;
				if (!ref || !Loaded(ref) || distance < 0.0f || distance > FIND_RADIUS * 1.25f) {
					// Leaving the world here: read once more, so a spend that ends in a disable or a removal is told (a
					// magic trap's gem taken goes "disarmed" and is disabled in one call, before a read could see it in the
					// world). Who was near as last read when it has no place now (gone into an inventory).
					if (ref) {
						++counts.left;
						const auto read = ReadScripts(ref);
						if (read.kind != Kind::kNone && read.kind == it->second.kind) {
							const bool placed = ref->GetParentCell() != nullptr;
							const auto reach = placed && distance >= 0.0f ? Reach(us, ref) : it->second.distance;
							const auto standIn = placed ? NearestStandIn(ref, standIns) : it->second.standIn;
							if (Tell(ref, read.kind, TrapRules::LookOf(read.kind, read.state), reach, standIn, a_now, true)) {
								++counts.leftTold;
							}
						}
					}
					if (ref && ref->IsDisabled()) {
						waiting.Left(id);  // a word made on it is done: its making ended in a disable
					}
					watch.Forget(id);  // read afresh when it is loaded near again
					it = tracked.erase(it);
					continue;
				}
				++counts.reads;
				const auto read = ReadScripts(ref);
				it->second.kind = read.kind;
				if (!read.script.empty()) {
					it->second.script = read.script;
				}
				it->second.distance = Reach(us, ref);
				it->second.standIn = NearestStandIn(ref, standIns);
				if (read.kind != Kind::kNone) {
					Tell(ref, read.kind, TrapRules::LookOf(read.kind, read.state), it->second.distance, it->second.standIn, a_now, false);
				}
				if (const auto entry = waiting.Find(id)) {
					Try(*entry, a_now);
				}
				++it;
			}
		}

		void ForgetWorld()
		{
			watch.Clear();
			waiting.Clear();
			tracked.clear();
			notTraps.clear();
			lastPlayerAt.reset();
			lastStandInAt.clear();
			logged = 0;
		}
	}

	bool Install()
	{
		std::lock_guard guard{ stateLock };
		installed = true;
		logs::info("Traps: tripwires, trapped hinges, rockfalls, breakaway floors, bear traps and magic traps' soul gems are shared");
		return true;
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		std::lock_guard guard{ stateLock };
		ourId = a_playerId;
		ForgetWorld();
		outbox.Clear();
		askDue = false;  // the server sends what it holds with the welcome
		active = true;
	}

	void Frame(const SendFn& a_send)
	{
		std::vector<std::vector<std::uint8_t>> packets;
		{
			std::lock_guard guard{ stateLock };
			const auto      now = Clock::now();
			if (LocalPlayer::InWorld()) {
				if (now >= nextFind) {
					nextFind = now + FIND_EVERY;
					Find(now);
				}
				if (now >= nextRead) {
					nextRead = now + READ_EVERY;
					Read(now);
				}
			}
			if (askDue) {
				askDue = false;
				++counts.asks;
				packets.push_back(Protocol::Writer{ Protocol::MessageType::kAskTraps }.Data());
				logs::info("Traps: asked the session again for what became of the world's traps");
			}
			if (const auto batch = outbox.Due(now)) {
				Protocol::TrapBatch msg;
				msg.entries = *batch;
				packets.push_back(Protocol::Encode(msg));
			}
		}
		for (auto& packet : packets) {
			a_send(std::move(packet));
		}
	}

	void Heard(const Protocol::TrapBatch& a_batch)
	{
		std::lock_guard guard{ stateLock };
		const auto      now = Clock::now();
		if (a_batch.catchUp) {
			++counts.catchUps;
		}
		for (const auto& entry : a_batch.entries) {
			++counts.heard;
			if (!waiting.Add(entry, a_batch.catchUp, now) && waiting.Refused() == 1) {
				logs::warn("Traps: {} of the session's words wait here, the most; a new one is not made (counted in `traps` as refused)", waiting.Size());
			}
			if (!a_batch.catchUp) {
				logs::info("Traps: player {} says {:08X} ({}) {}", entry.playerId, entry.ref, KindName(entry.kind), PhaseName(entry.phase));
			}
		}
	}

	void AfterLoad()
	{
		std::lock_guard guard{ stateLock };
		ForgetWorld();
		askDue = active;  // in a session: the session's words again (our own come back with the save)
	}

	void NewGame()
	{
		AfterLoad();
	}

	void Clear()
	{
		std::lock_guard guard{ stateLock };
		active = false;
		ForgetWorld();
		outbox.Clear();
		askDue = false;
		ourId = 0;
	}

	bool IsTrap(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		const auto id = a_ref->GetFormID();
		{
			std::lock_guard guard{ ownLock };
			if (owned.contains(id)) {
				return true;
			}
		}
		if (!MayBeTrap(a_ref->GetBaseObject()) || ReadScripts(a_ref).kind == Kind::kNone) {
			return false;
		}
		Own(id);
		return true;
	}

	namespace
	{
		struct Near
		{
			RE::TESObjectREFR* ref = nullptr;
			float              distance = 0.0f;
			ScriptRead         read;
			Look               look = Look::kUnknown;
		};

		std::vector<Near> FindNear(float a_radius)
		{
			std::vector<Near> found;
			const auto        player = Player();
			const auto        tes = RE::TES::GetSingleton();
			if (!player || !tes) {
				return found;
			}
			std::vector<RE::TESObjectREFR*> look;
			tes->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && a_ref != player && look.size() < 4 * MAX_LOOKS && MayBeTrap(a_ref->GetBaseObject())) {
					look.push_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (const auto ref : look) {
				auto read = ReadScripts(ref);
				if (read.kind == Kind::kNone) {
					continue;
				}
				const auto state = TrapRules::LookOf(read.kind, read.state);
				found.push_back({ .ref = ref, .distance = Distance(player, ref), .read = std::move(read), .look = state });
			}
			std::ranges::sort(found, {}, &Near::distance);
			return found;
		}

		std::optional<float> Radius(std::string_view a_word, float a_default)
		{
			if (a_word.empty()) {
				return a_default;
			}
			const auto n = DevLine::Number(a_word);
			if (!n || *n == 0 || *n > 8000) {
				return std::nullopt;
			}
			return static_cast<float>(*n);
		}

		std::string Line(const Near& a_near)
		{
			const auto base = a_near.ref->GetBaseObject();
			const auto at = a_near.ref->GetPosition();
			std::string state = a_near.read.state.empty() ? "-" : a_near.read.state;
			std::ranges::replace_if(state, [](char a_c) { return a_c == ' ' || a_c == '=' || static_cast<unsigned char>(a_c) < 0x20; }, '_');
			return std::format("ref={:08X} base={:08X} kind={} look={} state={} runtime={} loaded={} x={:.0f} y={:.0f} z={:.0f} dist={:.0f}", a_near.ref->GetFormID(),
				base ? base->GetFormID() : 0, KindName(a_near.read.kind), TrapRules::LookName(a_near.look), state, FormIds::IsRuntime(a_near.ref->GetFormID()) ? 1 : 0,
				Loaded(a_near.ref) ? 1 : 0, at.x, at.y, at.z, a_near.distance);
		}

		std::string NearCommand(const std::vector<std::string_view>& a_words)
		{
			const auto radius = Radius(a_words.size() > 1 ? a_words[1] : std::string_view{}, 2000.0f);
			if (!radius || a_words.size() > 2) {
				return "error: usage: traps near [radius]";
			}
			if (!LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			const auto  found = FindNear(*radius);
			std::string out = std::format("near={}", found.size());
			for (std::size_t i = 0; i < found.size() && i < 20; ++i) {
				out += " | " + Line(found[i]);
			}
			return out;
		}

		// pick <kind|oneshot|any> [armed|spent] [radius]: the nearest trap of that kind of the load order, loaded, reading
		// armed (the default) or spent: for the rig to walk to.
		std::string PickCommand(const std::vector<std::string_view>& a_words)
		{
			constexpr auto USAGE = "error: usage: traps pick <tripwire|hinge|rockfall|breakaway|bear|soulgem|oneshot|any> [armed|spent] [radius]";
			if (a_words.size() < 2 || a_words.size() > 4) {
				return USAGE;
			}
			const auto sort = a_words[1];
			bool       wantArmed = true;
			std::size_t next = 2;
			if (a_words.size() > next && (a_words[next] == "armed" || a_words[next] == "spent")) {
				wantArmed = a_words[next] == "armed";
				++next;
			}
			if (a_words.size() > next + 1) {
				return USAGE;
			}
			const auto radius = Radius(a_words.size() > next ? a_words[next] : std::string_view{}, 4000.0f);
			if (!radius) {
				return USAGE;
			}
			constexpr std::array<std::string_view, 8> SORTS{ "tripwire", "hinge", "rockfall", "breakaway", "bear", "soulgem", "oneshot", "any" };
			if (std::ranges::find(SORTS, sort) == SORTS.end()) {
				return USAGE;
			}
			if (!LocalPlayer::InWorld()) {
				return "error: not in the world";
			}
			for (const auto& spot : FindNear(*radius)) {
				const auto side = TrapRules::Armed(spot.look);
				if (FormIds::IsRuntime(spot.ref->GetFormID()) || !Loaded(spot.ref) || spot.distance < 0.0f || !side || *side != wantArmed) {
					continue;
				}
				const bool match = sort == "any" || (sort == "oneshot" && !Protocol::TrapToggles(spot.read.kind)) || sort == KindName(spot.read.kind);
				if (match) {
					return Line(spot);
				}
			}
			return "none";
		}

		// use <ref hex>: our player's activation of a trap within reach (the rig walks there first), as the activate key does:
		// a bear trap snapped shut or set open, a magic trap's soul gem taken off.
		std::string UseCommand(const std::vector<std::string_view>& a_words)
		{
			constexpr float REACH = 250.0f;
			const auto      id = a_words.size() == 2 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			if (!id) {
				return "error: usage: traps use <ref hex>";
			}
			const auto player = Player();
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(*id);
			if (!player || !ref || !Loaded(ref)) {
				return "error: no such reference loaded here";
			}
			const auto distance = Distance(player, ref);
			if (distance < 0.0f || distance > REACH) {
				return std::format("error: out of reach (dist={:.0f}, reach {:.0f})", distance, REACH);
			}
			logs::info("Traps: the developer channel has our player activate {:08X}", *id);
			ref->ActivateRef(player, 0, nullptr, 0, false);
			return "activated";
		}

		std::string RefCommand(const std::vector<std::string_view>& a_words)
		{
			const auto id = a_words.size() == 2 ? FormsCommand::ParseHex(a_words[1]) : std::nullopt;
			if (!id) {
				return "error: usage: traps ref <ref hex>";
			}
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(*id);
			const auto read = ref ? ReadScripts(ref) : ScriptRead{};
			const auto look = TrapRules::LookOf(read.kind, read.state);
			const auto base = ref ? ref->GetBaseObject() : nullptr;
			std::string state = read.state.empty() ? "-" : read.state;
			std::ranges::replace_if(state, [](char a_c) { return a_c == ' ' || a_c == '=' || static_cast<unsigned char>(a_c) < 0x20; }, '_');
			const auto now = Clock::now();
			const auto entry = waiting.Find(*id);
			return std::format("ref={:08X} found={} base={:08X} kind={} look={} state={} disabled={} destroyed={} loaded={} watched={} settled={} waiting={} word={} "
							   "attempts={} dist={:.0f}",
				*id, ref ? 1 : 0, base ? base->GetFormID() : 0, KindName(read.kind), TrapRules::LookName(look), state, ref && ref->IsDisabled() ? 1 : 0,
				ref && ref->IsDestroyed() ? 1 : 0, ref && Loaded(ref) ? 1 : 0, tracked.contains(*id) ? 1 : 0, watch.Settled(*id, now) ? 1 : 0, entry ? 1 : 0,
				entry ? PhaseName(entry->word.phase) : "-", entry ? entry->attempts : 0, ref ? Distance(Player(), ref) : -1.0f);
		}
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "near") {
			return NearCommand(words);
		}
		if (!words.empty() && words[0] == "pick") {
			return PickCommand(words);
		}
		if (!words.empty() && words[0] == "use") {
			return UseCommand(words);  // not under the state lock: an activation sends its events from inside the call
		}
		std::lock_guard guard{ stateLock };
		if (words.empty()) {
			std::string verdicts;
			for (std::size_t i = 0; i < TrapRules::VERDICT_COUNT; ++i) {
				if (counts.verdicts[i] != 0) {
					verdicts += std::format("{}{}:{}", verdicts.empty() ? "" : ",", TrapRules::VerdictName(static_cast<Verdict>(i)), Counted(counts.verdicts[i]));
				}
			}
			std::size_t ownedNow = 0;
			{
				std::lock_guard ownGuard{ ownLock };
				ownedNow = owned.size();
			}
			return std::format("installed={} us={} session={} tracked={} owned={} watched={} waiting={} queued={} finds={} found={} reads={} left={} leftTold={} "
							   "sent={} heard={} catchUps={} asks={} made={} verdicts={} quieted={} farToggles={} standInNearer={} dispatchFailed={} refused={} "
							   "watchRefused={} overflow={} offmain={}",
				installed ? 1 : 0, ourId, active ? 1 : 0, tracked.size(), ownedNow, watch.Size(), waiting.Size(), outbox.Size(), Counted(counts.finds),
				Counted(counts.found), Counted(counts.reads), Counted(counts.left), Counted(counts.leftTold), Counted(counts.sent), Counted(counts.heard),
				Counted(counts.catchUps), Counted(counts.asks), Counted(counts.made), verdicts.empty() ? std::string{ "-" } : verdicts, Counted(watch.Quieted()),
				Counted(watch.FarToggles()), Counted(watch.StandInsNearer()), Counted(counts.dispatchFailed), Counted(waiting.Refused()), Counted(watch.Refused()),
				Counted(outbox.Overflow()), Counted(counts.offMain));
		}
		if (words[0] == "ref") {
			return RefCommand(words);
		}
		return "error: usage: traps [near [radius] | pick <tripwire|hinge|rockfall|breakaway|bear|soulgem|oneshot|any> [armed|spent] [radius] | ref <ref hex> | use <ref hex>]";
	}
}
