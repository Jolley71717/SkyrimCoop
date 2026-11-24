#!/bin/bash
# steps/npcspells.sh: the rig step `npcspells` (protocol 21, docs/DESIGN.md "NPCs": "Spells"), sourced by steps.sh. A
# copy of an NPC another game runs holds its runner's spells and voice (NpcGear carries the voice now), plays its casts,
# is never taught a spell it does not know (a copy of a load-order NPC goes into this game's saves), makes nothing here
# (a summon, a raised corpse, a placed reference, telekinesis, a lock, slowed time: the effect hook refuses it, by
# archetype), and what it made here all the same is ended by the scan. Not run in a game yet.
#
#   step_npcspells     the step
#   bash tools/rig/steps/npcspells.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the runner's game runs a person, the watcher's game shows its copy), both players near each other:
#   0. Set-up (said in the run): a person the runner runs that the watcher shows as a copy (shots.sh's sh_pick; none is
#      a rig fault). The runner's game finds the spells (`npcs spells find aimed|self|summon|power`: the lowest-id spell
#      of the load order of that kind; no aimed, self or summon spell is a rig fault, no power only skips the voice with
#      a WARN). The watcher's console teaches the copy the aimed and self spells and the power (`<copy>.addspell`, taken
#      back after: a copy of a load-order NPC readies only what it knows), and the runner's dev channel arms its NPC
#      (`npcs spells arm <local> right <aimed>`, `left <self>`, `voice <power>`; taught to it if it lacks them, taken back
#      after). A refused arm is a rig fault.
#   1. Hands: within NPCSPELLS_WAIT the watcher's copy holds its runner's spell in each hand and its voice ready
#      (`npcs spells <key>`: right=, left=, voice= each `shown`); a hand still not shown is a FAIL (the row is said).
#   2. Never taught: the runner arms its NPC's left hand with the summon spell, which the copy was not taught. Within
#      NPCSPELLS_WAIT the watcher's copy's left hand reads `lacks` (it keeps its own; PASS). `shown` is a FAIL: the copy
#      was taught a spell, which would stay in its save. Anything else at the end (`differs`) is a FAIL. The left hand is
#      armed with the self spell again after.
#   3. Refused: the watcher's console makes the copy cast the summon spell on itself (`<copy>.cast <summon> <copy>`:
#      set-up, the injected cast of a copy's own AI). Within NPCSPELLS_SETTLE the effect hook refused it (`npcs spells`
#      refused=summon: up) and nothing it made is in the world (madeNow=0): PASS. Not refused, and what it made ended by
#      the scan within NPCSPELLS_END (madeEnded= up, madeNow=0): a WARN (the hook missed it; the belt caught it). Made and
#      still there: a FAIL. Neither refused nor made: the console's cast did nothing (a rig fault).
#   4. Ended: the watcher's dev channel lets a copy's effects through (`npcs spells refuse off`, set-up) and the console
#      makes the copy cast the summon again. Within NPCSPELLS_END the scan ended it (madeEnded= up, madeNow=0): PASS;
#      still there is a FAIL; nothing made at all is a rig fault. `npcs spells refuse on` after, whatever happened.
#   5. Casts: the runner's NPC is set on the runner's player with the console (npcs.sh's fight set-up: confidence and
#      aggression set so it stands and fights, the player given NPCSPELLS_HEALTH health; all put back after). Within
#      NPCSPELLS_FIGHT_WAIT the watcher's copy plays a cast event of its runner's (castsPlayed= up; PASS), and turns away
#      no more cast events than it plays. The runner's NPC never cast (its row's castsSent= not up: its AI chose to run
#      or punch) is a WARN; cast while the copy played none is a FAIL. The magicka given back to the copy is said.
#   6. Undo: the runner's hands and voice emptied (`arm ... 0`, which takes back what arm taught), the copy's taught
#      spells removed with the console, the effects refused again.
# Not on the rig yet: a creature's spells (a spriggan's, an atronach's: arm takes people's hands only), a dragon's
# shout, a raised corpse (Reanimate) and the other refused kinds (they share the summon's path in the hook and the
# scan), a spawn's copy taught what it lacks.

