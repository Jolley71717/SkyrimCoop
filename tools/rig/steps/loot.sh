#!/bin/bash
# steps/loot.sh: the rig step `loot` (protocol 10, docs/DESIGN.md "Containers and loot"), sourced by steps.sh. What one
# player takes from or puts into a container or a body is so in the other game's copy of it, both ways; a book stays its
# player's own; a Take All that leaves a holder holding nothing shared takes the same items out of the other's copy (the
# emptied marker); after a load and after a rejoin the VM's copy is the session's again; a living actor's inventory is
# never shared, and a move at a body alive in the other game waits there for its death. Not run in a game yet.
#
#   step_loot                                   the step (in the default run, after clock, before final)
#   bash tools/rig/steps/loot.sh selftest       the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `loot`, `loot near`, `loot inv <id>`, `loot take|put <holder>
# <item> <n>` and `loot find <kind>`; `loot take` and `loot put` move as the container menu's take and store do):
#   1. Both players are put in LOOT_CELL with coc (console placement, said), each with its stand-in of the other. The
#      container is the nearest one in both games' `loot near` lists, loaded in both, not a merchant's, not storage, not
#      runtime. Each game rolls its own leveled loot, so every check compares how much a move changed each copy. The
#      legs through the container menu (6 and 7) use one that nobody owns (owned=0: taking from it is no theft), in
#      LOOT_CELL or else in LOOT_FREE_CELL; an inn's barrels belong to the inn, and a Take All there is a theft in the
#      host's world (a bounty, owners and guards reacting over the later legs). None in either: a RIG verdict for them.
#   2. Gold: the VM's player is given LOOT_GOLD (console `player.additem`, set-up) and puts it in; the host's copy holds
#      that much more. The host takes 5 out; the VM's copy holds 5 less.
#   3. A weapon (`loot find weapon`: the first plain dagger of the base game, the same in both): the host is given one
#      and puts it in, the VM's copy has it; the VM takes it, the host's copy has it no more.
#   4. A book (`loot find book`: neither a spell tome nor a skill book) the VM puts in stays the VM's own: after
#      LOOT_WAIT the host's copy holds none more, and the VM's count of moves kept as books goes up. The VM takes it back.
#   5. The session's moves after a load: the VM saves ($SAVE_NAME), then takes 2 gold (its own move) and the host puts 9
#      in; the VM loads its save, and right after its load starts (its loading screen up) the host puts 3 more in. The
#      VM asks the session (asks= and caughtUp= go up) and its copy must hold what it held before the load and the 3:
#      a move heard on the loading screen is held for the catch-up, never moving the VM's ledger past the moves its save
#      lacks (held= going up says the put landed in that window; a WARN when it landed outside it). A rejoin: the VM
#      leaves, the host takes 4 gold, the VM joins again and its copy holds 4 less.
#   6. An open menu: the host opens the unowned container (`use <id>`, the activate key's own call) and its menu shows it
#      (`loot`: menu=); the VM takes 1 gold from its copy. While the host's menu is open its copy is not changed (no write
#      to what the open menu lists: openHeld= goes up, the move waits); the host shuts the menu (pad b) and the take is
#      made in its copy within LOOT_WAIT. When the host's game heard nothing while its menu was open (a menu that pauses
#      the tick), a WARN says the hold was not exercised.
#   7. Take All: the host opens the unowned container, Take All (pad x), and shuts the menu (pad b). Its copy must hold
#      nothing shared (a RIG verdict otherwise: the menu did not take the pad), its emptiedSent= goes up, the VM's
#      emptied= goes up, and the VM's copy holds none of the items the host took (the ones only the VM's roll had more
#      of come out too).
#   8. Bodies: both are put in LOOT_BODY_CELL (coc). A creature (not essential, not runtime) near both and alive in both:
#      the VM puts 3 gold into it, alive, and the host's copy holds none more (a living actor's inventory is its own).
#      The VM kills its copy (console `kill`, set-up: each game's own fight is not this step's), puts 4 gold into the
#      body. While actors' deaths are each game's own, the host's copy, alive, waits for its death (waiting= at least 1,
#      its gold unchanged); the host kills its copy, and the 4 gold is in it within LOOT_WAIT. Once deaths are shared
#      (the host's copy died with the VM's kill), the 4 gold is in the host's copy at once. The host takes the 4 gold;
#      the VM's copy holds 4 less.
#   9. Both are put back in the harness's cell (coc).

