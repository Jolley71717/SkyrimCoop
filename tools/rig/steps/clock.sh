#!/bin/bash
# steps/clock.sh: the rig step `clock` (protocols 9 and 32, docs/DESIGN.md "Time and weather"), sourced by steps.sh. The host's
# clock is the session's: a wait of the host's is followed in the VM's game, through midnight, the day count moving with
# the hours; a wait of the VM's is undone after it ends (never under it) and never moves the host; the host's timescale
# is the VM's while in the session and the VM's own goes into its save and comes back when it leaves; after a load and a
# rejoin the VM lines up again; outside, the host's weather is the VM's (held against its climate), a weather of the VM's
# own is undone, the VM's weather spell is the host's sky until it ends, and a player inside a walled city's walls shares
# the sky of one outside them. Not run in a game yet.
#
#   step_clock                                  the step (in the default run, after quests, before final)
#   bash tools/rig/steps/clock.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `clock`, `clock pick <kind> [n]`, `clock weather <id>` and
# `clock release`; the clock= keys are WorldClock::Command's):
#   1. Both games' clocks are ready (in the world, not held by the Helgen opening; the report says what MQ101 reads in
#      each, so a run from coc shows whether the opening's test holds there); the VM follows the host's (owner= the host's
#      player number, heard= going up); the host follows nobody (heard=0).
#   2. Both read the same hour (within CLOCK_SLACK) and timescale.
#   3. The host's wait through midnight: the host's hour is set to CLOCK_START (console `set gamehour`: the scene, said in
#      the report) and the VM lines up; then the host's player waits CLOCK_HOURS hours in the game's Wait menu (the pad:
#      back opens it, right adds an hour, a waits; when the menu does not take the pad, `set gamehour` stands in for it,
#      said as HEAL). The host follows nobody, so its own hour must have moved the hours asked: a wait of another size is
#      the rig's input (RIG), never the mod's. The VM reads the host's hour again, its date rolled over (a midnight step),
#      and its day count moved with its hours (a game's day count and hour move together; a follower that moves one
#      without the other breaks every timer in its save).
#   4. The VM's wait: the VM's player waits CLOCK_VM_HOURS hours in its Wait menu (long enough that the host's reports
#      come while it runs). Its menu must shut by itself: a wait that ran and left the menu open is a FAIL (a player stuck
#      in it), never healed. Within CLOCK_WAIT of its end it is back at the host's hour, by one move back after the wait
#      (its log says its wait held the session's clock and ended; never a move under the wait), as big as the wait (RIG
#      otherwise: the menu's slider); the host's clock did not move (it follows nobody: no lineUps, hourSets or backSets
#      in the host's game, the whole step).
#   5. The timescale: the host sets CLOCK_SCALE (console `set timescale`); the VM reads it, its own kept (own=); the VM
#      sets another itself and is brought back to the host's; the VM saves (its own goes into the save: scaleBacks= up,
#      its log says so), leaves (its own back), joins again (the host's again, its hours lined up), loads its save (lined
#      up again, and the timescale the save held, own= after the load, is the VM's own, not the session's); the host puts
#      its own back and the VM follows.
#   6. Weather, when both stand outside in one worldspace (else a WARN: the step's scene is outside): `clock pick rain`
#      names the same weather in both; the host's weather changes to it (`clock weather`: as its climate changes it, no
#      override); the VM's sky reads it within CLOCK_SKY_WAIT (eased in or forced: eased= or forced= up, and its log). The
#      VM's own weather changes to a snowy one the same way; it is undone within CLOCK_SKY_WAIT and the host's sky stays.
#      Following, the VM holds the host's weather against its own climate (pinned=1; protocol 32).
#   7. Weather magic (protocol 32), outside as 6: the VM's player casts CLOCK_SKY_SPELL (Clear Skies, the shout's third word,
#      as a spell: console `player.cast`, the input, said) on itself. Within CLOCK_MAGIC_WAIT the VM asks the host for the
#      weather its spell holds (asks= up, magicAsked= that weather); the host holds it (asked= the same, askSets= up) and
#      its sky turns to it within CLOCK_SKY_WAIT, the VM's too. The VM's effects are dispelled (console
#      `player.dispelallspells`): the VM says so (askReleases= up), the host lets the weather go (asked=00000000, askLetGo=
#      up), and the VM follows the host's sky again, pinned.
#   8. The walled city, both ways: one player is put inside CLOCK_CITY's walls (console coc: the scene, said), a child
#      worldspace on Tamriel's climate, the other outside; both read one sky (world=), and a weather the host changes to is
#      the VM's. The player goes back to SMOKE_CELL (coc). Any weather override left is let go in both games (`clock
#      release`), so nothing forced outlives the step.
# The host never saves and never loads; its hour stays where the step left it (its save is not written), its timescale
# goes back to what it read at the start.

CLOCK_WAIT=${CLOCK_WAIT:-15}            # s: a clock to reach the other game and be followed (a report within a second of a change)
CLOCK_SKY_WAIT=${CLOCK_SKY_WAIT:-45}    # s: a weather eased in, forced after 30 s (ClockRules::WEATHER_PATIENCE)
CLOCK_SLACK=${CLOCK_SLACK:-0.1}         # game hours two clocks may differ by when read (6 game minutes, 18 real seconds at timescale 20)
CLOCK_DAYS_SLACK=${CLOCK_DAYS_SLACK:-0.1}  # game hours the day count may move apart from the hour over a follow
CLOCK_START=${CLOCK_START:-22.5}        # the host's hour before its wait, so a wait of CLOCK_HOURS crosses midnight
CLOCK_HOURS=${CLOCK_HOURS:-3}           # hours the host's wait asks for
CLOCK_VM_HOURS=${CLOCK_VM_HOURS:-11}    # hours the VM's wait asks for: from about 1.5 (where the host's wait left it), within its day, and long enough to overlap the host's 5 s reports
CLOCK_MENU_WAIT=${CLOCK_MENU_WAIT:-60}  # s: a wait's menu to shut by itself once it took the wait
CLOCK_RAN=${CLOCK_RAN:-0.5}             # game hours a clock must run, its Wait menu open, for the wait to have started
CLOCK_WAIT_SLACK=${CLOCK_WAIT_SLACK:-0.3}  # game hours the host's wait may differ from the hours asked (its clock runs on meanwhile)
CLOCK_BACK_SLACK=${CLOCK_BACK_SLACK:-1.0}  # game hours the VM's move back may fall short of its wait (the host's clock ran on during it)
CLOCK_SCALE=${CLOCK_SCALE:-40}          # the host's timescale for the timescale legs
CLOCK_VM_SCALE=${CLOCK_VM_SCALE:-7}     # the timescale the VM's console sets for itself
CLOCK_CITY=${CLOCK_CITY:-Whiterun}      # a cell inside a walled city's walls (a child worldspace on Tamriel's climate), for coc
CLOCK_SKY_SPELL=${CLOCK_SKY_SPELL:-00078BA4}  # VoiceClearSkiesSelf3 (Skyrim.esm): its script holds a clear weather as an override until the effect ends
CLOCK_MAGIC_WAIT=${CLOCK_MAGIC_WAIT:-20}      # s: a weather magic's ask to reach the host (its script runs a few frames after the effect starts), or its end
CLOCK_HOW=; CLOCK_RAN_PEAK=0.000; CLOCK_MENUS=

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# clock_gap <h1> <h2>: the hours between two times of day on the 24-hour clock (0 to 12), 3 decimals; empty when either is
# not a number
clock_gap() { is_num "$1" && is_num "$2" || return 0; awk -v a="$1" -v b="$2" 'BEGIN{g = a - b; g -= 24 * int(g / 24); if (g < 0) g += 24; if (g > 12) g = 24 - g; printf "%.3f\n", g}'; }
# clock_forward <from> <to>: the hours forward from one time of day to the other (0 to 24), 3 decimals
clock_forward() { is_num "$1" && is_num "$2" || return 0; awk -v a="$1" -v b="$2" 'BEGIN{g = b - a; g -= 24 * int(g / 24); if (g < 0) g += 24; printf "%.3f\n", g}'; }
# clock_days_fit <days0> <hour0> <days1> <hour1>: the hours by which the day count's move and the hour's move differ (the
# hour's move taken in whole days nearest the day count's), 3 decimals: 0 when the two moved together
clock_days_fit() {
	is_num "$1" && is_num "$2" && is_num "$3" && is_num "$4" || return 0
	awk -v d0="$1" -v h0="$2" -v d1="$3" -v h1="$4" 'BEGIN{dd = (d1 - d0) * 24; dh = h1 - h0; while (dd - dh > 12) dh += 24; while (dh - dd > 12) dh -= 24; r = dd - dh; if (r < 0) r = -r; printf "%.3f\n", r}'
}
# clock_le <a> <b>: a <= b, as numbers
clock_le() { is_num "$1" && is_num "$2" && awk -v a="$1" -v b="$2" 'BEGIN{exit !(a <= b)}'; }
# clock_sum <a> <b>: a + b, 3 decimals
clock_sum() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.3f\n", a + b}'; }
# clock_hex <reply> <key>: an 8-digit form ID the reply gives for that key, or empty
clock_hex() { local v; v=$(field "$2" "$1"); [[ $v =~ ^[0-9A-F]{8}$ ]] && echo "$v"; }
# clock_back_size <log lines>: the size in hours of the last "Clock: back <from> -> <to>" move among them, 3 decimals; empty
# with none
clock_back_size() {
	local last; last=$(grep -aE 'Clock: back [0-9.]+ -> [0-9.]+' <<<"$1" | tail -1 | sed -n 's/.*Clock: back \([0-9.]*\) -> \([0-9.]*\).*/\1 \2/p')
	[ -n "$last" ] || return 0
	awk -v a="${last% *}" -v b="${last#* }" 'BEGIN{printf "%.3f\n", a - b}'
}

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

