#!/bin/bash
# steps/drops.sh: the rig step `drops` (protocol 11, docs/DESIGN.md "Dropped items"), sourced by steps.sh. An item one
# player drops lies in the other's world too, as a copy blocked from activation; the player who takes it has it, and it
# is gone from both worlds; two players taking it at once give it to one; the dropper leaving takes what still lies there
# back to their world; a save from before a drop, loaded, takes the drop back from the session. Not run in a game yet.
#
#   step_drops                                  the step (in the default run, after loot, before final)
#   bash tools/rig/steps/drops.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `drops`, `drops list`, `drops drop <item> <n>` (our player
# drops, through the dev channel's DropObject, which the plugin counts as its player's own drop; any other script's drop
# stays a local item), `drops take <drop id>` (our player takes the copy, as Take on it does), `drops count <item>` and
# `drops ref <ref>` (what became of a reference)):
#   1. Both players are put in DROPS_CELL with coc (console placement, said), near each other.
#   2. Gold, both ways: the host's player is given DROPS_GOLD (console `player.additem`, set-up) and drops 7. Within
#      DROPS_WAIT the VM lists the drop (the host's, gold, 7) with a copy blocked from activation, and so does the host:
#      its own item swapped for the session's copy (swapped= goes up by one, and the reference the drop became, from the
#      `dropped ref=` reply, is out of the host's world: disabled or gone; a dropper holding both the item and the copy
#      fails here). The VM takes it: its gold is 7 more, the drop is in neither list, and the host's gold did not come
#      back. Then the VM drops 5 and the host takes them.
#   3. A weapon (`loot find weapon`: the first plain dagger of the base game, the same in both): the host drops one, the
#      VM's copy is of the same base, and the VM takes it the way a player does: it walks up to its copy (walkto, real
#      movement), looks down with the pad's right stick until its crosshair is on the copy (`aimed=1` in `drops list`),
#      and presses a (Take). A crosshair never on the copy is RIG (the input), and the dev take stands in.
#   Every drop: the dropper reports it (reports= up by one: the reference the item became was found; on SE 1.7.104 the
#   engine's event names none, so the plugin reads its player's drop call or searches near it, and `named=` says which
#   way; a drop never reported is a FAIL that names named=, judged=, hooked= and calls=).
#   3b. Through the real inventory menu, both ways: the dropper is given one of the weapon, its inventory menu is shown
#      (`drops menu open`, the UI's own show message; said), the pad moves the selection onto the weapon (right, then
#      down; `drops menu <item>` reads selected=), x (DROPS_MENU_DROP: the menu's Drop) drops it, b shuts the menu. The
#      other game lists it with a copy, the dropper swapped its own, and the drop was judged its player's own (a drop
#      menu open), not a script's; the other takes it. A menu, selection or Drop the pad could not work is RIG.
#   4. Both take at once: the host drops 3 gold, and both players take it at the same moment. One of them has 3 more, the
#      other none (both together: 3), and neither still waits on its claim.
#   5. The dropper leaves: the VM drops 4 gold, the host lists it; the VM leaves the session. The drop goes from the
#      host's list, and the VM's game gives the item back to its world (returned= goes up; its copy lies where it lay, in
#      its world, its own again). The VM joins again: for DROPS_WAIT the drop lies in neither list again (a record that
#      still said the item left its world would have the host lay it down again: two of it), and the VM picks its item
#      up (`use`): its gold is what it was before the drop.
#   6. A dropper's load: the VM saves ($SAVE_NAME), drops 6 gold, the host lists it; the VM loads its save (from before
#      the drop). The drop goes from the host's list: the VM's world has the item again.
#   7. A taker's load: the host drops 8 gold, the VM saves, takes it, and loads its save (from before the take). The drop
#      lies in the host's list again, the VM's gold is the save's, and the VM has a copy of it again; the host takes it
#      back (none left lying).
#   8. Both are put back in the harness's cell (coc).

DROPS_CELL=${DROPS_CELL:-RiverwoodSleepingGiantInn}  # an interior near the coc marker: both players stand close
DROPS_GOLD=${DROPS_GOLD:-40}                         # gold each player is given to drop
DROPS_WAIT=${DROPS_WAIT:-20}                         # s: an item comes to rest (5 s at most), is offered, swapped, placed
DROPS_AIM_STEPS=${DROPS_AIM_STEPS:-12}               # looks down (the right stick, DROPS_AIM_MS each) before a RIG
DROPS_AIM_MS=${DROPS_AIM_MS:-120}
DROPS_MENU_STEPS=${DROPS_MENU_STEPS:-40}             # pad moves in the inventory menu before a RIG
DROPS_MENU_LIST=${DROPS_MENU_LIST:-12}               # downs in one category's list before the next category
DROPS_MENU_DROP=${DROPS_MENU_DROP:-x}                # the pad button of the inventory menu's Drop
DROPS_GOLD_ID=0000000F                               # Skyrim.esm's gold

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# drops_lines <drops list reply>: its drop lines, one a line ("lying=N | line | line ...")
drops_lines() { sed 's/ | /\n/g' <<<"$1" | tail -n +2; }
# drops_ids <drops list reply>: the drop IDs it lists, one a line
drops_ids() { local line; while IFS= read -r line; do field id "$line"; done < <(drops_lines "$1"); }
# drops_line <drops list reply> <id>: that drop's line (empty: not listed)
drops_line() { drops_lines "$1" | grep -E "^id=$2 " | head -1; }
# drops_new <drops list reply> <owner> <base> <count> [known ids...]: the ID of a drop of that owner, base and count not
# among the known ones; 1 when there is none
drops_new() {
	local line id
	while IFS= read -r line; do
		[ "$(field owner "$line")" = "$2" ] && [ "$(field base "$line")" = "$3" ] && [ "$(field count "$line")" = "$4" ] || continue
		id=$(field id "$line")
		case " ${*:5} " in *" $id "*) continue ;; esac
		echo "$id"; return 0
	done < <(drops_lines "$1")
	return 1
}
# drops_copied <line>: a copy of the drop is in that world, blocked from activation
drops_copied() { local c; c=$(field copy "$1"); [ -n "$c" ] && [ "$c" != none ] && [ "$(field blocked "$1")" = 1 ]; }

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

