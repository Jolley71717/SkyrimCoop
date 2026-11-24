#include "game/MapShare.h"

#include "Config.h"
#include "Text.h"
#include "game/LocalPlayer.h"
#include "game/MapRules.h"
#include "game/RemotePlayers.h"

#include <format>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace MapShare
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::vector<std::uint32_t>                      markers;  // the load order's map markers of places, found once
		bool                                            scanned = false;
		std::unordered_map<std::uint32_t, std::uint8_t> known;  // flags last read here: ours, and what we put on
		bool                                            baselined = false;
		std::map<std::uint32_t, std::uint8_t>           session;  // what the session found (theirs and ours), for after a load
		std::map<std::uint32_t, std::uint32_t>          finders;  // who found each heard marker (0: no notice)
		std::vector<std::uint32_t>                      toApply;  // the session's markers not yet put on our map
		MapRules::Feed                                  feed;
		Clock::time_point                               nextPoll{};
		std::uint64_t                                   reported = 0;
		std::uint64_t                                   heardCount = 0;
		std::uint64_t                                   applied = 0;
		std::uint64_t                                   failed = 0;
		std::uint64_t                                   notices = 0;

		RE::MapMarkerData* DataOf(std::uint32_t a_ref)
		{
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
			const auto extra = ref ? ref->extraList.GetByType<RE::ExtraMapMarker>() : nullptr;
			return extra ? extra->mapData : nullptr;
		}

		std::optional<std::uint8_t> FlagsOf(std::uint32_t a_ref)
		{
			const auto data = DataOf(a_ref);
			if (!data) {
				return std::nullopt;
			}
			std::uint8_t flags = 0;
			flags |= data->flags.all(RE::MapMarkerData::Flag::kVisible) ? Protocol::kMarkerVisible : 0;
			flags |= data->flags.all(RE::MapMarkerData::Flag::kCanTravelTo) ? Protocol::kMarkerTravel : 0;
			return flags;
		}

		std::string PlaceOf(std::uint32_t a_ref)
		{
			const auto data = DataOf(a_ref);
			const auto name = data ? data->locationName.GetFullName() : nullptr;
			return name ? name : "";
		}

		std::string NameOf(std::uint32_t a_playerId)
		{
			for (const auto& friendOf : RemotePlayers::Friends()) {
				if (friendOf.id == a_playerId) {
					return friendOf.name;
				}
			}
			return {};
		}

		// The markers of places (not a door's, a quest target's or the one the player sets): references of the load
		// order, loaded with the game's data and never deleted, so listed once.
		void Scan()
		{
			scanned = true;
			const auto [map, mapLock] = RE::TESForm::GetAllForms();
			if (!map) {
				return;
			}
			RE::BSReadLockGuard guard{ mapLock };
			for (const auto& [id, form] : *map) {
				if (!form || id >= 0xFF000000 || form->GetFormType() != RE::FormType::Reference) {
					continue;
				}
				const auto ref = static_cast<RE::TESObjectREFR*>(form);
				const auto extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
				if (extra && extra->mapData && extra->mapData->type.underlying() < static_cast<std::uint16_t>(RE::MARKER_TYPE::kTotalLocationTypes)) {
					markers.push_back(id);
				}
			}
			logs::info("Map: {} map markers of places in the load order", markers.size());
		}

		// The game's own way onto the map (the Papyrus function quests use), which the save keeps.
		bool AddToMap(std::uint32_t a_ref, bool a_travel)
		{
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			const auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!ref || !policy) {
				return false;
			}
			const auto handle = policy->GetHandleForObject(RE::TESObjectREFR::FORMTYPE, ref);
			if (handle == policy->EmptyHandle()) {
				return false;
			}
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			return vm->DispatchMethodCall2(handle, "ObjectReference", "AddToMap", RE::MakeFunctionArguments(static_cast<bool>(a_travel)), none);
		}

		// Our finds since the last read go to the session; the first read after a welcome or a load is the start.
		void Poll(const SendFn& a_send)
		{
			std::vector<Protocol::MapMarker> found;
			for (const auto ref : markers) {
				const auto flags = FlagsOf(ref);
				if (!flags) {
					continue;
				}
				auto&      last = known[ref];
				const auto gained = MapRules::Gained(last, *flags);
				last |= *flags;
				if (baselined && gained != 0) {
					const auto all = static_cast<std::uint8_t>(*flags & Protocol::kMarkerAllFlags);
					found.push_back({ ref, all });
					session[ref] |= all;  // ours too go on a save loaded later in the session
				}
			}
			if (!baselined) {
				baselined = true;
				logs::info("Map: {} of {} markers on our map at the start, {} discovered", std::ranges::count_if(known, [](const auto& a_e) { return a_e.second != 0; }),
					markers.size(), std::ranges::count_if(known, [](const auto& a_e) { return (a_e.second & Protocol::kMarkerTravel) != 0; }));
			}
			for (auto& batch : MapRules::Batches(found, Protocol::MAX_MARKERS)) {
				reported += batch.size();
				logs::info("Map: our player found {} marker(s), first {:08X}: the others are told", batch.size(), batch.front().ref);
				a_send(Protocol::Encode(Protocol::MapMarkers{ 0, std::move(batch) }), true);
			}
		}

		void Apply(Clock::time_point a_now)
		{
			std::size_t done = 0;
			while (!toApply.empty() && done < MapRules::APPLY_PER_TICK) {
				const auto ref = toApply.back();
				toApply.pop_back();
				const auto want = session[ref];
				const auto have = FlagsOf(ref);
				if (!have || !MapRules::Lacks(*have, want)) {
					continue;
				}
				++done;
				const bool travel = (want & Protocol::kMarkerTravel) != 0;
				if (!AddToMap(ref, travel)) {
					++failed;
					logs::warn("Map: marker {:08X} could not be put on our map", ref);
					continue;
				}
				++applied;
				known[ref] |= want;  // not ours to report
				const auto finder = std::exchange(finders[ref], 0);
				if (finder != 0 && travel && (*have & Protocol::kMarkerTravel) == 0 && feed.Take(a_now)) {
					const auto place = PlaceOf(ref);
					const auto who = NameOf(finder);
					if (!place.empty() && !who.empty()) {
						++notices;
						RE::SendHUDMessage::ShowHUDMessage(Text::Format("map.found", { { "player", who }, { "place", place } }).c_str());
					}
				}
			}
		}
	}

	void Started()
	{
		baselined = false;
		known.clear();
		nextPoll = {};
	}

	void Frame(const SendFn& a_send)
	{
		if (!Config::Get().shareMap || !LocalPlayer::InWorld()) {
			return;
		}
		if (!scanned) {
			Scan();
		}
		const auto now = Clock::now();
		Apply(now);  // first: what we put on is known before our own read
		if (now >= nextPoll) {
			nextPoll = now + MapRules::POLL_EVERY;
			Poll(a_send);
		}
	}

	void Heard(const Protocol::MapMarkers& a_msg)
	{
		if (!Config::Get().shareMap) {
			return;
		}
		for (const auto& marker : a_msg.markers) {
			auto& flags = session[marker.ref];
			if (!MapRules::Lacks(flags, marker.flags)) {
				continue;
			}
			flags |= marker.flags;
			finders[marker.ref] = a_msg.finder;
			toApply.push_back(marker.ref);
			++heardCount;
		}
	}

	void AfterLoad()
	{
		known.clear();
		baselined = false;
		toApply.clear();
		finders.clear();  // no notices again for what was found before the load
		for (const auto& [ref, flags] : session) {
			toApply.push_back(ref);
		}
		if (!session.empty()) {
			logs::info("Map: a save was loaded: the session's {} markers go on its map again", session.size());
		}
	}

	void Clear()
	{
		session.clear();
		finders.clear();
		toApply.clear();
		known.clear();
		baselined = false;
	}

	const std::vector<std::uint32_t>& Places()
	{
		if (!scanned) {
			Scan();
		}
		return markers;
	}

	std::string PlaceName(std::uint32_t a_ref)
	{
		return PlaceOf(a_ref);
	}

	std::string Command(std::string_view)
	{
		std::size_t visible = 0;
		std::size_t discovered = 0;
		for (const auto ref : markers) {
			const auto flags = FlagsOf(ref).value_or(0);
			visible += (flags & Protocol::kMarkerVisible) ? 1 : 0;
			discovered += (flags & Protocol::kMarkerTravel) ? 1 : 0;
		}
		return std::format("share={} markers={} visible={} discovered={} baselined={} reported={} heard={} session={} waiting={} applied={} failed={} notices={}",
			Config::Get().shareMap ? 1 : 0, markers.size(), visible, discovered, baselined ? 1 : 0, reported, heardCount, session.size(), toApply.size(), applied, failed,
			notices);
	}
}
