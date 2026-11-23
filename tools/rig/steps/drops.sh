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
# drops as the inventory's Drop does), `drops take <drop id>` (our player takes the copy, as Take on it does) and `drops
# count <item>`):
#   1. Both players are put in DROPS_CELL with coc (console placement, said), near each other.
#   2. Gold, both ways: the host's player is given DROPS_GOLD (console `player.additem`, set-up) and drops 7. Within
#      DROPS_WAIT the VM lists the drop (the host's, gold, 7) with a copy blocked from activation, and so does the host
#      (its own item swapped for the session's copy: swapped= goes up). The VM takes it: its gold is 7 more, the drop is
#      in neither list, and the host's gold did not come back. Then the VM drops 5 and the host takes them.
#   3. A weapon (`loot find weapon`: the first plain dagger of the base game, the same in both): the host drops one, the
#      VM's copy is of the same base, and the VM takes it.
#   4. Both take at once: the host drops 3 gold, and both players take it at the same moment. One of them has 3 more, the
#      other none (both together: 3), and neither still waits on its claim.
#   5. The dropper leaves: the VM drops 4 gold, the host lists it; the VM leaves the session. The drop goes from the
#      host's list, and the VM's game gives the item back to its world (returned= goes up). The VM joins again.
#   6. A load: the VM saves ($SAVE_NAME), drops 6 gold, the host lists it; the VM loads its save (from before the drop).
#      The drop goes from the host's list: the VM's world has the item again.
#   7. Both are put back in the harness's cell (coc).

DROPS_CELL=${DROPS_CELL:-RiverwoodSleepingGiantInn}  # an interior near the coc marker: both players stand close
DROPS_GOLD=${DROPS_GOLD:-40}                         # gold each player is given to drop
DROPS_WAIT=${DROPS_WAIT:-20}                         # s: an item comes to rest (5 s at most), is offered, swapped, placed
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

# drops_drop <who> <item> <n>: that game's player drops n of the item; DROP_ID is the drop's ID once the other game lists
# it with a copy, and the dropper's own game too (its item swapped for the session's copy). 1 and a FAIL when not listed.
DROP_ID=""
drops_drop() {
	local who=$1 item=$2 n=$3 watcher id known before r i
	DROP_ID=""
	watcher=$(other "$who")
	known=$(drops_ids "$(dc "$watcher" 'drops list')" | tr '\n' ' ')
	before=$(drops_n "$who" "$item")
	r=$(dc "$who" "drops drop $item $n")
	[[ $r == dropped* ]] || { rig_broken input "$who: drops $n of $item" "'drops drop' replied '$r'"; return 1; }
	is_num "$before" && expect_eq "$who: has $n of $item less once it dropped them" "$(drops_n "$who" "$item")" $((before - n))
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
	DROP_ID=$id
}

