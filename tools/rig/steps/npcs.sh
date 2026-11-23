#!/bin/bash
# steps/npcs.sh: the rig step `npcs` (protocol 6, docs/DESIGN.md "NPCs"), sourced by steps.sh. Every NPC near the two
# players is run by one game and shown as a copy in the other, the copy plays its runner's actions and wears its gear,
# summons are shared, and NPCs change hands when their runner's player walks away. Not run in a game yet.
#
#   step_npcs          the step
#   bash tools/rig/steps/npcs.sh selftest      the step's reading, no game and no rig (CI runs it)
#
# What it does (both players in one session, near each other at the harness's cell, Riverwood):
#   1. Both games share NPCs: the graph event hook is in (hooked=1), each has NPCs loaded, and every NPC loaded in both
#      has one runner both games agree on (`npcs list`: owner=, the same in both, never 0).
#   2. Copies stand where their runners have them: each NPC one game runs is, in the other, a copy following its
#      runner (role=copy) within NPC_COPY_LIMIT of where the runner's game has it, for NPC_COPY_SHARE % of them (the two
#      games are read one after the other and a copy is drawn about 120 ms behind: a walking NPC is a few dozen units
#      off). And in each game on its own, each copy's actor stands where it was last drawn (apart=, one machine, no
#      read gap): NPC_APART_LIMIT, for NPC_APART_SHARE % of them.
#   3. Gear: every copy whose runner's gear came (gear=1) has it on within NPC_WAIT, as the plugin reads it back from
#      the copy's actor after dressing it (dressed=1: every worn item of the runner's on but those it lacks, each hand and
#      the quiver as the runner's, nothing left on that the runner's NPC took off); the items copies lack are said
#      (gearMissing=).
#   4. The kinds seen, in either game (`npcs`: kinds=): people are required (the cell has them); a creature missing is a
#      WARN (none may be near); dragons, horses, followers and summons are said, not required.
#   5. Actions, both ways: an NPC one game runs (no child: children flee), shown as a copy in the other, is set on that
#      game's player with the console (`<ref>.startcombat player`, the scene's set-up, as the harness's injected attacks
#      are; the NPC's confidence and aggression are set so it stands and fights, and the player is given
#      NPC_FIGHT_HEALTH health; all put back after). The other game's copy plays its runner's events (played= goes up)
#      within NPC_FIGHT_WAIT, and turns fewer away than it plays. An NPC that never drew a weapon nor entered combat in
#      its runner's game is the scene's (RIG); one that fought while its runner's hook heard nothing (actionsSent= not
#      up) is a FAIL: copies would show the walk and no attack.
#   6. Summons, both ways: each player in turn learns Conjure Familiar (console addspell, set-up) and casts it with the
#      virtual pad (X readies the hands, the right trigger casts); should the pad make no summon, the console casts it
#      (a HEAL line says so). The caster's game shares it as its spawn (role=run, key <id>:<player>), the other shows a
#      copy of it (role=copy) within NPC_WAIT; killed with the console, it is taken away there (spawnsTaken= up). The
#      spell is taken back after.
#   7. Hand-over, both ways, by real movement along a fixed route (NPC_ROUTE: Riverwood's road north towards Whiterun,
#      from the coc marker): the walker walks the route with the virtual pad's walkto, side-stepping NPC_DETOUR u when
#      something stops it (a cart, a fence), until it is about NPC_WALK u from the stayer. Of the NPCs the walker ran near
#      the stayer before the walk, those the release rule lets go of from where both players are after it (past
#      NPC_HANDOFF_FAR from the walker, the stayer NPC_HANDOFF_MARGIN nearer, NPC_RULE_SLACK to spare; or no longer
#      loaded in the walker's game) go to the stayer's game in both books; the rest are said, not checked (none left is a
#      WARN). The walker walks the route back. A walk that cannot get far enough is a rig fault (RIG), not a FAIL:
#      NPC_ROUTE then wants new points.
#   8. No graph event of an NPC a game runs was lost (eventsLost=0); over the step some copy played an event (a WARN
#      if none: the scene made none) and no game's copies turned away more of their runners' events than they played.
#
# Nothing is moved by a script: the players walk and cast with the virtual pad, the NPCs are the cell's own. The
# console is used for set-up only (the health, the fight's start, the spell, the summon's end), each said in the run.

NPC_COPY_LIMIT=${NPC_COPY_LIMIT:-150}    # u: a copy from where its runner's game has the NPC (two machines)
NPC_COPY_SHARE=${NPC_COPY_SHARE:-80}     # %: of the copies, at least this many within NPC_COPY_LIMIT
NPC_APART_LIMIT=${NPC_APART_LIMIT:-40}   # u: a copy's actor from the runner's state it was last drawn at (one machine)
NPC_APART_SHARE=${NPC_APART_SHARE:-90}   # %: of the mirrored copies, at least this many within NPC_APART_LIMIT
NPC_NEAR=${NPC_NEAR:-2000}               # u: the NPCs near the staying player that the hand-over check follows
NPC_WALK=${NPC_WALK:-9000}               # u: how far the walker gets (NpcRules.h: a runner lets go past 6000 with a friend 1000 nearer)
NPC_HANDOFF_FAR=${NPC_HANDOFF_FAR:-6000}       # u: NpcRules.h's HANDOFF_FAR
NPC_HANDOFF_MARGIN=${NPC_HANDOFF_MARGIN:-1000} # u: NpcRules.h's HANDOFF_MARGIN
NPC_RULE_SLACK=${NPC_RULE_SLACK:-500}    # u: an NPC checked in the hand-over is past the release rule by this much (it walks)
NPC_FIGHT_CONFIDENCE=${NPC_FIGHT_CONFIDENCE:-4}  # the fight's NPC's confidence (4: foolhardy, never flees) and aggression
NPC_FIGHT_AGGRESSION=${NPC_FIGHT_AGGRESSION:-1}  # (1: aggressive), set for the fight and put back after
NPC_WAIT=${NPC_WAIT:-20}                 # s: for the books to agree, for gear, for a hand-over, for a summon's copy
NPC_FIGHT_WAIT=${NPC_FIGHT_WAIT:-20}     # s: for a copy to play its fighting runner's events
NPC_FIGHT_HEALTH=${NPC_FIGHT_HEALTH:-5000}  # the health a player is given for the fight (taken back after)
NPC_SPELL=${NPC_SPELL:-000640B6}         # Skyrim.esm's Conjure Familiar
NPC_DETOUR=${NPC_DETOUR:-600}            # u: a side-step round what stopped a walk
# Riverwood's road north towards Whiterun, from the coc marker at about (17225,-47204): x,y points. A first guess, never
# walked yet: the first rig turn shows whether each point is reached (each leg is said), and new points go here.
NPC_ROUTE=${NPC_ROUTE:-"17300,-46200 17500,-44800 17900,-43200 18300,-41500 18700,-39800 19000,-38300"}

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# npc_rows <reply of `npcs list`>: one line per NPC, its columns:
#   1 key (its id, and :<spawner> for a spawn)  2 owner  3 role  4 kind  5 x  6 y  7 z (- when its actor is not in this
#   game's world)  8 played  9 refused  10 gear  11 geared  12 apart  13 mode  14 local  15 dead  16 child  17 weapon
#   (drawn)  18 combat  19 dressed (its gear read back: all of it on but what it lacks, 1; ? not read back yet)
#   20 wears (<worn of the runner's>/<the runner's named>)  (- for none: not listed, or not a copy)
npc_rows() {
	awk 'BEGIN { RS = " [|] " }
		NR > 1 {
			sub(/\n$/, "")
			n = split($0, f, " "); key = f[1]; if (length(key) != 8 || key !~ /^[0-9A-F]+$/) next
			owner = "-"; role = "-"; kind = "-"; x = "-"; y = "-"; z = "-"; played = "-"; refused = "-"; gear = "-"; geared = "-"; apart = "-"; mode = "-"; local = "-"; dead = "-"
			child = "-"; weapon = "-"; combat = "-"; dressed = "-"; wears = "-"
			for (i = 2; i <= n; i++) {
				split(f[i], kv, "=")
				if (kv[1] == "spawner") key = key ":" kv[2]
				else if (kv[1] == "owner" && owner == "-") owner = kv[2]
				else if (kv[1] == "role") role = kv[2]
				else if (kv[1] == "kind") kind = kv[2]
				else if (kv[1] == "played") played = kv[2]
				else if (kv[1] == "refused") refused = kv[2]
				else if (kv[1] == "gear") gear = kv[2]
				else if (kv[1] == "geared") geared = kv[2]
				else if (kv[1] == "apart") apart = kv[2]
				else if (kv[1] == "mode") mode = kv[2]
				else if (kv[1] == "local") local = kv[2]
				else if (kv[1] == "dead") dead = kv[2]
				else if (kv[1] == "child") child = kv[2]
				else if (kv[1] == "weapon") weapon = kv[2]
				else if (kv[1] == "combat") combat = kv[2]
				else if (kv[1] == "dressed") dressed = kv[2]
				else if (kv[1] == "wears") wears = kv[2]
				else if (kv[1] == "at" && f[i] ~ /^at=\(/) { s = substr(f[i], 5, length(f[i]) - 5); split(s, p, ","); x = p[1]; y = p[2]; z = p[3] }
			}
			print key, owner, role, kind, x, y, z, played, refused, gear, geared, apart, mode, local, dead, child, weapon, combat, dressed, wears
		}' <<<"$1"
}

# npc_col <rows> <key> <column>: that column of that NPC's row (empty when not listed)
npc_col() { awk -v k="$2" -v c="$3" '$1 == k { print $c; exit }' <<<"$1"; }

# npcs_agree <host rows> <vm rows>: "both=N agreed=N bad=<key>:<host owner>/<vm owner>,..." over the NPCs both list
npcs_agree() {
	awk 'NF == 0 { next } FILENAME == ARGV[1] { owner[$1] = $2; next }
		($1 in owner) { both++; if (owner[$1] == $2 && $2 != "0" && $2 != "-") agreed++; else bad = bad (bad ? "," : "") $1 ":" owner[$1] "/" $2 }
		END { printf "both=%d agreed=%d bad=%s\n", both, agreed, bad ? bad : "none" }' <(printf '%s\n' "$1") <(printf '%s\n' "$2")
}

# npcs_copies <runner rows> <watcher rows> <runner's player id>: "copies=N within=N share=P mean=D max=D off=<key>:<d>,...":
# each NPC the runner runs and the watcher lists, how far the watcher's copy is from the runner's NPC (on the ground)
npcs_copies() {
	awk -v me="$3" -v lim="$NPC_COPY_LIMIT" 'NF == 0 { next } FILENAME == ARGV[1] { if ($2 == me && $3 == "run" && $5 != "-") { rx[$1] = $5; ry[$1] = $6 } next }
		($1 in rx) {
			n++
			if ($3 != "copy" || $5 == "-") { off = off (off ? "," : "") $1 ":" ($3 == "copy" ? "nowhere" : $3); next }
			d = sqrt(($5 - rx[$1]) ^ 2 + ($6 - ry[$1]) ^ 2); sum += d; if (d > max) max = d
			if (d <= lim) within++; else off = off (off ? "," : "") $1 ":" sprintf("%.0f", d)
			placed++
		}
		END { printf "copies=%d within=%d share=%d mean=%.0f max=%.0f off=%s\n", n, within, n ? int(100 * within / n) : 0, placed ? sum / placed : 0, max, off ? off : "none" }' \
		<(printf '%s\n' "$1") <(printf '%s\n' "$2")
}

# npcs_apart <rows>: "copies=N within=N share=P max=D off=<key>:<d>,...": each mirrored copy (role=copy) with an actor
# drawn (apart= a number), how far its actor is from the runner's state it was last drawn at, in this one game
npcs_apart() {
	awk -v lim="$NPC_APART_LIMIT" '$3 == "copy" && $12 ~ /^[0-9.]+$/ {
			n++; d = $12 + 0; if (d > max) max = d
			if (d <= lim) within++; else off = off (off ? "," : "") $1 ":" sprintf("%.0f", d)
		}
		END { printf "copies=%d within=%d share=%d max=%.0f off=%s\n", n, within, n ? int(100 * within / n) : 0, max, off ? off : "none" }' <<<"$1"
}