LOOT_CELL=${LOOT_CELL:-RiverwoodSleepingGiantInn}  # containers (chests, barrels, sacks) near the coc marker
LOOT_FREE_CELL=${LOOT_FREE_CELL:-Riverwood}        # where an unowned container is looked for when LOOT_CELL has none
LOOT_BODY_CELL=${LOOT_BODY_CELL:-Riverwood}        # chickens and dogs, killed by console in both games
LOOT_NEAR=${LOOT_NEAR:-1800}                       # u: how far from each player the containers are looked for
LOOT_BODY_NEAR=${LOOT_BODY_NEAR:-4000}             # u: ... and the creatures
LOOT_GOLD=${LOOT_GOLD:-20}                         # gold the VM's player is given and puts in
LOOT_WAIT=${LOOT_WAIT:-10}                         # s: for a move to reach the other game and be made there
LOOT_GOLD_ID=0000000F                              # Skyrim.esm's gold

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# loot_lines <loot near reply>: its holder lines, one a line ("near=N | line | line ...")
loot_lines() { sed 's/ | /\n/g' <<<"$1" | tail -n +2; }
# loot_count <loot inv reply> <item>: how many of the item the holder holds as plain stacks (0: none); empty for a reply
# that is not a holder's
loot_count() {
	[[ $1 == ref=* ]] || return 0
	[[ $1 == *" | "* ]] || { echo 0; return 0; }
	tr ' ' '\n' <<<"${1#* | }" | awk -F: -v id="$2" '$1 == id { print $2; f = 1 } END { if (!f) print 0 }'
}
# loot_shared <loot inv reply>: the items it holds the session shares (plain count above 0), one a line
loot_shared() {
	[[ $1 == *" | "* ]] || return 0
	tr ' ' '\n' <<<"${1#* | }" | awk -F: '$4 == "shared" && $2 > 0 { print $1 }'
}
# loot_ok <line> <kind> [unowned]: a holder line of that kind, loaded, neither a merchant's nor storage nor runtime; a
# creature that is not essential for kind npc; with unowned, owned by nobody (owned=0, or a reply without the field:
# never taken for unowned)
loot_ok() {
	[ "$(field kind "$1")" = "$2" ] && [ "$(field loaded "$1")" = 1 ] && [ "$(field vendor "$1")" = 0 ] &&
		[ "$(field storage "$1")" = 0 ] && [ "$(field runtime "$1")" = 0 ] || return 1
	[ "${3:-}" != unowned ] || [ "$(field owned "$1")" = 0 ] || return 1
	[ "$2" != npc ] || { [ "$(field creature "$1")" = 1 ] && [ "$(field essential "$1")" = 0 ]; }
}
# loot_pick <host's loot near> <vm's loot near> <kind> [unowned]: the nearest holder of that kind (loot_ok) in the host's
# list that the VM's holds too, the same kind and loaded there (and owned by nobody in both, with unowned); 1 when there
# is none
loot_pick() {
	local line id vline
	while IFS= read -r line; do
		loot_ok "$line" "$3" "${4:-}" || continue
		id=$(field ref "$line")
		vline=$(loot_lines "$2" | grep "^ref=$id " | head -1)
		[ -n "$vline" ] && loot_ok "$vline" "$3" "${4:-}" || continue
		echo "$id"; return 0
	done < <(loot_lines "$1")
	return 1
}
# loot_kinds <loot near reply>: what the listing holds, "kind:ref:owned" each, for a report when the one wanted is not
# there
loot_kinds() { local line; while IFS= read -r line; do echo -n "$(field kind "$line"):$(field ref "$line"):owned=$(field owned "$line") "; done < <(loot_lines "$1"); }
# loot_kept <loot reply> <scope>: how many of our player's moves that game kept as its own for that scope (0: none)
loot_kept() { local k; k=$(field kept "$1"); tr ',' '\n' <<<"$k" | awk -F: -v s="$2" '$1 == s { print $2; f = 1 } END { if (!f) print 0 }'; }

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

loot_n() { loot_count "$(dc "$1" "loot inv $2")" "$3"; }
# loot_has <who> <holder> <item> <want>: that game's copy holds want of the item
loot_has() { [ "$(loot_n "$1" "$2" "$3")" = "$4" ]; }
# loot_expect <check> <who> <holder> <item> <want>: within LOOT_WAIT that game's copy holds so many (PASS), or FAIL
loot_expect() {
	if wait_until "$LOOT_WAIT" loot_has "$2" "$3" "$4" "$5"; then pass "$1" "$2 holds $5 of $4 in $3"
	else fail "$1" "want $5 of $4, $2 holds $(loot_n "$2" "$3" "$4"): $(dc "$2" "loot inv $3" | cut -c1-200)"; return 1; fi
}
# loot_own <check> <who> <holder> <item> <want>: the game that moved reads it itself; when not, the move did not take
# (the rig's input, not the mod: a RIG verdict, and the rest of that check is not made)
loot_own() {
	if wait_until "$LOOT_WAIT" loot_has "$2" "$3" "$4" "$5"; then pass "$1" "$2 holds $5 of $4"; return 0; fi
	rig_fault input "$1" "$2's own copy did not change (want $5 of $4, holds $(loot_n "$2" "$3" "$4"))"; return 1
}
# loot_run <check> <who> <command> <want>: a dev command whose reply must be <want>; a rig fault otherwise
loot_run() {
	local r; r=$(dc "$2" "$3")
	[ "$r" = "$4" ] && return 0
	rig_broken input "$1" "$2: '$3' replied '$r'"; return 1
}
# loot_give <who> <item> <n>: that game's player is given n of the item (console, set-up)
loot_give() {
	local before
	before=$(loot_n "$1" 14 "$2")
	loot_run "$1: is given $3 of $2 (console player.additem, set-up)" "$1" "console player.additem $2 $3" ran || return 1
	wait_until 5 loot_has "$1" 14 "$2" $((${before:-0} + $3)) ||
		{ rig_fault input "$1: is given $3 of $2 (console player.additem, set-up)" "its player holds $(loot_n "$1" 14 "$2")"; return 1; }
}
# loot_find <kind>: the item of that kind both games find (`loot find`); a RIG verdict when they differ or find none
loot_find() {
	local h v
	h=$(field found "$(dc host "loot find $1")"); v=$(field found "$(dc vm "loot find $1")")
	if [ -n "$h" ] && [ "$h" != none ] && [ "$h" = "$v" ]; then echo "$h"; return 0; fi
	rig_unhealed state "both: find the same $1 to move" "host found '$h', vm '$v'" >&2; return 1
}