clock_of() { dc "$1" clock; }
clock_key() { field "$2" "$(clock_of "$1")"; }
# clock_together: the two games read the same hour now (within CLOCK_SLACK)
clock_together() { clock_le "$(clock_gap "$(clock_key host hour)" "$(clock_key vm hour)")" "$CLOCK_SLACK"; }
# clock_expect_together <check>: within CLOCK_WAIT both games read the same hour (PASS), or FAIL with both hours
clock_expect_together() {
	if wait_until "$CLOCK_WAIT" clock_together; then pass "$1" "host hour=$(clock_key host hour), vm hour=$(clock_key vm hour)"; return 0; fi
	fail "$1" "host hour=$(clock_key host hour), vm hour=$(clock_key vm hour) (more than $CLOCK_SLACK h apart after $CLOCK_WAIT s)"; return 1
}
_clock_scale_is() { [ "$(awk -v a="$(clock_key "$1" scale)" -v b="$2" 'BEGIN{print (a - b < 0.01 && b - a < 0.01)}')" = 1 ]; }
# clock_expect_scale <check> <who> <value>: within CLOCK_WAIT that game's timescale reads <value>
clock_expect_scale() {
	if wait_until "$CLOCK_WAIT" _clock_scale_is "$2" "$3"; then pass "$1" "scale=$(clock_key "$2" scale) own=$(clock_key "$2" own)"; return 0; fi
	fail "$1" "$2 reads scale=$(clock_key "$2" scale), want $3"; return 1
}
_clock_weather_is() { [ "$(clock_key "$1" weather)" = "$2" ]; }
# clock_count <who> <key>: a count of that game's clock line (0 when it gives none)
clock_count() { local v; v=$(clock_key "$1" "$2"); is_num "$v" && echo "$v" || echo 0; }
# clock_said <who> <pattern>: that game's plugin log lines since the run's mark that match
clock_said() { log_since_mark "$1" "$2" | wc -l | tr -d ' '; }
# clock_console <check> <who> <command>: the console ran it, or the rig is broken (the input)
clock_console() {
	local r; r=$(dc "$2" "console $3")
	[ "$r" = ran ] && return 0
	rig_broken input "$1" "$2: 'console $3' replied '$r'"; return 1
}
# clock_weather <check> <who> <weather>: that game's own weather changed to it (dev `clock weather`: as its climate changes
# it, not an override), or the rig is broken (the input)
clock_weather() {
	local r; r=$(dc "$2" "clock weather $3")
	[ "$(field weather "$r")" = "$3" ] && return 0
	rig_broken input "$1" "$2: 'clock weather $3' replied '$r'"; return 1
}

# clock_status <who>: that game's clock is ready: in the world, not held by the opening. The report says what MQ101 reads
# there (the clock line's mq101= and canMove=, and the quest's own line), so the first run from coc shows the opening's
# test against the game.
clock_status() {
	local s q; s=$(clock_of "$1")
	if [[ $s != ready=* ]]; then rig_broken game "$1: its clock answers" "$(cut -c1-160 <<<"$s")"; return 1; fi
	q=$(dc "$1" 'quest MQ101' | cut -c1-110)
	if [ "$(field opening "$s")" = 1 ]; then
		rig_fault state "$1: out of the Helgen opening" "the opening holds its clock (mq101=$(field mq101 "$s") canMove=$(field canMove "$s"); $q): its own until the opening ends"; return 1
	fi
	if [ "$(field ready "$s")" != 1 ]; then rig_fault state "$1: its clock is ready (in the world)" "$(cut -c1-160 <<<"$s")"; return 1; fi
	pass "$1: its clock is ready" "mq101=$(field mq101 "$s") canMove=$(field canMove "$s") ($q); $(cut -c1-80 <<<"$s")"
}

# clock_menu_wait <who> <hours>: that game's player waits in the game's Wait menu (the pad: back opens it, it starts at
# one hour, right adds one, a waits; the menu shuts itself when the wait is done). While the menu is open its hour is
# read each second: CLOCK_RAN_PEAK is the most it ran forward from where it was (a pull back reads as none). 0 when the
# menu shut itself after a; 1 when it never opened, or stayed open with the hour not running (the menu did not take the
# pad: the rig's input), then shut with b; 2 when the wait ran (CLOCK_RAN hours or more) and the menu did not shut within
# CLOCK_MENU_WAIT s (a player stuck in a wait: CLOCK_MENUS, the menus then), then shut with b.
_clock_menu() { [[ $(field menus "$(dc "$1" where)") == *Sleep/Wait* ]]; }
clock_menu_wait() {
	local w=$1 n=$2 i h0 f
	CLOCK_RAN_PEAK=0.000; CLOCK_MENUS=
	h0=$(clock_key "$w" hour)
	dc "$w" 'pad button back' >/dev/null
	wait_until 5 _clock_menu "$w" || return 1
	for ((i = 1; i < n; i++)); do dc "$w" 'pad button right' >/dev/null; sleep 0.4; done
	dc "$w" 'pad button a' >/dev/null
	for ((i = 0; i < CLOCK_MENU_WAIT; i++)); do
		_clock_menu "$w" || return 0
		f=$(clock_forward "$h0" "$(clock_key "$w" hour)")
		CLOCK_RAN_PEAK=$(awk -v p="$CLOCK_RAN_PEAK" -v f="${f:-0}" -v n="$n" 'BEGIN{if (f <= n + 1 && f > p) p = f; printf "%.3f\n", p}')
		sleep 1
	done
	CLOCK_MENUS=$(field menus "$(dc "$w" where)")
	dc "$w" 'pad button b' >/dev/null; sleep 1
	clock_le "$CLOCK_RAN" "$CLOCK_RAN_PEAK" && return 2
	return 1
}
# clock_wait <check> <who> <hours>: that game's player waits <hours>: in its Wait menu (CLOCK_HOW=menu), or, when the
# menu does not take the pad, by the console's set gamehour, the hour <hours> on past midnight (a HEAL, CLOCK_HOW=console).
# A wait that ran and left its menu open is a FAIL, never healed. 0 when the wait or its stand-in was made.
clock_wait() {
	local w=$2 n=$3 h0 to r=0
	CLOCK_HOW=
	h0=$(clock_key "$w" hour)
	clock_menu_wait "$w" "$n" || r=$?
	if [ "$r" = 0 ]; then CLOCK_HOW=menu; pass "$1" "$w waited in its Wait menu: hour $h0 -> $(clock_key "$w" hour)"; return 0; fi
	if [ "$r" = 2 ]; then
		fail "$1" "$w's Wait menu took the wait (its hour ran $CLOCK_RAN_PEAK h of $n) and never shut in $CLOCK_MENU_WAIT s: menus=$CLOCK_MENUS"; return 1
	fi
	to=$(awk -v h="$(clock_key "$w" hour)" -v n="$n" 'BEGIN{t = h + n; t -= 24 * int(t / 24); printf "%.2f\n", t}')
	healed "$1" "the Wait menu did not take the pad in the $w's game: its hour is set with the console instead (set gamehour to $to)"
	CLOCK_HOW=console
	clock_console "$1: the console" "$w" "set gamehour to $to"
}

