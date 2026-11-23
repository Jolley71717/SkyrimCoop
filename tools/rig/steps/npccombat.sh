#!/bin/bash
# steps/npccombat.sh: the rig step `npccombat` (protocol 8, docs/DESIGN.md "NPC health and deaths"), sourced by
# steps.sh. A player's hits on a copy of an NPC the other game runs reach that game and are applied there; both games
# show the NPC at the same health; its death is its runner's, said once and shown in both; raised, it lives in both.
# Not run in a game yet.
#
#   step_npccombat     the step
#   bash tools/rig/steps/npccombat.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# What it does, each way (one game runs the NPC, the other's player hits its copy), both players in one session near
# each other at the harness's cell (Riverwood), after the npcs step; a creature the host runs one way, a person the VM
# runs the other, so both actor kinds' paths run (a person's: the crime, its killer a stand-in):
#   1. An NPC to fight: one the runner runs (a load-order NPC, alive, no child, not essential, not protected: a protected
#      NPC dies only to its own game's player) that the hitter shows as a mirrored copy: the way's kind first (a creature
#      or a person), then the nearest. None is a WARN; none of the way's kind is a WARN too, and the other kind is fought.
#      The fight's set-up: the runner's console sets it on the runner's player (`startcombat`, its confidence and
#      aggression raised and the player given NPC_FIGHT_HEALTH health, as the npcs step's fight), and the step waits
#      until it fights (combat=1): a game whose player hits a copy claims it as a fight, and only the runner's own
#      fight keeps it with its runner (NpcBook), so the hits stay a friend's on an NPC another game runs. No fight
#      is the scene's (RIG).
#   2. The runner's word: the runner's console brings its NPC to COMBAT_FIRST of its health (`damageav`, set-up: as a
#      trap or another NPC's blow would there). The hitter's copy shows the same health (hp=, within COMBAT_HP_TOL)
#      within NPC_WAIT: the copy follows its runner's word, never its own game's.
#   3. The hitter's hit: the hitter's player casts Firebolt at the copy (`player.cast` from the console: an injected
#      attack, as the harness injects the host's attacks in combat tests; a real spell, a real hit event). The hit is
#      sent (hitsSent= up), reaches the runner and is applied there (the hitter's hitsApplied= and the runner's
#      hitsInApplied= up), and both games show the NPC at the same health after it; the runner still runs it (owner=).
#      A copy that took the hit and sent none is a FAIL; a cast that never hit the copy is the scene's (RIG).
#  3b. The core rule: what no hit of ours explains is set back. COMBAT_SETTLE after the last cast (its hit event out of
#      reach), the hitter's console damages its copy by COMBAT_SETBACK of its most (`damageav`: no hit, as another NPC's
#      blow or a trap would): within COMBAT_SETBACK_WAIT the copy is back where it was (its runner's word) and the
#      hitter's givenBack= is up. Then the same with `restoreav` (a heal no runner said): back, takenBack= up. A copy
#      that keeps either is the double blow, or the heal on hit, this design is for: FAIL.
#   4. The kill: the runner's console brings its NPC to COMBAT_LOW (set-up), and the hitter casts until it dies. The
#      runner's NPC dies of the hitter's hits (dead=1 there), the copy is dead in the hitter's game too, and both games
#      have it dead for the session, the hitter its killer (sessionDead=1 killer=<the hitter's player>).
#   5. Raised: after COMBAT_FIGHT_HOLD (the hitter's game claims the NPC as a fight that long after its last hit), the
#      runner's console raises its NPC (`resurrect 1`, set-up: as a spell would). It lives in both games again (dead=0,
#      sessionDead=0) within NPC_WAIT + REVIVE_AFTER.
#   6. The runner's own kill (`kill` from its console: a death no hit of the hitter's made) kills the copy in the hitter's
#      game (copyKills= up there); raised again, it lives in both.
#   After both ways: no hit of either game was lost (hitsLost=0); the hits refused and the healths set back are said.
#   Then the session's dead reach a game that joins (npccombat_rejoin): the VM leaves the session, the host's console
#   kills an NPC the host runs (set-up), and when the VM joins again its game has that NPC dead, killed as it joined
#   (killsOnLoad=, or its copy for the death list: copyKills=); raised by the host's console, it lives in both.
#   Put back after each way: the fight stopped, the NPC's confidence and aggression and the runner's player's health as
#   they were, the NPC's health restored, and the hitter's bounty for the hold paid (`paycrimegold`: hitting a person is
#   a crime there).
#
# Nothing moves by a script: the players stand where the npcs step left them, the NPC is the cell's own. The console is
# used for set-up and for the injected casts only, each said in the run.

COMBAT_SPELL=${COMBAT_SPELL:-0012FCD0}            # Skyrim.esm's Firebolt
COMBAT_CRIME_FACTION=${COMBAT_CRIME_FACTION:-000267EA}  # Skyrim.esm's CrimeFactionWhiterun (Riverwood's hold)
COMBAT_FIRST=${COMBAT_FIRST:-0.7}                 # the share of its health the runner's console leaves its NPC at first
COMBAT_LOW=${COMBAT_LOW:-0.1}                     # ... before it is killed by the hitter's casts
COMBAT_HP_TOL=${COMBAT_HP_TOL:-0.03}              # both games' health of the NPC agree within this share
COMBAT_CASTS=${COMBAT_CASTS:-8}                   # casts at most, for a hit and for the kill
COMBAT_REVIVE=${COMBAT_REVIVE:-5}                 # s: CopyHealth.h's REVIVE_AFTER
COMBAT_RESTORE=${COMBAT_RESTORE:-100000}          # the health given back to the NPC after (restoreav: up to its most)
COMBAT_FIGHT_HOLD=${COMBAT_FIGHT_HOLD:-11}         # s: NpcRules' FIGHT_HOLD and a second (a game claims an NPC that long after its last hit)
COMBAT_SETTLE=${COMBAT_SETTLE:-2}                 # s: after the last cast, before the console's change (CopyHealth's HIT_REACH and more)
COMBAT_SETBACK=${COMBAT_SETBACK:-0.2}             # the share of its most the hitter's console takes off its copy, then gives it
COMBAT_SETBACK_WAIT=${COMBAT_SETBACK_WAIT:-5}     # s: for the copy to be set back (CopyHealth's JUDGE_AFTER, and the dev channel's rounds)

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# combat_foes <runner rows> <watcher rows> <runner id> <x y z> [<kind first: creature|human>]: one line per NPC the
# hitter may fight, the best first: "<key> <its id in the runner's game> <its copy's id in the watcher's game> <kind>".
# Each one the runner runs (a load-order NPC, alive, in its world, no child) that the watcher shows as a mirrored copy,
# alive, in its world; the kind asked for first (a creature when none is), then the nearest to that place. Rows as
# npc_rows writes them (steps/npcs.sh).
combat_foes() {
	awk -v me="$3" -v p="$4" -v first="${5:-creature}" 'BEGIN { split(p, q, " ") } NF == 0 { next }
		FILENAME == ARGV[1] { if ($3 == "copy" && $5 != "-" && $15 == "0") copy[$1] = $14; next }
		$2 == me && $3 == "run" && $1 !~ /:/ && $15 == "0" && $16 != "1" && $5 != "-" && ($4 == "human" || $4 == "creature") && ($1 in copy) {
			d = sqrt(($5 - q[1]) ^ 2 + ($6 - q[2]) ^ 2) + ($4 == first ? 0 : 1e9)
			printf "%020.0f %s %s %s %s\n", d, $1, $14, copy[$1], $4
		}' <(printf '%s\n' "$2") <(printf '%s\n' "$1") | sort | cut -d' ' -f2-
}

