#!/bin/bash
# steps/actions.sh: the rig step `actions` (protocol 9, docs/DESIGN.md "Players"), sourced by steps.sh. A friend's
# stand-in holds what they hold and does what they do: draws and puts away, swings, power attacks, blocks, bashes, draws
# and looses a bow (a conjured one too), casts from each hand, shouts; and it deals nothing in the watcher's game (its
# friend's game deals it all). Not run in a game yet.
#
#   step_actions     the step
#   bash tools/rig/steps/actions.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover's player acts, the watcher's stand-in of it is read), both players in one session near each other
# at the harness's cell, after the npccombat step:
#   1. Set-up of the mover's hands from its console and developer channel (as the console sets a scene up, said in the
#      run): an iron sword, an iron shield, a hunting bow and iron arrows given (`player.additem`), Flames, Healing and
#      Bound Bow taught (`player.addspell`), Unrelenting Force's first word taught and unlocked (`player.addshout`,
#      `teachword`, `unlockword`); then each put in a hand, the quiver or the voice (`actions equip`). The set-up is read
#      back from the mover's own game (its `actions` right=, left=, ammo=, voice=): one that did not take (a console
#      command that did nothing, an equip the game refused) is a rig fault, never blamed on the sync, and ends that way.
#      Then the watcher's stand-in holds the same within ACTIONS_WAIT (its `right=`, `left=`, `ammo=`: the id and 1
#      when it holds it; `voice=`).
#   2. Each action from the mover's virtual pad (the game's own input, never a graph event sent by a script): the
#      weapon drawn (X), a swing (a tap of the right trigger), a power attack (the right trigger held), a block (the
#      left trigger held, the shield up), a bash (the right trigger tapped while blocking), the weapon put away (X);
#      the bow drawn and loosed (the right trigger held, then let go); Bound Bow cast (a conjured bow and its arrows,
#      both non-playable: the stand-in holds them all the same, nothing refused) and loosed; Flames from the right hand
#      and Healing from the left (each trigger held); the shout (RB). For each:
#        - the mover's own graph hook heard that kind of action (its `actions` heard byKind=). When it heard none, the
#          mover's game is asked whether the press did anything (its `actions` seen= by kind: the attack, block, cast
#          and shout states the game shows; staminaSpent= for a power attack or a bash, magickaSpent= for a cast): it
#          did, and the hook heard none, is a FAIL (the events go some way the hook does not see); it did nothing is the
#          press's (a rig fault).
#        - the watcher's stand-in played that kind (its byKind=) within ACTIONS_WAIT: FAIL if not.
#      The weapon drawn and put away is read on the stand-in itself (drawn=; a FAIL says its weaponState=, process= and
#      responses=), and a draw the stand-in had to be told again (drawFixes= up: the first did not hold) is a WARN.
#   3. It deals nothing here: the mover walks up to the watcher's player with its virtual pad (`walkto` the stand-in of
#      it, ACTIONS_REACH u; a walk that does not get there facing it is a rig fault), casts Flames at it (magicka spent,
#      or a rig fault), then looses an arrow at it, then shouts at it. In the watcher's game, its stand-in cast
#      (casts= up), its Flames' effects on the watcher's player were refused (magicRefused= up), its arrow hit
#      (blows= up), and the watcher's player kept its health (hp=, within 1): each a FAIL if not; after the shout, its
#      effects refused is a WARN when not seen.
#   4. After both: neither stand-in took any health here that was not given back (damaged=0 or dealt=undone), the
#      shouts its stand-in's base keeps are as they were when placed (baseShouts=now/placed: a shout is readied, never
#      taught to a load-order preset a save would keep), the events its graph turned away (refused=, a WARN when any),
#      and both games' magic target hooks in (magicHooks: FAIL when not).
#   Put back: the weapon put away. What the console gave stays in the mover's inventory (the host never saves; the
#   VM's test save is its own).