NPCSPELLS_WAIT=${NPCSPELLS_WAIT:-10}              # s: for the watcher's copy to hold what its runner's NPC holds
NPCSPELLS_SETTLE=${NPCSPELLS_SETTLE:-2}           # s: after a console cast, for the hook's count and the scan
NPCSPELLS_END=${NPCSPELLS_END:-8}                 # s: for what a copy made to be ended (MADE_GRACE 2 s, scans 4 a second)
NPCSPELLS_FIGHT_WAIT=${NPCSPELLS_FIGHT_WAIT:-25}  # s: for the copy to play a cast of its fighting runner's NPC
NPCSPELLS_HEALTH=${NPCSPELLS_HEALTH:-5000}        # the health the runner's player is given for the fight

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# ns_head <npcs spells reply>: the counts (before the first " | ")
ns_head() { echo "${1%% | *}"; }
# ns_row <npcs spells reply> <key>: that NPC's part ("KEY local=... role=..."); nothing when not listed
ns_row() { awk -v k="$2" 'BEGIN { RS = " [|] " } NR > 1 { sub(/\n$/, "") } NR > 1 && $1 == k { print; exit }' <<<"$1"; }
# ns_hand <row> <right|left|voice>: that hand's state (shown, lacks, unnamed, differs, empty); nothing when not read
ns_hand() { local v; v=$(field "$2" "$1"); [[ $v == *:* ]] && echo "${v##*:}"; }
# ns_kind <list> <kind>: how many of that kind ("summon:3,spawn:0"), 0 when none
ns_kind() { local n; n=$(tr ',' '\n' <<<"$1" | sed -n "s/^$2://p" | head -1); echo "${n:-0}"; }
# ns_refused <npcs spells reply> <kind>: the copy's effects of that kind refused so far
ns_refused() { ns_kind "$(field refused "$(ns_head "$1")")" "$2"; }
# ns_found <npcs spells find reply>: the spell's id; nothing when none
ns_found() { [[ $1 == "spell id="* ]] && field id "$1"; }
# ns_rose <before> <after>: both numbers and the second larger
ns_rose() { is_num "$1" && is_num "$2" && _cmp "$2" ">" "$1"; }

# ns_read <who> <key>: that game's `npcs spells <key>` reply
ns_read() { dc "$1" "npcs spells $2"; }
# ns_get <who> <field>: a count of that game's `npcs spells`
ns_get() { field "$2" "$(ns_head "$(dc "$1" 'npcs spells')")"; }
# ns_hands_are <who> <key> <state> <hand...>: every named hand of that game's copy is in that state
ns_hands_are() {
	local r h; r=$(ns_row "$(ns_read "$1" "$2")" "$2")
	for h in "${@:4}"; do [ "$(ns_hand "$r" "$h")" = "$3" ] || return 1; done
}
# ns_left_settled <who> <key>: the copy's left hand reads lacks or shown (taught): the step's answer either way
ns_left_settled() { local s; s=$(ns_hand "$(ns_row "$(ns_read "$1" "$2")" "$2")" left); [ "$s" = lacks ] || [ "$s" = shown ]; }
# ns_ended <who> <madeEnded before>: something made here was ended since, and nothing made is left
ns_ended() { local h; h=$(ns_head "$(dc "$1" 'npcs spells')"); ns_rose "$2" "$(field madeEnded "$h")" && [ "$(field madeNow "$h")" = 0 ]; }
# ns_played <who> <key> <castsPlayed before>: the copy played a cast since
ns_played() { ns_rose "$3" "$(field castsPlayed "$(ns_row "$(ns_read "$1" "$2")" "$2")")"; }
# ns_arm <who> <local> <hand> <id>: arms (or, with 0, empties) a hand of an NPC that game runs; true when it took
ns_arm() { local r; r=$(dc "$1" "npcs spells arm $2 $3 $4"); NS_ARMED=$r; [[ $r == armed* || $r == cleared* ]]; }

# The way's set-up, kept for its legs and the undo.
NS_KEY=""; NS_MLOCAL=""; NS_WLOCAL=""; NS_AIMED=""; NS_SELF=""; NS_SUMMON=""; NS_POWER=""; NS_ARMED=""