# combat_damage_to <health> <most> <share>: how much to damage it by to leave it at that share of its most (0: it is
# there or below already)
combat_damage_to() { awk -v h="$1" -v m="$2" -v s="$3" 'BEGIN { d = h - s * m; printf "%.0f\n", (d > 0 ? d : 0) }'; }

# combat_close <a> <b> [<tolerance>]: 0 when two shares are within the tolerance (COMBAT_HP_TOL) of each other
combat_close() {
	[[ $1 =~ ^[0-9.]+$ ]] && [[ $2 =~ ^[0-9.]+$ ]] || return 1
	awk -v a="$1" -v b="$2" -v t="${3:-$COMBAT_HP_TOL}" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'
}

# combat_below <a> <b>: 0 when share a is below share b
combat_below() { [[ $1 =~ ^[0-9.]+$ ]] && [[ $2 =~ ^[0-9.]+$ ]] && awk -v a="$1" -v b="$2" 'BEGIN { exit !(a < b) }'; }

# combat_one <reply of `npcs <id>`> <name>: that field of the one NPC's line (the header's npcs= is not one)
combat_one() { field "$2" "${1#*|}"; }

# combat_pick <runner> <foes> <label>: the first of the foes (combat_foes) neither essential nor protected in the runner's
# game (a protected NPC dies only to its own game's player, never to a friend's hits); sets COMBAT_KEY, COMBAT_R (its id
# in the runner's game), COMBAT_W (its copy's in the other) and COMBAT_KIND. 1 when there is none.
combat_pick() {
	local k r w kd one ess prot
	COMBAT_KEY=""; COMBAT_R=""; COMBAT_W=""; COMBAT_KIND=""
	while read -r k r w kd; do
		[ -n "$k" ] || continue
		one=$(_combat_one "$1" "$r"); ess=$(combat_one "$one" essential); prot=$(combat_one "$one" protected)
		if [ "$ess" = 0 ] && [ "${prot:-0}" = 0 ]; then COMBAT_KEY=$k; COMBAT_R=$r; COMBAT_W=$w; COMBAT_KIND=$kd; return 0; fi
		say "$3: $k ($r) is essential or protected (essential=${ess:-?} protected=${prot:-?}): not this one"
	done <<<"$2"
	return 1
}

# ---- the step -----------------------------------------------------------------------------------------------------

_combat_one() { dc "$1" "npcs $2"; }
_combat_hp() { combat_one "$(_combat_one "$1" "$2")" hp; }
# _combat_follows <hitter> <copy id> <runner> <NPC id> [<below>]: both games show the NPC at the same health (and, given
# <below>, the runner's is under it)
_combat_follows() {
	local r w; r=$(_combat_hp "$3" "$4"); w=$(_combat_hp "$1" "$2")
	combat_close "$r" "$w" || return 1
	[ -z "${5:-}" ] || combat_below "$r" "$5"
}
_combat_count() { field "$2" "$(dc "$1" npcs)"; }
_combat_up() { local n; n=$(_combat_count "$1" "$2"); is_num "$n" && [ "$n" -gt "$3" ]; }
# _combat_dead <who> <id> <0|1>: that game's NPC (or copy) is dead (1) or alive (0), and the session has it so
_combat_dead() { local o; o=$(_combat_one "$1" "$2"); [ "$(combat_one "$o" dead)" = "$3" ] && [ "$(combat_one "$o" sessionDead)" = "$3" ]; }
_combat_both() { _combat_dead "$1" "$2" "$5" && _combat_dead "$3" "$4" "$5"; }

# combat_bring <runner> <NPC id> <share>: the runner's console leaves its NPC at that share of its health (set-up)
combat_bring() {
	local o h m dmg
	o=$(_combat_one "$1" "$2"); h=$(combat_one "$o" health); m=$(combat_one "$o" max)
	is_num "$h" && is_num "$m" || return 1
	dmg=$(combat_damage_to "$h" "$m" "$3")
	[ "$dmg" = 0 ] && return 0
	[ "$(dc "$1" "console $2.damageav health $dmg")" = ran ]
}

# combat_cast <hitter> <copy id> <until command...>: casts at the copy, a second and a half apart, until the command
# succeeds or COMBAT_CASTS were cast; 0 when it succeeded
combat_cast() {
	local who=$1 id=$2 i; shift 2
	for ((i = 0; i < COMBAT_CASTS; i++)); do
		dc "$who" "console player.cast $COMBAT_SPELL $id" >/dev/null
		sleep 1; "$@" && return 0; sleep 1
		"$@" && return 0
	done
	return 1
}