drops_n() { field count "$(dc "$1" "drops count $2")"; }
drops_us() { field us "$(dc "$1" drops)"; }
drops_has() { [ "$(drops_n "$1" "$2")" = "$3" ]; }
drops_listed() { [ -n "$(drops_line "$(dc "$1" 'drops list')" "$2")" ]; }
drops_unlisted() { ! drops_listed "$@"; }
drops_returned_ge() { [ "$(field returned "$(dc "$1" drops)")" -ge "$2" ] 2>/dev/null; }
drops_copy_in() { drops_copied "$(drops_line "$(dc "$1" 'drops list')" "$2")"; }
drops_swapped_ge() { [ "$(field swapped "$(dc "$1" drops)")" -ge "$2" ] 2>/dev/null; }
# drops_ref_out <who> <ref>: that reference is out of that game's world (disabled, deleted or gone)
drops_ref_out() { local r; r=$(dc "$1" "drops ref $2"); [[ $r == ref=* ]] && [ "$(field inworld "$r")" = 0 ]; }
drops_ref_in() { local r; r=$(dc "$1" "drops ref $2"); [[ $r == ref=* ]] && [ "$(field inworld "$r")" = 1 ]; }
drops_aimed() { [ "$(field aimed "$(drops_line "$(dc "$1" 'drops list')" "$2")")" = 1 ]; }
# drops_stays_off <who> <id> <s>: the drop lies in that game's list at no look over that time
drops_stays_off() { local i; for ((i = 0; i < $3; i++)); do drops_listed "$1" "$2" && return 1; sleep 1; done; drops_unlisted "$1" "$2"; }
# drops_seen <who> <owner> <base> <count> <known...>: prints the new drop's ID once that game lists it
drops_seen() { drops_new "$(dc "$1" 'drops list')" "$2" "$3" "$4" "${@:5}"; }

# drops_give <who> <item> <n>: that game's player is given n of the item (console, set-up)
drops_give() {
	local before
	before=$(drops_n "$1" "$2")
	[ "$(dc "$1" "console player.additem $2 $3")" = ran ] &&
		wait_until 5 drops_has "$1" "$2" $((${before:-0} + $3)) && return 0
	rig_fault input "$1: is given $3 of $2 (console player.additem, set-up)" "its player holds $(drops_n "$1" "$2")"; return 1
}

# drops_drop <who> <item> <n>: that game's player drops n of the item (the dev channel's DropObject); DROP_ID is the drop's
# ID once the other game lists it with a copy, and the dropper's own game too: its own item swapped for the session's copy
# (swapped= up by one, and the reference the drop became out of its world). 1 and a FAIL when not listed.
DROP_ID=""
drops_drop() {
	local who=$1 item=$2 n=$3 known before r sw0 rp0
	DROP_ID=""
	known=$(drops_ids "$(dc "$(other "$who")" 'drops list')" | tr '\n' ' ')
	before=$(drops_n "$who" "$item")
	sw0=$(field swapped "$(dc "$who" drops)"); rp0=$(field reports "$(dc "$who" drops)")
	r=$(dc "$who" "drops drop $item $n")
	[[ $r == dropped* ]] || { rig_broken input "$who: drops $n of $item" "'drops drop' replied '$r'"; return 1; }
	is_num "$before" && expect_eq "$who: has $n of $item less once it dropped them" "$(drops_n "$who" "$item")" $((before - n))
	drops_shared "$who" "$item" "$n" "$known" "$sw0" "$rp0" "$(field ref "$r")" "$r"
}

drops_reports_ge() { [ "$(field reports "$(dc "$1" drops)")" -ge "$2" ] 2>/dev/null; }
# drops_judged_n <who> <why>: how many of that game's drops were judged so (judged=noref:3,scripted:1 ...): 0 when none
drops_judged_n() { local j; j=$(field judged "$(dc "$1" drops)" | tr ',' '\n' | grep -E "^$2:" | cut -d: -f2); echo "${j:-0}"; }

# drops_shared <who> <item> <n> <known ids> <swapped before> <reports before> <ref|-> <drop reply>: after that game's
# player dropped n of the item, it reports the drop (the reference the item became was found: the engine's event names
# none on SE 1.7.104, so `named=` says which way did: event, late, call or search; a drop judged noref is never reported),
# the other game lists it with a copy, the dropper swaps its own item for the session's copy, and the reference the drop
# call named (- when none did: a menu drop) is out of the dropper's world. DROP_ID as drops_drop.
drops_shared() {
	local who=$1 item=$2 n=$3 known=$4 sw0=$5 rp0=$6 ref=$7 r=$8 watcher id i st
	DROP_ID=""
	watcher=$(other "$who")
	st=""
	is_num "$rp0" && wait_until 10 drops_reports_ge "$who" $((rp0 + 1)) && st=$(dc "$who" drops)
	if [ -n "$st" ]; then pass "$who: reports its drop of $n of $item (the reference it became found)" "named=$(field named "$st") judged=$(field judged "$st")"
	else
		st=$(dc "$who" drops)
		fail "$who: reports its drop of $n of $item (the reference it became found)" "reports was '$rp0': reports=$(field reports "$st") named=$(field named "$st") judged=$(field judged "$st") hooked=$(field hooked "$st") calls=$(field calls "$st") callsForgotten=$(field callsForgotten "$st")"
		return 1
	fi
	id=""
	for ((i = 0; i < DROPS_WAIT; i++)); do
		id=$(drops_seen "$watcher" "$(drops_us "$who")" "$item" "$n" $known) && break
		sleep 1
	done
	[ -n "$id" ] || { fail "$watcher: lists the $n of $item $who dropped" "$(dc "$watcher" 'drops list' | cut -c1-300) | $who: $(dc "$who" drops | cut -c1-300)"; return 1; }
	pass "$watcher: lists the $n of $item $who dropped" "drop $id"
	if wait_until "$DROPS_WAIT" drops_copy_in "$watcher" "$id"; then pass "$watcher: has a copy of drop $id where it lies, blocked from activation" "$(drops_line "$(dc "$watcher" 'drops list')" "$id" | cut -c1-200)"
	else fail "$watcher: has a copy of drop $id where it lies, blocked from activation" "$(drops_line "$(dc "$watcher" 'drops list')" "$id" | cut -c1-200)"; fi
	if wait_until "$DROPS_WAIT" drops_copy_in "$who" "$id"; then pass "$who: its own item swapped for the session's copy of drop $id" "$(drops_line "$(dc "$who" 'drops list')" "$id" | cut -c1-200)"
	else fail "$who: its own item swapped for the session's copy of drop $id" "$(dc "$who" drops | cut -c1-300)"; fi
	# The swap itself: counted once, and the item the drop became is out of the dropper's world (both there would be two)
	if is_num "$sw0" && wait_until 5 drops_swapped_ge "$who" $((sw0 + 1)); then pass "$who: counts the swap of its own item for drop $id" "swapped=$(field swapped "$(dc "$who" drops)")"
	else fail "$who: counts the swap of its own item for drop $id" "swapped was '$sw0': $(dc "$who" drops | cut -c1-300)"; fi
	if [[ $ref =~ ^[0-9A-F]{8}$ ]]; then
		if wait_until 5 drops_ref_out "$who" "$ref"; then pass "$who: the item it dropped ($ref) left its world for the copy" "$(dc "$who" "drops ref $ref")"
		else fail "$who: the item it dropped ($ref) left its world for the copy" "$(dc "$who" "drops ref $ref")"; fi
	elif [ "$ref" != - ]; then fail "$who: names the reference its drop became" "'$r'"; fi
	DROP_ID=$id
}