ACTIONS_WAIT=${ACTIONS_WAIT:-5}       # s: for the stand-in to hold or play what its friend does
ACTIONS_SETTLE=${ACTIONS_SETTLE:-2}   # s: after a press, for the mover's graph to have taken it
ACTIONS_REACH=${ACTIONS_REACH:-150}   # u: how near the mover walks to the watcher's player before it casts and shoots
ACTIONS_FACE=${ACTIONS_FACE:-0.5}     # rad: how far off facing the watcher's player the mover may end its walk
ACTIONS_SWORD=${ACTIONS_SWORD:-00012EB7}    # Skyrim.esm's Iron Sword
ACTIONS_SHIELD=${ACTIONS_SHIELD:-00012EB6}  # Iron Shield
ACTIONS_BOW=${ACTIONS_BOW:-00013985}        # Hunting Bow
ACTIONS_ARROW=${ACTIONS_ARROW:-0001397D}    # Iron Arrow
ACTIONS_FLAMES=${ACTIONS_FLAMES:-00012FCD}  # Flames (a concentration spell)
ACTIONS_HEAL=${ACTIONS_HEAL:-00012FCC}      # Healing (on the caster itself)
ACTIONS_BOUND=${ACTIONS_BOUND:-000211ED}    # Bound Bow (the spell: a conjured bow and its arrows, both non-playable)
ACTIONS_SHOUT=${ACTIONS_SHOUT:-00013E07}    # Unrelenting Force
ACTIONS_WORD=${ACTIONS_WORD:-00013E22}      # its first word

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# act_own <actions reply>: our player's part (before the first " | ")
act_own() { echo "${1%% | *}"; }
# act_friend <actions reply>: the friend's stand-in's part (the last "| player " section: one friend on the rig)
act_friend() { [[ $1 == *"| player "* ]] && echo "player ${1##*| player }"; }
# act_kind <byKind value> <kind>: how many of that kind ("attack:3,power:1" -> attack 3); 0 when none
act_kind() { local n; n=$(tr ',' '\n' <<<"$1" | sed -n "s/^$2://p" | head -1); echo "${n:-0}"; }
# act_heard <who> <kind>: how many of that kind that game's player's graph heard
act_heard() { act_kind "$(field byKind "$(act_own "$(dc "$1" actions)")")" "$2"; }
# act_played <who> <kind>: how many of that kind that game's stand-in of its friend played
act_played() { act_kind "$(field byKind "$(act_friend "$(dc "$1" actions)")")" "$2"; }
# act_field <who> <name>: a field of that game's stand-in's part
act_field() { field "$2" "$(act_friend "$(dc "$1" actions)")"; }
# act_own_field <who> <name>: a field of that game's own part (our player)
act_own_field() { field "$2" "$(act_own "$(dc "$1" actions)")"; }
# act_own_holds <who> <hand|ammo|voice> <id>: that game's own player holds the id there, as its game reads it back
act_own_holds() { [ "$(act_own_field "$1" "$2")" = "$3" ]; }
# act_bound <who>: that game's own player has a conjured weapon in a hand
act_bound() { [ "$(act_own_field "$1" bound)" = 1 ]; }
# act_holds <who> <hand> <id>: that game's stand-in holds the id in that hand (right=<id>:1)
act_holds() { [ "$(act_field "$1" "$2")" = "$3:1" ]; }
# act_voice <who> <id>: its friend's voice is that id, and the stand-in's too (voice=<id>/<id>)
act_voice() { [ "$(act_field "$1" voice)" = "$2/$2" ]; }
# act_drawn <who> <0|1>: that game's stand-in has its weapon out (1) or put away (0)
act_drawn() { [ "$(act_field "$1" drawn)" = "$2" ]; }
# act_up <who> <kind> <before>: that game's stand-in played more of that kind than before
act_up() { local n; n=$(act_played "$1" "$2"); is_num "$n" && [ "$n" -gt "$3" ]; }
# act_own_drawn <who>: that game's player has its weapon out (where's flags, kWeaponDrawn 0x04)
act_own_drawn() { local f; f=$(field flags "$(dc "$1" where)"); [[ $f =~ ^0x[0-9a-fA-F]+$ ]] && [ $(( f & 4 )) -ne 0 ]; }
# act_more <before> <after>: after is a number above before (either may have decimals)
act_more() { is_num "$1" && is_num "$2" && _cmp "$2" ">" "$1"; }

# act_proof <own part before> <own part now> <kind>: what the mover's game shows of that kind of action between the two
# readings (printed; false when nothing): the state seen begun, or the stamina (a power attack, a bash) or the magicka
# (a cast) spent
act_proof() {
	local a=$1 b=$2 kind=$3 s0 s1
	s0=$(act_kind "$(field seen "$a")" "$kind"); s1=$(act_kind "$(field seen "$b")" "$kind")
	if is_num "$s0" && is_num "$s1" && [ "$s1" -gt "$s0" ]; then echo "seen $kind $s0 -> $s1"; return 0; fi
	case $kind in
		power|bash) act_more "$(field staminaSpent "$a")" "$(field staminaSpent "$b")" && { echo "stamina spent $(field staminaSpent "$a") -> $(field staminaSpent "$b")"; return 0; } ;;
		cast) act_more "$(field magickaSpent "$a")" "$(field magickaSpent "$b")" && { echo "magicka spent $(field magickaSpent "$a") -> $(field magickaSpent "$b")"; return 0; } ;;
	esac
	return 1
}

# act_setup <mover> <watcher>: the mover given and taught what the step uses (console set-up), each said
act_setup() {
	local m=$1 id r
	for id in "$ACTIONS_SWORD 1" "$ACTIONS_SHIELD 1" "$ACTIONS_BOW 1" "$ACTIONS_ARROW 60"; do
		r=$(dc "$m" "console player.additem $id"); [ "$r" = ran ] || { rig_fault state "$m: set-up" "the console refused player.additem $id: $r"; return 1; }
	done
	for id in "$ACTIONS_FLAMES" "$ACTIONS_HEAL" "$ACTIONS_BOUND"; do dc "$m" "console player.addspell $id" >/dev/null; done
	dc "$m" "console player.addshout $ACTIONS_SHOUT" >/dev/null
	dc "$m" "console player.teachword $ACTIONS_WORD" >/dev/null
	dc "$m" "console player.unlockword $ACTIONS_WORD" >/dev/null
	say "actions: $m given a sword, a shield, a bow, arrows, Flames, Healing, Bound Bow and Unrelenting Force from its console (set-up; each read back as it is equipped)"
}

# act_shown <watcher> <hand|ammo|voice> <id> <label>: the watcher's stand-in holds the id there within ACTIONS_WAIT
act_shown() {
	local w=$1 hand=$2 id=$3 label=$4
	if [ "$hand" = voice ]; then
		wait_until "$ACTIONS_WAIT" act_voice "$w" "$id" && pass "$label: the $w's stand-in has it ready" "voice=$(act_field "$w" voice)" ||
			fail "$label: the $w's stand-in has it ready" "voice=$(act_field "$w" voice) after $ACTIONS_WAIT s (want $id/$id)"
	else
		wait_until "$ACTIONS_WAIT" act_holds "$w" "$hand" "$id" && pass "$label: the $w's stand-in holds it" "$hand=$(act_field "$w" "$hand")" ||
			fail "$label: the $w's stand-in holds it" "$hand=$(act_field "$w" "$hand") after $ACTIONS_WAIT s (want $id:1; refusedForms=$(act_field "$w" refusedForms))"
	fi
}