# ns_setup <runner> <watcher> <label>: the person picked, the spells found, the copy taught, the runner's NPC armed;
# false (a rig fault said) when the way cannot go on
ns_setup() {
	local m=$1 w=$2 label=$3 mid pick kind r
	NS_KEY=""; NS_MLOCAL=""; NS_WLOCAL=""; NS_AIMED=""; NS_SELF=""; NS_SUMMON=""; NS_POWER=""
	mid=$(field player "$(dc "$m" 'session status')")
	pick=$(sh_pick "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")" human)
	if [ -z "$pick" ]; then rig_fault state "$label: a person the $m runs that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"; return 1; fi
	read -r NS_KEY NS_MLOCAL _ <<<"$pick"
	NS_WLOCAL=$(npc_col "$(npc_rows "$(dc "$w" 'npcs list')")" "$NS_KEY" 14)
	if ! [[ $NS_WLOCAL =~ ^[0-9A-F]{8}$ ]]; then rig_fault state "$label: the $w's copy of $NS_KEY (set-up)" "no local id: $(dc "$w" "npcs $NS_KEY" | cut -c1-200)"; return 1; fi
	for kind in aimed self summon power; do
		r=$(dc "$m" "npcs spells find $kind")
		printf -v "NS_${kind^^}" '%s' "$(ns_found "$r")"
	done
	if [ -z "$NS_AIMED" ] || [ -z "$NS_SELF" ] || [ -z "$NS_SUMMON" ]; then
		rig_fault state "$label: an aimed, a self and a summon spell in the load order (set-up)" "aimed=${NS_AIMED:-none} self=${NS_SELF:-none} summon=${NS_SUMMON:-none}"
		return 1
	fi
	[ -n "$NS_POWER" ] || warn "$label: a power in the load order for the voice" "none found ($(dc "$m" 'npcs spells find power' | cut -c1-120)): the voice is not tried"
	for r in $NS_AIMED $NS_SELF $NS_POWER; do dc "$w" "console $NS_WLOCAL.addspell $r" >/dev/null; done
	say "$label: set-up: the $m's $NS_KEY ($NS_MLOCAL, the $w's copy $NS_WLOCAL); the $w's console taught the copy aimed=$NS_AIMED self=$NS_SELF power=${NS_POWER:-none} (taken back after)"
	if ! ns_arm "$m" "$NS_MLOCAL" right "$NS_AIMED" || ! ns_arm "$m" "$NS_MLOCAL" left "$NS_SELF" || { [ -n "$NS_POWER" ] && ! ns_arm "$m" "$NS_MLOCAL" voice "$NS_POWER"; }; then
		rig_fault state "$label: the $m's NPC armed (set-up)" "npcs spells arm: $NS_ARMED"
		ns_undo "$m" "$w"
		return 1
	fi
	say "$label: set-up (dev): the $m's NPC armed right=$NS_AIMED left=$NS_SELF voice=${NS_POWER:-none}"
}

# ns_undo <runner> <watcher>: the runner's NPC's hands and voice emptied (what arm taught taken back), the copy's taught
# spells removed, the copy's effects refused again
ns_undo() {
	local m=$1 w=$2 id
	[ -n "$NS_MLOCAL" ] && for id in right left voice; do dc "$m" "npcs spells arm $NS_MLOCAL $id 0" >/dev/null; done
	[ -n "$NS_WLOCAL" ] && for id in $NS_AIMED $NS_SELF $NS_POWER; do dc "$w" "console $NS_WLOCAL.removespell $id" >/dev/null; done
	dc "$w" 'npcs spells refuse on' >/dev/null
	say "npcspells: set-up undone (the $m's NPC's hands and voice emptied, the $w's copy's taught spells removed, its effects refused)"
}

# ns_hands <runner> <watcher> <label>: the copy holds its runner's spells and voice
ns_hands() {
	local m=$1 w=$2 label="$3: the $2's copy holds its runner's spells and voice" hands="right left" row
	[ -n "$NS_POWER" ] && hands="right left voice"
	if wait_until "$NPCSPELLS_WAIT" ns_hands_are "$w" "$NS_KEY" shown $hands; then
		row=$(ns_row "$(ns_read "$w" "$NS_KEY")" "$NS_KEY")
		pass "$label" "right=$(field right "$row") left=$(field left "$row") voice=$(field voice "$row")"
	else
		fail "$label" "$(ns_row "$(ns_read "$w" "$NS_KEY")" "$NS_KEY" | cut -c1-300); the $m's: $(ns_row "$(ns_read "$m" "$NS_KEY")" "$NS_KEY" | cut -c1-160)"
	fi
}