# loot_trade <mover> <take|put> <holder> <item> <n>: the mover moves it; its copy and the other's change by as much
loot_trade() {
	local mover=$1 how=$2 holder=$3 item=$4 n=$5 watcher m0 w0 sign=-1 reply="took $5"
	watcher=$(other "$mover")
	[ "$how" = put ] && sign=1 reply="put $n"
	m0=$(loot_n "$mover" "$holder" "$item"); w0=$(loot_n "$watcher" "$holder" "$item")
	is_num "$m0" && is_num "$w0" || { rig_broken game "both: read $holder" "$mover [$(dc "$mover" "loot inv $holder" | cut -c1-120)] $watcher [$(dc "$watcher" "loot inv $holder" | cut -c1-120)]"; return 1; }
	if [ "$how" = take ] && [ "$m0" -lt "$n" ]; then
		rig_unhealed state "$mover: has $n of $item in $holder to take" "it holds $m0"; return 1
	fi
	loot_run "$mover: ${how}s $n of $item ($holder)" "$mover" "loot $how $holder $item $n" "$reply" || return 1
	loot_own "$mover: ${how}s $n of $item ($holder)" "$mover" "$holder" "$item" $((m0 + sign * n)) || return 1
	loot_expect "$watcher: its copy of $holder holds the $n of $item the $mover ${how/take/took} $([ "$how" = put ] && echo more || echo less)" \
		"$watcher" "$holder" "$item" $((w0 + sign * n))
}

# loot_personal <who> <holder> <item> <scope>: an item of that scope the player puts in stays its own: the other's copy
# holds none more after LOOT_WAIT, and the player's game counts the move as kept; the player takes it back
loot_personal() {
	local who=$1 holder=$2 item=$3 scope=$4 watcher w0 m0 k0
	watcher=$(other "$who")
	w0=$(loot_n "$watcher" "$holder" "$item"); m0=$(loot_n "$who" "$holder" "$item"); k0=$(loot_kept "$(dc "$who" loot)" "$scope")
	loot_run "$who: puts the $scope $item in $holder" "$who" "loot put $holder $item 1" "put 1" || return 1
	loot_own "$who: puts the $scope $item in $holder" "$who" "$holder" "$item" $((m0 + 1)) || return 1
	sleep "$LOOT_WAIT"
	expect_eq "$watcher: its copy of $holder holds no more of the $scope the $who put in (each player's own)" "$(loot_n "$watcher" "$holder" "$item")" "$w0"
	expect_ge "$who: kept the move as its own ($scope)" "$(loot_kept "$(dc "$who" loot)" "$scope")" $((k0 + 1))
	loot_run "$who: takes the $scope $item back" "$who" "loot take $holder $item 1" "took 1" && loot_own "$who: takes the $scope $item back" "$who" "$holder" "$item" "$m0"
}

