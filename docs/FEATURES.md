# Features

SkyrimCoop is for playing Skyrim Special Edition through with a few friends: one world, the host's, and everyone's own character in it. The host invites friends from the Steam overlay and they join. There is no server to rent or run, no port to open and no address to pass around. It is meant to play with the mods you already use, whatever order you load them in, and with friends whose mods are not quite the same as yours.

This page is the feature plan. For each feature it says what players get, how it is meant to be built, what the Fallout 4 sister mod ([F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer)) already does, and in which release it is meant to arrive. It is a draft for the maintainer to review: nothing here is a promise or a date. [PLAN.md](PLAN.md) has the phases and what each must prove; [DESIGN.md](DESIGN.md) has the decisions made so far.

**Where it is today.** Nothing is playable yet. In a game, the plugin loads, two games find each other through a Steam lobby and pass messages over Steam's relay, and on the test rig a friend's stand-in has had its legs fed the friend's speed and direction (read from the logs, not yet watched on screen). Everything else on this page is planned, not built.

## How to read this page

- Each feature has a short paragraph for players, then a line marked *How* for developers.
- "The Fallout 4 mod" means F4Multiplayer 0.21. Its [list of what is shared](https://github.com/Jolley71717/F4Multiplayer/blob/main/docs/SYNCED.md) and its design document are where most of the answers below come from. Its ideas and its code carry over (same maintainer, same license); code from other Skyrim multiplayer mods does not ([PRIOR-ART.md](PRIOR-ART.md)).
- Engine names in the *How* lines (classes, events, graph variables) are starting points to check on an installed game, not facts relied on yet.
- The chart near the end lists every feature once, with its target release and difficulty. The open questions after it are the choices the maintainer is asked to make.

## What it is meant to do differently

Other Skyrim co-op mods exist and some are well loved. Players' most common complaints about them are the ones this project was started for:

- **Joining is a Steam invite.** No server program, no port forwarding, no private network to set up.
- **Your mods are fine.** Friends may have different mods, or the same ones in another order. What only one of you has is left out for the other, and the game says which mods differ. Nobody has to play unmodded.
- **Quests do not need a "leader".** Whoever does a quest's step moves the quest on for everyone. Nobody has to stand back while one player talks to every quest giver.
- **Each release is played for hours first.** Two scripted games play every release candidate for a long session before it ships ([RELEASING.md](RELEASING.md)).

## 1. Players: movement and animation

The friend you see is a stand-in: a character in your game that looks like your friend and does what your friend does, which the game's own AI never moves. It is drawn about a tenth of a second behind your friend so its movement stays smooth.

*How, for all of this section:* each game sends its player's state 20 times a second (place, heading, the animation graph's Speed and Direction, a few flags) and its looks when they change. The stand-in is placed from those states inside its own actor update, and its animation graph is fed two ways: its variables (Speed and Direction today, through the engine's own channels for them, `GaitDrive`), and its events. For everything past the legs, the plan is to record the animation events the local player's own graph sends (attacks, draws, casts, shouts) and replay a known list of them on the stand-in's graph, so each new action does not need a case written by hand. An event the viewer's graph does not have is dropped.

**Walk, run and sprint.** Friends walk, jog and sprint with their legs moving at the right pace and in the right direction, strafing and walking backwards included.

*How:* built in a first form: states at 20 Hz, `StateBuffer` 120 ms behind, Speed and Direction through the hooked graph channels, the move and sprint events from `Gait`. Seen in the rig's logs only.

**Sneak.** A sneaking friend crouches and moves in the sneak walk. Whether enemies notice them is decided in the game that runs those enemies, using the friend's own sneak skill.

*How:* the sneak flag is sent and turned into the graph's sneak events (written). Detection needs the stand-in to count as sneaking for the game's detection, and the friend's sneak skill and perks to be sent with their looks.

**Jump, fall and land.** Friends jump, fall and land with the game's own animations; a long fall shows the hard landing. Fall damage is the falling player's own.

*How:* the in-air flag and the jump and land events are written; the height comes from the state. Telling a jump from a fall off a ledge needs one more flag.

**Swim.** Friends swim on the surface and under water, and climb out onto the shore.

*How:* the swim flag is sent; the swim start and stop events and keeping the stand-in at its friend's depth (rather than floating up) are to do.

**Turning, looking and aiming.** Friends face where they face, and their head and upper body follow where they look and aim, so you can tell what a friend is looking at or aiming a bow at.

*How:* the heading is sent; to add the camera's pitch and a look point to the state, drive the graph's aim pitch, and give the stand-in a head-tracking target.

**Horses.** A friend mounts their horse, rides, gallops and gets off, and you see both of them. Their horse is theirs: run by their game and shown in yours.

*How:* the mounted flag is sent. The horse is an NPC run by its rider's game (see ownership, below); the friend's stand-in is put on your game's copy of that horse with the engine's own mount call. Fighting from horseback comes later.

**Sitting and furniture.** Friends sit on chairs and benches, lie in beds, lean on rails, and work at forges, grindstones, alchemy and enchanting tables, cooking pots, wood-chopping blocks and ore veins, with the furniture's own animations.

*How:* send which furniture reference the player is in and when they get up; the stand-in is told to use the same reference in the viewer's game (named by plugin and id, see Mods). If the viewer's game lacks it, the stand-in stands beside it.

**Drawing and sheathing.** Friends draw and put away weapons, shields, spells and torches with the game's animations, and you see what is in each hand.

*How:* the weapon flag is sent; to add what each hand holds (weapon, spell, shield, torch, nothing) and the draw and sheathe events.

**Gear and looks.** Friends look like their characters: race, sex, body weight, the armor and clothes they wear, then their face, hair and its colour, skin and warpaint, and the weapons on their back and hip, their quiver, shield and lit torch. Enchanted weapons glow.

*How:* race, sex, weight and worn armor are sent (protocol 2) and the stand-in is a character-creation preset of that race and sex, dressed. Faces (head parts, face shape, tint layers, hair colour) need a way to put them on a stand-in without a base made at run time ending up in a save, which crashed the sister mod; that is the engine question of this feature.

**Name and health.** A friend's name shows when you look at them, and their health when they are hurt.

*How:* the stand-in carries the friend's name; health is to be added to the state as a fraction and shown on the stand-in's own health bar.

**Werewolf and vampire lord.** A friend who turns into a werewolf or a vampire lord turns into one in your game too, and turns back.

*How:* the stand-in's race and graph change with its friend's; the beast forms' own attacks and howls are replayed events like any others.

**Menus.** A friend in their inventory, map, crafting, lockpicking or a book stands still with "[in a menu]" by their name. A menu pauses only your own game: friends play on.

*How:* menu open and close events become a state flag. NPCs that a paused game runs pass to a nearby friend's game after a few seconds, as in the Fallout 4 mod.

## 2. Combat

The rule from the Fallout 4 mod carries: every NPC is run by exactly one game, everyone else sees a copy of it, and a hit counts once wherever it was struck.

**Who runs each NPC (ownership).** Each NPC near any player is run by one player's game, its AI included; the others show a copy that moves and fights as the real one does. An NPC you fight or talk to comes to your game, so your swings land on the real one without waiting for the network, and an NPC near you passes to your game when the player whose game ran it walks far away. The host's game breaks ties and runs anything no player claims.

*How:* the Fallout 4 mod's design: the server records who runs each NPC (first to claim it, a hit or talk takes it over with a short cool-down, a companion goes to its owner); the runner sends each NPC's place, heading, gait, weapon state and health; copies are moved like stand-ins. DESIGN.md's combat line says "the host runs every NPC": the open questions ask which of the two to start with.

**Melee swings and power attacks.** Friends' sword, axe, mace, dagger and fist attacks, power attacks included, play in your game, and the hits they land count for everyone.

*How:* the attack events of the friend's graph are replayed on the stand-in, which only shows them. The attacker's game judges the hit (it saw the blade connect); a hit on an NPC another game runs goes to that game as damage, as in the Fallout 4 mod's damage reports with their answers.

**Blocking and bashing.** Friends raise their shield or weapon to block and bash with it; a blocked blow does what blocking does, and a bash staggers.

*How:* block start and stop and the bash events are replayed. A blow on a blocking player is judged in that player's own game, which knows its block, its shield and its perks.

**Staggers and knockdowns.** A power attack or a bash that staggers an enemy staggers it in every game; a giant's club that sends a friend flying sends them flying for you too.

*How:* the NPC's runner applies the stagger and its copies replay it from the runner's graph events; a player's own stagger is replayed on their stand-in.

**Bows and crossbows.** You see a friend nock, draw, aim and loose, and the arrow fly. It hits what it hit in their game, once; arrows stuck in a body can be picked up from it.

*How:* the draw and release events are replayed; the stand-in launches a harmless copy of the arrow along the friend's aim (the Fallout 4 mod's harmless shots); the friend's game reports the hit as damage to the target's runner. Arrows left in bodies are loot on the body like any other.

