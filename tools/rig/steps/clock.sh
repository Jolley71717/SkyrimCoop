#!/bin/bash
# steps/clock.sh: the rig step `clock` (protocol 9, docs/DESIGN.md "Time and weather"), sourced by steps.sh. The host's
# clock is the session's: a wait of the host's is followed in the VM's game, through midnight, the day count moving with
# the hours; a wait of the VM's is undone and never moves the host; the host's timescale is the VM's while in the session
# and the VM's own goes into its save and comes back when it leaves; after a load and a rejoin the VM lines up again;
# outside, the host's weather is the VM's, and a weather the VM's console sets is undone. Not run in a game yet.
#
#   step_clock                                  the step (in the default run, after quests, before final)
#   bash tools/rig/steps/clock.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `clock` and `clock pick <kind>`; the clock= keys are
# WorldClock::Command's):
#   1. Both games' clocks are ready (in the world, not in the Helgen opening); the VM follows the host's (owner= the
#      host's player number, heard= going up); the host follows nobody (heard=0).
#   2. Both read the same hour (within CLOCK_SLACK) and timescale.
#   3. The host's wait through midnight: the host's hour is set to CLOCK_START (console `set gamehour`: the scene, said in
#      the report) and the VM lines up; then the host's player waits CLOCK_HOURS hours in the game's Wait menu (the pad:
#      back opens it, right adds an hour, a waits; when the menu does not take the pad, `set gamehour` stands in for it,
#      said as HEAL). The VM reads the host's hour again, its date rolled over (a midnight step), and its day count moved
#      with its hours (a game's day count and hour move together; a follower that moves one without the other breaks
#      every timer in its save).
#   4. The VM's wait: the VM's player waits CLOCK_HOURS hours in its Wait menu; within CLOCK_WAIT it is back at the
#      host's hour (a move back within the day), and the host's clock did not move (it follows nobody: no lineUps,
#      hourSets or backSets in the host's game, the whole step).
#   5. The timescale: the host sets CLOCK_SCALE (console `set timescale`); the VM reads it, its own kept (own=); the VM
#      sets another itself and is brought back to the host's; the VM saves (its own goes into the save: scaleBacks= up,
#      its log says so), leaves (its own back), joins again (the host's again, its hours lined up), loads its save (lined
#      up again); the host puts its own back and the VM follows.
#   6. Weather, when both stand outside in one worldspace (else a WARN: the step's scene is outside): `clock pick rain`
#      names the same weather in both; the host forces it (console `fw`); the VM's sky reads it within CLOCK_SKY_WAIT
#      (eased in or forced: eased= or forced= up, and its log). The VM forces a snowy one itself; it is undone within
#      CLOCK_SKY_WAIT and the host's sky stays (a weather the console set as an override is held as a script's: a WARN).
# The host never saves and never loads; its hour stays where the step left it (its save is not written), its timescale
# goes back to what it read at the start.

CLOCK_WAIT=${CLOCK_WAIT:-15}            # s: a clock to reach the other game and be followed (a report within a second of a change)
CLOCK_SKY_WAIT=${CLOCK_SKY_WAIT:-45}    # s: a weather eased in, forced after 30 s (ClockRules::WEATHER_PATIENCE)
CLOCK_SLACK=${CLOCK_SLACK:-0.1}         # game hours two clocks may differ by when read (6 game minutes, 18 real seconds at timescale 20)
CLOCK_DAYS_SLACK=${CLOCK_DAYS_SLACK:-0.1}  # game hours the day count may move apart from the hour over a follow
CLOCK_START=${CLOCK_START:-22.5}        # the host's hour before its wait, so a wait of CLOCK_HOURS crosses midnight
CLOCK_HOURS=${CLOCK_HOURS:-3}           # hours each wait asks for
CLOCK_SCALE=${CLOCK_SCALE:-40}          # the host's timescale for the timescale legs
CLOCK_VM_SCALE=${CLOCK_VM_SCALE:-7}     # the timescale the VM's console sets for itself

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
# clock_hex <reply> <key>: an 8-digit form ID the reply gives for that key, or empty
clock_hex() { local v; v=$(field "$2" "$1"); [[ $v =~ ^[0-9A-F]{8}$ ]] && echo "$v"; }

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