# drops_menu <who> <item>: that game's player drops one of the item through the real inventory menu, as a player does:
# the menu is shown (`drops menu open`: the UI's own show message, as the game's menu keys send; said), the pad moves its
# selection to the item (right into the list, then down along it; past DROPS_MENU_LIST downs: left, down, right, the next
# category), x (Drop) drops it, and b closes the menu. Then as drops_shared (with no reference named by a call reply), and
# the drop is judged its player's own (a drop menu open), never a script's. A menu that never shows, a selection that
# never reaches the item, or an x that drops nothing is RIG (the input), never FAIL.
drops_menu_sel() { [ "$(field selected "$(dc "$1" "drops menu $2")")" = "$2" ]; }
drops_menu_open() { [ "$(field inventory "$(dc "$1" 'drops menu')")" = 1 ]; }
drops_menu_shut() { [ "$(field inventory "$(dc "$1" 'drops menu')")" = 0 ]; }
drops_menu_close() {
	local k
	for k in 1 2 3; do drops_menu_shut "$1" && return 0; dc "$1" 'pad button b' >/dev/null; sleep 0.8; done
	drops_menu_shut "$1" && return 0
	dc "$1" 'drops menu close' >/dev/null; sleep 1
	if drops_menu_shut "$1"; then healed "$1: its inventory menu shut" "'drops menu close' (b did not shut it)"; return 0; fi
	rig_fault input "$1: shuts its inventory menu" "$(dc "$1" 'drops menu' | cut -c1-200)"; return 1
}
drops_menu() {
	local who=$1 item=$2 known before now sw0 rp0 sc0 r m i k downs=0
	DROP_ID=""; DROP_N=0
	known=$(drops_ids "$(dc "$(other "$who")" 'drops list')" | tr '\n' ' ')
	before=$(drops_n "$who" "$item")
	sw0=$(field swapped "$(dc "$who" drops)"); rp0=$(field reports "$(dc "$who" drops)"); sc0=$(drops_judged_n "$who" scripted)
	is_num "$before" && [ "$before" -ge 1 ] || { rig_fault state "$who: holds a $item to drop through its inventory menu" "it holds '$before'"; return 1; }
	r=$(dc "$who" 'drops menu open')
	if [ "$r" != opening ] || ! wait_until 5 drops_menu_open "$who"; then
		rig_fault input "$who: shows its inventory menu" "'drops menu open' replied '$r': $(dc "$who" 'drops menu')"; drops_menu_close "$who"; return 1
	fi
	sleep 1
	m=$(dc "$who" "drops menu $item")
	[ "$(field has "$m")" = 1 ] || { rig_fault state "$who: its inventory menu lists $item" "$m"; drops_menu_close "$who"; return 1; }
	dc "$who" 'pad button right' >/dev/null; sleep 0.5
	for ((i = 0; i < DROPS_MENU_STEPS; i++)); do
		drops_menu_sel "$who" "$item" && break
		if [ "$downs" -ge "$DROPS_MENU_LIST" ]; then
			for k in left down right; do dc "$who" "pad button $k" >/dev/null; sleep 0.5; done
			downs=0
		else
			dc "$who" 'pad button down' >/dev/null; downs=$((downs + 1)); sleep 0.4
		fi
	done
	if ! drops_menu_sel "$who" "$item"; then
		rig_fault input "$who: its inventory menu's selection on $item ($i moves of the pad)" "$(dc "$who" "drops menu $item")"
		drops_menu_close "$who"; return 1
	fi
	pass "$who: its inventory menu's selection is on $item" "after $i move(s) of the pad"
	dc "$who" "pad button $DROPS_MENU_DROP" >/dev/null
	# More than one held: the menu asks how many (its quantity box); a takes what it offers
	if ! wait_until 3 drops_less "$who" "$item" "$before"; then
		dc "$who" 'pad button a' >/dev/null
		wait_until 5 drops_less "$who" "$item" "$before"
	fi
	now=$(drops_n "$who" "$item")
	if ! is_num "$now" || [ "$now" -ge "$before" ]; then
		rig_fault input "$who: drops $item with the menu's Drop (pad $DROPS_MENU_DROP)" "it holds $now, had $before: $(dc "$who" "drops menu $item")"
		drops_menu_close "$who"; return 1
	fi
	DROP_N=$((before - now))
	pass "$who: dropped $DROP_N $item with its inventory menu's Drop (pad $DROPS_MENU_DROP)" "$now left"
	drops_menu_close "$who"
	drops_shared "$who" "$item" "$DROP_N" "$known" "$sw0" "$rp0" - "a menu drop" || return 1
	expect_eq "$who: its menu drop is its player's own, never judged a script's" "$(drops_judged_n "$who" scripted)" "$sc0"
}
drops_less() { local c; c=$(drops_n "$1" "$2"); is_num "$c" && [ "$c" -lt "$3" ]; }

