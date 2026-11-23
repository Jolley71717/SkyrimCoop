#include "game/Riding.h"

#include "DevLine.h"
#include "game/Console.h"
#include "game/NpcSync.h"
#include "game/RemotePlayers.h"
#include "game/RideRules.h"
#include "net/FormsCommand.h"

#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace Riding
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Rider = RideRules::Said::Rider;
		using Key = Protocol::NpcKey;

		bool          active = false;
		std::uint32_t ourId = 0;

		// A rider another game says rides (a friend, an NPC that game runs), and how this game seats it.
		struct Wanted
		{
			Key                              mount;  // id 0: a mount this game lacks (shown on foot)
			bool                             on = false;
			std::uint32_t                    sender = 0;  // the game that said it (0: the server's list for a joiner)
			RideRules::Seat                  seat;
			RideRules::SaddleWatch           saddle;
			std::optional<Clock::time_point> readySince;  // rider and mount both here with their 3D
			RE::FormID                       riderActor = 0;  // the actors that show them here, last looked (for `ride`)
			RE::FormID                       mountActor = 0;
			bool                             engineOn = false;
			bool                             onOther = false;
			bool                             busy = false;
			float                            gap = -1.0f;  // the rider's distance from its mount while seated
			float                            mountX = 0.0f;  // where the mount is here, last looked (the rig sees it go)
			float                            mountY = 0.0f;
			RideRules::Handover              handover;  // an NPC's ride while its runner changes
			bool                             held = false;  // kept as it was for its new runner to say (`ride`: handover=1)
			std::uint64_t                    heard = 0;
		};
		std::map<Rider, Wanted> wanted;
		RideRules::Said         said;

		// The riders this game has seated: the update hooks leave their place to the engine (any thread).
		std::mutex                     seatLock;
		std::unordered_set<RE::FormID> seated;

		struct Counts
		{
			std::uint64_t sent = 0, heard = 0, notBelieved = 0, lacked = 0;
			std::uint64_t activates = 0, consoles = 0, quicks = 0, dismounts = 0, holds = 0, releases = 0, handedToUs = 0;
		};
		Counts counts;

		std::string KeyText(const Key& a_key)
		{
			if (a_key.id == 0) {
				return "none";
			}
			return a_key.IsSpawn() ? std::format("{:08X}@{}", a_key.id, a_key.spawner) : std::format("{:08X}", a_key.id);
		}

		std::string RiderText(const Rider& a_rider)
		{
			return a_rider.player != 0 ? std::format("player={}", a_rider.player) : std::format("npc={}", KeyText(a_rider.npc));
		}

		void SetSeated(RE::FormID a_id, bool a_seated)
		{
			std::scoped_lock l{ seatLock };
			if (a_seated) {
				seated.insert(a_id);
			} else {
				seated.erase(a_id);
			}
		}

		RE::NiPointer<RE::Actor> MountOf(RE::Actor* a_actor)
		{
			RE::NiPointer<RE::Actor> mount;
			if (!a_actor || !a_actor->GetMount(mount)) {
				return {};
			}
			return mount;
		}

		bool GettingOnOrOff(RE::Actor* a_actor)
		{
			return a_actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kGettingOnOffMount);
		}

		float Distance(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b)
		{
			const auto d = a_a - a_b;
			return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		}

		// ---- ours ----

		// What our player and each NPC we run rides now: a mount the session knows (NpcSync::KeyOf), or on foot (id 0: on
		// none, or on one not shared yet: said once it is).
		std::map<Rider, Key> OursNow()
		{
			std::map<Rider, Key> out;
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				const auto mount = MountOf(player);
				const auto key = mount ? NpcSync::KeyOf(mount.get()) : std::nullopt;
				out[Rider{ ourId, {} }] = key.value_or(Key{});
			}
			for (const auto& [npc, actor] : NpcSync::RunActors()) {
				const auto mount = MountOf(actor.get());
				if (!mount) {
					continue;  // on foot: an end is said for one said on before (RideRules::Said)
				}
				out[Rider{ 0, npc }] = NpcSync::KeyOf(mount.get()).value_or(Key{});
			}
			return out;
		}

		void SendOurs(const SendFn& a_send)
		{
			auto changes = said.Changes(OursNow());
			for (std::size_t first = 0; first < changes.size(); first += Protocol::MAX_RIDES) {
				Protocol::Rides msg;
				const auto      end = (std::min)(changes.size(), first + Protocol::MAX_RIDES);
				msg.rides.assign(changes.begin() + static_cast<std::ptrdiff_t>(first), changes.begin() + static_cast<std::ptrdiff_t>(end));
				counts.sent += msg.rides.size();
				for (const auto& ride : msg.rides) {
					if (ride.player != 0) {
						logs::info("Riding: our player {} {}", ride.on ? "rides" : "got off", ride.on ? KeyText(ride.mount) : std::string{});
					}
				}
				a_send(Protocol::Encode(msg), true);
			}
		}

		// ---- theirs ----

		// The actor that shows a rider here: a friend's stand-in, or the copy of an NPC another game runs (none for one
		// we run: our own AI rides it).
		RE::NiPointer<RE::Actor> RiderActor(const Rider& a_rider)
		{
			if (a_rider.player != 0) {
				return RemotePlayers::StandInOf(a_rider.player);
			}
			const auto runner = NpcSync::RunnerOf(a_rider.npc);
			if (runner == 0 || runner == ourId) {
				return {};
			}
			return NpcSync::ActorOf(a_rider.npc);
		}

		// Whether this game still takes what a rider's ride says: an NPC's from the game that runs it now.
		bool StillBelieved(const Rider& a_rider, const Wanted& a_wanted)
		{
			if (a_rider.player != 0) {
				return true;
			}
			const auto runner = NpcSync::RunnerOf(a_rider.npc);
			return runner != 0 && runner != ourId && (a_wanted.sender == 0 || a_wanted.sender == runner);
		}

		// a_activate with the mount's activation open: a copy this game placed of another game's spawn is blocked from
		// activation (NpcSync's PlaceActor), and both ways onto it (the rider's activate, the console's, which goes through
		// the same activation) would be refused. Opened for this one, and blocked again.
		template <class Fn>
		void Unblocked(RE::Actor* a_mount, const Key& a_mountKey, Fn&& a_activate)
		{
			const bool placed = a_mountKey.IsSpawn() && a_mountKey.spawner != ourId;
			if (placed) {
				a_mount->extraList.SetExtraFlags(RE::ExtraFlags::Flag::kBlockActivate, false);
			}
			a_activate();
			if (placed) {
				a_mount->extraList.SetExtraFlags(RE::ExtraFlags::Flag::kBlockActivate, true);
			}
		}

		void Tend(const Rider& a_rider, Wanted& a_wanted, Clock::time_point a_now)
		{
			const auto rider = RiderActor(a_rider);
			const auto believed = StillBelieved(a_rider, a_wanted);
			// An NPC's ride said by a game that runs it no more: kept as it is a moment, for its new runner to say it (the
			// same ride, then it stays seated; its end, then it gets off), not pulled off its horse at every handover.
			a_wanted.held = a_rider.player == 0 && a_wanted.on && a_wanted.handover.Holds(!believed, a_now);
			if (a_wanted.held) {
				a_wanted.riderActor = rider ? rider->GetFormID() : 0;
				return;
			}
			auto       mount = a_wanted.on && believed && a_wanted.mount.id != 0 ? NpcSync::ActorOf(a_wanted.mount) : RE::NiPointer<RE::Actor>{};
			// A mount our player sits on is ours to ride: no friend is seated on it here.
			if (mount) {
				if (const auto player = RE::PlayerCharacter::GetSingleton(); player && MountOf(player).get() == mount.get()) {
					mount.reset();
				}
			}
			a_wanted.riderActor = rider ? rider->GetFormID() : 0;
			a_wanted.mountActor = mount ? mount->GetFormID() : 0;
			if (!rider) {
				a_wanted.readySince.reset();
				a_wanted.engineOn = false;
				a_wanted.gap = -1.0f;
				return;
			}
			const auto on = MountOf(rider.get());
			const bool ready = mount && rider->Is3DLoaded() && mount->Is3DLoaded() && !rider->IsDead() && !mount->IsDead();
			if (ready) {
				a_wanted.readySince = a_wanted.readySince.value_or(a_now);
			} else {
				a_wanted.readySince.reset();
			}
			RideRules::SeatFacts facts;
			facts.wanted = mount != nullptr;
			facts.ready = ready && a_now - *a_wanted.readySince >= RideRules::READY_AFTER;
			facts.onWanted = on && mount && on.get() == mount.get();
			facts.onOther = on && !facts.onWanted && facts.wanted;
			if (on && !facts.wanted) {
				facts.onWanted = true;  // on a mount it should be off: Seat gets it off
				facts.onOther = false;
			}
			facts.busy = GettingOnOrOff(rider.get());
			a_wanted.engineOn = on != nullptr;
			a_wanted.onOther = facts.onOther;
			a_wanted.busy = facts.busy;
			const auto act = a_wanted.seat.Step(facts, a_now);
			switch (act) {
			case RideRules::Act::kActivate:
				++counts.activates;
				logs::info("Riding: {} is seated on {} ({:08X}): the mount activated by its rider", RiderText(a_rider), KeyText(a_wanted.mount), mount->GetFormID());
				Unblocked(mount.get(), a_wanted.mount, [&] { mount->ActivateRef(rider.get(), 0, nullptr, 1, false); });
				break;
			case RideRules::Act::kConsole:
				++counts.consoles;
				logs::info("Riding: {} not on {} yet: the console's activate", RiderText(a_rider), KeyText(a_wanted.mount));
				Unblocked(mount.get(), a_wanted.mount, [&] { Console::Run(std::format("{:08X}.activate {:08X}", mount->GetFormID(), rider->GetFormID())); });
				break;
			case RideRules::Act::kDismount:
				++counts.dismounts;
				logs::info("Riding: {} gets off its mount", RiderText(a_rider));
				rider->StopInteractingQuick(true);
				break;
			default:
				break;
			}
			const bool nowSeated = a_wanted.seat.Seated();
			SetSeated(rider->GetFormID(), nowSeated);
			a_wanted.gap = -1.0f;
			if (mount) {
				const auto mountAt = mount->GetPosition();
				a_wanted.mountX = mountAt.x;
				a_wanted.mountY = mountAt.y;
			}
			if (nowSeated && mount) {
				const auto mountAt = mount->GetPosition();
				a_wanted.gap = Distance(rider->GetPosition(), mountAt);
				switch (a_wanted.saddle.Step(a_wanted.gap, a_now)) {
				case RideRules::Act::kQuick:
					// The engine has it on that mount (its interaction names it) but not in the saddle: finished onto it.
					++counts.quicks;
					logs::info("Riding: {} is {:.0f} u from its mount: finished onto its saddle (quick put-on)", RiderText(a_rider), a_wanted.gap);
					rider->PutActorOnMountQuick();
					break;
				case RideRules::Act::kHold:
					++counts.holds;
					rider->SetPosition(mountAt, true);
					break;
				default:
					break;
				}
			}
		}

		// ---- `ride find` and `ride base` ----

		bool IsHorseRace(const RE::TESRace* a_race)
		{
			const auto editorId = a_race && a_race->GetFormEditorID() ? std::string_view{ a_race->GetFormEditorID() } : std::string_view{};
			return editorId.find("Horse") != std::string_view::npos;
		}

		std::string Find(float a_radius)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!player || !lists) {
				return "mount none";
			}
			const auto               at = player->GetPosition();
			RE::NiPointer<RE::Actor> best;
			float                    bestDistance = a_radius;
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (const auto& handle : *handles) {
					const auto actor = handle.get();
					if (!actor || actor.get() == player || actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded() || RemotePlayers::IsOurs(actor.get())) {
						continue;
					}
					if (!actor->IsAMount() && !IsHorseRace(actor->GetRace())) {
						continue;
					}
					if (RE::NiPointer<RE::Actor> rider; actor->GetMountedBy(rider) && rider) {
						continue;
					}
					const float d = Distance(actor->GetPosition(), at);
					if (d <= bestDistance) {
						bestDistance = d;
						best = actor;
					}
				}
			}
			if (!best) {
				return "mount none";
			}
			const auto pick = RE::CrosshairPickData::GetSingleton();
			const auto target = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
			const auto base = best->GetActorBase();
			const auto key = NpcSync::KeyOf(best.get());
			const auto mountAt = best->GetPosition();
			return std::format("mount id={:08X} base={:08X} key={} dist={:.0f} x={:.0f} y={:.0f} z={:.0f} aimed={}", best->GetFormID(), base ? base->GetFormID() : 0,
				key ? KeyText(*key) : std::string{ "none" }, bestDistance, mountAt.x, mountAt.y, mountAt.z, target && target.get() == best.get() ? 1 : 0);
		}

		std::string Base()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			if (!data) {
				return "base none";
			}
			RE::TESNPC* best = nullptr;
			for (const auto npc : data->GetFormArray<RE::TESNPC>()) {
				if (!npc || !IsHorseRace(npc->race) || npc->IsUnique() || (npc->GetFormID() >> 24) != 0) {
					continue;  // Skyrim.esm's own plain horses: every game has them
				}
				if (!best || npc->GetFormID() < best->GetFormID()) {
					best = npc;
				}
			}
			return best ? std::format("base={:08X}", best->GetFormID()) : "base none";
		}

		// `ride order`: a horse of the load order (a reference of Skyrim.esm's, never one made as the game ran), alive,
		// enabled and ridden by no one, in our player's worldspace: the nearest of those with their 3D, else the nearest
		// the game holds. For the rig to bring over with the console's moveto (set-up): its NPC rider leg needs a mount
		// the session keys by its own reference.
		std::string Order()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto [map, mapLock] = RE::TESForm::GetAllForms();
			if (!player || !map) {
				return "horse none";
			}
			const auto   at = player->GetPosition();
			const auto   world = player->GetWorldspace();
			RE::Actor*   best = nullptr;
			bool         bestLoaded = false;
			float        bestDistance = 0.0f;
			std::size_t  seen = 0;
			RE::BSReadLockGuard guard{ mapLock };
			for (const auto& [id, form] : *map) {
				if (!form || (id >> 24) != 0 || form->GetFormType() != RE::FormType::ActorCharacter) {
					continue;
				}
				const auto actor = form->As<RE::Actor>();
				if (!actor || actor->IsDead() || actor->IsDisabled() || actor->IsDeleted() || !IsHorseRace(actor->GetRace()) || RemotePlayers::IsOurs(actor)) {
					continue;
				}
				if (!world || actor->GetWorldspace() != world) {
					continue;
				}
				if (RE::NiPointer<RE::Actor> rider; actor->GetMountedBy(rider) && rider) {
					continue;
				}
				++seen;
				const bool  loaded = actor->Is3DLoaded();
				const float d = Distance(actor->GetPosition(), at);
				if (!best || (loaded && !bestLoaded) || (loaded == bestLoaded && d < bestDistance)) {
					best = actor;
					bestLoaded = loaded;
					bestDistance = d;
				}
			}
			if (!best) {
				return "horse none";
			}
			const auto base = best->GetActorBase();
			return std::format("horse id={:08X} base={:08X} loaded={} dist={:.0f} seen={}", best->GetFormID(), base ? base->GetFormID() : 0, bestLoaded ? 1 : 0, bestDistance, seen);
		}

		// `ride off <id>`: an NPC this game runs gets off its mount (set-up for the rig's NPC rider leg: the console has no
		// way to have an NPC get off). Refused for any other actor: a stand-in or a copy follows its own game's word.
		std::string Off(std::string_view a_id)
		{
			const auto id = FormsCommand::ParseHex(a_id);
			if (!id) {
				return "usage: ride off <form id>";
			}
			for (const auto& [npc, actor] : NpcSync::RunActors()) {
				if (actor->GetFormID() != *id) {
					continue;
				}
				if (!MountOf(actor.get())) {
					return std::format("off: {:08X} rides nothing", *id);
				}
				actor->StopInteractingQuick(true);
				logs::info("Riding: the developer channel gets {} ({:08X}) off its mount", KeyText(npc), *id);
				return std::format("off: {:08X}", *id);
			}
			return std::format("off: {:08X} is no NPC this game runs", *id);
		}

		std::string Ours()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return "ours none";
			}
			const auto mount = MountOf(player);
			const auto key = mount ? NpcSync::KeyOf(mount.get()) : std::nullopt;
			return std::format("ours on={} mount={} mountActor={:08X} getting={} said={} sent={}", player->IsOnMount() ? 1 : 0, key ? KeyText(*key) : std::string{ "none" },
				mount ? mount->GetFormID() : 0, GettingOnOrOff(player) ? 1 : 0, said.Count(), counts.sent);
		}
	}

	void Started(std::uint32_t a_ourId)
	{
		ourId = a_ourId;
		active = true;
		said.Clear();
		wanted.clear();
	}

	void Frame(const SendFn& a_send)
	{
		if (!active) {
			return;
		}
		// An NPC handed to this game that another game said rides is ours to say now: what it rides here goes out, even
		// on foot (its end), and its copy's seat is let go of (our own AI rides it now).
		for (auto it = wanted.begin(); it != wanted.end();) {
			if (it->first.player == 0 && NpcSync::RunnerOf(it->first.npc) == ourId) {
				if (it->second.on) {
					said.HandedOver(it->first);
				}
				if (it->second.riderActor != 0) {
					SetSeated(it->second.riderActor, false);
				}
				++counts.handedToUs;
				logs::info("Riding: {} was handed to this game {}: said from here now", RiderText(it->first), it->second.on ? "riding" : "on foot");
				it = wanted.erase(it);
			} else {
				++it;
			}
		}
		SendOurs(a_send);
		const auto now = Clock::now();
		for (auto it = wanted.begin(); it != wanted.end();) {
			Tend(it->first, it->second, now);
			// An end of a ride, once its rider is off (or not here), is forgotten.
			if (!it->second.on && !it->second.engineOn) {
				if (it->second.riderActor != 0) {
					SetSeated(it->second.riderActor, false);
				}
				it = wanted.erase(it);
			} else {
				++it;
			}
		}
	}

	void Rides(const Protocol::Rides& a_msg)
	{
		if (!active) {
			return;
		}
		if (a_msg.full) {
			for (auto& [rider, want] : wanted) {
				want.on = false;
			}
		}
		for (const auto& ride : a_msg.rides) {
			++counts.heard;
			if (ride.player == ourId) {
				continue;
			}
			const auto rider = ride.player != 0 ? Rider{ ride.player, {} } : Rider{ 0, ride.rider };
			const auto runner = ride.player != 0 ? 0 : NpcSync::RunnerOf(ride.rider);
			if (ride.player == 0 && runner == ourId) {
				continue;  // ours to say
			}
			if (!RideRules::Believed(ride, a_msg.sender, runner)) {
				++counts.notBelieved;
				continue;
			}
			auto& want = wanted[rider];
			if (want.mount != ride.mount) {
				want.seat = {};
				want.saddle = {};
			}
			want.mount = ride.mount;
			want.on = ride.on;
			want.sender = a_msg.sender;
			++want.heard;
			if (ride.on && ride.mount.id == 0) {
				++counts.lacked;
				logs::info("Riding: {} rides a mount this game lacks: shown on foot", RiderText(rider));
			}
		}
	}

	void Left(std::uint32_t a_playerId)
	{
		for (auto& [rider, want] : wanted) {
			if (rider.player == a_playerId || want.sender == a_playerId) {
				want.on = false;
			}
		}
	}

	void Clear()
	{
		for (const auto& [rider, want] : wanted) {
			if (const auto actor = RiderActor(rider); actor && want.seat.Seated()) {
				actor->StopInteractingQuick(true);
			}
		}
		wanted.clear();
		said.Clear();
		{
			std::scoped_lock l{ seatLock };
			seated.clear();
		}
		active = false;
		ourId = 0;
	}

	void ForgetWorld()
	{
		for (auto& [rider, want] : wanted) {
			want.seat = {};
			want.saddle = {};
			want.readySince.reset();
		}
		std::scoped_lock l{ seatLock };
		seated.clear();
	}

	void Release(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		bool released = false;
		if (MountOf(a_actor)) {
			a_actor->StopInteractingQuick(true);
			released = true;
		}
		if (RE::NiPointer<RE::Actor> rider; a_actor->GetMountedBy(rider) && rider) {
			rider->StopInteractingQuick(true);
			SetSeated(rider->GetFormID(), false);
			released = true;
		}
		SetSeated(a_actor->GetFormID(), false);
		counts.releases += released ? 1 : 0;
	}

	bool Seated(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::scoped_lock l{ seatLock };
		return !seated.empty() && seated.contains(a_actor->GetFormID());
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "find") {
			float radius = 3000.0f;
			if (words.size() > 1) {
				if (const auto number = DevLine::Number(words[1])) {
					radius = static_cast<float>(*number);
				}
			}
			return Find(radius);
		}
		if (!words.empty() && words[0] == "base") {
			return Base();
		}
		if (!words.empty() && words[0] == "order") {
			return Order();
		}
		if (!words.empty() && words[0] == "off") {
			return words.size() == 2 ? Off(words[1]) : "usage: ride off <form id>";
		}
		std::string out = Ours();
		// The rides of the NPCs we run, as said.
		for (const auto& [rider, mount] : said.Mounts()) {
			if (rider.player == 0) {
				out += std::format(" | said npc={} mount={}", KeyText(rider.npc), KeyText(mount));
			}
		}
		for (const auto& [rider, want] : wanted) {
			out += std::format(" | rider {} on={} mount={} mountHere={:08X} riderHere={:08X} engineOn={} onOther={} busy={} seated={} rounds={} tried={} seatedBy={} givenUp={} gap={:.0f} quicks={} holds={} dismounts={} sender={} handover={} heard={} mx={:.0f} my={:.0f}",
				RiderText(rider), want.on ? 1 : 0, KeyText(want.mount), want.mountActor, want.riderActor, want.engineOn ? 1 : 0, want.onOther ? 1 : 0, want.busy ? 1 : 0,
				want.seat.Seated() ? 1 : 0, want.seat.Rounds(), RideRules::ActName(want.seat.LastTried()), RideRules::ActName(want.seat.SeatedBy()), want.seat.GivenUp() ? 1 : 0,
				want.gap, want.saddle.Quicks(), want.saddle.Holds(), want.seat.Dismounts(), want.sender, want.held ? 1 : 0, want.heard, want.mountX, want.mountY);
		}
		out += std::format(" | rides sent={} heard={} notBelieved={} lacked={} activates={} consoles={} quicks={} dismounts={} holds={} releases={} handedToUs={}", counts.sent, counts.heard,
			counts.notBelieved, counts.lacked, counts.activates, counts.consoles, counts.quicks, counts.dismounts, counts.holds, counts.releases, counts.handedToUs);
		return out;
	}
}