**Magic.** Friends cast with either hand or both: fire-and-forget spells (a firebolt, an ice spike) fly; concentration spells (flames, sparks, healing hands) stream for as long as they hold them; dual casting looks and hits like dual casting. A friend's healing spell on you heals you. Wards block spells in front of the friend who holds them. A rune a friend places is there for everyone and goes off once. Cloak spells burn what stands around the caster.

*How:* the cast events of each hand are replayed and the stand-in casts a harmless copy of the same spell, so the effect shows; what a spell does to an NPC is applied by its runner, from the caster's report. A spell on a friend's stand-in (a heal, a buff) is sent to that friend's game and applied to the real player there. Runes are hazards placed at a spot: shared as an object, set off once.

**Conjured creatures and weapons.** A friend's summoned atronach or familiar, or a corpse they raise, fights beside them in your game. Bound weapons appear in their hands.

*How:* a summoned creature is an NPC made at run time, run by its caster's game and shown as a copy elsewhere (the Fallout 4 mod's "spawns"). A raised corpse is an NPC everyone already has: it passes to the caster's game while it lasts. Bound weapons are gear in a hand, sent as gear is.

**Shouts.** A friend's shout plays its voice, its effect and its words: Unrelenting Force throws enemies back in every game, Fire Breath burns, Whirlwind Sprint dashes. Shouts that change the world (Clear Skies, the storm shouts, calling a dragon) change it for everyone.

*How:* a shout is cast like a spell and replayed as one; its effect on NPCs is applied by their runner. Weather shouts are sent to the host's game, which owns the weather. Slow Time is an open question.

**Kill moves.** A finishing blow can play its kill move, and friends see it.

*How:* a kill move is a paired animation between the killer and the victim. It is played only when the killer's game runs the victim (a hit makes it so); the others replay both sides from the events. An NPC's kill move on a player becomes that player going down (below). Hard to keep in step: v0.1 can turn kill moves off in a session.

**Ragdoll and bodies.** The dead fall where they fall and stay there for everyone, so you loot the same body in the same place.

*How:* each game ragdolls its own copy; once the body settles in the runner's game, its resting place is sent and the copies are moved to it.

**Damage and health.** An enemy's health is the same in every game, within a hair, and an enemy dies once, at the same moment, for everyone. Sneak attack bonuses, enchantments and poisons on a weapon count.

