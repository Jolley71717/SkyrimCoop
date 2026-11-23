#!/bin/bash
# steps/ride.sh: the rig step `ride` (protocol 12, docs/DESIGN.md "Riding"), sourced by steps.sh. A friend who rides a
# horse is shown riding it: their stand-in is seated, by the engine, on the other game's copy of that horse, carried in
# its saddle as it goes, and gets off with them. Not run in a game yet.
#
#   step_ride     the step
#   bash tools/rig/steps/ride.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover rides, the watcher's stand-in of it is read), both players outside near each other:
#   1. A horse by the mover: its `ride base` (a plain horse of Skyrim.esm) placed with the console's
#      `player.placeatme <base> 1` (set-up: a horse made as the game ran, shared as the mover's own once it rides it).
#      No base, or a console that refuses, is a rig fault.
#   2. The mover walks to within RIDE_REACH of it with its virtual pad, then turns its view with the right stick until
#      its crosshair is on it (`ride find`: aimed=1; never on it is a rig fault), and presses A (the game's own
#      activation: mounting). Not on it after RIDE_WAIT is a rig fault.
#   3. It rides forward (the left stick, RIDE_GO ms). In the watcher: the stand-in of the mover is a rider on the copy
#      of the mover's horse (`ride`: its rider part names the same mount key as the mover's own part), the engine has
#      it on that mount and it counts as seated (seated=1 engineOn=1 onOther=0), and it is in its saddle (gap= at most
#      RIDE_GAP). Said for the record: how it was seated (seatedBy=: activate, console, quick, or none when it was on
#      already) and the rounds it took.
#   4. The mover presses A again (getting off). Not off after RIDE_WAIT is a rig fault. In the watcher, the stand-in is
#      off too within RIDE_WAIT (no rider part, or on=0 engineOn=0).
#   5. The horse goes (`<id>.disable`, then `markfordelete`, set-up).
# NPC riders, dragons and horses of the load order are not on the rig yet (the unit tests cover their rules).

