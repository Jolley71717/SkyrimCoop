#!/bin/bash
# steps/traps.sh: the rig step `traps` (protocol 32, docs/DESIGN.md "Traps"), sourced by steps.sh. A one-shot trap one
# player sets off is spent in the other's world too; a bear trap one player snaps shut or sets open again is so in the
# other's; a game that loads a save, or joins again, is told the session's again, never its own; a trap's use is never
# made as a mechanism's. Not run in a game yet.
#
#   step_traps                                 the step (in the default run, after pickups, before jobsreset)
#   bash tools/rig/steps/traps.sh selftest     the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `traps` (counts), `traps pick <kind|oneshot|any> [armed|spent]
# [radius]` (the nearest trap of that kind of the load order, loaded, reading so, with its place), `traps ref <ref>` (how
# a trap reads: kind=, look= armed|going|fired|disarmed|busy|unknown, disabled=, and the session's word waiting on it) and
# `traps use <ref>` (our player's activation of it, as the activate key does, refused out of reach); `refs` gives the
# mechanism uses sent, made and refused; `where` our player's place):
#   1. God mode on in both games (`console tgm`, said; off again at the end, whatever happened): a trap that goes off
#      hits whoever walked into it, and a player killed mid-step would end the run.
#   2. Both players are put in each of TRAPS_CELLS in turn (coc: console placement, said) until each finds an armed
#      tripwire (or breakaway floor) that the other's world holds armed too.
#   3. A one-shot trap each way (host to VM, then VM to host): the walker walks up near it (walkto, real movement), then
#      on through it, TRAPS_PAST units past it (walkto x y: real movement, never placed). Its own world reads it spent and
#      told the session (sent= up). Within TRAPS_WAIT the other's world reads it spent too (gone off, or broken: disabled).
#   4. A taker's friend loads: the VM saves, the host sets one off, the VM loads its save (from before): the VM asks the
#      session again (asks= up) and the trap reads spent in its world again.
#   5. A taker's own load: the VM saves, sets one off, loads its save: the trap reads armed in its world again (the save's:
#      the VM's to set off again) and stays so for TRAPS_WAIT (the session never sends a player its own); the host's still
#      reads spent.
#   6. Joining again: the VM leaves, the host sets one off, the VM joins again: the trap reads spent in the VM's world (the
#      welcome's catch-up, catchUps= up).
#   7. A bear trap each way, in each of TRAPS_BEAR_CELLS in turn (coc) until both find one open: the user walks up to it
#      and activates it (snapped shut: disarmed, as a player does), and it reads shut in the other's world within
#      TRAPS_WAIT; it activates it again (set open), and it reads open in the other's. Neither use goes as a mechanism's
#      (refs uses= sent by the user, made by the other: unchanged): a trap's word says what became of it.
#   8. God mode off in both, both put back in the harness's cell (coc).
# One way only, by the rig's rules: legs 4, 5 and 6 save, load, leave or rejoin, which only the VM may do; the host is the
# other side of each.
# Not checked here (follow-ups): a trapped chest's or door's hinge (opening one needs a lock the rig can pick), a rigged
# rockfall set off by its tripwire's link (checked through the tripwire), a magic trap's soul gem, pressure plates and
# the traps that set themselves again (each game's own), rune traps.