# loot_again <holder>: the session's moves after the VM loads a save (one of them made on its loading screen), and after
# it joins again
loot_again() {
	local holder=$1 g=$LOOT_GOLD_ID want asks0 caught0 held0 r host0 hp
	loot_run "vm: saves" vm "console save $SAVE_NAME" ran || return
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return; }
	loot_trade vm take "$holder" "$g" 2 || return
	loot_give host "$g" 12 && loot_trade host put "$holder" "$g" 9 || return
	want=$(loot_n vm "$holder" "$g"); host0=$(loot_n host "$holder" "$g")
	r=$(dc vm loot); asks0=$(field asks "$r"); caught0=$(field caughtUp "$r"); held0=$(field held "$r")
	loot_run "vm: loads its save (before its take and the host's put)" vm "console load $SAVE_NAME" ran || return
	# At once, while the VM's loading screen is up: the host puts 3 more in (its own read of its copy comes after).
	hp=$(dc host "loot put $holder $g 3")
	[ "$hp" = "put 3" ] || { rig_broken input "host: puts 3 gold in $holder while the VM loads" "'loot put' replied '$hp'"; return; }
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; }
	wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return; }
	loot_own "host: put 3 gold in $holder while the VM loaded" host "$holder" "$g" $((host0 + 3)) || return
	loot_expect "vm: after loading its save, its copy holds what the session's moves since make it, the host's put on its loading screen too ($want + 3 gold)" \
		vm "$holder" "$g" $((want + 3))
	r=$(dc vm loot)
	if is_num "$asks0" && is_num "$caught0" && is_num "$held0"; then
		expect_ge "vm: asked the session for its moves again after the load" "$(field asks "$r")" $((asks0 + 1))
		expect_ge "vm: caught up with the session's moves after the load" "$(field caughtUp "$r")" $((caught0 + 1))
		expect_eq "vm: no longer asking once caught up" "$(field asking "$r")" 0
		if [ "$(field held "$r")" -ge $((held0 + 1)) ] 2>/dev/null; then
			pass "vm: the host's put landed while its catch-up was coming, and was held for it" "held=$(field held "$r") (was $held0)"
		else
			warn "vm: the host's put landed while its catch-up was coming" "held=$(field held "$r") (was $held0): it landed before the load was done or after the catch-up; the loading-screen window was not exercised this run"
		fi
	else fail "vm: counts its asks" "$(cut -c1-160 <<<"$r")"; fi
	expect_eq "host: its copy changed by its own put alone, not by the VM's load" "$(loot_n host "$holder" "$g")" $((host0 + 3))

	want=$(( $(loot_n vm "$holder" "$g") - 4 ))
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	gone_from host || fail "host: the VM's stand-in goes when it leaves" "$(dc host standins | cut -c1-160)"
	host0=$(loot_n host "$holder" "$g")
	is_num "$host0" && [ "$host0" -ge 4 ] || { rig_unhealed state "host: has 4 gold in $holder to take" "it holds $host0"; return; }
	loot_run "host: takes 4 gold while the VM is away" host "loot take $holder $g 4" "took 4" &&
		loot_own "host: takes 4 gold while the VM is away" host "$holder" "$g" $((host0 - 4)) || return
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	loot_expect "vm: after joining again, its copy holds the 4 gold less the host took while it was away" vm "$holder" "$g" "$want"
}

_loot_menu() { [ "$(field menu "$(dc "$1" loot)")" = "$2" ]; }

# loot_menu_hold <holder>: while the host's container menu shows the holder, the VM's take there waits in the host's
# game (nothing is written to what the open menu lists); it is made once the menu shuts. An unowned holder: opening it
# is no theft, and nothing is taken through the menu.
loot_menu_hold() {
	local holder=$1 g=$LOOT_GOLD_ID h0 v0 r heard0 held0 heard1
	v0=$(loot_n vm "$holder" "$g")
	if ! is_num "$v0" || [ "$v0" -lt 1 ]; then
		loot_give vm "$g" 2 && loot_trade vm put "$holder" "$g" 2 || return
		v0=$(loot_n vm "$holder" "$g")
	fi
	h0=$(loot_n host "$holder" "$g")
	r=$(dc host loot); heard0=$(field heard "$r"); held0=$(field openHeld "$r")
	is_num "$h0" && is_num "$heard0" && is_num "$held0" || { rig_broken game "host: reads $holder and its loot counts" "$(cut -c1-160 <<<"$r")"; return; }
	log_mark host >/dev/null 2>&1
	loot_run "host: opens $holder (use)" host "use $holder" used || return
	if ! wait_until 5 _loot_menu host "$holder"; then
		rig_fault input "host: its container menu shows $holder" "loot says $(dc host loot | tr ' ' '\n' | grep '^menu=')"
		dc host 'pad button b' >/dev/null; return
	fi
	pass "host: its container menu shows $holder" "menu=$holder"
	if ! { loot_run "vm: takes 1 gold from $holder while the host has it open" vm "loot take $holder $g 1" "took 1" &&
		loot_own "vm: takes 1 gold from $holder while the host has it open" vm "$holder" "$g" $((v0 - 1)); }; then
		dc host 'pad button b' >/dev/null; return
	fi
	sleep "$LOOT_WAIT"
	r=$(dc host loot); heard1=$(field heard "$r")
	if is_num "$heard1" && [ "$heard1" -gt "$heard0" ]; then
		expect_eq "host: its copy of $holder is not changed while its menu shows it" "$(loot_n host "$holder" "$g")" "$h0"
		expect_ge "host: held the move for its open menu" "$(field openHeld "$r")" $((held0 + 1))
		expect_ge "host: the move waits for the menu to shut" "$(field waiting "$r")" 1
		[ "$held0" != 0 ] || if log_since_mark host 'waits while our container menu shows it' >/dev/null; then
			pass "host: its log says the move waits for the menu" "Loot: a move at ... waits while our container menu shows it"
		else fail "host: its log says the move waits for the menu" "no such line since the mark"; fi
	else
		warn "host: heard the VM's take while its menu was open" "heard=$heard1 (was $heard0): the menu paused the host's tick, so the hold was not exercised"
	fi
	dc host 'pad button b' >/dev/null
	wait_until 5 _loot_menu host 00000000 || { rig_fault input "host: shuts its container menu (pad b)" "loot says $(dc host loot | tr ' ' '\n' | grep '^menu=')"; return; }
	loot_expect "host: once its menu shut, the VM's take is made in its copy of $holder" host "$holder" "$g" $((h0 - 1))
}