# drops_aim <who> <id>: that game's player walks up to its copy of the drop (real movement), looks down with the right
# stick until its crosshair is on it, and presses a (Take). 1 and a RIG when the crosshair never was on it. DROPS_DOWN:
# the steps it looked down (drops_level looks up as many).
DROPS_DOWN=0
drops_level() { local i; for ((i = 0; i < $2; i++)); do dc "$1" "pad stick r 0 0.5 $DROPS_AIM_MS" >/dev/null; sleep 0.6; done; }
drops_aim() {
	local who=$1 id=$2 copy r st k i
	DROPS_DOWN=0
	copy=$(field copy "$(drops_line "$(dc "$who" 'drops list')" "$id")")
	[[ $copy =~ ^[0-9A-F]{8}$ ]] || { rig_fault state "$who: has a copy of drop $id to walk up to" "copy='$copy'"; return 1; }
	r=$(dc "$who" "walkto $copy 0.3 70 20")
	[[ $r == walking* ]] || { rig_fault input "$who: walks up to its copy of drop $id" "walkto replied '$r'"; return 1; }
	for ((k = 0; k < 50; k++)); do st=$(dc "$who" walkto); [[ $st == "not walking"* ]] && break; sleep 0.5; done
	still "$who"; sleep 1
	for ((i = 0; i < DROPS_AIM_STEPS; i++)); do
		drops_aimed "$who" "$id" && break
		dc "$who" "pad stick r 0 -0.5 $DROPS_AIM_MS" >/dev/null; DROPS_DOWN=$((DROPS_DOWN + 1)); sleep 0.6
	done
	if ! drops_aimed "$who" "$id"; then
		rig_fault input "$who: its crosshair on its copy of drop $id (walked up, looked down $DROPS_DOWN steps)" "$(drops_line "$(dc "$who" 'drops list')" "$id" | cut -c1-220)"
		drops_level "$who" "$DROPS_DOWN"; DROPS_DOWN=0
		return 1
	fi
	pass "$who: its crosshair is on its copy of drop $id" "after $DROPS_DOWN look(s) down"
	r=$(dc "$who" 'pad button a')
	[[ $r == button* ]] || { rig_fault input "$who: presses a (Take) on its copy of drop $id" "the pad replied '$r'"; drops_level "$who" "$DROPS_DOWN"; DROPS_DOWN=0; return 1; }
	return 0
}

# drops_take <who> <id> <item> <n> [aim]: that game's player takes the drop (the dev take; aim: with the copy in its
# crosshair and the pad's a, drops_aim); it has n more of the item, the drop is in neither list, and the other's count
# did not change
drops_take() {
	local who=$1 id=$2 item=$3 n=$4 how=${5:-dev} o before obefore r w aimed=0
	o=$(other "$who")
	before=$(drops_n "$who" "$item"); obefore=$(drops_n "$o" "$item")
	if [ "$how" = aim ] && drops_aim "$who" "$id"; then aimed=1; fi
	if [ "$aimed" = 0 ]; then
		r=$(dc "$who" "drops take $id")
		[ "$r" = asked ] || { rig_broken input "$who: takes drop $id" "'drops take' replied '$r'"; return 1; }
	fi
	if wait_until "$DROPS_WAIT" drops_has "$who" "$item" $((before + n)); then pass "$who: has the $n of $item it took (drop $id$([ "$aimed" = 1 ] && echo ', Take with the pad'))" "$((before + n))"
	else
		fail "$who: has the $n of $item it took (drop $id$([ "$aimed" = 1 ] && echo ', Take with the pad'))" "it has $(drops_n "$who" "$item"), had $before: $(dc "$who" drops | cut -c1-300)"
		[ "$aimed" = 1 ] && drops_level "$who" "$DROPS_DOWN"
		return 1
	fi
	[ "$aimed" = 1 ] && drops_level "$who" "$DROPS_DOWN"
	for w in "$who" "$o"; do
		if wait_until "$DROPS_WAIT" drops_unlisted "$w" "$id"; then pass "$w: drop $id lies there no more" ""
		else fail "$w: drop $id lies there no more" "$(drops_line "$(dc "$w" 'drops list')" "$id" | cut -c1-200)"; fi
	done
	expect_eq "$o: its count of $item did not change with the other's take" "$(drops_n "$o" "$item")" "$obefore"
}

# drops_both <id> <item> <n>: both players take the drop at the same moment: one has it, the other not
drops_both() {
	local id=$1 item=$2 n=$3 h0 v0 h1 v1 w
	h0=$(drops_n host "$item"); v0=$(drops_n vm "$item")
	dc host "drops take $id" >/dev/null & dc vm "drops take $id" >/dev/null; wait
	sleep 5
	wait_until "$DROPS_WAIT" drops_unlisted host "$id"
	h1=$(drops_n host "$item"); v1=$(drops_n vm "$item")
	if is_num "$h0" && is_num "$v0" && is_num "$h1" && is_num "$v1"; then
		expect_eq "both: took drop $id at once, and the $n of $item went to one of them" "$((h1 - h0 + v1 - v0))" "$n"
		case "$((h1 - h0)),$((v1 - v0))" in
			"$n,0"|"0,$n") pass "both: one of the two has it, the other none" "host +$((h1 - h0)), vm +$((v1 - v0))" ;;
			*) fail "both: one of the two has it, the other none" "host +$((h1 - h0)), vm +$((v1 - v0))" ;;
		esac
	else fail "both: read their counts of $item" "host $h0 -> $h1, vm $v0 -> $v1"; fi
	for w in host vm; do expect_eq "$w: no claim left waiting" "$(field claims "$(dc $w drops)")" 0; done
}