TRAPS_CELLS=${TRAPS_CELLS:-"BleakFallsBarrow01 BleakFallsBarrow02 BleakFallsSanctum01"}  # tried in turn for an armed tripwire
TRAPS_BEAR_CELLS=${TRAPS_BEAR_CELLS:-"BleakFallsBarrow01 Riverwood"}                     # ... and for an open bear trap
TRAPS_WAIT=${TRAPS_WAIT:-20}      # s: a word is told, passed on and made (four batches a second, read four times a second)
TRAPS_RADIUS=${TRAPS_RADIUS:-4000}  # how far the walker looks for a trap
TRAPS_PAST=${TRAPS_PAST:-250}       # u: how far past a tripwire the walker walks on (its trigger is a few units across)
TRAPS_KINDS=${TRAPS_KINDS:-"tripwire breakaway"}  # one-shot kinds a walk through sets off, tried in turn

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# tr_found <pick reply>: the reference it names (empty: none, or an error)
tr_found() { local r; r=$(field ref "$1"); [[ $r =~ ^[0-9A-F]{8}$ ]] && echo "$r"; }
# tr_is <ref reply> <field> <value>: that reply is of a trap found there, and its field reads so
tr_is() { [[ $1 == ref=* ]] && [ "$(field found "$1")" = 1 ] && [ "$(field "$2" "$1")" = "$3" ]; }
# tr_spent_reply <ref reply>: a trap found there reads spent: gone off, going, disarmed, or broken away (disabled)
tr_spent_reply() { [[ $1 == ref=* ]] && [ "$(field found "$1")" = 1 ] && { [[ $(field look "$1") =~ ^(going|fired|disarmed)$ ]] || [ "$(field disabled "$1")" = 1 ]; }; }
# tr_armed_reply <ref reply>: a trap found there reads armed (set, in the world)
tr_armed_reply() { tr_is "$1" look armed && tr_is "$1" disabled 0; }
# tr_xy <reply with x= y=>: "x y"
tr_xy() { echo "$(field x "$1") $(field y "$1")"; }
# tr_pos <where reply>: our player's "x y" (pos=(x,y,z))
tr_pos() { sed -n 's/.*pos=(\([-0-9.]*\),\([-0-9.]*\),.*/\1 \2/p' <<<"$1"; }
# tr_past <px> <py> <tx> <ty> <d>: the point d units past the trap (tx,ty) on the line from the player (px,py) through it,
# as whole units; nothing when the two are less than a unit apart (no line)
tr_past() { awk -v px="$1" -v py="$2" -v tx="$3" -v ty="$4" -v d="$5" 'BEGIN { dx = tx - px; dy = ty - py; l = sqrt(dx * dx + dy * dy); if (l < 1) exit 1; printf "%d %d\n", tx + dx / l * d, ty + dy / l * d }'; }
# tr_uses <refs reply> <n>: the n-th number of uses= (1 sent, 2 made, 3 refused)
tr_uses() { field uses "$1" | cut -d/ -f"$2"; }

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

tr_count() { field "$2" "$(dc "$1" traps)"; }
tr_ref() { dc "$1" "traps ref $2"; }
tr_spent() { tr_spent_reply "$(tr_ref "$1" "$2")"; }
tr_armed() { tr_armed_reply "$(tr_ref "$1" "$2")"; }
tr_ge() { local n; n=$(tr_count "$1" "$2"); [ "$n" -ge "$3" ] 2>/dev/null; }
# tr_stays <s> <check...>: the check holds at every look over that time
tr_stays() { local i t=$1; shift; for ((i = 0; i < t; i++)); do "$@" || return 1; sleep 1; done; "$@"; }

# tr_god <on|off>: god mode toggled in both games (said); TR_GOD says whether it is on (the rig never leaves it on)
TR_GOD=0
tr_god() {
	local w r
	[ "$1" = on ] && [ "$TR_GOD" = 1 ] && return 0
	[ "$1" = off ] && [ "$TR_GOD" = 0 ] && return 0
	for w in host vm; do
		r=$(dc $w 'console tgm')
		[ "$r" = ran ] || { rig_broken input "$w: god mode $1 (console tgm)" "'console tgm' replied '$r'"; return 1; }
	done
	[ "$1" = on ] && TR_GOD=1 || TR_GOD=0
	healed "both: god mode $1 for the traps step (console tgm)" "a trap that goes off hits whoever walked into it"
}

# tr_pick <who> <sort> [armed|spent]: prints the nearest trap of that sort that game reads so, which the other's world
# holds too, of the same base and kind, reading so too; 1 when there is none
tr_pick() {
	local who=$1 sort=$2 want=${3:-armed} r ref base kind o
	o=$(other "$who")
	r=$(dc "$who" "traps pick $sort $want $TRAPS_RADIUS")
	ref=$(tr_found "$r") || return 1
	base=$(field base "$r"); kind=$(field kind "$r")
	r=$(tr_ref "$o" "$ref")
	tr_is "$r" base "$base" && tr_is "$r" kind "$kind" || return 1
	if [ "$want" = armed ]; then tr_armed_reply "$r" || return 1; else tr_spent_reply "$r" || return 1; fi
	echo "$ref"
}