# loot_takeall <who> <holder>: Take All through the container menu; the other's copy holds none of what was taken
loot_takeall() {
	local who=$1 holder=$2 watcher items sent0 emptied0 item left
	watcher=$(other "$who")
	items=$(loot_shared "$(dc "$who" "loot inv $holder")")
	if [ -z "$items" ]; then
		loot_give "$who" "$LOOT_GOLD_ID" 3 && loot_trade "$who" put "$holder" "$LOOT_GOLD_ID" 3 || return
		items=$(loot_shared "$(dc "$who" "loot inv $holder")")
	fi
	sent0=$(field emptiedSent "$(dc "$who" loot)"); emptied0=$(field emptied "$(dc "$watcher" loot)")
	is_num "$sent0" && is_num "$emptied0" || { fail "both: count the emptied markers" "$who $(dc "$who" loot | cut -c1-120); $watcher $(dc "$watcher" loot | cut -c1-120)"; return; }
	loot_run "$who: opens $holder (use)" "$who" "use $holder" used || return
	sleep 2
	dc "$who" 'pad button x' >/dev/null; sleep 2
	dc "$who" 'pad button b' >/dev/null; sleep 1
	left=$(loot_shared "$(dc "$who" "loot inv $holder")")
	[ -z "$left" ] || { rig_fault input "$who: Take All (pad x) emptied its copy of $holder" "it still holds $(tr '\n' ' ' <<<"$left"): the container menu did not take the pad"; return; }
	pass "$who: Take All emptied its copy of $holder of what the session shares" "took $(tr '\n' ' ' <<<"$items")"
	expect_ge "$who: said $holder emptied" "$(field emptiedSent "$(dc "$who" loot)")" $((sent0 + 1))
	wait_until "$LOOT_WAIT" _loot_ge "$watcher" emptied $((emptied0 + 1))
	expect_ge "$watcher: made the emptied marker" "$(field emptied "$(dc "$watcher" loot)")" $((emptied0 + 1))
	for item in $items; do
		loot_expect "$watcher: its copy of $holder holds none of the $item the $who took (its own roll's too)" "$watcher" "$holder" "$item" 0
	done
}
_loot_ge() { local n; n=$(field "$2" "$(dc "$1" loot)"); is_num "$n" && [ "$n" -ge "$3" ]; }

# loot_body <id>: a creature alive in both: a put into it alive is the VM's own; once dead in the VM's game a put waits
# in the host's until it dies there too; a take from the body is the VM's too
loot_body() {
	local id=$1 g=$LOOT_GOLD_ID h0 v0
	h0=$(loot_n host "$id" "$g")
	loot_give vm "$g" 7 || return
	v0=$(loot_n vm "$id" "$g")
	loot_run "vm: puts 3 gold in the living $id" vm "loot put $id $g 3" "put 3" && loot_own "vm: puts 3 gold in the living $id" vm "$id" "$g" $((v0 + 3)) || return
	sleep "$LOOT_WAIT"
	expect_eq "host: the living $id's inventory is its own (no more gold)" "$(loot_n host "$id" "$g")" "$h0"

	loot_run "vm: kills its $id (console kill, set-up)" vm "console $id.kill" ran || return
	wait_until 10 _loot_dead vm "$id" || { rig_fault input "vm: kills its $id (console kill, set-up)" "$(loot_lines "$(dc vm "loot near $LOOT_BODY_NEAR")" | grep "^ref=$id " | cut -c1-160)"; return; }
	v0=$(loot_n vm "$id" "$g")
	loot_run "vm: puts 4 gold in the body $id" vm "loot put $id $g 4" "put 4" && loot_own "vm: puts 4 gold in the body $id" vm "$id" "$g" $((v0 + 4)) || return
	if wait_until 5 _loot_dead host "$id"; then
		# Actors' deaths are shared (the NPC work): the host's copy died with the VM's kill, so the put is made at once.
		pass "host: its $id died with the VM's kill (deaths shared)" "kind=body in the host's game too"
		loot_expect "host: its $id, dead with the VM's, holds the VM's 4 gold" host "$id" "$g" $((h0 + 4)) || return
	else
		sleep "$LOOT_WAIT"
		expect_eq "host: its $id, alive, holds no more gold yet (the move waits for its death)" "$(loot_n host "$id" "$g")" "$h0"
		expect_ge "host: a move waits for its holder" "$(field waiting "$(dc host loot)")" 1
		loot_run "host: kills its $id (console kill, set-up)" host "console $id.kill" ran || return
		loot_expect "host: once its $id is dead, the VM's 4 gold is in it" host "$id" "$g" $((h0 + 4)) || return
	fi
	loot_trade host take "$id" "$g" 4
}
_loot_dead() { loot_lines "$(dc "$1" "loot near $LOOT_BODY_NEAR")" | grep -q "^ref=$2 kind=body "; }