# drops_take <who> <id> <item> <n>: that game's player takes the drop; it has n more of the item, the drop is in neither
# list, and the other's count did not change
drops_take() {
	local who=$1 id=$2 item=$3 n=$4 o before obefore r w
	o=$(other "$who")
	before=$(drops_n "$who" "$item"); obefore=$(drops_n "$o" "$item")
	r=$(dc "$who" "drops take $id")
	[ "$r" = asked ] || { rig_broken input "$who: takes drop $id" "'drops take' replied '$r'"; return 1; }
	if wait_until "$DROPS_WAIT" drops_has "$who" "$item" $((before + n)); then pass "$who: has the $n of $item it took (drop $id)" "$((before + n))"
	else fail "$who: has the $n of $item it took (drop $id)" "it has $(drops_n "$who" "$item"), had $before: $(dc "$who" drops | cut -c1-300)"; return 1; fi
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
# world; the VM joins again
drops_leave() {
	local item=$1 id r ret0
	drops_drop vm "$item" 4 || return
	id=$DROP_ID
	ret0=$(field returned "$(dc vm drops)")
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	if wait_until "$DROPS_WAIT" drops_unlisted host "$id"; then pass "host: the VM's drop $id goes with the VM" ""
	else fail "host: the VM's drop $id goes with the VM" "$(drops_line "$(dc host 'drops list')" "$id" | cut -c1-200)"; fi
	if is_num "$ret0"; then
		wait_until "$DROPS_WAIT" drops_returned_ge vm $((ret0 + 1))
		expect_ge "vm: gave its item back to its world as it left" "$(field returned "$(dc vm drops)")" $((ret0 + 1))
	else fail "vm: counts what it gave back" "$(dc vm drops | cut -c1-200)"; fi
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; fi
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
			drops_take vm "$id" "$weapon" 1
		fi
	else rig_unhealed state "both: find the same weapon to drop" "host found '$weapon'"; fi

	section "drops: two players taking one item at once: one has it"
	drops_drop host "$g" 3 && drops_both "$DROP_ID" "$g" 3

	section "drops: a player who leaves takes what they dropped still lying there back to their world"
	drops_leave "$g"

	section "drops: a save from before a drop, loaded, takes the drop back from the session"
	drops_load "$g"

	# How the plugin made its game writes (until the frame hook, off the game's main thread: counted, for the record)
	for w in host vm; do echo "     $w: $(dc $w drops | tr ' ' '\n' | grep -E '^(offmain|lost|unplaceable|unsure|gaveBack|forgotten|refused|eventsDropped|judged)=' | tr '\n' ' ')"; done

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
	# (a drop is listed in both with a copy, a take moves the count), 0 a mod that lists nothing for the other game.
	fake() {
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; : > "$FK/list"; echo 10 > "$FK/next"
		for w in host vm; do echo 50 > "$FK/$w.0000000F"; done
	}
	get() { cat "$FK/$1.$2" 2>/dev/null || echo 0; }
	put() { echo "$3" > "$FK/$1.$2"; }
	us() { [ "$1" = host ] && echo 1 || echo 2; }
	dc() {
		local w=$1 c=$2 a n id line; read -ra a <<<"$c"
		case "$c" in
			"drops count "*) echo "count=$(get "$w" "${a[2]}")" ;;
			"drops drop "*)
				put "$w" "${a[2]}" $(( $(get "$w" "${a[2]}") - a[3] )); id=$(cat "$FK/next"); echo $((id + 1)) > "$FK/next"
				echo "id=$id owner=$(us "$w") base=${a[2]} count=${a[3]} copy=FF00$id blocked=1 dist=50 who=$w" >> "$FK/list"
				echo "dropped ref=FF000900" ;;
			"drops list")
				echo -n "lying=$(wc -l < "$FK/list" | tr -d ' ')"
				while IFS= read -r line; do
					[ "$SHARE" = 1 ] || [ "$(field who "$line")" = "$w" ] || continue
					echo -n " | $line"
				done < "$FK/list"; echo ;;
			"drops take "*)
				line=$(grep -E "^id=${a[2]} " "$FK/list"); [ -n "$line" ] || { echo "error: no copy of that drop here"; return; }
				[ "$SHARE" = 1 ] || [ "$(field who "$line")" = "$w" ] || { echo "error: no copy of that drop here"; return; }
				grep -vE "^id=${a[2]} " "$FK/list" > "$FK/list.new"; mv -f "$FK/list.new" "$FK/list"
				n=$(field count "$line"); put "$w" "$(field base "$line")" $(( $(get "$w" "$(field base "$line")") + n ))
				echo asked ;;
			drops) echo "installed=1 us=$(us "$w") claims=0 returned=0 offmain=0" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init dt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; o=$(run 1 eval 'drops_drop host 0000000F 7; echo "id=$DROP_ID"')
	t "drop: listed in both with a copy passes" "$(verdicts "$o")" "PASS=4 "
	t "drop: ... its id" "$(grep -o '^id=.*' <<<"$o")" id=10
	o=$(run 2 drops_take vm 10 0000000F 7)
	t "take: the taker has it, it lies in neither world" "$(verdicts "$o")" "PASS=4 "
	t "take: ... the counts" "$(get host 0000000F),$(get vm 0000000F)" 43,57
	fake 0; o=$(run 3 eval 'drops_drop host 0000000F 7 || echo "none id=$DROP_ID"')
	t "drop: not listed in the other game fails" "$(grep -c '^FAIL vm: lists the 7 of 0000000F host dropped' <<<"$o")" 1
	t "drop: ... and names no id" "$(grep -o '^none id=.*' <<<"$o")" "none id="
	fake 1; o=$(run 4 drops_take vm 99 0000000F 7)
	t "take: a take its game refused is a broken input, not the mod" "$(grep -c '^FAIL vm: takes drop 99' <<<"$o")" 1

	# the step is wired into the harness: in STEPS before final, a session step, in the default run
	t "drops is a step, before final" "$(case " $STEPS " in *" drops "*"final "*) echo yes;; esac)" yes
	t "drops needs a session" "$(case " $SESSION_STEPS " in *" drops "*) echo yes;; esac)" yes
	t "drops runs by default" "$(case " $OPTIN_STEPS " in *" drops "*) echo no;; *) echo yes;; esac)" yes
	t "drops has its line" "$([ -n "${STEP_INFO[drops]:-}" ] && echo yes)" yes
	[ $bad -eq 0 ] && echo "drops.sh selftest: all ok" || echo "drops.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