# combat_setback <runner> <NPC id> <hitter> <copy id> <label>: the core rule (3b above). The hitter's console changes its
# copy's health with no hit, down (damageav) then up (restoreav), each COMBAT_SETBACK of its most; each time the copy is
# back where it was within COMBAT_SETBACK_WAIT, the hitter's givenBack= (down) or takenBack= (up) counting it.
combat_setback() {
	local runner=$1 rlocal=$2 hitter=$3 wlocal=$4 label=$5 what cmd counter o m h0 c0 n
	say "$label: waiting $COMBAT_SETTLE s: the last cast's hit event out of reach, so the next change is no hit of ours"
	sleep "$COMBAT_SETTLE"
	for what in drop gain; do
		if [ "$what" = drop ]; then cmd=damageav; counter=givenBack; else cmd=restoreav; counter=takenBack; fi
		o=$(_combat_one "$hitter" "$wlocal"); m=$(combat_one "$o" max); h0=$(combat_one "$o" hp); c0=$(_combat_count "$hitter" "$counter")
		if ! is_num "$m" || ! is_num "$h0" || ! is_num "$c0"; then
			rig_broken game "$label: a $what no hit of ours explains is set back" "no max=, hp= or $counter= in the $hitter's replies: $(cut -c1-200 <<<"$o")"; return
		fi
		n=$(awk -v m="$m" -v s="$COMBAT_SETBACK" 'BEGIN { printf "%.0f\n", m * s }')
		if [ "$(dc "$hitter" "console $wlocal.$cmd health $n")" != ran ]; then
			rig_broken input "$label: a $what no hit of ours explains is set back" "the $hitter's console refused $cmd"; return
		fi
		say "$label: set-up (console): the $hitter's copy $wlocal: $cmd health $n (no hit: as another NPC's blow, a trap or a heal no runner said)"
		if wait_until "$COMBAT_SETBACK_WAIT" _combat_back "$hitter" "$wlocal" "$h0" "$counter" "$c0"; then
			pass "$label: a $what no hit of ours explains is set back to the runner's word" "hp $h0, $cmd $n, back to $(_combat_hp "$hitter" "$wlocal"); $counter $c0 -> $(_combat_count "$hitter" "$counter")"
		else
			fail "$label: a $what no hit of ours explains is set back to the runner's word" "after $COMBAT_SETBACK_WAIT s: hp $h0, $cmd $n, now $(_combat_hp "$hitter" "$wlocal"); $counter $c0 -> $(_combat_count "$hitter" "$counter"); the $runner's hp $(_combat_hp "$runner" "$rlocal")"
		fi
	done
}
# _combat_back <hitter> <copy id> <hp before> <counter> <its count before>: the copy is back at that hp, the count up
_combat_back() { _combat_up "$1" "$4" "$5" && combat_close "$(_combat_hp "$1" "$2")" "$3"; }

# _combat_fighting <runner> <NPC id>: the runner's NPC is in combat or has its weapon out
_combat_fighting() { local o; o=$(_combat_one "$1" "$2"); [ "$(combat_one "$o" combat)" = 1 ] || [ "$(combat_one "$o" weapon)" = 1 ]; }

# combat_engage <runner> <NPC id> <label>: the fight's set-up. The runner's console sets its NPC on its player (as the
# npcs step's fight does: confidence and aggression so it stands and fights, the player given NPC_FIGHT_HEALTH health)
# and waits until it fights. While the runner's player fights it, the hitter's hits never take it over (NpcBook: a fight
# claim loses to the runner's own fight), so the hits are a friend's on an NPC another game runs, the case this step is
# for. 1 (said: RIG) when the console refused or no fight came; npccombat_putback puts it all back.
COMBAT_ENGAGED=0; COMBAT_CONF0=""; COMBAT_AGGR0=""
combat_engage() {
	local who=$1 id=$2 label=$3 one
	one=$(_combat_one "$who" "$id"); COMBAT_CONF0=$(combat_one "$one" confidence); COMBAT_AGGR0=$(combat_one "$one" aggression)
	COMBAT_ENGAGED=1
	if is_num "$COMBAT_CONF0" && is_num "$COMBAT_AGGR0"; then
		dc "$who" "console $id.setav confidence $NPC_FIGHT_CONFIDENCE" >/dev/null; dc "$who" "console $id.setav aggression $NPC_FIGHT_AGGRESSION" >/dev/null
	else
		COMBAT_CONF0=""; COMBAT_AGGR0=""
	fi
	if [ "$(dc "$who" "console player.modav health $NPC_FIGHT_HEALTH")" != ran ] || [ "$(dc "$who" "console $id.startcombat player")" != ran ]; then
		rig_broken input "$label: the fight's set-up" "the $who's console refused modav or startcombat"; return 1
	fi
	say "$label: set-up (console): the $who's player +$NPC_FIGHT_HEALTH health; $id${COMBAT_CONF0:+ confidence $COMBAT_CONF0 -> $NPC_FIGHT_CONFIDENCE, aggression $COMBAT_AGGR0 -> $NPC_FIGHT_AGGRESSION,} set on it with startcombat (so its runner keeps it while the other player hits it)"
	if ! wait_until "$NPC_FIGHT_WAIT" _combat_fighting "$who" "$id"; then
		rig_fault state "$label: the fight's set-up" "$id never entered combat with the $who's player in $NPC_FIGHT_WAIT s: $(_combat_one "$who" "$id" | cut -c1-200)"; return 1
	fi
}