# tr_walked <who>: waits for that game's walk to end (at most 25 s)
tr_walked() { local k st; for ((k = 0; k < 50; k++)); do st=$(dc "$1" walkto); [[ $st == "not walking"* ]] && break; sleep 0.5; done; still "$1"; sleep 1; }

# tr_approach <who> <ref> <radius>: that game's player walks up to the reference, to within radius (walkto: real movement)
tr_approach() {
	local r
	r=$(dc "$1" "walkto $2 0.3 $3 20")
	[[ $r == walking* ]] || { rig_fault input "$1: walks up to $2" "walkto replied '$r'"; return 1; }
	tr_walked "$1"
}

# tr_through <who> <ref>: that game's player walks on through the trap from where it stands, to TRAPS_PAST units past it
# (walkto x y: real movement, never placed)
tr_through() {
	local who=$1 ref=$2 p t to r
	p=$(tr_pos "$(dc "$who" where)")
	t=$(tr_xy "$(tr_ref "$who" "$ref")")
	# shellcheck disable=SC2086
	to=$(tr_past $p $t "$TRAPS_PAST") || { rig_fault state "$who: stands apart from $ref to walk through it" "player at '$p', trap at '$t'"; return 1; }
	r=$(dc "$who" "walkto $to 0.5 40 20")
	[[ $r == walking* ]] || { rig_fault input "$who: walks through $ref (to $to)" "walkto replied '$r'"; return 1; }
	tr_walked "$who"
}

# tr_oneshot <who>: that game's player walks through an armed one-shot trap (TRAPS_KINDS in turn); it reads spent there
# and told, and spent in the other's within TRAPS_WAIT. TR_REF: the trap (empty when none was set off).
TR_REF=""
tr_oneshot() {
	local who=$1 o ref="" kind s0
	o=$(other "$who"); TR_REF=""
	for kind in $TRAPS_KINDS; do ref=$(tr_pick "$who" "$kind") && break; done
	[ -n "$ref" ] || { rig_fault state "$who: finds an armed one-shot trap ($TRAPS_KINDS) that $o's world holds armed too" "$(dc "$who" "traps pick oneshot armed $TRAPS_RADIUS" | cut -c1-200)"; return 1; }
	s0=$(tr_count "$who" sent)
	tr_approach "$who" "$ref" 400 || return 1
	tr_through "$who" "$ref" || return 1
	if wait_until 10 tr_spent "$who" "$ref"; then pass "$who: walked through the $kind $ref and set it off" "$(tr_ref "$who" "$ref" | cut -c1-160)"
	else rig_fault walk "$who: walked through the $kind $ref and set it off" "$(tr_ref "$who" "$ref" | cut -c1-160); $(dc "$who" where | cut -c1-80)"; return 1; fi
	TR_REF=$ref
	if is_num "$s0" && wait_until 5 tr_ge "$who" sent $((s0 + 1)); then pass "$who: told the session the $kind $ref went off" "sent=$(tr_count "$who" sent)"
	else fail "$who: told the session the $kind $ref went off" "sent was '$s0': $(dc "$who" traps | cut -c1-300)"; fi
	if wait_until "$TRAPS_WAIT" tr_spent "$o" "$ref"; then pass "$o: the $kind $ref the other set off is spent in its world" "$(tr_ref "$o" "$ref" | cut -c1-160)"
	else fail "$o: the $kind $ref the other set off is spent in its world" "$(tr_ref "$o" "$ref" | cut -c1-160) | $(dc "$o" traps | cut -c1-300)"; fi
}

# tr_cell <check...>: both players put in each of TRAPS_CELLS in turn until the check holds; 1 when it never does
tr_cell() {
	local cell
	for cell in $TRAPS_CELLS; do
		doors_coc "$cell" || continue
		sleep 3
		"$@" && return 0
		warn "both: an armed one-shot trap each may walk through in $cell" "host: $(dc host "traps pick oneshot armed $TRAPS_RADIUS" | cut -c1-120)"
	done
	return 1
}
tr_both_have() { local k; for k in $TRAPS_KINDS; do tr_pick host "$k" >/dev/null && tr_pick vm "$k" >/dev/null && return 0; done; return 1; }
tr_host_has() { local k; for k in $TRAPS_KINDS; do tr_pick host "$k" >/dev/null && return 0; done; return 1; }
tr_vm_has() { local k; for k in $TRAPS_KINDS; do tr_pick vm "$k" >/dev/null && return 0; done; return 1; }