# ns_lacks <runner> <watcher> <label>: a spell the copy does not know is not taught to it
ns_lacks() {
	local m=$1 w=$2 label="$3: a spell the $2's copy does not know is never taught to it (lacks)" s row
	if ! ns_arm "$m" "$NS_MLOCAL" left "$NS_SUMMON"; then rig_fault state "$label: the $m's NPC armed with the summon (set-up)" "$NS_ARMED"; return; fi
	wait_until "$NPCSPELLS_WAIT" ns_left_settled "$w" "$NS_KEY"
	row=$(ns_row "$(ns_read "$w" "$NS_KEY")" "$NS_KEY"); s=$(ns_hand "$row" left)
	case "$s" in
		lacks) pass "$label" "left=$(field left "$row") voicesLacked=$(ns_get "$w" voicesLacked)" ;;
		shown) fail "$label" "left=$(field left "$row"): the copy was taught $NS_SUMMON, which would stay in its save" ;;
		*) fail "$label" "left=$(field left "$row") (${s:-not read}) after $NPCSPELLS_WAIT s: $(cut -c1-200 <<<"$row")" ;;
	esac
	ns_arm "$m" "$NS_MLOCAL" left "$NS_SELF" >/dev/null || say "$label: the left hand armed with $NS_SELF again: $NS_ARMED"
}

# ns_refused_leg <watcher> <label>: the copy's summon is refused by the effect hook
ns_refused_leg() {
	local w=$1 label="$2: the $1's copy's summon is refused here" h0 h1 r0 r1 e0 r
	h0=$(dc "$w" 'npcs spells'); r0=$(ns_refused "$h0" summon); e0=$(field madeEnded "$(ns_head "$h0")")
	r=$(dc "$w" "console $NS_WLOCAL.cast $NS_SUMMON $NS_WLOCAL")
	if [ "$r" != ran ]; then rig_fault input "$label: the copy casts the summon (console set-up)" "the console refused: $r"; return; fi
	say "$label: set-up (console): the $w's copy $NS_WLOCAL cast $NS_SUMMON on itself"
	sleep "$NPCSPELLS_SETTLE"
	h1=$(dc "$w" 'npcs spells'); r1=$(ns_refused "$h1" summon)
	if ns_rose "$r0" "$r1"; then
		if [ "$(field madeNow "$(ns_head "$h1")")" = 0 ]; then pass "$label" "refused summon $r0 -> $r1, madeNow=0"
		elif wait_until "$NPCSPELLS_END" ns_ended "$w" "$e0"; then warn "$label" "refused ($r0 -> $r1), and something else it made was ended by the scan (madeEnded $e0 -> $(ns_get "$w" madeEnded))"
		else fail "$label" "refused ($r0 -> $r1), yet something it made stays: madeNow=$(ns_get "$w" madeNow)"; fi
	elif wait_until "$NPCSPELLS_END" ns_ended "$w" "$e0"; then
		warn "$label" "the hook let it through (refused summon $r0 -> $r1) and the scan ended what it made (madeEnded $e0 -> $(ns_get "$w" madeEnded)): a path the hook does not see"
	elif [ "$(ns_get "$w" madeNow)" != 0 ]; then
		fail "$label" "not refused (summon $r0 -> $r1) and what it made stays: madeNow=$(ns_get "$w" madeNow) madeEnded=$(ns_get "$w" madeEnded)"
	else
		rig_fault input "$label: the copy casts the summon (console set-up)" "neither refused nor made (refused=$(field refused "$(ns_head "$h1")") madeEnded=$(ns_get "$w" madeEnded)): the console's cast did nothing"
	fi
}

# ns_ended_leg <watcher> <label>: what a copy made here all the same is ended by the scan
ns_ended_leg() {
	local w=$1 label="$2: what the $1's copy made here is ended by the scan" e0 r
	r=$(dc "$w" 'npcs spells refuse off')
	if [ "$r" != refuse=0 ]; then rig_fault state "$label: the copy's effects let through (dev set-up)" "$r"; return; fi
	e0=$(ns_get "$w" madeEnded)
	r=$(dc "$w" "console $NS_WLOCAL.cast $NS_SUMMON $NS_WLOCAL")
	if [ "$r" != ran ]; then
		rig_fault input "$label: the copy casts the summon (console set-up)" "the console refused: $r"
	else
		say "$label: set-up (dev, console): the $w lets its copies' effects through; its copy $NS_WLOCAL cast $NS_SUMMON on itself"
		if wait_until "$NPCSPELLS_END" ns_ended "$w" "$e0"; then
			pass "$label" "madeEnded $e0 -> $(ns_get "$w" madeEnded), madeRemoved=$(ns_get "$w" madeRemoved), madeNow=0"
		elif [ "$(ns_get "$w" madeNow)" != 0 ]; then
			fail "$label" "madeNow=$(ns_get "$w" madeNow) after $NPCSPELLS_END s (madeEnded $e0 -> $(ns_get "$w" madeEnded)): a summon stands beside the copy of the real one"
		else
			rig_fault input "$label: the copy casts the summon (console set-up)" "nothing made (madeEnded $e0 -> $(ns_get "$w" madeEnded), madeNow=0): the console's cast did nothing"
		fi
	fi
	r=$(dc "$w" 'npcs spells refuse on')
	[ "$r" = refuse=1 ] || warn "$label: the copy's effects refused again" "$r: later steps run with a copy's effects let through"
}