# loot_free <host's loot near> <vm's loot near>: the unowned container for the menu's legs, in LOOT_CELL, else in
# LOOT_FREE_CELL (both players put there with coc, and left there); empty and a RIG verdict when neither has one
loot_free() {
	local id list vlist
	if id=$(loot_pick "$1" "$2" container unowned); then echo "$id"; return 0; fi
	echo "     no unowned container near both in $LOOT_CELL: $(loot_kinds "$1" | cut -c1-300)" >&2
	doors_coc "$LOOT_FREE_CELL" >&2 || return 1
	sleep 3
	list=$(dc host "loot near $LOOT_NEAR"); vlist=$(dc vm "loot near $LOOT_NEAR")
	if id=$(loot_pick "$list" "$vlist" container unowned); then echo "$id"; return 0; fi
	rig_unhealed state "both: an unowned container near both players (the menu's legs)" \
		"none within $LOOT_NEAR u of both in $LOOT_CELL or $LOOT_FREE_CELL: $(loot_kinds "$list" | cut -c1-200); set LOOT_FREE_CELL" >&2
	return 1
}

step_loot() {
	local list vlist chest free weapon book creature w g=$LOOT_GOLD_ID
	section "loot: what one player takes from or puts into a container is so in the other's copy"
	doors_coc "$LOOT_CELL" || return
	sleep 3
	list=$(dc host "loot near $LOOT_NEAR")
	[[ $list == near=* ]] || { rig_broken game "host: lists the containers near it" "$(cut -c1-160 <<<"$list")"; return; }
	vlist=$(dc vm "loot near $LOOT_NEAR")
	[[ $vlist == near=* ]] || { rig_broken game "vm: lists the containers near it" "$(cut -c1-160 <<<"$vlist")"; return; }
	echo "     host: $(cut -c1-600 <<<"$list")"
	chest=$(loot_pick "$list" "$vlist" container) ||
		{ rig_unhealed state "both: a container near both players" "none within $LOOT_NEAR u of both in $LOOT_CELL: $(loot_kinds "$list" | cut -c1-200)"; return; }
	loot_give vm "$g" "$LOOT_GOLD" && loot_trade vm put "$chest" "$g" "$LOOT_GOLD" && loot_trade host take "$chest" "$g" 5
	if weapon=$(loot_find weapon); then
		loot_give host "$weapon" 1 && loot_trade host put "$chest" "$weapon" 1 && loot_trade vm take "$chest" "$weapon" 1
	fi

	section "loot: a book stays its player's own"
	if book=$(loot_find book); then
		loot_give vm "$book" 1 && loot_personal vm "$chest" "$book" book
	fi

	section "loot: the session's moves after the VM loads a save and after it joins again"
	loot_again "$chest"

	section "loot: a container the host has open in its menu is not written to until the menu shuts"
	if free=$(loot_free "$list" "$vlist"); then
		loot_menu_hold "$free"

		section "loot: a Take All that empties a container empties the other's copy of what was taken"
		loot_takeall host "$free"
	fi


	section "loot: a body's inventory is shared once dead in both games; a living actor's never"
	if doors_coc "$LOOT_BODY_CELL"; then
		sleep 3
		list=$(dc host "loot near $LOOT_BODY_NEAR"); vlist=$(dc vm "loot near $LOOT_BODY_NEAR")
		if creature=$(loot_pick "$list" "$vlist" npc); then
			loot_body "$creature"
		else
			rig_unhealed state "both: a creature alive near both players" "none within $LOOT_BODY_NEAR u of both in $LOOT_BODY_CELL: $(loot_kinds "$list" | cut -c1-200)"
		fi
	fi

	# How the plugin made its game writes (until the frame hook, off the game's main thread: counted, for the record)
	for w in host vm; do echo "     $w: $(dc $w loot | tr ' ' '\n' | grep -E '^(offmain|short|stray|lost|dropped|expired|eventsDropped|skipped|kept)=' | tr '\n' ' ')"; done

	section "loot: both back in $SMOKE_CELL"
	doors_coc "$SMOKE_CELL"
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init loottest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	LOOT_WAIT=2

	C1='ref=0001A6B0 kind=container loaded=1 items=3 shared=2 vendor=0 storage=0 runtime=0 owned=0 creature=0 essential=0 dist=120 at=(1,2,3)'
	C6='ref=0001A6B4 kind=container loaded=1 items=3 shared=2 vendor=0 storage=0 runtime=0 owned=1 creature=0 essential=0 dist=40 at=(1,2,3)'
	C2='ref=0001A6B1 kind=container loaded=1 items=1 shared=1 vendor=1 storage=0 runtime=0 creature=0 essential=0 dist=90 at=(1,2,3)'
	C3='ref=FF000801 kind=container loaded=1 items=1 shared=1 vendor=0 storage=0 runtime=1 creature=0 essential=0 dist=80 at=(1,2,3)'
	C4='ref=0001A6B2 kind=container loaded=1 items=1 shared=1 vendor=0 storage=1 runtime=0 creature=0 essential=0 dist=70 at=(1,2,3)'
	C5='ref=0001A6B3 kind=container loaded=0 items=0 shared=0 vendor=0 storage=0 runtime=0 creature=0 essential=0 dist=60 at=(1,2,3)'
	N1='ref=0001B001 kind=npc loaded=1 items=2 shared=1 vendor=0 storage=0 runtime=0 creature=0 essential=0 dist=50 at=(1,2,3)'
	N2='ref=0001B002 kind=npc loaded=1 items=0 shared=0 vendor=0 storage=0 runtime=0 creature=1 essential=1 dist=55 at=(1,2,3)'
	N3='ref=0001B003 kind=npc loaded=1 items=0 shared=0 vendor=0 storage=0 runtime=0 creature=1 essential=0 dist=58 at=(1,2,3)'
	B3='ref=0001B003 kind=body loaded=1 items=0 shared=0 vendor=0 storage=0 runtime=0 creature=1 essential=0 dist=58 at=(1,2,3)'
	NEAR="near=8 | $C5 | $C4 | $C3 | $C2 | $N1 | $N2 | $N3 | $C1"
	t "lines: one a holder" "$(loot_lines "$NEAR" | wc -l | tr -d ' ')" 8
	t "pick: the nearest plain container both hold (not a merchant's, storage, runtime or unloaded)" "$(loot_pick "$NEAR" "$NEAR" container)" 0001A6B0
	OWNED="near=9 | $C6 | $C5 | $C4 | $C3 | $C2 | $N1 | $N2 | $N3 | $C1"
	t "pick: an owned container for the moves made by command (no theft)" "$(loot_pick "$OWNED" "$OWNED" container)" 0001A6B4
	t "pick unowned: the nearest container nobody owns, for the menu's legs" "$(loot_pick "$OWNED" "$OWNED" container unowned)" 0001A6B0
	t "pick unowned: none when the VM's copy says owned" "$(loot_pick "$OWNED" "near=1 | ${C1/owned=0/owned=1}" container unowned || echo none)" none
	t "pick unowned: none from a reply without owned=" "$(loot_pick "near=1 | ${C1/owned=0 /}" "near=1 | ${C1/owned=0 /}" container unowned || echo none)" none
	t "kinds: with who owns each" "$(loot_kinds "near=1 | $C6")" "container:0001A6B4:owned=1 "
	t "pick: none when the VM's list lacks it" "$(loot_pick "$NEAR" "near=1 | $C2" container || echo none)" none
	t "pick: none when the VM's is not loaded" "$(loot_pick "$NEAR" "near=1 | ${C1/loaded=1/loaded=0}" container || echo none)" none
	t "pick: a creature alive, not essential (never a person)" "$(loot_pick "$NEAR" "$NEAR" npc)" 0001B003
	t "pick: not one dead in the VM's game" "$(loot_pick "$NEAR" "near=1 | $B3" npc || echo none)" none
	t "kinds" "$(loot_kinds "near=2 | $C1 | $N3")" "container:0001A6B0:owned=0 npc:0001B003:owned= "

	INV='ref=0001A6B0 items=3 | 0000000F:25:misc:shared 0001397E:1:weapon:shared 000ED617:0:book:book 0001B3AE:2:book:book'
	t "count: an item held" "$(loot_count "$INV" 0000000F)" 25
	t "count: an item not held" "$(loot_count "$INV" 00012EB7)" 0
	t "count: an empty holder" "$(loot_count 'ref=0001A6B0 items=0' 0000000F)" 0
	t "count: an error reply is no number" "$(loot_count 'error: usage: loot inv <hex>' 0000000F)" ""
	t "shared: the plain items the session shares" "$(loot_shared "$INV" | tr '\n' ' ')" "0000000F 0001397E "
	t "shared: none" "$(loot_shared 'ref=0001A6B0 items=0')" ""
	t "kept: by scope" "$(loot_kept 'installed=1 kept=living:2,book:3,key:1 offmain=0' book)" 3
	t "kept: none of a scope" "$(loot_kept 'installed=1 kept=- offmain=0' book)" 0

	# Both games faked: each holds its own copy of the holders (an item's count a file), and the loot commands move and
	# read it; SHARE=1 is the mod at work (a move in one game is made in the other's copy), 0 a mod that shares nothing,
	# OWN=0 a game whose own move did not take.
	fake() {  # fake <share> [own]: the fake's state files, fresh
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; OWN=${2:-1}
		for w in host vm; do echo 10 > "$FK/$w.0001A6B0.0000000F"; echo 0 > "$FK/$w.kept"; done
	}
	get() { cat "$FK/$1.$2.$3" 2>/dev/null || echo 0; }
	add() { echo $(( $(get "$1" "$2" "$3") + $4 )) > "$FK/$1.$2.$3"; }
	dc() {
		local w=$1 c=$2 a; read -ra a <<<"$c"
		case "$c" in
			"loot inv "*) echo "ref=${a[2]} items=1 | 0000000F:$(get "$w" "${a[2]}" 0000000F):misc:shared 0001397E:$(get "$w" "${a[2]}" 0001397E):weapon:shared 000ED617:$(get "$w" "${a[2]}" 000ED617):book:book" ;;
			"loot take "*|"loot put "*)
				local n=${a[4]}; [ "${a[1]}" = take ] && n=$((-n))
				[ "$OWN" = 1 ] && add "$w" "${a[2]}" "${a[3]}" "$n"
				if [ "${a[3]}" = 000ED617 ] && [ "${BOOKS:-}" != shared ]; then echo $(( $(cat "$FK/$w.kept") + 1 )) > "$FK/$w.kept"
				elif [ "$SHARE" = 1 ]; then add "$(other "$w")" "${a[2]}" "${a[3]}" "$n"; fi
				[ "${a[1]}" = take ] && echo "took ${a[4]}" || echo "put ${a[4]}" ;;
			loot) echo "installed=1 kept=book:$(cat "$FK/$w.kept") offmain=0" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init lt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; o=$(run 1 loot_trade vm put 0001A6B0 0000000F 7)
	t "trade: a put shared passes" "$(verdicts "$o")" "PASS=2 "
	t "trade: ... and both copies hold 7 more" "$(get host 0001A6B0 0000000F),$(get vm 0001A6B0 0000000F)" 17,17
	o=$(run 2 loot_trade host take 0001A6B0 0000000F 5)
	t "trade: a take shared passes" "$(verdicts "$o")" "PASS=2 "
	t "trade: ... and both copies hold 5 less" "$(get host 0001A6B0 0000000F),$(get vm 0001A6B0 0000000F)" 12,12
	fake 0; o=$(run 3 loot_trade vm put 0001A6B0 0000000F 7)
	t "trade: a put not shared fails on the other" "$(grep -c '^FAIL host: its copy of 0001A6B0 holds the 7 of 0000000F the vm put more' <<<"$o")" 1
	fake 1; echo 20 > "$FK/host.0001A6B0.0000000F"; o=$(run 4 loot_trade host take 0001A6B0 0000000F 5)
	t "trade: copies that differ before compare what the move changed" "$(verdicts "$o")" "PASS=2 "
	t "trade: ... 15 and 5" "$(get host 0001A6B0 0000000F),$(get vm 0001A6B0 0000000F)" 15,5
	fake 1 0; o=$(run 5 loot_trade vm put 0001A6B0 0000000F 7)
	t "trade: a move its own game did not make is RIG, not FAIL" "$(grep -c '^RIG .*vm: puts 7 of 0000000F' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0
	fake 1; o=$(run 6 loot_trade vm take 0001A6B0 0000000F 11)
	t "trade: a take of more than the copy holds is a rig fault, and none is made" "$(grep -c '^RIGFAULT ' <<<"$o"),$(get vm 0001A6B0 0000000F)" 1,10
	fake 1; o=$(run 7 loot_personal vm 0001A6B0 000ED617 book)
	t "personal: a book kept as its own passes" "$(verdicts "$o")" "PASS=4 "
	t "personal: ... and is taken back" "$(get vm 0001A6B0 000ED617),$(get host 0001A6B0 000ED617)" 0,0
	# a mod that shares the book: the host's copy has it, and the VM never counted it kept
	fake 1; BOOKS=shared; o=$(run 8 loot_personal vm 0001A6B0 000ED617 book); BOOKS=
	t "personal: a book shared fails, and so does its count" "$(grep -c '^FAIL host: its copy of 0001A6B0 holds no more of the book' <<<"$o"),$(grep -c '^FAIL vm: kept the move as its own' <<<"$o")" 1,1

	# the step is wired into the harness: in STEPS before final, a session step, in the default run
	t "loot is a step, before final" "$(case " $STEPS " in *" loot "*"final "*) echo yes;; esac)" yes
	t "loot needs a session" "$(case " $SESSION_STEPS " in *" loot "*) echo yes;; esac)" yes
	t "loot runs by default" "$(case " $OPTIN_STEPS " in *" loot "*) echo no;; *) echo yes;; esac)" yes
	t "loot has its line" "$([ -n "${STEP_INFO[loot]:-}" ] && echo yes)" yes
	[ $bad -eq 0 ] && echo "loot.sh selftest: all ok" || echo "loot.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