# tr_save / tr_load: the VM saves ($SAVE_NAME) / loads it and is back in the world (and in the session unless $1 is 0)
tr_save() {
	local r
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "vm: saves" "'console save' replied '$r'"; return 1; }
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return 1; }
}
tr_load() {
	local r
	r=$(dc vm "console load $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "vm: loads its save" "'console load' replied '$r'"; return 1; }
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return 1; }
	[ "${1:-1}" = 0 ] || wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return 1; }
}

# tr_friend_load: the VM saves, the host sets one off, the VM loads: the trap reads spent in its world again
tr_friend_load() {
	local ref q0
	tr_cell tr_host_has || { rig_fault state "host: an armed one-shot trap for the VM's load" "$TRAPS_CELLS"; return; }
	tr_save || return
	tr_oneshot host || return
	ref=$TR_REF
	q0=$(tr_count vm asks)
	tr_load || return
	if is_num "$q0" && wait_until 10 tr_ge vm asks $((q0 + 1)); then pass "vm: asked the session again after its load" "asks=$(tr_count vm asks)"
	else fail "vm: asked the session again after its load" "asks was '$q0': $(dc vm traps | cut -c1-300)"; fi
	if wait_until "$TRAPS_WAIT" tr_spent vm "$ref"; then pass "vm: the trap $ref the host set off is spent in its world again after its load" "$(tr_ref vm "$ref" | cut -c1-160)"
	else fail "vm: the trap $ref the host set off is spent in its world again after its load" "$(tr_ref vm "$ref" | cut -c1-160) | $(dc vm traps | cut -c1-300)"; fi
}

# tr_own_load: the VM saves, sets one off, loads: the trap reads armed in its world again and stays so (never sent its
# own); the host's still reads spent
tr_own_load() {
	local ref
	tr_cell tr_vm_has || { rig_fault state "vm: an armed one-shot trap for its own load" "$TRAPS_CELLS"; return; }
	tr_save || return
	tr_oneshot vm || return
	ref=$TR_REF
	tr_load || return
	if wait_until 15 tr_armed vm "$ref"; then pass "vm: the trap $ref is armed in its world again after loading a save from before it set it off" "$(tr_ref vm "$ref" | cut -c1-160)"
	else fail "vm: the trap $ref is armed in its world again after loading a save from before it set it off" "$(tr_ref vm "$ref" | cut -c1-160)"; return; fi
	if tr_stays "$TRAPS_WAIT" tr_armed vm "$ref"; then pass "vm: the trap $ref stays armed (${TRAPS_WAIT} s): the session never sends a player its own" ""
	else fail "vm: the trap $ref stays armed (${TRAPS_WAIT} s): the session never sends a player its own" "$(tr_ref vm "$ref" | cut -c1-160)"; fi
	if tr_spent host "$ref"; then pass "host: the trap $ref is still spent in its world" ""; else fail "host: the trap $ref is still spent in its world" "$(tr_ref host "$ref" | cut -c1-160)"; fi
}

# tr_rejoin: the VM leaves, the host sets one off, the VM joins again: the trap reads spent in its world (the catch-up)
tr_rejoin() {
	local ref r c0
	tr_cell tr_host_has || { rig_fault state "host: an armed one-shot trap while the VM is away" "$TRAPS_CELLS"; return; }
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	if ! tr_oneshot_alone host; then r=$(vm_join); wait_until 90 joined_both; return; fi
	ref=$TR_REF
	if tr_armed vm "$ref"; then pass "vm: the trap $ref is armed in its world while it is out of the session" ""; else fail "vm: the trap $ref is armed in its world while it is out of the session" "$(tr_ref vm "$ref" | cut -c1-160)"; fi
	c0=$(tr_count vm catchUps)
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	if wait_until "$TRAPS_WAIT" tr_spent vm "$ref"; then pass "vm: the trap $ref the host set off while it was away is spent once it is back" "$(tr_ref vm "$ref" | cut -c1-160)"
	else fail "vm: the trap $ref the host set off while it was away is spent once it is back" "$(tr_ref vm "$ref" | cut -c1-160) | $(dc vm traps | cut -c1-300)"; fi
	if is_num "$c0"; then expect_ge "vm: heard the session's catch-up with its welcome" "$(tr_count vm catchUps)" $((c0 + 1)); fi
}

