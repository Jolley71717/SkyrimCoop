#!/bin/bash
# steps/sit.sh: the rig step `sit` (protocol 14, docs/DESIGN.md "Furniture"), sourced by steps.sh. A friend who sits in
# a chair or works at a crafting station is shown there: their stand-in is put in the other game's copy of that
# furniture by the engine's own way in, stays in it through a save and a cell change, and gets up with them; an NPC in
# furniture of any kind (a seat, a bed, a lean spot, a work spot) is shown the same way. Not run in a game yet.
#
#   step_sit     the step
#   bash tools/rig/steps/sit.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# A player, each way (the mover sits, the watcher's stand-in of it is read), both players near each other:
#   1. `sit find <kind>` in the mover: the nearest free furniture of that kind (sit: a chair or a bench; work: a crafting
#      station). No seat near is a rig fault (the scene); no crafting station near is a WARN (that leg is not tried).
#   2. The mover walks to within SIT_REACH of it with its virtual pad, turns its view with the right stick until its
#      crosshair is on it (`sit find`: aimed=1, the same id; never on it is a rig fault) and presses A (the game's own
#      activation). Not in it after SIT_WAIT (`sit`: ours in=1 furn=<that id>) is a rig fault.
#   3. In the watcher, the stand-in of the mover is in its copy of that furniture within SIT_WAIT (its sitter part:
#      seated=1 inWanted=1), the same reference (found=) and kind. The engine's furniture play in the mover was kept
#      from the others (eventsKept= rose; none kept is a WARN: the engine may send none).
#   4. Removal while in it (the class of the F4MP crash after a cell change with a stand-in in a terminal): when the VM
#      watches, the VM saves (never the host) with its stand-in in the furniture: it lives, the stand-in was taken out
#      before the save took it (releases= rose), no stray, and it is in it again after. At the crafting station, the
#      watcher's player goes to an interior and back with the console (`coc SIT_CELL`, then `player.moveto <the
#      station>`: placement, set-up): both games live, no stray, and the stand-in is in the station again.
#   5. The mover gets up: from a seat with the left stick (the game's own getting up), from a crafting station with B
#      (closing its menu). Not up after SIT_WAIT is a rig fault. In the watcher, the stand-in is up within SIT_WAIT (no
#      sitter part, or on=0 state=0 occupies=00000000).
# An NPC, each way and each kind (sit, sleep, lean, work; the mover's game runs the NPC):
#   6. A person the mover runs that the watcher shows (the nearest to the mover), and the nearest free furniture of that
#      kind to the mover, marker furniture too (`sit find <kind> SIT_RANGE markers`); the mover's console
#      `<furniture>.activate <person>` puts the person in it (set-up). For a seat, none found or the person not in it
#      after SIT_NPC_WAIT is a rig fault; for the other kinds a WARN (not every scene has a bed or a lean spot near).
#   7. In the watcher, the copy of that person is in its copy of that furniture (seated=1 inWanted=1, found= the same).
#   8. `sit off <person>` in the mover's game (set-up): the watcher's copy is up within SIT_WAIT.
# Not on the rig yet: a dragon on its perch, a player in a bed (the game's sleep menu), a handover of an NPC in furniture
# from one runner to the other, two players on one bench (the unit tests cover their rules).