# clock_follow_leg: the host's wait through midnight, followed by the VM with its date and day count
clock_follow_leg() {
	local v0 v1 d0 d1 h0 h1 steps0 day0 lines0 fit hw0 moved
	clock_console "host: its hour to $CLOCK_START, the scene (console set gamehour)" host "set gamehour to $CLOCK_START" || return
	clock_expect_together "vm: lined up with the host's hour $CLOCK_START" || return
	v0=$(clock_of vm); d0=$(field days "$v0"); h0=$(field hour "$v0"); steps0=$(field midnightSteps "$v0"); day0=$(field day "$v0")
	lines0=$(clock_said vm 'Clock: forward')
	hw0=$(clock_key host hour)
	clock_wait "host: waits $CLOCK_HOURS hours, through midnight" host "$CLOCK_HOURS" || return
	# The host follows nobody: its own hour moved as far as it waited, or the wait was not the one asked (the rig's input).
	moved=$(clock_forward "$hw0" "$(clock_key host hour)")
	if clock_le "$(clock_sum "$CLOCK_HOURS" "-$CLOCK_WAIT_SLACK")" "$moved" && clock_le "$moved" "$(clock_sum "$CLOCK_HOURS" "$CLOCK_WAIT_SLACK")"; then
		pass "host: its wait made the $CLOCK_HOURS hours asked (it follows nobody)" "hour $hw0 -> $(clock_key host hour), $moved h"
	else
		rig_fault input "host: its wait made the $CLOCK_HOURS hours asked" "its hour moved ${moved:-?} h ($hw0 -> $(clock_key host hour)): the Wait menu did not take the pad as asked (its slider, pad right)"; return
	fi
	clock_expect_together "vm: reads the host's hour after the host's wait" || return
	v1=$(clock_of vm); d1=$(field days "$v1"); h1=$(field hour "$v1")
	expect_ge "vm: stopped at midnight for its date to roll over (midnightSteps)" "$(field midnightSteps "$v1")" $(( ${steps0:-0} + 1 ))
	if [ "$(field day "$v1")" != "$day0" ]; then pass "vm: its date rolled over" "day $day0 -> $(field day "$v1")"; else fail "vm: its date rolled over" "day $day0 -> $(field day "$v1")"; fi
	fit=$(clock_days_fit "$d0" "$h0" "$d1" "$h1")
	if clock_le "$fit" "$CLOCK_DAYS_SLACK"; then pass "vm: its day count moved with its hours" "days $d0 -> $d1, hour $h0 -> $h1 (apart by $fit h)"
	else fail "vm: its day count moved with its hours" "days $d0 -> $d1, hour $h0 -> $h1: apart by ${fit:-?} h"; fi
	expect_ge "vm: its log says it moved forward by the host's clock" "$(clock_said vm 'Clock: forward')" $((lines0 + 1))
}

# clock_own_wait_leg: the VM's own wait runs untouched and is undone after it ends; the host's clock does not move
clock_own_wait_leg() {
	local n=$CLOCK_VM_HOURS h0 back0 hostHour lines0 ended0 size
	h0=$(clock_key vm hour)
	if ! clock_le "$(clock_sum "$h0" "$n")" 23.5; then
		rig_fault state "vm: its wait of $n hours stays within its day" "its hour is ${h0:-?}: the leg needs it before $(clock_sum 23.5 "-$n") (the host's wait leaves it near 1.5)"; return
	fi
	back0=$(clock_count vm backSets); lines0=$(clock_said vm 'Clock: back '); ended0=$(clock_said vm 'Clock: our own sleep or wait ended')
	hostHour=$(clock_key host hour)
	clock_wait "vm: waits $n hours itself" vm "$n" || return
	clock_expect_together "vm: brought back to the host's hour after its own wait" || return
	expect_ge "vm: moved back within the day (backSets)" "$(clock_count vm backSets)" $((back0 + 1))
	expect_eq "vm: moved back once, after its wait, never under it (its log's back moves)" "$(clock_said vm 'Clock: back ')" $((lines0 + 1))
	if [ "$CLOCK_HOW" = menu ]; then
		expect_ge "vm: its log says its own wait held the session's clock until it ended" "$(clock_said vm 'Clock: our own sleep or wait ended')" $((ended0 + 1))
		size=$(clock_back_size "$(log_since_mark vm 'Clock: back ')")
		if clock_le "$(clock_sum "$n" "-$CLOCK_BACK_SLACK")" "$size"; then pass "vm: its wait made the $n hours asked (its move back)" "back $size h"
		else rig_fault input "vm: its wait made the $n hours asked (its move back)" "moved back ${size:-?} h: the Wait menu did not take the pad as asked (its slider, pad right)"; fi
	fi
	h0=$(clock_forward "$hostHour" "$(clock_key host hour)")
	if clock_le "$h0" 0.25; then pass "host: its clock did not move for the VM's wait" "it ran $h0 h meanwhile"
	else fail "host: its clock did not move for the VM's wait" "it moved $h0 h"; fi
}