# npcs_gear <rows>: "withgear=N dressed=N undressed=<key>:<why>,...": the mirrored copies whose runner's gear came
# (gear=1), and of those, the ones dressed (geared=1) whose gear read back from the actor is all of it on, apart from
# the items they lack, with nothing left on that the runner's NPC took off (dressed=1). Why not: geared=0 (never
# dressed), wears=? (not read back yet), or wears=<worn>/<named> (the read-back fell short: `npcs <id>` says which part)
npcs_gear() {
	awk '$3 == "copy" && $10 == "1" { n++
			if ($11 == "1" && $19 == "1") g++
			else bad = bad (bad ? "," : "") $1 ":" ($11 != "1" ? "geared=0" : ($19 == "?" || $19 == "-") ? "wears=?" : "wears=" $20)
		}
		END { printf "withgear=%d dressed=%d undressed=%s\n", n, g, bad ? bad : "none" }' <<<"$1"
}

# npcs_events <rows>: "copies=N played=P refused=R": the runner's events the mirrored copies played and turned away
npcs_events() {
	awk '$3 == "copy" && $8 ~ /^[0-9]+$/ { n++; p += $8; r += $9 } END { printf "copies=%d played=%d refused=%d\n", n, p, r }' <<<"$1"
}

# npcs_kind <kind> <npcs reply> [<npcs reply>...]: how many NPCs of that kind the replies count (kinds=human:3,...)
npcs_kind() {
	local k=$1 r total=0 n; shift
	for r in "$@"; do n=$(field kinds "$r" | tr ',' '\n' | sed -n "s/^$k://p"); is_num "$n" && total=$((total + n)); done
	echo "$total"
}

# npcs_near <rows> <x y z> <runner id>: the keys that runner runs within NPC_NEAR of that place, space-separated; not
# its spawns, summons or followers (they go with its player)
npcs_near() {
	awk -v p="$2" -v me="$3" -v lim="$NPC_NEAR" 'BEGIN { split(p, q, " ") }
		$2 == me && $1 !~ /:/ && $4 != "follower" && $4 != "summon" && $5 != "-" && sqrt(($5 - q[1]) ^ 2 + ($6 - q[2]) ^ 2) <= lim { printf "%s ", $1 }' <<<"$1" | sed 's/ $//'
}

# npcs_releasable <walker rows> <stayer rows> <walker x y z> <stayer x y z> <keys...>: one line per key, "<key> yes|no
# <from the walker> <from the stayer>": yes when the walker's game lets go of it and the stayer's takes it by the
# release rule (NpcRules.h: the walker's player past HANDOFF_FAR, the stayer's HANDOFF_MARGIN nearer), each with
# NPC_RULE_SLACK to spare for an NPC that walks its sandbox meanwhile; also yes when the walker's game no longer has it
# loaded (let go of as gone). The NPC's place is the stayer's game's (its copy or its own); no when the stayer's game
# does not have it in its world (gone: it cannot take it).
npcs_releasable() {
	local wr=$1 sr=$2 wp=$3 sp=$4; shift 4
	awk -v keys="$*" -v wp="$wp" -v sp="$sp" -v far="$NPC_HANDOFF_FAR" -v margin="$NPC_HANDOFF_MARGIN" -v slack="$NPC_RULE_SLACK" '
		BEGIN { split(wp, w, " "); split(sp, s, " ") } NF == 0 { next }
		FILENAME == ARGV[1] { if ($5 != "-") inWalker[$1] = 1; next }
		{ if ($5 != "-") { x[$1] = $5; y[$1] = $6 } }
		END {
			n = split(keys, k, " ")
			for (i = 1; i <= n; i++) {
				if (!(k[i] in x)) { print k[i], "no", "?", "gone"; continue }
				dw = sqrt((x[k[i]] - w[1]) ^ 2 + (y[k[i]] - w[2]) ^ 2); ds = sqrt((x[k[i]] - s[1]) ^ 2 + (y[k[i]] - s[2]) ^ 2)
				ok = !(k[i] in inWalker) || (dw > far + slack && ds + margin + slack < dw)
				printf "%s %s %.0f %.0f\n", k[i], ok ? "yes" : "no", dw, ds
			}
		}' <(printf '%s\n' "$wr") <(printf '%s\n' "$sr")
}

# npcs_owned_by <rows> <id> <keys...>: 0 when every key is in the rows with that owner; else prints the ones not
npcs_owned_by() {
	local rows=$1 id=$2 k o not=""; shift 2
	for k in "$@"; do o=$(awk -v k="$k" '$1 == k { print $2; exit }' <<<"$rows"); [ "$o" = "$id" ] || not+="$k:${o:-gone} "; done
	[ -z "$not" ] && return 0
	echo "${not% }"; return 1
}

