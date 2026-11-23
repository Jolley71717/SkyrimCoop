#!/bin/bash
# steps/ride.sh: the rig step `ride` (protocol 12, docs/DESIGN.md "Riding"), sourced by steps.sh. A friend who rides a
# horse is shown riding it: their stand-in is seated, by the engine, on the other game's copy of that horse, carried in
# its saddle as it goes, and gets off with them; an NPC that rides is shown the same way on a horse of the load order,
# and the horse goes with its rider's game. Not run in a game yet.
#
#   step_ride     the step
#   bash tools/rig/steps/ride.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# A player on a horse made as the game ran, each way (the mover rides, the watcher's stand-in of it is read), both
# players outside near each other:
#   1. A horse by the mover: its `ride base` (a plain horse of Skyrim.esm) placed with the console's
#      `player.placeatme <base> 1` (set-up: a horse made as the game ran, shared as the mover's own once it rides it).
#      `ride find` must give that horse back (the same base, a runtime id), or nothing is ridden or deleted: a rig fault.
#   2. The mover walks to within RIDE_REACH of it with its virtual pad, then turns its view with the right stick until
#      its crosshair is on it (`ride find`: aimed=1; never on it is a rig fault), and presses A (the game's own
#      activation: mounting). Not on it after RIDE_WAIT, or its ride never naming a shared mount (mount=none: the scan
#      has not shared the horse), is a rig fault.
#   3. At rest: in the watcher, the stand-in of the mover is a rider on the copy of the mover's horse (the same mount
#      key), the engine has it on that mount and it counts as seated (seated=1 engineOn=1 onOther=0).
#   4. It rides forward (the left stick, RIDE_GO ms), and the watcher's `ride` is read each second while it goes: the
#      copy of the horse must go RIDE_MOVED or more (else the carry is not tested: a rig fault), the stand-in stays
#      seated in every sample, its largest gap= from its mount is at most RIDE_GAP (under the plugin's own put-back at
#      150 u), and the plugin never put it back (holds= did not rise: the engine carried it). A quick put-on during the
#      ride (quicks= rose) is a WARN: the engine had it on the mount but not in the saddle.
#   5. Removal while seated (the class of the stand-in removal crash): when the VM watches, the VM saves (never the
#      host) with its stand-in of the host seated: it lives, the stand-in got off before it left the world
#      (releases= rose), no stray, and it is seated again after. Each way, the VM leaves while the mover still rides:
#      both games live, no stray, the host's seated stand-in of the VM (when the VM rides) got off first; the VM joins
#      again and the watcher's stand-in is seated again on the horse (the VM's new id and mount key when it rides).
#   6. The mover presses A again (getting off). Not off after RIDE_WAIT is a rig fault. In the watcher, the stand-in is
#      off too within RIDE_WAIT (no rider part, or on=0 engineOn=0).
#   7. The horse goes (`<id>.disable`, then `markfordelete`, set-up).
# An NPC on a horse of the load order, each way (the mover's game runs the NPC):
#   8. A person the mover runs near it that the watcher shows as a copy, and a horse reference of Skyrim.esm no one
#      rides (`ride order`) brought to the watcher's player with the watcher's console `moveto player` (set-up), so
#      the watcher's game runs it first. The mover's console `<horse>.activate <person>` puts the person on it (set-up;
#      not on after RIDE_WAIT is a rig fault).
#   9. In the watcher, the copy of that person is seated on its copy of the horse (the load-order key: Key{id, 0}); the
#      horse goes with its rider (the watcher lets go of it, the mover asks for it): both games' books say the mover runs
#      it within RIDE_NPC_WAIT.
#  10. `ride off <person>` in the mover's game (set-up: the console has no way to have an NPC get off): the watcher's
#      copy gets off within RIDE_WAIT. The horse stays where it was brought (the host never saves).
# Not on the rig yet: dragons, a handover of a riding NPC from one runner to the other (the unit tests cover its rules).

