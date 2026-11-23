#!/bin/bash
# steps/swim.sh: the rig step `swim` (protocol 11, docs/DESIGN.md "Players": "In water"), sourced by steps.sh. A friend
# who swims is shown swimming: their stand-in's actor state and graph go into the water's mode, it is held at the depth
# its friend swims at (the water's own pull does not float it up) and pitched as its friend dives, and it walks out of
# the water with them. Not run in a game yet.
#
#   step_swim     the step
#   bash tools/rig/steps/swim.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover swims, the watcher's stand-in of it is read), both players outside near each other:
#   1. Water deep enough to swim in near the mover (its `swim find`: the nearest point where the water is SWIM_DEPTH
#      deeper than the land; none is a rig fault, and SWIM_SPOT="x y" names one).
#   2. The watcher walks to within SWIM_WATCH of it and the mover walks into it, both with their virtual pads (walkto):
#      the game's own swimming. A mover that is not swimming there is a rig fault (the walk or the spot).
#   3. A stroke forward (the left stick): the watcher's stand-in swims (its friend's state says so, its actor state's
#      swimming bit is set and its graph was told: friendSwims=1 swimming=1 shown=w), its graph took the swim's start
#      (swimTaken=), it is at its friend's height (|depthErr| within SWIM_DEPTH_ERR: the water did not float it), and at
#      rest it stands within SWIM_REST of where its friend swims.
#   4. A dive (the right stick down, the left stick forward; a mover not under water then is a rig fault): the
#      stand-in's friend is under (friendUnder=1), the stand-in is at its friend's depth and went down with it (at least
#      SWIM_DIVE_DROP below where it swam at the surface), and it is pitched as its friend swims (its pitch within
#      SWIM_PITCH_TOL of its friend's, held to 75 degrees).
#   5. Up again (the right stick up, the left stick forward): its friend is not under (friendUnder=0), and the stand-in
#      is level (|pitch| within 5).
#   6. Both walk back to where they began: the stand-in is no longer shown swimming (friendSwims=0 swimming=0 shown=-),
#      its graph took the swim's start and end (swimTaken= 2 or more) and gave none up (swimGivenUp=0: an event name
#      its graph never takes, a FAIL that says the names the mover's own graph heard).
#   Said for the record: the events the mover's own graph took going in, coming out, diving and surfacing (heardIn=,
#   heardOut=, heardDive=, heardSurface=: the names the stand-ins are sent are checked against them), how often the water
#   floated a stand-in off its depth (refloats=), and each game's NPC copies in water.

SWIM_RADIUS=${SWIM_RADIUS:-4096}        # u: how far `swim find` looks from the mover
SWIM_SPOT=${SWIM_SPOT:-}                # "x y": water to swim in, when the scene has none within SWIM_RADIUS (outside)
SWIM_WAIT=${SWIM_WAIT:-10}              # s: for the game to say a player swims, and a state to reach the other game
SWIM_REACH=${SWIM_REACH:-300}           # u: a mover that stopped further than this from the spot did not get there (the walk's)
SWIM_WATCH=${SWIM_WATCH:-600}           # u: the watcher walks to within this of the spot (its stand-in of the mover in view)
SWIM_DEPTH_ERR=${SWIM_DEPTH_ERR:-40}    # u: the stand-in's height off its friend's while swimming
SWIM_DIVE_DROP=${SWIM_DIVE_DROP:-50}    # u: a dive takes the stand-in at least this far below where it swam at the surface
SWIM_PITCH_TOL=${SWIM_PITCH_TOL:-15}    # degrees: the stand-in's pitch off its friend's under water
SWIM_LEVEL=${SWIM_LEVEL:-5}             # degrees: the stand-in's pitch at the surface
SWIM_REST=${SWIM_REST:-100}             # u: the stand-in from its friend at rest in the water (it bobs)

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# sw_ours <swim reply>: our player's part (before the first " | ")
sw_ours() { echo "${1%% | *}"; }
# sw_friend <swim reply>: the friend's stand-in's part ("player N ...", up to the copies' part; one friend on the rig)
sw_friend() { [[ $1 == *"| player "* ]] || return 0; local r="player ${1#*| player }"; echo "${r%% | copies*}"; }
# sw_copies <swim reply>: the NPC copies' part
sw_copies() { [[ $1 == *"| copies "* ]] && echo "copies ${1#*| copies }"; }
sw_own_field() { field "$2" "$(sw_ours "$(dc "$1" swim)")"; }
sw_field() { field "$2" "$(sw_friend "$(dc "$1" swim)")"; }
# sw_spot <swim find reply>: "x y" of the spot found; nothing when none
sw_spot() { sed -n 's/^spot=(\([-0-9.]*\),\([-0-9.]*\)).*/\1 \2/p' <<<"$1"; }
# sw_abs <number>: its size, whole units; nothing when it is no number
sw_abs() { is_num "$1" && awk -v a="$1" 'BEGIN{printf "%.0f", (a < 0 ? -a : a)}'; }
# sw_pitch_off <stand-in pitch> <friend pitch>: degrees between them, the friend's held to the dive's 75
sw_pitch_off() { is_num "$1" && is_num "$2" && awk -v s="$1" -v f="$2" 'BEGIN{if (f > 75) f = 75; if (f < -75) f = -75; d = s - f; printf "%.0f", (d < 0 ? -d : d)}'; }
# sw_drop <z at the surface> <z under>: how far it went down, whole units
sw_drop() { is_num "$1" && is_num "$2" && awk -v a="$1" -v b="$2" 'BEGIN{printf "%.0f", a - b}'; }

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