# tr_oneshot_alone <who>: as tr_oneshot, with nobody else in the session (the other's world is not checked): TR_REF
tr_oneshot_alone() {
	local who=$1 ref="" kind r
	TR_REF=""
	for kind in $TRAPS_KINDS; do
		r=$(dc "$who" "traps pick $kind armed $TRAPS_RADIUS"); ref=$(tr_found "$r") && tr_armed vm "$ref" && break
		ref=""
	done
	[ -n "$ref" ] || { rig_fault state "$who: finds an armed one-shot trap the VM's world holds armed too" "$(dc "$who" "traps pick oneshot armed $TRAPS_RADIUS" | cut -c1-200)"; return 1; }
	tr_approach "$who" "$ref" 400 || return 1
	tr_through "$who" "$ref" || return 1
	if wait_until 10 tr_spent "$who" "$ref"; then pass "$who: walked through the $kind $ref and set it off" "$(tr_ref "$who" "$ref" | cut -c1-160)"
	else rig_fault walk "$who: walked through the $kind $ref and set it off" "$(tr_ref "$who" "$ref" | cut -c1-160)"; return 1; fi
	TR_REF=$ref
}

# tr_bear <who>: that game's player snaps an open bear trap shut (an activation) and sets it open again; the other's
# reads each within TRAPS_WAIT; neither use goes as a mechanism's
tr_bear() {
	local who=$1 o ref s0 u0 m0 r
	o=$(other "$who")
	ref=$(tr_pick "$who" bear armed) || { rig_fault state "$who: finds an open bear trap that $o's world holds open too" "$(dc "$who" "traps pick bear armed $TRAPS_RADIUS" | cut -c1-200)"; return 1; }
	s0=$(tr_count "$who" sent); u0=$(tr_uses "$(dc "$who" refs)" 1); m0=$(tr_uses "$(dc "$o" refs)" 2)
	tr_approach "$who" "$ref" 100 || return 1
	r=$(dc "$who" "traps use $ref")
	[ "$r" = activated ] || { rig_fault walk "$who: snaps the bear trap $ref shut within reach" "'traps use' replied '$r'"; return 1; }
	if wait_until 10 tr_spent "$who" "$ref"; then pass "$who: snapped the bear trap $ref shut" "$(tr_ref "$who" "$ref" | cut -c1-160)"
	else rig_fault input "$who: snapped the bear trap $ref shut" "$(tr_ref "$who" "$ref" | cut -c1-160)"; return 1; fi
	if is_num "$s0" && wait_until 5 tr_ge "$who" sent $((s0 + 1)); then pass "$who: told the session the bear trap $ref is shut" "sent=$(tr_count "$who" sent)"
	else fail "$who: told the session the bear trap $ref is shut" "sent was '$s0': $(dc "$who" traps | cut -c1-300)"; fi
	local oshut=0
	if wait_until "$TRAPS_WAIT" tr_spent "$o" "$ref"; then oshut=1; pass "$o: the bear trap $ref the other snapped is shut in its world" "$(tr_ref "$o" "$ref" | cut -c1-160)"
	else fail "$o: the bear trap $ref the other snapped is shut in its world" "$(tr_ref "$o" "$ref" | cut -c1-160) | $(dc "$o" traps | cut -c1-300)"; fi
	sleep 3  # past its busy state, and past the other's quiet time
	r=$(dc "$who" "traps use $ref")
	[ "$r" = activated ] || { rig_fault walk "$who: sets the bear trap $ref open within reach" "'traps use' replied '$r'"; return 1; }
	if wait_until 10 tr_armed "$who" "$ref"; then pass "$who: set the bear trap $ref open again" "$(tr_ref "$who" "$ref" | cut -c1-160)"
	else rig_fault input "$who: set the bear trap $ref open again" "$(tr_ref "$who" "$ref" | cut -c1-160)"; return 1; fi
	if [ "$oshut" = 0 ]; then :  # never shut there: its reading open now proves nothing
	elif wait_until "$TRAPS_WAIT" tr_armed "$o" "$ref"; then pass "$o: the bear trap $ref the other set is open in its world" "$(tr_ref "$o" "$ref" | cut -c1-160)"
	else fail "$o: the bear trap $ref the other set is open in its world" "$(tr_ref "$o" "$ref" | cut -c1-160) | $(dc "$o" traps | cut -c1-300)"; fi
	# A trap's use is its word's, never a mechanism's: the user sent no use, the other made none.
	if is_num "$u0" && is_num "$m0"; then
		r=$(dc "$who" refs)
		if [ "$(tr_uses "$r" 1)" = "$u0" ] && [ "$(tr_uses "$(dc "$o" refs)" 2)" = "$m0" ]; then pass "both: the bear trap's uses went as its word, never as a mechanism's use" "$who uses=$(field uses "$r")"
		else fail "both: the bear trap's uses went as its word, never as a mechanism's use" "$who uses=$(field uses "$r") (sent was $u0); $o uses=$(field uses "$(dc "$o" refs)") (made was $m0)"; fi
	fi
}

