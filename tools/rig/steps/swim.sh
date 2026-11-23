#!/bin/bash
# steps/swim.sh: the rig step `swim` (protocol 11, docs/DESIGN.md "Players": "In water"), sourced by steps.sh. A friend
# who swims is shown swimming: their stand-in's actor state and graph go into the water's mode, it is held at the depth
# its friend swims at (the water's own pull does not float it up) and its body dives as its friend dives, and it walks
# out of the water with them. NPC copies in water are shown swimming the same way. Not run in a game yet.
#
#   step_swim     the step
#   bash tools/rig/steps/swim.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# The main checks read what the engine decided, not only what the plugin set: the stand-in's character controller in
# its swimming state (charState=5) or the engine's own swimming flag, how often the engine's update moved it off the
# depth it is shown at while it rests (refloats= a second: a controller that never swims is pulled down or up each
# frame and put back each frame, a jitter), and its body as drawn (body=: the rise of the line from its 3D's pelvis to
# its head, which tips down when the body dives). What the plugin set (shown=, swimming=, pitch=, depthErr=) is checked
# too, as the plugin's own part.
#
# Each way (the mover swims, the watcher's stand-in of it is read), both players outside near each other:
#   1. Water deep enough to swim in near the mover (its `swim find`: the nearest point where the water is SWIM_DEPTH
#      deeper than the land, there and all round it; none is a rig fault, and SWIM_SPOT="x y" names one).
#   2. The watcher walks to within SWIM_WATCH of it and the mover walks to within SWIM_ARRIVE of it, both with their
#      virtual pads (walkto): the game's own swimming. A mover that is not swimming there is a rig fault.
#   3. A stroke forward (the left stick): the watcher's stand-in is shown swimming (friendSwims=1 swimming=1 shown=w,
#      the plugin's part), its engine has it swimming (charState=5 or engineSwimming=1), its graph took the swim's
#      start (swimTaken=), it is at its friend's height (|depthErr|), at rest it is within SWIM_REST of its friend, and
#      at rest the engine moved it off its depth at most SWIM_REFLOAT_RATE times a second over SWIM_REST_WINDOW.
#   4. A dive (the right stick down, the left stick forward; a mover not under water then is a rig fault): the
#      stand-in's friend is under (friendUnder=1), the stand-in is at its friend's depth and went down with it, its
#      angle is its friend's pitch (pitch=, the plugin's), and its body as drawn tipped down at least SWIM_TIP degrees
#      from the surface. A body that did not tip while the mover's own did is a FAIL; when the mover's own body did not
#      tip either (or a body can't be read) it is a WARN that says both.
#   5. Up again (the right stick up, the left stick forward): its friend is not under, and the stand-in is level.
#   6. NPC copies, while the mover is still at the surface: the mover's console moves a person and a creature its game
#      runs (`npcs list`: role=run, not a child, not dead) to its player in the water (`moveto player`, set-up). Each is
#      a copy in the watcher's game; once its runner has it swimming (else RIG for that NPC: it did not swim where it
#      was put), the copy is shown swimming (shown=w), its engine has it swimming, its graph took a swim event and gave
#      none up, and at rest its engine moved it off its depth at most SWIM_REFLOAT_RATE times a second. Every copy of a
#      race that does not walk (a fish) the watcher shows was told no swim event. None of a kind run by the mover is a
#      WARN. Each NPC is put back where it stood (`setpos`, set-up).
#   7. Both walk back to where they began: the stand-in is no longer shown swimming, its graph took the swim's start and
#      end (swimTaken= 2 or more) and gave none up (swimGivenUp=0; a FAIL that says the names the mover's own graph
#      heard).
#   Said for the record: the events the mover's own graph took going in, coming out, diving and surfacing (heardIn=,
#   heardOut=, heardDive=, heardSurface=: the names the stand-ins are sent are checked against them), how often the water
#   floated a stand-in off its depth in all (refloats=) and the engine's last move of it (drift=).

