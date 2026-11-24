#include "game/Downed.h"

#include "Text.h"
#include "game/DownRules.h"
#include "game/LocalPlayer.h"
#include "game/RemotePlayers.h"

namespace Downed
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Friend
		{
			DownRules::Said         said = DownRules::Said::kUp;
			std::uint16_t           secondsLeft = 0;
			DownRules::Helper       helper;
			DownRules::StandInKneel kneel;
			bool                    toldDown = false;     // our player was told they are down
			bool                    toldHelping = false;  // our player was told they are helping them up
		};

		std::uint32_t                            ourId = 0;
		DownRules::Self                          self;
		bool                                     oursOn = false;  // our flag is on the player (we set it)
		std::unordered_map<std::uint32_t, Friend> friends;
		std::uint64_t                            downsHeard = 0;
		std::uint64_t                            helpsIn = 0;
		std::uint64_t                            helpsSent = 0;
		std::uint64_t                            kneels = 0;
		std::uint64_t                            kneelsTurnedAway = 0;
		DownRules::Said                          lastSaid = DownRules::Said::kUp;

		void Show(const std::string& a_text)
		{
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str());
		}

		std::string NameOf(std::uint32_t a_player)
		{
			auto name = RemotePlayers::NameOf(a_player);
			return name.empty() ? std::format("player {}", a_player) : name;
		}

		// Our flag: the player's base (what the engine reads when it works out an actor's essential flag again) and the
		// actor's own (what it reads when a blow would kill), both, or neither.
		void SetOurs(RE::PlayerCharacter* a_player, bool a_on)
		{
			using BaseFlag = RE::ACTOR_BASE_DATA::Flag;
			if (const auto base = a_player->GetActorBase()) {
				if (a_on) {
					base->actorData.actorBaseFlags.set(BaseFlag::kEssential);
				} else {
					base->actorData.actorBaseFlags.reset(BaseFlag::kEssential);
				}
			}
			auto& flags = a_player->GetActorRuntimeData().boolFlags;
			if (a_on) {
				flags.set(RE::Actor::BOOL_FLAGS::kEssential);
			} else {
				flags.reset(RE::Actor::BOOL_FLAGS::kEssential);
			}
			oursOn = a_on;
		}

		DownRules::Facts Read(RE::PlayerCharacter* a_player, std::size_t a_friends)
		{
			DownRules::Facts facts;
			facts.friends = a_friends > 0;
			const auto base = a_player->GetActorBase();
			const bool actorFlag = a_player->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential);
			facts.ours = oursOn && actorFlag;
			// Essential or protected by anything but our flag (a quest's alias, another mod, the console): that one's down,
			// never ours to begin or end. While ours is on, the essential flags are ours to read; protected is never ours.
			const bool protectedFlag = a_player->IsProtected() || (base && base->IsProtected());
			facts.elsewise = protectedFlag || (!oursOn && (actorFlag || (base && base->IsEssential())));
			const auto state = a_player->AsActorState();
			const auto life = state ? state->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
			facts.down = state && state->IsBleedingOut();
			facts.dead = life == RE::ACTOR_LIFE_STATE::kDying || life == RE::ACTOR_LIFE_STATE::kDead || (!facts.down && a_player->IsDead());
			const auto ui = RE::UI::GetSingleton();
			facts.paused = ui && ui->GameIsPaused();
			const auto camera = RE::PlayerCamera::GetSingleton();
			facts.bleedCamera = camera && camera->IsInBleedoutMode();
			return facts;
		}

		void GiveHealth(RE::PlayerCharacter* a_player)
		{
			const auto values = a_player->AsActorValueOwner();
			if (!values) {
				return;
			}
			const float most = a_player->GetActorValueMax(RE::ActorValue::kHealth);
			const float now = values->GetActorValue(RE::ActorValue::kHealth);
			if (const float give = most * DownRules::HEALTH_ON_UP - now; give > 0.0f) {
				values->RestoreActorValue(RE::ActorValue::kHealth, give);
			}
		}

		void Apply(RE::PlayerCharacter* a_player, const DownRules::Step& a_step, Clock::time_point a_now)
		{
			auto& flags = a_player->GetActorRuntimeData().boolFlags;
			if (a_step.essential) {
				SetOurs(a_player, *a_step.essential);
			}
			if (a_step.noRecovery) {
				flags.set(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
			}
			if (a_step.recovery) {
				flags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
			}
			if (a_step.health) {
				GiveHealth(a_player);
			}
			if (a_step.wentDown) {
				logs::info("Downed: our player is down; waiting {} s for a friend's help", DownRules::BLEED_OUT.count());
				Show(Text::Format("down.you", { { "n", static_cast<int>(DownRules::BLEED_OUT.count()) } }));
			}
			if (a_step.remind != 0) {
				Show(Text::Format("down.left", { { "n", a_step.remind } }));
			}
			if (a_step.standUp) {
				// The engine's own get-up did not come: its end of the bleedout by hand (the life state back to alive, the
				// graph's bleedout idle ended).
				logs::info("Downed: our player is helped but still down: stood up by hand ({} so far)", self.counts.standUps);
				a_player->SetLifeState(RE::ACTOR_LIFE_STATE::kAlive);
				a_player->NotifyAnimationGraph(RE::BSFixedString{ DownRules::KNEEL_STOP.data() });
			}
			if (a_step.kill) {
				// Our flag first (it is off by now), then the engine's kill: its death loads the save, as a death alone does.
				SetOurs(a_player, false);
				flags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
				logs::info("Downed: no help came: our player dies ({} kill(s) asked)", self.counts.kills);
				if (self.counts.kills % 2 == 1) {
					a_player->KillImpl(nullptr, 0.0f, true, false);
				} else {
					a_player->KillImmediate();
				}
			}
			if (a_step.gotUp) {
				logs::info("Downed: our player is up again (phase {})", DownRules::PhaseName(self.Now()));
			}
			if (a_step.fixCamera) {
				logs::warn("Downed: the camera stayed in its bleedout view with nobody down: put back in third person ({} so far)", self.counts.cameraFixes);
				if (const auto camera = RE::PlayerCamera::GetSingleton()) {
					camera->ForceThirdPerson();
				}
			}
			(void)a_now;
		}

		// A friend who waits: our player near them helps them up (DownRules::Helper); their stand-in kneels while they are on
		// the ground (DownRules::StandInKneel).
		void Friends(RE::PlayerCharacter* a_player, const DownRules::Facts& a_ours, const SendFn& a_send, Clock::time_point a_now)
		{
			const bool able = !a_ours.down && !a_ours.dead && !a_ours.paused && LocalPlayer::InWorld();
			for (auto& [id, f] : friends) {
				const auto standIn = RemotePlayers::StandInOf(id);
				const bool onGround = f.said != DownRules::Said::kUp;
				switch (f.kneel.Frame(onGround, standIn ? standIn->GetFormID() : 0, a_now)) {
				case DownRules::Kneel::kStart:
				case DownRules::Kneel::kStop:
					{
						const bool start = f.kneel.Kneeling();
						const bool taken = standIn->NotifyAnimationGraph(RE::BSFixedString{ (start ? DownRules::KNEEL_START : DownRules::KNEEL_STOP).data() });
						f.kneel.Took(taken);
						++kneels;
						kneelsTurnedAway += taken ? 0 : 1;
						if (!taken) {
							logs::info("Downed: player {}'s stand-in {:08X} turned {} away", id, standIn->GetFormID(), start ? DownRules::KNEEL_START : DownRules::KNEEL_STOP);
						}
					}
					break;
				case DownRules::Kneel::kNothing:
					break;
				}
				std::optional<float> distance;
				if (standIn) {
					distance = a_player->GetPosition().GetDistance(standIn->GetPosition());
				}
				const bool waits = f.said == DownRules::Said::kWaiting;
				if (f.helper.Frame(waits, able, distance, a_now) && a_send) {
					a_send(Protocol::Encode(Protocol::HelpUp{ id }), true);
					++helpsSent;
					logs::info("Downed: our player helps player {} up ({:.0f} u from them)", id, distance.value_or(-1.0f));
					Show(Text::Format("down.helped_them", { { "player", NameOf(id) } }));
				}
				if (waits && f.helper.Helping() && !f.toldHelping) {
					f.toldHelping = true;
					Show(Text::Format("down.helping", { { "player", NameOf(id) } }));
				} else if (!f.helper.Helping()) {
					f.toldHelping = false;
				}
			}
		}
	}

	void Started(std::uint32_t a_ourId)
	{
		ourId = a_ourId;
		friends.clear();
		lastSaid = DownRules::Said::kUp;
	}

	void Frame(std::size_t a_friends, const SendFn& a_send)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !LocalPlayer::InWorld()) {
			return;  // a load or the main menu: nothing of the down changes meanwhile
		}
		const auto now = Clock::now();
		const auto facts = Read(player, a_friends);
		if (self.TakeHelpPending()) {
			// Helped: the health to get up with, and the engine let stand our player up on its own (its get-up).
			GiveHealth(player);
			player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
		}
		const auto step = self.Frame(facts, now);
		Apply(player, step, now);
		if (a_send && (step.say || self.Says() != lastSaid)) {
			lastSaid = self.Says();
			a_send(Protocol::Encode(Protocol::Downed{ .state = static_cast<std::uint8_t>(lastSaid), .secondsLeft = static_cast<std::uint16_t>(self.SecondsLeft(now)) }), true);
		}
		Friends(player, facts, a_send, now);
	}

	void Heard(const Protocol::Downed& a_msg)
	{
		if (a_msg.player == 0 || a_msg.player == ourId || a_msg.state >= DownRules::SAID_COUNT) {
			return;
		}
		auto&      f = friends[a_msg.player];
		const auto said = static_cast<DownRules::Said>(a_msg.state);
		if (said != f.said) {
			++downsHeard;
			logs::info("Downed: player {} is {}", a_msg.player, DownRules::SaidName(said));
			if (said == DownRules::Said::kWaiting && !f.toldDown) {
				Show(Text::Format("down.friend", { { "player", NameOf(a_msg.player) } }));
			}
			f.toldDown = said != DownRules::Said::kUp;
		}
		f.said = said;
		f.secondsLeft = a_msg.secondsLeft;
	}

	void HelpedBy(std::uint32_t a_helper)
	{
		++helpsIn;
		if (self.Help(Clock::now())) {
			logs::info("Downed: player {} helped our player up", a_helper);
			Show(Text::Format("down.helped_you", { { "player", NameOf(a_helper) } }));
		} else {
			logs::info("Downed: player {}'s help came while our player is {}: nothing to do", a_helper, DownRules::PhaseName(self.Now()));
		}
	}

	void Left(std::uint32_t a_playerId)
	{
		friends.erase(a_playerId);
	}

	void Clear()
	{
		friends.clear();
		ourId = 0;
		lastSaid = DownRules::Said::kUp;
	}

	void BeforeSave()
	{
		if (oursOn) {
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				SetOurs(player, false);  // the next frame puts it back
				logs::info("Downed: our essential flag off for the save (back next frame)");
			}
		}
	}

	void ForgetWorld()
	{
		self.Forget();
		for (auto& [id, f] : friends) {
			f.kneel = {};
			f.helper = {};
		}
	}

	std::string Describe()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto state = player ? player->AsActorState() : nullptr;
		const auto now = Clock::now();
		const auto& c = self.counts;
		std::string out = std::format("phase={} said={} secondsLeft={} oursOn={} essential={} lifeState={} downs={} helped={} helpsRefused={} gotUp={} upElsewise={} standUps={} bledOut={}"
									  " neverUp={} kills={} cameraFixes={} helpsIn={} helpsSent={} downsHeard={} kneels={} kneelsTurnedAway={}",
			DownRules::PhaseName(self.Now()), DownRules::SaidName(self.Says()), self.SecondsLeft(now), oursOn ? 1 : 0,
			player && player->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential) ? 1 : 0, state ? std::to_underlying(state->GetLifeState()) : 0u,
			c.downs, c.helped, c.helpsRefused, c.gotUp, c.upElsewise, c.standUps, c.bledOut, c.neverUp, c.kills, c.cameraFixes, helpsIn, helpsSent, downsHeard, kneels,
			kneelsTurnedAway);
		for (const auto& [id, f] : friends) {
			out += std::format(" | friend player={} said={} secondsLeft={} kneeling={} approaches={} helpsSent={} kneelTurnedAway={}", id, DownRules::SaidName(f.said),
				f.secondsLeft, f.kneel.Kneeling() ? 1 : 0, f.helper.approaches, f.helper.sent, f.kneel.turnedAway);
		}
		return out;
	}
}