# act_equip <mover> <watcher> <hand> <id> <label>: the mover's hand given the id (set-up), read back from the mover's own
# game (a set-up that did not take is the rig's: false), and the watcher's stand-in holds it
act_equip() {
	local m=$1 w=$2 hand=$3 id=$4 label=$5 r
	r=$(dc "$m" "actions equip $hand $id")
	[[ $r == equipped* || $r == asked* ]] || { rig_fault state "$label: the $m's $hand" "actions equip refused: $r"; return 1; }
	if ! wait_until "$ACTIONS_WAIT" act_own_holds "$m" "$hand" "$id"; then
		rig_fault state "$label: the $m's $hand" "the $m's own $hand holds $(act_own_field "$m" "$hand") after $ACTIONS_WAIT s, not $id: the set-up did not take ($r)"
		return 1
	fi
	act_shown "$w" "$hand" "$id" "$label"
}

# act_press <mover> <press...>: each pad command on the mover's game; "wait:<s>" waits between
act_press() {
	local m=$1 p; shift
	for p in "$@"; do
		if [[ $p == wait:* ]]; then sleep "${p#wait:}"; else dc "$m" "pad $p" >/dev/null; fi
	done
}

# act_try <mover> <watcher> <label> <kind> <press...>: the presses make the mover's graph take that kind of action, and
# the watcher's stand-in plays it
act_try() {
	local m=$1 w=$2 label=$3 kind=$4 own0 own1 h0 p0 h1 proof; shift 4
	own0=$(act_own "$(dc "$m" actions)"); h0=$(act_kind "$(field byKind "$own0")" "$kind"); p0=$(act_played "$w" "$kind")
	act_press "$m" "$@"
	sleep "$ACTIONS_SETTLE"
	own1=$(act_own "$(dc "$m" actions)"); h1=$(act_kind "$(field byKind "$own1")" "$kind")
	if ! is_num "$h0" || ! is_num "$h1" || [ "$h1" -le "$h0" ]; then
		if proof=$(act_proof "$own0" "$own1" "$kind"); then
			fail "$label: the $m's graph hook hears it" "the $m's game did it ($proof), but its graph hook heard no $kind ($h0 -> $h1): the game sends those events by some way the hook does not see"
		else
			rig_fault input "$label: the $m's press" "the $m's game shows no $kind and its graph heard none ($kind $h0 -> $h1; seen=$(field seen "$own1") staminaSpent=$(field staminaSpent "$own1") magickaSpent=$(field magickaSpent "$own1"))"
		fi
		return 1
	fi
	wait_until "$ACTIONS_WAIT" act_up "$w" "$kind" "$p0" && pass "$label: the $w's stand-in plays it" "$kind played $p0 -> $(act_played "$w" "$kind")" ||
		fail "$label: the $w's stand-in plays it" "the $m sent $((h1 - h0)) $kind event(s); the $w's stand-in played none in $ACTIONS_WAIT s ($(act_friend "$(dc "$w" actions)" | cut -c1-200))"
}

# act_draw <mover> <watcher> <label> <0|1>: X draws (1) or puts away (0) the mover's weapon; the stand-in follows
act_draw() {
	local m=$1 w=$2 label=$3 want=$4 f0
	f0=$(act_field "$w" drawFixes)
	act_press "$m" "button x 100"
	sleep "$ACTIONS_SETTLE"
	if [ "$want" = 1 ] && ! act_own_drawn "$m"; then rig_fault input "$label: the $m's press" "X left the $m's weapon away ($(dc "$m" where | grep -o 'flags=[^ ]*'))"; return 1; fi
	if [ "$want" = 0 ] && act_own_drawn "$m"; then rig_fault input "$label: the $m's press" "X left the $m's weapon out"; return 1; fi
	wait_until "$ACTIONS_WAIT" act_drawn "$w" "$want" && pass "$label: the $w's stand-in follows" "drawn=$want" ||
		fail "$label: the $w's stand-in follows" "drawn=$(act_field "$w" drawn) wanted=$(act_field "$w" wanted) weaponState=$(act_field "$w" weaponState) drawAsks=$(act_field "$w" drawAsks) drawsFinished=$(act_field "$w" drawsFinished) process=$(act_field "$w" process) responses=$(act_field "$w" responses) after $ACTIONS_WAIT s"
	local f1; f1=$(act_field "$w" drawFixes)
	is_num "$f0" && is_num "$f1" && [ "$f1" -gt "$f0" ] && warn "$label: the $w's stand-in's draw held the second time" "told again (drawFixes $f0 -> $f1): the first time did not hold"
	return 0
}

# act_bound_bow <mover> <watcher>: Bound Bow cast with the pad; the conjured bow and its arrows (non-playable, both) are
# held by the stand-in, nothing refused, and it looses one
act_bound_bow() {
	local m=$1 w=$2 rf0 bow arrow
	act_equip "$m" "$w" right "$ACTIONS_BOUND" "$m readies Bound Bow" || return 1
	act_draw "$m" "$w" "$m raises its hands" 1 || return 1
	rf0=$(act_field "$w" refusedForms)
	act_press "$m" "trigger r 1 2000"
	if ! wait_until "$ACTIONS_WAIT" act_bound "$m"; then
		rig_fault input "$m casts Bound Bow: the press" "no conjured weapon in the $m's hands ($(act_own "$(dc "$m" actions)" | grep -o 'right=.*'))"
		return 1
	fi
	bow=$(act_own_field "$m" right); arrow=$(act_own_field "$m" ammo)
	say "actions: the $m conjured bow $bow with arrows $arrow"
	act_shown "$w" right "$bow" "$m holds a conjured bow"
	if [ -n "$arrow" ] && [ "$arrow" != 00000000 ]; then act_shown "$w" ammo "$arrow" "$m has conjured arrows"; fi
	local rf1; rf1=$(act_field "$w" refusedForms)
	if is_num "$rf0" && is_num "$rf1" && [ "$rf1" -gt "$rf0" ]; then fail "$m's conjured bow: nothing of it refused by the $w" "refusedForms $rf0 -> $rf1"
	else pass "$m's conjured bow: nothing of it refused by the $w" "refusedForms $rf0 -> $rf1"; fi
	act_try "$m" "$w" "$m draws and looses a conjured arrow" bow "trigger r 1 1500"
	act_draw "$m" "$w" "$m puts the conjured bow away" 0
}

