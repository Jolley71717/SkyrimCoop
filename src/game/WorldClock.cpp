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
			here.overridden = sky->overrideWeather != nullptr;
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

		void FollowWeather(const Protocol::ClockState& a_owner, Clock::time_point a_now)
		{
			const auto sky = RE::Sky::GetSingleton();
			if (!sky) {
				return;
			}
			const auto here = HereSky();
			const auto move = weather.Decide(a_owner, here, a_now);
			if (move == ClockRules::WeatherMove::kNone) {
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
			if (move == ClockRules::WeatherMove::kForce) {
				sky->ForceWeather(target, false);
			} else {
				sky->SetWeather(target, false, true);
			}
			logs::info("Clock: weather {:08X} -> {:08X} {} (player {}'s sky)", here.weather, a_owner.weather, move == ClockRules::WeatherMove::kForce ? "forced" : "easing in",
				a_owner.playerId);
		}

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
				"backSets={} midnightSteps={} forced={} eased={} held={} scaleSets={} scaleBacks={} place={:08X} waiting={} waits={} mq101={} canMove={}",
				Ready() ? 1 : 0, opening.holds ? 1 : 0, c->gameHour->value, c->gameDaysPassed->value, c->rawDaysPassed, c->midnightsPassed,
				c->gameDay ? c->gameDay->value : 0.0f, c->gameMonth ? c->gameMonth->value + 1.0f : 0.0f, c->timeScale->value,
				kept ? std::format("{:.2f}", *kept) : std::string{ "-" }, here.outside ? 1 : 0, here.worldspace, here.weather, here.overridden ? 1 : 0,
				heard ? heard->playerId : 0, heardCount, age, heard ? std::format("{:.3f}", heard->gameHour) : std::string{ "-" }, heard ? heard->weather : 0,
				follower.Offset() ? std::format("{:.4f}", *follower.Offset()) : std::string{ "-" }, follower.Left(), reports, follower.lineUps, follower.hourSets,
				follower.backSets, follower.midnightSteps, weather.forced, weather.eased, weather.held, scale.sets, scaleBacks, HerePlace(), waitGate.Open() ? 1 : 0,
				waitGate.waits,
				opening.found ? std::format("{}/{}/{}", opening.running ? "running" : "stopped", opening.completed ? "completed" : "open", opening.stage) : std::string{ "-" },
				opening.canMove ? 1 : 0);
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
		if (heard && now - heardAt > ClockRules::HEARD_LASTS) {
			heard.reset();  // the owner went quiet: our own clock until it speaks again
			fresh = false;
			weather.Reset();
		}
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
			logs::info("Clock: player {}, whose clock this game followed, left: our own until the next owner's", a_playerId);
		}
	}

	void BeforeSave()
	{
		if (const auto calendar = Calendar()) {
			if (const auto own = scale.Own(calendar->timeScale->value)) {
				SetScale(calendar, *own, "our own, for the save");
				++scaleBacks;
			}
		}
	}

	void AfterLoad()
	{
		follower.Rebaseline();
		waitGate.Reset();
		scale.Forget();
		weather.Reset();
		reporter.Reset();
		fresh = heard.has_value();  // the owner's last word, followed again from the loaded save's clock
	}

	void Clear()
	{
		if (const auto calendar = Calendar()) {
			if (const auto own = scale.Restore(calendar->timeScale->value)) {
				SetScale(calendar, *own, "our own again, the session ended");
				++scaleBacks;
			}
		} else {
			scale.Forget();
		}
		heard.reset();
		fresh = false;
		ourId = 0;
		follower.Rebaseline();
		waitGate.Reset();
		weather.Reset();
		reporter.Reset();
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