# npcs_pick_foe <runner rows> <watcher rows> <runner id> <x y z>: "<key> <local id>" of the NPC to set on the runner's
# player: one the runner runs (a load-order NPC, alive, in its world, no follower or summon, no child: a child flees and
# never draws) that the watcher shows as a mirrored copy; a person before a creature, then the nearest to that place.
# Empty when there is none.
npcs_pick_foe() {
	awk -v me="$3" -v p="$4" 'BEGIN { split(p, q, " ") } NF == 0 { next }
		FILENAME == ARGV[1] { if ($3 == "copy") shown[$1] = 1; next }
		$2 == me && $3 == "run" && $1 !~ /:/ && $15 == "0" && $16 != "1" && $5 != "-" && ($4 == "human" || $4 == "creature") && ($1 in shown) {
			d = sqrt(($5 - q[1]) ^ 2 + ($6 - q[2]) ^ 2) + ($4 == "human" ? 0 : 1e9)
			if (best == "" || d < bestd) { best = $1 " " $14; bestd = d }
		}
		END { if (best != "") print best }' <(printf '%s\n' "$2") <(printf '%s\n' "$1")
}

# npcs_spawn_of <rows> <player id>: "<key> <local id>" of the first spawn of that player its game runs; empty: none
npcs_spawn_of() { awk -v s=":$2" 'substr($1, length($1) - length(s) + 1) == s && $3 == "run" { print $1, $14; exit }' <<<"$1"; }

# npcs_points <route>: the route's "x,y" points, one "x y" a line
npcs_points() { tr ' ' '\n' <<<"$1" | awk -F, 'NF == 2 && $1 ~ /^-?[0-9.]+$/ && $2 ~ /^-?[0-9.]+$/ { print $1, $2 }'; }

# npcs_reversed <route>: the same points, last first (the way back)
npcs_reversed() { tr ' ' '\n' <<<"$1" | awk 'NF { p[++n] = $0 } END { for (i = n; i >= 1; i--) printf "%s%s", p[i], (i > 1 ? " " : "\n") }'; }

# npcs_detour <x y (where the walker stopped)> <x y (where it was going)> <left|right>: "x y" of a side-step, NPC_DETOUR
# to that side of the way and half of it on along it
npcs_detour() {
	awk -v a="$1" -v b="$2" -v side="$3" -v w="$NPC_DETOUR" 'BEGIN { split(a, p, " "); split(b, t, " "); dx = t[1] - p[1]; dy = t[2] - p[2]; l = sqrt(dx * dx + dy * dy)
		if (l < 1) { dx = 1; dy = 0; l = 1 } dx /= l; dy /= l; s = (side == "left" ? 1 : -1)
		printf "%.0f %.0f\n", p[1] - dy * w * s + dx * w / 2, p[2] + dx * w * s + dy * w / 2 }'
}

# ---- the step -----------------------------------------------------------------------------------------------------

_npcs_rows() { npc_rows "$(dc "$1" 'npcs list')"; }
_npcs_agreed() {
	local a; a=$(npcs_agree "$(_npcs_rows host)" "$(_npcs_rows vm)")
	[ "$(field bad "$a")" = none ] && [ "$(field both "$a")" != 0 ]
}
_npcs_handed() { npcs_owned_by "$(_npcs_rows host)" "$1" "${@:2}" >/dev/null && npcs_owned_by "$(_npcs_rows vm)" "$1" "${@:2}" >/dev/null; }
_npcs_walk_over() { [[ $(dc "$1" walkto) == "not walking"* ]]; }
_npcs_geared() { [ "$(field undressed "$(npcs_gear "$(_npcs_rows "$1")")")" = none ]; }
_npcs_played() { local p; p=$(npc_col "$(_npcs_rows "$1")" "$2" 8); is_num "$p" && [ "$p" -gt "$3" ]; }
# _npcs_fight_played <watcher> <key> <played before> <runner>: _npcs_played, and NPCS_FOUGHT=1 once the runner's own row
# shows the NPC with its weapon drawn or in combat
_npcs_fight_played() {
	local r; r=$(_npcs_rows "$4")
	{ [ "$(npc_col "$r" "$2" 17)" = 1 ] || [ "$(npc_col "$r" "$2" 18)" = 1 ]; } && NPCS_FOUGHT=1
	_npcs_played "$1" "$2" "$3"
}
_npcs_spawned() { [ -n "$(npcs_spawn_of "$(_npcs_rows "$1")" "$2")" ]; }
_npcs_copy_shown() { local r; r=$(_npcs_rows "$1"); [ "$(npc_col "$r" "$2" 3)" = copy ] && [ "$(npc_col "$r" "$2" 5)" != - ]; }
_npcs_spawn_gone() { local t; t=$(field spawnsTaken "$(dc "$1" npcs)"); is_num "$t" && [ "$t" -gt "$3" ] && [ "$(npc_col "$(_npcs_rows "$1")" "$2" 3)" != copy ]; }

# npcs_leg <who> <x> <y>: one walk to a point (radius 150, 90 s); prints how far from it the walker stopped
npcs_leg() {
	local r
	r=$(dc "$1" "walkto $2 $3 1.0 150 90")
	[[ $r == walking* ]] || { echo "refused"; return 1; }
	wait_until 100 _npcs_walk_over "$1"; still "$1"
	dist2 "$(pos_of "$1")" "$2 $3 0"
}

# npcs_walk_route <who> <route>: walks the route's points in order; a leg that stops short gets a side-step left, then
# right, and the point again. 0 when every point was reached; else prints which was not and where the walker is.
npcs_walk_route() {
	local who=$1 x y d i=0 side dx dy
	while read -r x y; do
		i=$((i + 1))
		d=$(npcs_leg "$who" "$x" "$y") || { echo "point $i ($x,$y): the walkto was refused"; return 1; }
		for side in left right; do
			is_num "$d" && [ "$d" -le 400 ] && break
			read -r dx dy < <(npcs_detour "$(pos_of "$who")" "$x $y" "$side")
			npcs_leg "$who" "$dx" "$dy" >/dev/null
			d=$(npcs_leg "$who" "$x" "$y") || { echo "point $i ($x,$y): the walkto was refused"; return 1; }
		done
		if ! is_num "$d" || [ "$d" -gt 400 ]; then
			echo "point $i ($x,$y) not reached: stopped ${d:-?} u from it at ($(pos_of "$who" | awk '{print $1 "," $2}')), side-steps tried"
			return 1
		fi
	done < <(npcs_points "$2")
	return 0
}

# npcs_handover <walker> <stayer>: the walker's player walks NPC_ROUTE away; the NPCs it ran near the stayer go to the
# stayer's game in both books; the walker walks the route back to where it began
npcs_handover() {
	local walker=$1 stayer=$2 label="npcs: the $1 walks away, the $2 takes its NPCs" wid sid p0 sp keys moved why back rel ready left
	section "$label"
	wid=$(field player "$(dc "$walker" 'session status')"); sid=$(field player "$(dc "$stayer" 'session status')")
	if ! is_num "$wid" || ! is_num "$sid"; then rig_broken game "$label" "no player ids (walker '$wid', stayer '$sid')"; return; fi
	p0=$(pos_of "$walker"); sp=$(pos_of "$stayer")
	if [ -z "$p0" ] || [ -z "$sp" ]; then rig_broken game "$label" "no place for a player (walker '$p0', stayer '$sp')"; return; fi
	keys=$(npcs_near "$(_npcs_rows "$stayer")" "$sp" "$wid")
	if [ -z "$keys" ]; then
		warn "$label: NPCs to hand over" "the $walker runs none within $NPC_NEAR u of the $stayer's player: nothing to follow this way"
		return
	fi
	say "$label: following $(wc -w <<<"$keys") NPC(s): $keys"
	back="$(npcs_reversed "$NPC_ROUTE") $(awk '{printf "%.0f,%.0f", $1, $2}' <<<"$p0")"
	if ! why=$(npcs_walk_route "$walker" "$NPC_ROUTE"); then
		rig_fault walk "$label: the walk" "the $walker's walk along NPC_ROUTE stopped: $why"
		npcs_walk_route "$walker" "$back" >/dev/null; return
	fi
	moved=$(dist2 "$(pos_of "$walker")" "$sp")
	if ! is_num "$moved" || [ "$moved" -lt $((NPC_WALK * 3 / 4)) ]; then
		rig_fault walk "$label: the walk" "the $walker walked the route but is ${moved:-?} u from the $stayer, not about $NPC_WALK: NPC_ROUTE wants farther points"
		npcs_walk_route "$walker" "$back" >/dev/null; return
	fi
	pass "$label: the walk" "the $walker is $moved u from the $stayer, along the road"
	# Only the NPCs the release rule lets go of from where the players are now (they were picked before the walk, and
	# they walk their own ways meanwhile): the rest are said, not checked.
	# shellcheck disable=SC2086
	rel=$(npcs_releasable "$(_npcs_rows "$walker")" "$(_npcs_rows "$stayer")" "$(pos_of "$walker")" "$(pos_of "$stayer")" $keys)
	ready=$(awk '$2 == "yes" { printf "%s ", $1 }' <<<"$rel" | sed 's/ $//')
	left=$(awk '$2 == "no" { printf "%s%s:%s/%s", (n++ ? "," : ""), $1, $3, $4 }' <<<"$rel")
	[ -n "$left" ] && say "$label: not checked, not past the release rule from where the players are now (key:u from the $walker/u from the $stayer): $left"
	if [ -z "$ready" ]; then
		warn "$label: NPCs to hand over" "none of the $(wc -w <<<"$keys") NPC(s) is past the release rule after the walk ($left): nothing to check this way"
		npcs_walk_route "$walker" "$back" >/dev/null; return
	fi
	# shellcheck disable=SC2086
	if wait_until "$NPC_WAIT" _npcs_handed "$sid" $ready; then
		pass "$label: both books say the $stayer runs them" "$(wc -w <<<"$ready") NPC(s) past the release rule: $ready"
	else
		# shellcheck disable=SC2086
		why="host: $(npcs_owned_by "$(_npcs_rows host)" "$sid" $ready); vm: $(npcs_owned_by "$(_npcs_rows vm)" "$sid" $ready)"
		fail "$label: both books say the $stayer runs them" "after $NPC_WAIT s, not the $stayer's (key:owner): $why"
	fi
	# no check: the next step puts the players together again if the way back falls short
	why=$(npcs_walk_route "$walker" "$back") || say "$label: the way back stopped short ($why)"
}