# npccombat_way <runner> <hitter> [<kind: creature|human>]: one NPC the runner runs (of that kind if it has one), its copy
# hit and killed by the hitter's player
npccombat_way() {
	local runner=$1 hitter=$2 want=${3:-creature} label="npccombat: the $2 hits the $1's ${3:-creature}" rid hid foes key rlocal wlocal kind hp0 w0 sent0 applied0 in0 kills0
	section "$label"
	rid=$(field player "$(dc "$runner" 'session status')"); hid=$(field player "$(dc "$hitter" 'session status')")
	if ! is_num "$rid" || ! is_num "$hid"; then rig_broken game "$label" "no player ids (runner '$rid', hitter '$hid')"; return; fi
	foes=$(combat_foes "$(_npcs_rows "$runner")" "$(_npcs_rows "$hitter")" "$rid" "$(pos_of "$runner")" "$want")
	if ! combat_pick "$runner" "$foes" "$label"; then
		warn "$label: an NPC to fight" "the $runner runs no living grown person or creature, not essential or protected, that the $hitter shows as a copy: nothing to fight this way"
		return
	fi
	key=$COMBAT_KEY; rlocal=$COMBAT_R; wlocal=$COMBAT_W; kind=$COMBAT_KIND
	if [ "$kind" != "$want" ]; then
		warn "$label: a $want to fight" "the $runner runs no $want (alive, grown, not essential or protected) that the $hitter shows as a copy: a $kind instead, so a $want's path is not covered this way"
	fi
	say "$label: the NPC: $key, a $kind ($rlocal in the $runner's game, its copy $wlocal in the $hitter's)"
	if ! combat_engage "$runner" "$rlocal" "$label"; then npccombat_putback "$runner" "$hitter" "$rlocal"; return; fi

	# 2. the runner's word
	if ! combat_bring "$runner" "$rlocal" "$COMBAT_FIRST"; then
		rig_broken input "$label: set-up" "the $runner's console could not bring $rlocal to $COMBAT_FIRST of its health: $(_combat_one "$runner" "$rlocal" | cut -c1-200)"
		npccombat_putback "$runner" "$hitter" "$rlocal"; return
	fi
	say "$label: set-up (console): the $runner's $rlocal brought to $COMBAT_FIRST of its health (damageav)"
	if wait_until "$NPC_WAIT" _combat_follows "$hitter" "$wlocal" "$runner" "$rlocal" 0.9; then
		pass "$label: the copy shows its runner's word" "hp $(_combat_hp "$runner" "$rlocal") there, $(_combat_hp "$hitter" "$wlocal") here"
	else
		fail "$label: the copy shows its runner's word" "after $NPC_WAIT s: the $runner's $(_combat_one "$runner" "$rlocal" | cut -c1-200) / the $hitter's $(_combat_one "$hitter" "$wlocal" | cut -c1-240)"
	fi

	# 3. the hitter's hit
	hp0=$(_combat_hp "$runner" "$rlocal"); w0=$(_combat_hp "$hitter" "$wlocal")
	sent0=$(_combat_count "$hitter" hitsSent); applied0=$(_combat_count "$hitter" hitsApplied); in0=$(_combat_count "$runner" hitsInApplied)
	if ! is_num "$sent0" || ! is_num "$applied0" || ! is_num "$in0"; then rig_broken game "$label" "no hit counts in the npcs replies"; return; fi
	say "$label: the $hitter's player casts Firebolt ($COMBAT_SPELL) at the copy (console: an injected attack)"
	if combat_cast "$hitter" "$wlocal" _combat_up "$hitter" hitsSent "$sent0"; then
		pass "$label: the hit on the copy is sent" "hitsSent $sent0 -> $(_combat_count "$hitter" hitsSent)"
		if wait_until "$NPC_WAIT" _combat_up "$hitter" hitsApplied "$applied0" && _combat_up "$runner" hitsInApplied "$in0"; then
			pass "$label: the runner applied it" "the $hitter's hitsApplied $applied0 -> $(_combat_count "$hitter" hitsApplied), the $runner's hitsInApplied $in0 -> $(_combat_count "$runner" hitsInApplied)"
		else
			fail "$label: the runner applied it" "the $hitter: $(dc "$hitter" npcs | grep -o 'hits[A-Za-z]*=[0-9]*' | tr '\n' ' '); the $runner: $(dc "$runner" npcs | grep -o 'hitsIn[A-Za-z]*=[0-9]*' | tr '\n' ' ')"
		fi
		if wait_until "$NPC_WAIT" _combat_follows "$hitter" "$wlocal" "$runner" "$rlocal" "$hp0"; then
			pass "$label: both show the NPC at the same health after it" "hp $hp0 -> $(_combat_hp "$runner" "$rlocal") there, $(_combat_hp "$hitter" "$wlocal") here"
		else
			fail "$label: both show the NPC at the same health after it" "the $runner's hp $hp0 -> $(_combat_hp "$runner" "$rlocal"), the $hitter's copy $(_combat_one "$hitter" "$wlocal" | grep -o 'hp=[^ ]* word=[^ ]* inFlight=[^ ]*')"
		fi
		if [ "$(combat_one "$(_combat_one "$runner" "$rlocal")" owner)" = "$rid" ]; then
			pass "$label: the $runner still runs it (its player fights it: the hits took nothing over)" ""
		else
			fail "$label: the $runner still runs it (its player fights it: the hits took nothing over)" "the $runner's $(_combat_one "$runner" "$rlocal" | cut -c1-200)"
			npccombat_putback "$runner" "$hitter" "$rlocal"; return
		fi
		# 3b. what no hit of ours explains is set back
		combat_setback "$runner" "$rlocal" "$hitter" "$wlocal" "$label"
	elif combat_below "$(_combat_hp "$hitter" "$wlocal")" "$w0"; then
		fail "$label: the hit on the copy is sent" "the copy's hp $w0 -> $(_combat_hp "$hitter" "$wlocal") in the $hitter's game, but no hit went (hitsSent $sent0): the runner never hears of it"
	else
		rig_fault state "$label: the hit on the copy" "$COMBAT_CASTS casts at $wlocal never hit it (hp $w0, hitsSent $sent0): the spell, the line of sight, or the distance"
		npccombat_putback "$runner" "$hitter" "$rlocal"; return
	fi

	# 4. the kill
	if combat_bring "$runner" "$rlocal" "$COMBAT_LOW"; then
		say "$label: set-up (console): the $runner's $rlocal brought to $COMBAT_LOW of its health"
		wait_until "$NPC_WAIT" _combat_follows "$hitter" "$wlocal" "$runner" "$rlocal" >/dev/null
	fi
	if combat_cast "$hitter" "$wlocal" _combat_dead "$runner" "$rlocal" 1; then
		pass "$label: the $hitter's hits kill the $runner's NPC" "$(_combat_one "$runner" "$rlocal" | grep -o 'dead=[01]\|sessionDead=[01] killer=[0-9]*' | tr '\n' ' ')"
		if wait_until "$NPC_WAIT" _combat_dead "$hitter" "$wlocal" 1; then
			pass "$label: the copy is dead in the $hitter's game too, and the session has it dead" "$(_combat_one "$hitter" "$wlocal" | grep -o 'dead=[01]\|sessionDead=[01] killer=[0-9]*' | tr '\n' ' ')"
		else
			fail "$label: the copy is dead in the $hitter's game too, and the session has it dead" "$(_combat_one "$hitter" "$wlocal" | cut -c1-240)"
		fi
		expect_eq "$label: the $runner's game names the $hitter's player its killer" "$(combat_one "$(_combat_one "$runner" "$rlocal")" killer)" "$hid"
		expect_eq "$label: the $hitter's game names its player the killer" "$(combat_one "$(_combat_one "$hitter" "$wlocal")" killer)" "$hid"
	else
		fail "$label: the $hitter's hits kill the $runner's NPC" "$COMBAT_CASTS casts: the $runner's $(_combat_one "$runner" "$rlocal" | cut -c1-200) / the $hitter's $(_combat_one "$hitter" "$wlocal" | grep -o 'hp=[^ ]* word=[^ ]* inFlight=[^ ]*')"
		npccombat_putback "$runner" "$hitter" "$rlocal"; return
	fi

	# 5. raised by its runner
	say "$label: waiting $COMBAT_FIGHT_HOLD s: the $hitter's game holds its fight with the NPC that long after its last hit"
	sleep "$COMBAT_FIGHT_HOLD"
	dc "$runner" "console $rlocal.resurrect 1" >/dev/null
	say "$label: set-up (console): the $runner raises $rlocal (resurrect)"
	if wait_until $((NPC_WAIT + COMBAT_REVIVE)) _combat_both "$runner" "$rlocal" "$hitter" "$wlocal" 0; then
		pass "$label: raised by its runner, it lives in both games" "the $hitter's copy: $(_combat_one "$hitter" "$wlocal" | grep -o 'dead=[01]\|hp=[^ ]*' | tr '\n' ' ')"
	else
		fail "$label: raised by its runner, it lives in both games" "the $runner's $(_combat_one "$runner" "$rlocal" | grep -o 'dead=[01]\|sessionDead=[01]' | tr '\n' ' ')/ the $hitter's $(_combat_one "$hitter" "$wlocal" | grep -o 'dead=[01]\|sessionDead=[01]\|word=[^ ]*\|runnerDead=[01]' | tr '\n' ' ')"
		npccombat_putback "$runner" "$hitter" "$rlocal"; return
	fi

	# 6. the runner's own kill reaches the copy
	kills0=$(_combat_count "$hitter" copyKills); is_num "$kills0" || kills0=0
	dc "$runner" "console $rlocal.kill" >/dev/null
	say "$label: set-up (console): the $runner kills $rlocal (kill)"
	if wait_until "$NPC_WAIT" _combat_dead "$hitter" "$wlocal" 1; then
		pass "$label: its runner's kill kills the copy" "copyKills $kills0 -> $(_combat_count "$hitter" copyKills) in the $hitter's game"
		expect_eq "$label: ... no player's kill" "$(combat_one "$(_combat_one "$hitter" "$wlocal")" killer)" 0
	else
		fail "$label: its runner's kill kills the copy" "after $NPC_WAIT s: the $hitter's $(_combat_one "$hitter" "$wlocal" | cut -c1-240)"
	fi
	dc "$runner" "console $rlocal.resurrect 1" >/dev/null
	if wait_until $((NPC_WAIT + COMBAT_REVIVE)) _combat_both "$runner" "$rlocal" "$hitter" "$wlocal" 0; then
		pass "$label: raised again, it lives in both games" ""
	else
		fail "$label: raised again, it lives in both games" "the $hitter's $(_combat_one "$hitter" "$wlocal" | cut -c1-240)"
	fi
	npccombat_putback "$runner" "$hitter" "$rlocal"
}

