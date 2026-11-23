#include "game/WorldClock.h"

#include "DevLine.h"
#include "game/ClockRules.h"
#include "game/LocalPlayer.h"

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

		bool InOpening()
		{
			const auto quest = OpeningQuest();
			return quest && quest->IsRunning() && !quest->IsCompleted();
		}

		bool Ready()
		{
			return LocalPlayer::InWorld() && Calendar() && !InOpening();
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
				here.worldspace = world->GetFormID();
			}
			return here;
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
			return std::format(
				"ready={} opening={} hour={:.3f} days={:.4f} raw={:.4f} midnights={} day={} month={} scale={:.2f} own={} outside={} world={:08X} weather={:08X} "
				"override={} owner={} heard={} heardAge={:.1f} ownerHour={} ownerWeather={:08X} offset={} moving={:.3f} reports={} lineUps={} hourSets={} "
				"backSets={} midnightSteps={} forced={} eased={} held={} scaleSets={} scaleBacks={}",
				Ready() ? 1 : 0, InOpening() ? 1 : 0, c->gameHour->value, c->gameDaysPassed->value, c->rawDaysPassed, c->midnightsPassed,
				c->gameDay ? c->gameDay->value : 0.0f, c->gameMonth ? c->gameMonth->value + 1.0f : 0.0f, c->timeScale->value,
				kept ? std::format("{:.2f}", *kept) : std::string{ "-" }, here.outside ? 1 : 0, here.worldspace, here.weather, here.overridden ? 1 : 0,
				heard ? heard->playerId : 0, heardCount, age, heard ? std::format("{:.3f}", heard->gameHour) : std::string{ "-" }, heard ? heard->weather : 0,
				follower.Offset() ? std::format("{:.4f}", *follower.Offset()) : std::string{ "-" }, follower.Left(), reports, follower.lineUps, follower.hourSets,
				follower.backSets, follower.midnightSteps, weather.forced, weather.eased, weather.held, scale.sets, scaleBacks);
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
	}

	void Frame(const SendFn& a_send)
	{
		const auto calendar = Calendar();
		if (!calendar || !Ready()) {
			return;
		}
		const auto now = Clock::now();
		if (heard && now - heardAt > ClockRules::HEARD_LASTS) {
			heard.reset();  // the owner went quiet: our own clock until it speaks again
			fresh = false;
			weather.Reset();
		}
		if (const auto set = follower.Advance(calendar->gameHour->value)) {
			Shift(calendar, *set, set->hour >= ClockRules::BEFORE_MIDNIGHT ? "forward to midnight" : "forward");
		}
		if (heard) {
			if (fresh && !follower.Moving()) {
				fresh = false;
				if (const auto set = follower.Follow(*heard, calendar->gameHour->value, calendar->gameDaysPassed->value)) {
					Shift(calendar, *set, set->days < 0.0 ? "back" : set->hour >= ClockRules::BEFORE_MIDNIGHT ? "forward to midnight" : "forward");
				}
			}
			if (const auto value = scale.Follow(heard->timeScale, calendar->timeScale->value)) {
				SetScale(calendar, *value, std::format("player {}'s", heard->playerId));
			}
			FollowWeather(*heard, now);
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
		return "error: usage: clock [status] | clock pick <clear|cloudy|rain|snow> [n]";
	}
}