# clock_scale_leg <host's own timescale>: the host's timescale is the VM's; the VM's own goes into its save and comes back
# when it leaves; a rejoin and a load line the VM up again
clock_scale_leg() {
	local own=$1 vmOwn backs0 saved0 ups0 r
	vmOwn=$(clock_key vm own); [ "$vmOwn" = - ] && vmOwn=$(clock_key vm scale)  # its own: kept when it follows already
	vmOwn=$(awk -v v="$vmOwn" 'BEGIN{printf "%.2f\n", v}')
	clock_console "host: timescale $CLOCK_SCALE (console set timescale)" host "set timescale to $CLOCK_SCALE" || return
	clock_expect_scale "vm: follows the host's timescale $CLOCK_SCALE" vm "$CLOCK_SCALE"
	expect_eq "vm: keeps its own timescale to put back" "$(clock_key vm own)" "$vmOwn"
	clock_console "vm: its own timescale $CLOCK_VM_SCALE (console)" vm "set timescale to $CLOCK_VM_SCALE" || return
	clock_expect_scale "vm: brought back to the host's timescale" vm "$CLOCK_SCALE"
	expect_eq "host: its timescale stays its own" "$(awk -v v="$(clock_key host scale)" 'BEGIN{printf "%.0f\n", v}')" "$CLOCK_SCALE"

	backs0=$(clock_count vm scaleBacks); saved0=$(clock_said vm 'Clock: timescale .*(our own, for the save)')
	clock_console "vm: saves mid-session (its own timescale into the save)" vm "save $SAVE_NAME" || return
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return; }
	expect_ge "vm: put its own timescale back for the save (scaleBacks)" "$(clock_count vm scaleBacks)" $((backs0 + 1))
	expect_ge "vm: its log says its own timescale went into the save" "$(clock_said vm 'Clock: timescale .*(our own, for the save)')" $((saved0 + 1))
	clock_expect_scale "vm: the host's timescale again after its save" vm "$CLOCK_SCALE"

	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	clock_expect_scale "vm: its own timescale back when it left the session" vm "$vmOwn"
	ups0=$(clock_count vm lineUps)
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	clock_expect_scale "vm: the host's timescale again after joining" vm "$CLOCK_SCALE"
	clock_expect_together "vm: the host's hour again after joining (the session's kept clock)"
	expect_ge "vm: lined up afresh after joining (lineUps)" "$(clock_count vm lineUps)" $((ups0 + 1))

	ups0=$(clock_count vm lineUps)
	clock_console "vm: loads its save" vm "load $SAVE_NAME" || return
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; }
	wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return; }
	clock_expect_together "vm: the host's hour after loading its save"
	clock_expect_scale "vm: the host's timescale after loading its save" vm "$CLOCK_SCALE"
	expect_ge "vm: lined up afresh after its load (lineUps)" "$(clock_count vm lineUps)" $((ups0 + 1))
	# A load makes the loaded save's timescale the game's own (ScaleKeeper::Forget, kept again on the next follow): own=
	# now is what the save held. The counter and the log line above are written whether or not the value reached the file.
	expect_eq "vm: its save held its own timescale, not the session's (own= after its load)" "$(clock_key vm own)" "$vmOwn"

	clock_console "host: its own timescale back ($own)" host "set timescale to $own" || return
	clock_expect_scale "vm: follows the host's own timescale back" vm "$own"
}

# clock_sky_leg: the host's weather is the VM's outside; the VM's own is undone. 1 when the scene is not outside (WARN).
clock_sky_leg() {
	local h v world rain snow forced0 eased0 lines0 vw
	h=$(clock_of host); v=$(clock_of vm)
	world=$(clock_hex "$h" world)
	if [ "$(field outside "$h")" != 1 ] || [ "$(field outside "$v")" != 1 ] || [ -z "$world" ] || [ "$world" != "$(clock_hex "$v" world)" ]; then
		warn "both: outside in one worldspace for the weather legs" "host outside=$(field outside "$h") world=$(field world "$h"), vm outside=$(field outside "$v") world=$(field world "$v"): the weather legs are not made"
		return 1
	fi
	rain=$(clock_hex "$(dc host 'clock pick rain')" weather)
	[ -n "$rain" ] || { rig_fault state "host: a rainy weather of the base game" "$(dc host 'clock pick rain' | cut -c1-120)"; return 1; }
	forced0=$(clock_count vm forced); eased0=$(clock_count vm eased); lines0=$(clock_said vm "Clock: weather .* -> $rain")
	clock_weather "host: its weather to $rain (clock weather: as its climate changes it)" host "$rain" || return 1
	wait_until 10 _clock_weather_is host "$rain" || { rig_fault input "host: its sky reads $rain after its change" "weather=$(clock_key host weather)"; return 1; }
	if wait_until "$CLOCK_SKY_WAIT" _clock_weather_is vm "$rain"; then pass "vm: its sky is the host's ($rain)" "forced=$(clock_count vm forced) eased=$(clock_count vm eased)"
	else fail "vm: its sky is the host's ($rain)" "vm weather=$(clock_key vm weather) override=$(clock_key vm override) held=$(clock_count vm held)"; fi
	expect_ge "vm: eased in or forced the host's weather (eased + forced)" $(( $(clock_count vm forced) + $(clock_count vm eased) )) $((forced0 + eased0 + 1))
	expect_ge "vm: its log says the weather changed to the host's" "$(clock_said vm "Clock: weather .* -> $rain")" $((lines0 + 1))
	if wait_until "$CLOCK_WAIT" _clock_key_is vm pinned 1; then pass "vm: holds the host's weather against its own climate (pinned=1)" "pins=$(clock_count vm pins)"
	else fail "vm: holds the host's weather against its own climate (pinned=1)" "pinned=$(clock_key vm pinned) override=$(clock_key vm override) weather=$(clock_key vm weather)"; fi

	snow=$(clock_hex "$(dc vm 'clock pick snow')" weather)
	[ -n "$snow" ] || { rig_fault state "vm: a snowy weather of the base game" "$(dc vm 'clock pick snow' | cut -c1-120)"; return 1; }
	clock_weather "vm: its own weather to $snow (clock weather: as its climate changes it)" vm "$snow" || return 1
	sleep 2
	vw=$(clock_of vm)
	if [ "$(field override "$vw")" = 1 ]; then
		rig_fault state "vm: its own weather undone, the host's again" "a script of the VM's game holds a weather override (held=$(field held "$vw")): the session's weather leaves it alone; weather=$(field weather "$vw")"
	elif wait_until "$CLOCK_SKY_WAIT" _clock_weather_is vm "$rain"; then
		pass "vm: its own weather undone, the host's again" "weather=$rain"
	else
		fail "vm: its own weather undone, the host's again" "vm weather=$(clock_key vm weather), want $rain"
	fi
	expect_eq "host: its sky stays its own" "$(clock_key host weather)" "$rain"
}

_clock_key_is() { [ "$(clock_key "$1" "$2")" = "$3" ]; }
_clock_above() { [ "$(clock_count "$1" "$2")" -gt "$3" ]; }
_clock_let_go() { _clock_key_is host asked 00000000 && _clock_above host askLetGo "$1"; }

# clock_magic_leg: the VM's weather spell is the host's sky until it ends (the step's 7). Run after clock_sky_leg (both
# outside, under one sky).
clock_magic_leg() {
	local asks0 rel0 lets0 mine
	asks0=$(clock_count vm asks); rel0=$(clock_count vm askReleases); lets0=$(clock_count host askLetGo)
	clock_console "vm: casts $CLOCK_SKY_SPELL on itself (console player.cast: the input)" vm "player.cast $CLOCK_SKY_SPELL player" || return 1
	if wait_until "$CLOCK_MAGIC_WAIT" _clock_above vm asks "$asks0"; then pass "vm: its weather magic asked of the host" "asks=$(clock_count vm asks) magicAsked=$(clock_key vm magicAsked) magicWindows=$(clock_count vm magicWindows)"
	else fail "vm: its weather magic asked of the host" "asks=$(clock_count vm asks) magicWindows=$(clock_count vm magicWindows) magic=$(clock_key vm magic) weather=$(clock_key vm weather) override=$(clock_key vm override)"; return 1; fi
	mine=$(clock_hex "$(clock_of vm)" magicAsked)
	if [ -z "$mine" ] || [ "$mine" = 00000000 ]; then
		fail "vm: its spell holds its weather (magicAsked=)" "magicAsked=$(clock_key vm magicAsked) magic=$(clock_key vm magic) override=$(clock_key vm override): asked as not held, so the host never lets it go"
		return 1
	fi
	if wait_until "$CLOCK_WAIT" _clock_key_is host asked "$mine"; then pass "host: holds the VM's weather magic ($mine)" "askSets=$(clock_count host askSets) askedBy=$(clock_key host askedBy)"
	else fail "host: holds the VM's weather magic ($mine)" "asked=$(clock_key host asked) askSets=$(clock_count host askSets) askIgnored=$(clock_count host askIgnored) host world=$(clock_key host world)"; fi
	if wait_until "$CLOCK_SKY_WAIT" _clock_weather_is host "$mine"; then pass "host: its sky is the VM's spell's ($mine)" "askLetGo=$(clock_count host askLetGo)"
	else fail "host: its sky is the VM's spell's ($mine)" "host weather=$(clock_key host weather) asked=$(clock_key host asked) askLetGo=$(clock_count host askLetGo)"; fi
	if wait_until "$CLOCK_SKY_WAIT" _clock_weather_is vm "$mine"; then pass "vm: its sky is its spell's ($mine)" "magic=$(clock_key vm magic)"
	else fail "vm: its sky is its spell's ($mine)" "vm weather=$(clock_key vm weather) override=$(clock_key vm override) magic=$(clock_key vm magic)"; fi

	clock_console "vm: its effects dispelled (console player.dispelallspells: the input)" vm "player.dispelallspells" || return 1
	if wait_until "$CLOCK_MAGIC_WAIT" _clock_above vm askReleases "$rel0"; then pass "vm: tells the host its weather magic ended" "askReleases=$(clock_count vm askReleases)"
	else fail "vm: tells the host its weather magic ended" "askReleases=$(clock_count vm askReleases) magicAsked=$(clock_key vm magicAsked) override=$(clock_key vm override)"; fi
	if wait_until "$CLOCK_WAIT" _clock_let_go "$lets0"; then pass "host: let the VM's weather go" "askLetGo=$(clock_count host askLetGo)"
	else fail "host: let the VM's weather go" "asked=$(clock_key host asked) askLetGo=$(clock_count host askLetGo), was $lets0"; fi
	if wait_until "$CLOCK_SKY_WAIT" _clock_key_is vm pinned 1; then pass "vm: follows the host's sky again, pinned" "vm weather=$(clock_key vm weather) host weather=$(clock_key host weather)"
	else fail "vm: follows the host's sky again, pinned" "pinned=$(clock_key vm pinned) magic=$(clock_key vm magic) override=$(clock_key vm override) vm weather=$(clock_key vm weather) host weather=$(clock_key host weather)"; fi
}