# drops_leave <item>: the VM drops, the host lists it, the VM leaves: it goes from the host's list, and back to the VM's
# world (its copy there its own again); the VM joins again, the drop lies in neither list again, and the VM picks its
# item up: its count is the one from before the drop (one of it, in one world)
drops_leave() {
	local item=$1 id r ret0 g0 h0 copy
	g0=$(drops_n vm "$item"); h0=$(drops_n host "$item")
	drops_drop vm "$item" 4 || return
	id=$DROP_ID
	copy=$(field copy "$(drops_line "$(dc vm 'drops list')" "$id")")
	ret0=$(field returned "$(dc vm drops)")
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	if wait_until "$DROPS_WAIT" drops_unlisted host "$id"; then pass "host: the VM's drop $id goes with the VM" ""
	else fail "host: the VM's drop $id goes with the VM" "$(drops_line "$(dc host 'drops list')" "$id" | cut -c1-200)"; fi
	if is_num "$ret0"; then
		wait_until "$DROPS_WAIT" drops_returned_ge vm $((ret0 + 1))
		expect_ge "vm: gave its item back to its world as it left" "$(field returned "$(dc vm drops)")" $((ret0 + 1))
	else fail "vm: counts what it gave back" "$(dc vm drops | cut -c1-200)"; fi
	if [[ $copy =~ ^[0-9A-F]{8}$ ]] && wait_until 5 drops_ref_in vm "$copy"; then pass "vm: its item lies where it lay, in its world" "$(dc vm "drops ref $copy")"
	else fail "vm: its item lies where it lay, in its world" "copy '$copy': $(dc vm "drops ref ${copy:-none}")"; copy=""; fi
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	# The VM's record, sent with its welcome, says the item is back in its world: the session must not lay it down again
	if drops_stays_off host "$id" "$DROPS_WAIT"; then pass "host: the VM's drop $id does not lie there again once the VM is back (${DROPS_WAIT} s)" ""
	else fail "host: the VM's drop $id does not lie there again once the VM is back (${DROPS_WAIT} s)" "$(drops_line "$(dc host 'drops list')" "$id" | cut -c1-200)"; fi
	expect_eq "vm: lists its returned drop $id no more" "$(drops_line "$(dc vm 'drops list')" "$id")" ""
	[ -n "$copy" ] || return
	r=$(dc vm "use $copy"); [ "$r" = used ] || { rig_fault input "vm: picks up its returned item ($copy)" "'use' replied '$r'"; return; }
	if is_num "$g0" && wait_until 10 drops_has vm "$item" "$g0"; then pass "vm: has its $item back as before the drop, once" "$g0"
	else fail "vm: has its $item back as before the drop, once" "it has $(drops_n vm "$item"), had $g0"; fi
	expect_eq "host: its count of $item did not change" "$(drops_n host "$item")" "$h0"
}

# drops_taker_load <item>: the host drops, the VM saves, takes it and loads its save (from before the take): the drop lies
# there again (the host lists it, the VM has a copy again), the VM's count is the save's; the host takes it back
drops_taker_load() {
	local item=$1 id r v0
	drops_drop host "$item" 8 || return
	id=$DROP_ID
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "vm: saves" "'console save' replied '$r'"; return; }
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return; }
	v0=$(drops_n vm "$item")
	drops_take vm "$id" "$item" 8 || return
	r=$(dc vm "console load $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "vm: loads its save" "'console load' replied '$r'"; return; }
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; }
	wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return; }
	if wait_until "$DROPS_WAIT" drops_listed host "$id"; then pass "host: drop $id lies there again once the VM loads a save from before its take" ""
	else fail "host: drop $id lies there again once the VM loads a save from before its take" "$(dc host 'drops list' | cut -c1-300)"; return; fi
	expect_eq "vm: its count of $item is its save's, from before the take" "$(drops_n vm "$item")" "$v0"
	if wait_until "$DROPS_WAIT" drops_copy_in vm "$id"; then pass "vm: has a copy of drop $id again" "$(drops_line "$(dc vm 'drops list')" "$id" | cut -c1-200)"
	else fail "vm: has a copy of drop $id again" "$(dc vm 'drops list' | cut -c1-300)"; fi
	drops_take host "$id" "$item" 8
}

# drops_load <item>: the VM saves, drops, the host lists it; the VM loads the save from before the drop: the drop goes
# from the host's list
drops_load() {
	local item=$1 id r
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "vm: saves" "'console save' replied '$r'"; return; }
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return; }
	drops_drop vm "$item" 6 || return
	id=$DROP_ID
	r=$(dc vm "console load $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "vm: loads its save" "'console load' replied '$r'"; return; }
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; }
	wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return; }
	if wait_until "$DROPS_WAIT" drops_unlisted host "$id"; then pass "host: the VM's drop $id goes once the VM loads a save from before it" ""
	else fail "host: the VM's drop $id goes once the VM loads a save from before it" "$(drops_line "$(dc host 'drops list')" "$id" | cut -c1-200)"; fi
	wait_until "$DROPS_WAIT" drops_unlisted vm "$id"
	expect_eq "vm: lists its own drop no more after the load" "$(drops_line "$(dc vm 'drops list')" "$id")" ""
}

