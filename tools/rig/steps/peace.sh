#!/bin/bash
# steps/peace.sh: the rig step `peace` (no protocol change, docs/DESIGN.md "Players": "The world leaves it alone"),
# sourced by steps.sh. A friend's stand-in is no one's fight, talk, crowd or threat here, and friends' weapons and spells
# pass through each other: no actor of this game picks the stand-in as a combat target (the engine's own check is turned
# away), one made to fight it all the same is told to stop, and our player's blows and spells on it make no hit, no
# crime and no fight. Not run in a game yet.
#
#   step_peace     the step
#   bash tools/rig/steps/peace.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# In each game (the game where the stand-in of the other player lives), both players near each other:
#   1. State: the four hooks are in (`peace`: hooks=target:1,talk:1,watch:1,flee:1; one out is a FAIL), the stand-in is a
#      ghost to the engine (its part: ghost=1; the flag is kept on after each of its updates), it is in no fight and has
#      no combat target of its own.
#   2. Friendly fire: the game's player is given an Iron Sword (`actions equip`, set-up), walks up to the stand-in with
#      its virtual pad (`walkto <stand-in>`; ending far from it is a rig fault), draws and swings PEACE_SWINGS times
#      (the pad's right trigger; no attack seen in its own game is a rig fault), readies Flames and casts it at the
#      stand-in (the right trigger held; no magicka spent is a rig fault), and casts Firebolt at it from the console (an
#      injected attack, as the harness injects attacks in combat tests). Then: no hit landed on the stand-in (hitsOn=
#      unchanged; one that landed is a WARN: the ghost flag let it land, and only what follows matters to a player), our
#      player's bounty is unchanged (a FAIL otherwise; paid back after), the stand-in is in no fight, and no actor of the
#      game fights it (`peace targeting`: targeting=0; a guard or townsperson turned on it is a FAIL). The effects
#      refused on it (magicOn=) are said.
#   3. A creature: the game's console places a wolf (PEACE_CREATURE, hostile to everyone) at the stand-in (set-up; the
#      game's player is given PEACE_HEALTH health for it, taken back after) and finds it (`peace find`; none is a rig
#      fault). For PEACE_WATCH s it is read each second: it never has the stand-in as its combat target (PASS; it may
#      fight our player). Picked and then told to stop by the sweep (calmed= rose) is a WARN: the engine chose it on a
#      path the target check does not see. Still on the stand-in at the end is a FAIL. Then the console makes it fight
#      the stand-in (`<wolf>.startcombat <stand-in>`): within PEACE_WAIT it is not on the stand-in, refused at the check
#      (targetRefused= rose) or told to stop (calmed= rose); neither rose is a WARN (the console may have set nothing).
#      Still on the stand-in is a FAIL. The other game has no actor fighting its stand-in either. The wolf is disabled
#      and marked for deletion after.
# Not on the rig yet: a guard's crime alarm at a stand-in's harmless swing in a town, a crowd for a brawl, a
# conversation an NPC starts (they are counted by kind in `peace`: talkAsked=, watchAsked=, fleeAsked=), the stand-in's
# own blows staggering an NPC.