step_traps() {
	local cell found="" w
	section "traps: god mode on in both (a trap that goes off hits whoever walked into it)"
	tr_god on || return

	section "traps: a one-shot trap one player walks through is spent in the other's world"
	if tr_cell tr_both_have; then
		tr_oneshot host
		tr_oneshot vm

		section "traps: a save from before the other set one off, loaded, has it spent again (the VM loads: no load on the host)"
		tr_friend_load

		section "traps: a save from before our own, loaded, has it armed again, ours to set off (the VM loads)"
		tr_own_load

		section "traps: a player who joins again hears what went off while it was away (the VM rejoins)"
		tr_rejoin
	else
		rig_unhealed state "both: a cell of TRAPS_CELLS with an armed one-shot trap each may walk through" "$TRAPS_CELLS"
	fi

	section "traps: a bear trap one player snaps shut or sets open is so in the other's world"
	for cell in $TRAPS_BEAR_CELLS; do
		doors_coc "$cell" || continue
		sleep 3
		if tr_pick host bear >/dev/null && tr_pick vm bear >/dev/null; then found=$cell; break; fi
		warn "both: an open bear trap each may use in $cell" "host: $(dc host "traps pick bear armed $TRAPS_RADIUS" | cut -c1-120)"
	done
	if [ -n "$found" ]; then
		tr_bear host
		tr_bear vm
	else
		rig_unhealed state "both: a cell of TRAPS_BEAR_CELLS with an open bear trap" "$TRAPS_BEAR_CELLS"
	fi

	for w in host vm; do echo "     $w: $(dc $w traps | tr ' ' '\n' | grep -E '^(offmain|tracked|owned|waiting|made|verdicts|quieted|farToggles|dispatchFailed|refused|watchRefused|overflow)=' | tr '\n' ' ')"; done

	section "traps: god mode off, both back in $SMOKE_CELL"
	tr_god off
	doors_coc "$SMOKE_CELL"
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init trapstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	TRAPS_WAIT=2

	P='ref=0001A2B3 base=0006A4F5 kind=tripwire look=armed state=- runtime=0 loaded=1 x=100 y=200 z=3 dist=212'
	t "found: the reference a pick names" "$(tr_found "$P")" 0001A2B3
	t "found: none" "$(tr_found none || echo none)" none
	t "found: an error names none" "$(tr_found 'error: not in the world' || echo none)" none
	R='ref=0001A2B3 found=1 base=0006A4F5 kind=tripwire look=fired state=DoNothing disabled=0 destroyed=0 loaded=1 watched=1 settled=1 waiting=0 word=- attempts=0 dist=200'
	t "spent: gone off" "$(tr_spent_reply "$R" && echo yes)" yes
	t "spent: going" "$(tr_spent_reply "${R/look=fired/look=going}" && echo yes)" yes
	t "spent: disarmed" "$(tr_spent_reply "${R/look=fired/look=disarmed}" && echo yes)" yes
	t "spent: broken away (disabled)" "$(tr_spent_reply "$(sed 's/look=fired/look=armed/; s/disabled=0/disabled=1/' <<<"$R")" && echo yes)" yes
	t "spent: armed is not" "$(tr_spent_reply "${R/look=fired/look=armed}" || echo no)" no
	t "spent: not found is never so" "$(tr_spent_reply 'ref=0001A2B3 found=0 base=00000000 kind=none look=unknown disabled=0' || echo no)" no
	t "spent: an error is never so" "$(tr_spent_reply 'error: usage' || echo no)" no
	t "armed: armed and in the world" "$(tr_armed_reply "${R/look=fired/look=armed}" && echo yes)" yes
	t "armed: busy is not" "$(tr_armed_reply "${R/look=fired/look=busy}" || echo no)" no
	t "xy: a pick's place" "$(tr_xy "$P")" "100 200"
	t "pos: our player's place" "$(tr_pos 'cell=0000ABCD world=00000000 pos=(-10.5,20,3) heading=0 speed=0 menus=HUD_Menu modal=0')" "-10.5 20"
	t "past: 250 past the trap from the player" "$(tr_past 0 0 100 0 250)" "350 0"
	t "past: the other way" "$(tr_past 100 300 100 200 50)" "100 150"
	t "past: no line when on it" "$(tr_past 5 5 5 5 250 || echo none)" none
	t "uses: sent, made, refused" "$(tr_uses 'installed=1 uses=4/2/1 lastRefused=- lent=5' 1),$(tr_uses 'installed=1 uses=4/2/1 lastRefused=-' 2),$(tr_uses 'uses=4/2/1' 3)" 4,2,1

	# Both games faked: traps W1 (a tripwire) and B1 (a bear trap), each armed or spent in each world; SHARE=1 is the mod at
	# work (a trap set off, snapped or set is so in the other world), 0 a mod that shares nothing; MECH=1 a mod that also
	# sends a trap's use as a mechanism's (counted in refs). A walk to a point (walkto x y) sets off an armed tripwire in the
	# walker's world (it walked through it); DEAF=1: it does not (the walk missed it).
	fake() {
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; MECH=${2:-0}; F_DEAF=${DEAF:-0}
		for w in host vm; do echo armed > "$FK/$w.0006A001"; echo armed > "$FK/$w.0001B001"; for c in sent asks catchUps usent umade tgm; do echo 0 > "$FK/$w.$c"; done; done
	}
	get() { cat "$FK/$1.$2" 2>/dev/null || echo 0; }
	put() { echo "$3" > "$FK/$1.$2"; }
	up() { put "$1" "$2" $(( $(get "$1" "$2") + 1 )); }
	kindof() { case "$1" in 0006A*) echo tripwire ;; *) echo bear ;; esac; }
	baseof() { case "$1" in 0006A*) echo 0006A4F5 ;; *) echo 0001F2E1 ;; esac; }
	refline() {
		local s; s=$(get "$1" "$2")
		echo "ref=$2 found=1 base=$(baseof "$2") kind=$(kindof "$2") look=$s state=- disabled=0 destroyed=0 loaded=1 watched=1 settled=1 waiting=0 word=- attempts=0 x=100 y=200 z=0 dist=150"
	}
	pick() {  # pick <who> <sort> <want>
		local r; case "$2" in tripwire | oneshot | any) r=0006A001 ;; bear) r=0001B001 ;; *) echo none; return ;; esac
		local s; s=$(get "$1" "$r")
		if { [ "$3" = armed ] && [ "$s" = armed ]; } || { [ "$3" = spent ] && [ "$s" != armed ]; }; then echo "ref=$r base=$(baseof "$r") kind=$(kindof "$r") look=$s state=- runtime=0 loaded=1 x=100 y=200 z=0 dist=300"; else echo none; fi
	}
	fset() { put "$1" "$2" "$3"; up "$1" sent; [ "$MECH" = 1 ] && { up "$1" usent; up "$(other "$1")" umade; }; [ "$SHARE" = 1 ] && put "$(other "$1")" "$2" "$3"; }
	fwalk() { [ "$F_DEAF" = 1 ] && return; [ "$(get "$1" 0006A001)" = armed ] && fset "$1" 0006A001 fired; }
	fuse() { if [ "$(get "$1" "$2")" = armed ]; then fset "$1" "$2" fired; else fset "$1" "$2" armed; fi; echo activated; }
	dc() {
		local w=$1 c=$2 a; read -ra a <<<"$c"
		case "$c" in
			"traps pick "*) pick "$w" "${a[2]}" "${a[3]}" ;;
			"traps ref "*) refline "$w" "${a[2]}" ;;
			"traps use "*) fuse "$w" "${a[2]}" ;;
			traps) echo "installed=1 us=1 session=1 sent=$(get "$w" sent) asks=$(get "$w" asks) catchUps=$(get "$w" catchUps) offmain=0" ;;
			refs) echo "installed=1 uses=$(get "$w" usent)/$(get "$w" umade)/0 lastRefused=- asks=0" ;;
			"console tgm") up "$w" tgm; echo ran ;;
			"walkto stop") echo stopped ;;
			"walkto "*) if [[ ${a[1]} =~ ^[0-9A-F]{8}$ ]]; then echo "walking to ${a[1]}"; else echo "walking to ${a[1]} ${a[2]}"; fwalk "$w"; fi ;;
			walkto) echo "not walking" ;;
			"pad off") echo off ;;
			where) echo "cell=0000ABCD world=00000000 pos=(0,0,0) heading=0 speed=0 menus=HUD_Menu modal=0" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init tt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT|HEAL) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; o=$(run 1 eval 'tr_oneshot host; echo "ref=$TR_REF"')
	t "one-shot: walked through, set off, told, spent in the other's: passes" "$(verdicts "$o")" "PASS=3 "
	t "one-shot: ... its reference" "$(grep -o '^ref=.*' <<<"$o")" ref=0006A001
	t "one-shot: ... the states" "$(get host 0006A001),$(get vm 0006A001)" fired,fired
	fake 1; o=$(run 2 tr_oneshot vm)
	t "one-shot: the VM's way passes too" "$(verdicts "$o")" "PASS=3 "
	fake 0; o=$(run 3 tr_oneshot host)
	t "one-shot: a mod that shares nothing fails in the other's world" "$(grep -c '^FAIL vm: the tripwire 0006A001 the other set off is spent' <<<"$o")" 1
	fake 1; put vm 0006A001 fired; o=$(run 4 tr_oneshot host)
	t "one-shot: one the other's world holds spent already is no test (RIG), never FAIL" "$(verdicts "$o" | grep -c FAIL),$(grep -c '^RIG' <<<"$o")" 0,1
	DEAF=1 fake 1; o=$(run 5 tr_oneshot host)
	t "one-shot: a walk that missed the trap is the rig's (RIGFAULT), never FAIL" "$(verdicts "$o" | grep -c FAIL),$(grep -cE '^RIG +host: walked through' <<<"$o")" 0,1
	fake 1; o=$(run 6 tr_bear host)
	t "bear: snapped, told, shut in the other's, set open, open in the other's, no mechanism use: passes" "$(verdicts "$o")" "PASS=6 "
	t "bear: ... the states" "$(get host 0001B001),$(get vm 0001B001)" armed,armed
	fake 1; o=$(run 7 tr_bear vm)
	t "bear: the VM's way passes too" "$(verdicts "$o")" "PASS=6 "
	fake 0; o=$(run 8 tr_bear host)
	t "bear: a mod that shares nothing fails in the other's world (its open after proves nothing: not checked)" "$(grep -c '^FAIL vm: the bear trap 0001B001' <<<"$o"),$(grep -c '^PASS vm: the bear trap 0001B001 the other set is open' <<<"$o")" 1,0
	fake 1 1; o=$(run 9 tr_bear host)
	t "bear: a trap's use sent as a mechanism's fails" "$(grep -c "^FAIL both: the bear trap's uses went as its word" <<<"$o")" 1
	fake 1; o=$(run 10 eval 'tr_god on; tr_god on; tr_god off; tr_god off')
	t "god: toggled once on and once off in each game, said each time" "$(get host tgm),$(get vm tgm),$(grep -c '^HEAL ' <<<"$o")" 2,2,2

	# the step is wired into the harness: in STEPS before final, a session step, in the default run
	t "traps is a step, before final" "$(case " $STEPS " in *" traps "*"final "*) echo yes;; esac)" yes
	t "traps needs a session" "$(case " $SESSION_STEPS " in *" traps "*) echo yes;; esac)" yes
	t "traps runs by default" "$(case " $OPTIN_STEPS " in *" traps "*) echo no;; *) echo yes;; esac)" yes
	t "traps has its line" "$([ -n "${STEP_INFO[traps]:-}" ] && echo yes)" yes
	[ $bad -eq 0 ] && echo "traps.sh selftest: all ok" || echo "traps.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