# npccombat_putback <runner> <hitter> <NPC id>: the NPC alive at all its health, the hitter's bounty in the hold paid
npccombat_putback() {
	local o; o=$(_combat_one "$1" "$3")
	[ "$(combat_one "$o" dead)" = 1 ] && dc "$1" "console $3.resurrect 1" >/dev/null
	if [ "$COMBAT_ENGAGED" = 1 ]; then
		dc "$1" "console $3.stopcombat" >/dev/null
		[ -n "$COMBAT_CONF0" ] && { dc "$1" "console $3.setav confidence $COMBAT_CONF0" >/dev/null; dc "$1" "console $3.setav aggression $COMBAT_AGGR0" >/dev/null; }
		dc "$1" "console player.modav health -$NPC_FIGHT_HEALTH" >/dev/null
		COMBAT_ENGAGED=0
		say "npccombat: put back (console): the $1's $3 out of the fight${COMBAT_CONF0:+ (confidence $COMBAT_CONF0, aggression $COMBAT_AGGR0)}, the $1's player's health"
	fi
	dc "$1" "console $3.restoreav health $COMBAT_RESTORE" >/dev/null
	dc "$2" "console player.paycrimegold 0 0 $COMBAT_CRIME_FACTION" >/dev/null
	say "npccombat: put back (console): the $1's $3 alive at all its health, the $2's bounty in the hold paid"
}