# ---- it deals nothing here ----

# act_face_off <who>: how far (rad) that game's player faces from its stand-in of its friend (heading 0 is +y, clockwise)
act_face_off() {
	local p s h; p=$(pos_of "$1"); s=$(standin_at "$1"); h=$(field heading "$(dc "$1" where)")
	[ -n "$p" ] && [ -n "$s" ] && is_num "$h" || return 0
	angle_diff "$(awk -v a="$p" -v b="$s" 'BEGIN{split(a, p, " "); split(b, q, " "); printf "%.4f\n", atan2(q[1] - p[1], q[2] - p[2])}')" "$h"
}

# act_walk_near <mover> <watcher>: the mover walks with its virtual pad up to its stand-in of the watcher's player and
# ends facing it; a walk that does not get there is a rig fault (false)
act_walk_near() {
	local m=$1 w=$2 label="$1 walks up to the $2's player" sid r k d off
	sid=$(field standin "$(dc "$m" standins)")
	if ! [[ $sid =~ ^[0-9A-F]{8}$ ]] || [ "$sid" = 00000000 ]; then rig_fault state "$label" "the $m has no stand-in of the $w's player to walk to"; return 1; fi
	r=$(dc "$m" "walkto $sid 0.5 $ACTIONS_REACH 20")
	[[ $r == walking* ]] || { rig_fault input "$label" "the $m's walkto refused: $r"; return 1; }
	for ((k = 0; k < 25; k++)); do [[ $(dc "$m" walkto) == "not walking"* ]] && break; sleep 1; done
	still "$m"; sleep 1
	d=$(dist2 "$(pos_of "$m")" "$(standin_at "$m")"); off=$(act_face_off "$m")
	if ! is_num "$d" || [ "$d" -gt $((ACTIONS_REACH * 2)) ] || ! is_num "$off" || _cmp "$off" ">" "$ACTIONS_FACE"; then
		rig_fault walk "$label" "the $m ended ${d:-?} u from the $w's player, ${off:-?} rad off facing it"
		return 1
	fi
	pass "$label" "the $m is $d u from the $w's player, $off rad off facing it"
}

# act_harm_read <watcher>: "hp magicRefused casts blows damaged undone": the watcher's player's health and what its
# stand-in of the mover did there
act_harm_read() {
	local s f h; s=$(dc "$1" actions); f=$(act_friend "$s"); h=$(field hp "$(act_own "$s")")
	echo "${h%%/*} $(field magicRefused "$f") $(field casts "$f") $(field blows "$f") $(field damaged "$f") $(field undone "$f")"
}

# act_up_by <label> <before> <after> [fail|warn]: after is above before
act_up_by() {
	if is_num "$2" && is_num "$3" && [ "$3" -gt "$2" ]; then pass "$1" "$2 -> $3"; elif [ "${4:-fail}" = warn ]; then warn "$1" "${2:-?} -> ${3:-?}"; else fail "$1" "${2:-?} -> ${3:-?}"; fi
}

# act_unharmed <label> <hp before> <hp after> <damaged before> <damaged after> <undone before> <undone after>: the
# watcher's player kept its health (within 1)
act_unharmed() {
	local lo; lo=$(awk -v a="$2" 'BEGIN{print a - 1}')
	if is_num "$2" && is_num "$3" && ! _cmp "$3" "<" "$lo"; then pass "$1" "hp $2 -> $3"
	else fail "$1" "hp ${2:-?} -> ${3:-?} (its blows' health taken here $4 -> $5 times, given back $6 -> $7)"; fi
}