step_drops() {
	local g=$DROPS_GOLD_ID id weapon w
	section "drops: an item one player drops lies in the other's world, and the one who takes it has it"
	doors_coc "$DROPS_CELL" || return
	sleep 3
	drops_give host "$g" "$DROPS_GOLD" && drops_give vm "$g" "$DROPS_GOLD" || return
	drops_drop host "$g" 7 && drops_take vm "$DROP_ID" "$g" 7
	drops_drop vm "$g" 5 && drops_take host "$DROP_ID" "$g" 5
	weapon=$(field found "$(dc host 'loot find weapon')")
	if [ -n "$weapon" ] && [ "$weapon" != none ] && [ "$weapon" = "$(field found "$(dc vm 'loot find weapon')")" ]; then
		if drops_give host "$weapon" 1 && drops_drop host "$weapon" 1; then
			id=$DROP_ID
			expect_eq "vm: its copy of drop $id is the same weapon" "$(field base "$(drops_line "$(dc vm 'drops list')" "$id")")" "$weapon"
			drops_take vm "$id" "$weapon" 1 aim
		fi
	else rig_unhealed state "both: find the same weapon to drop" "host found '$weapon'"; fi

	section "drops: an item dropped through the inventory menu (the pad's Drop) lies in the other's world, both ways"
	if [ -n "$weapon" ] && [ "$weapon" != none ] && [ "$weapon" = "$(field found "$(dc vm 'loot find weapon')")" ]; then
		for w in host vm; do
			# one held, so the Drop needs no quantity box (one taken in leg 3 is enough)
			[ "$(drops_n "$w" "$weapon")" = 0 ] && { drops_give "$w" "$weapon" 1 || continue; }
			drops_menu "$w" "$weapon" && drops_take "$(other "$w")" "$DROP_ID" "$weapon" "$DROP_N"
		done
	else rig_unhealed state "both: find the same weapon to drop through the menu" "host found '$weapon'"; fi

	section "drops: two players taking one item at once: one has it"
	drops_drop host "$g" 3 && drops_both "$DROP_ID" "$g" 3

	section "drops: a player who leaves takes what they dropped still lying there back to their world"
	drops_leave "$g"

	section "drops: a save from before a drop, loaded, takes the drop back from the session"
	drops_load "$g"

	section "drops: a save from before a take, loaded, gives the drop back to the session"
	drops_taker_load "$g"

	# How the plugin made its game writes (until the frame hook, off the game's main thread: counted, for the record)
	# and which way each drop's reference was found (named=event|late|call|search|none; hooked=1: the drop calls read)
	for w in host vm; do echo "     $w: $(dc $w drops | tr ' ' '\n' | grep -E '^(offmain|lost|unplaceable|unsure|gaveBack|forgotten|refused|eventsDropped|judged|hooked|named|callsForgotten|namingFull)=' | tr '\n' ' ')"; done

	section "drops: both back in $SMOKE_CELL"
	doors_coc "$SMOKE_CELL"
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init dropstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	DROPS_WAIT=2

	L1='id=3 owner=1 base=0000000F count=7 health=-1.00 ench=00000000 charge=-1 poison=00000000 poisonCount=0 soul=0 named=0 copy=FF000801 blocked=1 dist=120'
	L2='id=4 owner=2 base=0001397E count=1 health=1.60 ench=00000000 charge=-1 poison=00000000 poisonCount=0 soul=0 named=1 copy=none blocked=0 dist=-1'
	L3='id=5 owner=1 base=0000000F count=7 health=-1.00 ench=00000000 charge=-1 poison=00000000 poisonCount=0 soul=0 named=0 copy=FF000802 blocked=0 dist=80'
	LIST="lying=3 | $L1 | $L2 | $L3"
	t "lines: one a drop" "$(drops_lines "$LIST" | wc -l | tr -d ' ')" 3
	t "ids" "$(drops_ids "$LIST" | tr '\n' ' ')" "3 4 5 "
	t "line: by id" "$(drops_line "$LIST" 4)" "$L2"
	t "line: none for an id not listed (never a prefix)" "$(drops_line "$LIST" 30)" ""
	t "new: the first of that owner, base and count" "$(drops_new "$LIST" 1 0000000F 7)" 3
	t "new: not one already known" "$(drops_new "$LIST" 1 0000000F 7 3)" 5
	t "new: none of another count" "$(drops_new "$LIST" 1 0000000F 8 || echo none)" none
	t "new: none of another owner" "$(drops_new "$LIST" 2 0000000F 7 || echo none)" none
	t "copied: placed and blocked" "$(drops_copied "$L1" && echo yes)" yes
	t "copied: no copy" "$(drops_copied "$L2" || echo no)" no
	t "copied: a copy not blocked is none of ours" "$(drops_copied "$L3" || echo no)" no
	t "list: empty" "$(drops_lines 'lying=0' | wc -l | tr -d ' ')" 0

	# Both games faked: a session's list of drops (a file of lines) and each player's counts; SHARE=1 is the mod at work
	# (a drop is listed in both with a copy, a take moves the count), 0 a mod that lists nothing for the other game; SWAP=1
	# its dropper's own item swapped out of its world (swapped= up, the ref out), 0 a dropper holding both. A player's
	# crosshair is on a copy once it looked down AIMAT steps (the right stick); the pad's a takes that copy.
	# NAMED=1 the dropper's game found the reference its drop became (reports= up), 0 it judged it noref (the rig's first
	# run: never reported, never listed). The inventory menu: shown by `drops menu open`, right puts the focus in its list,
	# down moves the selection, the weapon is the entry at MENUAT; x (MENUX=1) drops the selected weapon, or with more
	# than one held shows the quantity box, which a takes (one); b shuts the box, leaves the list, then shuts the menu.
	fake() {
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; SWAP=${2:-1}; AIMAT=${3:-3}; NAMED=${4:-1}; MENUAT=${5:-3}; MENUX=${6:-1}
		: > "$FK/list"; echo 10 > "$FK/next"
		for w in host vm; do echo 50 > "$FK/$w.0000000F"; echo 0 > "$FK/$w.swapped"; echo 0 > "$FK/$w.pitch"; done
	}
	WEAPON=0001397E
	# fdrop <who> <item> <n>: the fake's drop (the dev channel's, or the menu's)
	fdrop() {
		local id
		put "$1" "$2" $(( $(get "$1" "$2") - $3 ))
		if [ "$NAMED" != 1 ]; then put "$1" noref $(( $(get "$1" noref) + 1 )); return; fi
		put "$1" reports $(( $(get "$1" reports) + 1 ))
		id=$(cat "$FK/next"); echo $((id + 1)) > "$FK/next"
		echo "id=$id owner=$(us "$1") base=$2 count=$3 copy=FF0000$id blocked=1 dist=50 who=$1" >> "$FK/list"
		if [ "$SWAP" = 1 ]; then put "$1" swapped $(( $(get "$1" swapped) + 1 )); echo out > "$FK/ref.FF0009$id"; else echo in > "$FK/ref.FF0009$id"; fi
	}
	selected() { if [ "$(get "$1" menu)" = 1 ] && [ "$(get "$1" inlist)" = 1 ]; then [ "$(get "$1" sel)" = "$MENUAT" ] && echo "$WEAPON" || echo "000A000$(get "$1" sel)"; else echo none; fi; }
	get() { cat "$FK/$1.$2" 2>/dev/null || echo 0; }
	put() { echo "$3" > "$FK/$1.$2"; }
	us() { [ "$1" = host ] && echo 1 || echo 2; }
	aimed() { [ "$(get "$1" pitch)" -ge "$AIMAT" ] && echo 1 || echo 0; }
	take() {  # take <who> <id>: the fake's grant
		local line n; line=$(grep -E "^id=$2 " "$FK/list"); [ -n "$line" ] || return 1
		[ "$SHARE" = 1 ] || [ "$(field who "$line")" = "$1" ] || return 1
		grep -vE "^id=$2 " "$FK/list" > "$FK/list.new"; mv -f "$FK/list.new" "$FK/list"
		n=$(field count "$line"); put "$1" "$(field base "$line")" $(( $(get "$1" "$(field base "$line")") + n ))
	}
	dc() {
		local w=$1 c=$2 a id line; read -ra a <<<"$c"
		case "$c" in
			"drops count "*) echo "count=$(get "$w" "${a[2]}")" ;;
			"drops drop "*) id=$(cat "$FK/next"); fdrop "$w" "${a[2]}" "${a[3]}"; echo "dropped ref=FF0009$id" ;;
			"drops menu open") put "$w" menu 1; put "$w" inlist 0; put "$w" sel 0; echo opening ;;
			"drops menu close") put "$w" menu 0; echo closing ;;
			"drops menu"*)
				echo "inventory=$(get "$w" menu) selected=$(selected "$w") selCount=1 entries=8 has=$([ "$(get "$w" menu)" = 1 ] && [ "${a[2]:-}" = "$WEAPON" ] && echo 1 || echo 0)" ;;
			"drops ref "*)
				case "$(cat "$FK/ref.${a[2]}" 2>/dev/null)" in
					out) echo "ref=${a[2]} found=1 inworld=0 disabled=1 deleted=0 buried=1" ;;
					in) echo "ref=${a[2]} found=1 inworld=1 disabled=0 deleted=0 buried=0" ;;
					*) echo "ref=${a[2]} found=0 inworld=0" ;;
				esac ;;
			"drops list")
				echo -n "lying=$(wc -l < "$FK/list" | tr -d ' ')"
				while IFS= read -r line; do
					[ "$SHARE" = 1 ] || [ "$(field who "$line")" = "$w" ] || continue
					echo -n " | $line aimed=$(aimed "$w")"
				done < "$FK/list"; echo ;;
			"drops take "*) take "$w" "${a[2]}" && echo asked || echo "error: no copy of that drop here" ;;
			drops)
				echo "installed=1 us=$(us "$w") claims=0 returned=0 swapped=$(get "$w" swapped) reports=$(get "$w" reports) offmain=0" \
					"judged=$([ "$(get "$w" noref)" -gt 0 ] && echo "noref:$(get "$w" noref)" || echo -) hooked=1 named=$([ "$(get "$w" reports)" -gt 0 ] && echo "call:$(get "$w" reports)" || echo -) calls=0 callsForgotten=0" ;;
			"walkto "*) [ "${a[1]}" = stop ] && echo stopped || echo "walking to ${a[1]}" ;;
			walkto) echo "not walking" ;;
			"pad stick r 0 -"*) put "$w" pitch $(( $(get "$w" pitch) + 1 )); echo "right stick" ;;
			"pad stick r 0 "*) put "$w" pitch $(( $(get "$w" pitch) - 1 )); echo "right stick" ;;
			"pad button a")
				if [ "$(get "$w" qty)" = 1 ]; then put "$w" qty 0; fdrop "$w" "$WEAPON" 1
				elif [ "$(get "$w" menu)" != 1 ] && [ "$(aimed "$w")" = 1 ]; then line=$(grep -m1 . "$FK/list"); take "$w" "$(field id "$line")"; fi
				echo "button a" ;;
			"pad button right") [ "$(get "$w" menu)" = 1 ] && put "$w" inlist 1; echo "button right" ;;
			"pad button left") put "$w" inlist 0; echo "button left" ;;
			"pad button down") [ "$(get "$w" menu)" = 1 ] && [ "$(get "$w" inlist)" = 1 ] && put "$w" sel $(( $(get "$w" sel) + 1 )); echo "button down" ;;
			"pad button x")
				if [ "$MENUX" = 1 ] && [ "$(selected "$w")" = "$WEAPON" ]; then
					if [ "$(get "$w" "$WEAPON")" -gt 1 ]; then put "$w" qty 1; else fdrop "$w" "$WEAPON" 1; fi
				fi
				echo "button x" ;;
			"pad button b")
				if [ "$(get "$w" qty)" = 1 ]; then put "$w" qty 0; elif [ "$(get "$w" inlist)" = 1 ]; then put "$w" inlist 0; else put "$w" menu 0; fi
				echo "button b" ;;
			"pad off") echo off ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init dt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; o=$(run 1 eval 'drops_drop host 0000000F 7; echo "id=$DROP_ID"')
	t "drop: reported, listed in both with a copy, swapped out of the dropper's world, passes" "$(verdicts "$o")" "PASS=7 "
	t "drop: ... its id" "$(grep -o '^id=.*' <<<"$o")" id=10
	o=$(run 2 drops_take vm 10 0000000F 7)
	t "take: the taker has it, it lies in neither world" "$(verdicts "$o")" "PASS=4 "
	t "take: ... the counts" "$(get host 0000000F),$(get vm 0000000F)" 43,57
	fake 0; o=$(run 3 eval 'drops_drop host 0000000F 7 || echo "none id=$DROP_ID"')
	t "drop: not listed in the other game fails" "$(grep -c '^FAIL vm: lists the 7 of 0000000F host dropped' <<<"$o")" 1
	t "drop: ... and names no id" "$(grep -o '^none id=.*' <<<"$o")" "none id="
	fake 1 0; o=$(run 5 drops_drop host 0000000F 7)
	t "drop: a dropper holding both its item and the copy fails the swap (counted, and its ref out of its world)" \
		"$(grep -c '^FAIL host: counts the swap' <<<"$o"),$(grep -c '^FAIL host: the item it dropped (FF000910) left its world' <<<"$o")" 1,1
	fake 1; run 6 drops_drop host 0000000F 7 >/dev/null; o=$(run 7 drops_take vm 10 0000000F 7 aim)
	t "take with the pad: walked up, crosshair on the copy, a: passes" "$(verdicts "$o")" "PASS=5 "
	t "take with the pad: ... looked down, then level again" "$(grep -c '^PASS vm: its crosshair is on its copy of drop 10: after 3 look' <<<"$o"),$(get vm pitch)" 1,0
	t "take with the pad: ... the counts" "$(get host 0000000F),$(get vm 0000000F)" 43,57
	fake 1 1 99; run 8 drops_drop host 0000000F 7 >/dev/null; o=$(run 9 drops_take vm 10 0000000F 7 aim)
	t "take with the pad: a crosshair never on the copy is RIG (the dev take stands in), never FAIL" "$(verdicts "$o")" "PASS=4 RIG=1 "
	t "take with the pad: ... level again" "$(get vm pitch)" 0
	fake 1; o=$(run 4 drops_take vm 99 0000000F 7)
	t "take: a take its game refused is a broken input, not the mod" "$(grep -c '^FAIL vm: takes drop 99' <<<"$o")" 1

	# The rig's first run: every drop judged noref, never reported: the step says so, naming the counts, and goes no further
	fake 1 1 3 0; o=$(run 10 drops_drop host 0000000F 7)
	t "drop judged noref: the dropper's report fails, with named= judged= hooked= calls=" \
		"$(grep -c '^FAIL host: reports its drop of 7 of 0000000F (the reference it became found).*judged=noref:1 hooked=1 calls=0' <<<"$o"),$(grep -c '^FAIL vm: lists' <<<"$o")" 1,0
	fake 1; o=$(run 11 drops_drop host 0000000F 7)
	t "drop: the report passes with the way the reference was found" "$(grep -c '^PASS host: reports its drop of 7 of 0000000F (the reference it became found): named=call:1' <<<"$o")" 1

	# Through the inventory menu
	fake 1; put host "$WEAPON" 1; o=$(run 12 eval 'drops_menu host '"$WEAPON"'; echo "id=$DROP_ID n=$DROP_N"')
	t "menu: shown, selection moved onto the weapon, x, shut: listed in both, swapped, its player's own: passes" "$(verdicts "$o")" "PASS=8 "
	t "menu: ... its id and count, the menu shut, the weapon gone from the dropper" "$(grep -o '^id=.*' <<<"$o"),$(get host menu),$(get host "$WEAPON")" "id=10 n=1,0,0"
	t "menu: ... the selection reached in MENUAT downs" "$(grep -c '^PASS host: its inventory menu.s selection is on 0001397E: after 3 move' <<<"$o")" 1
	o=$(run 13 drops_take vm 10 "$WEAPON" 1)
	t "menu: ... and the other takes it" "$(verdicts "$o")" "PASS=4 "
	fake 1 1 3 1 3; put vm "$WEAPON" 2; o=$(run 14 eval 'drops_menu vm '"$WEAPON"'; echo "n=$DROP_N"')
	t "menu: two held: the quantity box, a takes one: passes" "$(verdicts "$o"),$(grep -o '^n=.*' <<<"$o"),$(get vm "$WEAPON")" "PASS=8 ,n=1,1"
	fake 1 1 3 1 20; put host "$WEAPON" 1; o=$(run 15 drops_menu host "$WEAPON")
	t "menu: the weapon past the downs of one list: the next category (left, down, right), found again" "$(grep -c '^FAIL\|^RIG' <<<"$o")" 0
	fake 1 1 3 1 99; put host "$WEAPON" 1; o=$(run 16 drops_menu host "$WEAPON")
	t "menu: a selection never on the weapon is RIG, never FAIL, and the menu is shut" "$(verdicts "$o"),$(get host menu),$(get host "$WEAPON")" "RIG=1 ,0,1"
	fake 1 1 3 1 3 0; put host "$WEAPON" 1; o=$(run 17 drops_menu host "$WEAPON")
	t "menu: an x that drops nothing is RIG, never FAIL" "$(verdicts "$o"),$(get host menu)" "PASS=1 RIG=1 ,0"
	fake 1 1 3 0; put host "$WEAPON" 1; o=$(run 18 drops_menu host "$WEAPON")
	t "menu: a menu drop judged noref fails its report" "$(grep -c '^FAIL host: reports its drop of 1 of 0001397E' <<<"$o")" 1
	fake 1; o=$(run 19 drops_menu host "$WEAPON")
	t "menu: nothing to drop is RIG (the set-up)" "$(verdicts "$o")" "RIG=1 "

	# the step is wired into the harness: in STEPS before final, a session step, in the default run
	t "drops is a step, before final" "$(case " $STEPS " in *" drops "*"final "*) echo yes;; esac)" yes
	t "drops needs a session" "$(case " $SESSION_STEPS " in *" drops "*) echo yes;; esac)" yes
	t "drops runs by default" "$(case " $OPTIN_STEPS " in *" drops "*) echo no;; *) echo yes;; esac)" yes
	t "drops has its line" "$([ -n "${STEP_INFO[drops]:-}" ] && echo yes)" yes
	[ $bad -eq 0 ] && echo "drops.sh selftest: all ok" || echo "drops.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