# clock_status <who>: that game's clock is ready: in the world, not in the opening
clock_status() {
	local s; s=$(clock_of "$1")
	if [[ $s != ready=* ]]; then rig_broken game "$1: its clock answers" "$(cut -c1-160 <<<"$s")"; return 1; fi
	if [ "$(field opening "$s")" = 1 ]; then rig_fault state "$1: out of the Helgen opening" "MQ101 runs in that game: its clock is its own until the opening ends"; return 1; fi
	if [ "$(field ready "$s")" != 1 ]; then rig_fault state "$1: its clock is ready (in the world)" "$(cut -c1-160 <<<"$s")"; return 1; fi
	pass "$1: its clock is ready" "$(cut -c1-120 <<<"$s")"
}

# clock_menu_wait <who> <hours>: that game's player waits in the game's Wait menu (the pad: back opens it, it starts at
# one hour, right adds one, a waits; the menu shuts itself when the wait is done). 0 when the menu opened and shut itself
# after a (the wait was made: a follower's own wait is undone within a second, so its hour cannot tell); 1 when the menu
# never opened or never shut (then shut with b).
_clock_menu() { [[ $(field menus "$(dc "$1" where)") == *Sleep/Wait* ]]; }
_clock_menu_shut() { ! _clock_menu "$1"; }
clock_menu_wait() {
	local w=$1 n=$2 i
	dc "$w" 'pad button back' >/dev/null
	wait_until 5 _clock_menu "$w" || return 1
	for ((i = 1; i < n; i++)); do dc "$w" 'pad button right' >/dev/null; sleep 0.4; done
	dc "$w" 'pad button a' >/dev/null
	wait_until 30 _clock_menu_shut "$w" && return 0
	dc "$w" 'pad button b' >/dev/null; sleep 1; return 1
}
# clock_wait <check> <who> <hours>: that game's player waits; by its Wait menu, or (HEAL) the console's set gamehour, the
# hour <hours> on past midnight, when the menu does not take the pad. 0 when the wait or its stand-in was made.
clock_wait() {
	local w=$2 n=$3 h0 to
	h0=$(clock_key "$w" hour)
	if clock_menu_wait "$w" "$n"; then pass "$1" "$w waited in its Wait menu: hour $h0 -> $(clock_key "$w" hour)"; return 0; fi
	to=$(awk -v h="$(clock_key "$w" hour)" -v n="$n" 'BEGIN{t = h + n; t -= 24 * int(t / 24); printf "%.2f\n", t}')
	healed "$1" "the Wait menu did not take the pad in the $w's game: its hour is set with the console instead (set gamehour to $to)"
	clock_console "$1: the console" "$w" "set gamehour to $to"
}