RIDE_WAIT=${RIDE_WAIT:-10}            # s: for a ride to start or end in the mover's game, and to show in the watcher's
RIDE_REACH=${RIDE_REACH:-130}         # u: the mover walks to within this of the horse before it mounts
RIDE_TURNS=${RIDE_TURNS:-16}          # short turns of the view to put the crosshair on the horse (16 x 22 degrees: all round)
RIDE_GO=${RIDE_GO:-4000}              # ms: the ride forward, read each second
RIDE_GAP=${RIDE_GAP:-100}             # u: the stand-in from the copy of its horse while it rides (the plugin puts it back at 150)
RIDE_MOVED=${RIDE_MOVED:-300}         # u: the copy of the horse goes at least this far in the ride, or the carry is not tested
RIDE_NPC_WAIT=${RIDE_NPC_WAIT:-20}    # s: for the NPC's horse to go to its rider's game

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# rd_ours <ride reply>: our player's part (before the first " | ")
rd_ours() { echo "${1%% | *}"; }
# rd_rider <ride reply> <player id>: that player's rider part ("rider player=N ..."); nothing when not listed
rd_rider() { awk -v p="player=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "rider" && $2 == p { print; exit }' <<<"$1"; }
# rd_rider_npc <ride reply> <NPC key>: that NPC's rider part ("rider npc=KEY ..."); nothing when not listed
rd_rider_npc() { awk -v p="npc=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "rider" && $2 == p { print; exit }' <<<"$1"; }
# rd_said_npc <ride reply> <NPC key>: the mount our game said that NPC of ours rides; nothing when none
rd_said_npc() { awk -v p="npc=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "said" && $2 == p { sub(/^mount=/, "", $3); print $3; exit }' <<<"$1"; }
# rd_counts <ride reply>: the counts' part ("rides sent=...")
rd_counts() { awk 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "rides" { print; exit }' <<<"$1"; }
# rd_find <ride find reply> <field>: a field of the mount found; nothing when none
rd_find() { [[ $1 == "mount id="* ]] && field "$2" "$1"; }
# rd_base <ride base reply>: the base's form id; nothing when none
rd_base() { sed -n 's/^base=\([0-9A-F]\{8\}\)$/\1/p' <<<"$1"; }
# rd_order <ride order reply>: the horse reference's id; nothing when none
rd_order() { [[ $1 == "horse id="* ]] && field id "$1"; }
# rd_runtime <form id>: made as the game ran (FF......)
rd_runtime() { [[ $1 =~ ^FF[0-9A-F]{6}$ ]]; }
# rd_seated_in <rider part>: seated by this game, on its mount as the engine has it, not on another
rd_seated_in() { [ "$(field seated "$1")" = 1 ] && [ "$(field engineOn "$1")" = 1 ] && [ "$(field onOther "$1")" = 0 ]; }
# rd_off_in <rider part>: off (none listed, or neither said nor on as the engine has it)
rd_off_in() { [ -z "$1" ] || { [ "$(field on "$1")" = 0 ] && [ "$(field engineOn "$1")" = 0 ]; }; }
# rd_moved <rider part before> <rider part after>: how far its mount went here (mx= my=), whole units; empty: unknown
rd_moved() {
	local a b
	a="$(field mx "$1") $(field my "$1")"; b="$(field mx "$2") $(field my "$2")"
	is_num "$(field mx "$1")" && is_num "$(field mx "$2")" || return 0
	awk -v a="$a" -v b="$b" 'BEGIN { split(a, p, " "); split(b, q, " "); printf "%.0f\n", sqrt((p[1] - q[1]) ^ 2 + (p[2] - q[2]) ^ 2) }'
}
# rd_npc_pick <mover rows> <watcher rows> <mover id> <x y z>: "<key> <local id>" of a person (a human, alive, no child,
# of the load order, in the world) the mover runs and the watcher shows as a copy, the nearest to that place; empty: none
rd_npc_pick() {
	awk -v me="$3" -v p="$4" 'BEGIN { split(p, q, " ") } NF == 0 { next }
		FILENAME == ARGV[1] { if ($3 == "copy") shown[$1] = 1; next }
		$2 == me && $3 == "run" && $4 == "human" && $1 !~ /:/ && $15 == "0" && $16 != "1" && $5 != "-" && ($1 in shown) {
			d = sqrt(($5 - q[1]) ^ 2 + ($6 - q[2]) ^ 2)
			if (best == "" || d < bestd) { best = $1 " " $14; bestd = d }
		}
		END { if (best != "") print best }' <(printf '%s\n' "$2") <(printf '%s\n' "$1")
}

# Waits' tests.
rd_on() { [ "$(field on "$(rd_ours "$(dc "$1" ride)")")" = "${2:-1}" ]; }
# rd_mkey <who>: prints the mount key its player's ride names, once it names one (not none)
rd_mkey() { local k; k=$(field mount "$(rd_ours "$(dc "$1" ride)")"); [ -n "$k" ] && [ "$k" != none ] && echo "$k"; }
rd_seated() { rd_seated_in "$(rd_rider "$(dc "$1" ride)" "$2")"; }
rd_off() { rd_off_in "$(rd_rider "$(dc "$1" ride)" "$2")"; }
rd_npc_said() { [ "$(rd_said_npc "$(dc "$1" ride)" "$2")" = "$3" ]; }
rd_npc_seated() { rd_seated_in "$(rd_rider_npc "$(dc "$1" ride)" "$2")"; }
rd_npc_off() { rd_off_in "$(rd_rider_npc "$(dc "$1" ride)" "$2")"; }
rd_aimed() { [ "$(rd_find "$(dc "$1" 'ride find 600')" aimed)" = 1 ]; }
rd_walk_over() { [[ $(dc "$1" walkto) == "not walking"* ]]; }
# rd_owner <who> <NPC key>: who runs it, as that game's book says
rd_owner() { npc_col "$(npc_rows "$(dc "$1" 'npcs list')")" "$2" 2; }
rd_run_by() { [ "$(rd_owner "$1" "$2")" = "$3" ]; }
rd_run_by_both() { rd_run_by host "$1" "$2" && rd_run_by vm "$1" "$2"; }

# rd_gone <mover> <horse id>: the horse placed for the step goes (set-up)
rd_gone() { dc "$1" "console $2.disable" >/dev/null; dc "$1" "console $2.markfordelete" >/dev/null; }

# The mover's id after a rejoin (ride_rejoin sets it: the VM's id is new).
RD_ID=""

# ride_save <host's id>: the VM saves while its stand-in of the host's player is seated (never the host)
ride_save() {
	local id=$1 r rel0 saves
	section "ride: the VM saves while its stand-in of the host is seated (the host never saves)"
	rel0=$(field releases "$(rd_counts "$(dc vm ride)")")
	saves=$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { fail "ride save: the VM saves" "the console refused: $r"; return 1; }
	sleep 8
	if [ "$(dc vm ping)" != pong ]; then
		fail "ride save: the VM's game lives through its save with a seated stand-in" "its dev channel went silent after the save (a crash? see the crash scan)"
		_rig_stop "the VM's game went down" "after its save with a seated stand-in"
		return 1
	fi
	pass "ride save: the VM's game lives through its save with a seated stand-in" "its dev channel answers"
	expect_ge "ride save: the VM's log says it saved" "$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)" $((saves + 1))
	if is_num "$rel0"; then
		expect_ge "ride save: the stand-in got off its horse before the save took it out (releases=)" "$(field releases "$(rd_counts "$(dc vm ride)")")" $((rel0 + 1))
	else
		fail "ride save: the stand-in got off its horse before the save took it out (releases=)" "no releases= count before the save: $(rd_counts "$(dc vm ride)")"
	fi
	expect_zero "ride save: no stray stand-in after the save (strays=)" "$(field strays "$(dc vm strays)")"
	if wait_until 30 placed vm && wait_until "$RIDE_WAIT" rd_seated vm "$id"; then
		pass "ride save: the VM's stand-in of the host is seated again after the save" "$(rd_rider "$(dc vm ride)" "$id" | cut -c1-200)"
	else
		fail "ride save: the VM's stand-in of the host is seated again after the save" "$(rd_rider "$(dc vm ride)" "$id" | cut -c1-200)"
	fi
}

# ride_rejoin <mover> <watcher>: the VM leaves while the mover rides, then joins again; RD_ID is the mover's id after
ride_rejoin() {
	local m=$1 w=$2 r rel0 mkey
	section "ride: the VM leaves and joins again while the $m rides"
	[ "$w" = host ] && rel0=$(field releases "$(rd_counts "$(dc host ride)")")
	r=$(dc vm 'session leave'); [[ $r == left* ]] || fail "ride leave: the VM leaves" "$r"
	gone_from host || fail "ride leave: the host's stand-in of the VM goes when it leaves" "$(dc host standins | cut -c1-200)"
	sleep 3
	if [ "$(dc host ping)" != pong ] || [ "$(dc vm ping)" != pong ]; then
		fail "ride leave: both games live through a leave with a seated stand-in" "$(alive)"
		_rig_stop "a game went down" "after the VM left with a seated stand-in"
		return 1
	fi
	pass "ride leave: both games live through a leave with a seated stand-in" "$(alive)"
	expect_zero "ride leave: no stray stand-in in the host's game (strays=)" "$(field strays "$(dc host strays)")"
	expect_zero "ride leave: no stray stand-in in the VM's game (strays=)" "$(field strays "$(dc vm strays)")"
	if [ "$w" = host ]; then
		if is_num "$rel0"; then
			expect_ge "ride leave: the host's seated stand-in of the VM got off before it left the world (releases=)" "$(field releases "$(rd_counts "$(dc host ride)")")" $((rel0 + 1))
		else
			fail "ride leave: the host's seated stand-in of the VM got off before it left the world (releases=)" "no releases= count before the leave"
		fi
	fi
	if r=$(vm_join) && wait_until 90 joined_both; then pass "ride rejoin: the VM joins again while the $m rides" "$(dc vm 'session status' | cut -c1-80)"; else fail "ride rejoin: the VM joins again while the $m rides" "$r"; return 1; fi
	RD_ID=$(field player "$(dc "$m" 'session status')")
	if [ "$m" = vm ] && ! mkey=$(wait_until "$RIDE_WAIT" rd_mkey vm); then
		fail "ride rejoin: the VM says its ride again after it joins (its horse shared under its new id)" "$(rd_ours "$(dc vm ride)")"
	fi
	if wait_until 30 placed "$w" && wait_until "$RIDE_WAIT" rd_seated "$w" "$RD_ID"; then
		pass "ride rejoin: the $w's stand-in of the $m is seated again after the rejoin" "$(rd_rider "$(dc "$w" ride)" "$RD_ID" | cut -c1-200)"
	else
		fail "ride rejoin: the $w's stand-in of the $m is seated again after the rejoin" "$(rd_rider "$(dc "$w" ride)" "$RD_ID" | cut -c1-200)"
	fi
}

# ride_way <mover> <watcher>
ride_way() {
	local m=$1 w=$2 label="ride ($1 rides, $2 watches)" base r found horse hbase hx hy i id mkey part part0 seatedBy n seatedN gapMax g moved q0 h0 q1 h1
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
	horse=$(rd_find "$found" id); hbase=$(rd_find "$found" base)
	if [ -z "$horse" ] || [ "$hbase" != "$base" ] || ! rd_runtime "$horse"; then
		# not the one placed (another horse nearby, one of the load order): neither ridden nor deleted
		rig_fault state "$label: the horse placed by the $m is the one found (set-up)" "placed base $base; ride find gave: $found"
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
	# its ride names the horse once the scan (every 250 ms) has shared it
	if ! mkey=$(wait_until "$RIDE_WAIT" rd_mkey "$m"); then
		rig_fault state "$label: the $m's horse is shared once it rides it (its ride names a mount key)" "$(rd_ours "$(dc "$m" ride)")"
		dc "$m" 'pad button a' >/dev/null; sleep 3; rd_gone "$m" "$horse"; return
	fi
	say "ride: the $m's player rides $mkey ($(rd_ours "$(dc "$m" ride)"))"

	# 3. at rest: the stand-in seated on the copy of that horse
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

	# 4. the ride: carried by the engine, read each second while the horse goes
	if rd_seated_in "$part"; then
		part0=$part; q0=$(field quicks "$part0"); h0=$(field holds "$part0")
		dc "$m" "pad stick l 0 1 $RIDE_GO" >/dev/null
		n=$((RIDE_GO / 1000)); seatedN=0; gapMax=""
		for ((i = 0; i < n; i++)); do
			sleep 1
			part=$(rd_rider "$(dc "$w" ride)" "$id")
			rd_seated_in "$part" || continue
			seatedN=$((seatedN + 1)); g=$(field gap "$part")
			is_num "$g" && { [ -z "$gapMax" ] || _cmp "$g" '>' "$gapMax"; } && gapMax=$g
		done
		still "$m"
		moved=$(rd_moved "$part0" "$part")
		q1=$(field quicks "$part"); h1=$(field holds "$part")
		if ! is_num "$moved" || [ "$moved" -lt "$RIDE_MOVED" ]; then
			rig_fault input "$label: the $m's horse goes forward under its rider (for the carry check)" "its copy in the $w's game went ${moved:-?} u in $n s (want $RIDE_MOVED u): $(cut -c1-200 <<<"$part")"
		else
			say "ride: the $w's copy of the horse went $moved u while the $m rode"
			if [ "$seatedN" = "$n" ]; then
				pass "$label: riding, the $w's stand-in stays seated on its mount (each of $n samples)" "$(cut -c1-200 <<<"$part")"
			else
				fail "$label: riding, the $w's stand-in stays seated on its mount (each of $n samples)" "seated in $seatedN of $n: $(cut -c1-200 <<<"$part")"
			fi
			if [ "$seatedN" -gt 0 ]; then
				expect_le "$label: riding, the $w's stand-in is in its saddle (largest gap= of $seatedN samples)" "$gapMax" "$RIDE_GAP" u
			else
				fail "$label: riding, the $w's stand-in is in its saddle (largest gap= of the samples)" "no sample seated"
			fi
			if is_num "$h0" && is_num "$h1" && [ "$h1" -le "$h0" ]; then
				pass "$label: riding, the engine carries the $w's stand-in (never put back by the plugin: holds=)" "holds=$h0 -> $h1"
			else
				fail "$label: riding, the engine carries the $w's stand-in (never put back by the plugin: holds=)" "holds=$h0 -> $h1: the plugin placed it on its saddle, the engine did not carry it"
			fi
			if is_num "$q0" && is_num "$q1" && [ "$q1" -gt "$q0" ]; then
				warn "$label: riding, the $w's stand-in needs no quick put-on (quicks=)" "quicks=$q0 -> $q1: the engine had it on the mount but not in the saddle; see the $w's log"
			fi
		fi
	fi
	seatedBy=$(field seatedBy "$part")
	say "ride: the $w seated it by ${seatedBy:-?} after $(field rounds "$part") round(s); finished onto its saddle $(field quicks "$part") time(s), put back $(field holds "$part") time(s)"

	# 5. removal while seated: a save in the VM (when it watches), and the VM's leave and rejoin
	if rd_seated "$w" "$id"; then
		[ "$w" = vm ] && { ride_save "$id" || return; }
		RD_ID=$id
		ride_rejoin "$m" "$w" || return
		id=$RD_ID
	else
		fail "$label: a seated stand-in for the save and leave checks" "the $w's stand-in is not seated: those checks are not made"
	fi

	# 6. off again
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

	# 7. the horse goes
	rd_gone "$m" "$horse"
}

# ride_npc_way <mover> <watcher>: a person the mover runs rides a horse of the load order
ride_npc_way() {
	local m=$1 w=$2 label="ride npc ($1's person rides, $2 watches)" mid wid pick key local found horse r part was
	mid=$(field player "$(dc "$m" 'session status')"); wid=$(field player "$(dc "$w" 'session status')")
	# 8. a person of the mover's, and a horse of the load order brought to the watcher (its game runs it first)
	pick=$(rd_npc_pick "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")")
	if [ -z "$pick" ]; then
		rig_fault state "$label: a person the $m runs that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"
		return
	fi
	read -r key local <<<"$pick"
	found=$(dc "$w" 'ride order'); horse=$(rd_order "$found")
	if [ -z "$horse" ] || rd_runtime "$horse"; then
		rig_fault state "$label: a horse of the load order no one rides (set-up)" "ride order: $found"
		return
	fi
	r=$(dc "$w" "console $horse.moveto player")
	if [ "$r" != ran ]; then
		rig_fault state "$label: the horse $horse brought to the $w's player (set-up)" "the console refused $horse.moveto player: $r"
		return
	fi
	if wait_until "$RIDE_NPC_WAIT" rd_run_by "$w" "$horse" "$wid"; then
		say "ride npc: the horse $horse ($found) brought to the $w's player with the console (set-up); the $w runs it"
	else
		say "ride npc: the horse $horse brought to the $w's player (set-up); run by $(rd_owner "$w" "$horse"), not the $w: its letting go is not what hands it over"
	fi
	was=$(rd_owner "$w" "$horse")
	sleep 2
	r=$(dc "$m" "console $horse.activate $local")
	if [ "$r" != ran ] || ! wait_until "$RIDE_WAIT" rd_npc_said "$m" "$key" "$horse"; then
		rig_fault input "$label: the $m's person $key gets on the horse $horse (the console's activate; set-up)" "console: $r; $(dc "$m" ride | cut -c1-220)"
		dc "$m" "ride off $local" >/dev/null
		return
	fi
	say "ride npc: the $m's person $key rides the horse $horse (set-up)"

	# 9. the watcher's copy seated on its copy of the horse, and the horse goes with its rider
	if wait_until "$RIDE_WAIT" rd_npc_seated "$w" "$key"; then
		pass "$label: the $w's copy of the person is seated on a mount (seated=1 engineOn=1 onOther=0)" "$(rd_rider_npc "$(dc "$w" ride)" "$key" | cut -c1-220)"
	else
		fail "$label: the $w's copy of the person is seated on a mount (seated=1 engineOn=1 onOther=0)" "$(rd_rider_npc "$(dc "$w" ride)" "$key" | cut -c1-220); $(rd_counts "$(dc "$w" ride)")"
	fi
	part=$(rd_rider_npc "$(dc "$w" ride)" "$key")
	if [ "$(field mount "$part")" = "$horse" ]; then
		pass "$label: the $w seats it on its copy of the load-order horse (mount=$horse)" "mountHere=$(field mountHere "$part")"
	else
		fail "$label: the $w seats it on its copy of the load-order horse (mount=$horse)" "mount=$(field mount "$part") mountHere=$(field mountHere "$part")"
	fi
	if wait_until "$RIDE_NPC_WAIT" rd_run_by_both "$horse" "$mid"; then
		pass "$label: the horse goes with its rider: the $m runs it in both books (it was ${was:-nobody}'s)" "owner=$mid"
	else
		fail "$label: the horse goes with its rider: the $m runs it in both books (it was ${was:-nobody}'s)" "host's book: $(rd_owner host "$horse"), the VM's: $(rd_owner vm "$horse")"
	fi

	# 10. off
	r=$(dc "$m" "ride off $local")
	if [ "$r" != "off: $local" ]; then
		rig_fault state "$label: the $m's person gets off (ride off; set-up)" "$r"
		return
	fi
	if wait_until "$RIDE_WAIT" rd_npc_off "$w" "$key"; then
		pass "$label: the $w's copy gets off with it" "$(rd_counts "$(dc "$w" ride)")"
	else
		fail "$label: the $w's copy gets off with it" "$(rd_rider_npc "$(dc "$w" ride)" "$key" | cut -c1-220)"
	fi
	say "ride npc: the horse $horse stays where it was brought (the host never saves; the VM's test saves are moved aside)"
}

step_ride() {
	local w
	for w in host vm; do still $w; done
	ride_way vm host
	ride_way host vm
	ride_npc_way vm host
	ride_npc_way host vm
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init ridetest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Riding::Command)
	R='ours on=0 mount=none mountActor=00000000 getting=0 said=1 sent=2 | said npc=0001A693 mount=00068D08 | rider player=2 on=1 mount=FF000801@2 mountHere=FF0009AA riderHere=FF0008AE engineOn=1 onOther=0 busy=0 seated=1 rounds=0 tried=activate seatedBy=activate givenUp=0 gap=12 quicks=0 holds=0 dismounts=0 sender=2 handover=0 heard=1 mx=100 my=200 | rider npc=0001A692 on=1 mount=00068D07 mountHere=00068D07 riderHere=0001A692 engineOn=0 onOther=0 busy=0 seated=0 rounds=1 tried=console seatedBy=none givenUp=0 gap=-1 quicks=0 holds=0 dismounts=0 sender=2 handover=0 heard=1 mx=500 my=600 | rides sent=2 heard=3 notBelieved=0 lacked=0 activates=2 consoles=1 quicks=1 dismounts=0 holds=0 releases=0 handedToUs=0'
	t "ours: our part only" "$(field on "$(rd_ours "$R")")" 0
	t "ours: not a rider's fields" "$(field seated "$(rd_ours "$R")")" ""
	t "rider: that player's part" "$(field mount "$(rd_rider "$R" 2)")" FF000801@2
	t "rider: not an NPC's fields" "$(field rounds "$(rd_rider "$R" 2)")" 0
	t "rider: none not listed" "$(rd_rider "$R" 3)" ""
	t "rider: not player 1 in player 12" "$(rd_rider "${R/player=2/player=12}" 1)" ""
	t "rider npc: that NPC's part" "$(field mount "$(rd_rider_npc "$R" 0001A692)")" 00068D07
	t "rider npc: none not listed" "$(rd_rider_npc "$R" 0001A693)" ""
	t "said npc: the mount said" "$(rd_said_npc "$R" 0001A693)" 00068D08
	t "said npc: none" "$(rd_said_npc "$R" 0001A692)" ""
	t "counts" "$(field activates "$(rd_counts "$R")")" 2
	t "seated" "$(rd_seated_in "$(rd_rider "$R" 2)" && echo yes)" yes
	t "seated: not while the engine has it off" "$(rd_seated_in "$(rd_rider "${R/engineOn=1/engineOn=0}" 2)" || echo no)" no
	t "seated: not on another" "$(rd_seated_in "$(rd_rider "${R/engineOn=1 onOther=0/engineOn=1 onOther=1}" 2)" || echo no)" no
	t "off: none listed" "$(rd_off_in "" && echo yes)" yes
	t "off: said off, engine off" "$(rd_off_in 'rider player=2 on=0 engineOn=0' && echo yes)" yes
	t "off: not while still on" "$(rd_off_in 'rider player=2 on=0 engineOn=1' || echo no)" no
	t "moved" "$(rd_moved 'rider mx=100 my=200' 'rider mx=400 my=600')" 500
	t "moved: unknown" "$(rd_moved 'rider mx=100' 'rider gap=3')" ""
	t "find: the mount" "$(rd_find 'mount id=FF000AB0 base=00068D5C key=FF000AB0@2 dist=250 x=10 y=20 z=30 aimed=1' id)" FF000AB0
	t "find: aimed" "$(rd_find 'mount id=FF000AB0 base=00068D5C key=none dist=250 x=10 y=20 z=30 aimed=0' aimed)" 0
	t "find: none" "$(rd_find 'mount none' id)" ""
	t "base" "$(rd_base 'base=00068D5C')" 00068D5C
	t "base: none" "$(rd_base 'base none')" ""
	t "order" "$(rd_order 'horse id=00097E1E base=00068D5C loaded=0 dist=9000 seen=4')" 00097E1E
	t "order: none" "$(rd_order 'horse none')" ""
	t "runtime" "$(rd_runtime FF000AB0 && echo yes)" yes
	t "runtime: not a load-order id" "$(rd_runtime 00068D5C || echo no)" no
	ROWS_M=$(printf '%s\n' '0001A001 2 run human 100 0 0 - - - - - - 0001A001 0 0' '0001A002 2 run human 50 0 0 - - - - - - 0001A002 0 1' '0001A003 2 run creature 10 0 0 - - - - - - 0001A003 0 0' 'FF000900:2 2 run human 10 0 0 - - - - - - FF000900 0 0' '0001A004 1 copy human 10 0 0 - - - - - - 0001A004 0 0' '0001A005 2 run human 300 0 0 - - - - - - 0001A005 0 0')
	ROWS_W=$(printf '%s\n' '0001A001 2 copy human 100 0 0' '0001A002 2 copy human 50 0 0' '0001A003 2 copy creature 10 0 0' 'FF000900:2 2 copy human 10 0 0' '0001A005 2 copy human 300 0 0')
	t "npc pick: the nearest person the mover runs and the watcher shows; no child, creature or spawn" "$(rd_npc_pick "$ROWS_M" "$ROWS_W" 2 "0 0 0")" "0001A001 0001A001"
	t "npc pick: none the watcher shows" "$(rd_npc_pick "$ROWS_M" "" 2 "0 0 0")" ""

	# The player way, both games faked: the mover's console places a horse, its view turns onto it (aimed after a few
	# turns), A gets it on and off; the watcher's stand-in follows as the plugin would, its copy of the horse going on
	# each read while the mover rides. The save and the rejoin are faked apart (below). FR_MODE: ok, nobase (no horse
	# base: the rig's), otherhorse (ride find gives a horse of another base: the rig's), noaim (the rig's), nomount (A
	# does not get it on: the rig's), unshared (its ride names no mount: the rig's), stood (the horse does not go: the
	# rig's), noseat (the stand-in is never seated), wrongmount (seated on another mount), far (out of its saddle while it
	# rides), held (the plugin put it back while it rode), quick (a quick put-on while it rode: a WARN), stuckon (it stays
	# on after its friend got off), unseated (seated at rest, off its mount while its friend rides).
	fake_ride() {
		FR=$d/fr.$RANDOM; mkdir -p "$FR"; FR_MODE=$1
		echo 0 > "$FR/placed"; echo 0 > "$FR/turns"; echo 0 > "$FR/on"; echo 0 > "$FR/rode"; echo 0 > "$FR/reads"; echo 0 > "$FR/keeps"
		dc() { local on rode part gap seated eng mk reads mx q h; case "$1 $2" in
			"vm session status") echo "role=client connection=up welcomed=1 player=2 host=0 players=2" ;;
			"vm ride base") [ "$FR_MODE" = nobase ] && echo "base none" || echo "base=00068D5C" ;;
			"vm console player.placeatme 00068D5C 1") echo 1 > "$FR/placed"; echo ran ;;
			"vm console "*) echo ran ;;
			"vm ride find 600")
				if [ "$(cat "$FR/placed")" = 1 ]; then
					local aimed=0 b=00068D5C; [ "$(cat "$FR/turns")" -ge 3 ] && [ "$FR_MODE" != noaim ] && aimed=1; [ "$FR_MODE" = otherhorse ] && b=00068D5D
					echo "mount id=FF000AB0 base=$b key=none dist=120 x=500 y=600 z=0 aimed=$aimed"
				else echo "mount none"; fi ;;
			"vm walkto "[0-9]*) echo "walking to (500,600)" ;;
			*" walkto") echo "not walking; last: arrived" ;;
			"vm pad stick r"*) echo $(( $(cat "$FR/turns") + 1 )) > "$FR/turns"; echo ok ;;
			"vm pad stick l"*) echo 1 > "$FR/rode"; echo ok ;;
			"vm pad button a")
				if [ "$(cat "$FR/on")" = 1 ]; then echo 0 > "$FR/on"; elif [ "$FR_MODE" != nomount ]; then echo 1 > "$FR/on"; fi
				echo ok ;;
			"vm ride")
				on=$(cat "$FR/on"); mk=none; [ "$on" = 1 ] && [ "$FR_MODE" != unshared ] && mk=FF000AB0@2
				echo "ours on=$on mount=$mk mountActor=00000000 getting=0 said=$on sent=1 | rides sent=1 heard=0 notBelieved=0 lacked=0 activates=0 consoles=0 quicks=0 dismounts=0 holds=0 releases=0" ;;
			"host ride")
				on=$(cat "$FR/on"); rode=$(cat "$FR/rode"); reads=$(cat "$FR/reads")
				[ "$rode" = 1 ] && { reads=$((reads + 1)); echo $reads > "$FR/reads"; }
				[ "$FR_MODE" = stuckon ] && [ "$rode" = 1 ] && on=1
				part=""
				if [ "$on" = 1 ]; then
					seated=1; eng=1; gap=14; mk=FF000AB0@2; mx=$((500 + 150 * reads)); q=0; h=0
					case "$FR_MODE" in
						noseat) seated=0; eng=0 ;; wrongmount) mk=FF000AB1@2 ;; far) [ "$rode" = 1 ] && gap=130 ;; stood) mx=500 ;;
						held) h=$reads ;; quick) [ "$rode" = 1 ] && q=1 ;; unseated) [ "$rode" = 1 ] && { seated=0; eng=0; } ;;
					esac
					part=" | rider player=2 on=1 mount=$mk mountHere=FF0009AA riderHere=FF0008AE engineOn=$eng onOther=0 busy=0 seated=$seated rounds=0 tried=activate seatedBy=activate givenUp=0 gap=$gap quicks=$q holds=$h dismounts=0 sender=2 handover=0 heard=1 mx=$mx my=600"
				fi
				echo "ours on=0 mount=none mountActor=00000000 getting=0 said=0 sent=0$part | rides sent=0 heard=2 notBelieved=0 lacked=0 activates=1 consoles=0 quicks=0 dismounts=0 holds=0 releases=0" ;;
			*) echo ok ;; esac; }
		( ride_save() { echo 1 > "$FR/keeps"; }; ride_rejoin() { echo 1 > "$FR/keeps"; RD_ID=2; }
			A_OUT=$d/fr$RANDOM; mkdir -p "$A_OUT"; a_init fr >/dev/null; ride_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}'; [ "$(cat "$FR/keeps")" = 1 ] && echo -n "kept" )
	}
	# ok: seated (1), on the copy of the mover's horse (1), stays seated (1), in its saddle (1), carried (1), off (1); the
	# save and rejoin run (kept)
	t "way: all of it works" "$(fake_ride ok)" "PASS:6 kept"
	t "way: no horse base is the rig's, and nothing else is tried" "$(fake_ride nobase)" "RIG:1 "
	t "way: a horse found of another base is the rig's, and is not ridden" "$(fake_ride otherhorse)" "RIG:1 "
	t "way: a crosshair never on the horse is the rig's" "$(fake_ride noaim)" "RIG:1 "
	t "way: a mover A does not mount is the rig's" "$(fake_ride nomount)" "RIG:1 "
	t "way: a ride that never names its horse is the rig's" "$(fake_ride unshared)" "RIG:1 "
	# stood: seated (1), mount (1), carried not judged (the rig's), the save and rejoin, off (1)
	t "way: a horse that does not go is the rig's (the carry is not judged)" "$(fake_ride stood)" "PASS:3 RIG:1 kept"
	# noseat: not seated (1), no ride judged, no save or rejoin (1); its mount key is the mover's, and it is off at the end
	t "way: a stand-in never seated fails" "$(fake_ride noseat)" "FAIL:2 PASS:2 "
	t "way: a stand-in seated on another mount fails" "$(fake_ride wrongmount)" "FAIL:1 PASS:5 kept"
	t "way: a stand-in out of its saddle while it rides fails" "$(fake_ride far)" "FAIL:1 PASS:5 kept"
	t "way: a stand-in the plugin put back while it rode fails" "$(fake_ride held)" "FAIL:1 PASS:5 kept"
	t "way: a quick put-on while it rode is a WARN" "$(fake_ride quick)" "PASS:6 WARN:1 kept"
	t "way: a stand-in still on after its friend got off fails" "$(fake_ride stuckon)" "FAIL:1 PASS:5 kept"
	# unseated: stays seated (1), its saddle (1: no sample seated), no save or rejoin (1)
	t "way: a stand-in that falls off while its friend rides fails" "$(fake_ride unseated)" "FAIL:3 PASS:4 "

	# The save and the rejoin with a seated stand-in, both games faked. FK_MODE: ok, norelease (the save took the
	# stand-in out still seated), stray (a stray after the save), noseat (not seated again after the save), rejoinoff
	# (not seated again after the rejoin), leaverelease (the host's stand-in of the VM left the world still seated).
	fake_keep() {
		FK=$d/fk.$RANDOM; mkdir -p "$FK"; FK_MODE=$1; echo 0 > "$FK/saved"; echo 1 > "$FK/in"; echo 0 > "$FK/left"
		dc() { local s l rel seated; case "$1 $2" in
			"vm session status") echo "player=$([ "$(cat "$FK/left")" = 1 ] && echo 3 || echo 2)" ;;
			"host session status") echo "player=1" ;;
			"vm console save"*) echo 1 > "$FK/saved"; echo ran ;;
			*" ping") echo pong ;;
			"vm session leave") echo 0 > "$FK/in"; echo 1 > "$FK/left"; echo "left role=none" ;;
			"vm strays") [ "$FK_MODE" = stray ] && [ "$(cat "$FK/saved")" = 1 ] && echo "strays=1" || echo "strays=0" ;;
			*" strays") echo "strays=0" ;;
			"vm ride")
				s=$(cat "$FK/saved"); l=$(cat "$FK/left"); rel=$s; seated=1
				[ "$FK_MODE" = norelease ] && rel=0
				[ "$FK_MODE" = noseat ] && [ "$s" = 1 ] && seated=0
				[ "$FK_MODE" = rejoinoff ] && [ "$l" = 1 ] && seated=0
				echo "ours on=1 mount=FF000AB0@$([ "$l" = 1 ] && echo 3 || echo 2) | rider player=1 on=1 mount=FF000AB1@1 engineOn=$seated onOther=0 seated=$seated gap=10 | rides sent=1 releases=$rel" ;;
			"host ride")
				l=$(cat "$FK/left"); rel=$l; [ "$FK_MODE" = leaverelease ] && rel=0
				echo "ours on=1 mount=FF000AB1@1 | rider player=$([ "$l" = 1 ] && echo 3 || echo 2) on=1 mount=FF000AB0@3 engineOn=1 onOther=0 seated=1 gap=10 | rides sent=1 releases=$rel" ;;
			*) echo ok ;; esac; }
		( log_since_mark() { [ "$(cat "$FK/saved")" = 1 ] && echo "Save: saving \"$SAVE_NAME\""; }
			vm_join() { echo 1 > "$FK/in"; }; joined_both() { true; }; gone_from() { true; }; placed() { true; }
			A_OUT=$d/fk$RANDOM; mkdir -p "$A_OUT"; a_init fk >/dev/null; if [ "$2" = save ]; then ride_save 1 >/dev/null; else ride_rejoin "$3" "$4" >/dev/null; echo -n "id=$RD_ID "; fi
			cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# save ok: lives (1), its log (1), released (1), no stray (1), seated again (1)
	t "save: all of it works" "$(fake_keep ok save)" "PASS:5 "
	t "save: a stand-in taken out still seated fails" "$(fake_keep norelease save)" "FAIL:1 PASS:4 "
	t "save: a stray after the save fails" "$(fake_keep stray save)" "FAIL:1 PASS:4 "
	t "save: a stand-in not seated again fails" "$(fake_keep noseat save)" "FAIL:1 PASS:4 "
	# rejoin, the host rides and the VM watches: lives (1), strays (2), joins (1), seated again (1)
	t "rejoin (host rides): all of it works" "$(fake_keep ok rejoin host vm)" "id=1 PASS:5 "
	t "rejoin (host rides): not seated again fails" "$(fake_keep rejoinoff rejoin host vm)" "id=1 FAIL:1 PASS:4 "
	# rejoin, the VM rides and the host watches: also the host's stand-in got off first (1); the VM's new id
	t "rejoin (VM rides): all of it works, with the VM's new id" "$(fake_keep ok rejoin vm host)" "id=3 PASS:6 "
	t "rejoin (VM rides): a stand-in that left the world still seated fails" "$(fake_keep leaverelease rejoin vm host)" "id=3 FAIL:1 PASS:5 "

	# The NPC way, both games faked: the VM runs a person, the host is given a load-order horse with its console and
	# runs it, the VM's console sets the person on it; the host's copy follows as the plugin would. FN_MODE: ok, noperson
	# (the VM runs no person the host shows: the rig's), nohorse (no load-order horse: the rig's), nomount (the console's
	# activate does not seat the person: the rig's), noseat (the host's copy is never seated), kept (the host keeps
	# running the horse), stuckon (the copy stays on after the person got off).
	fake_npc() {
		FN=$d/fn.$RANDOM; mkdir -p "$FN"; FN_MODE=$1; echo 0 > "$FN/moved"; echo 0 > "$FN/on"
		dc() { local on owner part seated; case "$1 $2" in
			"vm session status") echo "player=2" ;;
			"host session status") echo "player=1" ;;
			"vm npcs list")
				on=$(cat "$FN/on"); owner=1; [ "$on" = 1 ] && [ "$FN_MODE" != kept ] && owner=2
				[ "$FN_MODE" = noperson ] && echo "npcs=0" && return
				echo "npcs=2 | 0001A001 local=0001A001 owner=2 role=run kind=human at=(100,0,0) dead=0 child=0 | 00097E1E local=00097E1E owner=$owner role=copy kind=horse at=(50,0,0) dead=0 child=0" ;;
			"host npcs list")
				on=$(cat "$FN/on"); owner=1; [ "$on" = 1 ] && [ "$FN_MODE" != kept ] && owner=2
				echo "npcs=2 | 0001A001 local=0001A001 owner=2 role=copy kind=human at=(100,0,0) dead=0 child=0 | 00097E1E local=00097E1E owner=$owner role=$([ "$owner" = 1 ] && echo run || echo copy) kind=horse at=(50,0,0) dead=0 child=0" ;;
			"host ride order") [ "$FN_MODE" = nohorse ] && echo "horse none" || echo "horse id=00097E1E base=00068D5C loaded=0 dist=9000 seen=3" ;;
			"host console 00097E1E.moveto player") echo 1 > "$FN/moved"; echo ran ;;
			"vm console 00097E1E.activate 0001A001") [ "$FN_MODE" != nomount ] && echo 1 > "$FN/on"; echo ran ;;
			"vm ride off 0001A001") echo 0 > "$FN/on"; echo "off: 0001A001" ;;
			"vm ride")
				on=$(cat "$FN/on")
				echo "ours on=0 mount=none$([ "$on" = 1 ] && echo " | said npc=0001A001 mount=00097E1E") | rides sent=1" ;;
			"host ride")
				on=$(cat "$FN/on"); [ "$FN_MODE" = stuckon ] && [ "$(cat "$FN/moved")" = 1 ] && [ -f "$FN/rode" ] && on=1
				[ "$on" = 1 ] && touch "$FN/rode"
				part=""; seated=1; [ "$FN_MODE" = noseat ] && seated=0
				[ "$on" = 1 ] && part=" | rider npc=0001A001 on=1 mount=00097E1E mountHere=00097E1E riderHere=0001A001 engineOn=$seated onOther=0 seated=$seated gap=8"
				echo "ours on=0 mount=none$part | rides sent=0 heard=1" ;;
			*) echo ok ;; esac; }
		( pos_of() { echo "0 0 0"; }; A_OUT=$d/fn$RANDOM; mkdir -p "$A_OUT"; a_init fn >/dev/null; ride_npc_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: seated (1), on the load-order horse (1), the horse goes with its rider (1), off (1)
	t "npc: all of it works" "$(fake_npc ok)" "PASS:4 "
	t "npc: no person to ride is the rig's" "$(fake_npc noperson)" "RIG:1 "
	t "npc: no load-order horse is the rig's" "$(fake_npc nohorse)" "RIG:1 "
	t "npc: a console activate that seats no one is the rig's" "$(fake_npc nomount)" "RIG:1 "
	t "npc: a copy never seated fails" "$(fake_npc noseat)" "FAIL:1 PASS:3 "
	t "npc: a horse that stays with the watcher fails" "$(fake_npc kept)" "FAIL:1 PASS:3 "
	t "npc: a copy still on after the person got off fails" "$(fake_npc stuckon)" "FAIL:1 PASS:3 "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "ride is a step" "$(case " $STEPS " in *" ride "*) echo yes ;; esac)" yes
	t "ride comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' ride')" 1
	t "ride needs a session" "$(case " $SESSION_STEPS " in *" ride "*) echo yes ;; esac)" yes
	t "ride runs by default" "$(case " $OPTIN_STEPS " in *" ride "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "ride.sh selftest: all ok" || echo "ride.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