# act_harm <mover> <watcher>: the mover casts Flames at the watcher's player and looses an arrow at it; the watcher's
# stand-in of it casts and hits, all of it refused or given back, and the watcher's player keeps its health
act_harm() {
	local m=$1 w=$2 hp0 mr0 c0 b0 d0 u0 hp mr c b d u g0 g1
	act_walk_near "$m" "$w" || return 1
	act_equip "$m" "$w" right "$ACTIONS_FLAMES" "$m readies Flames for the $w's player" || return 1
	act_draw "$m" "$w" "$m raises its hands at the $w's player" 1 || return 1
	read -r hp0 mr0 c0 b0 d0 u0 <<<"$(act_harm_read "$w")"
	g0=$(act_own_field "$m" magickaSpent)
	act_press "$m" "trigger r 1 2500"
	sleep "$ACTIONS_SETTLE"
	g1=$(act_own_field "$m" magickaSpent)
	if ! act_more "$g0" "$g1"; then rig_fault input "$m casts Flames at the $w's player: the press" "the $m spent no magicka ($g0 -> $g1)"; return 1; fi
	read -r hp mr c b d u <<<"$(act_harm_read "$w")"
	act_up_by "$m's Flames at the $w's player: the $w's stand-in casts them (casts=)" "$c0" "$c"
	act_up_by "$m's Flames at the $w's player: their effects on the $w's player are refused (magicRefused=)" "$mr0" "$mr"
	act_unharmed "$m's Flames at the $w's player: the $w's player keeps its health" "$hp0" "$hp" "$d0" "$d" "$u0" "$u"
	act_draw "$m" "$w" "$m lowers its hands" 0
	act_equip "$m" "$w" right "$ACTIONS_BOW" "$m holds a bow for the $w's player" || return 1
	act_equip "$m" "$w" ammo "$ACTIONS_ARROW" "$m has arrows for the $w's player" || return 1
	act_draw "$m" "$w" "$m draws the bow at the $w's player" 1 || return 1
	read -r hp0 mr0 c0 b0 d0 u0 <<<"$(act_harm_read "$w")"
	act_try "$m" "$w" "$m looses an arrow at the $w's player" bow "trigger r 1 1500" || return 1
	sleep 1  # the arrow's flight
	read -r hp mr c b d u <<<"$(act_harm_read "$w")"
	act_up_by "$m's arrow at the $w's player: the $w's stand-in's arrow hits it (blows=)" "$b0" "$b"
	act_unharmed "$m's arrow at the $w's player: the $w's player keeps its health" "$hp0" "$hp" "$d0" "$d" "$u0" "$u"
	act_draw "$m" "$w" "$m puts the bow away" 0
}

actions_way() {
	local m=$1 w=$2 own hp0 mr0 d0 u0 hp mr d u
	section "actions: the $m's player acts, the $w's stand-in of it shows it"
	still "$m"
	own=$(act_own "$(dc "$m" actions)")
	if [ "$(field hooked "$own")" != 1 ]; then fail "$m: its player's graph events are heard" "$own"; return; fi
	pass "$m: its player's graph events are heard" "$(cut -c1-120 <<<"$own")"
	act_setup "$m" "$w" || return
	# melee: sword and shield
	act_equip "$m" "$w" right "$ACTIONS_SWORD" "$m holds a sword" || return
	act_equip "$m" "$w" left "$ACTIONS_SHIELD" "$m holds a shield" || return
	act_draw "$m" "$w" "$m draws" 1
	act_try "$m" "$w" "$m swings" attack "trigger r 1 150"
	act_try "$m" "$w" "$m power attacks" power "trigger r 1 1200"
	act_try "$m" "$w" "$m blocks" block "trigger l 1 1500"
	act_try "$m" "$w" "$m bashes" bash "trigger l 1 2000" wait:0.6 "trigger r 1 150"
	act_draw "$m" "$w" "$m puts it away" 0
	# the bow
	act_equip "$m" "$w" right "$ACTIONS_BOW" "$m holds a bow" || return
	act_equip "$m" "$w" ammo "$ACTIONS_ARROW" "$m has arrows" || return
	act_draw "$m" "$w" "$m draws the bow" 1
	act_try "$m" "$w" "$m draws and looses an arrow" bow "trigger r 1 1500"
	say "actions: the $w's stand-in aims at $(act_field "$w" aim) (its angle x $(act_field "$w" angleX)); its blows $(act_field "$w" blows)"
	act_draw "$m" "$w" "$m puts the bow away" 0
	# a conjured bow
	act_bound_bow "$m" "$w"
	# magic, each hand
	act_equip "$m" "$w" right "$ACTIONS_FLAMES" "$m readies Flames in the right hand" || return
	act_equip "$m" "$w" left "$ACTIONS_HEAL" "$m readies Healing in the left hand" || return
	act_draw "$m" "$w" "$m raises its hands" 1
	act_try "$m" "$w" "$m casts Flames (right, held)" cast "trigger r 1 1500"
	act_try "$m" "$w" "$m casts Healing (left, held)" cast "trigger l 1 1500"
	act_draw "$m" "$w" "$m lowers its hands" 0
	# at the watcher's player: it deals nothing there
	act_harm "$m" "$w"
	# the voice, shouted at the watcher's player
	act_equip "$m" "$w" voice "$ACTIONS_SHOUT" "$m readies a shout" || return
	read -r hp0 mr0 _ _ d0 u0 <<<"$(act_harm_read "$w")"
	act_try "$m" "$w" "$m shouts" shout "button rb 300"
	sleep 2  # the shout's cooldown and its push, before the next way
	read -r hp mr _ _ d u <<<"$(act_harm_read "$w")"
	act_unharmed "$m's shout: the $w's player keeps its health" "$hp0" "$hp" "$d0" "$d" "$u0" "$u"
	act_up_by "$m's shout: its effects on the $w's player are refused (magicRefused=; a WARN: the shout may have missed)" "$mr0" "$mr" warn
	act_own_drawn "$m" && act_press "$m" "button x 100"
}