# npccombat_rejoin: the session's dead reach a game that joins (the joiner's full death list, killsOnLoad). An NPC the
# host runs that the VM shows as a copy is killed by the host's console (set-up) while the VM is out of the session;
# when the VM joins again its game has it dead, killed as it joined (killsOnLoad= up, or its copy killed for the death
# list: copyKills=). Raised by the host's console, it lives in both. Nobody moves.
npccombat_rejoin() {
	local label="npccombat: an NPC killed while the VM is out is dead in its game when it joins again" rid foes r k0 rlocal wlocal
	section "$label"
	rid=$(field player "$(dc host 'session status')")
	if ! is_num "$rid"; then rig_broken game "$label" "no player id for the host"; return; fi
	foes=$(combat_foes "$(_npcs_rows host)" "$(_npcs_rows vm)" "$rid" "$(pos_of host)" creature)
	if ! combat_pick host "$foes" "$label"; then
		warn "$label: an NPC" "the host runs no living grown NPC, not essential or protected, that the VM shows as a copy: nothing to kill while the VM is out"
		return
	fi
	rlocal=$COMBAT_R; wlocal=$COMBAT_W
	say "$label: the NPC: $COMBAT_KEY, a $COMBAT_KIND ($rlocal in the host's game, $wlocal in the VM's)"
	k0=$(_combat_count vm killsOnLoad); is_num "$k0" || k0=0
	r=$(dc vm 'session leave'); if [[ $r != left* ]]; then fail "$label: the VM leaves" "$r"; return; fi
	if ! wait_until 30 _combat_alone; then
		fail "$label: the VM leaves" "the host still counts $(field players "$(dc host 'session status')") player(s) after 30 s"
		_combat_rejoin_vm "$label"; return
	fi
	dc host "console $rlocal.kill" >/dev/null
	say "$label: set-up (console): the host kills $rlocal (kill) while the VM is out of the session"
	if wait_until "$NPC_WAIT" _combat_dead host "$rlocal" 1; then
		pass "$label: the host's game has it dead for the session" "$(_combat_one host "$rlocal" | grep -o 'dead=[01]\|sessionDead=[01]' | tr '\n' ' ')"
	else
		fail "$label: the host's game has it dead for the session" "after $NPC_WAIT s: $(_combat_one host "$rlocal" | cut -c1-200)"
	fi
	_combat_rejoin_vm "$label" || return
	if wait_until "$NPC_WAIT" _combat_dead vm "$wlocal" 1 && wait_until 5 _combat_joined_kill "$k0"; then
		pass "$label: the VM's game kills it as it joins (the session's death list)" "killsOnLoad $k0 -> $(_combat_count vm killsOnLoad), copyKills $(_combat_count vm copyKills)"
	else
		fail "$label: the VM's game kills it as it joins (the session's death list)" "after $NPC_WAIT s: the VM's $(_combat_one vm "$wlocal" | cut -c1-240); killsOnLoad $k0 -> $(_combat_count vm killsOnLoad), copyKills $(_combat_count vm copyKills)"
	fi
	dc host "console $rlocal.resurrect 1" >/dev/null
	say "$label: set-up (console): the host raises $rlocal (resurrect)"
	if wait_until $((NPC_WAIT + COMBAT_REVIVE)) _combat_both host "$rlocal" vm "$wlocal" 0; then
		pass "$label: raised by the host, it lives in both games" ""
	else
		fail "$label: raised by the host, it lives in both games" "the host's $(_combat_one host "$rlocal" | grep -o 'dead=[01]\|sessionDead=[01]' | tr '\n' ' ')/ the VM's $(_combat_one vm "$wlocal" | grep -o 'dead=[01]\|sessionDead=[01]\|word=[^ ]*\|runnerDead=[01]' | tr '\n' ' ')"
	fi
	dc host "console $rlocal.restoreav health $COMBAT_RESTORE" >/dev/null
	say "npccombat: put back (console): the host's $rlocal alive at all its health"
}
_combat_alone() { [ "$(field players "$(dc host 'session status')")" = 0 ]; }
_combat_joined_kill() { _combat_up vm killsOnLoad "$1" || _combat_up vm copyKills 0; }
# _combat_rejoin_vm <label>: the VM joins the host's session again (as save_rejoin does); 1 (a FAIL) when it does not
_combat_rejoin_vm() {
	local r
	if r=$(vm_join) && wait_until 90 joined_both; then pass "$1: the VM joins again" "$(dc vm 'session status' | cut -c1-80)"; return 0; fi
	fail "$1: the VM joins again" "${r:-not both in the session after 90 s}"
	return 1
}