RIDE_WAIT=${RIDE_WAIT:-10}      # s: for a ride to start or end in the mover's game, and to show in the watcher's
RIDE_REACH=${RIDE_REACH:-130}   # u: the mover walks to within this of the horse before it mounts
RIDE_TURNS=${RIDE_TURNS:-16}    # short turns of the view to put the crosshair on the horse (16 x 22 degrees: all round)
RIDE_GO=${RIDE_GO:-3000}        # ms: the ride forward
RIDE_GAP=${RIDE_GAP:-150}       # u: the stand-in from the copy of its horse while seated (the engine carries it)

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# rd_ours <ride reply>: our player's part (before the first " | ")
rd_ours() { echo "${1%% | *}"; }
# rd_rider <ride reply> <player id>: that player's rider part ("rider player=N ..."); nothing when not listed
rd_rider() { awk -v p="player=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "rider" && $2 == p { print; exit }' <<<"$1"; }
# rd_counts <ride reply>: the counts' part ("rides sent=...")
rd_counts() { awk 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "rides" { print; exit }' <<<"$1"; }
# rd_find <ride find reply> <field>: a field of the mount found; nothing when none
rd_find() { [[ $1 == "mount id="* ]] && field "$2" "$1"; }
# rd_base <ride base reply>: the base's form id; nothing when none
rd_base() { sed -n 's/^base=\([0-9A-F]\{8\}\)$/\1/p' <<<"$1"; }
# rd_seated_in <rider part>: seated by this game, on its mount as the engine has it, not on another
rd_seated_in() { [ "$(field seated "$1")" = 1 ] && [ "$(field engineOn "$1")" = 1 ] && [ "$(field onOther "$1")" = 0 ]; }
# rd_off_in <rider part>: off (none listed, or neither said nor on as the engine has it)
rd_off_in() { [ -z "$1" ] || { [ "$(field on "$1")" = 0 ] && [ "$(field engineOn "$1")" = 0 ]; }; }

# Waits' tests.
rd_on() { [ "$(field on "$(rd_ours "$(dc "$1" ride)")")" = "${2:-1}" ]; }
rd_seated() { rd_seated_in "$(rd_rider "$(dc "$1" ride)" "$2")"; }
rd_off() { rd_off_in "$(rd_rider "$(dc "$1" ride)" "$2")"; }
rd_aimed() { [ "$(rd_find "$(dc "$1" 'ride find 600')" aimed)" = 1 ]; }
rd_walk_over() { [[ $(dc "$1" walkto) == "not walking"* ]]; }

# rd_gone <mover> <horse id>: the horse placed for the step goes (set-up)
rd_gone() { dc "$1" "console $2.disable" >/dev/null; dc "$1" "console $2.markfordelete" >/dev/null; }

# ride_way <mover> <watcher>
ride_way() {
	local m=$1 w=$2 label="ride ($1 rides, $2 watches)" base r found horse hx hy i id ours mkey part seatedBy
	id=$(field player "$(dc "$m" 'session status')")
	# 1. a horse by the mover
	base=$(rd_base "$(dc "$m" 'ride base')")
	if [ -z "$base" ]; then
		rig_fault state "$label: a horse to ride (set-up)" "the $m's game found no plain horse base (ride base: $(dc "$m" 'ride base'))"
		return
	fi
	r=$(dc "$m" "console player.placeatme $base 1")
	if [ "$r" != ran ]; then
		rig_fault state "$label: a horse placed by the $m (set-up)" "the console refused player.placeatme $base 1: $r"
		return
	fi
	sleep 2
	found=$(dc "$m" 'ride find 600')
	horse=$(rd_find "$found" id)
	if [ -z "$horse" ]; then
		rig_fault state "$label: a horse placed by the $m (set-up)" "none found near its player after placeatme $base: $found"
		return
	fi
	say "ride: a horse $horse of base $base placed by the $m's player with the console (set-up)"

	# 2. mounted with the game's own activation
	hx=$(rd_find "$found" x); hy=$(rd_find "$found" y)
	r=$(dc "$m" "walkto $hx $hy 0.5 $RIDE_REACH 30")
	[[ $r == walking* ]] && wait_until 35 rd_walk_over "$m"
	still "$m"
	for ((i = 0; i < RIDE_TURNS; i++)); do
		rd_aimed "$m" && break
		dc "$m" 'pad stick r 0.5 0 150' >/dev/null; sleep 0.4
	done
	if ! rd_aimed "$m"; then
		rig_fault input "$label: the $m's crosshair on its horse (set-up)" "$(dc "$m" 'ride find 600')"
		rd_gone "$m" "$horse"; return
	fi
	dc "$m" 'pad button a' >/dev/null
	if ! wait_until "$RIDE_WAIT" rd_on "$m" 1; then
		rig_fault input "$label: the $m's player gets on its horse (A on it; set-up)" "$(rd_ours "$(dc "$m" ride)")"
		rd_gone "$m" "$horse"; return
	fi
	ours=$(rd_ours "$(dc "$m" ride)")
	mkey=$(field mount "$ours")
	say "ride: the $m's player rides $mkey ($ours)"

	# 3. the stand-in rides the copy of that horse
	dc "$m" "pad stick l 0 1 $RIDE_GO" >/dev/null
	if wait_until "$RIDE_WAIT" rd_seated "$w" "$id"; then
		part=$(rd_rider "$(dc "$w" ride)" "$id")
		pass "$label: the $w's stand-in of the $m is seated on a mount (seated=1 engineOn=1 onOther=0)" "$(cut -c1-220 <<<"$part")"
	else
		part=$(rd_rider "$(dc "$w" ride)" "$id")
		fail "$label: the $w's stand-in of the $m is seated on a mount (seated=1 engineOn=1 onOther=0)" "${part:-no rider part for player $id: the ride never came}; $(rd_counts "$(dc "$w" ride)")"
	fi
	if [ -n "$part" ] && [ "$(field mount "$part")" = "$mkey" ]; then
		pass "$label: the $w seats it on its copy of the $m's horse (mount=$mkey)" "mountHere=$(field mountHere "$part")"
	else
		fail "$label: the $w seats it on its copy of the $m's horse (mount=$mkey)" "the $w has mount=$(field mount "$part") mountHere=$(field mountHere "$part")"
	fi
	sleep "$((RIDE_GO / 1000))"
	part=$(rd_rider "$(dc "$w" ride)" "$id")
	if rd_seated_in "$part"; then
		expect_le "$label: riding, the $w's stand-in is in its saddle (gap=)" "$(field gap "$part")" "$RIDE_GAP" u
	else
		fail "$label: riding, the $w's stand-in is in its saddle (gap=)" "not seated after the ride forward: $(cut -c1-220 <<<"$part")"
	fi
	seatedBy=$(field seatedBy "$part")
	say "ride: the $w seated it by ${seatedBy:-?} after $(field rounds "$part") round(s); put back on its saddle $(field holds "$part") time(s)"
	still "$m"

	# 4. off again
	dc "$m" 'pad button a' >/dev/null
	if ! wait_until "$RIDE_WAIT" rd_on "$m" 0; then
		rig_fault input "$label: the $m's player gets off its horse (A again; set-up)" "$(rd_ours "$(dc "$m" ride)")"
		rd_gone "$m" "$horse"; return
	fi
	if wait_until "$RIDE_WAIT" rd_off "$w" "$id"; then
		pass "$label: the $w's stand-in gets off with its friend" "$(rd_counts "$(dc "$w" ride)")"
	else
		fail "$label: the $w's stand-in gets off with its friend" "$(rd_rider "$(dc "$w" ride)" "$id" | cut -c1-220)"
	fi

	# 5. the horse goes
	rd_gone "$m" "$horse"
}

step_ride() {
	local w
	for w in host vm; do still $w; done
	ride_way vm host
	ride_way host vm
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init ridetest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Riding::Command)
	R='ours on=0 mount=none mountActor=00000000 getting=0 said=0 sent=2 | rider player=2 on=1 mount=FF000801@2 mountHere=FF0009AA riderHere=FF0008AE engineOn=1 onOther=0 busy=0 seated=1 rounds=0 tried=activate seatedBy=activate givenUp=0 gap=12 holds=0 dismounts=0 sender=2 heard=1 | rider npc=0001A692 on=1 mount=00068D07 mountHere=00068D07 riderHere=0001A692 engineOn=0 onOther=0 busy=0 seated=0 rounds=1 tried=quick seatedBy=none givenUp=0 gap=-1 holds=0 dismounts=0 sender=2 heard=1 | rides sent=2 heard=3 notBelieved=0 lacked=0 activates=2 consoles=1 quicks=1 dismounts=0 holds=0 releases=0'
	t "ours: our part only" "$(field on "$(rd_ours "$R")")" 0
	t "ours: not a rider's fields" "$(field seated "$(rd_ours "$R")")" ""
	t "rider: that player's part" "$(field mount "$(rd_rider "$R" 2)")" FF000801@2
	t "rider: not an NPC's fields" "$(field rounds "$(rd_rider "$R" 2)")" 0
	t "rider: none not listed" "$(rd_rider "$R" 3)" ""
	t "rider: not player 1 in player 12" "$(rd_rider "${R/player=2/player=12}" 1)" ""
	t "counts" "$(field activates "$(rd_counts "$R")")" 2
	t "seated" "$(rd_seated_in "$(rd_rider "$R" 2)" && echo yes)" yes
	t "seated: not while the engine has it off" "$(rd_seated_in "$(rd_rider "${R/engineOn=1/engineOn=0}" 2)" || echo no)" no
	t "seated: not on another" "$(rd_seated_in "$(rd_rider "${R/engineOn=1 onOther=0/engineOn=1 onOther=1}" 2)" || echo no)" no
	t "off: none listed" "$(rd_off_in "" && echo yes)" yes
	t "off: said off, engine off" "$(rd_off_in 'rider player=2 on=0 engineOn=0' && echo yes)" yes
	t "off: not while still on" "$(rd_off_in 'rider player=2 on=0 engineOn=1' || echo no)" no
	t "find: the mount" "$(rd_find 'mount id=FF000AB0 base=00068D5C key=FF000AB0@2 dist=250 x=10 y=20 z=30 aimed=1' id)" FF000AB0
	t "find: aimed" "$(rd_find 'mount id=FF000AB0 base=00068D5C key=none dist=250 x=10 y=20 z=30 aimed=0' aimed)" 0
	t "find: none" "$(rd_find 'mount none' id)" ""
	t "base" "$(rd_base 'base=00068D5C')" 00068D5C
	t "base: none" "$(rd_base 'base none')" ""

	# The step's way, both games faked: the mover's console places a horse, its view turns onto it (aimed after a few
	# turns), A gets it on and off; the watcher's stand-in follows as the plugin would. FR_MODE: ok, nobase (no horse
	# base: the rig's), noaim (the crosshair never on it: the rig's), nomount (A does not get it on: the rig's), noseat
	# (the stand-in is never seated), wrongmount (it is seated on another mount), far (it is left far from its saddle),
	# stuckon (it stays on after its friend got off), unseated (seated, then off its mount while its friend rides).
	fake_ride() {
		FR=$d/fr.$RANDOM; mkdir -p "$FR"; FR_MODE=$1
		echo 0 > "$FR/placed"; echo 0 > "$FR/turns"; echo 0 > "$FR/on"; echo 0 > "$FR/rode"
		dc() { local on rode part gap seated eng mk; case "$1 $2" in
			"vm session status") echo "role=client connection=up welcomed=1 player=2 host=0 players=2" ;;
			"vm ride base") [ "$FR_MODE" = nobase ] && echo "base none" || echo "base=00068D5C" ;;
			"vm console player.placeatme 00068D5C 1") echo 1 > "$FR/placed"; echo ran ;;
			"vm console "*) echo ran ;;
			"vm ride find 600")
				if [ "$(cat "$FR/placed")" = 1 ]; then
					local aimed=0; [ "$(cat "$FR/turns")" -ge 3 ] && [ "$FR_MODE" != noaim ] && aimed=1
					echo "mount id=FF000AB0 base=00068D5C key=none dist=120 x=500 y=600 z=0 aimed=$aimed"
				else echo "mount none"; fi ;;
			"vm walkto "[0-9]*) echo "walking to (500,600)" ;;
			*" walkto") echo "not walking; last: arrived" ;;
			"vm pad stick r"*) echo $(( $(cat "$FR/turns") + 1 )) > "$FR/turns"; echo ok ;;
			"vm pad stick l"*) echo 1 > "$FR/rode"; echo ok ;;
			"vm pad button a")
				if [ "$(cat "$FR/on")" = 1 ]; then echo 0 > "$FR/on"; elif [ "$FR_MODE" != nomount ]; then echo 1 > "$FR/on"; fi
				echo ok ;;
			"vm ride")
				on=$(cat "$FR/on")
				echo "ours on=$on mount=$([ "$on" = 1 ] && echo FF000AB0@2 || echo none) mountActor=00000000 getting=0 said=$on sent=1 | rides sent=1 heard=0 notBelieved=0 lacked=0 activates=0 consoles=0 quicks=0 dismounts=0 holds=0 releases=0" ;;
			"host ride")
				on=$(cat "$FR/on"); rode=$(cat "$FR/rode")
				[ "$FR_MODE" = stuckon ] && [ "$rode" = 1 ] && on=1
				part=""
				if [ "$on" = 1 ]; then
					seated=1; eng=1; gap=14; mk=FF000AB0@2
					case "$FR_MODE" in noseat) seated=0; eng=0 ;; wrongmount) mk=FF000AB1@2 ;; far) gap=600 ;; unseated) [ "$rode" = 1 ] && { seated=0; eng=0; } ;; esac
					part=" | rider player=2 on=1 mount=$mk mountHere=FF0009AA riderHere=FF0008AE engineOn=$eng onOther=0 busy=0 seated=$seated rounds=0 tried=activate seatedBy=activate givenUp=0 gap=$gap holds=0 dismounts=0 sender=2 heard=1"
				fi
				echo "ours on=0 mount=none mountActor=00000000 getting=0 said=0 sent=0$part | rides sent=0 heard=2 notBelieved=0 lacked=0 activates=1 consoles=0 quicks=0 dismounts=0 holds=0 releases=0" ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fr$RANDOM; mkdir -p "$A_OUT"; a_init fr >/dev/null; ride_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: seated (1), on the copy of the mover's horse (1), in its saddle (1), off with its friend (1)
	t "way: all of it works" "$(fake_ride ok)" "PASS:4 "
	t "way: no horse base is the rig's, and nothing else is tried" "$(fake_ride nobase)" "RIG:1 "
	t "way: a crosshair never on the horse is the rig's" "$(fake_ride noaim)" "RIG:1 "
	t "way: a mover A does not mount is the rig's" "$(fake_ride nomount)" "RIG:1 "
	# noseat: not seated (1), and not in its saddle (1); its mount key is still the mover's, and it is off at the end
	t "way: a stand-in never seated fails" "$(fake_ride noseat)" "FAIL:2 PASS:2 "
	t "way: a stand-in seated on another mount fails" "$(fake_ride wrongmount)" "FAIL:1 PASS:3 "
	t "way: a stand-in far from its saddle fails" "$(fake_ride far)" "FAIL:1 PASS:3 "
	t "way: a stand-in still on after its friend got off fails" "$(fake_ride stuckon)" "FAIL:1 PASS:3 "
	# unseated: off as soon as its friend rides on: not seated (1), not in its saddle (1)
	t "way: a stand-in that falls off while its friend rides fails" "$(fake_ride unseated)" "FAIL:2 PASS:2 "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "ride is a step" "$(case " $STEPS " in *" ride "*) echo yes ;; esac)" yes
	t "ride comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' ride')" 1
	t "ride needs a session" "$(case " $SESSION_STEPS " in *" ride "*) echo yes ;; esac)" yes
	t "ride runs by default" "$(case " $OPTIN_STEPS " in *" ride "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "ride.sh selftest: all ok" || echo "ride.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
