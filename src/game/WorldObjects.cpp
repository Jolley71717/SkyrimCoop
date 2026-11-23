#include "game/WorldObjects.h"

#include "DevLine.h"
#include "game/LocalPlayer.h"
#include "game/RefRules.h"
#include "game/RemotePlayers.h"
#include "net/FormsCommand.h"

namespace WorldObjects
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using RefState = Protocol::RefState;

		// An event the sinks queued, read on the next frame on the main thread.
		struct Event
		{
			enum class Kind : std::uint8_t
			{
				kOpenClose,
				kLock,
				kUse,
			};
			Kind        kind = Kind::kOpenClose;
			RE::FormID  ref = 0;
			bool        byOurPlayer = false;
		};

		std::mutex         queueLock;
		std::vector<Event> queue;  // under queueLock
		constexpr std::size_t MAX_QUEUED = 1024;  // a frame's worth and more: past it the oldest wait no longer

		std::uint32_t       ourId = 0;
		RefRules::Book      book;
		RefRules::Outbox    outbox;
		RefRules::Pending   pending;
		bool                askAgain = false;
		Clock::time_point   nextRetry{};

		struct Counts
		{
			std::uint64_t events = 0;    // queued by the sinks
			std::uint64_t sent = 0;      // reports sent
			std::uint64_t paced = 0;     // ... of them held a while first (RefRules::Outbox)
			std::uint64_t heard = 0;     // words of the session
			std::uint64_t applied = 0;   // ... brought a reference here to them
			std::uint64_t same = 0;      // ... found it so already
			std::uint64_t waited = 0;    // ... waited for the reference to load
			std::uint64_t stale = 0;     // ... our own, older than a report of ours still out (RefRules::Book)
			std::uint64_t usesSent = 0;  // our player's uses of a mechanism
			std::uint64_t usesMade = 0;  // a friend's, made here by their stand-in
			std::uint64_t usesRefused = 0;
			std::uint64_t asks = 0;      // every kept word asked for again (a load)
		};
		Counts      counts;
		std::string lastRefused = "none";

		RE::PlayerCharacter* Player()
		{
			return RE::PlayerCharacter::GetSingleton();
		}

		// The distance between two references in the same place (a cell's interior, or one worldspace outside); -1
		// when they are not (a distance between places means nothing).
		float Distance(const RE::TESObjectREFR* a_a, const RE::TESObjectREFR* a_b)
		{
			if (!a_a || !a_b) {
				return -1.0f;
			}
			const auto cellA = a_a->GetParentCell();
			const auto cellB = a_b->GetParentCell();
			if (!cellA || !cellB) {
				return -1.0f;
			}
			if (cellA->IsInteriorCell() || cellB->IsInteriorCell()) {
				if (cellA != cellB) {
					return -1.0f;
				}
			} else if (a_a->GetWorldspace() != a_b->GetWorldspace()) {
				return -1.0f;
			}
			return a_a->GetPosition().GetDistance(a_b->GetPosition());
		}

		RefRules::Facts FactsOf(RE::TESObjectREFR* a_ref)
		{
			RefRules::Facts facts;
			const auto      base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base) {
				return facts;
			}
			// The record's "is marker" flag (bit 23 of a base's flags): an editor marker nobody sees.
			facts.marker = (base->GetFormFlags() & (1u << 23)) != 0;
			switch (base->GetFormType()) {
			case RE::FormType::Door:
				{
					facts.base = RefRules::Base::kDoor;
					facts.loadDoor = a_ref->extraList.GetByType<RE::ExtraTeleport>() != nullptr;
					const auto door = base->As<RE::TESObjectDOOR>();
					facts.automatic = door && door->flags.any(RE::TESObjectDOOR::Flag::kAutomatic);
					break;
				}
			case RE::FormType::Activator:
				{
					facts.base = RefRules::Base::kActivator;
					const auto name = base->GetName();
					facts.named = name && name[0] != '\0';
					break;
				}
			case RE::FormType::Container:
				facts.base = RefRules::Base::kContainer;
				break;
			default:
				break;
			}
			return facts;
		}

		// What the reference reads here, whole: its open state (none for what has none) and its lock (a reference with
		// no lock reads unlocked, its level not known).
		RefState Read(RE::TESObjectREFR* a_ref)
		{
			RefState state;
			state.refId = a_ref->GetFormID();
			state.open = RefRules::OpenOf(static_cast<int>(RE::BGSOpenCloseForm::GetOpenState(a_ref)));
			if (const auto lock = a_ref->GetLock()) {
				state.locked = lock->IsLocked() ? RefState::kYes : RefState::kNo;
				// locked: the game's own level (a leveled lock scaled to this player); unlocked: the lock's own number,
				// unless it is leveled (its level then depends on the player, so it is not said)
				if (lock->IsLocked()) {
					state.lockLevel = RefRules::LevelOf(static_cast<int>(lock->GetLockLevel(a_ref)));
				} else if (!lock->flags.any(RE::REFR_LOCK::Flag::kLeveled)) {
					state.lockLevel = RefRules::LevelOfBase(lock->baseLevel);
				}
			} else {
				state.locked = RefState::kNo;
			}
			return state;
		}

		RE::TESObjectREFR* Find(RE::FormID a_id)
		{
			return a_id != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id) : nullptr;
		}

		// Brings a loaded reference to the session's word; false when it is not loaded here (it waits).
		bool Apply(const RefState& a_want)
		{
			const auto ref = Find(a_want.refId);
			if (!ref || !ref->Is3DLoaded() || ref->IsDisabled() || ref->IsDeleted()) {
				return false;
			}
			const auto shares = RefRules::Of(FactsOf(ref));
			const auto change = RefRules::Plan(a_want, RefRules::Shared(Read(ref), shares));
			if (!change.Any()) {
				++counts.same;
				return true;
			}
			if (change.locked || change.level) {
				auto lock = ref->GetLock();
				if (!lock && change.locked.value_or(false)) {
					lock = ref->AddLock();
				}
				if (lock) {
					if (change.level) {
						lock->baseLevel = RefRules::BaseOfLevel(*change.level);
						lock->flags.reset(RE::REFR_LOCK::Flag::kLeveled);
					}
					if (change.locked) {
						lock->SetLocked(*change.locked);
					}
					ref->AddLockChange();  // kept in this game's save, as a lock Papyrus changes is
				}
			}
			if (change.open) {
				RE::BGSOpenCloseForm::SetOpenState(ref, *change.open, false);
			}
			++counts.applied;
			logs::info("World: {:08X} ({}) brought to the session's word:{}{}{}", ref->GetFormID(), RefRules::KindName(FactsOf(ref)),
				change.open ? (*change.open ? " opened" : " closed") : "", change.locked ? (*change.locked ? " locked" : " unlocked") : "",
				change.level ? std::format(" level {}", *change.level) : "");
			return true;
		}

		void Report(const RefState& a_state, const SendFn& a_send)
		{
			book.Sent(a_state);
			a_send(Protocol::Encode(a_state));
			++counts.sent;
		}

		// A queued event, read now: what the reference reads, reported if the session doesn't know it.
		void Look(const Event& a_event, const SendFn& a_send, Clock::time_point a_now)
		{
			if (!RefRules::Shareable(a_event.ref)) {
				return;
			}
			const auto ref = Find(a_event.ref);
			if (!ref || !ref->Is3DLoaded()) {
				return;
			}
			const auto facts = FactsOf(ref);
			const auto shares = RefRules::Of(facts);
			if (a_event.kind == Event::Kind::kUse) {
				if (shares.use && a_event.byOurPlayer) {
					a_send(Protocol::Encode(Protocol::Activation{ .refId = a_event.ref }));
					++counts.usesSent;
					logs::info("World: our player used {:08X} ({}); the others' stand-ins of them use it too", a_event.ref, RefRules::KindName(facts));
				}
				return;
			}
			if (!shares.open && !shares.lock) {
				return;
			}
			if (!RefRules::Reports(a_event.byOurPlayer, Distance(Player(), ref))) {
				return;
			}
			const auto read = RefRules::Shared(Read(ref), shares);
			if (!book.News(read)) {
				return;
			}
			if (outbox.Offer(read, a_now)) {
				Report(read, a_send);
			} else {
				++counts.paced;
			}
		}

		std::vector<Event> TakeQueue()
		{
			std::lock_guard guard{ queueLock };
			return std::exchange(queue, {});
		}

		void Queue(Event a_event)
		{
			std::lock_guard guard{ queueLock };
			if (queue.size() < MAX_QUEUED) {
				queue.push_back(a_event);
			}
		}

		bool IsOurPlayer(const RE::TESObjectREFR* a_ref)
		{
			return a_ref && a_ref == Player();
		}

		class Sinks :
			public RE::BSTEventSink<RE::TESOpenCloseEvent>,
			public RE::BSTEventSink<RE::TESLockChangedEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESOpenCloseEvent* a_event, RE::BSTEventSource<RE::TESOpenCloseEvent>*) override
			{
				if (a_event && a_event->ref) {
					Queue({ .kind = Event::Kind::kOpenClose, .ref = a_event->ref->GetFormID(), .byOurPlayer = IsOurPlayer(a_event->activeRef.get()) });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESLockChangedEvent* a_event, RE::BSTEventSource<RE::TESLockChangedEvent>*) override
			{
				if (a_event && a_event->lockedObject) {
					// The event names no cause: a lock picked, opened with a key or changed by a script near our player
					// all count (RefRules::Reports by distance), our player's own lockpicking among them.
					Queue({ .kind = Event::Kind::kLock, .ref = a_event->lockedObject->GetFormID() });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				// Only our player's uses: a stand-in's use (a friend's, made here) or an NPC's is not reported.
				if (a_event && a_event->objectActivated && IsOurPlayer(a_event->actionRef.get())) {
					Queue({ .kind = Event::Kind::kUse, .ref = a_event->objectActivated->GetFormID(), .byOurPlayer = true });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		Sinks sinks;
		bool  installed = false;

		std::uint32_t Counted(std::uint64_t a_n)
		{
			return static_cast<std::uint32_t>((std::min<std::uint64_t>)(a_n, 0xFFFFFFFF));
		}

		std::string Line(RE::TESObjectREFR* a_ref)
		{
			const auto facts = FactsOf(a_ref);
			const auto read = Read(a_ref);
			const auto known = book.Known(a_ref->GetFormID());
			const auto value = [](std::uint8_t a_v) { return a_v == RefState::kUnknown ? "-"s : std::to_string(a_v); };
			const auto level = [](std::uint8_t a_v) { return a_v == RefState::LEVEL_UNKNOWN ? "-"s : std::to_string(a_v); };
			const auto at = a_ref->GetPosition();
			const auto dist = Distance(Player(), a_ref);
			// its name as players see it, one word (spaces as _; "-" for none): the rig picks a mechanism by it
			std::string name = a_ref->GetDisplayFullName() ? a_ref->GetDisplayFullName() : "";
			std::ranges::replace_if(name, [](char a_c) { return a_c == ' ' || a_c == '|' || a_c == '=' || static_cast<unsigned char>(a_c) < 0x20; }, '_');
			if (name.empty()) {
				name = "-";
			}
			return std::format("ref={:08X} kind={} open={} locked={} level={} loaded={} known={}/{}/{} dist={:.0f} at=({:.0f},{:.0f},{:.0f}) name={}",
				a_ref->GetFormID(), RefRules::KindName(facts), value(read.open), value(read.locked), level(read.lockLevel), a_ref->Is3DLoaded() ? 1 : 0,
				known ? value(known->open) : "-", known ? value(known->locked) : "-", known ? level(known->lockLevel) : "-", dist, at.x, at.y, at.z, name);
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::warn("World: the game's event source is not there; doors and mechanisms are not shared");
			return false;
		}
		events->AddEventSink<RE::TESOpenCloseEvent>(&sinks);
		events->AddEventSink<RE::TESLockChangedEvent>(&sinks);
		events->AddEventSink<RE::TESActivateEvent>(&sinks);
		installed = true;
		logs::info("World: watching doors, locks and mechanisms (open, close, lock and use events)");
		return true;
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		ourId = a_playerId;
		book.Clear();
		outbox.Clear();
		pending.Clear();
		askAgain = false;
		TakeQueue();  // what happened before we were in is in the world as it is, not news
	}

	void Frame(const SendFn& a_send)
	{
		const auto now = Clock::now();
		if (!LocalPlayer::InWorld()) {
			return;  // a loading screen, the main menu: the events wait in the queue (up to MAX_QUEUED)
		}
		if (std::exchange(askAgain, false)) {
			a_send(Protocol::Writer{ Protocol::MessageType::kAskRefStates }.Data());
			++counts.asks;
			logs::info("World: asked the session for every door's state again (a load)");
		}
		for (const auto& event : TakeQueue()) {
			++counts.events;
			Look(event, a_send, now);
		}
		for (const auto& held : outbox.Due(now)) {
			// Held a while: what it reads now goes, if the session still doesn't know it.
			if (const auto ref = Find(held.refId); ref && ref->Is3DLoaded()) {
				const auto read = RefRules::Shared(Read(ref), RefRules::Of(FactsOf(ref)));
				if (book.News(read)) {
					Report(read, a_send);
				}
			}
		}
		if (now >= nextRetry && pending.Size() != 0) {
			nextRetry = now + RefRules::RETRY_EVERY;
			for (const auto& want : pending.All()) {
				if (Apply(want)) {
					pending.Remove(want.refId);
				}
			}
		}
	}

	void Heard(const RefState& a_state)
	{
		if (!RefRules::Shareable(a_state.refId)) {
			return;  // a reference this game lacks (FormTranslate made it 0)
		}
		++counts.heard;
		if (!book.Heard(a_state, ourId)) {
			++counts.stale;
			return;
		}
		const auto want = book.Known(a_state.refId).value_or(a_state);
		if (!LocalPlayer::InWorld() || !Apply(want)) {
			++counts.waited;
			pending.Add(want);
		} else {
			pending.Remove(want.refId);
		}
	}

	void Used(const Protocol::Activation& a_use)
	{
		const auto ref = RefRules::Shareable(a_use.refId) ? Find(a_use.refId) : nullptr;
		const auto standIn = RemotePlayers::StandInOf(a_use.playerId);
		const auto facts = ref ? FactsOf(ref) : RefRules::Facts{};
		const auto verdict = RefRules::JudgeUse({
			.loaded = ref && ref->Is3DLoaded() && !ref->IsDisabled(),
			.mechanism = RefRules::Of(facts).use,
			.standIn = standIn != nullptr,
			.standInDistance = ref && standIn ? Distance(standIn, ref) : -1.0f,
		});
		if (verdict != RefRules::UseVerdict::kUse) {
			++counts.usesRefused;
			lastRefused = RefRules::UseVerdictName(verdict);
			logs::info("World: player {} used {:08X}; not made here: {}", a_use.playerId, a_use.refId, lastRefused);
			return;
		}
		// As the activate key does for a player, with the stand-in as the one who uses it: the mechanism's own script runs
		// here once, as it ran in the user's game. The stand-in's use is no use of our player's: it is not reported.
		ref->ActivateRef(standIn, 0, nullptr, 0, false);
		++counts.usesMade;
		logs::info("World: player {} used {:08X} ({}); their stand-in used it here", a_use.playerId, a_use.refId, RefRules::KindName(facts));
	}

	void AfterLoad()
	{
		book.Clear();
		outbox.Clear();
		pending.Clear();
		TakeQueue();
		askAgain = ourId != 0;
	}

	void Clear()
	{
		ourId = 0;
		book.Clear();
		outbox.Clear();
		pending.Clear();
		askAgain = false;
		TakeQueue();
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.empty()) {
			return std::format("installed={} book={} outstanding={} pending={} held={} events={} sent={} paced={} heard={} applied={} same={} waited={} stale={} "
							   "dropped={} uses={}/{}/{} lastRefused={} asks={}",
				installed ? 1 : 0, book.Size(), book.Outstanding(), pending.Size(), outbox.Held(), Counted(counts.events), Counted(counts.sent), Counted(counts.paced),
				Counted(counts.heard), Counted(counts.applied), Counted(counts.same), Counted(counts.waited), Counted(counts.stale), Counted(pending.Dropped()),
				Counted(counts.usesSent), Counted(counts.usesMade), Counted(counts.usesRefused), lastRefused, Counted(counts.asks));
		}
		if (words[0] != "near" || words.size() > 2) {
			return "error: usage: refs [near [radius]]";
		}
		const auto radius = words.size() == 2 ? DevLine::Number(words[1]) : std::optional<std::uint64_t>{ 3000 };
		if (!radius || *radius == 0 || *radius > 20000) {
			return "error: usage: refs near [radius 1-20000]";
		}
		const auto player = Player();
		const auto tes = RE::TES::GetSingleton();
		if (!player || !tes || !LocalPlayer::InWorld()) {
			return "error: not in the world";
		}
		// This class's references within the radius, nearest first, at most 40 (one line each, " | " between).
		std::vector<std::pair<float, RE::TESObjectREFR*>> found;
		tes->ForEachReferenceInRange(player, static_cast<float>(*radius), [&](RE::TESObjectREFR* a_ref) {
			if (a_ref && a_ref != player && RefRules::Of(FactsOf(a_ref)).Any()) {
				found.emplace_back(Distance(player, a_ref), a_ref);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::ranges::sort(found, {}, &std::pair<float, RE::TESObjectREFR*>::first);
		std::string out = std::format("near={}", found.size());
		for (std::size_t i = 0; i < found.size() && i < 40; ++i) {
			out += " | " + Line(found[i].second);
		}
		return out;
	}

	std::string One(std::string_view a_args)
	{
		const auto id = FormsCommand::ParseHex(a_args);
		const auto ref = id ? Find(*id) : nullptr;
		if (!ref) {
			return "error: usage: ref <hex id of a reference loaded here>";
		}
		return Line(ref);
	}

	std::string Use(std::string_view a_args)
	{
		const auto id = FormsCommand::ParseHex(a_args);
		const auto ref = id ? Find(*id) : nullptr;
		const auto player = Player();
		if (!ref || !player || !ref->Is3DLoaded()) {
			return "error: usage: use <hex id of a reference loaded here>";
		}
		logs::info("World: the developer channel has our player use {:08X}", ref->GetFormID());
		return ref->ActivateRef(player, 0, nullptr, 0, false) ? "used" : "error: the game refused it";
	}
}