*How:* the runner reports health as a fraction after hits and when it changes otherwise; a hit by someone else goes as numbered damage the runner applies and answers (the Fallout 4 mod's health design). The attacker's game works out the damage with its own skill, perks and sneak bonus. Soul Trap fills the gem of the player who cast it, in their own game.

**Enemies fight every player.** Enemies attack whichever player is nearest or most dangerous, not only the one whose game runs them, and their hits hurt you with your own armor and resistances counting.

*How:* in the runner's game the stand-ins are valid targets; a hit on a stand-in goes to its player's game as the blow's raw damage and kind, which that game applies to its player through its own armor, block and perks (better than the Fallout 4 mod, where a friend's game's enemy ignores your perks).

**Friendly fire.** Off by default: friends' weapons, spells and shouts pass through each other. The host can turn it on.

*How:* a host rule in the welcome; with it off, a hit or area effect on a stand-in is not sent on.

**Going down and dying.** A blow that would kill a player puts them down instead: they kneel, bleeding out, and a friend has about 45 seconds to help them up. Nobody coming, the player dies, by the host's rules.

*How:* the Fallout 4 mod's downed rules are game-free and carry; Skyrim's own bleed-out state (as essential characters have) shows it. What death then does is an open question.

**Leveled enemies and group difficulty.** Everyone meets the same enemies, built at a level the host chooses (the group's average by default). The host can make enemies tougher for each extra player.

*How:* the runner's game picks which leveled NPC appears and sends that pick (the Fallout 4 mod's "rolls"); a level rule from the host; the group difficulty multiplier carries.

## 3. Quests

**How the Fallout 4 mod does it.** Every game runs its own copy of every quest. When a quest reaches a new stage in one game, the others are told and set that stage too, never going backwards, so the quest's own scripts run in every game: doors open, NPCs move on, rewards are given to each game's own player. The host chooses one story for everyone (the default) or each player's own. A rule book says, quest by quest, what cannot be shared as it is: steps that teleport the player become an offer to follow, steps that turn a faction hostile wait until no player stands among that faction, timers are the host's, console quickstarts stay with whoever typed them. Radiant jobs are shared by sending the targets one game picked, so every game runs the same job. Creatures a quest makes at run time are paired across games so they are one enemy, not two. A "more quests" switch shares the misc quests that a scan of the game's files shows play out the same everywhere (about 80% of its quests with it on). What can't be shared stays each player's own, and the page of shared systems says so.

**How SkyrimCoop should do it.** The same, with Skyrim's own hard cases below. The host's world is the truth: a friend who joins is brought forward to the host's stages, and what the group does is kept in the host's save.

**Shared stages.** The main quest, side quests and the guild questlines (Companions, College of Winterhold, Thieves Guild, Dark Brotherhood, the Dawnguard and Dragonborn stories) move on for everyone, whoever does the step. Everyone's journal shows the same objectives and markers.

*How:* the Fallout 4 mod's stage watch: read each quest's done stages, send new ones, set them in the other games (forward only), with a rule book per quest. Skyrim's quest type field and stage flags decide the first filter; the rest comes from an audit of every quest in the base game, done by a scan of the game's files as the sister mod's quest scan does.

**Talking and choices.** One player talks to an NPC at a time. Friends nearby see the subtitles and hear the lines from the NPC. The choice the talker makes (persuade, intimidate, bribe, agree, refuse) counts for the group when it moves a quest on. Persuasion uses the talker's own speech skill.

*How:* talking takes the NPC over to the talker's game; the lines' topic and response are sent and played on the copies with their voice files; a friend who tries to talk to an NPC in a conversation is told it is busy. Skyrim's dialogue does not pause the world, which helps.

**Scenes written for "the player".** Scenes that wait for the player (an NPC leading you somewhere, a speech at a gathering) play in the game of the player they are about. Friends see them through the NPCs' copies.

*How:* a scene runs in the game that runs its NPCs; its lines are sent as conversations are. Scenes whose actors would be run by two games at once are found by the audit and given to one.

**Quest-made enemies paired.** When a quest makes its enemies at run time (a bandit leader, a necromancer, a quest's draugr), they are one enemy in the session: hurt by everyone's hits, dying once, and counted by the quest in every game.

*How:* the Fallout 4 mod's quest-actor pairing: the game that shares such an enemy first names its quest and alias, and the other games pair their own creature from the same alias with it instead of placing a copy. Reference collections (several enemies in one alias) are paired one by one, which the sister mod left for later.

**Quests the Story Manager starts.** Many Skyrim quests start from an event: entering a place, a kill, a crime, an item picked up, a script's story event. Each game's Story Manager picks its own quest and targets, so the same event could start different things in each game. In a session the quest is started once, in the game where the event happened, and the others start the same quest with the same targets.

*How:* the Fallout 4 mod's radiant sharing: the starting game sends the quest and what its aliases hold (each named by plugin and id); the others start it with those aliases forced. Events that only one game sees (an item a friend picked up) start the quest only for that player unless the quest is on the shared list.

**Radiant quests.** Bounties from the Jarls, the Companions' jobs, the College's errands, the Thieves Guild's jobs from Delvin and Vex, the Dark Brotherhood's contracts, and their many cousins pick their targets at random. In a session one job is the group's: everyone gets the same target in the same place, and finishing it finishes it for everyone.

*How:* as above, by forced aliases. Radiant quests that repeat are the hard case: a second run of the same quest must not be read as the first. v0.1 keeps radiants each player's own; v0.2 shares them.

**Misc quests and favors.** "Bring this to", "find my", the Jarls' favors and the hundreds of little errands. Shared with the host's "more quests" switch when the scan shows they play the same in every game; otherwise each player's own.

*How:* the sister mod's safe-quest scan, rewritten for Skyrim's quests and scripts: a quest qualifies when it runs once, its aliases are fixed, and none of its stages teleports the player, changes a faction, takes the player's gear or starts a quest each game runs its own way.

**The Civil War.** Choosing a side, the fort battles and the attacks on Whiterun, Windhelm and Solitude are shared: the group fights one war, and when a hold changes hands its guards, Jarl and flags change for everyone.

*How:* the war is a chain of quests whose stages change the holds through scripts; shared stages run those scripts in every game. The fort battles pick their fort at random (forced aliases, as radiants) and put many soldiers on the field at once, which tests the NPC budget. The side is an exclusive choice (open questions).

**Dragons and word walls.** Dragon attacks of the main quest happen for everyone. Every player near a dragon when it dies absorbs its soul. Word walls teach their word to each player who walks up to them, so each player learns words for themselves.

*How:* a dragon's death is shared like any death, and each game's own death script lets its player absorb the soul if near. Word walls are triggers each game runs for its own player; nothing is sent. A word a quest teaches (the Greybeards') reaches each player with the stage.

**Guilds, ranks and titles.** When the group joins a guild, rises in it or becomes Thane, every player who was part of it gets the membership, the rank and the title.

*How:* the stage that makes the talker a member runs its script in every game, on each game's own player. Housecarls and other followers a title gives are followers (World, below).

**Quest items.** A quest item one player picks up is given in every player's game, so nobody is stuck waiting for the one who holds it.

*How:* a shared quest's pickup is sent like a stage; each game gives its own player the item from its own quest alias. Quest items can't be dropped or given, so this is the only way they move.

**Rewards.** When the group finishes a quest, each player gets its reward once: the gold, the item, the Daedric artifact, the perk.

*How:* the Fallout 4 mod's reward book, which gives each player a shared quest's reward once, whichever game finished it. Rewards that a script gives only to the speaker are found by the audit.

**Steps that move a player.** A quest step that teleports the player (a summons to Sovngarde, a Black Book's pull into Apocrypha, a guild leader's "come with me") moves only the player it happened to. Friends are offered to follow.

*How:* the Fallout 4 mod's follow offer: such stages are marked in the rule book, set in the friend's game without the move, and the friend is offered a teleport to their friend.

**Exclusive choices.** Some choices close another path: the Stormcloaks or the Legion, the Dawnguard or the vampires, the Dark Brotherhood or its destruction, some Daedric quests. The group lives with the choice made in the host's world.

*How:* shared stages carry the choice; a stage that turns a faction hostile waits until no player stands among that faction (the Fallout 4 mod's hostile hold). Who may make such a choice is an open question.

**What stays each player's own.** The Helgen opening (played alone; join after it), word walls, standing stones and shrine blessings, skill books, diseases, vampirism and lycanthropy as a condition, marriage and adoption, a console quickstart, and quests from mods only one player has. Each is a player's own because it is about that character, not the world.

*How:* a per-player list in the rule book, beside the shared one; every quest of the base game ends up on one of the two, none left "unknown" (PLAN.md, phase 5).

**Catching up.** A friend who joins late, reconnects or loads a save is brought forward to the host's stages and the world's state. Stages a friend has gone past in their own game are not taken back.

*How:* the server keeps every shared stage of the session and sends them with the world state to a joiner, as the Fallout 4 mod does.

## 4. Mods

Lots of people play Skyrim with mods, often hundreds. Asking a group to have exactly the same list in exactly the same order would shut most of them out. The aim is "any mod": different mods, the same mods in another order, light or full plugins, all fine.

**How the Fallout 4 mod does it.** Since its protocol 36 it lets in a friend with any load order. Every object in a message is named in the session's numbering: a table of plugins built from the host's load order, with each joiner's extra plugins added at the end. Each game translates at its edge, plugin by plugin, by file name (in any case), so a reorder changes nothing, a light plugin in another light slot maps, and a plugin light in one game and full in the other maps too. Forms made at run time pass as they are. What one game does not have is skipped, never guessed: a message about a mod's NPC, door or quest is dropped, a mod's armor on a friend is left off, a mod's weather becomes "none". The log names each missing plugin once. The joiner sees "Your mods differ from the host's" and a *Compare mods* page that lists what is missing and what is extra, and says that plugins in another place need no change. The host can name cosmetic plugins (textures, sounds, menus) that may differ without a notice. Versions are not compared: two versions of a plugin count as one. A mod's change to a vanilla record (an NPC's new gear) is shared as that vanilla record, so each game shows its own version of it.

**SkyrimCoop today.** The hello already carries the whole load order (protocol 3: up to 254 full and 4096 light plugins, in one Steam message), and the form id arithmetic is carried as is (`common/FormIds.h`). The server still refuses any game whose plugins differ in name or order. Order not mattering is the next step, and it is mostly carrying code that already works.

**Any load order, order never matters.** You and your friends may load the same mods in any order, and may have mods the others don't. Objects are named by the plugin file that made them and their number inside it, never by a place in the load order.

*How:* carry the sister mod's `FormTranslate.h` and its session plugin table: every form id on the wire is the session's; each game maps by plugin name at `Session`'s send and receive; a message type without a case in the translator stops the build. Full plugins (`.esm`, `.esp`) and light plugins (`.esl`, and `.esp` or `.esm` flagged light) are mapped apart, a plugin light in one game and full in the other included; ids made at run time (`FF`) pass untouched.

**What must match.** The game version (1.7.104 today), the same SkyrimCoop protocol, and SKSE with the Address Library. Nothing else is required to join.

*How:* the hello's checks: protocol version, password, room. The load-order hash is needed only for a hello without a list (a test bot).

**When a friend lacks a mod.** You can still play. What comes from a mod only one of you has is left out for the other: its NPCs and creatures are not there (and their fights, deaths and loot are skipped), its armor is missing from your friend's stand-in, its items can't be given, its quests are that player's own, and a friend standing in a mod's new place disappears from your game until they come back out, with their name and "in a place your game doesn't have" on the friends list. A friend of a race from a mod shows as a vanilla race with their name. On joining, a notice names how many mods differ, and the menu lists them.

*How:* the skip rules above, and the sister mod's `ModDiff.h` for the notice and the list. A stand-in's race falls back to a vanilla race by the host's rule (open questions).

**Cosmetic mods.** Texture, mesh, sound, lighting, body and animation replacers change only how your own game looks and sounds; they never need to match. A friend's stand-in uses your game's animations, so a friend's animation mod shows in your game only if you have it too. The host can list plugins that may differ without a notice.

*How:* the host's ignored-plugins list, carried; replayed graph events the viewer's behavior files lack (generated behaviors differ between setups) are dropped.

**Mods that change records.** A mod that rebalances weapons, changes an NPC's looks or edits a quest is shared as the vanilla record it changes: each game shows its own version. Health and damage stay in agreement because health is sent as a fraction and damage is applied by the NPC's runner. Mods that change leveled lists differently can still show different enemies, unless the runner's pick exists in the other game.

*How:* translation by the overridden form's own plugin; the leveled pick is sent as the chosen NPC, used when the viewer has it.

**Script-heavy mods.** Mods that run their own scripts (survival needs, new combat rules, a new menu) run in each player's own game for that player. What they do to the shared world goes through the same shared events as anything else: an NPC they spawn is shared as a spawn, an item they move is shared as a move. Mods that keep their own state in their scripts keep it per game. A big scripted mod is played as each player has it; some will do odd things in a session, and the notice and the log say which plugins differ so a report can name them.

*How:* nothing special at first: no Papyrus state is mirrored. Mods found to break a session are listed in the install page with what goes wrong, as the sister mod does for its known cases.

**Creation Club and the Anniversary Edition.** Its files are plugins like any other. Players with the Anniversary Edition's extra content and players with only the free Creation Club files can play together; what one lacks is left out as for any mod.

*How:* nothing beyond the above.

**Versions of one mod.** Two versions of a mod with the same file name count as the same mod. If they differ in what they contain, things from the newer one can be missing or odd for the other.

*How:* as the sister mod. Whether to warn when a plugin's file differs (a checksum taken at start) is an open question.

## 5. The world

The host's world is the world. What anyone changes in it is changed for everyone and kept in the host's save.

**Doors and locks.** A door opened or closed, a lock picked or opened with a key is so for everyone. A door a quest or a lever opens opens for everyone too.

*How:* the sister mod's door and lock sharing: watch what players activate and the doors near them, send open and lock changes, set them in the other games.

**Containers and loot.** What one player takes from a chest, a barrel, a sack or a body is gone for the others; what one puts in is there. Chests hold the same loot in every game: the host's game rolls a container's contents the first time anyone opens it.

*How:* the sister mod's container ledger (numbered changes, applied in order); its books are game-free and carry. Skyrim's items keep their tempering, enchantment, poison, charge and name with them. The sister mod lets each game roll its own chest; sending the host's roll is new.

**Items lying about.** An item picked up from a table or the ground is gone for everyone. Things knocked over or thrown end up in the same place.

*How:* the sister mod's pickups and moved objects: pickups disable the reference elsewhere; physics objects settle in the game that moved them, and their resting place is sent.

**Giving and dropping items.** Drop an item near a friend and they can pick it up, with its enchantment, tempering and name. Only one of you can take it.

*How:* the sister mod's drops: the item is sent with its extra data and placed in the friend's game. Items a player made at run time (their own enchanted gear) are sent whole, since only their game has them.

**Plants, ore and fish.** A flower, a mushroom or a nest picked by one player is picked for everyone; an ore vein mined out is mined out. They grow back on the host's schedule.

*How:* harvested flags on flora shared like pickups; ore veins as furniture with a count.

**Activators, puzzles and traps.** Levers, chains, buttons, rotating puzzle pillars and claw doors turn for everyone. Pressure plates, swinging blades, dart traps, falling rocks and oil set alight go off once, and are set off or disarmed for everyone.

*How:* activations by a player are sent and replayed on the same reference in the other games (the sister mod's "a friend's press is made in your game by their stand-in"); trap state from its trap rules, rewritten for Skyrim's traps.

**Crafting.** Each player smiths, enchants, makes potions and cooks for themselves, with their own materials and skills; what they make is theirs, and friends see them at the station.

*How:* nothing shared but the furniture animation; giving moves the result.

**Time and weather.** The host's clock and weather are everyone's. When the host sleeps or waits, everyone's day moves on; a friend who sleeps or waits is brought back to the host's time of day.

*How:* the sister mod's clock (host's time sent, others follow) and weather while in the same worldspace as the host; Clear Skies from a friend is sent to the host.

**Cells, interiors and travel.** Everyone goes where they like: two players can be in two different dungeons. A friend shows in your game when you are in the same interior or near them outside. Through a door, a friend leaves your game and shows up again when you come through. A friend can be teleported to another (not in combat or a conversation), and fast travel offers to bring the group along.

*How:* stand-ins exist while their friends share an interior cell or are within about two cells outside (written). Teleport to a friend and the fast-travel offer are the sister mod's, carried. Carriages are fast travel.

**NPC health and deaths.** NPCs are as hurt or as dead in every game. The dead stay dead for everyone until the host's game resets the place.

*How:* deaths and health as in Combat; the server keeps the session's deaths and health and sends them to a joiner.

**Places that reset.** A cleared dungeon fills up again after the game's usual days, on the host's game's schedule, for everyone.

*How:* the host's game's cell reset decides; the sister mod's "deaths that end" carries.

**Dragons in the sky.** A dragon circling, diving, breathing fire and landing does so in every game, run by one player's game. Shooting it down, it lands for everyone; Dragonrend works on it whoever's game runs it.

*How:* a dragon is an NPC like any other, owned and copied, but its flight is not on the ground: its states need the height, pitch and roll and a higher rate in flight, and its landings and take-offs are graph events. Random dragon attacks are Story Manager starts: one game starts one, the others get it.

**Followers.** Lydia, Serana, a hireling or a dog follows the player who recruited them, in every game. Each follower has one owner: the first to recruit them keeps them, and only they can talk to, trade with or dismiss them.

*How:* the sister mod's companions: one owner each, the follower runs in the owner's game, a second player's recruiting is refused with a notice. Skyrim's followers are a faction and an alias rather than Fallout 4's companion system: the rules carry, the engine side is new.

**Horses.** Your horse is yours: bought, named and stabled in your own game, and shown to friends while you ride or lead it. Friends can't take it.

*How:* a horse is a follower-like NPC owned by its player's game.

**Houses.** A house bought in the host's world belongs to the group: anyone can go in, sleep there and use its chests, which are shared like any container. Decorations bought from the steward are there for everyone. Building a Hearthfire house together comes later.

*How:* a house purchase is quest stages and doors, shared as such. Mannequins, weapon plaques and display cases are objects with contents: later, with the sister mod's weapon racks as the model. Hearthfire's building menus run scripts that place pieces: a quest-and-object problem for later.

**Crime and bounty.** A crime is the criminal's own: their bounty, the guards' reaction to them, and their jail. A friend's crime doesn't put a bounty on you.

*How:* the game that runs the witness and the guards judges the crime against the player who did it; bounties are per player; arrest moves only that player.

**Vendors and trainers.** Each game's merchants keep their own goods and gold; you trade and train in your own game.

*How:* nothing shared, as in the sister mod.

**Books and notes.** Reading is your own: skill books raise your skill, and notes and letters are yours to collect.

*How:* nothing shared.

## 6. The session

**Steam invites and joining.** The host hosts from their own save. Friends join from the Steam overlay's invite, from "Join Game" on the friends list, or from a link. Only the host's Steam friends, or players they invite, can join. Nobody sees anyone's address.

*How:* carried. In a game, a Steam lobby, pings over the relay and a first session carrying a player's state from one game to the other have been seen in the rig's logs; the invite and "Join Game" have not been tried yet.

**Join by address and a dedicated server.** For groups who want it: join by address and port on a LAN or a forwarded port, or run the small server program. Steam remains the main way.

*How:* carried (`sTransport = enet`, `SkyrimCoopServer.exe`); the sister mod's join-by-address menu with a password and the host's approval comes later.

**Password and group size.** The host can set a password. Four players by default, up to eight.

*How:* carried and unit-tested.

**Host rules.** The host decides for the group: friendly fire, one story for everyone or each player's own, more quests shared, the level enemies are built at, group difficulty, and what happens when a player dies. Each player's own difficulty still sets the damage they deal and take.

*How:* the welcome carries the host's rules, as in the sister mod; each comes with its feature.

**Joining late and dropping out.** A friend who joins late, or whose game drops and comes back, catches up on the world and the quests and keeps their place in the group.

*How:* each game's random identity keeps a returning player's id (carried); the server's world state and stages are sent on joining (to build per system).

**Saves.** The host's save keeps the world. A friend's own save keeps their character (level, skills, perks, inventory, spells and shouts) and nothing broken: friends' stand-ins and copies are never written into anyone's save, and loading a friend's save alone afterwards plays as normal.

*How:* the save guard (written: stand-ins disabled before a save, swept after a load, the co-save lists them). Copies of NPCs, spawns and placed items are added to it system by system. What a friend's save keeps of quests done together is an open question.

**The opening and alternate starts.** A new character plays the Helgen opening alone and can host or join once out of Helgen. With an alternate-start mod, the start counts as the opening until the player leaves the start's room.

*How:* the sister mod's opening rule (its Vault 111), with Skyrim's Helgen quest and the start room of the alternate-start mods most players use.

**The menu.** A menu in the game: host, invite, join a friend, who is here, the host's rules, compare mods. Until it exists, hosting and inviting are an ini setting and the console.

*How:* Skyrim's menus are Flash files in ActionScript 2, built from source by the workflows; no compiled file is committed.

**Friends on the map, compass and pings.** Friends show on the world map, the local map and the compass in their colour, with their name. A key pings a spot, an enemy or an item for everyone.

*How:* the sister mod's map marks, compass and ping, carried as rules; Skyrim's map and compass are their own menus.

**Voice chat.** Steam voice, loud up close and fading with distance.

*How:* the sister mod's voice code is mostly game-free; where the sound plays is new.

**Languages.** The mod's own texts in 14 languages, English as the fallback. Languages whose letters the game's font lacks need the game in that language.

*How:* the text files are carried and checked for missing keys; Skyrim's fonts are to be checked for Cyrillic and the East Asian scripts.

**Logs and bug reports.** The mod's log names players as "player N" and masks addresses, so it can be attached to a public bug report.

*How:* carried and tested over every log line.

## 7. The feature chart

"In the Fallout 4 mod" is F4Multiplayer 0.21: shipped, partial (some of it), or no (not built, or not a thing in Fallout 4). "Target" is when SkyrimCoop means to have it: v0.1 (first playable), v0.2, later, or not planned. Difficulty is a rough size: S (days), M (a week or two), L (weeks, with engine work), XL (the hardest problems, a month or more).

| Feature | In the Fallout 4 mod | SkyrimCoop target | Difficulty | Notes |
| --- | --- | --- | --- | --- |
| **Players: movement and animation** | | | | |
| Walk, run, sprint | shipped | v0.1 | M | First form built; legs seen in the rig's logs only. |
| Sneak | shipped | v0.1 | M | Detection with the friend's sneak skill is v0.2. |
| Jump, fall, land | shipped | v0.1 | S | Events written. |
| Swim | partial | v0.1 | M | The sister mod sends a swim flag; no swim work listed beyond it. |
| Turning, looking, aiming | shipped | v0.1 | M | Aim pitch matters for bows and spells. |
| Horses (mount, ride, dismount) | no | v0.2 | L | No mounts in Fallout 4. Mounted combat later. |
| Sitting and furniture | partial | v0.2 | M | The sister mod shows seats and beds, not work stations. |
| Drawing and sheathing | shipped | v0.1 | S | Flag sent; what each hand holds to add. |
| Gear and looks (race, armor, weapons) | shipped | v0.1 | M | Race, sex, weight and armor sent today. |
| Face, hair, skin | shipped | v0.2 | L | Needs a face on a stand-in without a run-time base in a save. |
| Name and health | shipped | v0.1 | S | |
| Werewolf and vampire lord | no | later | L | Fallout 4 has none. |
| Menus ("[in a menu]") | partial | v0.1 | S | Menus pause only your own game. |
| **Combat** | | | | |
| Who runs each NPC (ownership) | shipped | v0.1 | XL | The core of combat; the sister mod's largest system. |
| Melee swings and power attacks | shipped | v0.1 | L | Replayed graph events; hits judged by the attacker's game. |
| Blocking and bashing | partial | v0.1 | M | The sister mod has melee and lacks gun bashes. |
| Staggers and knockdowns | partial | v0.2 | M | |
| Bows and crossbows | partial | v0.1 | L | Fallout 4 has guns, not bows: its harmless shots and hit reports serve arrows. |
| Magic: fire-and-forget and concentration | no | v0.1 | L | Fallout 4 has no spells; projectiles are the nearest. |
| Magic: dual casting | no | v0.2 | M | |
| Magic: healing and buffing a friend | no | v0.1 | M | Co-op needs it from the start. |
| Magic: wards and cloaks | no | v0.2 | M | |
| Magic: runes | partial | v0.2 | M | Like the sister mod's mines: set off once. |
| Conjured creatures and raised dead | partial | v0.2 | L | Like the sister mod's spawns. |
| Bound weapons | no | v0.2 | S | Gear in a hand. |
| Shouts | no | v0.2 | L | Slow Time is an open question. |
| Kill moves | no | later | XL | Off in a session in v0.1. |
| Ragdoll and bodies | partial | v0.1 | M | Deaths are shared there; moving bodies to the runner's resting place is new. |
| Damage and health agreement | shipped | v0.1 | L | The sister mod's numbered damage and answers. |
| Enemies fight every player | partial | v0.1 | M | Your own armor and perks count here, unlike the sister mod. |
| Friendly fire (host rule) | shipped | v0.1 | S | |
| Going down and being helped up | shipped | v0.1 | M | Skyrim's bleed-out state. |
| Death of a player | shipped | v0.1 | M | Respawn or reload: open question. |
| Leveled enemies and group difficulty | shipped | v0.2 | M | |
| Soul Trap, enchantments and poisons on hits | no | v0.2 | M | |
| **Quests** | | | | |
| Shared stages (main, side, guild quests) | shipped | v0.1 | XL | Forward only; per-quest rule book from an audit. |
| Journal and markers | shipped | v0.1 | S | Follows the stages. |
| Talking and choices | partial | v0.1 | L | One talker; others see subtitles and hear lines. |
| Scenes written for "the player" | partial | v0.2 | L | |
| Quest-made enemies paired | shipped | v0.2 | L | Collections one by one is new. |
| Story Manager starts | partial | v0.2 | L | The sister mod shares encounters and radiants this way. |
| Radiant quests (bounties, guild jobs, contracts) | partial | v0.2 | XL | Each player's own in v0.1. |
| Misc quests and favors ("more quests") | partial | v0.2 | L | Needs the scan of Skyrim's quests. |
| The Civil War | no | later | XL | Many scripts, many soldiers, an exclusive choice. |
| Dragon attacks and dragon souls | no | v0.1 | M | Souls follow from shared deaths. |
| Word walls (each player's own) | no | v0.1 | S | Nothing to send. |
| Guilds, ranks and titles | shipped | v0.1 | M | Follows from shared stages. |
| Quest items | partial | v0.1 | M | Given in each game. |
| Rewards, once per player | shipped | v0.1 | M | The sister mod's reward book. |
| Steps that move a player (follow offer) | shipped | v0.2 | M | |
| Exclusive choices and hostile turns | shipped | v0.2 | M | The hostile hold carries. |
| Per-player quests list | shipped | v0.1 | M | Part of the audit. |
| Catching up on stages | shipped | v0.1 | M | |
| **Mods** | | | | |
| Any load order, order never matters | shipped | v0.1 | M | Carry the translator; the hello already carries the list. |
| Light and full plugins, either way round | shipped | v0.1 | S | Comes with the translator. |
| Only the game version, protocol and SKSE must match | shipped | v0.1 | S | |
| A missing mod left out, never guessed | shipped | v0.1 | M | |
| "Your mods differ" notice and compare page | shipped | v0.2 | M | The log lists them in v0.1; the page needs the menu. |
| Mod races fall back to a vanilla race | no | v0.2 | S | |
| Cosmetic plugins listed by the host | shipped | v0.1 | S | An ini list. |
| Animation and behavior mods | no | v0.1 | S | Unknown events dropped. |
| Mods that change records (overrides, leveled lists) | partial | v0.2 | M | Leveled picks sent by the runner. |
| Script-heavy mods | partial | v0.1 | S | Each runs in its player's game; known trouble listed. |
| Creation Club and Anniversary Edition content | shipped | v0.1 | S | As any mod. |
| Warning when a mod's version differs | no | later | S | Open question. |
| **The world** | | | | |
| Doors and locks | shipped | v0.1 | M | |
| Containers and loot | shipped | v0.1 | L | The host's roll for a chest's contents is new. |
| Items lying about, pickups | shipped | v0.1 | M | |
| Things knocked about | shipped | v0.2 | M | |
| Giving and dropping items | shipped | v0.1 | M | With tempering, enchantment, poison, name. |
| Plants, ore and fish | shipped | v0.2 | S | |
| Activators, puzzles and traps | shipped | v0.1 | L | Nordic puzzle doors are on the way through most dungeons. |
| Crafting (each player's own) | partial | v0.2 | S | Only the animation is shared. |
| Time and weather | shipped | v0.1 | M | |
| Cells, interiors, roaming apart | shipped | v0.1 | M | Range rule written. |
| Teleport to a friend | shipped | v0.1 | S | |
| Fast travel together | no | v0.2 | M | The sister mod has each player travel alone. |
| NPC health and deaths kept | shipped | v0.1 | M | |
| Places that reset | partial | v0.2 | M | |
| Dragons in the sky | no | v0.1 | L | Flight needs its own states. |
| Followers, one owner each | partial | v0.2 | L | |
| Horses, owned | no | v0.2 | M | |
| Houses and home containers | partial | v0.2 | M | Fallout 4's settlements are the nearest thing; Hearthfire building later. |
| Mannequins and displays | partial | later | M | The sister mod's weapon racks are the model. |
| Hearthfire building | no | later | L | Settlement building is the nearest; its code does not carry. |
| Crime and bounty, each player's own | no | v0.2 | M | |
| Vendors and trainers, each game's own | shipped | v0.1 | S | Nothing to build. |
| Books and notes, each player's own | shipped | v0.1 | S | Nothing to build. |
| **The session** | | | | |
| Steam invites and join | shipped | v0.1 | M | Lobby and relay seen in a game. |
| Join by address, dedicated server | shipped | v0.2 | S | Carried; the menu for it later. |
| Password and group size | shipped | v0.1 | S | |
| Host rules | shipped | v0.1 | M | Each with its feature. |
| Joining late, dropping out, reconnecting | shipped | v0.1 | L | |
| Saves: host's world, friends' characters, nothing broken | shipped | v0.1 | L | Save guard written for stand-ins. |
| The opening and alternate starts | partial | v0.1 | M | The sister mod's alternate-start support is untried. |
| The menu in the game | shipped | v0.2 | L | ActionScript 2 from source. |
| Friends on the map and compass | shipped | v0.2 | M | |
| Pings | shipped | v0.2 | S | |
| Voice chat | shipped | later | M | |
| Languages | shipped | v0.1 | S | Texts carried; fonts to check. |
| Logs fit for public bug reports | shipped | v0.1 | S | Carried. |
| Skyrim VR, the 2011 game, GOG and Microsoft Store | no | not planned | XL | DESIGN.md, decisions made. |

### The phases

**v0.1, first playable.** The smallest session that is fun: invite a friend, see each other move with your gear, walk into a dungeon together, fight the same draugr with swords, bows and spells, heal each other, get a fallen friend back up, solve the puzzle door, loot the same chests and bodies, and move the main quest and side quests on together, each with your own mods in your own order. That needs, from the chart: walking, running, sprinting, sneaking, jumping, swimming, aiming, drawing, gear, names and health, menus; NPC ownership, melee, blocking, bows, simple magic and healing, ragdolls, damage and health agreement, enemies fighting every player, friendly fire off, going down and dying; shared stages for the main, side and guild quests with journal, talking, quest items, rewards, the per-player list, dragon attacks and souls, word walls and catching up; any load order with missing mods left out, cosmetic and animation mods, script-heavy mods as they come, Creation Club content; doors, locks, containers, pickups, giving, activators and traps, time and weather, roaming apart, teleporting to a friend, deaths kept, dragons in the sky; Steam invites, password, host rules, joining late, saves, the opening, languages and clean logs. In PLAN.md's terms, v0.1 is phases 1 to 5 each shown on the rig for their core, then the release gate and a long two-game test.

**v0.2, the full first game.** Faces, horses, furniture, staggers, the rest of the magic (dual casting, wards, cloaks, runes, conjuring, bound weapons), shouts, leveled enemies and group difficulty, scenes, paired quest enemies, Story Manager starts and shared radiant quests, more quests, the follow offer and exclusive choices, the compare-mods page, mod races, leveled picks across mods, followers, owned horses, houses, crime, fast travel together, the menu, the map and pings.

**Later.** The Civil War, kill moves, werewolves and vampire lords, mounted combat, mannequins and displays, Hearthfire building, voice chat, a warning for differing mod versions.

**Not planned.** Skyrim VR, the 2011 game, the GOG and Microsoft Store versions (DESIGN.md). Large public servers: this is a mod for a group of friends.

## 8. Open questions for the maintainer

1. **Who runs NPCs: the host's game alone, or the sister mod's ownership (the nearest or fighting player's game)?** Recommended: the sister mod's ownership, with the host's game breaking ties. A host-only rule makes every friend's swing wait for the host's game, and fails when the group splits up. DESIGN.md's combat line would change to say so.
2. **What happens when a player dies and nobody helps them up?** Recommended: in a session, a player respawns near the nearest friend (or the last door they came through) with a short penalty, and no one reloads a save. A host who reloads takes the whole group's world back with them. The host can choose "reload your own save" instead, as the sister mod does.
3. **What does a friend's own save keep of quests done in the host's world?** Recommended: the stages they reached together, as in the sister mod: credit for what they did. Their world (dead NPCs, emptied chests) is their own game's as it last was, and stand-ins and copies are never kept.
4. **Who may make an exclusive choice (a Civil War side, the Dawnguard or the vampires, the Dark Brotherhood's fate)?** Recommended: anyone, as with every other step, with a notice to everyone naming the choice and who made it; a host rule "only the host makes faction choices" for groups who want it.
5. **What does Slow Time do in a session?** Recommended: by default it slows everyone's game together for its length (as the sister mod's host option for VATS); the host can switch it to doing nothing in a session.
6. **Kill moves in v0.1?** Recommended: off in a session until v0.2 or later; a finishing blow kills as it would without one. NPCs' kill moves on players become going down.
7. **A friend's character of a mod's race, when you lack that mod: which race do you see?** Recommended: Nord of the same sex, with their real name, and a line in the log; the compare-mods page says why.
8. **Warn when two players have different versions of the same mod?** Recommended: yes, a warning in the compare page, never a refusal; it costs a checksum of each plugin at start.
9. **Whose level do leveled enemies use?** Recommended: the group's average, as the sister mod, with the host able to choose the host's level or the highest.
10. **Dragon souls: one for whoever is closest, or one each?** Recommended: one each for every player near the dragon when it dies. It is what each game's own death script does anyway, and nobody misses shouts.
11. **Home chests: shared like any container?** Recommended: yes, so a group can keep its stash in a house; a player's own items are safe in their own inventory.
12. **The old game (1.5.97) that many heavily modded setups keep: support it?** Recommended: not for v0.1; ask players after v0.1 and decide by demand (DESIGN.md already lists it as a decision to confirm).
13. **Radiant quests in v0.1: each player's own, or held back until they can be shared?** Recommended: each player's own in v0.1 (they work, just not together), shared in v0.2.