# clock_follow_leg: the host's wait through midnight, followed by the VM with its date and day count
clock_follow_leg() {
	local v0 v1 d0 d1 h0 h1 steps0 day0 lines0 fit
	clock_console "host: its hour to $CLOCK_START, the scene (console set gamehour)" host "set gamehour to $CLOCK_START" || return
	clock_expect_together "vm: lined up with the host's hour $CLOCK_START" || return
	v0=$(clock_of vm); d0=$(field days "$v0"); h0=$(field hour "$v0"); steps0=$(field midnightSteps "$v0"); day0=$(field day "$v0")
	lines0=$(clock_said vm 'Clock: forward')
	clock_wait "host: waits $CLOCK_HOURS hours, through midnight" host "$CLOCK_HOURS" || return
	clock_expect_together "vm: reads the host's hour after the host's wait" || return
	v1=$(clock_of vm); d1=$(field days "$v1"); h1=$(field hour "$v1")
	expect_ge "vm: stopped at midnight for its date to roll over (midnightSteps)" "$(field midnightSteps "$v1")" $(( ${steps0:-0} + 1 ))
	if [ "$(field day "$v1")" != "$day0" ]; then pass "vm: its date rolled over" "day $day0 -> $(field day "$v1")"; else fail "vm: its date rolled over" "day $day0 -> $(field day "$v1")"; fi
	fit=$(clock_days_fit "$d0" "$h0" "$d1" "$h1")
	if clock_le "$fit" "$CLOCK_DAYS_SLACK"; then pass "vm: its day count moved with its hours" "days $d0 -> $d1, hour $h0 -> $h1 (apart by $fit h)"
	else fail "vm: its day count moved with its hours" "days $d0 -> $d1, hour $h0 -> $h1: apart by ${fit:-?} h"; fi
	expect_ge "vm: its log says it moved forward by the host's clock" "$(clock_said vm 'Clock: forward')" $((lines0 + 1))
}

# clock_own_wait_leg: the VM's own wait is undone; the host's clock does not move
clock_own_wait_leg() {
	local h0 back0 hostHour lines0
	back0=$(clock_count vm backSets); lines0=$(clock_said vm 'Clock: back')
	hostHour=$(clock_key host hour)
	clock_wait "vm: waits $CLOCK_HOURS hours itself" vm "$CLOCK_HOURS" || return
	clock_expect_together "vm: brought back to the host's hour after its own wait" || return
	expect_ge "vm: moved back within the day (backSets)" "$(clock_count vm backSets)" $((back0 + 1))
	expect_ge "vm: its log says it moved back" "$(clock_said vm 'Clock: back')" $((lines0 + 1))
	h0=$(clock_forward "$hostHour" "$(clock_key host hour)")
	if clock_le "$h0" 0.25; then pass "host: its clock did not move for the VM's wait" "it ran $h0 h meanwhile"
	else fail "host: its clock did not move for the VM's wait" "it moved $h0 h"; fi
}

# clock_scale_leg <host's own timescale>: the host's timescale is the VM's; the VM's own goes into its save and comes back
# when it leaves; a rejoin and a load line the VM up again
clock_scale_leg() {
	local own=$1 vmOwn backs0 saved0 lines0 ups0 r
	vmOwn=$(clock_key vm own); [ "$vmOwn" = - ] && vmOwn=$(clock_key vm scale)  # its own: kept when it follows already
	clock_console "host: timescale $CLOCK_SCALE (console set timescale)" host "set timescale to $CLOCK_SCALE" || return
	clock_expect_scale "vm: follows the host's timescale $CLOCK_SCALE" vm "$CLOCK_SCALE"
	expect_eq "vm: keeps its own timescale to put back" "$(clock_key vm own)" "$(awk -v v="$vmOwn" 'BEGIN{printf "%.2f\n", v}')"
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

	clock_console "host: its own timescale back ($own)" host "set timescale to $own" || return
	clock_expect_scale "vm: follows the host's own timescale back" vm "$own"
}