# ns_casts <runner> <watcher> <label>: the runner's NPC fights its player; the watcher's copy plays its casts
ns_casts() {
	local m=$1 w=$2 label="$3: the $2's copy plays its runner's casts" one conf0 aggr0 setav=0 p0 x0 s0 row mrow p1 x1 s1
	row=$(ns_row "$(ns_read "$w" "$NS_KEY")" "$NS_KEY"); p0=$(field castsPlayed "$row"); x0=$(field castsRefused "$row")
	s0=$(field castsSent "$(ns_row "$(ns_read "$m" "$NS_KEY")" "$NS_KEY")")
	one=$(dc "$m" "npcs $NS_MLOCAL"); conf0=$(field confidence "$one"); aggr0=$(field aggression "$one")
	if is_num "$conf0" && is_num "$aggr0"; then
		dc "$m" "console $NS_MLOCAL.setav confidence $NPC_FIGHT_CONFIDENCE" >/dev/null; dc "$m" "console $NS_MLOCAL.setav aggression $NPC_FIGHT_AGGRESSION" >/dev/null; setav=1
	fi
	if [ "$(dc "$m" "console player.modav health $NPCSPELLS_HEALTH")" != ran ] || [ "$(dc "$m" "console $NS_MLOCAL.startcombat player")" != ran ]; then
		rig_fault input "$label: the fight's set-up" "the $m's console refused modav or startcombat"
	else
		say "$label: set-up (console): the $m's player +$NPCSPELLS_HEALTH health; $NS_MLOCAL ($NS_KEY) set on it with startcombat$([ "$setav" = 1 ] && echo ", confidence $conf0 -> $NPC_FIGHT_CONFIDENCE, aggression $aggr0 -> $NPC_FIGHT_AGGRESSION")"
		if wait_until "$NPCSPELLS_FIGHT_WAIT" ns_played "$w" "$NS_KEY" "$p0"; then
			sleep 3  # a few more casts, for the counts
			row=$(ns_row "$(ns_read "$w" "$NS_KEY")" "$NS_KEY"); p1=$(field castsPlayed "$row"); x1=$(field castsRefused "$row")
			mrow=$(ns_row "$(ns_read "$m" "$NS_KEY")" "$NS_KEY"); s1=$(field castsSent "$mrow")
			pass "$label" "castsPlayed $p0 -> $p1 (last $(field lastCast "$row")), the $m sent $s0 -> $s1"
			expect_le "$label: it turns away no more of them than it plays" "$((x1 - x0))" "$((p1 - p0))" "cast events refused, of $((p1 - p0)) played"
			say "$label: the copy's magicka $(field magicka "$row"), given back $(field magickaBack "$row") (magickaTopUps=$(ns_get "$w" magickaTopUps))"
		else
			mrow=$(ns_row "$(ns_read "$m" "$NS_KEY")" "$NS_KEY"); s1=$(field castsSent "$mrow")
			if ns_rose "$s0" "$s1"; then
				fail "$label" "the $m's NPC cast (castsSent $s0 -> $s1) and the copy played none in $NPCSPELLS_FIGHT_WAIT s: $(ns_row "$(ns_read "$w" "$NS_KEY")" "$NS_KEY" | cut -c1-240)"
			else
				warn "$label" "the $m's NPC never cast in $NPCSPELLS_FIGHT_WAIT s (castsSent $s0 -> ${s1:-?}: its AI fought another way): casts not seen"
			fi
		fi
	fi
	dc "$m" "console $NS_MLOCAL.stopcombat" >/dev/null
	if [ "$setav" = 1 ]; then dc "$m" "console $NS_MLOCAL.setav confidence $conf0" >/dev/null; dc "$m" "console $NS_MLOCAL.setav aggression $aggr0" >/dev/null; fi
	dc "$m" "console player.restoreav health $((NPCSPELLS_HEALTH * 2))" >/dev/null
	dc "$m" "console player.modav health -$NPCSPELLS_HEALTH" >/dev/null
	still "$m"
}