swim_way() {
	local m=$1 w=$2 label="swim: the $1 swims, the $2 watches" p0 q0 f x y d o p z0 dz off fp dived0 surfaced0 heard
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
		say "swim: water $(field depth "$f") u deep at ($x,$y), $(field dist "$f") u from the $m's player ($(field looked "$f") points read)"
	fi

	# 2. the watcher near it, the mover into it
	d=$(sw_walk "$w" "$x" "$y" "$SWIM_WATCH") || { rig_fault input "$label: the $w walks to the water (set-up)" "$d"; return; }
	dived0=$(sw_own_field "$m" dived); surfaced0=$(sw_own_field "$m" surfaced)
	d=$(sw_walk "$m" "$x" "$y" 150) || { rig_fault input "$label: the $m walks into the water (set-up)" "$d"; sw_back "$m" "$p0" "$w" "$q0"; return; }
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
		pass "$label: the $w's stand-in swims as the $m's player does (friendSwims=1 swimming=1 shown=w)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-240)"
	else
		fail "$label: the $w's stand-in swims as the $m's player does (friendSwims=1 swimming=1 shown=w)" "$(sw_friend "$(dc "$w" swim)" | cut -c1-240)"
	fi
	sleep 3; still "$m"; sleep 1
	p=$(sw_friend "$(dc "$w" swim)")
	if is_num "$(field swimTaken "$p")" && [ "$(field swimTaken "$p")" -gt 0 ]; then
		pass "$label: the $w's stand-in's graph took the swim's start (swimTaken=)" "swimTaken=$(field swimTaken "$p") swimRefused=$(field swimRefused "$p") charState=$(field charState "$p")"
	else
		fail "$label: the $w's stand-in's graph took the swim's start (swimTaken=)" "swimTaken=$(field swimTaken "$p") swimRefused=$(field swimRefused "$p") swimGivenUp=$(field swimGivenUp "$p"); the $m's own graph took on going in: $(sw_own_field "$m" heardIn)"
	fi
	expect_le "$label: the $w's stand-in swims at its friend's height, not floated off it (|depthErr|)" "$(sw_abs "$(field depthErr "$p")")" "$SWIM_DEPTH_ERR" u
	expect_le "$label: at rest, the $w's stand-in is where its friend swims" "$(field dist "$(dc "$w" standins)")" "$SWIM_REST" u
	z0=$(field z "$p")

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
	p=$(sw_friend "$(dc "$w" swim)")
	expect_le "$label: under water, the $w's stand-in is at its friend's depth (|depthErr|)" "$(sw_abs "$(field depthErr "$p")")" "$SWIM_DEPTH_ERR" u
	dz=$(sw_drop "$z0" "$(field z "$p")")
	expect_ge "$label: the $w's stand-in went down with its friend (below where it swam at the surface)" "$dz" "$SWIM_DIVE_DROP" u
	fp=$(field friendPitch "$p")
	if is_num "$fp" && [ "${fp#-}" -ge 10 ]; then
		off=$(sw_pitch_off "$(field pitch "$p")" "$fp")
		expect_le "$label: the $w's stand-in is pitched as its friend swims under water (pitch=$(field pitch "$p") friendPitch=$fp)" "$off" "$SWIM_PITCH_TOL" deg
	else
		warn "$label: the $w's stand-in is pitched as its friend swims under water" "its friend swam at ${fp:-?} degrees: too level to judge (the right stick's push moved the view too little)"
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
	expect_le "$label: at the surface the $w's stand-in is level (|pitch|)" "$(sw_abs "$(sw_field "$w" pitch)")" "$SWIM_LEVEL" deg
	dc "$m" 'pad stick r 0 -1 500' >/dev/null  # the view back near level for what comes next

	# 6. out of the water
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
	say "swim: the water floated the $w's stand-in off its depth $(field refloats "$p") time(s) (refloats=; each put back), its character controller's state in the water $(field charState "$p")"
}