step_npccombat() {
	local w s
	for w in host vm; do still $w; done
	npccombat_way host vm creature
	npccombat_way vm host human
	section "npccombat: hits, both games"
	for w in host vm; do
		s=$(dc $w npcs)
		expect_zero "$w: no hit on a copy lost (never answered)" "$(field hitsLost "$s")"
		say "npccombat: $w: hits sent $(field hitsSent "$s"), applied $(field hitsApplied "$s"), refused $(field hitsRefused "$s"); friends' hits in $(field hitsIn "$s"), applied $(field hitsInApplied "$s"); set back $(field givenBack "$s") down / $(field takenBack "$s") up; deaths sent $(field deathsSent "$s"), heard $(field deathsIn "$s"); copies killed $(field copyKills "$s"), raised $(field revived "$s"), killed as they loaded $(field killsOnLoad "$s")"
	done
	npccombat_rejoin
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init combattest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# rows in the format NpcSync::Describe writes (npc_rows reads them)
	RUN_LIST='npcs=4 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(100,200,0) dead=0 child=0 weapon=0 combat=0 hp=1.000 sessionDead=0 | 0001A690 local=0001A690 owner=1 role=run kind=creature at=(900,200,0) dead=0 child=0 weapon=0 combat=0 hp=1.000 sessionDead=0 | 0001A6A0 local=0001A6A0 owner=1 role=run kind=human at=(110,200,0) dead=0 child=1 weapon=0 combat=0 hp=1.000 sessionDead=0 | 0001A6B0 local=0001A6B0 owner=2 role=copy kind=human at=(50,50,0) dead=0 child=0 weapon=0 combat=0 hp=1.000 word=1.000 inFlight=0 runnerDead=0 sessionDead=0 runner=2 mode=mirror'
	SEE_LIST='npcs=4 | 0001A67C local=0001A67C owner=1 role=copy kind=human at=(101,200,0) dead=0 child=0 weapon=0 combat=0 hp=1.000 word=1.000 inFlight=0 runnerDead=0 sessionDead=0 runner=1 mode=mirror | 0001A690 local=0001A690 owner=1 role=copy kind=creature at=(901,200,0) dead=0 child=0 weapon=0 combat=0 hp=1.000 word=1.000 inFlight=0 runnerDead=0 sessionDead=0 runner=1 mode=mirror | 0001A6A0 local=0001A6A0 owner=1 role=copy kind=human at=(111,200,0) dead=0 child=1 weapon=0 combat=0 | 0001A6B0 local=0001A6B0 owner=2 role=run kind=human at=(50,50,0) dead=0 child=0 weapon=0 combat=0 hp=1.000 sessionDead=0'
	R=$(npc_rows "$RUN_LIST"); W=$(npc_rows "$SEE_LIST")
	t "foes: a creature before a person, never a child, never one the runner does not run" "$(combat_foes "$R" "$W" 1 "100 200 0" | tr '\n' ';')" "0001A690 0001A690 0001A690 creature;0001A67C 0001A67C 0001A67C human;"
	t "foes: the other way" "$(combat_foes "$W" "$R" 2 "0 0 0")" "0001A6B0 0001A6B0 0001A6B0 human"
	t "foes: none the watcher shows as a copy" "$(combat_foes "$R" "$(sed 's/ copy / other /' <<<"$W")" 1 "100 200 0")" ""
	t "foes: a copy dead in the watcher's game is none" "$(combat_foes "$R" "$(sed 's/^\(0001A690 .*\) 0001A690 0 /\1 0001A690 1 /' <<<"$W")" 1 "100 200 0" | head -1 | cut -d' ' -f1)" 0001A67C
	t "foes: a person first when a person is asked for" "$(combat_foes "$R" "$W" 1 "100 200 0" human | tr '\n' ';')" "0001A67C 0001A67C 0001A67C human;0001A690 0001A690 0001A690 creature;"
	t "foes: the nearest person first" "$(combat_foes "$(printf '%s\n%s\n' '00000A02 1 run human 900 0 0 - - - - - - 00000A02 0 0' '00000A03 1 run human 300 0 0 - - - - - - 00000A03 0 0')" "$(printf '%s\n%s\n' '00000A02 1 copy human 900 0 0 - - - - - - 0000BA02 0' '00000A03 1 copy human 300 0 0 - - - - - - 0000BA03 0')" 1 "0 0 0" | tr '\n' ';')" "00000A03 00000A03 0000BA03 human;00000A02 00000A02 0000BA02 human;"
	t "damage to: 70 % of 200 from 180" "$(combat_damage_to 180 200 0.7)" 40
	t "damage to: there already" "$(combat_damage_to 100 200 0.7)" 0
	t "close: within" "$(combat_close 0.70 0.72 && echo yes)" yes
	t "close: not" "$(combat_close 0.70 0.75 || echo no)" no
	t "close: no number" "$(combat_close none 0.75 || echo no)" no
	t "below" "$(combat_below 0.5 0.7 && echo yes)" yes
	t "below: not" "$(combat_below 0.7 0.7 || echo no)" no
	ONE='npcs=1 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(100,200,0) dead=1 child=0 weapon=0 combat=0 hp=0.000 sessionDead=1 killer=2 confidence=0 aggression=0 health=0 max=180 essential=0'
	t "one: a field of the NPC's line" "$(combat_one "$ONE" killer)" 2
	t "one: dead is not sessionDead" "$(combat_one "${ONE/dead=1/dead=0}" dead)" 0
	t "one: the header is not the NPC's" "$(combat_one 'npcs=1 | 0001A67C local=0001A67C' npcs)" ""

	# the step, both games faked: the runner's NPC and the hitter's copy, each cast taking 0.1 off both, the kill and the
	# raise as the plugin does them. FK_MODE: ok (all works), nosend (the copy takes hits, none is sent), miss (no cast
	# hits), noapply (sent, never applied), nokill (the copy dies, the runner's NPC does not), essential (every foe is),
	# nofight (startcombat never starts a fight), taken (the hits take the NPC over).
	fake_fight() {
		FK=$d/fk.$RANDOM; mkdir -p "$FK"; echo 1.000 > "$FK/rhp"; echo 1.000 > "$FK/whp"; echo 0 > "$FK/dead"; echo 0 > "$FK/killer"
		echo 0 > "$FK/sent"; echo 0 > "$FK/applied"; echo 0 > "$FK/in"; echo 0 > "$FK/kills"; echo 0 > "$FK/combat"; echo 1 > "$FK/owner"; FK_MODE=$1
		echo 0 > "$FK/gb"; echo 0 > "$FK/tb"
		fk_line() {  # fk_line <who> <id>: the one NPC's line, in that game
			local hp dead killer ess=0; [ "$FK_MODE" = essential ] && ess=1
			dead=$(cat "$FK/dead"); killer=$(cat "$FK/killer")
			if [ "$1" = host ]; then hp=$(cat "$FK/rhp"); else hp=$(cat "$FK/whp"); fi
			echo "npcs=1 | 0001A690 local=$2 owner=$(cat "$FK/owner") role=$([ "$1" = host ] && echo run || echo copy) kind=creature at=(900,200,0) dead=$dead child=0 weapon=0 combat=$(cat "$FK/combat") hp=$hp sessionDead=$dead killer=$killer confidence=0 aggression=0 health=$(awk -v h="$hp" 'BEGIN { printf "%.0f", h * 200 }') max=200 essential=$ess"
		}
		fk_set() { echo "$2" > "$FK/$1"; }
		fk_sub() { awk -v a="$(cat "$FK/$1")" -v b="$2" 'BEGIN { v = a - b; if (v < 0) v = 0; printf "%.3f\n", v }' > "$FK/$1.n"; mv -f "$FK/$1.n" "$FK/$1"; }
		dc() { case "$2" in
			'npcs list') if [ "$1" = host ]; then echo "$RUN_LIST"; else echo "$SEE_LIST"; fi ;;
			'npcs '*) fk_line "$1" "${2#npcs }" ;;
			npcs) if [ "$1" = host ]; then echo "hitsInApplied=$(cat "$FK/in") hitsLost=0"; else echo "hitsSent=$(cat "$FK/sent") hitsApplied=$(cat "$FK/applied") copyKills=$(cat "$FK/kills") hitsLost=0 givenBack=$(cat "$FK/gb") takenBack=$(cat "$FK/tb")"; fi ;;
			'session status') if [ "$1" = host ]; then echo 'player=1'; else echo 'player=2'; fi ;;
			where) echo 'cell=00000000 world=0000003C pos=(100,200,0) modal=0' ;;
			# the hitter's console on its copy: set back, and counted (as the plugin does), unless the mode keeps it
			*damageav*) if [ "$1" = vm ]; then
					if [ "$FK_MODE" = keepdrop ]; then fk_sub whp "$(awk -v d="${2##* }" 'BEGIN { printf "%.3f", d / 200 }')"; else fk_set gb $(($(cat "$FK/gb") + 1)); fi
					echo ran; return
				fi
				local h; h=$(awk -v d="${2##* }" -v r="$(cat "$FK/rhp")" 'BEGIN { printf "%.3f", r - d / 200 }'); fk_set rhp "$h"; fk_set whp "$h"; echo ran ;;
			*player.cast*)
				[ "$FK_MODE" = miss ] && { echo ran; return; }
				[ "$FK_MODE" = taken ] && fk_set owner 2
				fk_sub whp 0.1
				[ "$FK_MODE" = nosend ] && { echo ran; return; }
				fk_set sent $(($(cat "$FK/sent") + 1))
				[ "$FK_MODE" = noapply ] && { echo ran; return; }
				fk_sub rhp 0.1; fk_set applied $(($(cat "$FK/applied") + 1)); fk_set in $(($(cat "$FK/in") + 1))
				if [ "$(cat "$FK/rhp")" = 0.000 ] && [ "$FK_MODE" != nokill ]; then fk_set dead 1; fk_set killer 2; fi ;;
			*startcombat*) [ "$FK_MODE" = nofight ] || fk_set combat 1; echo ran ;;
			*stopcombat*) fk_set combat 0; echo ran ;;
			*restoreav*) if [ "$1" = vm ]; then
					if [ "$FK_MODE" = keepgain ]; then fk_sub whp "-$(awk -v d="${2##* }" 'BEGIN { printf "%.3f", d / 200 }')"; else fk_set tb $(($(cat "$FK/tb") + 1)); fi
				fi
				echo ran ;;
			*resurrect*) fk_set dead 0; fk_set killer 0; fk_set rhp 1.000; fk_set whp 1.000; echo ran ;;
			*.kill) fk_set dead 1; fk_set killer 0; fk_set kills $(($(cat "$FK/kills") + 1)); echo ran ;;
			console*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fk$RANDOM; mkdir -p "$A_OUT"; a_init fk >/dev/null; npccombat_way host vm >/dev/null; cut -f1 "$A_RESULTS" | tr '\n' ' ' )
	}
	t "way: all of it works" "$(fake_fight ok)" "PASS PASS PASS PASS PASS PASS PASS PASS PASS PASS PASS PASS PASS PASS PASS "
	t "way: a copy that takes the hit and sends none fails" "$(fake_fight nosend)" "PASS FAIL FAIL "
	t "way: a cast that never hits is the scene's (RIG), not the mod's" "$(fake_fight miss)" "PASS RIG "
	t "way: a hit never applied fails" "$(fake_fight noapply)" "PASS PASS FAIL FAIL PASS PASS PASS FAIL "
	t "way: a runner's NPC that never dies of the hits fails" "$(fake_fight nokill)" "PASS PASS PASS PASS PASS PASS PASS FAIL "
	t "way: a copy that keeps a drop no hit explains fails (a double blow)" "$(fake_fight keepdrop)" "PASS PASS PASS PASS PASS FAIL PASS PASS PASS PASS PASS PASS PASS PASS PASS "
	t "way: a copy that keeps a gain no runner said fails (heal on hit)" "$(fake_fight keepgain)" "PASS PASS PASS PASS PASS PASS FAIL PASS PASS PASS PASS PASS PASS PASS PASS "
	t "way: only essential NPCs is a WARN" "$(fake_fight essential)" "WARN "
	t "way: an NPC that never fights its runner's player is the scene's (RIG)" "$(fake_fight nofight)" "RIG "
	t "way: hits that take the NPC over from its fighting runner fail" "$(fake_fight taken)" "PASS PASS PASS PASS FAIL "

	# the session's dead reach a game that joins: the VM out, the host kills, the VM joins and has it dead. FR_MODE: ok, or
	# noload (the VM's game keeps it alive).
	fake_rejoin() {
		FR=$d/fr.$RANDOM; mkdir -p "$FR"; echo 0 > "$FR/hdead"; echo 0 > "$FR/vdead"; echo 1 > "$FR/vin"; echo 3 > "$FR/kol"; FR_MODE=$1
		dc() { local dd vin; case "$2" in
			'npcs list') if [ "$1" = host ]; then echo "$RUN_LIST"; else echo "$SEE_LIST"; fi ;;
			'npcs '*) if [ "$1" = host ]; then dd=$(cat "$FR/hdead"); else dd=$(cat "$FR/vdead"); fi
				echo "npcs=1 | 0001A690 local=${2#npcs } owner=1 role=$([ "$1" = host ] && echo run || echo copy) kind=creature at=(900,200,0) dead=$dd child=0 weapon=0 combat=0 hp=1.000 sessionDead=$dd killer=0 health=200 max=200 essential=0 protected=0" ;;
			npcs) echo "killsOnLoad=$(cat "$FR/kol") copyKills=0 hitsLost=0" ;;
			'session status') vin=$(cat "$FR/vin"); if [ "$1" = host ]; then echo "player=1 players=$vin welcomed=1 connection=up"; else echo "player=2 welcomed=$vin connection=$([ "$vin" = 1 ] && echo up || echo down)"; fi ;;
			'session leave') echo 0 > "$FR/vin"; echo left ;;
			status) echo 'lobby=5' ;;
			'steam join '*) echo 1 > "$FR/vin"; if [ "$FR_MODE" = ok ] && [ "$(cat "$FR/hdead")" = 1 ]; then echo 1 > "$FR/vdead"; echo 4 > "$FR/kol"; fi; echo joining ;;
			where) echo 'cell=00000000 world=0000003C pos=(100,200,0) modal=0' ;;
			*.kill) echo 1 > "$FR/hdead"; echo ran ;;
			*resurrect*) echo 0 > "$FR/hdead"; echo 0 > "$FR/vdead"; echo ran ;;
			console*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fr$RANDOM; mkdir -p "$A_OUT"; a_init fr >/dev/null; JOIN=dev; npccombat_rejoin >/dev/null; cut -f1 "$A_RESULTS" | tr '\n' ' ' )
	}
	t "rejoin: killed while the VM was out, dead in its game when it joins; raised, alive in both" "$(fake_rejoin ok)" "PASS PASS PASS PASS "
	t "rejoin: a joiner whose game keeps the session's dead alive fails" "$(fake_rejoin noload)" "PASS PASS FAIL PASS "

	# the step is wired into the harness: in STEPS after npcs and before final, a session step, run by default
	t "npccombat is a step" "$(case " $STEPS " in *" npcs npccombat "*) echo yes ;; esac)" yes
	t "npccombat comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' npccombat')" 1
	t "npccombat needs a session" "$(case " $SESSION_STEPS " in *" npccombat "*) echo yes ;; esac)" yes
	t "npccombat runs by default" "$(case " $OPTIN_STEPS " in *" npccombat "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "npccombat.sh selftest: all ok" || echo "npccombat.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