# npcspells_way <runner> <watcher>: the legs, one person of the runner's
npcspells_way() {
	local m=$1 w=$2 label="npcspells ($1 runs, $2 shows)"
	section "npcspells: a person the $m runs holds spells, the $w's copy of it"
	ns_setup "$m" "$w" "$label" || return 0
	ns_hands "$m" "$w" "$label"
	ns_lacks "$m" "$w" "$label"
	ns_refused_leg "$w" "$label"
	ns_ended_leg "$w" "$label"
	ns_casts "$m" "$w" "$label"
	ns_undo "$m" "$w"
}

step_npcspells() {
	local g
	for g in host vm; do still $g; done
	npcspells_way vm host
	npcspells_way host vm
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init npcspellstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (NpcSync's Spells)
	R='spells copies=2 holding=1 hands=shown:2,lacks:1,unnamed:0,differs:0 castsPlayed=5 castsRefused=1 castsSent=7 voicesSet=1 voicesLacked=0 magickaTopUps=3 refuse=1 refused=summon:2,spawn:0,move:1,lock:0,time:0 madeEnded=1 madeRemoved=0 madeNow=0 | 0001A67C local=0001A67C role=copy runner=2 mirror=1 gear=1 right=0001C789/0001C789:shown left=000640B6/00012FCC:lacks voice=00013E07/00013E07:shown magicka=80/120 magickaBack=35 castsPlayed=4 castsRefused=1 lastCast=BeginCastRight | 0001A690 local=0001A690 role=run right=0001C789 left=00000000 voice=00000000 castsSent=7'
	t "head: the counts only" "$(field lastCast "$(ns_head "$R")")" ""
	t "head: madeNow" "$(field madeNow "$(ns_head "$R")")" 0
	t "head: castsPlayed, not a row's" "$(field castsPlayed "$(ns_head "$R")")" 5
	t "row: that copy's" "$(field castsPlayed "$(ns_row "$R" 0001A67C)")" 4
	t "row: a run NPC's" "$(field castsSent "$(ns_row "$R" 0001A690)")" 7
	t "row: none not listed" "$(ns_row "$R" 0001A67D)" ""
	t "row: never the head" "$(ns_row "$R" spells)" ""
	t "hand: shown" "$(ns_hand "$(ns_row "$R" 0001A67C)" right)" shown
	t "hand: lacks" "$(ns_hand "$(ns_row "$R" 0001A67C)" left)" lacks
	t "hand: a run NPC's has no state" "$(ns_hand "$(ns_row "$R" 0001A690)" right)" ""
	t "hand: none read" "$(ns_hand "" right)" ""
	t "refused: summon" "$(ns_refused "$R" summon)" 2
	t "refused: move" "$(ns_refused "$R" move)" 1
	t "refused: lock" "$(ns_refused "$R" lock)" 0
	t "refused: none read" "$(ns_refused "" summon)" 0
	t "found: the id" "$(ns_found 'spell id=0001C789 kind=aimed casting=ff name="Firebolt"')" 0001C789
	t "found: none" "$(ns_found 'none: no power spell')" ""
	t "rose" "$(ns_rose 1 2 && echo yes)" yes
	t "rose: not equal" "$(ns_rose 2 2 || echo no)" no
	t "rose: not a number" "$(ns_rose '' 2 || echo no)" no

	# The VM runs the person (0001A67C), the host shows its copy (0001A67C: a load-order NPC, the same id). FN_MODE:
	# ok; nopick (no person: the rig's); nofind (no summon spell: the rig's); nopower (no power: a WARN, the voice not
	# tried); noarm (arm refused: the rig's); hands (the copy's left hand differs: a FAIL); taught (the summon shown in the
	# copy's left hand: a FAIL); stuck (the left hand still differs: a FAIL); unrefused (the hook misses, the scan ends it:
	# a WARN); made (not refused, and what it made stays: a FAIL, and the belt's FAIL too); nothing (the console's cast does
	# nothing, both times: two rig faults); nocast (the runner's NPC never casts: a WARN); noplay (it casts, the copy
	# plays none: a FAIL); turned (the copy turns away more casts than it plays: a FAIL). FN_SHOW=1 adds the undo's
	# console and dev lines in order.
	fake_ns() {
		FN=$d/fn.$RANDOM; mkdir -p "$FN"; FN_MODE=$1
		local f; for f in refused ended now sent played turned left_taught; do echo 0 > "$FN/$f"; done
		echo 1 > "$FN/refuse"; echo none > "$FN/left"; : > "$FN/undo"
		fn_inc() { echo $(( $(cat "$FN/$1") + ${2:-1} )) > "$FN/$1"; }
		fn_spells() {  # the host's `npcs spells` reply
			local left right=0001C789/0001C789:shown voice=00013E07/00013E07:shown lefts
			case "$(cat "$FN/left")" in
				self) left=00012FCC/00012FCC:shown ;;
				summon) case "$FN_MODE" in taught) left=000640B6/000640B6:shown ;; stuck) left=000640B6/00012FCC:differs ;; *) left=000640B6/00012FCC:lacks ;; esac ;;
				*) left=00000000/00000000:empty ;;
			esac
			[ "$FN_MODE" = hands ] && [ "$(cat "$FN/left")" = self ] && left=00012FCC/00000000:differs
			[ "$FN_MODE" = nopower ] && voice=00000000/00000000:empty
			echo "spells copies=1 holding=1 hands=shown:2,lacks:0,unnamed:0,differs:0 castsPlayed=$(cat "$FN/played") castsRefused=$(cat "$FN/turned") castsSent=0 voicesSet=1 voicesLacked=0 magickaTopUps=2 refuse=$(cat "$FN/refuse") refused=summon:$(cat "$FN/refused"),spawn:0,move:0,lock:0,time:0 madeEnded=$(cat "$FN/ended") madeRemoved=0 madeNow=$(cat "$FN/now") | 0001A67C local=0001A67C role=copy runner=2 mirror=1 gear=1 right=$right left=$left voice=$voice magicka=90/120 magickaBack=30 castsPlayed=$(cat "$FN/played") castsRefused=$(cat "$FN/turned") lastCast=BeginCastRight"
		}
		fn_cast() {  # the host's copy casts the summon
			case "$FN_MODE" in
				nothing) ;;
				made) echo 1 > "$FN/now" ;;
				unrefused) fn_inc ended ;;
				*) if [ "$(cat "$FN/refuse")" = 1 ]; then fn_inc refused; else fn_inc ended; fi ;;
			esac
		}
		dc() { case "$1 $2" in
			"vm session status") echo "role=client connection=up welcomed=1 player=2" ;;
			"vm where"|"host where") echo "cell=0000A1B2 world=0000003C pos=(100,200,0) modal=0" ;;
			"vm npcs list") [ "$FN_MODE" = nopick ] && { echo "npcs=0"; return; }
				echo 'npcs=1 | 0001A67C local=0001A67C owner=2 role=run kind=human at=(100,200,0) dead=0 child=0' ;;
			"host npcs list") echo 'npcs=1 | 0001A67C local=0001A67C owner=2 role=copy kind=human at=(110,200,0) dead=0 child=0 runner=2 mode=mirror' ;;
			"vm npcs spells find aimed") echo 'spell id=0001C789 kind=aimed casting=ff name="Firebolt"' ;;
			"vm npcs spells find self") echo 'spell id=00012FCC kind=self casting=held name="Healing"' ;;
			"vm npcs spells find summon") [ "$FN_MODE" = nofind ] && { echo "none: no summon spell"; return; }; echo 'spell id=000640B6 kind=summon casting=ff name="Conjure Familiar"' ;;
			"vm npcs spells find power") [ "$FN_MODE" = nopower ] && { echo "none: no power spell"; return; }; echo 'spell id=00013E07 kind=power casting=ff name="Power"' ;;
			"vm npcs spells arm 0001A67C "*)
				set -- $2
				[ "$FN_MODE" = noarm ] && { echo "error: the hand refused"; return; }
				if [ "$6" = 0 ]; then echo "cleared $5 0" >> "$FN/undo"; echo "cleared $5 holds=00000000 untaught=1"; return; fi
				[ "$5" = left ] && case "$6" in 00012FCC) echo self > "$FN/left" ;; 000640B6) echo summon > "$FN/left" ;; esac
				echo "armed $5 $6 taught=1 holds=$6" ;;
			"vm npcs spells 0001A67C") echo "spells copies=0 holding=0 hands=shown:0,lacks:0,unnamed:0,differs:0 castsPlayed=0 castsRefused=0 castsSent=$(cat "$FN/sent") | 0001A67C local=0001A67C role=run right=0001C789 left=00012FCC voice=00013E07 castsSent=$(cat "$FN/sent")" ;;
			"host npcs spells"|"host npcs spells 0001A67C") fn_spells ;;
			"host npcs spells refuse off") echo 0 > "$FN/refuse"; echo "refuse=0" ;;
			"host npcs spells refuse on") echo 1 > "$FN/refuse"; echo "refuse=1" ;;
			"host console 0001A67C.cast 000640B6 0001A67C") fn_cast; echo ran ;;
			"host console 0001A67C.removespell "*) set -- $2; echo "removespell ${3#*removespell }" >> "$FN/undo"; echo ran ;;
			"vm npcs 0001A67C") echo "npcs 0001A67C | role=run confidence=1 aggression=0" ;;
			"vm console 0001A67C.startcombat player")
				case "$FN_MODE" in
					nocast) ;;
					noplay) fn_inc sent 3 ;;
					turned) fn_inc sent 3; fn_inc played 1; fn_inc turned 4 ;;
					*) fn_inc sent 3; fn_inc played 3 ;;
				esac; echo ran ;;
			*" console"*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fn$RANDOM; mkdir -p "$A_OUT"; a_init fn >/dev/null
			npcspells_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}'; echo -n "refuse=$(cat "$FN/refuse")"
			[ -n "${FN_SHOW:-}" ] && echo -n " undo=$(paste -sd, "$FN/undo")" )
	}
	# hands (1), lacks (1), refused (1), ended (1), casts (1) and no more turned away than played (1)
	t "way: all of it" "$(fake_ns ok)" "PASS:6 refuse=1"
	t "way: no person is the rig's" "$(fake_ns nopick)" "RIG:1 refuse=1"
	t "way: no summon spell is the rig's" "$(fake_ns nofind)" "RIG:1 refuse=1"
	t "way: no power skips the voice with a WARN" "$(fake_ns nopower)" "PASS:6 WARN:1 refuse=1"
	t "way: a refused arm is the rig's, and undone" "$(fake_ns noarm)" "RIG:1 refuse=1"
	t "way: a hand not shown fails" "$(fake_ns hands)" "FAIL:1 PASS:5 refuse=1"
	t "way: a copy taught a spell fails (its save keeps it)" "$(fake_ns taught)" "FAIL:1 PASS:5 refuse=1"
	t "way: a hand that never settles fails" "$(fake_ns stuck)" "FAIL:1 PASS:5 refuse=1"
	t "way: a summon the hook missed and the scan ended is a WARN" "$(fake_ns unrefused)" "PASS:5 WARN:1 refuse=1"
	t "way: a summon made and left fails, both times" "$(fake_ns made)" "FAIL:2 PASS:4 refuse=1"
	t "way: a console cast that does nothing is the rig's, both times, and refusal back on" "$(fake_ns nothing)" "PASS:4 RIG:2 refuse=1"
	t "way: a runner's NPC that never casts is a WARN" "$(fake_ns nocast)" "PASS:4 WARN:1 refuse=1"
	t "way: casts sent and none played fails" "$(fake_ns noplay)" "FAIL:1 PASS:4 refuse=1"
	t "way: more casts turned away than played fails" "$(fake_ns turned)" "FAIL:1 PASS:5 refuse=1"
	t "undo: hands emptied, taught spells removed" "$(FN_SHOW=1 fake_ns ok)" "PASS:6 refuse=1 undo=cleared right 0,cleared left 0,cleared voice 0,removespell 0001C789,removespell 00012FCC,removespell 00013E07"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "npcspells is a step" "$(case " $STEPS " in *" npcspells "*) echo yes ;; esac)" yes
	t "npcspells comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' npcspells')" 1
	t "npcspells needs a session" "$(case " $SESSION_STEPS " in *" npcspells "*) echo yes ;; esac)" yes
	t "npcspells runs by default" "$(case " $OPTIN_STEPS " in *" npcspells "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "npcspells.sh selftest: all ok" || echo "npcspells.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
