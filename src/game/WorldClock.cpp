#include "game/WorldClock.h"

#include "DevLine.h"
#include "game/ClockRules.h"
#include "game/LocalPlayer.h"
#include "net/FormsCommand.h"

namespace WorldClock
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::uint32_t                       ourId = 0;
		std::optional<Protocol::ClockState> heard;  // the owner's last clock
		Clock::time_point                   heardAt{};
		bool                                fresh = false;  // heard and not yet followed (a move under way finishes first)
		ClockRules::Follower                follower;
		ClockRules::Reporter                reporter;
		ClockRules::WeatherFollower         weather;
		ClockRules::ScaleKeeper             scale;
		std::uint32_t                       heldSaid = 0;  // the weather last said held in the log (once each)
		std::uint64_t                       heardCount = 0;
		std::uint64_t                       reports = 0;
		std::uint64_t                       scaleBacks = 0;  // our own timescale put back (a save, the session's end)
		ClockRules::WaitGate                waitGate;        // our own Sleep/Wait: nothing followed while it runs
		bool                                openingSaid = false;  // the log last said the opening holds our clock
		std::uint64_t                       waitSaid = 0;          // the wait of ours the log last said holds the clock
		// The owner's weather pinned here as an override (ClockRules::WeatherMove): its form ID, 0 none.
		std::uint32_t                       pinned = 0;
		std::uint64_t                       saveUnpins = 0;  // pins (ours, or a friend's ask kept) taken off for a save
		// Our player's magic (TESMagicEffectApplyEvent, caster our player), counted on whatever thread sends the event
		// and read on the tick; and what it does to our sky (ClockRules::MagicWatch).
		std::atomic<std::uint64_t>          casts{ 0 };
		std::atomic<std::uint64_t>          castsSkipped{ 0 };  // our player's magic on others, or a drink: no window
		std::uint64_t                       castsSeen = 0;
		// Which magic opens the watch, read from the game's data once before the event sink is added (read only after).
		ClockRules::MagicKinds              kinds;
		ClockRules::MagicWatch              magic;
		// The owner's side: friends' weather magic asked of us (ClockRules::AskKeeper), and a kept one taken off for a
		// save, to put back on the next tick (its form ID, 0 none).
		ClockRules::AskKeeper               keeper;
		std::uint32_t                       keeperRepin = 0;
		// What a save keeps (ClockRules::SaveDays): the day count when this game joined the session or loaded in it.
		ClockRules::SaveDays                saveDays;
		bool                                magicInstalled = false;

		RE::Calendar* Calendar()
		{
			const auto calendar = RE::Calendar::GetSingleton();
			return calendar && calendar->gameHour && calendar->gameDaysPassed && calendar->timeScale ? calendar : nullptr;
		}

		// The Helgen opening (MQ101) runs: its scenes need the time and weather the game set them, and its clock is not
		// the session's. A start some mod gives the game (no MQ101 running) is no opening.
		RE::TESQuest* OpeningQuest()
		{
			static RE::TESQuest* const quest = [] {
				const auto data = RE::TESDataHandler::GetSingleton();
				if (!data) {
					return static_cast<RE::TESQuest*>(nullptr);
				}
				for (const auto candidate : data->GetFormArray<RE::TESQuest>()) {
					if (candidate && std::string_view{ candidate->formEditorID.c_str() } == "MQ101") {
						return candidate;
					}
				}
				return static_cast<RE::TESQuest*>(nullptr);
			}();
			return quest;
		}

		// The opening's quest as this game has it, and whether it holds our clock (ClockRules::OpeningHolds).
		struct Opening
		{
			bool          found = false;
			bool          running = false;
			bool          completed = false;
			std::uint16_t stage = 0;
			bool          canMove = true;
			bool          holds = false;
		};

		Opening ReadOpening()
		{
			Opening    opening;
			const auto quest = OpeningQuest();
			if (!quest) {
				return opening;
			}
			const auto controls = RE::ControlMap::GetSingleton();
			opening.found = true;
			opening.running = quest->IsRunning();
			opening.completed = quest->IsCompleted();
			opening.stage = quest->GetCurrentStageID();
			opening.canMove = !controls || controls->IsMovementControlsEnabled();
			opening.holds = ClockRules::OpeningHolds(opening.running, opening.completed, opening.stage, opening.canMove);
			return opening;
		}

		bool InOpening()
		{
			return ReadOpening().holds;
		}

		bool Ready()
		{
			return LocalPlayer::InWorld() && Calendar() && !InOpening();
		}

		// The log says each time the opening starts or stops holding our clock, with what it read: a game that never
		// follows or says a clock is never silent about why.
		void SayOpening(const Opening& a_opening)
		{
			if (a_opening.holds == openingSaid) {
				return;
			}
			openingSaid = a_opening.holds;
			if (a_opening.holds) {
				logs::info("Clock: the Helgen opening holds this game's clock (MQ101 running at stage {}, movement {}): its own time and weather, and nothing said, until it ends",
					a_opening.stage, a_opening.canMove ? "on" : "off");
			} else {
				logs::info("Clock: the Helgen opening no longer holds this game's clock (MQ101 running={} completed={} stage {}, movement {}): the session's clock from now on",
					a_opening.running ? 1 : 0, a_opening.completed ? 1 : 0, a_opening.stage, a_opening.canMove ? "on" : "off");
			}
		}

		bool SleepWaitOpen()
		{
			const auto ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::SleepWaitMenu::MENU_NAME);
		}

		// The worldspace the player stands in, and the one whose sky it has (a walled city's is Tamriel's).
		RE::TESWorldSpace* SkyOf(RE::TESWorldSpace* a_world)
		{
			return ClockRules::SkyWorld(a_world, [](RE::TESWorldSpace* a_child) -> RE::TESWorldSpace* {
				return a_child->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseClimateData) ? a_child->parentWorld : nullptr;
			});
		}

		ClockRules::Sky HereSky()
		{
			ClockRules::Sky here;
			const auto      player = RE::PlayerCharacter::GetSingleton();
			const auto      sky = RE::Sky::GetSingleton();
			const auto      cell = player ? player->GetParentCell() : nullptr;
			const auto      world = player ? player->GetWorldspace() : nullptr;
			if (!sky || !cell) {
				return here;
			}
			here.weather = sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
			here.overrideWeather = sky->overrideWeather ? sky->overrideWeather->GetFormID() : 0;
			here.pinned = here.overrideWeather != 0 && here.overrideWeather == pinned;
			here.overridden = here.overrideWeather != 0 && !here.pinned;
			if (!cell->IsInteriorCell() && world && sky->mode.get() == RE::Sky::Mode::kFull) {
				here.outside = true;
				here.worldspace = SkyOf(world)->GetFormID();
			}
			return here;
		}

		// The worldspace the player stands in (0 inside), for the dev line: the rig tells a walled city from its sky by it.
		std::uint32_t HerePlace()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			const auto world = player ? player->GetWorldspace() : nullptr;
			return cell && !cell->IsInteriorCell() && world ? world->GetFormID() : 0;
		}

		Protocol::ClockState Ours(RE::Calendar* a_calendar)
		{
			Protocol::ClockState ours;
			ours.gameHour = std::clamp(a_calendar->gameHour->value, 0.0f, 23.999f);
			ours.daysPassed = std::clamp(a_calendar->gameDaysPassed->value, 0.0f, Protocol::MAX_DAYS_PASSED);
			ours.timeScale = std::clamp(a_calendar->timeScale->value, 0.0f, Protocol::MAX_TIME_SCALE);
			if (const auto here = HereSky(); here.outside) {
				ours.worldspace = here.worldspace;
				ours.weather = here.weather;
			}
			return ours;
		}

		// Our clock moved by the rules: the hour, and the day count with it (the game keeps the two together as time
		// passes; a console "set gamehour" moves the hour alone).
		void Shift(RE::Calendar* a_calendar, const ClockRules::Set& a_set, std::string_view a_why)
		{
			const auto from = a_calendar->gameHour->value;
			a_calendar->gameHour->value = a_set.hour;
			a_calendar->gameDaysPassed->value = (std::max)(0.0f, a_calendar->gameDaysPassed->value + static_cast<float>(a_set.days));
			logs::info("Clock: {} {:.2f} -> {:.2f} (player {}'s clock)", a_why, from, a_set.hour, follower.Owner());
		}

		void SetScale(RE::Calendar* a_calendar, float a_value, std::string_view a_why)
		{
			logs::info("Clock: timescale {:.2f} -> {:.2f} ({})", a_calendar->timeScale->value, a_value, a_why);
			a_calendar->timeScale->value = a_value;
		}

		// Our pin let go, the game's own way (its climate takes over): on leaving the owner's sky, the owner gone quiet or
		// gone, the session's end. Only when the override is still ours.
		void ReleasePin(std::string_view a_why)
		{
			const auto sky = RE::Sky::GetSingleton();
			if (pinned != 0 && sky && sky->overrideWeather && sky->overrideWeather->GetFormID() == pinned) {
				sky->ReleaseWeatherOverride();
				logs::info("Clock: weather {:08X} no longer held here ({})", pinned, a_why);
			}
			pinned = 0;
		}

		// An override of ours (the pin, or a friend's ask kept) taken off without the release (which hands the sky to the
		// climate): for a save, or a moment of our magic's. Put back on a later tick.
		bool QuietUnpin(std::uint32_t a_weather)
		{
			const auto sky = RE::Sky::GetSingleton();
			if (a_weather == 0 || !sky || !sky->overrideWeather || sky->overrideWeather->GetFormID() != a_weather) {
				return false;
			}
			sky->overrideWeather = nullptr;
			return true;
		}

		// The current weather held as an override, with no transition (it is the current one already).
		bool PinCurrent(std::uint32_t a_weather)
		{
			const auto sky = RE::Sky::GetSingleton();
			if (!sky || sky->overrideWeather || !sky->currentWeather || sky->currentWeather->GetFormID() != a_weather) {
				return false;
			}
			sky->overrideWeather = sky->currentWeather;
			return true;
		}

		void FollowWeather(const Protocol::ClockState& a_owner, Clock::time_point a_now)
		{
			const auto sky = RE::Sky::GetSingleton();
			if (!sky) {
				return;
			}
			if (magic.Holds()) {
				// Our player's magic may be changing our sky: nothing followed or pinned until it has (ClockRules::MagicWatch).
				if (QuietUnpin(pinned)) {
					logs::info("Clock: weather {:08X} not held while our player's magic takes hold", pinned);
				}
				pinned = 0;
				return;
			}
			const auto here = HereSky();
			if (!here.pinned) {
				pinned = 0;  // a script of ours replaced the pin, or let it go: not ours any more
			}
			const auto move = weather.Decide(a_owner, here, a_now);
			if (move == ClockRules::WeatherMove::kNone) {
				return;
			}
			if (move == ClockRules::WeatherMove::kRelease) {
				ReleasePin("not under player " + std::to_string(a_owner.playerId) + "'s sky");
				return;
			}
			if (move == ClockRules::WeatherMove::kPin) {
				if (PinCurrent(a_owner.weather)) {
					pinned = a_owner.weather;
					logs::info("Clock: weather {:08X} held against this game's climate (player {}'s sky)", pinned, a_owner.playerId);
				}
				return;
			}
			if (move == ClockRules::WeatherMove::kHeld) {
				if (std::exchange(heldSaid, a_owner.weather) != a_owner.weather) {
					logs::info("Clock: player {}'s weather {:08X} is not set here: a script of this game forced {:08X}", a_owner.playerId, a_owner.weather, here.weather);
				}
				return;
			}
			const auto target = RE::TESForm::LookupByID<RE::TESWeather>(a_owner.weather);
			if (!target) {
				return;  // not a weather here (the edge kept its ID, but the form is another kind): nothing to follow
			}
			// Set as an override (the pin): this game's climate leaves it alone until the owner's sky turns.
			if (move == ClockRules::WeatherMove::kForce) {
				sky->ForceWeather(target, true);
			} else {
				sky->SetWeather(target, true, true);
			}
			pinned = sky->overrideWeather == target ? a_owner.weather : 0;
			logs::info("Clock: weather {:08X} -> {:08X} {} (player {}'s sky{})", here.weather, a_owner.weather, move == ClockRules::WeatherMove::kForce ? "forced" : "easing in",
				a_owner.playerId, pinned != 0 ? ", held" : ", not held: the game took no override");
		}

		// Our player's magic and what it did to our sky (ClockRules::MagicWatch), while we follow an owner's: a weather it
		// made is asked of the owner, and let go there when it ends.
		void WatchMagic(Clock::time_point a_now, const SendFn& a_send)
		{
			const auto seen = casts.load(std::memory_order_relaxed);
			const bool cast = seen != castsSeen;
			castsSeen = seen;
			if (!heard) {
				return;
			}
			if (cast) {
				magic.Cast(a_now, heard->worldspace);
			}
			const auto step = magic.Look(a_now, HereSky(), heard->weather);
			if (step.kind == ClockRules::MagicStep::Kind::kNone) {
				return;
			}
			Protocol::WeatherAsk ask;
			ask.worldspace = step.worldspace;
			ask.weather = step.weather;
			ask.held = step.held;
			ask.release = step.kind == ClockRules::MagicStep::Kind::kRelease;
			a_send(Protocol::Encode(ask));
			if (ask.release) {
				logs::info("Clock: our player's weather magic {:08X} ended: player {} is told to let it go", step.weather, heard->playerId);
			} else {
				logs::info("Clock: our player's magic made the weather {:08X} ({}): asked of player {}, whose sky is the session's", step.weather,
					step.held ? "held until it ends" : "not held", heard->playerId);
			}
		}

		// The magic's ask let go, said to the owner if one is asked and held (the owner went quiet or left, a load).
		void DropMagic(const SendFn* a_send)
		{
			if (const auto held = magic.Asked(); held && a_send) {
				Protocol::WeatherAsk ask;
				ask.release = true;
				(*a_send)(Protocol::Encode(ask));
				logs::info("Clock: our player's weather magic {:08X} no longer asked of anyone", *held);
			}
			magic.Reset();
		}

		// The owner's side, each tick: a friend's kept ask put back after a save, given up past its time, or forgotten
		// when a script of ours took the sky.
		void KeepAsks(Clock::time_point a_now)
		{
			if (keeperRepin != 0) {
				if (PinCurrent(keeperRepin)) {
					logs::info("Clock: player {}'s weather {:08X} held again after the save", keeper.By(), keeperRepin);
				}
				keeperRepin = 0;
			}
			const auto by = keeper.By();
			if (const auto move = keeper.Tick(a_now, HereSky()); move.kind == ClockRules::AskMove::Kind::kRelease) {
				if (const auto sky = RE::Sky::GetSingleton()) {
					sky->ReleaseWeatherOverride();
				}
				logs::info("Clock: player {}'s weather {:08X} let go: held {} minutes, its magic never said it ended", by, move.weather,
					std::chrono::duration_cast<std::chrono::minutes>(ClockRules::MAGIC_HOLD_MAX).count());
			}
		}

		class MagicSink : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
			{
				const auto player = RE::PlayerCharacter::GetSingleton();
				if (!a_event || !player || !a_event->caster || a_event->caster.get() != player) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// Only magic our player cast on itself, and not a drink (ClockRules::MagicKinds): a fight's enchanted hits
				// and potions leave our sky followed.
				if (kinds.Opens(true, a_event->target && a_event->target.get() == player, a_event->magicEffect)) {
					casts.fetch_add(1, std::memory_order_relaxed);
				} else {
					castsSkipped.fetch_add(1, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MagicSink magicSink;

		// The owner's clock followed, this tick: a move forward under way, the owner's report not yet followed, its
		// timescale and its sky. Never while our own Sleep/Wait runs (Frame).
		void FollowOwner(RE::Calendar* a_calendar, Clock::time_point a_now)
		{
			if (const auto set = follower.Advance(a_calendar->gameHour->value)) {
				Shift(a_calendar, *set, set->hour >= ClockRules::BEFORE_MIDNIGHT ? "forward to midnight" : "forward");
			}
			if (!heard) {
				return;
			}
			if (fresh && !follower.Moving()) {
				fresh = false;
				if (const auto set = follower.Follow(*heard, a_calendar->gameHour->value, a_calendar->gameDaysPassed->value)) {
					Shift(a_calendar, *set, set->days < 0.0 ? "back" : set->hour >= ClockRules::BEFORE_MIDNIGHT ? "forward to midnight" : "forward");
				}
			}
			if (const auto value = scale.Follow(heard->timeScale, a_calendar->timeScale->value)) {
				SetScale(a_calendar, *value, std::format("player {}'s", heard->playerId));
			}
			FollowWeather(*heard, a_now);
		}

		// Dev `clock weather <hex>`: this game's own weather changed as its climate changes it (Sky::ForceWeather, not an
		// override), for the rig: a change no script holds, so the session's weather may undo it.
		std::string ForceOwn(std::string_view a_id)
		{
			const auto id = FormsCommand::ParseHex(a_id);
			const auto sky = RE::Sky::GetSingleton();
			const auto target = id ? RE::TESForm::LookupByID<RE::TESWeather>(*id) : nullptr;
			if (!sky || !target) {
				return "error: usage: clock weather <weather form id, hex>";
			}
			sky->ForceWeather(target, false);
			return std::format("weather={:08X} forced override={}", *id, sky->overrideWeather ? 1 : 0);
		}

		// Dev `clock release`: any weather a script or the console forced as an override let go (the rig, at the end of
		// its weather legs).
		std::string Release()
		{
			const auto sky = RE::Sky::GetSingleton();
			if (!sky) {
				return "error: no sky";
			}
			const bool had = sky->overrideWeather != nullptr;
			if (had) {
				sky->ReleaseWeatherOverride();
			}
			return std::format("released={} override={}", had ? 1 : 0, sky->overrideWeather ? 1 : 0);
		}

		bool Kind(RE::TESWeather* a_weather, std::string_view a_kind)
		{
			using F = RE::TESWeather::WeatherDataFlag;
			const auto flags = a_weather->data.flags;
			if (a_kind == "clear") {
				return flags.any(F::kPleasant);
			}
			if (a_kind == "cloudy") {
				return flags.any(F::kCloudy);
			}
			if (a_kind == "rain") {
				return flags.any(F::kRainy);
			}
			if (a_kind == "snow") {
				return flags.any(F::kSnow);
			}
			return false;
		}

		// The a_skip-th weather of Skyrim.esm of that kind (lowest form ID first) that is not the current one: the same in
		// every game whose base game is the same.
		std::string Pick(std::string_view a_kind, std::uint64_t a_skip)
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			const auto sky = RE::Sky::GetSingleton();
			if (!data) {
				return "error: no game data yet";
			}
			if (a_kind != "clear" && a_kind != "cloudy" && a_kind != "rain" && a_kind != "snow") {
				return "error: usage: clock pick <clear|cloudy|rain|snow> [n]";
			}
			std::vector<RE::TESWeather*> found;
			for (const auto candidate : data->GetFormArray<RE::TESWeather>()) {
				const auto file = candidate ? candidate->GetFile(0) : nullptr;
				if (file && Protocol::SamePluginName(file->GetFilename(), "Skyrim.esm") && Kind(candidate, a_kind) &&
					!(sky && sky->currentWeather == candidate)) {
					found.push_back(candidate);
				}
			}
			std::ranges::sort(found, {}, [](const RE::TESWeather* a_weather) { return a_weather->GetFormID(); });
			if (a_skip >= found.size()) {
				return std::format("error: {} weathers of that kind, none at {}", found.size(), a_skip);
			}
			return std::format("weather={:08X} kind={} of={}", found[a_skip]->GetFormID(), a_kind, found.size());
		}

		std::string Describe()
		{
			const auto c = Calendar();
			if (!c) {
				return "ready=0 calendar=0";
			}
			const auto here = HereSky();
			const auto kept = scale.Kept();
			const auto age = heard ? std::chrono::duration<double>(Clock::now() - heardAt).count() : -1.0;
			const auto opening = ReadOpening();
			return std::format(
				"ready={} opening={} hour={:.3f} days={:.4f} raw={:.4f} midnights={} day={} month={} scale={:.2f} own={} outside={} world={:08X} weather={:08X} "
				"override={} owner={} heard={} heardAge={:.1f} ownerHour={} ownerWeather={:08X} offset={} moving={:.3f} reports={} lineUps={} hourSets={} "
				"backSets={} midnightSteps={} forced={} eased={} held={} scaleSets={} scaleBacks={} place={:08X} waiting={} waits={} mq101={} canMove={} "
				"pinned={} pins={} pinReleases={} saveUnpins={} magic={} magicWindows={} magicSkipped={} asks={} askReleases={} magicAsked={:08X} asked={:08X} askedBy={} askSets={} askIgnored={} "
				"askLetGo={} joinedDays={}",
				Ready() ? 1 : 0, opening.holds ? 1 : 0, c->gameHour->value, c->gameDaysPassed->value, c->rawDaysPassed, c->midnightsPassed,
				c->gameDay ? c->gameDay->value : 0.0f, c->gameMonth ? c->gameMonth->value + 1.0f : 0.0f, c->timeScale->value,
				kept ? std::format("{:.2f}", *kept) : std::string{ "-" }, here.outside ? 1 : 0, here.worldspace, here.weather, here.overridden ? 1 : 0,
				heard ? heard->playerId : 0, heardCount, age, heard ? std::format("{:.3f}", heard->gameHour) : std::string{ "-" }, heard ? heard->weather : 0,
				follower.Offset() ? std::format("{:.4f}", *follower.Offset()) : std::string{ "-" }, follower.Left(), reports, follower.lineUps, follower.hourSets,
				follower.backSets, follower.midnightSteps, weather.forced, weather.eased, weather.held, scale.sets, scaleBacks, HerePlace(), waitGate.Open() ? 1 : 0,
				waitGate.waits,
				opening.found ? std::format("{}/{}/{}", opening.running ? "running" : "stopped", opening.completed ? "completed" : "open", opening.stage) : std::string{ "-" },
				opening.canMove ? 1 : 0, here.pinned ? 1 : 0, weather.pins, weather.releases, saveUnpins, magic.Holds() ? 1 : 0, magic.windows, castsSkipped.load(std::memory_order_relaxed), magic.asks, magic.releases, magic.Asked().value_or(0),
				keeper.Weather(), keeper.By(), keeper.sets, keeper.ignored, keeper.releases,
				saveDays.joined ? std::format("{:.3f}", *saveDays.joined) : std::string{ "-" });
		}
	}

	void Welcomed(std::uint32_t a_playerId)
	{
		ourId = a_playerId;
		heard.reset();
		fresh = false;
		follower.Rebaseline();
		weather.Reset();
		reporter.Reset();  // ours goes on the first tick in the world
		waitGate.Reset();
		magic.Reset();
		openingSaid = false;  // a new session says again whether the opening holds our clock
	}

	void Frame(const SendFn& a_send)
	{
		const auto calendar = Calendar();
		if (!calendar || !LocalPlayer::InWorld()) {
			return;
		}
		const auto opening = ReadOpening();
		SayOpening(opening);
		if (opening.holds) {
			return;
		}
		const auto now = Clock::now();
		saveDays.Joined(calendar->gameDaysPassed->value);
		if (heard && now - heardAt > ClockRules::HEARD_LASTS) {
			DropMagic(&a_send);
			heard.reset();  // the owner went quiet: our own clock until it speaks again
			fresh = false;
			weather.Reset();
			ReleasePin("the owner went quiet");
		}
		KeepAsks(now);
		WatchMagic(now, a_send);
		// Our own Sleep/Wait runs: the engine passes the hours and ends the wait by the clock, so nothing of ours moves it
		// (the hour, the day count, the timescale, the sky) until the menu shuts; then the owner's last report is followed
		// from where the wait left us. Our clock is still said meanwhile (the server passes on only the owner's).
		switch (waitGate.Next(SleepWaitOpen())) {
		case ClockRules::WaitStep::kHold:
			if (waitGate.waits != waitSaid) {
				waitSaid = waitGate.waits;
				logs::info("Clock: our own sleep or wait runs at {:.2f}: nothing of the session's clock is followed until it ends", calendar->gameHour->value);
			}
			break;
		case ClockRules::WaitStep::kShut:
			follower.Drop();
			fresh = heard.has_value();
			logs::info("Clock: our own sleep or wait ended at {:.2f}: {}", calendar->gameHour->value,
				heard ? std::format("player {}'s clock followed again from here", heard->playerId) : std::string{ "no clock to follow" });
			[[fallthrough]];
		case ClockRules::WaitStep::kRun:
			FollowOwner(calendar, now);
			break;
		}
		magic.Seen(HereSky());  // the sky as this tick leaves it: what a cast next tick is compared with
		const auto ours = Ours(calendar);
		if (reporter.Due(now, ours)) {
			reporter.Sent(now, ours);
			++reports;
			a_send(Protocol::Encode(ours));
		}
	}

	void Heard(const Protocol::ClockState& a_clock)
	{
		if (a_clock.playerId == ourId) {
			return;
		}
		heard = a_clock;
		heardAt = Clock::now();
		fresh = true;
		++heardCount;
	}

	void Left(std::uint32_t a_playerId)
	{
		if (heard && heard->playerId == a_playerId) {
			heard.reset();
			fresh = false;
			weather.Reset();
			magic.Reset();  // its ask went with it
			ReleasePin("its owner left");
			logs::info("Clock: player {}, whose clock this game followed, left: our own until the next owner's", a_playerId);
		}
		// A friend whose weather magic we hold left: let it go.
		const auto kept = keeper.Weather();
		if (const auto move = keeper.Left(a_playerId, HereSky()); move.kind == ClockRules::AskMove::Kind::kRelease) {
			if (const auto sky = RE::Sky::GetSingleton()) {
				sky->ReleaseWeatherOverride();
			}
			logs::info("Clock: player {} left: its weather {:08X} let go", a_playerId, kept);
		}
	}

	void HeardAsk(const Protocol::WeatherAsk& a_ask)
	{
		const auto sky = RE::Sky::GetSingleton();
		if (!sky || a_ask.playerId == ourId) {
			return;
		}
		const auto before = keeper.Weather();
		const auto here = HereSky();
		const auto move = keeper.Heard(a_ask, here, Clock::now());
		switch (move.kind) {
		case ClockRules::AskMove::Kind::kNone:
			break;
		case ClockRules::AskMove::Kind::kIgnore:
			logs::info("Clock: player {}'s weather magic {:08X} not set here: this game is not under that sky (outside={} world={:08X}, asked {:08X})", a_ask.playerId,
				a_ask.weather, here.outside ? 1 : 0, here.worldspace, a_ask.worldspace);
			break;
		case ClockRules::AskMove::Kind::kRelease:
			sky->ReleaseWeatherOverride();
			logs::info("Clock: player {}'s weather magic ended: {:08X} let go", a_ask.playerId, move.weather);
			break;
		case ClockRules::AskMove::Kind::kSet:
			if (const auto target = RE::TESForm::LookupByID<RE::TESWeather>(move.weather)) {
				if (!move.held && before != 0 && here.overrideWeather == before) {
					sky->ReleaseWeatherOverride();  // a weather not held replaces a friend's held one: the climate goes on from it
				}
				sky->SetWeather(target, move.held, true);
				logs::info("Clock: player {}'s weather magic: {:08X} -> {:08X} ({}): the session's sky", a_ask.playerId, here.weather, move.weather,
					move.held ? "held until it ends" : "not held");
			} else {
				keeper.Forget();
			}
			break;
		}
	}

	void BeforeSave()
	{
		if (const auto calendar = Calendar()) {
			if (const auto own = scale.Own(calendar->timeScale->value)) {
				SetScale(calendar, *own, "our own, for the save");
				++scaleBacks;
			}
			// What the save keeps of the session's clock (ClockRules::SaveDays): its hour and day count, never put back.
			if (const auto since = saveDays.Since(calendar->gameDaysPassed->value); since && ourId != 0) {
				logs::info("Clock: this save keeps the session's hour {:.2f} and day count {:.3f} ({:+.3f} days since this game joined the session or loaded)",
					calendar->gameHour->value, calendar->gameDaysPassed->value, *since);
			}
		}
		// No override of ours goes into the save (no script would ever let it go after a load): taken off quietly, put
		// back on the next tick.
		if (QuietUnpin(pinned)) {
			++saveUnpins;
			logs::info("Clock: weather {:08X} not held in the save (held again after it)", pinned);
		}
		pinned = 0;
		if (QuietUnpin(keeper.Weather())) {
			++saveUnpins;
			keeperRepin = keeper.Weather();
			logs::info("Clock: player {}'s weather {:08X} not held in the save (held again after it)", keeper.By(), keeperRepin);
		}
	}

	void AfterLoad()
	{
		follower.Rebaseline();
		waitGate.Reset();
		scale.Forget();
		weather.Reset();
		reporter.Reset();
		pinned = 0;  // the loaded save holds no override of ours
		magic.Reset();
		keeper.Forget();
		keeperRepin = 0;
		saveDays.Reset();  // the loaded save's day count is the one this game is in the session with
		fresh = heard.has_value();  // the owner's last word, followed again from the loaded save's clock
	}

	void Clear()
	{
		if (const auto calendar = Calendar()) {
			if (const auto own = scale.Restore(calendar->timeScale->value)) {
				SetScale(calendar, *own, "our own again, the session ended");
				++scaleBacks;
			}
			if (const auto since = saveDays.Since(calendar->gameDaysPassed->value); since && ourId != 0) {
				logs::info("Clock: the session ended: this game keeps the session's hour {:.2f} and day count {:.3f} ({:+.3f} days since it joined), as a wait would",
					calendar->gameHour->value, calendar->gameDaysPassed->value, *since);
			}
		} else {
			scale.Forget();
		}
		ReleasePin("the session ended");
		if (const auto kept = keeper.Weather(); kept != 0) {
			const auto sky = RE::Sky::GetSingleton();
			if (sky && sky->overrideWeather && sky->overrideWeather->GetFormID() == kept) {
				sky->ReleaseWeatherOverride();
				logs::info("Clock: player {}'s weather {:08X} let go: the session ended", keeper.By(), kept);
			}
		}
		keeper.Forget();
		keeperRepin = 0;
		magic.Reset();
		saveDays.Reset();
		heard.reset();
		fresh = false;
		ourId = 0;
		follower.Rebaseline();
		waitGate.Reset();
		weather.Reset();
		reporter.Reset();
	}

	bool Install()
	{
		if (magicInstalled) {
			return true;
		}
		const auto events = RE::ScriptEventSourceHolder::GetSingleton();
		if (!events) {
			logs::warn("Clock: the game's event source is not there; a friend's weather magic stays in their own game");
			return false;
		}
		// The effects only alchemy carries (ClockRules::MagicKinds), before the sink reads them.
		if (const auto data = RE::TESDataHandler::GetSingleton()) {
			const auto add = [&](RE::MagicItem* a_item, bool a_alchemy) {
				if (!a_item) {
					return;
				}
				for (const auto effect : a_item->effects) {
					const auto base = effect ? effect->baseEffect : nullptr;
					if (base) {
						a_alchemy ? kinds.Alchemy(base->GetFormID()) : kinds.Other(base->GetFormID());
					}
				}
			};
			for (const auto item : data->GetFormArray<RE::SpellItem>()) {
				add(item, false);
			}
			for (const auto item : data->GetFormArray<RE::ScrollItem>()) {
				add(item, false);
			}
			for (const auto item : data->GetFormArray<RE::EnchantmentItem>()) {
				add(item, false);
			}
			for (const auto item : data->GetFormArray<RE::AlchemyItem>()) {
				add(item, true);
			}
			for (const auto item : data->GetFormArray<RE::IngredientItem>()) {
				add(item, true);
			}
		}
		events->AddEventSink<RE::TESMagicEffectApplyEvent>(&magicSink);
		magicInstalled = true;
		logs::info("Clock: watching our player's magic on itself (a weather it makes is asked of the session's clock owner); {} effects only drinks carry are not watched",
			kinds.AlchemyOnly());
		return true;
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.empty() || words[0] == "status") {
			return Describe();
		}
		if (words[0] == "pick" && words.size() >= 2) {
			const auto skip = words.size() >= 3 ? DevLine::Number(words[2]) : std::optional<std::uint64_t>{ 0 };
			if (!skip) {
				return "error: usage: clock pick <clear|cloudy|rain|snow> [n]";
			}
			return Pick(words[1], *skip);
		}
		if (words[0] == "weather" && words.size() >= 2) {
			return ForceOwn(words[1]);
		}
		if (words[0] == "release") {
			return Release();
		}
		return "error: usage: clock [status] | clock pick <clear|cloudy|rain|snow> [n] | clock weather <hex> | clock release";
	}
}