SIT_WAIT=${SIT_WAIT:-12}              # s: for a sit to start or end in the mover's game, and to show in the watcher's
SIT_NPC_WAIT=${SIT_NPC_WAIT:-20}      # s: for an NPC to walk to its furniture and be in it
SIT_REACH=${SIT_REACH:-110}           # u: the mover walks to within this of the furniture before it presses A
SIT_RANGE=${SIT_RANGE:-3000}          # u: furniture is looked for within this of the mover
SIT_TURNS=${SIT_TURNS:-16}            # short turns of the view to put the crosshair on it (16 x 22 degrees: all round)
SIT_CELL=${SIT_CELL:-RiverwoodSleepingGiantInn}  # the interior the watcher goes to and back from (Skyrim.esm's)

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# st_ours <sit reply>: our player's part (before the first " | ")
st_ours() { echo "${1%% | *}"; }
# st_sitter <sit reply> <player id>: that player's sitter part ("sitter player=N ..."); nothing when not listed
st_sitter() { awk -v p="player=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "sitter" && $2 == p { print; exit }' <<<"$1"; }
# st_sitter_npc <sit reply> <NPC key>: that NPC's sitter part ("sitter npc=KEY ..."); nothing when not listed
st_sitter_npc() { awk -v p="npc=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "sitter" && $2 == p { print; exit }' <<<"$1"; }
# st_said_npc <sit reply> <NPC key>: the furniture our game said that NPC of ours is in; nothing when none
st_said_npc() { awk -v p="npc=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "said" && $2 == p { sub(/^furn=/, "", $3); print $3; exit }' <<<"$1"; }
# st_counts <sit reply>: the counts' part ("sits sent=...")
st_counts() { awk 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "sits" { print; exit }' <<<"$1"; }
# st_find <sit find reply> <field>: a field of the furniture found; nothing when none
st_find() { [[ $1 == "furniture id="* ]] && field "$2" "$1"; }
# st_seated_in <sitter part>: in the furniture said, as the engine has it
st_seated_in() { [ "$(field seated "$1")" = 1 ] && [ "$(field inWanted "$1")" = 1 ]; }
# st_up_in <sitter part>: up (none listed, or said up, standing and in nothing as the engine has it)
st_up_in() { [ -z "$1" ] || { [ "$(field on "$1")" = 0 ] && [ "$(field state "$1")" = 0 ] && [ "$(field occupies "$1")" = 00000000 ]; }; }
# st_in_part <ours part> <furniture id>: our player in that furniture
st_in_part() { [ "$(field in "$1")" = 1 ] && [ "$(field furn "$1")" = "$2" ]; }

# Waits' tests.
st_in() { st_in_part "$(st_ours "$(dc "$1" sit)")" "$2"; }
st_out() { [ "$(field in "$(st_ours "$(dc "$1" sit)")")" = 0 ]; }
st_seated() { st_seated_in "$(st_sitter "$(dc "$1" sit)" "$2")"; }
st_up() { st_up_in "$(st_sitter "$(dc "$1" sit)" "$2")"; }
st_npc_said() { [ "$(st_said_npc "$(dc "$1" sit)" "$2")" = "$3" ]; }
st_npc_seated() { st_seated_in "$(st_sitter_npc "$(dc "$1" sit)" "$2")"; }
st_npc_up() { st_up_in "$(st_sitter_npc "$(dc "$1" sit)" "$2")"; }
# st_aimed <who> <kind> <id>: the crosshair on that furniture
st_aimed() { local f; f=$(dc "$1" "sit find $2 600"); [ "$(st_find "$f" id)" = "$3" ] && [ "$(st_find "$f" aimed)" = 1 ]; }
st_walk_over() { [[ $(dc "$1" walkto) == "not walking"* ]]; }

# sit_save <host's id>: the VM saves while its stand-in of the host's player is in furniture (never the host)
sit_save() {
	local id=$1 r rel0 saves
	section "sit: the VM saves while its stand-in of the host is in furniture (the host never saves)"
	rel0=$(field releases "$(st_counts "$(dc vm sit)")")
	saves=$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { fail "sit save: the VM saves" "the console refused: $r"; return 1; }
	sleep 8
	if [ "$(dc vm ping)" != pong ]; then
		fail "sit save: the VM's game lives through its save with a stand-in in furniture" "its dev channel went silent after the save (a crash? see the crash scan)"
		_rig_stop "the VM's game went down" "after its save with a stand-in in furniture"
		return 1
	fi
	pass "sit save: the VM's game lives through its save with a stand-in in furniture" "its dev channel answers"
	expect_ge "sit save: the VM's log says it saved" "$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)" $((saves + 1))
	if is_num "$rel0"; then
		expect_ge "sit save: the stand-in was taken out of its furniture before the save took it (releases=)" "$(field releases "$(st_counts "$(dc vm sit)")")" $((rel0 + 1))
	else
		fail "sit save: the stand-in was taken out of its furniture before the save took it (releases=)" "no releases= count before the save: $(st_counts "$(dc vm sit)")"
	fi
	expect_zero "sit save: no stray stand-in after the save (strays=)" "$(field strays "$(dc vm strays)")"
	if wait_until 30 placed vm && wait_until "$SIT_WAIT" st_seated vm "$id"; then
		pass "sit save: the VM's stand-in of the host is in its furniture again after the save" "$(st_sitter "$(dc vm sit)" "$id" | cut -c1-200)"
	else
		fail "sit save: the VM's stand-in of the host is in its furniture again after the save" "$(st_sitter "$(dc vm sit)" "$id" | cut -c1-200)"
	fi
}

# sit_cell <watcher> <mover's id> <furniture id>: the watcher's player goes to an interior and back while its stand-in
# of the mover is in that furniture (console placement, set-up)
sit_cell() {
	local w=$1 id=$2 furn=$3 r
	section "sit: the $w goes to $SIT_CELL and back while its stand-in of the mover is at the station (console placement, set-up)"
	r=$(dc "$w" "console coc $SIT_CELL"); [ "$r" = ran ] || { rig_fault state "sit cell: the $w goes to $SIT_CELL (set-up)" "the console refused coc $SIT_CELL: $r"; return 0; }
	sleep 10
	if [ "$(dc host ping)" != pong ] || [ "$(dc vm ping)" != pong ]; then
		fail "sit cell: both games live through the $w's cell change with a stand-in in furniture" "$(alive)"
		_rig_stop "a game went down" "after the $w left the cell of a stand-in in furniture"
		return 1
	fi
	r=$(dc "$w" "console player.moveto $furn"); [ "$r" = ran ] || { rig_fault state "sit cell: the $w comes back to the station (set-up)" "the console refused player.moveto $furn: $r"; return 0; }
	sleep 10
	if [ "$(dc host ping)" != pong ] || [ "$(dc vm ping)" != pong ]; then
		fail "sit cell: both games live through the $w's cell change with a stand-in in furniture" "$(alive)"
		_rig_stop "a game went down" "after the $w came back to a stand-in in furniture"
		return 1
	fi
	pass "sit cell: both games live through the $w's cell change with a stand-in in furniture" "$(alive)"
	expect_zero "sit cell: no stray stand-in in the host's game (strays=)" "$(field strays "$(dc host strays)")"
	expect_zero "sit cell: no stray stand-in in the VM's game (strays=)" "$(field strays "$(dc vm strays)")"
	if wait_until 30 placed "$w" && wait_until "$SIT_WAIT" st_seated "$w" "$id"; then
		pass "sit cell: the $w's stand-in is at the station again after the cell change" "$(st_sitter "$(dc "$w" sit)" "$id" | cut -c1-200)"
	else
		fail "sit cell: the $w's stand-in is at the station again after the cell change" "$(st_sitter "$(dc "$w" sit)" "$id" | cut -c1-200)"
	fi
}

# sit_way <mover> <watcher> <sit|work>: the mover's player in a seat or at a crafting station
sit_way() {
	local m=$1 w=$2 kind=$3 label="sit $3 ($1 sits, $2 watches)" id found furn fx fy i part k0 k1
	id=$(field player "$(dc "$m" 'session status')")
	# 1. furniture by the mover
	found=$(dc "$m" "sit find $kind $SIT_RANGE")
	furn=$(st_find "$found" id)
	if [ -z "$furn" ]; then
		if [ "$kind" = sit ]; then
			rig_fault state "$label: a free seat near the $m (set-up)" "sit find $kind: $found"
		else
			warn "$label: a free crafting station near the $m (that leg is not tried)" "sit find $kind: $found"
		fi
		return
	fi
	say "sit: the $m walks to the $kind furniture $furn ($found)"
	# 2. in it with the game's own activation
	fx=$(st_find "$found" x); fy=$(st_find "$found" y)
	[[ $(dc "$m" "walkto $fx $fy 0.5 $SIT_REACH 30") == walking* ]] && wait_until 35 st_walk_over "$m"
	still "$m"
	for ((i = 0; i < SIT_TURNS; i++)); do
		st_aimed "$m" "$kind" "$furn" && break
		dc "$m" 'pad stick r 0.5 0 150' >/dev/null; sleep 0.4
	done
	if ! st_aimed "$m" "$kind" "$furn"; then
		rig_fault input "$label: the $m's crosshair on the furniture (set-up)" "$(dc "$m" 'sit find '"$kind"' 600')"
		return
	fi
	k0=$(field eventsKept "$(st_counts "$(dc "$m" sit)")")
	dc "$m" 'pad button a' >/dev/null
	if ! wait_until "$SIT_WAIT" st_in "$m" "$furn"; then
		rig_fault input "$label: the $m's player gets in (A on it; set-up)" "$(st_ours "$(dc "$m" sit)")"
		dc "$m" 'pad button b' >/dev/null; still "$m"
		return
	fi
	say "sit: the $m's player is in $furn ($(st_ours "$(dc "$m" sit)"))"

	# 3. the watcher's stand-in in its copy of it
	if wait_until "$SIT_WAIT" st_seated "$w" "$id"; then
		part=$(st_sitter "$(dc "$w" sit)" "$id")
		pass "$label: the $w's stand-in of the $m is in its copy of the furniture (seated=1 inWanted=1)" "$(cut -c1-220 <<<"$part")"
	else
		part=$(st_sitter "$(dc "$w" sit)" "$id")
		fail "$label: the $w's stand-in of the $m is in its copy of the furniture (seated=1 inWanted=1)" "${part:-no sitter part for player $id: the sit never came}; $(st_counts "$(dc "$w" sit)")"
	fi
	if [ -n "$part" ] && [ "$(field found "$part")" = "$furn" ] && [ "$(field kind "$part")" = "$kind" ]; then
		pass "$label: the $w puts it in the same furniture ($furn), of the same kind" "marker=$(field marker "$part") markerHere=$(field markerHere "$part")"
	else
		fail "$label: the $w puts it in the same furniture ($furn), of the same kind" "the $w has found=$(field found "$part") kind=$(field kind "$part") lacked=$(field lacked "$part") taken=$(field taken "$part")"
	fi
	say "sit: the $w put it in by $(field seatedBy "$part") after $(field rounds "$part") round(s); stalls=$(field stalls "$part") quicks=$(field quicks "$part")"
	k1=$(field eventsKept "$(st_counts "$(dc "$m" sit)")")
	if is_num "$k0" && is_num "$k1" && [ "$k1" -gt "$k0" ]; then
		pass "$label: the engine's furniture play in the $m is not sent (eventsKept= rose)" "eventsKept=$k0 -> $k1"
	else
		warn "$label: the engine's furniture play in the $m is not sent (eventsKept= rose)" "eventsKept=$k0 -> $k1: none kept (the engine may send none; see the $w's stand-in for a doubled play)"
	fi

	# 4. removal while in it: a save in the VM (when it watches), a cell change of the watcher (at the station)
	if st_seated "$w" "$id"; then
		[ "$w" = vm ] && { sit_save "$id" || return; }
		[ "$kind" = work ] && { sit_cell "$w" "$id" "$furn" || return; }
	else
		fail "$label: a stand-in in furniture for the save and cell checks" "the $w's stand-in is not in it: those checks are not made"
	fi

	# 5. up again
	if [ "$kind" = work ]; then dc "$m" 'pad button b' >/dev/null; else dc "$m" 'pad stick l 0 1 800' >/dev/null; fi
	if ! wait_until "$SIT_WAIT" st_out "$m"; then
		rig_fault input "$label: the $m's player gets up (set-up)" "$(st_ours "$(dc "$m" sit)")"
		still "$m"; return
	fi
	still "$m"
	if wait_until "$SIT_WAIT" st_up "$w" "$id"; then
		pass "$label: the $w's stand-in gets up with its friend" "$(st_counts "$(dc "$w" sit)")"
	else
		fail "$label: the $w's stand-in gets up with its friend" "$(st_sitter "$(dc "$w" sit)" "$id" | cut -c1-220)"
	fi
}

# sit_npc_way <mover> <watcher> <sit|sleep|lean|work>: a person the mover runs in furniture of that kind
sit_npc_way() {
	local m=$1 w=$2 kind=$3 label="sit npc $3 ($1's person, $2 watches)" mid pick key local found furn r part
	mid=$(field player "$(dc "$m" 'session status')")
	# 6. a person of the mover's, furniture of that kind, and the console's activate
	pick=$(rd_npc_pick "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")")
	if [ -z "$pick" ]; then
		rig_fault state "$label: a person the $m runs that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"
		return
	fi
	read -r key local <<<"$pick"
	found=$(dc "$m" "sit find $kind $SIT_RANGE markers"); furn=$(st_find "$found" id)
	if [ -z "$furn" ]; then
		if [ "$kind" = sit ]; then rig_fault state "$label: a free seat near the $m (set-up)" "$found"; else warn "$label: free $kind furniture near the $m (that kind is not tried)" "$found"; fi
		return
	fi
	r=$(dc "$m" "console $furn.activate $local")
	if [ "$r" != ran ] || ! wait_until "$SIT_NPC_WAIT" st_npc_said "$m" "$key" "$furn"; then
		if [ "$kind" = sit ]; then
			rig_fault input "$label: the $m's person $key gets in $furn (the console's activate; set-up)" "console: $r; $(dc "$m" sit | cut -c1-220)"
		else
			warn "$label: the $m's person $key gets in $furn (the console's activate; that kind is not tried)" "console: $r; $(dc "$m" sit | cut -c1-220)"
		fi
		dc "$m" "sit off $local" >/dev/null
		return
	fi
	say "sit npc: the $m's person $key is in the $kind furniture $furn ($found; set-up)"

	# 7. the watcher's copy in its copy of it
	if wait_until "$SIT_WAIT" st_npc_seated "$w" "$key"; then
		pass "$label: the $w's copy of the person is in its copy of the furniture (seated=1 inWanted=1)" "$(st_sitter_npc "$(dc "$w" sit)" "$key" | cut -c1-220)"
	else
		fail "$label: the $w's copy of the person is in its copy of the furniture (seated=1 inWanted=1)" "$(st_sitter_npc "$(dc "$w" sit)" "$key" | cut -c1-220); $(st_counts "$(dc "$w" sit)")"
	fi
	part=$(st_sitter_npc "$(dc "$w" sit)" "$key")
	if [ "$(field found "$part")" = "$furn" ]; then
		pass "$label: the $w puts it in the same furniture ($furn)" "kind=$(field kind "$part") seatedBy=$(field seatedBy "$part")"
	else
		fail "$label: the $w puts it in the same furniture ($furn)" "found=$(field found "$part") lacked=$(field lacked "$part")"
	fi

	# 8. up
	r=$(dc "$m" "sit off $local")
	if [ "$r" != "off: $local" ]; then
		rig_fault state "$label: the $m's person gets up (sit off; set-up)" "$r"
		return
	fi
	if wait_until "$SIT_WAIT" st_npc_up "$w" "$key"; then
		pass "$label: the $w's copy gets up with it" "$(st_counts "$(dc "$w" sit)")"
	else
		fail "$label: the $w's copy gets up with it" "$(st_sitter_npc "$(dc "$w" sit)" "$key" | cut -c1-220)"
	fi
}

step_sit() {
	local w k
	for w in host vm; do still $w; done
	sit_way vm host sit
	sit_way host vm sit
	sit_way vm host work
	sit_way host vm work
	for k in sit sleep lean work; do
		sit_npc_way vm host "$k"
		sit_npc_way host vm "$k"
	done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init sittest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Furniture::Command)
	R='ours in=1 state=3 furn=0001A2B3 base=000BB7C1 kind=sit marker=0 said=2 sent=3 | said npc=0001A693 furn=0001A2D0 base=000BAD0C kind=work | sitter player=2 on=1 kind=sit furn=0001A2C0 base=000BB7C1 found=0001A2C0 lacked=0 taken=0 near=1 dist=14 here=FF0008AE state=3 occupies=0001A2C0 inWanted=1 inOther=0 seated=1 held=1 assist=0 rounds=0 tried=activate seatedBy=activate givenUp=0 stands=0 quicks=0 stalls=0 marker=1 markerHere=1 sender=2 handover=0 heard=1 | sitter npc=0001A692 on=1 kind=sleep furn=0001A2E0 base=000F5A1B found=0001A2E0 lacked=0 taken=0 near=1 dist=30 here=0001A692 state=5 occupies=0001A2E0 inWanted=1 inOther=0 seated=0 held=1 assist=1 rounds=0 tried=activate seatedBy=none givenUp=0 stands=0 quicks=0 stalls=0 marker=255 markerHere=0 sender=2 handover=0 heard=1 | sits sent=3 heard=4 notBelieved=0 lacked=0 taken=0 activates=2 consoles=0 stands=0 quicks=0 releases=0 handedToUs=0 assistFrames=40 eventsKept=7'
	t "ours: our part only" "$(field in "$(st_ours "$R")")" 1
	t "ours: not a sitter's fields" "$(field seated "$(st_ours "$R")")" ""
	t "ours: in that furniture" "$(st_in_part "$(st_ours "$R")" 0001A2B3 && echo yes)" yes
	t "ours: not in another" "$(st_in_part "$(st_ours "$R")" 0001A2C0 || echo no)" no
	t "sitter: that player's part" "$(field found "$(st_sitter "$R" 2)")" 0001A2C0
	t "sitter: none not listed" "$(st_sitter "$R" 3)" ""
	t "sitter: not player 1 in player 12" "$(st_sitter "${R/player=2/player=12}" 1)" ""
	t "sitter npc: that NPC's part" "$(field kind "$(st_sitter_npc "$R" 0001A692)")" sleep
	t "sitter npc: none not listed" "$(st_sitter_npc "$R" 0001A693)" ""
	t "said npc: the furniture said" "$(st_said_npc "$R" 0001A693)" 0001A2D0
	t "said npc: none" "$(st_said_npc "$R" 0001A692)" ""
	t "counts" "$(field eventsKept "$(st_counts "$R")")" 7
	t "seated" "$(st_seated_in "$(st_sitter "$R" 2)" && echo yes)" yes
	t "seated: not while getting in" "$(st_seated_in "$(st_sitter_npc "$R" 0001A692)" || echo no)" no
	t "seated: not in other furniture" "$(st_seated_in "$(st_sitter "${R/inWanted=1 inOther=0 seated=1/inWanted=0 inOther=1 seated=1}" 2)" || echo no)" no
	t "up: none listed" "$(st_up_in "" && echo yes)" yes
	t "up: said up, standing, in nothing" "$(st_up_in 'sitter player=2 on=0 state=0 occupies=00000000' && echo yes)" yes
	t "up: not while still in it" "$(st_up_in 'sitter player=2 on=0 state=3 occupies=0001A2C0' || echo no)" no
	t "up: not while getting up" "$(st_up_in 'sitter player=2 on=0 state=4 occupies=00000000' || echo no)" no
	t "find: the furniture" "$(st_find 'furniture id=0001A2B3 base=000BB7C1 kind=sit bench=0 markers=2 dist=250 x=10 y=20 z=30 aimed=1' id)" 0001A2B3
	t "find: aimed" "$(st_find 'furniture id=0001A2B3 base=000BB7C1 kind=sit bench=0 markers=2 dist=250 x=10 y=20 z=30 aimed=0' aimed)" 0
	t "find: none" "$(st_find 'furniture none' id)" ""

	# The player way, both games faked: the mover finds furniture, its view turns onto it (aimed after a few turns), A
	# puts it in and the left stick or B gets it up; the watcher's stand-in follows as the plugin would. The save and the
	# cell change are faked apart (sit_save and sit_cell are replaced: kept). FS_MODE: ok, nofurn (none found: the rig's
	# for a seat, a WARN for a station), noaim (the rig's), nosit (A does not put it in: the rig's), noseat (the stand-in
	# is never put in), wrongfurn (put in other furniture), nokept (no event kept: a WARN), stuck (the stand-in stays in
	# after its friend got up), noup (the mover does not get up: the rig's).
	fake_sit() {
		FS=$d/fs.$RANDOM; mkdir -p "$FS"; FS_MODE=$1
		echo 0 > "$FS/turns"; echo 0 > "$FS/in"; echo 0 > "$FS/was"; echo 0 > "$FS/keeps"
		dc() { local in part seated found kept st occ k; case "$1 $2" in
			"vm session status") echo "role=client connection=up welcomed=1 player=2 host=0 players=2" ;;
			"vm sit find "*)
				k=sit; [ "$FS_KIND" = work ] && k=work
				if [ "$FS_MODE" = nofurn ]; then echo "furniture none"
				else local aimed=0; [ "$(cat "$FS/turns")" -ge 3 ] && [ "$FS_MODE" != noaim ] && aimed=1
					echo "furniture id=0001A2B3 base=000BB7C1 kind=$k bench=0 markers=2 dist=300 x=500 y=600 z=0 aimed=$aimed"; fi ;;
			"vm walkto "[0-9]*) echo "walking to (500,600)" ;;
			*" walkto") echo "not walking; last: arrived" ;;
			"vm pad stick r"*) echo $(( $(cat "$FS/turns") + 1 )) > "$FS/turns"; echo ok ;;
			"vm pad button a") [ "$FS_MODE" != nosit ] && { echo 1 > "$FS/in"; echo 1 > "$FS/was"; }; echo ok ;;
			"vm pad stick l"* | "vm pad button b") [ "$FS_MODE" != noup ] && echo 0 > "$FS/in"; echo ok ;;
			"vm sit")
				in=$(cat "$FS/in"); kept=$([ "$in" = 1 ] && [ "$FS_MODE" != nokept ] && echo 9 || echo 2)
				echo "ours in=$in state=$([ "$in" = 1 ] && echo 3 || echo 0) furn=$([ "$in" = 1 ] && echo 0001A2B3 || echo 00000000) base=000BB7C1 kind=sit marker=0 said=$in sent=1 | sits sent=1 heard=0 eventsKept=$kept" ;;
			"host sit")
				in=$(cat "$FS/in"); [ "$FS_MODE" = stuck ] && [ "$(cat "$FS/was")" = 1 ] && in=1
				k=sit; [ "$FS_KIND" = work ] && k=work
				part=""
				if [ "$in" = 1 ]; then
					seated=1; found=0001A2B3; st=3; occ=0001A2B3
					case "$FS_MODE" in noseat) seated=0; st=0; occ=00000000 ;; wrongfurn) found=0001A2B4 ;; esac
					part=" | sitter player=2 on=1 kind=$k furn=0001A2B3 base=000BB7C1 found=$found lacked=0 taken=0 near=1 dist=10 here=FF0008AE state=$st occupies=$occ inWanted=$seated inOther=0 seated=$seated held=1 assist=0 rounds=0 tried=activate seatedBy=activate givenUp=0 stands=0 quicks=0 stalls=0 marker=0 markerHere=0 sender=2 handover=0 heard=1"
				fi
				echo "ours in=0 state=0 furn=00000000 base=00000000 kind=- marker=255 said=0 sent=0$part | sits sent=0 heard=2 releases=0 eventsKept=0" ;;
			*) echo ok ;; esac; }
		( sit_save() { echo 1 > "$FS/keeps"; }; sit_cell() { echo 2 > "$FS/keeps"; }
			A_OUT=$d/fs$RANDOM; mkdir -p "$A_OUT"; a_init fs >/dev/null; sit_way vm host "${FS_KIND:-sit}" >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}'; echo -n "keeps=$(cat "$FS/keeps")" )
	}
	# ok: in it (1), the same furniture (1), events kept (1), up (1); the host watches: no save, a seat: no cell change
	t "way: all of it works" "$(fake_sit ok)" "PASS:4 keeps=0"
	t "way: no seat is the rig's, and nothing else is tried" "$(fake_sit nofurn)" "RIG:1 keeps=0"
	t "way: a crosshair never on it is the rig's" "$(fake_sit noaim)" "RIG:1 keeps=0"
	t "way: a mover A does not put in is the rig's" "$(fake_sit nosit)" "RIG:1 keeps=0"
	# noseat: not in (1), no save or cell (1); the same furniture is still found (1), kept (1), up (1)
	t "way: a stand-in never put in fails" "$(fake_sit noseat)" "FAIL:2 PASS:3 keeps=0"
	t "way: a stand-in put in other furniture fails" "$(fake_sit wrongfurn)" "FAIL:1 PASS:3 keeps=0"
	t "way: no event kept is a WARN" "$(fake_sit nokept)" "PASS:3 WARN:1 keeps=0"
	t "way: a stand-in still in after its friend got up fails" "$(fake_sit stuck)" "FAIL:1 PASS:3 keeps=0"
	t "way: a mover that does not get up is the rig's" "$(fake_sit noup)" "PASS:3 RIG:1 keeps=0"
	t "way: at a station the watcher changes cell" "$(FS_KIND=work fake_sit ok)" "PASS:4 keeps=2"
	t "way: no station is a WARN" "$(FS_KIND=work fake_sit nofurn)" "WARN:1 keeps=0"

	# The NPC way, both games faked: the VM runs a person the host shows, its console puts it in furniture, `sit off`
	# gets it up; the host's copy follows as the plugin would. FN_MODE: ok, noperson (the rig's), nofurn (the rig's for a
	# seat, a WARN for the others), nosit (the console's activate puts no one in: the rig's for a seat, a WARN for the
	# others), noseat (the host's copy is never put in), stuck (the copy stays in after the person got up).
	fake_npc_sit() {
		FN=$d/fn.$RANDOM; mkdir -p "$FN"; FN_MODE=$1; echo 0 > "$FN/in"; echo 0 > "$FN/was"
		dc() { local in part seated; case "$1 $2" in
			"vm session status") echo "player=2" ;;
			"host session status") echo "player=1" ;;
			"vm npcs list") [ "$FN_MODE" = noperson ] && { echo "npcs=0"; return; }
				echo "npcs=1 | 0001A001 local=0001A001 owner=2 role=run kind=human at=(100,0,0) dead=0 child=0" ;;
			"host npcs list") echo "npcs=1 | 0001A001 local=0001A001 owner=2 role=copy kind=human at=(100,0,0) dead=0 child=0" ;;
			"vm sit find "*) [ "$FN_MODE" = nofurn ] && echo "furniture none" || echo "furniture id=0001A2E0 base=000F5A1B kind=${FN_KIND:-sit} bench=0 markers=1 dist=200 x=1 y=2 z=3 aimed=0" ;;
			"vm console 0001A2E0.activate 0001A001") [ "$FN_MODE" != nosit ] && { echo 1 > "$FN/in"; echo 1 > "$FN/was"; }; echo ran ;;
			"vm sit off 0001A001") echo 0 > "$FN/in"; echo "off: 0001A001" ;;
			"vm sit")
				in=$(cat "$FN/in")
				echo "ours in=0 state=0 furn=00000000 base=00000000 kind=- marker=255 said=$in sent=1$([ "$in" = 1 ] && echo " | said npc=0001A001 furn=0001A2E0 base=000F5A1B kind=sleep") | sits sent=1" ;;
			"host sit")
				in=$(cat "$FN/in"); [ "$FN_MODE" = stuck ] && [ "$(cat "$FN/was")" = 1 ] && in=1
				seated=1; [ "$FN_MODE" = noseat ] && seated=0
				part=""; [ "$in" = 1 ] && part=" | sitter npc=0001A001 on=1 kind=sleep furn=0001A2E0 base=000F5A1B found=0001A2E0 state=7 occupies=0001A2E0 inWanted=$seated inOther=0 seated=$seated seatedBy=console"
				echo "ours in=0 state=0$part | sits sent=0 heard=1" ;;
			*) echo ok ;; esac; }
		( pos_of() { echo "0 0 0"; }; A_OUT=$d/fn$RANDOM; mkdir -p "$A_OUT"; a_init fn >/dev/null; sit_npc_way vm host "${FN_KIND:-sit}" >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: in it (1), the same furniture (1), up (1)
	t "npc: all of it works" "$(fake_npc_sit ok)" "PASS:3 "
	t "npc: no person is the rig's" "$(fake_npc_sit noperson)" "RIG:1 "
	t "npc: no seat is the rig's" "$(fake_npc_sit nofurn)" "RIG:1 "
	t "npc: no bed is a WARN" "$(FN_KIND=sleep fake_npc_sit nofurn)" "WARN:1 "
	t "npc: a console activate that puts no one in a seat is the rig's" "$(fake_npc_sit nosit)" "RIG:1 "
	t "npc: one that puts no one in a lean spot is a WARN" "$(FN_KIND=lean fake_npc_sit nosit)" "WARN:1 "
	# noseat: not in (1), the same furniture is still found (1), up (1)
	t "npc: a copy never put in fails" "$(fake_npc_sit noseat)" "FAIL:1 PASS:2 "
	t "npc: a copy still in after the person got up fails" "$(fake_npc_sit stuck)" "FAIL:1 PASS:2 "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "sit is a step" "$(case " $STEPS " in *" sit "*) echo yes ;; esac)" yes
	t "sit comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' sit')" 1
	t "sit needs a session" "$(case " $SESSION_STEPS " in *" sit "*) echo yes ;; esac)" yes
	t "sit runs by default" "$(case " $OPTIN_STEPS " in *" sit "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "sit.sh selftest: all ok" || echo "sit.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