# clock_back <who>: that player back in SMOKE_CELL (console coc: the scene, said)
clock_back() {
	if [ "$(dc "$1" "console coc $SMOKE_CELL")" = ran ] && wait_until 90 in_world "$1"; then pass "$1: back in $SMOKE_CELL (console coc: the scene)" "$(dc "$1" where | cut -c1-80)"; return 0; fi
	rig_unhealed state "$1: back in $SMOKE_CELL (console coc)" "$(dc "$1" where | cut -c1-120)"; return 1
}

# clock_city_leg <who inside> <who outside>: one player inside CLOCK_CITY's walls (a child worldspace on Tamriel's climate),
# the other outside them: one sky (world=), and a weather the host changes to is the VM's, whichever is inside
clock_city_leg() {
	local in=$1 out=$2 c o place wx where
	clock_console "$in: inside $CLOCK_CITY's walls, the scene (console coc)" "$in" "coc $CLOCK_CITY" || return 1
	sleep 3
	wait_until 90 in_world "$in" || { rig_unhealed state "$in: in the world inside $CLOCK_CITY's walls" "$(dc "$in" where | cut -c1-120)"; return 1; }
	c=$(clock_of "$in"); o=$(clock_of "$out")
	place=$(clock_hex "$c" place)
	if [ "$(field outside "$c")" != 1 ] || [ -z "$place" ] || [ "$place" = "$(clock_hex "$o" place)" ] || [ "$(field outside "$o")" != 1 ]; then
		rig_fault state "$in: inside $CLOCK_CITY's walls, $out outside them" "$in outside=$(field outside "$c") place=$(field place "$c"), $out outside=$(field outside "$o") place=$(field place "$o"): coc $CLOCK_CITY did not make the scene"
		clock_back "$in"; return 1
	fi
	pass "$in: inside $CLOCK_CITY's walls, another worldspace than $out's outside them" "place=$place, $out's place=$(field place "$o")"
	expect_eq "$in: under the sky of the land round the walls (world= the same as $out's)" "$(clock_hex "$c" world)" "$(clock_hex "$o" world)"
	wx=$(clock_hex "$(dc host 'clock pick cloudy 0')" weather)
	[ -n "$wx" ] || { rig_fault state "host: a cloudy weather of the base game" "$(dc host 'clock pick cloudy 0' | cut -c1-120)"; clock_back "$in"; return 1; }
	where=$([ "$in" = vm ] && echo "inside the walls" || echo "outside the walls, the host inside")
	clock_weather "host: its weather to $wx (clock weather)" host "$wx" || { clock_back "$in"; return 1; }
	if wait_until "$CLOCK_SKY_WAIT" _clock_weather_is vm "$wx"; then pass "vm ($where): its sky is the host's ($wx)" "forced=$(clock_count vm forced) eased=$(clock_count vm eased)"
	else fail "vm ($where): its sky is the host's ($wx)" "vm weather=$(clock_key vm weather) world=$(clock_key vm world) place=$(clock_key vm place) override=$(clock_key vm override); host world=$(clock_key host world) place=$(clock_key host place)"; fi
	clock_back "$in"
}

# clock_release: no weather override left in either game (dev `clock release`), so nothing forced outlives the step
clock_release() {
	local w r
	for w in host vm; do
		r=$(dc $w 'clock release')
		if [ "$(field override "$r")" = 0 ]; then pass "$w: no weather override left" "$r"; else warn "$w: no weather override left" "'clock release' replied '$r'"; fi
	done
}