# npcs_fight <runner> <watcher>: an NPC the runner runs is set on the runner's player (console set-up); the watcher's
# copy of it plays its runner's events
npcs_fight() {
	local runner=$1 watcher=$2 label="npcs: an NPC the $1 runs fights, the $2's copy plays its actions" rid pick key local p0 sent0 sent1 played refused one conf0 aggr0 setav=0
	section "$label"
	rid=$(field player "$(dc "$runner" 'session status')")
	is_num "$rid" || { rig_broken game "$label" "no player id for the $runner"; return; }
	pick=$(npcs_pick_foe "$(_npcs_rows "$runner")" "$(_npcs_rows "$watcher")" "$rid" "$(pos_of "$runner")")
	if [ -z "$pick" ]; then
		warn "$label: an NPC to fight" "the $runner runs no living grown person or creature the $watcher shows as a copy: no fight this way"
		return
	fi
	read -r key local <<<"$pick"
	p0=$(npc_col "$(_npcs_rows "$watcher")" "$key" 8); is_num "$p0" || p0=0
	sent0=$(field actionsSent "$(dc "$runner" npcs)")
	# set-up: a civilian of low confidence flees instead of fighting (a flee sends only the walk's events, which are never
	# sent): it is made to stand and fight, and its own values are put back after
	one=$(dc "$runner" "npcs $local"); conf0=$(field confidence "$one"); aggr0=$(field aggression "$one")
	if is_num "$conf0" && is_num "$aggr0"; then
		dc "$runner" "console $local.setav confidence $NPC_FIGHT_CONFIDENCE" >/dev/null; dc "$runner" "console $local.setav aggression $NPC_FIGHT_AGGRESSION" >/dev/null; setav=1
	fi
	if [ "$(dc "$runner" "console player.modav health $NPC_FIGHT_HEALTH")" != ran ] || [ "$(dc "$runner" "console $local.startcombat player")" != ran ]; then
		rig_broken input "$label: the fight's set-up" "the $runner's console refused modav or startcombat"
		[ "$setav" = 1 ] && { dc "$runner" "console $local.setav confidence $conf0" >/dev/null; dc "$runner" "console $local.setav aggression $aggr0" >/dev/null; }
		dc "$runner" "console player.modav health -$NPC_FIGHT_HEALTH" >/dev/null; return
	fi
	if [ "$setav" = 1 ]; then
		say "$label: set-up (console): the $runner's player +$NPC_FIGHT_HEALTH health; $local ($key) confidence $conf0 -> $NPC_FIGHT_CONFIDENCE, aggression $aggr0 -> $NPC_FIGHT_AGGRESSION, set on it with startcombat"
	else
		say "$label: set-up (console): the $runner's player +$NPC_FIGHT_HEALTH health; $local ($key) set on it with startcombat (its confidence and aggression unread, left as they are)"
	fi
	NPCS_FOUGHT=0
	if wait_until "$NPC_FIGHT_WAIT" _npcs_fight_played "$watcher" "$key" "$p0" "$runner"; then
		played=$(npc_col "$(_npcs_rows "$watcher")" "$key" 8); refused=$(npc_col "$(_npcs_rows "$watcher")" "$key" 9)
		pass "$label: the copy played its runner's events" "$key: played $p0 -> $played (refused $refused)"
		expect_le "$label: the copy turns away fewer of its runner's events than it plays" "$((refused))" "$((played - p0))" "refused, of $((played - p0)) played"
	else
		sent1=$(field actionsSent "$(dc "$runner" npcs)")
		if [ "$NPCS_FOUGHT" != 1 ]; then
			# the scene's, not the mod's: an NPC that never drew nor fought makes no attack to show
			rig_fault state "$label: the copy played its runner's events" "$local ($key) never drew a weapon nor entered combat in the $runner's game in $NPC_FIGHT_WAIT s (actionsSent $sent0 -> $sent1): no fight to show"
		elif is_num "$sent0" && is_num "$sent1" && [ "$sent1" -le "$sent0" ]; then
			fail "$label: the copy played its runner's events" "$local fought in the $runner's game but its graph event hook heard nothing of it (actionsSent $sent0 -> $sent1): copies show the walk and no attack"
		else
			fail "$label: the copy played its runner's events" "the $runner sent events (actionsSent $sent0 -> $sent1) but the $watcher's copy played none in $NPC_FIGHT_WAIT s: $(dc "$watcher" "npcs $key" | cut -c1-240)"
		fi
	fi
	dc "$runner" "console $local.stopcombat" >/dev/null
	if [ "$setav" = 1 ]; then
		dc "$runner" "console $local.setav confidence $conf0" >/dev/null; dc "$runner" "console $local.setav aggression $aggr0" >/dev/null
	fi
	dc "$runner" "console player.modav health -$NPC_FIGHT_HEALTH" >/dev/null
}

# npcs_spawn <caster> <watcher>: the caster's player conjures with the pad (the console if the pad makes none); the
# watcher shows the summon as a copy; killed, it is taken away there
npcs_spawn() {
	local caster=$1 watcher=$2 label="npcs: the $1 conjures, the $2 shows the summon" cid taken0 pick key local
	section "$label"
	cid=$(field player "$(dc "$caster" 'session status')")
	is_num "$cid" || { rig_broken game "$label" "no player id for the $caster"; return; }
	taken0=$(field spawnsTaken "$(dc "$watcher" npcs)"); is_num "$taken0" || taken0=0
	[ "$(dc "$caster" "console player.addspell $NPC_SPELL")" = ran ] || { rig_broken input "$label: the spell's set-up" "the $caster's console refused addspell"; return; }
	say "$label: set-up (console): the $caster's player learns $NPC_SPELL"
	dc "$caster" "console player.equipspell $NPC_SPELL 1" >/dev/null
	dc "$caster" 'pad button x' >/dev/null; sleep 2
	dc "$caster" 'pad trigger r 1 2500' >/dev/null; sleep 3; still "$caster"
	if ! wait_until 8 _npcs_spawned "$caster" "$cid"; then
		healed "$label: the cast" "the pad's cast made no summon in 8 s: the $caster's console casts it (player.cast)"
		dc "$caster" "console player.cast $NPC_SPELL player" >/dev/null
		if ! wait_until 8 _npcs_spawned "$caster" "$cid"; then
			rig_fault state "$label: the summon" "no summon of the $caster's player in its npcs list after the pad's cast and the console's: $(dc "$caster" npcs | cut -c1-160)"
			dc "$caster" "console player.removespell $NPC_SPELL" >/dev/null; return
		fi
	fi
	pick=$(npcs_spawn_of "$(_npcs_rows "$caster")" "$cid"); read -r key local <<<"$pick"
	pass "$label: the $caster's game shares its summon as its spawn" "$key (here $local)"
	if wait_until "$NPC_WAIT" _npcs_copy_shown "$watcher" "$key"; then
		pass "$label: the $watcher shows a copy of it" "$(dc "$watcher" "npcs ${key%%:*}" | cut -c1-200)"
	else
		fail "$label: the $watcher shows a copy of it" "no mirrored copy of $key in $NPC_WAIT s: $(dc "$watcher" npcs | cut -c1-200)"
	fi
	dc "$caster" "console $local.kill" >/dev/null
	say "$label: set-up (console): the summon $local killed"
	if wait_until $((NPC_WAIT + 15)) _npcs_spawn_gone "$watcher" "$key" "$taken0"; then
		pass "$label: gone from the $caster, its copy is taken away in the $watcher" "spawnsTaken $taken0 -> $(field spawnsTaken "$(dc "$watcher" npcs)")"
	else
		fail "$label: gone from the $caster, its copy is taken away in the $watcher" "spawnsTaken $taken0 -> $(field spawnsTaken "$(dc "$watcher" npcs)"); row: $(npc_col "$(_npcs_rows "$watcher")" "$key" 3)"
	fi
	dc "$caster" "console player.removespell $NPC_SPELL" >/dev/null
}