PEACE_CREATURE=${PEACE_CREATURE:-00023ABE}            # Skyrim.esm's wolf (an actor base hostile to every player)
PEACE_WATCH=${PEACE_WATCH:-15}                        # s: the wolf is read each second this long
PEACE_WAIT=${PEACE_WAIT:-8}                           # s: for a wolf made to fight the stand-in to be off it
PEACE_FIND=${PEACE_FIND:-3000}                        # u: the wolf is looked for within this of the game's player
PEACE_REACH=${PEACE_REACH:-120}                       # u: the player walks to within this of the stand-in
PEACE_SWINGS=${PEACE_SWINGS:-3}                       # swings of the sword at the stand-in
PEACE_HEALTH=${PEACE_HEALTH:-5000}                    # the health the player is given while the wolf is out
PEACE_SWORD=${PEACE_SWORD:-00012EB7}                  # Iron Sword
PEACE_FLAMES=${PEACE_FLAMES:-00012FCD}                # Flames
PEACE_FIREBOLT=${PEACE_FIREBOLT:-0012FCD0}            # Firebolt, cast from the console
PEACE_CRIME_FACTION=${PEACE_CRIME_FACTION:-000267EA}  # CrimeFactionWhiterun (Riverwood's hold): a bounty paid back

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# pc_head <peace reply>: the counts (before the first " | ")
pc_head() { echo "${1%% | *}"; }
# pc_standin <peace reply> <stand-in id>: that stand-in's part ("standin player=N id=ID ..."); nothing when not listed
pc_standin() { awk -v p="id=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "standin" && $3 == p { print; exit }' <<<"$1"; }
# pc_hooks_in <peace reply>: all four hooks in
pc_hooks_in() { [ "$(tr ',' '\n' <<<"$(field hooks "$(pc_head "$1")")" | grep -c ':1$')" = 4 ]; }
# pc_found <peace find reply> <field>: a field of the actor found; nothing when none
pc_found() { [[ $1 == "actor id="* ]] && field "$2" "$1"; }
# pc_kind <byKind list> <kind>: how many of that kind ("attack:3,cast:1"), 0 when none
pc_kind() { local n; n=$(tr ',' '\n' <<<"$1" | sed -n "s/^$2://p" | head -1); echo "${n:-0}"; }
# pc_own <actions reply>: our player's part
pc_own() { echo "${1%% | *}"; }
# pc_rose <before> <after>: both numbers and the second larger
pc_rose() { is_num "$1" && is_num "$2" && _cmp "$2" ">" "$1"; }
# pc_on_standin <who>: that game's wolf has a stand-in as its combat target now
pc_on_standin() { [ "$(pc_found "$(dc "$1" "peace find $PEACE_CREATURE $PEACE_FIND")" targetStandIn)" = 1 ]; }
pc_off_standin() { ! pc_on_standin "$1"; }
pc_wolf_found() { [ -n "$(pc_found "$(dc "$1" "peace find $PEACE_CREATURE $PEACE_FIND")" id)" ]; }
pc_walk_over() { [[ $(dc "$1" walkto) == "not walking"* ]]; }
# pc_sid <who>: that game's stand-in of its friend, when placed; nothing otherwise
pc_sid() { placed "$1" && field standin "$(dc "$1" standins)"; }

# peace_state <who>: the hooks, and the stand-in a ghost in no fight
peace_state() {
	local g=$1 label="peace state ($1's game)" r sid part
	section "peace: the $g's game leaves its stand-in of the $(other "$g")'s player alone"
	r=$(dc "$g" peace)
	if pc_hooks_in "$r"; then pass "$label: the four hooks are in" "$(field hooks "$(pc_head "$r")")"
	else fail "$label: the four hooks are in" "$(field hooks "$(pc_head "$r")") (see the log's Peace: lines)"; fi
	sid=$(pc_sid "$g")
	if [ -z "$sid" ]; then rig_fault state "$label: a stand-in of the $(other "$g")'s player placed (set-up)" "$(dc "$g" standins | cut -c1-200)"; return; fi
	part=$(pc_standin "$r" "$sid")
	if [ "$(field ghost "$part")" = 1 ]; then pass "$label: the stand-in $sid is a ghost to the engine" "processGhost=$(field processGhost "$part") ghostSet=$(field ghostSet "$(pc_head "$r")")"
	else fail "$label: the stand-in $sid is a ghost to the engine" "${part:-no part for $sid}; ghostSet=$(field ghostSet "$(pc_head "$r")") noProcess=$(field noProcess "$(pc_head "$r")")"; fi
	if [ "$(field inCombat "$part")" = 0 ] && [ "$(field combatTarget "$part")" = 00000000 ]; then pass "$label: the stand-in is in no fight and picks no target" "inCombat=0 combatTarget=00000000"
	else fail "$label: the stand-in is in no fight and picks no target" "inCombat=$(field inCombat "$part") combatTarget=$(field combatTarget "$part")"; fi
}

# peace_fire <who>: that game's player strikes its stand-in of the other player with a sword, Flames and Firebolt
peace_fire() {
	local g=$1 label="peace fire ($1's player strikes its stand-in)" sid r d p0 p1 s0 s1 o0 o1 a0 a1 m0 m1 b0 b1 t
	section "peace: the $g's player strikes its stand-in of the $(other "$g")'s player (friendly fire off)"
	sid=$(pc_sid "$g")
	if [ -z "$sid" ]; then rig_fault state "$label: a stand-in to strike (set-up)" "$(dc "$g" standins | cut -c1-200)"; return; fi
	r=$(dc "$g" "actions equip right $PEACE_SWORD")
	[[ $r == equipped* || $r == asked* ]] || { rig_fault state "$label: the $g's player holds a sword (set-up)" "actions equip refused: $r"; return; }
	r=$(dc "$g" "walkto $sid 0.5 $PEACE_REACH 20")
	[[ $r == walking* ]] || { rig_fault input "$label: the $g's player walks up to the stand-in" "walkto refused: $r"; return; }
	wait_until 25 pc_walk_over "$g"
	still "$g"; sleep 1
	d=$(dist2 "$(pos_of "$g")" "$(standin_at "$g")")
	if ! is_num "$d" || [ "$d" -gt $((PEACE_REACH * 2)) ]; then rig_fault walk "$label: the $g's player walks up to the stand-in" "it ended ${d:-?} u from it"; return; fi
	say "peace: the $g's player is $d u from the stand-in $sid"

	p0=$(dc "$g" peace); s0=$(pc_standin "$p0" "$sid"); o0=$(pc_own "$(dc "$g" actions)")
	b0=$(field bounty "$(pc_head "$p0")")
	# the sword: drawn, swung
	dc "$g" 'pad button x 100' >/dev/null; sleep 1.5
	for ((t = 0; t < PEACE_SWINGS; t++)); do dc "$g" 'pad trigger r 1 150' >/dev/null; sleep 1; done
	o1=$(pc_own "$(dc "$g" actions)")
	a0=$(pc_kind "$(field seen "$o0")" attack); a1=$(pc_kind "$(field seen "$o1")" attack)
	if ! pc_rose "$a0" "$a1"; then
		rig_fault input "$label: the $g's player swings" "its game shows no attack (attack $a0 -> $a1; seen=$(field seen "$o1"))"
		dc "$g" 'pad button x 100' >/dev/null; still "$g"; return
	fi
	# Flames, held on it; then Firebolt from the console
	r=$(dc "$g" "actions equip right $PEACE_FLAMES")
	[[ $r == equipped* || $r == asked* ]] || { rig_fault state "$label: the $g's player readies Flames (set-up)" "actions equip refused: $r"; dc "$g" 'pad button x 100' >/dev/null; return; }
	sleep 1
	m0=$(field magickaSpent "$(pc_own "$(dc "$g" actions)")")
	dc "$g" 'pad trigger r 1 2500' >/dev/null; sleep 1
	m1=$(field magickaSpent "$(pc_own "$(dc "$g" actions)")")
	if ! pc_rose "$m0" "$m1"; then
		rig_fault input "$label: the $g's player casts Flames at the stand-in" "no magicka spent ($m0 -> $m1)"
		dc "$g" 'pad button x 100' >/dev/null; still "$g"; return
	fi
	r=$(dc "$g" "console player.cast $PEACE_FIREBOLT $sid")
	[ "$r" = ran ] || { rig_fault input "$label: the $g's player casts Firebolt at the stand-in (console: an injected attack)" "the console refused: $r"; dc "$g" 'pad button x 100' >/dev/null; return; }
	sleep 2
	dc "$g" 'pad button x 100' >/dev/null; still "$g"
	sleep 1  # a sweep, for the counts below

	p1=$(dc "$g" peace); s1=$(pc_standin "$p1" "$sid"); b1=$(field bounty "$(pc_head "$p1")")
	if [ "$(field hitsOn "$s0")" = "$(field hitsOn "$s1")" ] && is_num "$(field hitsOn "$s1")"; then
		pass "$label: nothing landed on the stand-in (hitsOn=)" "hitsOn=$(field hitsOn "$s1"); swings $a0 -> $a1, magicka $m0 -> $m1, Firebolt cast"
	else
		warn "$label: nothing landed on the stand-in (hitsOn=)" "hitsOn=$(field hitsOn "$s0") -> $(field hitsOn "$s1"): the ghost flag let them land (only what follows matters to a player)"
	fi
	say "peace: effects on the stand-in refused (magicOn=): $(field magicOn "$s0") -> $(field magicOn "$s1")"
	if is_num "$b0" && [ "$b0" = "$b1" ]; then pass "$label: the $g's player has no bounty for it" "bounty=$b1"
	else fail "$label: the $g's player has no bounty for it" "bounty=$b0 -> $b1 (paid back)"; dc "$g" "console player.paycrimegold 0 0 $PEACE_CRIME_FACTION" >/dev/null; fi
	if [ "$(field inCombat "$s1")" = 0 ]; then pass "$label: the stand-in is in no fight after it" "inCombat=0 combatTarget=$(field combatTarget "$s1")"
	else fail "$label: the stand-in is in no fight after it" "${s1:-no part for $sid}"; fi
	r=$(dc "$g" 'peace targeting')
	if [ "$(field targeting "$r")" = 0 ]; then pass "$label: no actor of the $g's game fights the stand-in after it" "$r"
	else fail "$label: no actor of the $g's game fights the stand-in after it" "$(cut -c1-240 <<<"$r")"; fi
}

# peace_creature <who>: a wolf placed at that game's stand-in, read, then made to fight it
peace_creature() {
	local g=$1 o label="peace creature ($1's game: a wolf at its stand-in)" sid r wid c0 c1 c2 c3 picked=0 onus=0 i f
	o=$(other "$g")
	section "peace: a wolf at the $g's stand-in of the $o's player"
	sid=$(pc_sid "$g")
	if [ -z "$sid" ]; then rig_fault state "$label: a stand-in to place the wolf at (set-up)" "$(dc "$g" standins | cut -c1-200)"; return; fi
	if [ "$(dc "$g" "console player.modav health $PEACE_HEALTH")" != ran ]; then rig_fault input "$label: the $g's player given $PEACE_HEALTH health (set-up)" "the console refused modav"; return; fi
	c0=$(pc_head "$(dc "$g" peace)")
	r=$(dc "$g" "console $sid.placeatme $PEACE_CREATURE 1")
	if [ "$r" != ran ] || ! wait_until 5 pc_wolf_found "$g"; then
		rig_fault state "$label: a wolf placed at the stand-in (set-up)" "console: $r; $(dc "$g" "peace find $PEACE_CREATURE $PEACE_FIND")"
		dc "$g" "console player.modav health -$PEACE_HEALTH" >/dev/null; return
	fi
	wid=$(pc_found "$(dc "$g" "peace find $PEACE_CREATURE $PEACE_FIND")" id)
	say "peace: the $g's console placed the wolf $wid at the stand-in $sid (set-up; the $g's player has +$PEACE_HEALTH health)"

	# the watch: it never picks the stand-in
	for ((i = 0; i < PEACE_WATCH; i++)); do
		f=$(dc "$g" "peace find $PEACE_CREATURE $PEACE_FIND")
		[ "$(pc_found "$f" targetStandIn)" = 1 ] && picked=$((picked + 1))
		[ "$(pc_found "$f" target)" = 00000014 ] && onus=1
		sleep 1
	done
	c1=$(pc_head "$(dc "$g" peace)")
	say "peace: in $PEACE_WATCH s the wolf fought the $g's player: $onus; the target check turned away $(field targetRefused "$c0") -> $(field targetRefused "$c1"), the sweep told $(field calmed "$c0") -> $(field calmed "$c1") to stop"
	if pc_on_standin "$g"; then
		fail "$label: the wolf never fights the stand-in in $PEACE_WATCH s" "it has the stand-in as its target at the end ($picked of $PEACE_WATCH reads); calmed $(field calmed "$c0") -> $(field calmed "$c1"), givenUp=$(field givenUp "$c1")"
	elif [ "$picked" -gt 0 ]; then
		warn "$label: the wolf never fights the stand-in in $PEACE_WATCH s" "it picked it in $picked of $PEACE_WATCH reads and was told to stop (calmed $(field calmed "$c0") -> $(field calmed "$c1")): a path the target check does not see"
	else
		pass "$label: the wolf never fights the stand-in in $PEACE_WATCH s" "targetRefused $(field targetRefused "$c0") -> $(field targetRefused "$c1")"
	fi

	# made to fight it
	c2=$(pc_head "$(dc "$g" peace)")
	r=$(dc "$g" "console $wid.startcombat $sid")
	if [ "$r" != ran ]; then
		rig_fault input "$label: the console makes the wolf fight the stand-in" "the console refused startcombat: $r"
	else
		sleep 1
		if wait_until "$PEACE_WAIT" pc_off_standin "$g"; then
			c3=$(pc_head "$(dc "$g" peace)")
			if pc_rose "$(field calmed "$c2")" "$(field calmed "$c3")" || pc_rose "$(field targetRefused "$c2")" "$(field targetRefused "$c3")"; then
				pass "$label: a wolf made to fight the stand-in is off it within $PEACE_WAIT s" "targetRefused $(field targetRefused "$c2") -> $(field targetRefused "$c3"), calmed $(field calmed "$c2") -> $(field calmed "$c3")"
			else
				warn "$label: a wolf made to fight the stand-in is off it within $PEACE_WAIT s" "off it, but nothing was turned away or told to stop (targetRefused=$(field targetRefused "$c3") calmed=$(field calmed "$c3")): did startcombat set anything?"
			fi
		else
			fail "$label: a wolf made to fight the stand-in is off it within $PEACE_WAIT s" "$(dc "$g" 'peace targeting' | cut -c1-200); $(pc_head "$(dc "$g" peace)" | grep -o 'calmed=[0-9]*\|givenUp=[0-9]*\|targetRefused=[0-9]*' | tr '\n' ' ')"
		fi
	fi
	expect_zero "$label: no actor of the $o's game fights its stand-in either (targeting=)" "$(field targeting "$(dc "$o" 'peace targeting')")"

	dc "$g" "console $wid.disable" >/dev/null; dc "$g" "console $wid.markfordelete" >/dev/null
	dc "$g" "console player.modav health -$PEACE_HEALTH" >/dev/null
	still "$g"
	say "peace: the $g's wolf $wid disabled and marked for deletion, the player's health taken back (set-up undone)"
}

step_peace() {
	local g
	for g in host vm; do still $g; done
	for g in vm host; do peace_state "$g"; done
	for g in vm host; do peace_fire "$g"; done
	for g in vm host; do peace_creature "$g"; done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init peacetest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (StandInPeace::Command, RemotePlayers::Peace)
	R='peace hooks=target:1,talk:1,watch:1,flee:1 targetAsked=12 targetRefused=9 talkAsked=3 talkRefused=1 watchAsked=0 watchRefused=0 fleeAsked=0 fleeRefused=0 sweeps=40 calmed=1 givenUp=0 targeting=0 standInsFighting=0 book=1 bookGivenUp=0 lastAsker=0001A68A lastAsk=target lastStandIn=FF0008AE lastCalmed=FF000900 lastCalmedFrom=FF0008AE ghostSet=2 noProcess=0 bounty=0 | standin player=2 id=FF0008AE ghost=1 processGhost=1 inCombat=0 combatTarget=00000000 crimeFaction=00000000 hitsOn=0 magicOn=3 blows=0 magicRefused=1 | standin player=12 id=FF0008B0 ghost=0 processGhost=-1 inCombat=1 combatTarget=00000014 crimeFaction=00000000 hitsOn=2 magicOn=0 blows=0 magicRefused=0'
	t "head: the counts only" "$(field hitsOn "$(pc_head "$R")")" ""
	t "head: bounty" "$(field bounty "$(pc_head "$R")")" 0
	t "hooks: all in" "$(pc_hooks_in "$R" && echo yes)" yes
	t "hooks: one out" "$(pc_hooks_in "${R/flee:1/flee:0}" || echo no)" no
	t "standin: that one's part" "$(field magicOn "$(pc_standin "$R" FF0008AE)")" 3
	t "standin: the other's" "$(field hitsOn "$(pc_standin "$R" FF0008B0)")" 2
	t "standin: none not listed" "$(pc_standin "$R" FF0008AF)" ""
	t "found: a field" "$(pc_found 'actor id=FF000900 base=00023ABE dist=120 dead=0 combat=1 target=FF0008AE targetStandIn=1' targetStandIn)" 1
	t "found: none" "$(pc_found 'actor none (no 00023ABE within 3000)' id)" ""
	t "kind: counted" "$(pc_kind 'attack:3,power:1' attack)" 3
	t "kind: none" "$(pc_kind 'cast:1' attack)" 0
	t "kind: not power for attack" "$(pc_kind 'power:4' attack)" 0
	t "rose" "$(pc_rose 1 2 && echo yes)" yes
	t "rose: not equal" "$(pc_rose 2 2 || echo no)" no
	t "rose: decimals" "$(pc_rose 10.0 30.5 && echo yes)" yes
	t "rose: not a number" "$(pc_rose '' 3 || echo no)" no

	# The VM's game faked, as the plugin would answer. FP_MODE: ok, nohooks (a hook out), noghost, fighting (the
	# stand-in in a fight from the start), nostandin (the rig's), far (the walk ends far: the rig's), noswing (no attack
	# seen: the rig's), hit (a blow lands: a WARN), bounty (a bounty: a FAIL), fight (the stand-in fights after: a FAIL),
	# targeted (a guard fights it after: a FAIL); for the wolf: picked (the wolf picks it twice, the sweep stops it: a
	# WARN), stuck (on it to the end, and after startcombat: two FAILs), noplace (the rig's), silent (startcombat sets
	# nothing and nothing is turned away: a WARN), copyfights (the host's game has an actor fighting its stand-in: a FAIL).
	fake_peace() {
		FP=$d/fp.$RANDOM; mkdir -p "$FP"; FP_MODE=$1
		local f; for f in hits magic bounty attack magicka refused calmed placed finds forced fighting; do echo 0 > "$FP/$f"; done
		[ "$FP_MODE" = fighting ] && echo 1 > "$FP/fighting"
		fp_inc() { echo $(( $(cat "$FP/$1") + ${2:-1} )) > "$FP/$1"; }
		dc() { local hooks=1 ghost=1 tgt ts n targeting=0; case "$1 $2" in
			"vm standins") if [ "$FP_MODE" = nostandin ]; then echo "players=1 standins=0 leaving=0 | player 1 standin=00000000 placed=0 at=none"
				else echo "players=1 standins=1 leaving=0 | player 1 standin=FF0008AE placed=1 dressed=1 at=(100,200,0) heading=0.000"; fi ;;
			"vm where") if [ "$FP_MODE" = far ]; then echo "cell=0000A1B2 world=0000003C pos=(100,900,0) modal=0"; else echo "cell=0000A1B2 world=0000003C pos=(100,120,0) modal=0"; fi ;;
			"vm peace")
				[ "$FP_MODE" = nohooks ] && hooks=0; [ "$FP_MODE" = noghost ] && ghost=0
				echo "peace hooks=target:$hooks,talk:1,watch:1,flee:1 targetAsked=20 targetRefused=$(cat "$FP/refused") sweeps=10 calmed=$(cat "$FP/calmed") givenUp=0 targeting=0 standInsFighting=$(cat "$FP/fighting") book=0 ghostSet=$ghost noProcess=0 bounty=$(cat "$FP/bounty") | standin player=1 id=FF0008AE ghost=$ghost processGhost=$ghost inCombat=$(cat "$FP/fighting") combatTarget=00000000 crimeFaction=00000000 hitsOn=$(cat "$FP/hits") magicOn=$(cat "$FP/magic") blows=0 magicRefused=0" ;;
			"vm peace targeting") [ "$FP_MODE" = targeted ] && [ "$(cat "$FP/attack")" -gt 0 ] && targeting=1
				[ "$targeting" = 1 ] && echo "targeting=1 standInsFighting=0 | 0001A68A base=0001A68B standIn=FF0008AE dead=0" || echo "targeting=0 standInsFighting=0" ;;
			"host peace targeting") [ "$FP_MODE" = copyfights ] && echo "targeting=1 standInsFighting=0 | FF000901 base=00023ABE standIn=FF0008B2 dead=0" || echo "targeting=0 standInsFighting=0" ;;
			"vm peace find 00023ABE 3000")
				if [ "$(cat "$FP/placed")" = 0 ]; then echo "actor none (no 00023ABE within 3000)"; return; fi
				fp_inc finds; n=$(cat "$FP/finds"); ts=0
				case "$FP_MODE" in
					picked) { [ "$n" = 3 ] || [ "$n" = 4 ]; } && ts=1; [ "$n" = 5 ] && echo 1 > "$FP/calmed" ;;
					stuck) ts=1 ;;
					*) [ "$(cat "$FP/forced")" = 0 ] && fp_inc refused ;;
				esac
				tgt=00000014; [ "$ts" = 1 ] && tgt=FF0008AE
				echo "actor id=FF000900 base=00023ABE dist=150 dead=0 combat=1 target=$tgt targetStandIn=$ts" ;;
			"vm actions equip right "*) set -- $2; echo "equipped right $4 holds=$4" ;;
			"vm walkto FF0008AE "*) echo "walking to FF0008AE" ;;
			"vm walkto") echo "not walking; last: arrived" ;;
			"vm actions") echo "actions hooked=1 recording=1 heard=1 byKind=none sent=0 dropped=0 waiting=0 notOurs=0 last=none seen=attack:$(cat "$FP/attack") staminaSpent=0.0 magickaSpent=$(cat "$FP/magicka").0 right=00012EB7 left=00000000 ammo=00000000 voice=00000000 unnamed=0 bound=0 hp=100.0/100.0 | magicHooks character=1 player=1" ;;
			"vm pad trigger r 1 150")
				[ "$FP_MODE" = noswing ] && { echo ok; return; }
				fp_inc attack
				case "$FP_MODE" in hit) fp_inc hits ;; bounty) fp_inc bounty 40 ;; fight) echo 1 > "$FP/fighting" ;; esac; echo ok ;;
			"vm pad trigger r 1 2500") fp_inc magicka 20; fp_inc magic; echo ok ;;
			"vm console player.cast 0012FCD0 FF0008AE") fp_inc magic; echo ran ;;
			"vm console player.paycrimegold"*) echo 0 > "$FP/bounty"; echo ran ;;
			"vm console FF0008AE.placeatme 00023ABE 1") [ "$FP_MODE" = noplace ] || echo 1 > "$FP/placed"; echo ran ;;
			"vm console FF000900.startcombat FF0008AE") echo 1 > "$FP/forced"; [ "$FP_MODE" = silent ] || [ "$FP_MODE" = stuck ] || fp_inc refused; echo ran ;;
			"vm console"*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fp$RANDOM; mkdir -p "$A_OUT"; a_init fp >/dev/null
			"${FP_LEG:-peace_fire}" vm >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}'; echo -n "bounty=$(cat "$FP/bounty")" )
	}
	# state: the hooks (1), a ghost (1), in no fight (1)
	t "state: all of it" "$(FP_LEG=peace_state fake_peace ok)" "PASS:3 bounty=0"
	t "state: a hook out fails" "$(FP_LEG=peace_state fake_peace nohooks)" "FAIL:1 PASS:2 bounty=0"
	t "state: no ghost fails" "$(FP_LEG=peace_state fake_peace noghost)" "FAIL:1 PASS:2 bounty=0"
	t "state: a stand-in in a fight fails" "$(FP_LEG=peace_state fake_peace fighting)" "FAIL:1 PASS:2 bounty=0"
	t "state: no stand-in is the rig's (after the hooks)" "$(FP_LEG=peace_state fake_peace nostandin)" "PASS:1 RIG:1 bounty=0"
	# fire: nothing landed (1), no bounty (1), no fight (1), no one fights it (1)
	t "fire: all of it" "$(fake_peace ok)" "PASS:4 bounty=0"
	t "fire: no stand-in is the rig's" "$(fake_peace nostandin)" "RIG:1 bounty=0"
	t "fire: a walk that ends far is the rig's" "$(fake_peace far)" "RIG:1 bounty=0"
	t "fire: no swing seen is the rig's" "$(fake_peace noswing)" "RIG:1 bounty=0"
	t "fire: a blow that lands is a WARN" "$(fake_peace hit)" "PASS:3 WARN:1 bounty=0"
	t "fire: a bounty fails, and is paid back" "$(fake_peace bounty)" "FAIL:1 PASS:3 bounty=0"
	t "fire: a stand-in in a fight after fails" "$(fake_peace fight)" "FAIL:1 PASS:3 bounty=0"
	t "fire: an actor fighting the stand-in after fails" "$(fake_peace targeted)" "FAIL:1 PASS:3 bounty=0"
	# creature: never picks it (1), made to fight it and off it (1), none in the other game (1)
	t "creature: all of it" "$(FP_LEG=peace_creature fake_peace ok)" "PASS:3 bounty=0"
	t "creature: picked and stopped by the sweep is a WARN" "$(FP_LEG=peace_creature fake_peace picked)" "PASS:2 WARN:1 bounty=0"
	t "creature: on it to the end fails, and made to fight it too" "$(FP_LEG=peace_creature fake_peace stuck)" "FAIL:2 PASS:1 bounty=0"
	t "creature: no wolf placed is the rig's" "$(FP_LEG=peace_creature fake_peace noplace)" "RIG:1 bounty=0"
	t "creature: startcombat that sets nothing is a WARN" "$(FP_LEG=peace_creature fake_peace silent)" "PASS:2 WARN:1 bounty=0"
	t "creature: an actor fighting the other game's stand-in fails" "$(FP_LEG=peace_creature fake_peace copyfights)" "FAIL:1 PASS:2 bounty=0"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "peace is a step" "$(case " $STEPS " in *" peace "*) echo yes ;; esac)" yes
	t "peace comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' peace')" 1
	t "peace needs a session" "$(case " $SESSION_STEPS " in *" peace "*) echo yes ;; esac)" yes
	t "peace runs by default" "$(case " $OPTIN_STEPS " in *" peace "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "peace.sh selftest: all ok" || echo "peace.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