SWIM_RADIUS=${SWIM_RADIUS:-4096}        # u: how far `swim find` looks from the mover
SWIM_SPOT=${SWIM_SPOT:-}                # "x y": water to swim in, when the scene has none within SWIM_RADIUS (outside)
SWIM_WAIT=${SWIM_WAIT:-10}              # s: for the game to say a player swims, and a state to reach the other game
SWIM_ARRIVE=${SWIM_ARRIVE:-80}          # u: the mover walks to within this of the spot (deep all round it: swim find)
SWIM_REACH=${SWIM_REACH:-300}           # u: a mover that stopped further than this from the spot did not get there (the walk's)
SWIM_WATCH=${SWIM_WATCH:-600}           # u: the watcher walks to within this of the spot (its stand-in of the mover in view)
SWIM_DEPTH_ERR=${SWIM_DEPTH_ERR:-40}    # u: the stand-in's height off its friend's while swimming
SWIM_DIVE_DROP=${SWIM_DIVE_DROP:-50}    # u: a dive takes the stand-in at least this far below where it swam at the surface
SWIM_PITCH_TOL=${SWIM_PITCH_TOL:-15}    # degrees: the stand-in's pitch off its friend's under water
SWIM_LEVEL=${SWIM_LEVEL:-5}             # degrees: the stand-in's pitch at the surface
SWIM_REST=${SWIM_REST:-100}             # u: the stand-in from its friend at rest in the water (it bobs)
SWIM_REST_WINDOW=${SWIM_REST_WINDOW:-5} # s: how long the refloats are counted at rest
SWIM_REFLOAT_RATE=${SWIM_REFLOAT_RATE:-2}  # a second, at rest: more is the engine fighting the depth each frame (a jitter)
SWIM_TIP=${SWIM_TIP:-15}                # degrees: a body that dives tips down at least this much from the surface

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# sw_ours <swim reply>: our player's part (before the first " | ")
sw_ours() { echo "${1%% | *}"; }
# sw_friend <swim reply>: the friend's stand-in's part ("player N ...", up to the copies' part; one friend on the rig)
sw_friend() { [[ $1 == *"| player "* ]] || return 0; local r="player ${1#*| player }"; echo "${r%% | copies*}"; }
# sw_copies <swim reply>: the NPC copies' part
sw_copies() { [[ $1 == *"| copies "* ]] && echo "copies ${1#*| copies }"; }
# sw_copy <swim reply> <key>: that copy's part ("copy <key> ...", up to the next " | "); nothing when not listed
sw_copy() { awk -v k="$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "copy" && $2 == k { print; exit }' <<<"$1"; }
# sw_copy_keys <swim reply> <walks 0|1>: the keys of the listed copies whose race walks (1) or does not (0), one a line
sw_copy_keys() { awk -v w="walks=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "copy" { for (i = 3; i <= NF; i++) if ($i == w) { print $2; break } }' <<<"$1"; }
sw_own_field() { field "$2" "$(sw_ours "$(dc "$1" swim)")"; }
sw_field() { field "$2" "$(sw_friend "$(dc "$1" swim)")"; }
# sw_spot <swim find reply>: "x y" of the spot found; nothing when none
sw_spot() { sed -n 's/^spot=(\([-0-9.]*\),\([-0-9.]*\)).*/\1 \2/p' <<<"$1"; }
# sw_abs <number>: its size, whole units; nothing when it is no number
sw_abs() { is_num "$1" && awk -v a="$1" 'BEGIN{printf "%.0f", (a < 0 ? -a : a)}'; }
# sw_pitch_off <stand-in pitch> <friend pitch>: degrees between them, the friend's held to the dive's 75
sw_pitch_off() { is_num "$1" && is_num "$2" && awk -v s="$1" -v f="$2" 'BEGIN{if (f > 75) f = 75; if (f < -75) f = -75; d = s - f; printf "%.0f", (d < 0 ? -d : d)}'; }
# sw_drop <z at the surface> <z under>: how far it went down, whole units (also a body's tip: its rise before, after)
sw_drop() { is_num "$1" && is_num "$2" && awk -v a="$1" -v b="$2" 'BEGIN{printf "%.0f", a - b}'; }
# sw_rate <count before> <count after> <seconds>: how many a second, one decimal
sw_rate() { is_num "$1" && is_num "$2" && is_num "$3" && awk -v a="$1" -v b="$2" -v s="$3" 'BEGIN{printf "%.1f", (s > 0 ? (b - a) / s : b - a)}'; }
# sw_engine_swims <part>: its engine has it swimming: its character controller's swimming state (5) or the engine's flag
sw_engine_swims() { [ "$(field charState "$1")" = 5 ] || [ "$(field engineSwimming "$1")" = 1 ]; }

# Waits' tests.
sw_swims() { [ "$(sw_own_field "$1" swimming)" = "${2:-1}" ]; }
sw_under() { [ "$(sw_own_field "$1" under)" = "${2:-1}" ]; }
sw_walk_over() { [[ $(dc "$1" walkto) == "not walking"* ]]; }
# sw_shows_in <friend part> <0|1>: that stand-in is shown swimming (1) or not (0): its friend's state, its actor state, its graph
sw_shows_in() {
	[ "$(field friendSwims "$1")" = "$2" ] && [ "$(field swimming "$1")" = "$2" ] && [ "$(field shown "$1")" = "$([ "$2" = 1 ] && echo w || echo -)" ]
}
sw_shows() { sw_shows_in "$(sw_friend "$(dc "$1" swim)")" "$2"; }
sw_friend_under() { [ "$(sw_field "$1" friendUnder)" = "$2" ]; }
# sw_copy_swims <watcher> <key>: the watcher lists that copy with its runner having it swimming
sw_copy_swims() { [ "$(field runnerSwims "$(sw_copy "$(dc "$1" swim)" "$2")")" = 1 ]; }

# sw_walk <who> <x> <y> <radius>: a walk with the virtual pad to within the radius of a point (90 s); prints how far
# from it the walker stopped (fails, printing why, when the walk was refused)
sw_walk() {
	local r
	r=$(dc "$1" "walkto $2 $3 1.0 $4 90")
	[[ $r == walking* ]] || { echo "refused: $r"; return 1; }
	wait_until 100 sw_walk_over "$1"; still "$1"
	dist2 "$(pos_of "$1")" "$2 $3 0"
}

# sw_back <mover> <its start "x y z"> <watcher> <its start>: both walk back to where they began
sw_back() {
	local p
	read -r p _ <<<"$2"; sw_walk "$1" "$p" "$(awk '{print $2}' <<<"$2")" 150 >/dev/null
	read -r p _ <<<"$4"; sw_walk "$3" "$p" "$(awk '{print $2}' <<<"$4")" 150 >/dev/null
}

# sw_pick <npcs list rows> <runner's player id> <kinds, a regex>: "<key> <local id> <x> <y> <z>" of the first NPC that
# runner's game runs of those kinds, alive, no child, not a spawn; nothing when none
sw_pick() { awk -v me="$2" -v kinds="^($3)\$" '$2 == me && $3 == "run" && $4 ~ kinds && $15 == "0" && $16 != "1" && $1 !~ /:/ && $14 != "-" { print $1, $14, $5, $6, $7; exit }' <<<"$1"; }

# swim_copies <mover> <watcher> <label>: step 6, the NPC copies in the water by the mover's player
swim_copies() {
	local m=$1 w=$2 label="$3: NPC copies" cid rows kind pick key local x y z keys="" k c r0 r1 rate fish told
	local -A where=() kindof=()
	cid=$(field player "$(dc "$m" 'session status')")
	rows=$(npc_rows "$(dc "$m" 'npcs list')")
	for kind in "human|follower" "creature|horse"; do
		pick=$(sw_pick "$rows" "$cid" "$kind")
		if [ -z "$pick" ]; then
			warn "$label: a ${kind%%|*} the $m's game runs is shown swimming in the $w" "the $m runs none near (npcs list: no role=run ${kind/|/ or } of player ${cid:-?}, alive, not a child)"
			continue
		fi
		read -r key local x y z <<<"$pick"
		r=$(dc "$m" "console $local.moveto player")
		if [ "$r" != ran ]; then
			rig_fault state "$label: the $m's ${kind%%|*} $key moved into the water (set-up)" "the console refused $local.moveto player: $r"
			continue
		fi
		say "swim: the $m's ${kind%%|*} $key (here $local) moved to its player in the water with the console (set-up; from ($x,$y,$z))"
		where[$key]="$local $x $y $z"; kindof[$key]=${kind%%|*}; keys="$keys $key"
	done
	for key in $keys; do
		if ! wait_until "$SWIM_WAIT" sw_copy_swims "$w" "$key"; then
			rig_fault state "$label: the $m's ${kindof[$key]} $key swims where it was put (set-up)" "the $w's copy of it is not listed with its runner swimming: $(sw_copies "$(dc "$w" swim)" | cut -c1-240)"
			keys=${keys/ $key/}
		fi
	done
	sleep 2
	c=$(dc "$w" swim)
	for key in $keys; do
		k=$(sw_copy "$c" "$key")
		if [ "$(field shown "$k")" = w ]; then
			pass "$label: the $w's copy of the $m's ${kindof[$key]} $key is shown swimming (shown=w)" "${k:0:240}"
		else
			fail "$label: the $w's copy of the $m's ${kindof[$key]} $key is shown swimming (shown=w)" "${k:0:240}"
		fi
		if sw_engine_swims "$k"; then
			pass "$label: the $w's engine has its copy of the $m's ${kindof[$key]} $key swimming (charState=5 or engineSwimming=1)" "charState=$(field charState "$k") engineSwimming=$(field engineSwimming "$k")"
		else
			fail "$label: the $w's engine has its copy of the $m's ${kindof[$key]} $key swimming (charState=5 or engineSwimming=1)" "charState=$(field charState "$k") engineSwimming=$(field engineSwimming "$k"): its controller is not in the water's state; drift=$(field drift "$k") refloats=$(field refloats "$k")"
		fi
		if [ "$(field walks "$k")" = 1 ] && is_num "$(field swimTaken "$k")" && [ "$(field swimTaken "$k")" -ge 1 ] && [ "$(field swimGivenUp "$k")" = 0 ]; then
			pass "$label: the $w's copy of the $m's ${kindof[$key]} $key: its graph took the swim's start and gave none up" "walks=1 swimTaken=$(field swimTaken "$k") swimRefused=$(field swimRefused "$k") swimGivenUp=0"
		else
			fail "$label: the $w's copy of the $m's ${kindof[$key]} $key: its graph took the swim's start and gave none up" "walks=$(field walks "$k") swimTaken=$(field swimTaken "$k") swimRefused=$(field swimRefused "$k") swimGivenUp=$(field swimGivenUp "$k") (base $(field base "$k"))"
		fi
	done
	# At rest, every copy checked: how often its engine moved it off its depth.
	declare -A before=()
	for key in $keys; do before[$key]=$(field refloats "$(sw_copy "$c" "$key")"); done
	if [ -n "$keys" ]; then
		sleep "$SWIM_REST_WINDOW"; c=$(dc "$w" swim)
		for key in $keys; do
			k=$(sw_copy "$c" "$key")
			rate=$(sw_rate "${before[$key]}" "$(field refloats "$k")" "$SWIM_REST_WINDOW")
			expect_le "$label: at rest, the $w's engine moves its copy of the $m's ${kindof[$key]} $key off its depth at most $SWIM_REFLOAT_RATE times a second (refloats; drift=$(field drift "$k"))" "$rate" "$SWIM_REFLOAT_RATE" /s
		done
	fi
	# A copy of a race that does not walk (a fish): never told to swim.
	fish=$(sw_copy_keys "$c" 0)
	if [ -z "$fish" ]; then
		say "swim: no copy of a race that does not walk (a fish) in the $w's water: its rule is unit-tested (tests/GaitTests.cpp)"
	fi
	for key in $fish; do
		k=$(sw_copy "$c" "$key")
		told=$(awk -v a="$(field swimTaken "$k")" -v b="$(field swimRefused "$k")" 'BEGIN { if (a !~ /^[0-9]+$/ || b !~ /^[0-9]+$/) print "?"; else print a + b }')
		if [ "$told" = 0 ]; then
			pass "$label: the $w's copy $key of a race that does not walk (a fish) was told no swim event" "${k:0:200}"
		else
			fail "$label: the $w's copy $key of a race that does not walk (a fish) was told no swim event" "${k:0:200}"
		fi
	done
	for key in "${!where[@]}"; do
		read -r local x y z <<<"${where[$key]}"
		if is_num "$x" && is_num "$y" && is_num "$z"; then
			dc "$m" "console $local.setpos x $x" >/dev/null; dc "$m" "console $local.setpos y $y" >/dev/null; dc "$m" "console $local.setpos z $z" >/dev/null
			say "swim: the $m's ${kindof[$key]} $key put back at ($x,$y,$z) with the console (set-up)"
		else
			say "swim: the $m's ${kindof[$key]} $key left in the water: where it stood was not known"
		fi
	done
}

swim_way() {
	local m=$1 w=$2 label="swim: the $1 swims, the $2 watches" p0 q0 f x y d o p z0 dz off fp dived0 surfaced0 heard r0 r1 sb0 ob0 sb1 ob1 st ot
	section "$label"
	if ! wait_until "$SWIM_WAIT" placed "$w"; then
		fail "$label: the $w has a stand-in of the $m's player" "$(dc "$w" standins | cut -c1-200)"
		return
	fi
	p0=$(pos_of "$m"); q0=$(pos_of "$w")
	if [ -z "$p0" ] || [ -z "$q0" ]; then rig_broken game "$label" "no place for a player (mover '$p0', watcher '$q0')"; return; fi

	# 1. the water
	if [ -n "$SWIM_SPOT" ]; then
		read -r x y <<<"$SWIM_SPOT"
		say "swim: the water named by SWIM_SPOT: ($x,$y)"
	else
		f=$(dc "$m" "swim find $SWIM_RADIUS")
		read -r x y <<<"$(sw_spot "$f")"
		if [ -z "$y" ]; then
			rig_fault state "$label: water to swim in near the scene (set-up)" "swim find: ${f:0:160}; SWIM_SPOT=\"x y\" names a lake's or a river's deep water near $SMOKE_CELL"
			return
		fi
		say "swim: water $(field depth "$f") u deep at ($x,$y), deep all round it, $(field dist "$f") u from the $m's player ($(field looked "$f") points read)"
	fi

	# 2. the watcher near it, the mover into it
	d=$(sw_walk "$w" "$x" "$y" "$SWIM_WATCH") || { rig_fault input "$label: the $w walks to the water (set-up)" "$d"; return; }
	dived0=$(sw_own_field "$m" dived); surfaced0=$(sw_own_field "$m" surfaced)
	d=$(sw_walk "$m" "$x" "$y" "$SWIM_ARRIVE") || { rig_fault input "$label: the $m walks into the water (set-up)" "$d"; sw_back "$m" "$p0" "$w" "$q0"; return; }
	if ! wait_until "$SWIM_WAIT" sw_swims "$m"; then
		if ! is_num "$d" || [ "$d" -gt "$SWIM_REACH" ]; then
			rig_fault walk "$label: the $m walks into the water (set-up)" "it stopped ${d:-?} u from ($x,$y)"
		else
			rig_fault state "$label: the $m's player swims in the water found (set-up)" "${d} u from ($x,$y) and not swimming: $(sw_ours "$(dc "$m" swim)" | cut -c1-240)"
		fi
		sw_back "$m" "$p0" "$w" "$q0"
		return
	fi
	say "swim: the $m's player swims ($d u from the spot): $(sw_ours "$(dc "$m" swim)" | cut -c1-200)"

	# 3. a stroke forward
	dc "$m" 'pad stick l 0 0.8 3000' >/dev/null
	if wait_until "$SWIM_WAIT" sw_shows "$w" 1; then
		pass "$label: the $w's stand-in is shown swimming as the $m's player swims (friendSwims=1 swimming=1 shown=w: the plugin's part)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-240)"
	else
		fail "$label: the $w's stand-in is shown swimming as the $m's player swims (friendSwims=1 swimming=1 shown=w: the plugin's part)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-240)"
	fi
	sleep 3; still "$m"; sleep 1
	p=$(sw_friend "$(dc "$w" swim)")
	if sw_engine_swims "$p"; then
		pass "$label: the $w's engine has the stand-in swimming (charState=5 or engineSwimming=1)" "charState=$(field charState "$p") engineSwimming=$(field engineSwimming "$p") inWater=$(field inWater "$p")"
	else
		fail "$label: the $w's engine has the stand-in swimming (charState=5 or engineSwimming=1)" "charState=$(field charState "$p") engineSwimming=$(field engineSwimming "$p") inWater=$(field inWater "$p"): its character controller is not in the water's state (drift=$(field drift "$p") refloats=$(field refloats "$p"))"
	fi
	if is_num "$(field swimTaken "$p")" && [ "$(field swimTaken "$p")" -gt 0 ]; then
		pass "$label: the $w's stand-in's graph took the swim's start (swimTaken=)" "swimTaken=$(field swimTaken "$p") swimRefused=$(field swimRefused "$p") charState=$(field charState "$p")"
	else
		fail "$label: the $w's stand-in's graph took the swim's start (swimTaken=)" "swimTaken=$(field swimTaken "$p") swimRefused=$(field swimRefused "$p") swimGivenUp=$(field swimGivenUp "$p"); the $m's own graph took on going in: $(sw_own_field "$m" heardIn)"
	fi
	expect_le "$label: the $w's stand-in swims at its friend's height (|depthErr|, the plugin's part)" "$(sw_abs "$(field depthErr "$p")")" "$SWIM_DEPTH_ERR" u
	expect_le "$label: at rest, the $w's stand-in is where its friend swims" "$(field dist "$(dc "$w" standins)")" "$SWIM_REST" u
	z0=$(field z "$p"); sb0=$(field body "$p"); ob0=$(sw_own_field "$m" body)
	r0=$(field refloats "$p"); sleep "$SWIM_REST_WINDOW"; p=$(sw_friend "$(dc "$w" swim)"); r1=$(field refloats "$p")
	expect_le "$label: at rest, the $w's engine moves the stand-in off its depth at most $SWIM_REFLOAT_RATE times a second (refloats $r0 -> $r1 in ${SWIM_REST_WINDOW} s; drift=$(field drift "$p"))" \
		"$(sw_rate "$r0" "$r1" "$SWIM_REST_WINDOW")" "$SWIM_REFLOAT_RATE" /s

	# 4. a dive
	dc "$m" 'pad stick r 0 -1 700' >/dev/null; sleep 1
	dc "$m" 'pad stick l 0 1 4000' >/dev/null
	if ! wait_until 6 sw_under "$m"; then
		rig_fault input "$label: the $m's player dives (the right stick down, the left stick forward; set-up)" "$(sw_ours "$(dc "$m" swim)" | cut -c1-240)"
		still "$m"; sw_back "$m" "$p0" "$w" "$q0"
		return
	fi
	sleep 1
	if wait_until 3 sw_friend_under "$w" 1; then
		pass "$label: the $w's stand-in's friend is under water (friendUnder=1)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-160)"
	else
		fail "$label: the $w's stand-in's friend is under water (friendUnder=1)" "the $m's player is under ($(sw_ours "$(dc "$m" swim)" | cut -c1-120)) but its state came without it: $(sw_friend "$(dc "$w" swim)" | cut -c1-160)"
	fi
	p=$(sw_friend "$(dc "$w" swim)"); ob1=$(sw_own_field "$m" body); sb1=$(field body "$p")
	expect_le "$label: under water, the $w's stand-in is at its friend's depth (|depthErr|, the plugin's part)" "$(sw_abs "$(field depthErr "$p")")" "$SWIM_DEPTH_ERR" u
	dz=$(sw_drop "$z0" "$(field z "$p")")
	expect_ge "$label: the $w's stand-in went down with its friend (below where it swam at the surface)" "$dz" "$SWIM_DIVE_DROP" u
	fp=$(field friendPitch "$p")
	if is_num "$fp" && [ "${fp#-}" -ge 10 ]; then
		off=$(sw_pitch_off "$(field pitch "$p")" "$fp")
		expect_le "$label: the $w's stand-in's angle is its friend's pitch under water (pitch=$(field pitch "$p") friendPitch=$fp: the plugin's part)" "$off" "$SWIM_PITCH_TOL" deg
	else
		warn "$label: the $w's stand-in's angle is its friend's pitch under water" "its friend swam at ${fp:-?} degrees: too level to judge (the right stick's push moved the view too little)"
	fi
	# Its body as the engine drew it: tipped down as it dives (from its 3D, not the angle the plugin set).
	st=$(sw_drop "$sb0" "$sb1"); ot=$(sw_drop "$ob0" "$ob1")
	if ! is_num "$fp" || [ "$fp" -gt "-$SWIM_TIP" ]; then
		warn "$label: the $w's stand-in's body dives as its friend's does (body=)" "its friend swam at ${fp:-?} degrees: not down enough to judge; body tip ${st:-?} (stand-in), ${ot:-?} (the $m's own)"
	elif ! is_num "$st"; then
		warn "$label: the $w's stand-in's body dives as its friend's does (body=)" "its body can't be read (body=$sb0 at the surface, $sb1 under): look at it"
	elif [ "$st" -ge "$SWIM_TIP" ]; then
		pass "$label: the $w's stand-in's body dives as its friend's does (body= tipped $st degrees)" "stand-in body $sb0 -> $sb1; the $m's own $ob0 -> $ob1; friendPitch=$fp"
	elif is_num "$ot" && [ "$ot" -ge "$SWIM_TIP" ]; then
		fail "$label: the $w's stand-in's body dives as its friend's does (body= tipped $st degrees)" "stand-in body $sb0 -> $sb1 while the $m's own tipped $ot ($ob0 -> $ob1): its angle was set (pitch=$(field pitch "$p")) but its body was drawn level"
	else
		warn "$label: the $w's stand-in's body dives as its friend's does (body= tipped $st degrees)" "neither body tipped $SWIM_TIP degrees: stand-in $sb0 -> $sb1, the $m's own ${ob0:-?} -> ${ob1:-?} (its third-person body may not be read in first person): look at it"
	fi
	still "$m"

	# 5. up again
	dc "$m" 'pad stick r 0 1 1000' >/dev/null; sleep 1.2
	dc "$m" 'pad stick l 0 1 5000' >/dev/null
	if ! wait_until 8 sw_under "$m" 0; then
		rig_fault input "$label: the $m's player comes up (the right stick up, the left stick forward; set-up)" "$(sw_ours "$(dc "$m" swim)" | cut -c1-240)"
		still "$m"; sw_back "$m" "$p0" "$w" "$q0"
		return
	fi
	still "$m"
	if wait_until 3 sw_friend_under "$w" 0; then
		pass "$label: the $w's stand-in's friend came up (friendUnder=0)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-160)"
	else
		fail "$label: the $w's stand-in's friend came up (friendUnder=0)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-160)"
	fi
	sleep 1
	p=$(sw_friend "$(dc "$w" swim)")
	expect_le "$label: at the surface the $w's stand-in is level (|pitch|)" "$(sw_abs "$(field pitch "$p")")" "$SWIM_LEVEL" deg
	say "swim: at the surface again, the $w's stand-in's body $(field body "$p") (it was $sb0 before the dive, $sb1 under)"
	dc "$m" 'pad stick r 0 -1 500' >/dev/null  # the view back near level for what comes next

	# 6. NPC copies in the water by the mover
	swim_copies "$m" "$w" "$label"

	# 7. out of the water
	sw_back "$m" "$p0" "$w" "$q0"
	if ! wait_until "$SWIM_WAIT" sw_swims "$m" 0; then
		rig_fault walk "$label: the $m walks out of the water (set-up)" "$(sw_ours "$(dc "$m" swim)" | cut -c1-200)"
		return
	fi
	if wait_until "$SWIM_WAIT" sw_shows "$w" 0; then
		pass "$label: the $w's stand-in walks out of the water with its friend (friendSwims=0 swimming=0 shown=-)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-200)"
	else
		fail "$label: the $w's stand-in walks out of the water with its friend (friendSwims=0 swimming=0 shown=-)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-200)"
	fi
	p=$(sw_friend "$(dc "$w" swim)")
	o=$(sw_ours "$(dc "$m" swim)")
	heard="going in: $(field heardIn "$o"); coming out: $(field heardOut "$o"); diving: $(field heardDive "$o"); surfacing: $(field heardSurface "$o")"
	expect_ge "$label: the $w's stand-in's graph took the swim's start and end (swimTaken=)" "$(field swimTaken "$p")" 2
	if [ "$(field swimGivenUp "$p")" = 0 ]; then
		pass "$label: no swim event given up by the $w's stand-in's graph (swimGivenUp=)" "swimRefused=$(field swimRefused "$p")"
	else
		fail "$label: no swim event given up by the $w's stand-in's graph (swimGivenUp=)" "swimGivenUp=$(field swimGivenUp "$p") swimRefused=$(field swimRefused "$p"): the stand-ins are sent SwimStart and SwimStop; the $m's own graph took, $heard"
	fi
	say "swim: the $m's own graph took, $heard (dived $(field dived "$o") times, ${dived0:-?} before; surfaced $(field surfaced "$o"), ${surfaced0:-?} before)"
	say "swim: the water floated the $w's stand-in off its depth $(field refloats "$p") time(s) in all (refloats=; each put back)"
}

step_swim() {
	local w
	for w in host vm; do still $w; done
	swim_way vm host
	swim_way host vm
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init swimtest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Water::Command: ours | RemotePlayers::Swim | NpcSync::Swim)
	S='ours swimming=1 engineSwimming=1 inWater=1 under=0 engineUnder=0 submerged=0.71 charState=5 surface=1 shoulders=1 controllerWater=-20 z=-130 water=-20 depth=110 pitch=-3 body=12 entered=1 left=0 dived=0 surfaced=0 heardIn=SwimStart,moveStart heardOut=none heardDive=none heardSurface=none | player 2 standin=FF0008AE friendSwims=1 friendUnder=0 shown=w swimming=1 engineSwimming=1 inWater=1 charState=5 z=-128 friendZ=-130 depthErr=2 water=-20 pitch=-3 friendPitch=-3 body=10 swimTaken=1 swimRefused=0 swimGivenUp=0 refloats=4 drift=3 | copies swimming=1 under=0 shownSwimming=1 swimTaken=1 swimRefused=0 swimGivenUp=0 refloats=0 | copy 0001A67C base=00013BA2 walks=1 runnerSwims=1 shown=w under=0 engineSwimming=1 charState=5 swimTaken=1 swimRefused=0 swimGivenUp=0 refloats=0 drift=1 z=-140 runnerZ=-141 depthErr=1 | copy 0002B000 base=00023ABA walks=0 runnerSwims=0 shown=- under=0 engineSwimming=0 charState=5 swimTaken=0 swimRefused=0 swimGivenUp=0 refloats=0 drift=0 z=-300 runnerZ=-300 depthErr=0'
	t "ours: our part only" "$(field pitch "$(sw_ours "$S")")" -3
	t "ours: not the stand-in's fields" "$(field friendSwims "$(sw_ours "$S")")" ""
	t "ours: the heard names" "$(field heardIn "$(sw_ours "$S")")" "SwimStart,moveStart"
	t "ours: the controller's state" "$(field charState "$(sw_ours "$S")")" 5
	t "ours: its body" "$(field body "$(sw_ours "$S")")" 12
	t "friend: the stand-in's part" "$(field depthErr "$(sw_friend "$S")")" 2
	t "friend: its swimming=, not engineSwimming=" "$(field swimming "$(sw_friend "$S")")" 1
	t "friend: not the copies' fields" "$(field shownSwimming "$(sw_friend "$S")")" ""
	t "friend: its refloats, not the copies'" "$(field refloats "$(sw_friend "$S")")" 4
	t "friend: its body, not ours" "$(field body "$(sw_friend "$S")")" 10
	t "friend: none without a friend" "$(sw_friend 'ours swimming=0 | copies swimming=0')" ""
	t "copies: their part" "$(field shownSwimming "$(sw_copies "$S")")" 1
	t "copy: its own part" "$(field runnerZ "$(sw_copy "$S" 0001A67C)")" -141
	t "copy: the next one's fields not in it" "$(sw_copy "$S" 0001A67C | grep -c 0002B000)" 0
	t "copy: the last one" "$(field walks "$(sw_copy "$S" 0002B000)")" 0
	t "copy: none not listed" "$(sw_copy "$S" 0001A680)" ""
	t "copy keys: of races that walk" "$(sw_copy_keys "$S" 1)" 0001A67C
	t "copy keys: of races that do not (a fish)" "$(sw_copy_keys "$S" 0)" 0002B000
	t "engine swims: its controller" "$(sw_engine_swims 'charState=5 engineSwimming=0' && echo yes)" yes
	t "engine swims: its flag" "$(sw_engine_swims 'charState=0 engineSwimming=1' && echo yes)" yes
	t "engine swims: neither" "$(sw_engine_swims 'charState=0 engineSwimming=0' || echo no)" no
	t "shows: swimming" "$(sw_shows_in "$(sw_friend "$S")" 1 && echo yes)" yes
	t "shows: not while its graph was not told" "$(sw_shows_in "$(sw_friend "${S/shown=w/shown=-}")" 1 || echo no)" no
	t "shows: not while its actor state says otherwise" "$(sw_shows_in "$(sw_friend "${S/ swimming=1 engineSwimming=1 inWater=1 charState/ swimming=0 engineSwimming=1 inWater=1 charState}")" 1 || echo no)" no
	t "spot" "$(sw_spot 'spot=(18200,-46900) depth=240 dist=1310 radius=4096 looked=310 from=(17225,-47204)')" "18200 -46900"
	t "spot: none" "$(sw_spot 'none radius=4096 looked=3217 from=(17225,-47204)')" ""
	t "spot: an error" "$(sw_spot 'error: our player is inside: the search reads the land, which only the outside has')" ""
	t "abs" "$(sw_abs -42)" 42
	t "abs: none" "$(sw_abs none)" ""
	t "pitch off: as its friend" "$(sw_pitch_off -38 -40)" 2
	t "pitch off: the friend's held to 75" "$(sw_pitch_off -75 -90)" 0
	t "pitch off: level when its friend dives" "$(sw_pitch_off 0 -40)" 40
	t "pitch off: none" "$(sw_pitch_off none -40)" ""
	t "drop" "$(sw_drop -128 -310)" 182
	t "drop: a body's tip" "$(sw_drop 10 -35)" 45
	t "drop: none" "$(sw_drop none -35)" ""
	t "rate" "$(sw_rate 4 14 5)" 2.0
	t "rate: none" "$(sw_rate none 14 5)" ""
	PICK_ROWS=$(npc_rows 'npcs=5 | 0001A67C local=0001A67C owner=1 role=copy kind=human at=(1,2,3) dead=0 child=0 | 0001A690 local=0001A690 owner=2 role=run kind=human at=(10,20,30) dead=0 child=1 | 0001A691 local=0001A691 owner=2 role=run kind=human at=(11,21,31) dead=1 child=0 | FF000801 spawner=2 local=FF000801 owner=2 role=run kind=creature at=(5,5,5) dead=0 child=0 | 0001A692 local=0001A692 owner=2 role=run kind=follower at=(12,22,32) dead=0 child=0 | 0001A693 local=0001A693 owner=2 role=run kind=horse at=(13,23,33) dead=0 child=0')
	t "pick: a person this game runs, no child, alive (a follower counts)" "$(sw_pick "$PICK_ROWS" 2 'human|follower')" "0001A692 0001A692 12 22 32"
	t "pick: a creature, not a spawn (a horse counts)" "$(sw_pick "$PICK_ROWS" 2 'creature|horse')" "0001A693 0001A693 13 23 33"
	t "pick: none run by that game" "$(sw_pick "$PICK_ROWS" 1 'human|follower')" ""

	# The step's way, both games faked: the mover walks into the water at the spot and swims there, dives when its view
	# is down and its left stick pushed, comes up when its view is up, walks out at where it began; the watcher's
	# stand-in follows as the plugin would, its body drawn tipping with its friend's. Its console moves the NPCs it runs
	# into the water, where the watcher's copies of them swim; a fish's copy is always there. FW_MODE: ok, nospot (no
	# water near: the rig's), dry (the spot is not water the mover swims in: the rig's), noshow (the stand-in is never
	# shown swimming), nocontrol (the stand-in's engine never has it swimming), jitter (at rest the engine moves it off
	# its depth every frame), refused (its graph turns the swim events away: given up), float (under water the stand-in
	# stays at the surface), flat (under water its angle and its body are level), stiff (its angle is set but its body is
	# drawn level), bothstiff (neither body tips: can't judge), nodive (the dive does not take the mover under: the
	# rig's), stuck (out of the water it is still shown swimming), nounder (its friend's state never says under),
	# copyrefused (the creature's copy's graph turns the swim events away), copydry (the NPCs moved do not swim: the
	# rig's), nocopies (the mover's game runs no NPC), fishtold (the fish's copy was told to swim).
	fake_swim() {
		FW=$d/fw.$RANDOM; mkdir -p "$FW"; FW_MODE=$1
		echo "0 0" > "$FW/vm"; echo "300 0" > "$FW/host"; echo 0 > "$FW/swim"; echo 0 > "$FW/under"; echo 0 > "$FW/view"; echo 0 > "$FW/taken"
		echo 3 > "$FW/refl"; echo 0 > "$FW/moved"
		fw_set() { local was; was=$(cat "$FW/swim"); [ "$was" != "$1" ] && echo $(( $(cat "$FW/taken") + 1 )) > "$FW/taken"; echo "$1" > "$FW/swim"; }
		# fw_body <swimming> <under> <view> <tips 0|1>: a body's rise as drawn
		fw_body() { if [ "$1" = 0 ]; then echo 85; elif [ "$2" = 1 ] && [ "$4" = 1 ]; then echo $((10 + $3)); else echo 10; fi; }
		fw_copies() {
			local s m out="" ct=5 tk=1 rf=0 gu=0 sh=w ft=0
			s=$(cat "$FW/swim"); m=$(cat "$FW/moved")
			[ "$FW_MODE" = copyrefused ] && { sh=-; tk=0; rf=3; gu=1; }
			[ "$FW_MODE" = fishtold ] && ft=1
			if [ "$s" = 1 ] && [ "$m" = 1 ] && [ "$FW_MODE" != copydry ]; then
				out=" | copy 0001A692 base=00013BA2 walks=1 runnerSwims=1 shown=w under=0 engineSwimming=1 charState=5 swimTaken=1 swimRefused=0 swimGivenUp=0 refloats=2 drift=1 z=-130 runnerZ=-131 depthErr=1"
				out="$out | copy 0001A693 base=00068D5C walks=1 runnerSwims=1 shown=$sh under=0 engineSwimming=1 charState=$ct swimTaken=$tk swimRefused=$rf swimGivenUp=$gu refloats=2 drift=1 z=-130 runnerZ=-131 depthErr=1"
			fi
			echo "$out | copy 0002B000 base=00023ABA walks=0 runnerSwims=0 shown=- under=0 engineSwimming=0 charState=5 swimTaken=$ft swimRefused=0 swimGivenUp=0 refloats=0 drift=0 z=-300 runnerZ=-300 depthErr=0"
		}
		dc() { local s u v z fz fu pt tk gu sh sw cs es tip ob sb rf; case "$1 $2" in
			"vm swim find"*) [ "$FW_MODE" = nospot ] && echo "none radius=4096 looked=3217 from=(0,0)" || echo "spot=(2000,1000) depth=240 dist=2236 radius=4096 looked=310 from=(0,0)" ;;
			*" walkto "[0-9-]*)
				set -- "$1" ${2#walkto }; echo "$2 $3" > "$FW/$1"
				if [ "$1" = vm ]; then
					if [ "$2 $3" = "2000 1000" ]; then [ "$FW_MODE" = dry ] || fw_set 1; else fw_set 0; echo 0 > "$FW/under"; fi
				fi
				echo "walking to ($2,$3) from (0,0), 10 away, speed 1.00, radius 150" ;;
			*" walkto") echo "not walking; last: arrived" ;;
			"vm pad stick r 0 -1 700") echo -45 > "$FW/view"; echo "stick r for 700 ms" ;;
			"vm pad stick r 0 1 1000") echo 30 > "$FW/view"; echo "stick r for 1000 ms" ;;
			"vm pad stick l 0 1 "*)
				v=$(cat "$FW/view")
				if [ "$(cat "$FW/swim")" = 1 ] && [ "$FW_MODE" != nodive ]; then [ "$v" -lt 0 ] && echo 1 > "$FW/under"; [ "$v" -gt 0 ] && echo 0 > "$FW/under"; fi
				echo "stick l for 4000 ms" ;;
			*" where") echo "cell=00000000 world=0000003C pos=($(sed 's/ /,/' "$FW/$1"),-100) heading=1.000 speed=0 modal=0" ;;
			"host standins") echo "players=1 standins=1 | player 1 standin=FF0008AE placed=1 dressed=1 at=($(sed 's/ /,/' "$FW/vm"),-100) heading=1.000 dist=12 named=friend" ;;
			"vm session status") echo "role=client connection=up welcomed=1 player=2 host=0 players=2" ;;
			"vm npcs list")
				if [ "$FW_MODE" = nocopies ]; then echo "npcs=1 | 0001A67C local=0001A67C owner=1 role=copy kind=human at=(1,2,3) dead=0 child=0"
				else echo "npcs=3 | 0001A67C local=0001A67C owner=1 role=copy kind=human at=(1,2,3) dead=0 child=0 | 0001A692 local=0001A692 owner=2 role=run kind=human at=(12,22,32) dead=0 child=0 | 0001A693 local=0001A693 owner=2 role=run kind=horse at=(13,23,33) dead=0 child=0"; fi ;;
			"vm console "*".moveto player") echo 1 > "$FW/moved"; echo ran ;;
			"vm swim")
				s=$(cat "$FW/swim"); u=$(cat "$FW/under"); v=$(cat "$FW/view")
				tip=1; [ "$FW_MODE" = bothstiff ] && tip=0
				ob=$(fw_body "$s" "$u" "$v" "$tip")
				echo "ours swimming=$s engineSwimming=$s inWater=$s under=$u engineUnder=$u submerged=0.70 charState=$([ "$s" = 1 ] && echo 5 || echo 0) z=$([ "$u" = 1 ] && echo -300 || echo -130) water=-20 depth=110 pitch=$v body=$ob entered=1 left=1 dived=1 surfaced=1 heardIn=SwimStart heardOut=SwimStop heardDive=none heardSurface=none | copies swimming=0 under=0 shownSwimming=0 swimTaken=0 swimRefused=0 swimGivenUp=0 refloats=0" ;;
			"host swim")
				s=$(cat "$FW/swim"); u=$(cat "$FW/under"); v=$(cat "$FW/view"); tk=$(cat "$FW/taken"); gu=0; sh=$s; sw=$s; cs=$([ "$s" = 1 ] && echo 5 || echo 0); es=$s
				fz=$([ "$u" = 1 ] && echo -300 || echo -130); z=$fz
				[ "$FW_MODE" = float ] && z=-130
				pt=$([ "$u" = 1 ] && echo "$v" || echo 0)
				fu=$u; [ "$FW_MODE" = nounder ] && { fu=0; pt=0; }  # its height still comes; no dive pitch without the flag
				tip=$fu
				case "$FW_MODE" in flat) pt=0; tip=0 ;; stiff|bothstiff) tip=0 ;; esac
				sb=$(fw_body "$s" "$u" "$v" "$tip")
				[ "$FW_MODE" = noshow ] && { sh=0; sw=0; }
				[ "$FW_MODE" = nocontrol ] && { cs=0; es=0; }
				[ "$FW_MODE" = refused ] && { tk=0; gu=$(( $(cat "$FW/taken") > 0 ? 1 : 0 )); }
				[ "$FW_MODE" = stuck ] && [ "$(cat "$FW/taken")" -ge 2 ] && { sh=1; sw=1; }
				rf=$(cat "$FW/refl"); [ "$FW_MODE" = jitter ] && [ "$s" = 1 ] && echo $((rf + 300)) > "$FW/refl"
				echo "ours swimming=0 under=0 body=85 | player 1 standin=FF0008AE friendSwims=$s friendUnder=$fu shown=$([ "$sh" = 1 ] && echo w || echo -) swimming=$sw engineSwimming=$es inWater=$s charState=$cs z=$z friendZ=$fz depthErr=$((z - fz)) water=-20 pitch=$pt friendPitch=$v body=$sb swimTaken=$tk swimRefused=0 swimGivenUp=$gu refloats=$rf drift=2 | copies swimming=0 under=0 shownSwimming=0 swimTaken=0 swimRefused=0 swimGivenUp=0 refloats=0$(fw_copies)" ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fw$RANDOM; mkdir -p "$A_OUT"; a_init fw >/dev/null; swim_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: shown (1), engine (1), graph took (1), depth (1), at rest (1), refloat rate (1); under (1), its depth (1), went
	# down (1), angle (1), body (1); came up (1), level (1); copies: two NPCs x shown, engine, events, refloat rate (8),
	# the fish told nothing (1); out (1), took both (1), none given up (1): 25
	t "way: all of it works" "$(fake_swim ok)" "PASS:25 "
	t "way: no water near is the rig's, and nothing else is tried" "$(fake_swim nospot)" "RIG:1 "
	t "way: a spot the mover does not swim in is the rig's" "$(fake_swim dry)" "RIG:1 "
	# noshow: not shown swimming (1); the rest of it holds (its engine, depth, body), and out of the water it is not
	t "way: a stand-in never shown swimming fails" "$(fake_swim noshow | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	t "way: a stand-in its engine never has swimming fails, whatever the plugin set" "$(fake_swim nocontrol | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	t "way: a stand-in the engine moves off its depth every frame (a jitter) fails" "$(fake_swim jitter | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	# refused: no start taken (1), nor both (1), and given up (1)
	t "way: a graph that turns the swim events away fails, the names heard said" "$(fake_swim refused | grep -o 'FAIL:[0-9]*')" "FAIL:3"
	t "way: a stand-in the water floats up fails: its depth and its dive" "$(fake_swim float | grep -o 'FAIL:[0-9]*')" "FAIL:2"
	t "way: a stand-in level under water fails: its angle and its body" "$(fake_swim flat | grep -o 'FAIL:[0-9]*')" "FAIL:2"
	t "way: a body drawn level while its angle is set and its friend's body dives fails" "$(fake_swim stiff | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	t "way: no body tipping, the mover's own neither, is a WARN to look, not a FAIL" "$(fake_swim bothstiff | grep -o 'FAIL:[0-9]*\|WARN:[0-9]*' | tr '\n' ' ')" "WARN:1 "
	t "way: a dive that does not go under is the rig's, after the checks at the surface" "$(fake_swim nodive)" "PASS:6 RIG:1 "
	t "way: a stand-in still shown swimming out of the water fails" "$(fake_swim stuck | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	# nounder: its friend never under (1), the stand-in's angle (1) and body (1) level under water
	t "way: a state that never says under fails" "$(fake_swim nounder | grep -o 'FAIL:[0-9]*')" "FAIL:3"
	# copyrefused: the creature's copy not shown swimming (1), its events given up (1)
	t "way: a copy whose graph turns the swim events away fails" "$(fake_swim copyrefused | grep -o 'FAIL:[0-9]*')" "FAIL:2"
	t "way: NPCs that do not swim where they were put are the rig's, one each" "$(fake_swim copydry | grep -o 'FAIL:[0-9]*\|RIG:[0-9]*' | tr '\n' ' ')" "RIG:2 "
	t "way: a mover's game that runs no NPC is a WARN a kind" "$(fake_swim nocopies | grep -o 'FAIL:[0-9]*\|WARN:[0-9]*' | tr '\n' ' ')" "WARN:2 "
	t "way: a fish's copy told to swim fails" "$(fake_swim fishtold | grep -o 'FAIL:[0-9]*')" "FAIL:1"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "swim is a step" "$(case " $STEPS " in *" swim "*) echo yes ;; esac)" yes
	t "swim comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' swim')" 1
	t "swim needs a session" "$(case " $SESSION_STEPS " in *" swim "*) echo yes ;; esac)" yes
	t "swim runs by default" "$(case " $OPTIN_STEPS " in *" swim "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "swim.sh selftest: all ok" || echo "swim.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