step_clock() {
	local w hostId own
	section "clock: both games' clocks, and whose is the session's"
	for w in host vm; do clock_status $w || return; done
	hostId=$(field player "$(dc host 'session status')")
	if wait_until "$CLOCK_WAIT" _clock_heard_from vm "$hostId"; then pass "vm: follows the host's clock" "owner=$hostId heard=$(clock_key vm heard)"
	else fail "vm: follows the host's clock" "owner=$(clock_key vm owner) heard=$(clock_key vm heard), the host is player $hostId"; return; fi
	expect_zero "host: follows nobody's clock" "$(clock_count host heard)"
	clock_expect_together "both: the same hour"
	own=$(awk -v v="$(clock_key host scale)" 'BEGIN{printf "%.2f\n", v}')
	clock_expect_scale "vm: the host's timescale" vm "$own"

	section "clock: the host's wait, through midnight, is the VM's too"
	clock_follow_leg
	section "clock: the VM's own wait runs untouched and is undone after it; the host's clock stays"
	clock_own_wait_leg
	section "clock: the host's timescale; the VM's own in its save and after it leaves; a rejoin and a load"
	clock_scale_leg "$own"
	section "clock: outside, the host's weather is the VM's"
	if clock_sky_leg; then
		section "clock: the VM's weather spell is the host's sky until it ends"
		clock_magic_leg
		section "clock: a walled city and the land round it are one sky, both ways"
		clock_city_leg host vm && clock_city_leg vm host
	fi
	clock_release

	section "clock: the host followed nobody, the whole step"
	for w in lineUps hourSets backSets midnightSteps; do expect_zero "host: $w" "$(clock_count host $w)"; done
}
_clock_heard_from() { local s; s=$(clock_of "$1"); [ "$(field owner "$s")" = "$2" ] && is_num "$(field heard "$s")" && [ "$(field heard "$s")" -ge 1 ]; }

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init clocktest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; grep -E "^(FAIL|RIG|RIGFAULT|WARN|HEAL) " <<<"${o:-}" | sed "s/^/       /"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	CLOCK_WAIT=2; CLOCK_SKY_WAIT=2; CLOCK_MENU_WAIT=3

	t "gap: within the day" "$(clock_gap 14.5 13.0)" 1.500
	t "gap: across midnight" "$(clock_gap 0.5 23.5)" 1.000
	t "gap: the other way" "$(clock_gap 23.5 0.5)" 1.000
	t "gap: not a number" "$(clock_gap none 1)" ""
	t "forward: within the day" "$(clock_forward 22.5 23.5)" 1.000
	t "forward: through midnight" "$(clock_forward 22.5 1.5)" 3.000
	t "forward: back is nearly a day" "$(clock_forward 4.0 3.0)" 23.000
	t "days fit: moved together" "$(clock_days_fit 10.9375 22.5 11.0625 1.5)" 0.000
	t "days fit: the hours moved, the days not" "$(clock_days_fit 10.9375 22.5 10.9375 1.5)" 3.000
	t "days fit: a whole day and three hours" "$(clock_days_fit 10.9375 22.5 12.0625 1.5)" 0.000
	t "days fit: a move back" "$(clock_days_fit 11.0 4.0 10.875 1.0)" 0.000
	t "le" "$(clock_le 0.05 0.1 && echo yes)" yes
	t "le: not" "$(clock_le 0.2 0.1 || echo no)" no
	t "le: not a number" "$(clock_le '' 0.1 || echo no)" no
	t "sum" "$(clock_sum 3 -0.3)" 2.700
	t "hex" "$(clock_hex 'weather=0010A241 world=00000000' weather)" 0010A241
	t "hex: none" "$(clock_hex 'weather=-' weather)" ""
	t "back size: the last move back" "$(clock_back_size $'[I] Clock: back 4.00 -> 3.00 (player 1\'s clock)\n[I] Clock: back 12.50 -> 1.60 (player 1\'s clock)')" 10.900
	t "back size: none" "$(clock_back_size '[I] Clock: forward 4.00 -> 5.00')" ""

	# ---- both games faked ----
	# Each game has an hour, a day count, a date, a timescale, a weather, a place (the worldspace it stands in) and counts. A
	# change in the host's game reaches the VM's by MODE: good (the mod at work: the VM follows, day count with the hours,
	# midnight steps, back within the day after its own wait, its own timescale kept, put back and saved, one sky across a
	# city's walls), none (nothing reaches the VM), nodays (the hours follow, the day count does not), friendmoves (the VM's
	# wait moves the host instead of being undone), nosky (the weather does not follow), underwait (the VM's own wait is
	# pulled back while it runs, then once more), savesession (the VM's save holds the session's timescale), rawsky (a
	# walled city is its own sky: the VM never follows across the walls), nopin (the VM never holds the host's weather),
	# nomagic (the VM's weather spell is never asked of the host), norelease (the host never lets the VM's spell's weather go).
	# MENU: 1 the Wait menu takes the pad; 0 it never opens (the console stands in); noright its slider ignores right (a
	# wait of one hour); noa it opens but a does nothing; stuck the wait runs and the menu never shuts.
	fake() {  # fake <mode> [menu]
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; MODE=$1; MENU=${2:-1}
		for w in host vm; do
			for k in lineUps hourSets backSets midnightSteps forced eased held scaleBacks heard menu away rights pinned magicWindows asks askReleases askSets askLetGo; do echo 0 > "$FK/$w.$k"; done
			echo 20.00 > "$FK/$w.scale"; echo - > "$FK/$w.own"; echo - > "$FK/$w.saved"; : > "$FK/$w.log"; echo 0010A242 > "$FK/$w.weather"; echo 0000003C > "$FK/$w.place"
			echo 00000000 > "$FK/$w.asked"; echo 00000000 > "$FK/$w.magicAsked"
		done
		[ "$1" = nopin ] || echo 1 > "$FK/vm.pinned"
		echo 9.000 > "$FK/host.hour"; echo 10.3750 > "$FK/host.days"; echo 17 > "$FK/host.day"
		echo 9.000 > "$FK/vm.hour"; echo 3.3750 > "$FK/vm.days"; echo 4 > "$FK/vm.day"
		[ "$1" = none ] || echo 1 > "$FK/vm.heard"
	}
	g() { cat "$FK/$1.$2"; }
	s() { echo "$3" > "$FK/$1.$2"; }
	inc() { s "$1" "$2" $(( $(g "$1" "$2") + 1 )); }
	say() { echo "[00:00:00.000] [1] [I] $2" >> "$FK/$1.log"; }
	# fmove <who> <hours>: that game's clock runs on (or back) by <hours> as waiting does: the hour, the day count, and the
	# date when it passes midnight
	fmove() {
		local h n; h=$(g "$1" hour)
		n=$(awk -v h="$h" -v m="$2" 'BEGIN{t = h + m; n = int(t / 24); if (t < 0 && n * 24 != t) n--; print n}')
		s "$1" hour "$(awk -v h="$h" -v m="$2" -v n="$n" 'BEGIN{printf "%.3f\n", h + m - 24 * n}')"
		s "$1" days "$(awk -v d="$(g "$1" days)" -v m="$2" 'BEGIN{printf "%.4f\n", d + m / 24}')"
		s "$1" day $(( $(g "$1" day) + n ))
	}
	# fworld <who>: the world= that game says: the sky's (Tamriel's, whichever side of a city's walls), or its place (rawsky)
	fworld() { if [ "$MODE" = rawsky ]; then g "$1" place; else echo 0000003C; fi; }
	# fsync: the VM follows the host's clock (forward to it, through midnight with a midnight step), timescale and sky as
	# MODE says
	fsync() {
		local m
		[ "$(g vm away)" = 1 ] && return 0
		[ "$MODE" = none ] && return 0
		inc vm heard
		m=$(clock_forward "$(g vm hour)" "$(g host hour)")
		if awk -v m="$m" 'BEGIN{exit !(m > 0.25 && m < 23.75)}'; then
			if [ "$MODE" = nodays ]; then s vm hour "$(g host hour)"; else
				[ "$(awk -v h="$(g vm hour)" -v m="$m" 'BEGIN{print (h + m >= 24)}')" = 1 ] && inc vm midnightSteps
				fmove vm "$m"
			fi
			inc vm hourSets; say vm "Clock: forward $(g vm hour) (player 1's clock)"
		fi
		if [ "$(g vm scale)" != "$(g host scale)" ]; then [ "$(g vm own)" = - ] && s vm own "$(g vm scale)"; s vm scale "$(g host scale)"; fi
		if [ "$MODE" != nosky ] && [ "$(fworld vm)" = "$(fworld host)" ] && [ "$(g vm weather)" != "$(g host weather)" ]; then
			inc vm eased; say vm "Clock: weather $(g vm weather) -> $(g host weather) easing in (player 1's sky)"; s vm weather "$(g host weather)"
		fi
		return 0
	}
	# fchange <who>: that game changed its own clock: the host's goes to the VM; the VM's own wait is undone, back within
	# the day (good), or moves the host (friendmoves)
	fchange() {
		local b from
		if [ "$1" = host ]; then fsync; return; fi
		if [ "$MODE" = friendmoves ]; then s host hour "$(g vm hour)"; inc host lineUps; return; fi
		[ "$MODE" = none ] && return 0
		b=$(clock_forward "$(g host hour)" "$(g vm hour)")
		if awk -v b="$b" 'BEGIN{exit !(b > 0.25 && b <= 12)}'; then
			from=$(g vm hour); fmove vm "-$b"; inc vm backSets; say vm "Clock: back $from -> $(g vm hour) (player 1's clock)"
		fi
		fsync
	}
	# fcast <who>: that game's player cast Clear Skies on itself: asked of the host, held there, both skies its (nomagic:
	# nothing asked); fdispel <who>: its effects dispelled: the host told, let go (norelease: never), the VM pinned again
	# (nopin: never)
	fcast() {
		[ "$MODE" = nomagic ] && return 0
		inc $1 magicWindows; inc $1 asks; s $1 magicAsked 0010D4FE; s $1 pinned 0
		s host asked 0010D4FE; inc host askSets; s host weather 0010D4FE; s $1 weather 0010D4FE
	}
	fdispel() {
		[ "$(g $1 magicAsked)" = 00000000 ] && return 0
		inc $1 askReleases; s $1 magicAsked 00000000
		[ "$MODE" = norelease ] || { s host asked 00000000; inc host askLetGo; }
		[ "$MODE" = nopin ] || s $1 pinned 1
	}
	# fwait <who>: the a of that game's Wait menu, as MENU and MODE say
	fwait() {
		local w=$1 v
		[ "$(g $w menu)" = 1 ] && [ "$MENU" != noa ] || return 0
		v=$(( 1 + $(g $w rights) )); s $w rights 0
		if [ "$MENU" = stuck ]; then fmove $w "$v"; return 0; fi
		if [ "$MODE" = underwait ] && [ $w = vm ]; then fmove vm 5; fchange vm; fmove vm "$v"; s vm menu 0; fchange vm; return 0; fi
		s $w menu 0; fmove $w "$v"
		say $w "Clock: our own sleep or wait ended at $(g $w hour): player 1's clock followed again from here"
		fchange $w
	}
	dc() {
		local w=$1 c=$2 v
		case "$c" in
			ping) echo pong ;;
			clock) echo "ready=1 opening=0 hour=$(g $w hour) days=$(g $w days) raw=$(g $w days) midnights=3 day=$(g $w day) month=8 scale=$(g $w scale) own=$(g $w own) outside=1 world=$(fworld $w) weather=$(g $w weather) override=0 owner=$([ $w = vm ] && [ "$(g vm heard)" -gt 0 ] && echo 1 || echo 0) heard=$(g $w heard) heardAge=1.0 ownerHour=- ownerWeather=00000000 offset=- moving=0.000 reports=9 lineUps=$(g $w lineUps) hourSets=$(g $w hourSets) backSets=$(g $w backSets) midnightSteps=$(g $w midnightSteps) forced=$(g $w forced) eased=$(g $w eased) held=$(g $w held) scaleSets=0 scaleBacks=$(g $w scaleBacks) place=$(g $w place) waiting=$(g $w menu) waits=0 mq101=stopped/completed/1000 canMove=1 pinned=$(g $w pinned) pins=1 pinReleases=0 saveUnpins=0 magic=0 magicWindows=$(g $w magicWindows) asks=$(g $w asks) askReleases=$(g $w askReleases) magicAsked=$(g $w magicAsked) asked=$(g $w asked) askedBy=$([ "$(g $w asked)" = 00000000 ] && echo 0 || echo 2) askSets=$(g $w askSets) askIgnored=0 askLetGo=$(g $w askLetGo) joinedDays=3.375" ;;
			"clock pick rain") echo "weather=0010A23F kind=rain of=12" ;;
			"clock pick snow") echo "weather=0010E1F2 kind=snow of=9" ;;
			"clock pick cloudy "*) if [ "$(g $w weather)" = 0010C001 ]; then echo "weather=0010C002 kind=cloudy of=10"; else echo "weather=0010C001 kind=cloudy of=10"; fi ;;
			"clock weather "*) s $w weather "${c##* }"; fchange $w; echo "weather=${c##* } forced override=0" ;;
			"clock release") echo "released=0 override=0" ;;
			"quest MQ101") echo "quest=MQ101 id=0003372B type=1 class=own enabled=0 completed=1 failed=0 current=1000" ;;
			"session status") echo "role=x connection=up welcomed=1 player=$([ $w = host ] && echo 1 || echo 2) host=1" ;;
			where) echo "cell=00000000 world=$(g $w place) pos=(1,2,3) heading=0 speed=0 menus=HUD_Menu$([ "$(g $w menu)" = 1 ] && echo ,Sleep/Wait_Menu) modal=0" ;;
			"pad button back") [ "$MENU" != 0 ] && s $w menu 1; echo "button back" ;;
			"pad button right") [ "$MENU" != noright ] && inc $w rights; echo "button right" ;;
			"pad button a") fwait $w; echo "button a" ;;
			"pad button b") s $w menu 0; s $w rights 0; echo "button b" ;;
			"console set gamehour to "*) v=${c##* }; s $w hour "$v"; fchange $w; echo ran ;;
			"console set timescale to "*) v=$(awk -v v="${c##* }" 'BEGIN{printf "%.2f\n", v}'); s $w scale "$v"; fchange $w; echo ran ;;
			"console player.cast "*) fcast $w; echo ran ;;
			"console player.dispelallspells") fdispel $w; echo ran ;;
			"console coc Whiterun") s $w place 0001A26F; fsync; echo ran ;;
			"console coc "*) s $w place 0000003C; fsync; echo ran ;;
			"console save "*)
				if [ "$(g vm own)" != - ]; then inc vm scaleBacks; say vm "Clock: timescale $(g vm scale) -> $(g vm own) (our own, for the save)"; fi
				if [ "$MODE" = savesession ] || [ "$(g vm own)" = - ]; then s vm saved "$(g vm scale)"; else s vm saved "$(g vm own)"; fi
				echo ran ;;
			"console load "*) s vm own -; s vm scale "$(g vm saved)"; inc vm lineUps; fsync; echo ran ;;
			"session leave") s vm away 1; [ "$(g vm own)" != - ] && { s vm scale "$(g vm own)"; s vm own -; }; echo left ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	log_since_mark() { grep -aE -- "$2" "$FK/$1.log"; }
	in_world() { :; }; welcomed() { :; }; joined_both() { :; }
	vm_join() { s vm away 0; inc vm lineUps; fsync; return 0; }
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init ct >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT|HEAL) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }
	count() { grep -cE "$2" <<<"$1"; }

	fake good; o=$(run 1 clock_follow_leg)
	t "follow: the host's wait through midnight (the hours asked), followed with the date and day count" "$(count "$o" '^(FAIL|RIG|WARN|HEAL) '),$(count "$o" '^PASS ')" 0,8
	fake nodays; o=$(run 2 clock_follow_leg)
	t "follow: hours followed without the day count fail the day count check" "$(count "$o" '^FAIL vm: its day count moved with its hours'),$(count "$o" '^FAIL vm: its date rolled over')" 1,1
	fake none; o=$(run 3 clock_follow_leg)
	t "follow: nothing followed fails the line-up" "$(count "$o" '^FAIL vm: lined up with the host')" 1
	fake good 0; o=$(run 4 clock_follow_leg)
	t "follow: a Wait menu that does not take the pad is healed by the console, and the leg still passes" "$(count "$o" '^HEAL '),$(count "$o" '^(FAIL|RIG) ')" 1,0
	fake good noright; o=$(run 5 clock_follow_leg)
	t "follow: a host's wait of another size than asked is the rig's input (RIG), never a FAIL of the mod" "$(count "$o" '^RIG +host: its wait made'),$(count "$o" '^FAIL ')" 1,0

	fake good; s host hour 1.5; s vm hour 1.5; o=$(run 6 clock_own_wait_leg)
	t "own wait: runs untouched, undone once after it, as big as asked, the host unmoved" "$(verdicts "$o")" "PASS=7 "
	fake friendmoves; s host hour 1.5; s vm hour 1.5; o=$(run 7 clock_own_wait_leg)
	t "own wait: the VM's wait moving the host fails" "$(count "$o" '^FAIL host: its clock did not move for the VM.s wait')" 1
	t "own wait: ... and no move back is a FAIL" "$(count "$o" '^FAIL vm: moved back within the day')" 1
	fake good stuck; s host hour 1.5; s vm hour 1.5; o=$(run 8 clock_own_wait_leg)
	t "own wait: a wait that ran and never shut its menu is a FAIL, not healed" "$(count "$o" '^FAIL vm: waits .*never shut'),$(count "$o" '^HEAL ')" 1,0
	fake good noa; s host hour 1.5; s vm hour 1.5; o=$(run 9 clock_own_wait_leg)
	t "own wait: a menu that took no wait (its hour never ran) is healed by the console" "$(verdicts "$o")" "HEAL=1 PASS=4 "
	fake underwait; s host hour 1.5; s vm hour 1.5; o=$(run 10 clock_own_wait_leg)
	t "own wait: pulled back while the wait ran fails (two moves back, no end of the wait said)" "$(count "$o" '^FAIL vm: moved back once'),$(count "$o" '^FAIL vm: its log says its own wait held')" 1,1
	fake good; s host hour 13.0; s vm hour 13.0; o=$(run 11 clock_own_wait_leg)
	t "own wait: a wait that would cross the VM's midnight is not made (RIG)" "$(verdicts "$o")" "RIG=1 "

	fake good; o=$(run 12 clock_scale_leg 20.00)
	t "scale: followed, kept, into the save, back on leaving, again after the rejoin and the load, its save its own" "$(count "$o" '^(FAIL|RIG|WARN) '),$(count "$o" '^PASS ')" 0,17
	fake none; o=$(run 13 clock_scale_leg 20.00)
	t "scale: not followed fails" "$(count "$o" '^FAIL vm: follows the host.s timescale')" 1
	fake savesession; o=$(run 14 clock_scale_leg 20.00)
	t "scale: a save that held the session's timescale fails, though its counter and log line passed" "$(count "$o" '^FAIL vm: its save held its own timescale'),$(count "$o" '^PASS vm: put its own timescale back for the save')" 1,1

	fake good; o=$(run 15 clock_sky_leg)
	t "sky: the host's weather is the VM's, held against its climate, the VM's own undone" "$(verdicts "$o")" "PASS=6 "
	fake nosky; o=$(run 16 clock_sky_leg)
	t "sky: a weather not followed fails" "$(count "$o" '^FAIL vm: its sky is the host.s')" 1
	fake nopin; o=$(run 16b clock_sky_leg)
	t "sky: a weather followed but not held against the VM's climate fails" "$(count "$o" '^FAIL vm: holds the host.s weather against its own climate'),$(count "$o" '^FAIL ')" 1,1

	fake good; o=$(run 27 clock_magic_leg)
	t "magic: the VM's spell asked, held by the host, both skies its, let go when dispelled, the VM pinned again" "$(verdicts "$o")" "PASS=7 "
	t "magic: ... the cast and the dispel are the console's, in the report" "$(count "$o" '^PASS vm: (its weather magic asked|tells the host)')" 2
	fake nomagic; o=$(run 28 clock_magic_leg)
	t "magic: a spell never asked of the host fails at once" "$(count "$o" '^FAIL vm: its weather magic asked of the host'),$(count "$o" '^FAIL ')" 1,1
	fake norelease; o=$(run 29 clock_magic_leg)
	t "magic: a host that never lets the VM's weather go fails" "$(count "$o" '^FAIL host: let the VM.s weather go'),$(count "$o" '^FAIL ')" 1,1
	fake nopin; o=$(run 30 clock_magic_leg)
	t "magic: a VM that never follows the host's sky again fails" "$(count "$o" '^FAIL vm: follows the host.s sky again, pinned'),$(count "$o" '^FAIL ')" 1,1
	fake good; dcfake=$(declare -f dc)
	dc() { case "$2" in "console player.cast "*) echo "error: console not ready" ;; *) _dc_fake "$@" ;; esac; }
	eval "_dc_fake ${dcfake#dc }"
	o=$(run 31 clock_magic_leg)
	t "magic: a cast the console refused is the rig broken, not a FAIL of the mod" "$(count "$o" '^FAIL .*rig broken \(input\)'),$(count "$o" '^PASS ')" 1,0
	eval "$dcfake"
	fake good; o=$(run 17 clock_city_leg host vm)
	t "city: the host inside the walls, the VM outside: one sky, the weather followed, the host back" "$(verdicts "$o")" "PASS=4 "
	fake good; o=$(run 18 clock_city_leg vm host)
	t "city: the VM inside the walls: the same" "$(verdicts "$o")" "PASS=4 "
	fake rawsky; o=$(run 19 clock_city_leg host vm)
	t "city: a walled city taken for its own sky fails both the sky and the weather" "$(count "$o" '^FAIL host: under the sky of the land'),$(count "$o" '^FAIL vm \(outside the walls, the host inside\): its sky')" 1,1
	fake rawsky; o=$(run 20 clock_city_leg vm host)
	t "city: ... and with the VM inside" "$(count "$o" '^FAIL vm: under the sky of the land'),$(count "$o" '^FAIL vm \(inside the walls\): its sky')" 1,1
	fake good; dcfake=$(declare -f dc)
	dc() { case "$2" in "console coc Whiterun") echo ran ;; *) _dc_fake "$@" ;; esac; }
	eval "_dc_fake ${dcfake#dc }"
	o=$(run 21 clock_city_leg host vm)
	t "city: a coc that did not leave the player in another worldspace is the rig's (RIG), not a FAIL" "$(count "$o" '^RIG +host: inside Whiterun'),$(count "$o" '^FAIL ')" 1,0
	eval "$dcfake"

	fake good; o=$(run 22 step_clock)
	t "step: all well, no FAIL, RIG or WARN" "$(count "$o" '^(FAIL|RIG|RIGFAULT|WARN) ')" 0
	t "step: ... the host followed nobody" "$(count "$o" '^PASS host: (lineUps|hourSets|backSets|midnightSteps)')" 4
	t "step: ... both city legs made, and no override left in either game" "$(count "$o" '^PASS (host|vm): inside Whiterun'),$(count "$o" '^PASS (host|vm): no weather override left')" 2,2
	t "step: ... what MQ101 reads is in the report" "$(count "$o" '^PASS (host|vm): its clock is ready: mq101=')" 2
	t "step: ... the weather magic leg made" "$(count "$o" '^PASS (host: let the VM.s weather go|vm: follows the host.s sky again, pinned)')" 2
	fake none; o=$(run 23 step_clock)
	t "step: with no clock sync it fails at once" "$(count "$o" '^FAIL vm: follows the host.s clock')" 1
	fake friendmoves; o=$(run 24 step_clock)
	t "step: a friend that moves the host fails the host's counts" "$( [ "$(count "$o" '^FAIL host: lineUps')" -ge 1 ] && echo yes)" yes

	# not ready: the opening is the rig's state, not the mod's FAIL; an error reply is the rig broken
	fake good; dcfake=$(declare -f dc)
	dc() { case "$2" in clock) echo "ready=0 opening=1 hour=9.000 mq101=running/open/5 canMove=0" ;; *) echo x ;; esac; }
	o=$(run 25 clock_status vm); t "status: in the opening is RIG" "$(verdicts "$o")" "RIG=1 "
	t "status: ... and says what MQ101 read" "$(count "$o" '^RIG .*mq101=running/open/5 canMove=0')" 1
	dc() { echo "error: no such command"; }
	o=$(run 26 clock_status vm); t "status: no clock command is the rig broken" "$(count "$o" '^FAIL .*rig broken \(game\)')" 1
	eval "$dcfake"

	# the step is wired into the harness: in STEPS after quests and before final, a session step, in the default run
	t "clock is a step, after quests, before final" "$(case " $STEPS " in *" quests "*"clock "*"final "*) echo yes;; esac)" yes
	t "clock needs a session" "$(case " $SESSION_STEPS " in *" clock "*) echo yes;; esac)" yes
	t "clock runs by default" "$(case " $OPTIN_STEPS " in *" clock "*) echo no;; *) echo yes;; esac)" yes
	t "clock has its line" "$([ -n "${STEP_INFO[clock]:-}" ] && echo yes)" yes
	t "clock is not run twice (its VM save and load)" "$(grep -qE 'RERUN_STEPS=.*clock' "$(dirname "${BASH_SOURCE[0]}")/../smoke.sh" && echo yes || echo no)" no
	[ $bad -eq 0 ] && echo "clock.sh selftest: all ok" || echo "clock.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