step_npcs() {
	section "npcs: one runner per NPC, copies where their runners have them"
	local w s a hostRows vmRows hid vid c total=0 hs vs g e p=0
	for w in host vm; do still $w; done
	for w in host vm; do
		s=$(dc $w npcs)
		expect_eq "$w: the graph event hook is in" "$(field hooked "$s")" 1
		expect_eq "$w: sharing NPCs (in a session)" "$(field active "$s")" 1
	done
	if wait_until "$NPC_WAIT" _npcs_agreed; then
		pass "npcs: every NPC loaded in both has one runner both agree on" "$(npcs_agree "$(_npcs_rows host)" "$(_npcs_rows vm)")"
	else
		a=$(npcs_agree "$(_npcs_rows host)" "$(_npcs_rows vm)")
		if [ "$(field both "$a")" = 0 ]; then
			rig_fault state "npcs: both games have NPCs loaded" "no NPC listed in both ($(dc host npcs | cut -c1-80) / $(dc vm npcs | cut -c1-80)): the cell has none near"
			return
		fi
		fail "npcs: every NPC loaded in both has one runner both agree on" "$a"
	fi
	sleep 2
	hid=$(field player "$(dc host 'session status')"); vid=$(field player "$(dc vm 'session status')")
	hostRows=$(_npcs_rows host); vmRows=$(_npcs_rows vm)
	for w in host vm; do
		if [ $w = host ]; then c=$(npcs_copies "$hostRows" "$vmRows" "$hid"); else c=$(npcs_copies "$vmRows" "$hostRows" "$vid"); fi
		if [ "$(field copies "$c")" = 0 ]; then
			say "npcs: the $w runs none the $(other $w) lists: no copies that way"
			continue
		fi
		total=$((total + $(field copies "$c")))
		expect_ge "npcs: the $(other $w)'s copies of the $w's NPCs stand where the $w has them (within $NPC_COPY_LIMIT u)" "$(field share "$c")" "$NPC_COPY_SHARE" "% ($c)"
	done
	[ "$total" -gt 0 ] || fail "npcs: some NPC is shown as a copy" "neither game shows a copy of an NPC the other runs"
	# the same, each game on its own: no read gap between two machines
	for w in host vm; do
		if [ $w = host ]; then c=$(npcs_apart "$hostRows"); else c=$(npcs_apart "$vmRows"); fi
		[ "$(field copies "$c")" = 0 ] && continue
		expect_ge "npcs: the $w's copies stand where they were last drawn (apart, within $NPC_APART_LIMIT u)" "$(field share "$c")" "$NPC_APART_SHARE" "% ($c)"
	done
	# gear: every copy whose runner's gear came has it on, as read back from the copy's actor (dressed=1: all of it but
	# what the copy lacks, nothing left on that the runner's NPC took off)
	for w in host vm; do
		if wait_until "$NPC_WAIT" _npcs_geared "$w"; then
			g=$(npcs_gear "$(_npcs_rows $w)")
			if [ "$(field withgear "$g")" = 0 ]; then warn "npcs: the $w's copies have their runners' gear on" "no copy has its runner's gear yet ($g)"
			else pass "npcs: the $w's copies have their runners' gear on" "$g; gearMissing=$(field gearMissing "$(dc $w npcs)") gearTakenOff=$(field gearTakenOff "$(dc $w npcs)")"; fi
		else
			g=$(npcs_gear "$(_npcs_rows $w)"); s=$(field undressed "$g"); s=${s%%,*}
			fail "npcs: the $w's copies have their runners' gear on" "after $NPC_WAIT s: $g; the first: $(dc $w "npcs ${s%%:*}" | cut -c1-400)"
		fi
	done
	hs=$(dc host npcs); vs=$(dc vm npcs)
	expect_ge "npcs: people are among the NPCs" "$(npcs_kind human "$hs" "$vs")" 1
	if [ "$(npcs_kind creature "$hs" "$vs")" -ge 1 ]; then pass "npcs: creatures are among the NPCs" "host $(field kinds "$hs")"
	else warn "npcs: creatures are among the NPCs" "none near: host $(field kinds "$hs"), vm $(field kinds "$vs")"; fi
	say "npcs: kinds seen: host $(field kinds "$hs"), vm $(field kinds "$vs"); runtime actors not shared: host $(field unshared "$hs"), vm $(field unshared "$vs")"
	npcs_fight host vm
	npcs_spawn host vm
	npcs_handover host vm
	npcs_fight vm host
	npcs_spawn vm host
	npcs_handover vm host
	section "npcs: events, both games"
	for w in host vm; do
		s=$(dc $w npcs)
		expect_zero "$w: no graph event of an NPC it runs lost" "$(field eventsLost "$s")"
		e=$(npcs_events "$(_npcs_rows $w)")
		is_num "$(field played "$s")" && p=$((p + $(field played "$s")))
		if is_num "$(field played "$s")" && is_num "$(field refusedEvents "$s")" && [ $(($(field played "$s") + $(field refusedEvents "$s"))) -gt 0 ]; then
			expect_le "$w: its copies turn away fewer of their runners' events than they play" "$(field refusedEvents "$s")" "$(field played "$s")" "refused (copies now: $e)"
		fi
	done
	if [ "$p" -gt 0 ]; then pass "npcs: some copy played its runner's events" "$p played over both games"
	else warn "npcs: some copy played its runner's events" "no copy in either game played any: the scene made none, or the hook hears nothing"; fi
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init npcstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# replies in the format NpcSync::Describe writes
	HOST_LIST='npcs=3 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(100,200,0) dead=0 child=0 weapon=0 combat=0 | 0001A680 local=0001A680 owner=2 role=copy kind=creature at=(500,500,0) dead=0 child=0 weapon=0 combat=0 runner=2 mode=mirror target=(505,500,0) apart=5 base=00023A8F states=2 drawn=40 played=1 refused=0 walkRefused=7 gear=1 geared=1 wears=2/2 leftOn=0 hands=ok ammo=ok lacks=0 dressed=1 graphSpeed=0 | FF000801 spawner=1 local=FF000801 owner=1 role=run kind=summon at=(150,250,0) dead=0 child=0 weapon=0 combat=0'
	VM_LIST='npcs=3 | 0001A67C local=0001A67C owner=1 role=copy kind=human at=(130,200,0) dead=0 child=0 weapon=0 combat=0 runner=1 mode=mirror target=(128,200,0) apart=2 base=00013BA2 states=2 drawn=40 played=3 refused=1 walkRefused=0 last=IdleChairSitting gear=1 geared=1 wears=3/3 leftOn=0 hands=ok ammo=ok lacks=0 dressed=1 graphSpeed=150 | 0001A680 local=0001A680 owner=2 role=run kind=creature at=(510,500,0) dead=0 child=0 weapon=0 combat=0 | FF000801 spawner=1 local=FF000AAA owner=1 role=copy kind=? at=(450,250,0) dead=0 child=0 weapon=0 combat=0 runner=1 mode=mirror target=(160,250,0) apart=290 base=000204C0 states=2 drawn=4 played=0 refused=0 walkRefused=0 gear=0 geared=0 wears=? dressed=? graphSpeed=none'
	NEAR_LIST=${VM_LIST/at=(450,250,0)/at=(170,250,0)}  # the spawn's copy where the host has it
	NEAR_LIST=${NEAR_LIST/apart=290/apart=10}
	H=$(npc_rows "$HOST_LIST"); V=$(npc_rows "$VM_LIST")
	t "rows: one per NPC" "$(wc -l <<<"$H" | tr -d ' ')" 3
	t "rows: a load-order NPC" "$(head -1 <<<"$H")" "0001A67C 1 run human 100 200 0 - - - - - - 0001A67C 0 0 0 0 - -"
	t "rows: a spawn's key names its spawner" "$(sed -n 3p <<<"$H" | cut -d' ' -f1-3)" "FF000801:1 1 run"
	t "rows: a copy" "$(sed -n 2p <<<"$H")" "0001A680 2 copy creature 500 500 0 1 0 1 1 5 mirror 0001A680 0 0 0 0 1 2/2"
	t "rows: a copy's events, gear and apart (the last= word is not the refused count)" "$(head -1 <<<"$V" | cut -d' ' -f8-13)" "3 1 1 1 2 mirror"
	t "rows: an actor not in the world" "$(npc_rows 'npcs=1 | 0001A67C local=00000000 owner=0 role=none kind=? at=none dead=0')" "0001A67C 0 none ? - - - - - - - - - 00000000 0 - - - - -"
	t "rows: the list's end" "$(npc_rows "npcs=61 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(1,2,3) dead=0 | ..." | wc -l | tr -d ' ')" 1
	t "rows: none" "$(npc_rows 'npcs=0')" ""
	t "col: a column of a row" "$(npc_col "$V" 0001A67C 8)" 3
	t "col: a row not listed" "$(npc_col "$V" 00099999 8)" ""
	t "agree: both books the same" "$(npcs_agree "$H" "$V")" "both=3 agreed=3 bad=none"
	t "agree: a different owner" "$(field bad "$(npcs_agree "$H" "$(sed 's/^0001A680 2/0001A680 1/' <<<"$V")")")" "0001A680:2/1"
	t "agree: nobody's is not agreed" "$(field bad "$(npcs_agree "$(sed 's/^0001A67C 1/0001A67C 0/' <<<"$H")" "$(sed 's/^0001A67C 1/0001A67C 0/' <<<"$V")")")" "0001A67C:0/0"
	t "agree: one only the host lists is not counted" "$(field both "$(npcs_agree "$H" "$(head -1 <<<"$V")")")" 1
	t "agree: no NPCs" "$(npcs_agree "" "")" "both=0 agreed=0 bad=none"
	t "agree: the VM lists none" "$(npcs_agree "$H" "")" "both=0 agreed=0 bad=none"
	t "copies: host's NPCs in the VM (one 30 u off, the spawn's copy 300 off)" "$(npcs_copies "$H" "$V" 1)" "copies=2 within=1 share=50 mean=165 max=300 off=FF000801:1:300"
	t "copies: the VM's NPC in the host" "$(npcs_copies "$V" "$H" 2)" "copies=1 within=1 share=100 mean=10 max=10 off=none"
	t "copies: one not shown as a copy" "$(field off "$(npcs_copies "$V" "$(sed 's/^0001A680 2 copy/0001A680 2 other/' <<<"$H")" 2)")" "0001A680:other"
	t "apart: the VM's copies, the spawn's 290 u from where it was drawn" "$(npcs_apart "$V")" "copies=2 within=1 share=50 max=290 off=FF000801:1:290"
	t "apart: the host's" "$(npcs_apart "$H")" "copies=1 within=1 share=100 max=5 off=none"
	t "apart: none drawn" "$(npcs_apart "$(npc_rows 'npcs=1 | 0001A67C local=0001A67C owner=2 role=copy kind=human at=none dead=0 runner=2 mode=mirror target=none apart=? played=0 refused=0 gear=0 geared=0')")" "copies=0 within=0 share=0 max=0 off=none"
	t "gear: the VM's copy with gear has it on, the spawn's has none yet" "$(npcs_gear "$V")" "withgear=1 dressed=1 undressed=none"
	t "gear: one with gear not on" "$(npcs_gear "$(sed 's/^\(0001A67C .* \)1 1 2 mirror/\11 0 2 mirror/' <<<"$V")")" "withgear=1 dressed=0 undressed=0001A67C:geared=0"
	t "gear: dressed, but the read-back finds an item not on (a refused equip)" "$(npcs_gear "$(npc_rows "${VM_LIST/wears=3\/3 leftOn=0 hands=ok ammo=ok lacks=0 dressed=1/wears=2/3 leftOn=0 hands=ok ammo=ok lacks=0 dressed=0}")")" "withgear=1 dressed=0 undressed=0001A67C:wears=2/3"
	t "gear: dressed, a bow left on its back (all worn, the hand differs)" "$(npcs_gear "$(npc_rows "${VM_LIST/hands=ok ammo=ok lacks=0 dressed=1/hands=differs ammo=ok lacks=0 dressed=0}")")" "withgear=1 dressed=0 undressed=0001A67C:wears=3/3"
	t "gear: dressed, not read back yet" "$(npcs_gear "$(npc_rows "${VM_LIST/wears=3\/3 leftOn=0 hands=ok ammo=ok lacks=0 dressed=1/wears=? dressed=?}")")" "withgear=1 dressed=0 undressed=0001A67C:wears=?"
	t "events: the VM's copies" "$(npcs_events "$V")" "copies=2 played=3 refused=1"
	S1='active=1 loaded=4 run=2 owners=4 copies=2 shown=2 mirrored=2 spawnCopies=1 kinds=human:3,creature:1,dragon:0,horse:0,follower:0,summon:1 claims=9 actionsSent=4 played=4 refusedEvents=1 eventsLost=0 spawnsTaken=0 unshared=0 hooked=1'
	S2='active=1 loaded=3 run=1 owners=4 kinds=human:2,creature:0,dragon:0,horse:1,follower:0,summon:0 hooked=1'
	t "kinds: summed over the games" "$(npcs_kind human "$S1" "$S2")" 5
	t "kinds: horse" "$(npcs_kind horse "$S1" "$S2")" 1
	t "kinds: none" "$(npcs_kind dragon "$S1" "$S2")" 0
	t "near: the host's NPCs near the VM's player, its summon not (it follows the host)" "$(npcs_near "$(npc_rows "$NEAR_LIST")" "120 200 0" 1)" "0001A67C"
	t "near: none that near" "$(NPC_NEAR=5; npcs_near "$V" "120 200 0" 1)" ""
	t "owned: all the VM's" "$(npcs_owned_by "$V" 2 0001A680 && echo yes)" yes
	t "owned: one not" "$(npcs_owned_by "$V" 2 0001A680 0001A67C)" "0001A67C:1"
	t "owned: one gone from the list" "$(npcs_owned_by "$V" 2 00099999)" "00099999:gone"
	t "foe: the host's person the VM shows as a copy" "$(npcs_pick_foe "$H" "$V" 1 "100 200 0")" "0001A67C 0001A67C"
	t "foe: the VM's creature the host shows" "$(npcs_pick_foe "$V" "$H" 2 "510 500 0")" "0001A680 0001A680"
	t "foe: none when the other shows no copy of it" "$(npcs_pick_foe "$H" "$(sed 's/^0001A67C 1 copy/0001A67C 1 other/' <<<"$V")" 1 "100 200 0")" ""
	t "foe: never a dead one" "$(npcs_pick_foe "$(sed 's/ 0001A67C 0 / 0001A67C 1 /' <<<"$H")" "$V" 1 "100 200 0")" ""
	PEOPLE=$(printf '%s\n%s\n%s\n' '00000A01 1 run creature 0 0 0 - - - - - - 00000A01 0' '00000A02 1 run human 900 0 0 - - - - - - 00000A02 0' '00000A03 1 run human 300 0 0 - - - - - - 00000A03 0')
	SHOWN=$(printf '%s\n%s\n%s\n' '00000A01 1 copy creature 0 0 0' '00000A02 1 copy human 900 0 0' '00000A03 1 copy human 300 0 0')
	t "foe: a person before a creature, then the nearest" "$(npcs_pick_foe "$PEOPLE" "$SHOWN" 1 "0 0 0")" "00000A03 00000A03"
	t "foe: never a child (it flees)" "$(npcs_pick_foe "$(sed 's/^\(00000A03 .* 00000A03 0\)$/\1 1/' <<<"$PEOPLE")" "$SHOWN" 1 "0 0 0")" "00000A02 00000A02"
	t "foe: a child row from the list" "$(npcs_pick_foe "$(sed 's/ 0001A67C 0 0 / 0001A67C 0 1 /' <<<"$H")" "$V" 1 "100 200 0")" ""
	t "releasable: past the rule from where the players are" "$(npcs_releasable "$H" "$V" "9200 200 0" "120 200 0" 0001A67C)" "0001A67C yes 9070 10"
	t "releasable: still near the walker's end of the route" "$(npcs_releasable "$H" "$V" "5000 200 0" "120 200 0" 0001A67C)" "0001A67C no 4870 10"
	t "releasable: past HANDOFF_FAR but by less than the slack" "$(npcs_releasable "$H" "$V" "6400 200 0" "120 200 0" 0001A67C)" "0001A67C no 6270 10"
	t "releasable: no longer loaded in the walker's game" "$(npcs_releasable "" "$V" "5000 200 0" "120 200 0" 0001A67C)" "0001A67C yes 4870 10"
	t "releasable: not in the stayer's world" "$(npcs_releasable "$H" "$V" "9200 200 0" "120 200 0" 00099999)" "00099999 no ? gone"
	t "spawn of: the host's summon" "$(npcs_spawn_of "$H" 1)" "FF000801:1 FF000801"
	t "spawn of: none of player 2's" "$(npcs_spawn_of "$H" 2)" ""
	t "spawn of: a copy is not the caster's" "$(npcs_spawn_of "$V" 1)" ""
	t "points: the route" "$(npcs_points "1,2 -3,4.5 junk 6,x" | tr '\n' ';')" "1 2;-3 4.5;"
	t "points: the default route has six" "$(npcs_points "$NPC_ROUTE" | wc -l | tr -d ' ')" 6
	t "reversed: last first" "$(npcs_reversed "1,2 3,4 5,6")" "5,6 3,4 1,2"
	t "detour: left of a walk north" "$(NPC_DETOUR=600 npcs_detour "0 0" "0 1000" left)" "-600 300"
	t "detour: right of it" "$(NPC_DETOUR=600 npcs_detour "0 0" "0 1000" right)" "600 300"
	t "detour: left of a walk east" "$(NPC_DETOUR=600 npcs_detour "0 0" "1000 0" left)" "300 600"

	# step_npcs on two faked games (fights, summons and the hand-over left out): agreed books and close copies pass
	fake() {  # fake <host list> <vm list>
		FN_H=$1; FN_V=$2
		dc() { case "$2" in
			'npcs list') if [ "$1" = host ]; then echo "$FN_H"; else echo "$FN_V"; fi ;;
			npcs) echo "$S1" ;;
			'session status') if [ "$1" = host ]; then echo 'role=host connection=up welcomed=1 player=1'; else echo 'role=friend connection=up welcomed=1 player=2'; fi ;;
			where) echo 'cell=00000000 world=0000003C pos=(120,200,0) heading=0 modal=0' ;;
			walkto) echo 'not walking' ;;
			*) echo ok ;; esac; }
	}
	run_step() {  # run_step <dir> <host list> <vm list>: the step's lines, then its summary
		mkdir -p "$1"; ( A_OUT=$1; a_init npcs >/dev/null; fake "$2" "$3"; npcs_handover() { :; }; npcs_fight() { :; }; npcs_spawn() { :; }; step_npcs >/dev/null; cut -f1,3 "$A_RESULTS" )
	}
	o=$(run_step "$d/n1" "$HOST_LIST" "$NEAR_LIST")
	t "step: agreed books and close copies pass" "$(grep -c '^FAIL' <<<"$o")" 0
	t "step: ... the copies both ways" "$(grep -c $'^PASS\t.*copies of the .* stand where' <<<"$o")" 2
	t "step: ... each game's copies where they were drawn" "$(grep -c $'^PASS\t.*copies stand where they were last drawn' <<<"$o")" 2
	t "step: ... gear on in both" "$(grep -c $'^PASS\t.*copies have their runners. gear on' <<<"$o")" 2
	t "step: ... some copy played" "$(grep -c $'^PASS\tnpcs: some copy played' <<<"$o")" 1
	o=$(run_step "$d/n2" "$HOST_LIST" "${NEAR_LIST/0001A680 local=0001A680 owner=2/0001A680 local=0001A680 owner=1}")
	t "step: two games that disagree on a runner fail" "$(grep -c $'^FAIL\t.*one runner both agree on' <<<"$o")" 1
	o=$(run_step "$d/n3" "$HOST_LIST" "$VM_LIST")
	t "step: a copy 300 u from its runner's NPC fails" "$(grep -c $'^FAIL\t.*the vm.s copies of the host.s NPCs stand where' <<<"$o")" 1
	t "step: ... and 290 u from where the VM drew it" "$(grep -c $'^FAIL\t.*the vm.s copies stand where they were last drawn' <<<"$o")" 1
	o=$(run_step "$d/n5" "$HOST_LIST" "${NEAR_LIST/played=3 refused=1 walkRefused=0 last=IdleChairSitting gear=1 geared=1/played=3 refused=1 walkRefused=0 gear=1 geared=0}")
	t "step: a copy whose gear never goes on fails" "$(grep -c $'^FAIL\t.*the vm.s copies have their runners. gear on' <<<"$o")" 1
	o=$(S1=${S1/played=4 refusedEvents=1/played=1 refusedEvents=9}; run_step "$d/n6" "$HOST_LIST" "$NEAR_LIST")
	t "step: copies turning away more events than they play fail" "$(grep -c $'^FAIL\t.*turn away fewer of their runners' <<<"$o")" 2
	o=$(S1=${S1/played=4 refusedEvents=1/played=0 refusedEvents=0}; run_step "$d/n7" "$HOST_LIST" "$NEAR_LIST")
	t "step: no copy playing any event is a WARN" "$(grep -c $'^WARN\tnpcs: some copy played' <<<"$o")" 1
	o=$(run_step "$d/n4" 'npcs=0' 'npcs=0')
	t "step: no NPCs near is the rig's state, not the mod's FAIL" "$(grep -c $'^RIG\t.*both games have NPCs loaded' <<<"$o")" 1
	t "step: ... no FAIL" "$(grep -c '^FAIL' <<<"$o")" 0

	# the fight, faked: the runner's console sets the NPC on its player; the watcher's copy plays (or not)
	fight() {  # fight <watcher plays: yes|no> <runner sends: yes|no> [<runner's NPC draws: yes|no>]: the fight's verdicts
		FT=$d/ft; echo 0 > "$FT"; FT_PLAYS=$1; FT_SENDS=$2; FT_DRAWS=${3:-yes}; FT_AV=$d/ft.av; : > "$FT_AV"
		dc() { local n; n=$(cat "$FT"); case "$2" in
			'npcs list') if [ "$1" = host ]; then
					if [ "$n" -ge 1 ] && [ "$FT_DRAWS" = yes ]; then echo "${HOST_LIST/dead=0 child=0 weapon=0 combat=0/dead=0 child=0 weapon=1 combat=1}"; else echo "$HOST_LIST"; fi
				elif [ "$n" -ge 1 ] && [ "$FT_PLAYS" = yes ]; then echo "${NEAR_LIST/played=3 refused=1/played=9 refused=1}"; else echo "$NEAR_LIST"; fi ;;
			'npcs 0001A67C') echo 'npcs=1 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(100,200,0) dead=0 child=0 weapon=0 combat=0 confidence=0 aggression=0' ;;
			npcs) if [ "$1" = host ] && [ "$n" -ge 1 ] && [ "$FT_SENDS" = yes ]; then echo 'actionsSent=12'; else echo 'actionsSent=4'; fi ;;
			'session status') if [ "$1" = host ]; then echo 'player=1'; else echo 'player=2'; fi ;;
			where) echo 'cell=00000000 world=0000003C pos=(120,200,0) modal=0' ;;
			*startcombat*) echo 1 > "$FT"; echo ran ;;
			*setav*) echo "${2#console }" >> "$FT_AV"; echo ran ;;
			console*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/ft$RANDOM; mkdir -p "$A_OUT"; a_init ft >/dev/null; npcs_fight host vm >/dev/null; cut -f1,4 "$A_RESULTS" | tr '\t\n' ': ' )
	}
	o=$(fight yes yes)
	t "fight: the copy plays its runner's events: passes" "$(grep -o 'PASS:0001A67C: played 3 -> 9' <<<"$o")" "PASS:0001A67C: played 3 -> 9"
	t "fight: ... and turns fewer away than it plays" "$(grep -c 'FAIL' <<<"$o")" 0
	t "fight: ... the NPC made to stand and fight, its own values put back after" "$(tr '\n' ';' < "$d/ft.av")" "0001A67C.setav confidence 4;0001A67C.setav aggression 1;0001A67C.setav confidence 0;0001A67C.setav aggression 0;"
	t "fight: a deaf hook, the NPC fighting, fails as such" "$(grep -c 'FAIL:.*hook heard nothing' <<<"$(fight no no)")" 1
	t "fight: events sent but not played fails as such" "$(grep -c 'FAIL:.*sent events' <<<"$(fight no yes)")" 1
	o=$(fight no no no)
	t "fight: an NPC that never drew nor fought is the scene's (RIG), not the mod's" "$(grep -c 'RIG:.*never drew' <<<"$o")" 1
	t "fight: ... no FAIL" "$(grep -c 'FAIL' <<<"$o")" 0

	# the summon, faked: the pad's cast makes one (or only the console's does); the watcher shows it, then takes it away
	spawn() {  # spawn <made by: pad|console|none> <watcher shows: yes|no>: the summon's verdicts
		SP=$d/sp; echo 0 > "$SP"; SP_BY=$1; SP_SHOWS=$2
		dc() { local n; n=$(cat "$SP"); case "$2" in
			'npcs list') if [ "$1" = host ]; then [ "$n" -ge 1 ] && [ "$n" -lt 3 ] && echo "npcs=1 | FF000900 spawner=1 local=FF000900 owner=1 role=run kind=summon at=(150,250,0) dead=0" || echo 'npcs=0'
				elif [ "$n" = 1 ] && [ "$SP_SHOWS" = yes ]; then echo "npcs=1 | FF000900 spawner=1 local=FF000B00 owner=1 role=copy kind=? at=(150,250,0) dead=0 runner=1 mode=mirror apart=0 played=0 refused=0 gear=0 geared=0"
				else echo 'npcs=0'; fi ;;
			npcs) if [ "$1" = vm ] && [ "$n" -ge 3 ]; then echo 'spawnsTaken=1'; else echo 'spawnsTaken=0'; fi ;;
			'session status') if [ "$1" = host ]; then echo 'player=1'; else echo 'player=2'; fi ;;
			'pad trigger'*) [ "$SP_BY" = pad ] && echo 1 > "$SP"; echo "right trigger" ;;
			*'player.cast'*) [ "$SP_BY" = console ] && echo 1 > "$SP"; echo ran ;;
			*'.kill') echo 3 > "$SP"; echo ran ;;
			console*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/sp$RANDOM; mkdir -p "$A_OUT"; a_init sp >/dev/null; npcs_spawn host vm >/dev/null; cut -f1 "$A_RESULTS" | tr '\n' ' ' )
	}
	t "spawn: the pad's summon shared, shown, taken away" "$(spawn pad yes)" "PASS PASS PASS "
	t "spawn: the console's cast when the pad made none (a HEAL)" "$(spawn console yes)" "HEAL PASS PASS PASS "
	t "spawn: never shown in the watcher: FAIL" "$(spawn pad no)" "PASS FAIL PASS "
	t "spawn: no summon at all is the rig's (RIG), not the mod's" "$(spawn none yes)" "HEAL RIG "

	# the walk along the route, faked: each walkto lands on its point, unless the point is behind a fence
	walk() {  # walk <route> <blocked point "x y" or none> <detour gets past: yes|no>: npcs_walk_route's exit and output
		# the blocked point stops the walker half way; once stopped there, a side-step gets it past when WK_PAST=yes
		WK=$d/wk; echo "0 0" > "$WK"; WK_BLOCK=$2; WK_PAST=$3; WK_HIT=$d/wkh; WK_PASSED=$d/wkp; rm -f "$WK_HIT" "$WK_PASSED"
		dc() { case "$2" in
			'walkto stop') echo stopped ;;
			walkto\ *) local x y; read -r x y _ <<<"${2#walkto }"
				if [ "$x $y" = "$WK_BLOCK" ] && [ ! -f "$WK_PASSED" ]; then echo "$((x / 2)) $((y / 2))" > "$WK"; touch "$WK_HIT"
				else echo "$x $y" > "$WK"; [ -f "$WK_HIT" ] && [ "$WK_PAST" = yes ] && touch "$WK_PASSED"; fi
				echo "walking to ($x,$y)" ;;
			walkto) echo 'not walking' ;;
			where) read -r x y < "$WK"; echo "cell=00000000 world=0000003C pos=($x,$y,0) modal=0" ;;
			*) echo ok ;; esac; }
		( r=$(npcs_walk_route host "$1"); echo "$?:${r:0:44}" )
	}
	t "walk: every point reached" "$(walk "1000,0 2000,0" none yes)" "0:"
	t "walk: a stopped leg side-steps and gets there" "$(walk "1000,0 2000,0" "2000 0" yes)" "0:"
	t "walk: a leg no side-step gets past says which" "$(walk "1000,0 2000,0" "2000 0" no)" "1:point 2 (2000,0) not reached: stopped 1000 u"

	# the hand-over, faked: the walker walks the route, then the books say the stayer runs what it ran
	ho() {  # ho <host list after the walk> <vm list after the walk>: the hand-over's verdicts
		HO_N=$d/ho_n; echo 0 > "$HO_N"; HO_AFTER_H=$1; HO_AFTER_V=$2
		dc() { local n; n=$(cat "$HO_N"); case "$2" in
			'npcs list') if [ "$n" -ge 1 ]; then if [ "$1" = host ]; then echo "$HO_AFTER_H"; else echo "$HO_AFTER_V"; fi
				elif [ "$1" = host ]; then echo "$HOST_LIST"; else echo "$NEAR_LIST"; fi ;;
			'session status') if [ "$1" = host ]; then echo 'player=1'; else echo 'player=2'; fi ;;
			where) if [ "$1" = host ] && [ "$n" -ge 1 ]; then echo 'cell=00000000 world=0000003C pos=(9200,200,0) modal=0'; else echo 'cell=00000000 world=0000003C pos=(120,200,0) modal=0'; fi ;;
			'walkto stop') echo stopped ;;
			walkto\ *) echo 1 > "$HO_N"; echo "walking to (x)" ;;
			walkto) echo 'not walking' ;;
			*) echo ok ;; esac; }
		# the walk itself is walk()'s test: here every point is reached and the walker ends 9000 u off
		npcs_walk_route() { dc "$1" "walkto 0 0" >/dev/null; return 0; }
		( A_OUT=$d/ho$RANDOM; mkdir -p "$A_OUT"; a_init ho >/dev/null; npcs_handover host vm >/dev/null; cut -f1 "$A_RESULTS" | tr '\n' ' ' )
	}
	HANDED_H=${HOST_LIST/0001A67C local=0001A67C owner=1 role=run/0001A67C local=0001A67C owner=2 role=copy}
	HANDED_V=${NEAR_LIST/0001A67C local=0001A67C owner=1 role=copy/0001A67C local=0001A67C owner=2 role=run}
	t "handover: the walk, then both books the VM's: passes" "$(ho "$HANDED_H" "$HANDED_V")" "PASS PASS "
	t "handover: the host kept it: FAIL" "$(ho "$HOST_LIST" "$NEAR_LIST")" "PASS FAIL "
	t "handover: one book only: FAIL" "$(ho "$HOST_LIST" "$HANDED_V")" "PASS FAIL "
	# picked within NPC_NEAR of the stayer before the walk, but after it the NPC is not past the release rule (it walked
	# towards the walker's end of the road): not checked, a WARN, never the mod's FAIL
	t "handover: an NPC not past the release rule after the walk is said, not failed" "$(ho "$HOST_LIST" "${NEAR_LIST/at=(130,200,0)/at=(6000,200,0)}")" "PASS WARN "
	t "handover: a route that stops is a rig fault, not a FAIL" "$( npcs_walk_route() { echo 'point 2 (x) not reached'; return 1; }; ho_route_stops() { :; }; HO_N=$d/ho_n; echo 0 > "$HO_N"
		dc() { case "$2" in 'npcs list') if [ "$1" = host ]; then echo "$HOST_LIST"; else echo "$NEAR_LIST"; fi ;; 'session status') if [ "$1" = host ]; then echo 'player=1'; else echo 'player=2'; fi ;;
			where) echo 'cell=00000000 world=0000003C pos=(120,200,0) modal=0' ;; *) echo ok ;; esac; }
		( A_OUT=$d/hr; mkdir -p "$A_OUT"; a_init hr >/dev/null; npcs_handover host vm >/dev/null; cut -f1 "$A_RESULTS" | tr '\n' ' ' ) )" "RIG "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "npcs is a step" "$(case " $STEPS " in *" npcs "*) echo yes ;; esac)" yes
	t "npcs comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' npcs')" 1
	t "npcs needs a session" "$(case " $SESSION_STEPS " in *" npcs "*) echo yes ;; esac)" yes
	t "npcs runs by default" "$(case " $OPTIN_STEPS " in *" npcs "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "npcs.sh selftest: all ok" || echo "npcs.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