step_actions() {
	local w s
	for w in host vm; do still $w; done
	actions_way vm host
	actions_way host vm
	section "actions: what the stand-ins did here, both games"
	for w in host vm; do
		s=$(dc $w actions)
		expect_eq "$w: the magic target hooks are in (a stand-in's spells take hold nowhere)" "$(field character "$s") $(field player "${s#*magicHooks }")" "1 1"
		if [ "$(act_field $w damaged)" = 0 ] || [ "$(act_field $w dealt)" = "$(act_field $w undone)" ]; then
			pass "$w: health its stand-in's blows took here was all given back (its friend's game deals them)" "damaged=$(act_field $w damaged) dealt=$(act_field $w dealt) undone=$(act_field $w undone)"
		else
			fail "$w: health its stand-in's blows took here was all given back (its friend's game deals them)" "damaged=$(act_field $w damaged) dealt=$(act_field $w dealt) undone=$(act_field $w undone)"
		fi
		local bs; bs=$(act_field $w baseShouts)
		if [[ $bs =~ ^([0-9]+)/([0-9]+)$ ]] && [ "${BASH_REMATCH[1]}" = "${BASH_REMATCH[2]}" ]; then
			pass "$w: its stand-in's base keeps the shouts it had (nothing taught to a preset a save would keep)" "baseShouts=$bs"
		else
			fail "$w: its stand-in's base keeps the shouts it had (nothing taught to a preset a save would keep)" "baseShouts=${bs:-?} (now/placed)"
		fi
		warn_gt "$w: events its stand-in's graph turned away" "$(act_field $w refused)" 0
		say "actions: $w: heard $(field heard "$(act_own "$s")") ($(field byKind "$(act_own "$s")")), seen $(field seen "$(act_own "$s")"), sent $(field sent "$(act_own "$s")"), dropped $(field dropped "$(act_own "$s")"); its stand-in played $(act_field $w played) ($(act_field $w byKind)), refused $(act_field $w refusedKinds); spells' effects refused $(act_field $w magicRefused), kept on itself $(act_field $w magicKept); blows $(act_field $w blows), casts $(act_field $w casts), health given back $(act_field $w undone)"
	done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init actionstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (PlayerActions::Describe | RemotePlayers::Actions)
	A='actions hooked=1 recording=1 heard=7 byKind=attack:3,power:1,draw:3 sent=7 dropped=0 waiting=0 notOurs=0 last=attackStop seen=attack:3,power:1,draw:3 staminaSpent=40.0 magickaSpent=0.0 right=00012EB7 left=00012EB6 ammo=00000000 voice=00013E07 unnamed=0 bound=0 hp=120.0/120.0 | magicHooks character=1 player=1 | player 2 standin=FF0008AE in=9 unplaced=0 waiting=0 lost=0 played=8 byKind=attack:2,block:1,draw:5 refused=1 refusedKinds=power:1 lastRefused=attackPowerStartForward drawn=1 wanted=1 drawFixes=0 aim=-0.20 angleX=-0.20 right=00012EB7:1 left=00012EB6:0 ammo=00000000:1 voice=00013E07/00013E07 gearOk=0 missing=0 refusedForms=0 redressed=1 baseShouts=3/3 ammoGiven=60 magicRefused=3 magicKept=1 blows=2 casts=1 damaged=0 dealt=0.0 undone=0.0'
	t "own: our part only" "$(field byKind "$(act_own "$A")")" "attack:3,power:1,draw:3"
	t "own: what our player holds, not the stand-in" "$(field left "$(act_own "$A")")" "00012EB6"
	t "own: health" "$(field hp "$(act_own "$A")")" "120.0/120.0"
	t "friend: the stand-in's part only" "$(field byKind "$(act_friend "$A")")" "attack:2,block:1,draw:5"
	t "friend: none without a friend" "$(act_friend 'actions hooked=1 byKind=none | magicHooks character=0 player=1')" ""
	t "kind: a count" "$(act_kind attack:3,power:1 power)" 1
	t "kind: none of it" "$(act_kind attack:3,power:1 bash)" 0
	t "kind: none at all" "$(act_kind none attack)" 0
	t "kind: attack is not attackpower" "$(act_kind power:4,attack:2 attack)" 2
	t "holds: right" "$(dc() { echo "$A"; }; act_holds host right 00012EB7 && echo yes)" yes
	t "holds: left not on" "$(dc() { echo "$A"; }; act_holds host left 00012EB6 || echo no)" no
	t "own holds: left" "$(dc() { echo "$A"; }; act_own_holds host left 00012EB6 && echo yes)" yes
	t "voice: both" "$(dc() { echo "$A"; }; act_voice host 00013E07 && echo yes)" yes
	t "drawn" "$(dc() { echo "$A"; }; act_drawn host 1 && echo yes)" yes
	t "own drawn: flags 0x04" "$(dc() { echo 'cell=0 world=3C pos=(1,2,3) flags=0x05 modal=0'; }; act_own_drawn host && echo yes)" yes
	t "own drawn: flags 0x01 is not" "$(dc() { echo 'cell=0 world=3C pos=(1,2,3) flags=0x01 modal=0'; }; act_own_drawn host || echo no)" no
	t "proof: a kind seen begun" "$(act_proof 'seen=attack:1' 'seen=attack:2' attack)" "seen attack 1 -> 2"
	t "proof: a power attack's stamina" "$(act_proof 'seen=none staminaSpent=10.0' 'seen=none staminaSpent=35.5' power)" "stamina spent 10.0 -> 35.5"
	t "proof: a cast's magicka" "$(act_proof 'seen=none magickaSpent=0.0' 'seen=none magickaSpent=12.0' cast)" "magicka spent 0.0 -> 12.0"
	t "proof: stamina is not a cast's" "$(act_proof 'seen=none staminaSpent=0.0' 'seen=none staminaSpent=9.0' cast || echo none)" none
	t "proof: nothing" "$(act_proof 'seen=attack:1 staminaSpent=1.0' 'seen=attack:1 staminaSpent=1.0' attack || echo none)" none
	t "face: facing its stand-in" "$(pos_of() { echo '0 0 0'; }; standin_at() { echo '0 100 0'; }; dc() { echo 'heading=0.000'; }; act_face_off host)" "0.000"
	t "face: its stand-in to the east, facing north" "$(pos_of() { echo '0 0 0'; }; standin_at() { echo '100 0 0'; }; dc() { echo 'heading=0.000'; }; act_face_off host)" "1.571"
	t "harm read: the watcher's health and its stand-in's deeds" "$(dc() { echo "$A"; }; act_harm_read host)" "120.0 3 1 2 0 0.0"

	# the step's way, both games faked: the mover's pad presses make its game do that kind (seen=, stamina and magicka
	# spent), its graph hear it, and the watcher's stand-in play it, as the plugin does. FA_MODE: ok, nohook (the mover's
	# hook is not in), deaf (the presses do nothing: the rig's input), unheard (the game does each action but the hook
	# hears none: the mod's), mute (heard and sent, the stand-in plays none), nohold (the stand-in never holds what its
	# friend does), noequip (the mover's own hands never take the set-up: the rig's), handdraw (each draw the stand-in
	# makes by hand), burn (the stand-in's Flames and arrow hurt the watcher's player, nothing refused), unbound (the
	# watcher refuses the conjured bow and its arrows), farwalk (the walk to the watcher never gets there).
	fake_actions() {
		FA=$d/fa.$RANDOM; mkdir -p "$FA"; FA_MODE=$1
		for k in attack power block bash bow cast shout draw; do echo 0 > "$FA/h_$k"; echo 0 > "$FA/s_$k"; echo 0 > "$FA/p_$k"; done
		for k in drawn fixes stamina magicka bound walked refused casts magicRefused blows damaged; do echo 0 > "$FA/$k"; done
		echo 120 > "$FA/hp"
		for k in right left ammo voice; do echo 00000000 > "$FA/$k"; echo 00000000 > "$FA/o_$k"; done
		fa_by() { local k out=""; for k in attack power block bash bow cast shout draw; do [ "$(cat "$FA/$1_$k")" -gt 0 ] && out+="${out:+,}$k:$(cat "$FA/$1_$k")"; done; echo "${out:-none}"; }
		fa_inc() { echo $(( $(cat "$FA/$1") + ${2:-1} )) > "$FA/$1"; }
		fa_add() {  # the game does it (seen), the hook hears it, the stand-in plays it
			fa_inc "s_$1"
			[ "$FA_MODE" = unheard ] && return
			fa_inc "h_$1"; [ "$FA_MODE" = mute ] || fa_inc "p_$1"
		}
		fa_flames_at() { [ "$(cat "$FA/walked")" = 1 ] || return 0; fa_inc casts; if [ "$FA_MODE" = burn ]; then fa_inc hp -10; else fa_inc magicRefused; fi; }
		fa_arrow_at() { [ "$(cat "$FA/walked")" = 1 ] || return 0; fa_inc blows; [ "$FA_MODE" = burn ] && { fa_inc hp -5; fa_inc damaged; }; return 0; }
		dc() { local v; case "$2" in
			actions) if [ "$1" = vm ]; then
					echo "actions hooked=$([ "$FA_MODE" = nohook ] && echo 0 || echo 1) recording=1 heard=1 byKind=$(fa_by h) sent=1 dropped=0 last=none seen=$(fa_by s) staminaSpent=$(cat "$FA/stamina").0 magickaSpent=$(cat "$FA/magicka").0 right=$(cat "$FA/o_right") left=$(cat "$FA/o_left") ammo=$(cat "$FA/o_ammo") voice=$(cat "$FA/o_voice") unnamed=0 bound=$(cat "$FA/bound") hp=100.0/100.0 | magicHooks character=1 player=1 | player 1 standin=FF000801 played=0 byKind=none refused=0 drawn=0 drawFixes=0 damaged=0 baseShouts=2/2"
				else
					v=$(cat "$FA/voice")
					echo "actions hooked=1 recording=1 heard=0 byKind=none seen=none staminaSpent=0.0 magickaSpent=0.0 right=00000000 left=00000000 ammo=00000000 voice=00000000 bound=0 hp=$(cat "$FA/hp").0/120.0 | magicHooks character=1 player=1 | player 2 standin=FF0008AE played=1 byKind=$(fa_by p) refused=0 refusedKinds=none drawn=$(cat "$FA/drawn") wanted=$(cat "$FA/drawn") drawFixes=$(cat "$FA/fixes") aim=0.00 angleX=0.00 right=$(cat "$FA/right"):1 left=$(cat "$FA/left"):1 ammo=$(cat "$FA/ammo"):1 voice=$v/$v refusedForms=$(cat "$FA/refused") baseShouts=3/3 magicRefused=$(cat "$FA/magicRefused") magicKept=0 blows=$(cat "$FA/blows") casts=$(cat "$FA/casts") damaged=$(cat "$FA/damaged") dealt=0.0 undone=0.0"
				fi ;;
			'actions equip '*) set -- $2
				[ "$FA_MODE" = noequip ] && { echo "asked $3 $4 holds=00000000"; return; }
				echo "$4" > "$FA/o_$3"; [ "$FA_MODE" = nohold ] || echo "$4" > "$FA/$3"; echo "equipped $3 $4 holds=$4" ;;
			'pad button x 100') [ "$FA_MODE" = deaf ] && return; v=$(( 1 - $(cat "$FA/drawn") )); echo "$v" > "$FA/drawn"; fa_add draw
				[ "$v" = 0 ] && [ "$(cat "$FA/bound")" = 1 ] && { echo 0 > "$FA/bound"; echo 00000000 > "$FA/o_right"; echo 00000000 > "$FA/right"; }
				[ "$FA_MODE" = handdraw ] && fa_inc fixes; echo ok ;;
			'pad trigger r 1 150') [ "$FA_MODE" = deaf ] && return
				if [ -f "$FA/blocking" ]; then fa_add bash; fa_inc stamina 20; rm -f "$FA/blocking"; else fa_add attack; fi; echo ok ;;
			'pad trigger r 1 1200') [ "$FA_MODE" = deaf ] || { fa_add power; fa_inc stamina 25; }; echo ok ;;
			'pad trigger r 1 2000') [ "$FA_MODE" = deaf ] && return; fa_add cast; fa_inc magicka 30
				if [ "$(cat "$FA/o_right")" = "$ACTIONS_BOUND" ]; then echo 1 > "$FA/bound"; echo 00058F60 > "$FA/o_right"; echo 0010B0A7 > "$FA/o_ammo"
					if [ "$FA_MODE" = unbound ]; then fa_inc refused 2; echo 00000000 > "$FA/right"; echo 00000000 > "$FA/ammo"; else echo 00058F60 > "$FA/right"; echo 0010B0A7 > "$FA/ammo"; fi
				fi; echo ok ;;
			'pad trigger l 1 1500') [ "$FA_MODE" = deaf ] && return; if [ "$(cat "$FA/o_left")" = "$ACTIONS_HEAL" ]; then fa_add cast; fa_inc magicka 10; else fa_add block; fi; echo ok ;;
			'pad trigger l 1 2000') [ "$FA_MODE" = deaf ] || { touch "$FA/blocking"; fa_add block; }; echo ok ;;
			'pad trigger r 1 1500') [ "$FA_MODE" = deaf ] && return
				case $(cat "$FA/o_right") in "$ACTIONS_BOW"|00058F60) fa_add bow; fa_arrow_at ;; *) fa_add cast; fa_inc magicka 15 ;; esac; echo ok ;;
			'pad trigger r 1 2500') [ "$FA_MODE" = deaf ] && return; fa_add cast; fa_inc magicka 25; fa_flames_at; echo ok ;;
			'pad button rb 300') [ "$FA_MODE" = deaf ] || { fa_add shout; [ "$(cat "$FA/walked")" = 1 ] && [ "$FA_MODE" != burn ] && fa_inc magicRefused; }; echo ok ;;
			standins) echo "players=1 | player 2 standin=FF000801 placed=1 dressed=1 at=(100,$([ "$(cat "$FA/walked")" = 1 ] && echo 300 || echo 2000),0) heading=3.142" ;;
			where) echo "cell=00000000 world=0000003C pos=(100,200,0) heading=0.000 flags=0x0$(( $(cat "$FA/drawn") * 4 )) modal=0" ;;
			walkto\ FF000801*) [ "$FA_MODE" = farwalk ] || echo 1 > "$FA/walked"; echo "walking to FF000801" ;;
			walkto) echo 'not walking' ;;
			console*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fa$RANDOM; mkdir -p "$A_OUT"; a_init fa >/dev/null; actions_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok (45): the hook; 13 held by the stand-in (sword, shield, bow, arrows, Bound Bow, the conjured bow and arrows,
	# Flames, Healing; at the watcher's player Flames, the bow and arrows again; the shout); 12 draws and put-aways;
	# 10 actions played; nothing of the conjured bow refused; the walk; Flames cast, refused, unharmed; the arrow's hit,
	# unharmed; the shout's unharmed and refused
	t "way: all of it works" "$(fake_actions ok)" "PASS:45 "
	t "way: no hook on the mover's graph fails, and nothing else is tried" "$(fake_actions nohook)" "FAIL:1 "
	# deaf: no press does anything in the game: the first is the rig's input (one RIG ends the attempt), never a FAIL
	t "way: presses that do nothing are the rig's input (RIG), never a FAIL" "$(fake_actions deaf | grep -o 'FAIL:[0-9]*' || echo nofail)" nofail
	t "way: the first press that does nothing is a RIG and ends the attempt" "$(fake_actions deaf | grep -c 'RIG:')" 1
	# unheard: the game shows each action done, its hook hears none: a FAIL each (10 actions; the draws are read on the
	# mover's own flags and still pass), no RIG; mute: the same 10 unplayed; nohold: the 11 held on the mover's
	# set-ups (the conjured bow and arrows are the cast's); burn: Flames not refused, the health Flames and the arrow took
	t "way: actions the game did that the hook never heard fail, never RIG" "$(fake_actions unheard | grep -o 'FAIL:[0-9]*\|RIG:[0-9]*' | tr '\n' ' ')" "FAIL:10 "
	t "way: events heard and sent that the stand-in never plays fail" "$(fake_actions mute | grep -o 'FAIL:[0-9]*')" "FAIL:10"
	t "way: a stand-in that never holds its friend's gear fails" "$(fake_actions nohold | grep -o 'FAIL:[0-9]*')" "FAIL:11"
	t "way: a set-up the mover's own game never took is the rig's, and ends the way" "$(fake_actions noequip)" "PASS:1 RIG:1 "
	t "way: a draw the stand-in made by hand is a WARN" "$(fake_actions handdraw | grep -o 'WARN:[0-9]*')" "WARN:12"
	t "way: a stand-in whose Flames and arrow hurt the watcher's player fails" "$(fake_actions burn | grep -o 'FAIL:[0-9]*')" "FAIL:3"
	t "way: a conjured bow refused by the watcher fails" "$(fake_actions unbound | grep -o 'FAIL:[0-9]*')" "FAIL:3"
	t "way: a walk to the watcher that never gets there is the rig's" "$(fake_actions farwalk | grep -o 'FAIL:[0-9]*\|RIG:[0-9]*' | tr '\n' ' ')" "RIG:1 "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "actions is a step" "$(case " $STEPS " in *" actions "*) echo yes ;; esac)" yes
	t "actions comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' actions')" 1
	t "actions needs a session" "$(case " $SESSION_STEPS " in *" actions "*) echo yes ;; esac)" yes
	t "actions runs by default" "$(case " $OPTIN_STEPS " in *" actions "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "actions.sh selftest: all ok" || echo "actions.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