# clock_sky_leg: the host's weather is the VM's outside; the VM's own is undone
clock_sky_leg() {
	local h v world rain snow forced0 eased0 lines0 vw
	h=$(clock_of host); v=$(clock_of vm)
	world=$(clock_hex "$h" world)
	if [ "$(field outside "$h")" != 1 ] || [ "$(field outside "$v")" != 1 ] || [ -z "$world" ] || [ "$world" != "$(clock_hex "$v" world)" ]; then
		warn "both: outside in one worldspace for the weather legs" "host outside=$(field outside "$h") world=$(field world "$h"), vm outside=$(field outside "$v") world=$(field world "$v"): the weather legs are not made"
		return
	fi
	rain=$(clock_hex "$(dc host 'clock pick rain')" weather)
	[ -n "$rain" ] || { rig_fault state "host: a rainy weather of the base game to force" "$(dc host 'clock pick rain' | cut -c1-120)"; return; }
	forced0=$(clock_count vm forced); eased0=$(clock_count vm eased); lines0=$(clock_said vm "Clock: weather .* -> $rain")
	clock_console "host: forces weather $rain (console fw)" host "fw $rain" || return
	wait_until 10 _clock_weather_is host "$rain" || { rig_fault input "host: its sky reads $rain after its fw" "weather=$(clock_key host weather)"; return; }
	if wait_until "$CLOCK_SKY_WAIT" _clock_weather_is vm "$rain"; then pass "vm: its sky is the host's ($rain)" "forced=$(clock_count vm forced) eased=$(clock_count vm eased)"
	else fail "vm: its sky is the host's ($rain)" "vm weather=$(clock_key vm weather) override=$(clock_key vm override) held=$(clock_count vm held)"; fi
	expect_ge "vm: eased in or forced the host's weather (eased + forced)" $(( $(clock_count vm forced) + $(clock_count vm eased) )) $((forced0 + eased0 + 1))
	expect_ge "vm: its log says the weather changed to the host's" "$(clock_said vm "Clock: weather .* -> $rain")" $((lines0 + 1))

	snow=$(clock_hex "$(dc vm 'clock pick snow')" weather)
	[ -n "$snow" ] || { rig_fault state "vm: a snowy weather of the base game to force" "$(dc vm 'clock pick snow' | cut -c1-120)"; return; }
	clock_console "vm: forces weather $snow itself (console fw)" vm "fw $snow" || return
	sleep 2
	vw=$(clock_of vm)
	if [ "$(field override "$vw")" = 1 ] && [ "$(field weather "$vw")" != "$rain" ]; then
		warn "vm: its own weather undone" "the console's fw is an override in this game: held as a script's (held=$(field held "$vw")); weather=$(field weather "$vw")"
	elif wait_until "$CLOCK_SKY_WAIT" _clock_weather_is vm "$rain"; then
		pass "vm: its own weather undone, the host's again" "weather=$rain"
	else
		fail "vm: its own weather undone, the host's again" "vm weather=$(clock_key vm weather), want $rain"
	fi
	expect_eq "host: its sky stays its own" "$(clock_key host weather)" "$rain"
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
	section "clock: the VM's own wait is undone, and the host's clock stays"
	clock_own_wait_leg
	section "clock: the host's timescale; the VM's own in its save and after it leaves; a rejoin and a load"
	clock_scale_leg "$own"
	section "clock: outside, the host's weather is the VM's"
	clock_sky_leg

	section "clock: the host followed nobody, the whole step"
	for w in lineUps hourSets backSets midnightSteps; do expect_zero "host: $w" "$(clock_count host $w)"; done
}
_clock_heard_from() { local s; s=$(clock_of "$1"); [ "$(field owner "$s")" = "$2" ] && is_num "$(field heard "$s")" && [ "$(field heard "$s")" -ge 1 ]; }

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init clocktest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; grep -E "^(FAIL|RIG|RIGFAULT|WARN) " <<<"${o:-}" | sed "s/^/       /"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	CLOCK_WAIT=2; CLOCK_SKY_WAIT=2

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
	t "hex" "$(clock_hex 'weather=0010A241 world=00000000' weather)" 0010A241
	t "hex: none" "$(clock_hex 'weather=-' weather)" ""

	# ---- both games faked ----
	# Each game has an hour, a day count, a date, a timescale, a weather and counts. A change in the host's game reaches the
	# VM's by MODE: good (the mod at work: the VM follows, day count with the hours, midnight steps, back within the day,
	# its own timescale kept and put back), none (nothing reaches the VM), nodays (the hours follow, the day count does
	# not), friendmoves (the VM's wait moves the host instead of being undone), nosky (the weather does not follow).
	# MENU: 1 the Wait menu takes the pad, 0 it does not (the console stands in).
	fake() {  # fake <mode> [menu]
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; MODE=$1; MENU=${2:-1}
		for w in host vm; do
			for k in lineUps hourSets backSets midnightSteps forced eased held scaleBacks heard menu away; do echo 0 > "$FK/$w.$k"; done
			echo 20.00 > "$FK/$w.scale"; echo - > "$FK/$w.own"; : > "$FK/$w.log"; echo 0010A242 > "$FK/$w.weather"
		done
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
		if [ "$MODE" != nosky ] && [ "$(g vm weather)" != "$(g host weather)" ]; then
			inc vm eased; say vm "Clock: weather $(g vm weather) -> $(g host weather) easing in (player 1's sky)"; s vm weather "$(g host weather)"
		fi
		return 0
	}
	# fchange <who>: that game changed its own clock: the host's goes to the VM; the VM's own wait is undone, back within
	# the day (good), or moves the host (friendmoves)
	fchange() {
		local b
		if [ "$1" = host ]; then fsync; return; fi
		if [ "$MODE" = friendmoves ]; then s host hour "$(g vm hour)"; inc host lineUps; return; fi
		[ "$MODE" = none ] && return 0
		b=$(clock_forward "$(g host hour)" "$(g vm hour)")
		if awk -v b="$b" 'BEGIN{exit !(b > 0.25 && b <= 12)}'; then fmove vm "-$b"; inc vm backSets; say vm "Clock: back $(g vm hour) (player 1's clock)"; fi
		fsync
	}
	dc() {
		local w=$1 c=$2 v
		case "$c" in
			ping) echo pong ;;
			clock) echo "ready=1 opening=0 hour=$(g $w hour) days=$(g $w days) raw=$(g $w days) midnights=3 day=$(g $w day) month=8 scale=$(g $w scale) own=$(g $w own) outside=1 world=0000003C weather=$(g $w weather) override=0 owner=$([ $w = vm ] && [ "$(g vm heard)" -gt 0 ] && echo 1 || echo 0) heard=$(g $w heard) heardAge=1.0 ownerHour=- ownerWeather=00000000 offset=- moving=0.000 reports=9 lineUps=$(g $w lineUps) hourSets=$(g $w hourSets) backSets=$(g $w backSets) midnightSteps=$(g $w midnightSteps) forced=$(g $w forced) eased=$(g $w eased) held=$(g $w held) scaleSets=0 scaleBacks=$(g $w scaleBacks)" ;;
			"clock pick rain") echo "weather=0010A23F kind=rain of=12" ;;
			"clock pick snow") echo "weather=0010E1F2 kind=snow of=9" ;;
			"session status") echo "role=x connection=up welcomed=1 player=$([ $w = host ] && echo 1 || echo 2) host=1" ;;
			where) echo "cell=00000000 world=0000003C pos=(1,2,3) heading=0 speed=0 menus=HUD_Menu$([ "$(g $w menu)" = 1 ] && echo ,Sleep/Wait_Menu) modal=0" ;;
			"pad button back") [ "$MENU" = 1 ] && s $w menu 1; echo "button back" ;;
			"pad button right") echo "button right" ;;
			"pad button a") if [ "$(g $w menu)" = 1 ]; then s $w menu 0; fmove $w "$CLOCK_HOURS"; fchange $w; fi; echo "button a" ;;
			"pad button b") s $w menu 0; echo "button b" ;;
			"console set gamehour to "*) v=${c##* }; s $w hour "$v"; fchange $w; echo ran ;;
			"console set timescale to "*) v=$(awk -v v="${c##* }" 'BEGIN{printf "%.2f\n", v}'); s $w scale "$v"; fchange $w; echo ran ;;
			"console fw "*) s $w weather "${c##* }"; fchange $w; echo ran ;;
			"console save "*) [ "$(g vm own)" != - ] && { inc vm scaleBacks; say vm "Clock: timescale $(g vm scale) -> $(g vm own) (our own, for the save)"; }; echo ran ;;
			"console load "*) inc vm lineUps; fsync; echo ran ;;
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
	t "follow: the host's wait through midnight, followed with the date and day count" "$(count "$o" '^(FAIL|RIG|WARN|HEAL) '),$(count "$o" '^PASS ')" 0,7
	fake nodays; o=$(run 2 clock_follow_leg)
	t "follow: hours followed without the day count fail the day count check" "$(count "$o" '^FAIL vm: its day count moved with its hours'),$(count "$o" '^FAIL vm: its date rolled over')" 1,1
	fake none; o=$(run 3 clock_follow_leg)
	t "follow: nothing followed fails the line-up" "$(count "$o" '^FAIL vm: lined up with the host')" 1
	fake good 0; o=$(run 4 clock_follow_leg)
	t "follow: a Wait menu that does not take the pad is healed by the console, and the leg still passes" "$(count "$o" '^HEAL '),$(count "$o" '^(FAIL|RIG) ')" 1,0

	fake good; s host hour 1.5; s vm hour 1.5; o=$(run 5 clock_own_wait_leg)
	t "own wait: undone, the host unmoved" "$(verdicts "$o")" "PASS=5 "
	fake friendmoves; s host hour 1.5; s vm hour 1.5; o=$(run 6 clock_own_wait_leg)
	t "own wait: the VM's wait moving the host fails" "$(count "$o" '^FAIL host: its clock did not move for the VM.s wait')" 1
	t "own wait: ... and no move back is a FAIL" "$(count "$o" '^FAIL vm: moved back within the day')" 1

	fake good; o=$(run 7 clock_scale_leg 20.00)
	t "scale: followed, kept, into the save, back on leaving, again after the rejoin and the load" "$(count "$o" '^(FAIL|RIG|WARN) '),$(count "$o" '^PASS ')" 0,16
	fake none; o=$(run 8 clock_scale_leg 20.00)
	t "scale: not followed fails" "$(count "$o" '^FAIL vm: follows the host.s timescale')" 1

	fake good; o=$(run 9 clock_sky_leg)
	t "sky: the host's weather is the VM's, the VM's own undone" "$(verdicts "$o")" "PASS=5 "
	fake nosky; o=$(run 10 clock_sky_leg)
	t "sky: a weather not followed fails" "$(count "$o" '^FAIL vm: its sky is the host.s')" 1

	fake good; o=$(run 11 step_clock)
	t "step: all well, no FAIL, RIG or WARN" "$(count "$o" '^(FAIL|RIG|RIGFAULT|WARN) ')" 0
	t "step: ... the host followed nobody" "$(count "$o" '^PASS host: (lineUps|hourSets|backSets|midnightSteps)')" 4
	fake none; o=$(run 12 step_clock)
	t "step: with no clock sync it fails at once" "$(count "$o" '^FAIL vm: follows the host.s clock')" 1
	fake friendmoves; o=$(run 13 step_clock)
	t "step: a friend that moves the host fails the host's counts" "$( [ "$(count "$o" '^FAIL host: lineUps')" -ge 1 ] && echo yes)" yes

	# not ready: the opening is the rig's state, not the mod's FAIL; an error reply is the rig broken
	fake good; dcfake=$(declare -f dc)
	dc() { case "$2" in clock) echo "ready=0 opening=1 hour=9.000" ;; *) echo x ;; esac; }
	o=$(run 14 clock_status vm); t "status: in the opening is RIG" "$(verdicts "$o")" "RIG=1 "
	dc() { echo "error: no such command"; }
	o=$(run 15 clock_status vm); t "status: no clock command is the rig broken" "$(count "$o" '^FAIL .*rig broken \(game\)')" 1
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