step_swim() {
	local w
	for w in host vm; do still $w; done
	swim_way vm host
	swim_way host vm
	section "swim: NPC copies in water, both games (for the record)"
	for w in host vm; do say "swim: $w: $(sw_copies "$(dc $w swim)" | cut -c1-400)"; done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init swimtest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Water::Command: ours | RemotePlayers::Swim | NpcSync::Swim)
	S='ours swimming=1 engineSwimming=1 inWater=1 under=0 engineUnder=0 submerged=0.71 charState=5 surface=1 shoulders=1 controllerWater=-20 z=-130 water=-20 depth=110 pitch=-3 entered=1 left=0 dived=0 surfaced=0 heardIn=SwimStart,moveStart heardOut=none heardDive=none heardSurface=none | player 2 standin=FF0008AE friendSwims=1 friendUnder=0 shown=w swimming=1 engineSwimming=1 inWater=1 charState=5 z=-128 friendZ=-130 depthErr=2 water=-20 pitch=-3 friendPitch=-3 swimTaken=1 swimRefused=0 swimGivenUp=0 refloats=4 | copies swimming=1 under=0 shownSwimming=1 swimTaken=1 swimRefused=0 swimGivenUp=0 refloats=0 | copy 0001A67C shown=w under=0 z=-140 runnerZ=-141 depthErr=1'
	t "ours: our part only" "$(field pitch "$(sw_ours "$S")")" -3
	t "ours: not the stand-in's fields" "$(field friendSwims "$(sw_ours "$S")")" ""
	t "ours: the heard names" "$(field heardIn "$(sw_ours "$S")")" "SwimStart,moveStart"
	t "ours: the controller's state" "$(field charState "$(sw_ours "$S")")" 5
	t "friend: the stand-in's part" "$(field depthErr "$(sw_friend "$S")")" 2
	t "friend: its swimming=, not engineSwimming=" "$(field swimming "$(sw_friend "$S")")" 1
	t "friend: not the copies' fields" "$(field shownSwimming "$(sw_friend "$S")")" ""
	t "friend: its swimTaken, not the copies'" "$(field refloats "$(sw_friend "$S")")" 4
	t "friend: none without a friend" "$(sw_friend 'ours swimming=0 | copies swimming=0')" ""
	t "copies: their part" "$(field shownSwimming "$(sw_copies "$S")")" 1
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

	# The step's way, both games faked: the mover walks into the water at the spot and swims there, dives when its view
	# is down and its left stick pushed, comes up when its view is up, walks out at where it began; the watcher's
	# stand-in follows as the plugin would. FW_MODE: ok, nospot (no water near: the rig's), dry (the spot is not water
	# the mover swims in: the rig's), noshow (the stand-in is never shown swimming), refused (its graph turns the swim
	# events away: given up), float (under water the stand-in stays at the surface), flat (under water it is level),
	# nodive (the dive does not take the mover under: the rig's), stuck (out of the water it is still shown swimming),
	# nounder (its friend's state never says under).
	fake_swim() {
		FW=$d/fw.$RANDOM; mkdir -p "$FW"; FW_MODE=$1
		echo "0 0" > "$FW/vm"; echo "300 0" > "$FW/host"; echo 0 > "$FW/swim"; echo 0 > "$FW/under"; echo 0 > "$FW/view"; echo 0 > "$FW/taken"
		fw_set() { local was; was=$(cat "$FW/swim"); [ "$was" != "$1" ] && echo $(( $(cat "$FW/taken") + 1 )) > "$FW/taken"; echo "$1" > "$FW/swim"; }
		dc() { local s u v z fz fu pt tk gu sh sw; case "$1 $2" in
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
			"vm swim")
				s=$(cat "$FW/swim"); u=$(cat "$FW/under"); v=$(cat "$FW/view")
				echo "ours swimming=$s engineSwimming=$s inWater=$s under=$u engineUnder=$u submerged=0.70 charState=$([ "$s" = 1 ] && echo 5 || echo 0) z=$([ "$u" = 1 ] && echo -300 || echo -130) water=-20 depth=110 pitch=$v entered=1 left=1 dived=1 surfaced=1 heardIn=SwimStart heardOut=SwimStop heardDive=none heardSurface=none | copies swimming=0 under=0 shownSwimming=0 swimTaken=0 swimRefused=0 swimGivenUp=0 refloats=0" ;;
			"host swim")
				s=$(cat "$FW/swim"); u=$(cat "$FW/under"); v=$(cat "$FW/view"); tk=$(cat "$FW/taken"); gu=0; sh=$s; sw=$s
				fz=$([ "$u" = 1 ] && echo -300 || echo -130); z=$fz
				[ "$FW_MODE" = float ] && z=-130
				pt=$([ "$u" = 1 ] && echo "$v" || echo 0)
				[ "$FW_MODE" = flat ] && pt=0
				fu=$u; [ "$FW_MODE" = nounder ] && { fu=0; pt=0; }  # its height still comes; no dive pitch without the flag
				[ "$FW_MODE" = noshow ] && { sh=0; sw=0; }
				[ "$FW_MODE" = refused ] && { tk=0; gu=$(( $(cat "$FW/taken") > 0 ? 1 : 0 )); }
				[ "$FW_MODE" = stuck ] && [ "$(cat "$FW/taken")" -ge 2 ] && { sh=1; sw=1; }
				echo "ours swimming=0 under=0 | player 1 standin=FF0008AE friendSwims=$s friendUnder=$fu shown=$([ "$sh" = 1 ] && echo w || echo -) swimming=$sw engineSwimming=$sw inWater=$s charState=5 z=$z friendZ=$fz depthErr=$((z - fz)) water=-20 pitch=$pt friendPitch=$v swimTaken=$tk swimRefused=0 swimGivenUp=$gu refloats=3 | copies swimming=0 under=0 shownSwimming=0 swimTaken=0 swimRefused=0 swimGivenUp=0 refloats=0" ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fw$RANDOM; mkdir -p "$A_OUT"; a_init fw >/dev/null; swim_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: swims (1), graph took (1), depth (1), at rest (1); under (1), its depth (1), went down (1), pitched (1); came up
	# (1), level (1); out (1), took both (1), none given up (1): 13
	t "way: all of it works" "$(fake_swim ok)" "PASS:13 "
	t "way: no water near is the rig's, and nothing else is tried" "$(fake_swim nospot)" "RIG:1 "
	t "way: a spot the mover does not swim in is the rig's" "$(fake_swim dry)" "RIG:1 "
	# noshow: not shown swimming (1); the rest of it holds (its state, depth, pitch), and out of the water it is not
	t "way: a stand-in never shown swimming fails" "$(fake_swim noshow | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	# refused: no start taken (1), nor both (1), and given up (1)
	t "way: a graph that turns the swim events away fails, the names heard said" "$(fake_swim refused | grep -o 'FAIL:[0-9]*')" "FAIL:3"
	t "way: a stand-in the water floats up fails: its depth and its dive" "$(fake_swim float | grep -o 'FAIL:[0-9]*')" "FAIL:2"
	t "way: a stand-in level under water fails" "$(fake_swim flat | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	t "way: a dive that does not go under is the rig's, after the checks at the surface" "$(fake_swim nodive)" "PASS:4 RIG:1 "
	t "way: a stand-in still shown swimming out of the water fails" "$(fake_swim stuck | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	# nounder: its friend never under (1), and the stand-in level under water (1); its height still comes with the state
	t "way: a state that never says under fails" "$(fake_swim nounder | grep -o 'FAIL:[0-9]*')" "FAIL:2"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "swim is a step" "$(case " $STEPS " in *" swim "*) echo yes ;; esac)" yes
	t "swim comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' swim')" 1
	t "swim needs a session" "$(case " $SESSION_STEPS " in *" swim "*) echo yes ;; esac)" yes
	t "swim runs by default" "$(case " $OPTIN_STEPS " in *" swim "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "swim.sh selftest: all ok" || echo "swim.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
