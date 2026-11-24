#!/bin/bash
# steps/claws.sh: the rig step `claws` (protocol 16, docs/DESIGN.md "What a use did"), sourced by steps.sh. A mechanism
# whose script checks what its user carries (a claw door's keyhole, a keyhole, an altar) works for a friend: the items
# its scripts name that the user holds are lent to the user's stand-in in the other game while it makes the use, and
# taken back after. A mechanism whose script does nothing for a stand-in still does in the other game what it did in the
# user's: the animations it played and the references it enabled or disabled there are made in the other game. Not run
# in a game yet.
#
#   step_claws                                  the step (in the default run, after talk, before jobsreset)
#   bash tools/rig/steps/claws.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `refs near`, `ref <id>`, `use <id>`, `script <id>`,
# `anims <id>`, `enables <id>`, `refs` and `refs refuse <on|off>`, and steps/doors.sh's helpers):
#   1. Both players are put in each of CLAWS_CELLS in turn (coc: console placement, said) until the VM lists a mechanism
#      within CLAWS_NEAR whose scripts name an item (`wants=` on its line: the items a use of it would lend).
#   2. The VM's player is given the first item it wants (console `player.additem`: set-up) and put at it (console
#      `player.moveto`: placement to reach it, said); the host's stand-in of the VM must come within the use's reach. The
#      VM uses it: its use lends the item (`lent=` sent up), the host's stand-in makes the use (`uses=` made up) holding
#      the item (`lent=` here up). After CLAWS_SETTLE (the 10 s watch, the 5 s match of a use made there, and slack):
#      what the VM's use did (`effects=` captured) reached the host (`mirror=` heard up by as many), and the host's
#      stand-in's run, holding the item, did all of it itself: `mirror=` made unchanged, matched and already up by as many
#      (a lend that did nothing fails here, as the host would have made what its run missed); no animation played twice
#      on the host (`anims`: one made there never followed by the same event since the use; the host's own late repeat
#      held back is a WARN, `mirror=` doubled); every item lent is back (`lent=` out 0); and what it did reads the same
#      in both: the mechanism's last animation (`anims`), the references disabled near it (`enables`), its script and the
#      open states near it (as steps/doors.sh compares them). The item given is taken back (`player.removeitem`). A use
#      that changed nothing the rig reads proves nothing: the next mechanism listed is tried (a claw door whose rings are
#      not set does nothing; a plain keyhole does), up to CLAWS_TRIES, and `RIG` when none did.
#   3. The reverse, the host using another such mechanism (the one the VM's use changed again when there is none, a
#      WARN), and the VM saving right after its stand-in made the use, while it lends: its log says the items lent were
#      taken back before the save, and none is out after it.
#   4. A script that does nothing for a stand-in: both put in DOORS_MECH_CELL, at its DOORS_MECH_NAME (the pillar). The
#      watcher refuses friends' uses (`refs refuse on`, the plugin's stand-in for such a script); the user uses it: the
#      watcher refused it (`uses=` refused up), and when the user's game read what it did, the watcher made it
#      (`mirror=` made up) and the pillar's last animation reads the same in both. Both ways (the watcher's own run made
#      nothing, so its script is one turn behind until the other way; after both, a script that differs is a WARN),
#      then `refs refuse off`.
#   5. Both are put back in the harness's cell (coc).

CLAWS_CELLS=${CLAWS_CELLS:-BleakFallsSanctum01 BleakFallsBarrow02}  # cells searched in turn for a mechanism that wants an item
CLAWS_NEAR=${CLAWS_NEAR:-20000}      # u: the mechanisms listed, from the cell's coc marker (the most refs near takes)
CLAWS_WAIT=${CLAWS_WAIT:-10}         # s: for the use to reach the other game and its stand-in to make it
CLAWS_SETTLE=${CLAWS_SETTLE:-18}     # s: after that, for the watch (UseRules::WATCH_FOR, 10 s), the match (5 s for a use made there) and the loans (10 s) to end
CLAWS_TRIES=${CLAWS_TRIES:-3}        # mechanisms tried, each way, until a use changes something the rig reads

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# claws_wanting <refs near reply>: "id item" for each loaded reference whose scripts name an item (its first), nearest first
claws_wanting() {
	local line w
	while IFS= read -r line; do
		[ "$(field loaded "$line")" = 1 ] || continue
		w=$(field wants "$line")
		[ -n "$w" ] && [ "$w" != - ] || continue
		echo "$(field ref "$line") ${w%%,*}"
	done < <(doors_lines "$1")
	return 0
}
# claws_part <refs reply> <uses|lent|effects|mirror> <n>: the n-th count of that field (uses=sent/made/refused,
# lent=sent/kept/here/back/gone/out/moved/undone, effects=captured/cut/batches/early,
# mirror=heard/matched/made/already/unloaded/doubled/waiting/kind)
claws_part() { field "$2" "$1" | cut -d/ -f"$3"; }
# claws_num: as claws_part, 0 when it is not a number (for sums; the counts were checked to be there first)
claws_num() { local v; v=$(claws_part "$@"); if is_num "$v"; then echo "$v"; else echo 0; fi; }
# claws_twice <anims reply> [n]: how many animations made there for a friend's use (*Event) were followed by the same
# event sent by the game itself, among the events listed after the first n (those since the leg began)
claws_twice() {
	local ev twice=0 i j skip=${2:-0}
	local -a e
	ev=$(field events "$1")
	if [ -z "$ev" ] || [ "$ev" = - ]; then echo 0; return 0; fi
	IFS=, read -ra e <<<"$ev"
	for ((i = skip; i < ${#e[@]}; i++)); do
		[ "${e[i]:0:1}" = '*' ] || continue
		for ((j = i + 1; j < ${#e[@]}; j++)); do
			if [ "${e[j]}" = "${e[i]:1}" ]; then twice=$((twice + 1)); break; fi
		done
	done
	echo "$twice"
}

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

# _claws_reached <watcher> <its refs before> <mode>: the friend's use reached it (made by its stand-in, or refused)
_claws_reached() {
	local now n; now=$(dc "$1" refs)
	n=2; [ "$3" = refuse ] && n=3
	[ "$(claws_num "$now" uses $n)" -gt "$(claws_num "$2" uses $n)" ]
}

# claws_save <watcher> <its refs now>: the VM saves while it lends its stand-in an item: the items are back before the
# save (its log says so) and none is out after it
claws_save() {
	local w=$1 s0
	[ "$w" = vm ] || return 0  # the host never saves
	if [ "$(claws_num "$2" lent 6)" -lt 1 ]; then
		rig_fault state "$w: saves while it lends its stand-in an item" "nothing out when the save began (lent=$(field lent "$2"))"; return
	fi
	s0=$(log_since_mark "$w" 'taken back before the save' | wc -l | tr -d ' ')
	doors_run "$w: saves while it lends its stand-in an item" "$w" "console save $SAVE_NAME" ran || return
	sleep 8
	[ "$(dc "$w" ping)" = pong ] || { fail "$w: the game lives through its save" "its dev channel went silent"; return; }
	expect_ge "$w: took back what it lent before the save (its log says so)" "$(log_since_mark "$w" 'taken back before the save' | wc -l | tr -d ' ')" $((s0 + 1))
	expect_eq "$w: nothing lent out after the save" "$(claws_num "$(dc "$w" refs)" lent 6)" 0
}

# _claws_use <user> <watcher> <id> <item|-> <what> <mode> <last>: the use and its checks (claws_use sets up and tidies).
# CLAWS_CHANGED: 1 when the use changed something the rig reads. One that changed nothing is a WARN when another
# mechanism is tried after it (last 0), a RIG when not (last 1).
_claws_use() {
	local user=$1 watcher=$2 id=$3 item=$4 what=$5 mode=$6 last=${7:-1} u0 w0 u1 w1 n captured here a0 own
	local -a ids
	CLAWS_CHANGED=0
	doors_run "$user: put at the $what $id (console player.moveto: placement)" "$user" "console player.moveto $id" ran || return
	sleep 3
	wait_until 20 doors_near "$watcher" "$id" ||
		{ rig_fault state "$watcher: its stand-in of the $user comes within reach of the $what" "stand-in at ($(standin_at "$watcher")), the $what: $(dc "$watcher" "ref $id" | cut -c1-160)"; return; }
	# shellcheck disable=SC2207
	ids=($(doors_opens "$(dc "$user" "refs near $DOORS_EFFECT_NEAR")"))
	E_S0U=$(dc "$user" "script $id"); E_S0W=$(dc "$watcher" "script $id")
	E_O0U=$(doors_states "$user" ${ids[@]+"${ids[@]}"}); E_O0W=$(doors_states "$watcher" ${ids[@]+"${ids[@]}"})
	C_N0U=$(field ids "$(dc "$user" "enables $id")"); C_N0W=$(field ids "$(dc "$watcher" "enables $id")")
	a0=$(field played "$(dc "$watcher" "anims $id")"); is_num "$a0" || a0=0
	u0=$(dc "$user" refs); w0=$(dc "$watcher" refs)
	for n in "$(claws_part "$u0" lent 1)" "$(claws_part "$u0" effects 1)" "$(claws_part "$w0" lent 3)" "$(claws_part "$w0" mirror 1)" "$(claws_part "$w0" uses 2)"; do
		is_num "$n" || { fail "both: count what uses lend and did (refs: lent=, effects=, mirror=)" "$user $(cut -c1-200 <<<"$u0"); $watcher $(cut -c1-200 <<<"$w0")"; return; }
	done

	doors_run "$user: uses the $what $id" "$user" "use $id" used || return
	sleep 2
	u1=$(dc "$user" refs)
	[ "$item" = - ] || expect_ge "$user: its use lends what it holds of the items the $what's scripts name" "$(claws_part "$u1" lent 1)" $(($(claws_num "$u0" lent 1) + 1))
	wait_until "$CLAWS_WAIT" _claws_reached "$watcher" "$w0" "$mode"
	w1=$(dc "$watcher" refs)
	if [ "$mode" = refuse ]; then
		expect_ge "$watcher: refused the $user's use (refs refuse on)" "$(claws_part "$w1" uses 3)" $(($(claws_num "$w0" uses 3) + 1))
	else
		expect_ge "$watcher: its stand-in of the $user uses the $what too" "$(claws_part "$w1" uses 2)" $(($(claws_num "$w0" uses 2) + 1))
		[ "$item" = - ] || expect_ge "$watcher: lends its stand-in of the $user the item for the use" "$(claws_part "$w1" lent 3)" $(($(claws_num "$w0" lent 3) + 1))
	fi
	[ "$mode" != save ] || claws_save "$watcher" "$w1"

	sleep "$CLAWS_SETTLE"
	u1=$(dc "$user" refs); w1=$(dc "$watcher" refs)
	captured=$(($(claws_num "$u1" effects 1) - $(claws_num "$u0" effects 1)))
	if [ "$captured" -lt 1 ]; then
		# a use that changed nothing proves nothing: not the lend (a stand-in's run would change nothing either), nor
		# what is made (nothing to make)
		if [ "$last" = 1 ]; then
			rig_fault state "$user: its use of the $what changed something the rig reads (an animation near it, a reference enabled or disabled)" "none captured (hooked=$(field hooked "$u1"))"
		else
			warn "$user: its use of the $what changed something the rig reads; the next mechanism is tried" "none captured (hooked=$(field hooked "$u1"))"
		fi
	else
		CLAWS_CHANGED=1
		pass "$user: says what its use of the $what did" "$captured effect(s) in $(($(claws_num "$u1" effects 3) - $(claws_num "$u0" effects 3))) batch(es), $(($(claws_num "$u1" effects 4) - $(claws_num "$u0" effects 4))) read before its tick"
		expect_ge "$watcher: heard what the $user's use did" "$(claws_part "$w1" mirror 1)" $(($(claws_num "$w0" mirror 1) + captured))
		if [ "$mode" = refuse ]; then
			expect_ge "$watcher: made what the $user's use did, its stand-in having made nothing" "$(claws_part "$w1" mirror 3)" $(($(claws_num "$w0" mirror 3) + 1))
		else
			# the stand-in's run, holding what was lent, did it all: had the lend done nothing, the watcher would have
			# made what its run missed
			expect_eq "$watcher: its stand-in's run of the $user's use did all of it itself (mirror= made unchanged)" "$(claws_part "$w1" mirror 3)" "$(claws_part "$w0" mirror 3)"
			own=$(($(claws_num "$w1" mirror 2) + $(claws_num "$w1" mirror 4) - $(claws_num "$w0" mirror 2) - $(claws_num "$w0" mirror 4)))
			expect_ge "$watcher: its own run matched what the $user's use did (mirror= matched and already up)" "$own" "$captured"
		fi
	fi
	echo "     $watcher: mirror=$(field mirror "$w1") (heard/matched/made/already/unloaded/doubled/waiting/kind) lent=$(field lent "$w1")"
	if [ "$(claws_num "$w1" mirror 6)" -gt "$(claws_num "$w0" mirror 6)" ]; then
		warn "$watcher: its own run sent no animation it had made already (a late run: its repeat held back)" "mirror= doubled $(claws_part "$w0" mirror 6) -> $(claws_part "$w1" mirror 6)"
	fi
	expect_eq "$watcher: no animation played twice on the $what for the $user's use (anims: a made one followed by the game's own)" "$(claws_twice "$(dc "$watcher" "anims $id")" "$a0")" 0
	here=$(($(claws_num "$w1" lent 3) - $(claws_num "$w0" lent 3)))
	[ "$here" -lt 1 ] || expect_eq "$watcher: every item lent to its stand-in taken back (none out ${CLAWS_SETTLE} s after)" "$(claws_part "$w1" lent 6)" 0

	# what it did, read in both games
	C_A1U=$(field last "$(dc "$user" "anims $id")"); C_A1W=$(field last "$(dc "$watcher" "anims $id")")
	if [ -n "$C_A1U" ] && [ "$C_A1U" != - ]; then expect_eq "$watcher: the $what's last animation reads as the $user's" "$C_A1W" "$C_A1U"; fi
	C_N1U=$(field ids "$(dc "$user" "enables $id")"); C_N1W=$(field ids "$(dc "$watcher" "enables $id")")
	if [ "$C_N0U" != "$C_N0W" ]; then
		rig_fault state "both: read the same references disabled near the $what before the $user's use" "$user [$C_N0U] $watcher [$C_N0W]"
	elif [ "$C_N1U" != "$C_N0U" ] || [ "$C_N1W" != "$C_N0W" ]; then
		expect_eq "$watcher: the references disabled near the $what read as the $user's" "$C_N1W" "$C_N1U"
	fi
	if [ "$mode" != refuse ]; then
		E_S1U=$(dc "$user" "script $id"); E_S1W=$(dc "$watcher" "script $id")
		E_O1U=$(doors_states "$user" ${ids[@]+"${ids[@]}"}); E_O1W=$(doors_states "$watcher" ${ids[@]+"${ids[@]}"})
		doors_effect "$user" "$watcher" "$what $id"
	fi
}

# claws_use <user> <id> <item|-> <what> [refuse|save|-] [last]: the user, given the item (when one), uses the mechanism;
# refuse: the watcher refuses friends' uses meanwhile; save: the watcher (the VM) saves while it lends; last 0: another
# mechanism is tried when this use changes nothing (CLAWS_CHANGED says whether it did)
claws_use() {
	local user=$1 id=$2 item=$3 what=$4 mode=${5:-} last=${6:-1} watcher
	[ "$mode" != - ] || mode=
	CLAWS_CHANGED=0
	watcher=$(other "$user")
	if [ "$item" != - ]; then
		doors_run "$user: given the item the $what's scripts name ($item; console player.additem: set-up)" "$user" "console player.additem $item 1" ran || return
	fi
	if [ "$mode" != refuse ] || doors_run "$watcher: refuses friends' uses (refs refuse on: a script that does nothing for a stand-in)" "$watcher" "refs refuse on" refuse=1; then
		_claws_use "$user" "$watcher" "$id" "$item" "$what" "$mode" "$last"
	fi
	[ "$mode" != refuse ] || doors_run "$watcher: makes friends' uses again (refs refuse off)" "$watcher" "refs refuse off" refuse=0
	[ "$item" = - ] || doors_run "$user: the item given taken back (console player.removeitem: set-up)" "$user" "console player.removeitem $item 1" ran
	return 0
}

# claws_try <user> <mode> <entry>...: each "id item" in turn (at most CLAWS_TRIES) until the user's use of one changes
# something the rig reads; CLAWS_DID: the entry whose use did, empty when none did
claws_try() {
	local user=$1 mode=$2 i n last id item
	shift 2
	n=$#; [ "$n" -le "$CLAWS_TRIES" ] || n=$CLAWS_TRIES
	CLAWS_DID=
	for ((i = 1; i <= n; i++)); do
		read -r id item <<<"${!i}"
		last=0; [ "$i" -lt "$n" ] || last=1
		claws_use "$user" "$id" "$item" mechanism "${mode:--}" "$last"
		if [ "$CLAWS_CHANGED" = 1 ]; then CLAWS_DID=${!i}; return 0; fi
	done
	return 0
}

step_claws() {
	local cell list id item r s did e
	local -a found=() rest=()
	section "claws: a mechanism whose script checks its user's items works for a friend (the items lent to their stand-in)"
	for cell in $CLAWS_CELLS; do
		doors_coc "$cell" || return
		sleep 3
		list=$(dc vm "refs near $CLAWS_NEAR")
		[[ $list == near=* ]] || { rig_broken game "vm: lists the mechanisms near it" "$(cut -c1-160 <<<"$list")"; return; }
		mapfile -t found < <(claws_wanting "$list")
		[ ${#found[@]} -gt 0 ] && break
	done
	if [ ${#found[@]} -eq 0 ]; then
		rig_unhealed state "vm: a mechanism whose scripts name an item (wants=)" "none within $CLAWS_NEAR u in $CLAWS_CELLS: $(doors_kinds "$list" | cut -c1-200)"
	else
		echo "     vm: wanting items: ${found[*]}"
		claws_try vm - "${found[@]}"
		did=$CLAWS_DID
		if [ -n "$did" ]; then
			section "claws: the reverse, the VM saving while it lends"
			rest=()
			for e in "${found[@]}"; do [ "$e" = "$did" ] || rest+=("$e"); done
			if [ ${#rest[@]} -eq 0 ]; then
				warn "vm: a second mechanism whose scripts name an item, for the reverse" "one only that does something in $cell: it again (its use may change nothing now)"
			fi
			claws_try host save ${rest[@]+"${rest[@]}"} "$did"
		fi
	fi

	section "claws: a script that does nothing for a stand-in: what the use did is made in the other game"
	if doors_coc "$DOORS_MECH_CELL"; then
		list=$(dc vm "refs near $DOORS_MECH_NEAR")
		if id=$(doors_pick "$list" mechanism "$DOORS_MECH_NAME"); then
			claws_use vm "$id" - "$DOORS_MECH_NAME" refuse
			claws_use host "$id" - "$DOORS_MECH_NAME" refuse
			r=$(dc host "script $id"); s=$(dc vm "script $id")
			if [ "$r" = "$s" ]; then pass "both: the $DOORS_MECH_NAME's script reads the same after one refused use each way" "$(cut -c1-120 <<<"$r")"
			else warn "both: the $DOORS_MECH_NAME's script reads the same after one refused use each way" "host [$(cut -c1-120 <<<"$r")] vm [$(cut -c1-120 <<<"$s")]"; fi
		else
			rig_unhealed state "vm: a mechanism named $DOORS_MECH_NAME near it" "none within $DOORS_MECH_NEAR u in $DOORS_MECH_CELL: $(doors_kinds "$list" | cut -c1-200)"
		fi
	fi

	section "claws: both back in $SMOKE_CELL"
	doors_coc "$SMOKE_CELL"
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init clawstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	W1='ref=000C0005 kind=mechanism open=- locked=0 level=- loaded=1 known=-/-/- dist=300 at=(1,2,3) enabled=1 wants=000999B3,00012345 name=Claw_Door'
	W2='ref=000C0006 kind=mechanism open=- locked=0 level=- loaded=0 known=-/-/- dist=400 at=(1,2,3) enabled=1 wants=000999B4 name=Keyhole'
	W3='ref=000C0007 kind=mechanism open=- locked=0 level=- loaded=1 known=-/-/- dist=500 at=(1,2,3) enabled=1 wants=- name=Lever'
	W4='ref=000C0008 kind=activator open=0 locked=0 level=- loaded=1 known=-/-/- dist=600 at=(1,2,3) enabled=1 wants=0001AAAA name=Altar'
	t "wanting: the loaded ones that name an item, each with its first" "$(claws_wanting "near=4 | $W1 | $W2 | $W3 | $W4" | tr '\n' ' ')" "000C0005 000999B3 000C0008 0001AAAA "
	t "wanting: none from a build with no wants=" "$(claws_wanting 'near=1 | ref=000C0005 kind=mechanism loaded=1 name=Claw_Door' | wc -l | tr -d ' ')" 0
	R='installed=1 uses=4/2/1 lastRefused=- lent=5/1/3/2/1/0 effects=7/0/2 mirror=9/4/5/0/0/1/0 notes=0 refuse=0 hooked=1'
	t "part: lent here" "$(claws_part "$R" lent 3)" 3
	t "part: lent out" "$(claws_part "$R" lent 6)" 0
	t "part: effects captured" "$(claws_part "$R" effects 1)" 7
	t "part: mirror doubled" "$(claws_part "$R" mirror 6)" 1
	t "part: uses refused" "$(claws_part "$R" uses 3)" 1
	t "num: an error reply is 0" "$(claws_num 'error: usage: refs' lent 1)" 0

	# Both games faked: F_USER uses the claw door 000C0005; the other watches. F_* say what the mod did once it is used.
	S0='ref=000C0005 scripts=1 | ClawDoor state=- ::done_var=0'; S1='ref=000C0005 scripts=1 | ClawDoor state=- ::done_var=1'
	cdef() {
		F_USER=vm F_SENT=1 F_UMADE=1 F_REF=0 F_HERE=1 F_OUT=0 F_CAPT=3 F_HEARD=3 F_MATCH=3 F_MADE=0 F_DBL=0 F_EV= F_AU=Open F_AW=Open F_NU=000C0010 F_NW=000C0010 F_SW=$S1 F_SAVEBACK=1
		F_USED=$d/cused F_SAVED=$d/csaved; rm -f "$F_USED" "$F_SAVED"
	}
	dc() {
		local u=0 me=w l=- i=- ev out
		[ -e "$F_USED" ] && u=1
		[ "$1" = "$F_USER" ] && me=u
		case "$2" in
			"console player.additem 000999B3 1" | "console player.removeitem 000999B3 1" | "console player.moveto 000C0005") echo ran ;;
			"console save "*) touch "$F_SAVED"; echo ran ;;
			ping) echo pong ;;
			"refs refuse on") echo refuse=1 ;;
			"refs refuse off") echo refuse=0 ;;
			"use 000C0005") touch "$F_USED"; echo used ;;
			"ref 000C0005") echo "ref=000C0005 kind=mechanism open=- locked=0 level=- loaded=1 known=-/-/- dist=100 at=(1000,1000,0) enabled=1 wants=000999B3 name=Claw_Door" ;;
			standins) echo "players=1 standins=1 leaving=0 | player 2 standin=FF0008AE placed=1 dressed=1 at=(1100,1000,0) heading=0" ;;
			"refs near"*) echo "near=0" ;;
			"script 000C0005") if [ $u = 0 ]; then echo "$S0"; elif [ $me = u ]; then echo "$S1"; else echo "$F_SW"; fi ;;
			"anims 000C0005") [ $u = 1 ] && { if [ $me = u ]; then l=$F_AU; else l=$F_AW; fi; }; ev=$l; [ $u = 1 ] && [ $me = w ] && [ -n "$F_EV" ] && ev=$F_EV; echo "ref=000C0005 played=$u made=0 last=$l events=$ev" ;;
			"enables 000C0005") [ $u = 1 ] && { if [ $me = u ]; then i=$F_NU; else i=$F_NW; fi; }; echo "ref=000C0005 near=3 disabled=$u ids=$i" ;;
			refs)
				if [ $me = u ]; then
					echo "installed=1 uses=$u/0/0 lastRefused=- lent=$((u * F_SENT))/0/0/0/0/0 effects=$((u * F_CAPT))/0/$u/0 mirror=0/0/0/0/0/0/0/0 notes=0 refuse=0 hooked=1"
				else
					out=$((u * F_OUT)); [ -e "$F_SAVED" ] && [ "$F_SAVEBACK" = 1 ] && out=0
					echo "installed=1 uses=0/$((u * F_UMADE))/$((u * F_REF)) lastRefused=- lent=0/0/$((u * F_HERE))/$((u * F_HERE))/0/$out/0/0 effects=0/0/0/0 mirror=$((u * F_HEARD))/$((u * F_MATCH))/$((u * F_MADE))/0/0/$((u * F_DBL))/0/0 notes=0 refuse=0 hooked=1"
				fi ;;
			*) echo "error: the fake has no '$2'" ;;
		esac
	}
	log_since_mark() { [ -e "$F_SAVED" ] && [ "$F_SAVEBACK" = 1 ] && echo "World: 1 item(s) lent to stand-ins for friends' uses taken back before the save"; return 0; }
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init ct >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }
	fails() { grep -c '^FAIL ' <<<"$1"; }
	claw() { run "$1" claws_use "${F_USER}" 000C0005 000999B3 Claw_Door "${2:-}"; }

	cdef; o=$(claw 1)
	t "claw: lent, made, said, its own run did it all, back and the same in both passes" "$(fails "$o"),$(grep -cE '^(WARN|RIG) ' <<<"$o"),$(grep -c '^PASS host: its stand-in.s run of the vm.s use did all of it itself' <<<"$o"),$(grep -c '^PASS host: its own run matched what the vm.s use did' <<<"$o"),$(grep -c '^PASS host: no animation played twice' <<<"$o")" 0,0,1,1,1
	cdef; F_MADE=2; o=$(claw 1b)
	t "claw: a lend that did nothing (the watcher made what its run missed) fails" "$(grep -c '^FAIL host: its stand-in.s run of the vm.s use did all of it itself' <<<"$o"),$(fails "$o")" 1,1
	cdef; F_MATCH=1; o=$(claw 1c)
	t "claw: its own run matching fewer than said fails" "$(grep -c '^FAIL host: its own run matched what the vm.s use did' <<<"$o"),$(fails "$o")" 1,1
	cdef; F_EV='*Open,Open'; o=$(claw 1d)
	t "claw: an animation made there and then sent again by its own run fails" "$(grep -c '^FAIL host: no animation played twice on the Claw_Door' <<<"$o"),$(fails "$o")" 1,1
	t "twice: only events since the leg began count" "$(claws_twice 'ref=1 played=3 events=*Turn,Turn,Open' 2),$(claws_twice 'ref=1 played=3 events=*Turn,Open,Turn' 0),$(claws_twice 'ref=1 played=0 events=-')" 0,1,0
	cdef; F_HERE=0; o=$(claw 2)
	t "claw: a stand-in lent nothing fails" "$(grep -c '^FAIL host: lends its stand-in of the vm the item' <<<"$o"),$(fails "$o")" 1,1
	cdef; F_SENT=0; o=$(claw 3)
	t "claw: a use that lends nothing fails" "$(grep -c '^FAIL vm: its use lends what it holds' <<<"$o")" 1
	cdef; F_OUT=1; o=$(claw 4)
	t "claw: an item still out after the loans' time fails" "$(grep -c '^FAIL host: every item lent to its stand-in taken back' <<<"$o"),$(fails "$o")" 1,1
	cdef; F_AW=Close; o=$(claw 5)
	t "claw: a last animation that differs fails" "$(grep -c "^FAIL host: the Claw_Door's last animation reads as the vm's" <<<"$o"),$(fails "$o")" 1,1
	cdef; F_DBL=1; o=$(claw 6)
	t "claw: its own late repeat held back is a WARN, not a FAIL" "$(grep -c '^WARN host: its own run sent no animation it had made already' <<<"$o"),$(fails "$o")" 1,0
	cdef; F_CAPT=0; F_HEARD=0; o=$(claw 7)
	t "claw: a use that changed nothing, the last tried, is RIG, not FAIL" "$(grep -c '^RIG .*vm: its use of the Claw_Door changed something' <<<"$o"),$(fails "$o")" 1,0
	cdef; F_CAPT=0; F_HEARD=0; o=$(run 7b claws_use vm 000C0005 000999B3 Claw_Door - 0)
	t "claw: a use that changed nothing with another to try is a WARN" "$(grep -c '^WARN vm: its use of the Claw_Door changed something the rig reads; the next' <<<"$o"),$(grep -c '^RIG ' <<<"$o"),$(fails "$o")" 1,0,0
	cdef; F_CAPT=0; F_HEARD=0; o=$(run 7c claws_try vm - "000C0005 000999B3" "000C0005 000999B3")
	t "try: each mechanism tried until the last, RIG when none changed anything" "$(grep -c '^WARN vm: its use of the mechanism changed something' <<<"$o"),$(grep -c '^RIG .*vm: its use of the mechanism changed something' <<<"$o")" 1,1
	cdef; o=$(run 7d claws_try vm - "000C0005 000999B3" "000C0005 000999B3")
	t "try: the first that changes something is the one" "$(grep -c '^PASS vm: says what its use of the mechanism did' <<<"$o"),$(fails "$o")" 1,0
	cdef; F_HEARD=1; o=$(claw 8)
	t "claw: fewer effects heard than said fails" "$(grep -c '^FAIL host: heard what the vm.s use did' <<<"$o"),$(fails "$o")" 1,1
	cdef; F_SW=$S0; o=$(claw 9)
	t "claw: the script not followed fails" "$(grep -c "^FAIL host: the Claw_Door 000C0005's script reads as" <<<"$o"),$(fails "$o")" 1,1
	cdef; F_NW=-; o=$(claw 10)
	t "claw: a wall the user's use disabled and the other did not fails" "$(grep -c '^FAIL host: the references disabled near the Claw_Door read as' <<<"$o"),$(fails "$o")" 1,1
	cdef; F_UMADE=0; o=$(claw 11)
	t "claw: a stand-in that never uses it fails" "$(grep -c '^FAIL host: its stand-in of the vm uses the Claw_Door too' <<<"$o")" 1

	# the watcher refusing (a script that does nothing for a stand-in): what the use did, made there
	cdef; F_UMADE=0 F_REF=1 F_HERE=0 F_MADE=2; o=$(run 12 claws_use vm 000C0005 - Pillar refuse)
	t "refuse: refused, and what the use did made, passes" "$(grep -c '^PASS host: refused the vm.s use' <<<"$o"),$(grep -c '^PASS host: made what the vm.s use did' <<<"$o"),$(fails "$o")" 1,1,0
	cdef; F_UMADE=0 F_REF=1 F_HERE=0 F_MADE=0; o=$(run 13 claws_use vm 000C0005 - Pillar refuse)
	t "refuse: nothing made fails" "$(grep -c '^FAIL host: made what the vm.s use did' <<<"$o"),$(fails "$o")" 1,1

	# the VM saving while it lends (the host uses)
	cdef; F_USER=host F_OUT=1; o=$(claw 14 save)
	t "save: the loans back before the save passes" "$(grep -c '^PASS vm: took back what it lent before the save' <<<"$o"),$(grep -c '^PASS vm: nothing lent out after the save' <<<"$o"),$(fails "$o")" 1,1,0
	cdef; F_USER=host F_OUT=1 F_SAVEBACK=0; o=$(claw 15 save)
	t "save: loans kept through the save fail" "$(grep -c '^FAIL vm: took back what it lent before the save' <<<"$o"),$(grep -c '^FAIL vm: nothing lent out after the save' <<<"$o")" 1,1
	cdef; F_USER=host F_OUT=0; o=$(claw 16 save)
	t "save: nothing out when the save begins is RIG, not FAIL" "$(grep -c '^RIG .*vm: saves while it lends' <<<"$o"),$(fails "$o")" 1,0

	# a stand-in out of reach: RIG, and no use made
	cdef; o=$(dc() { case "$2" in "console player"*) echo ran ;; standins) echo "standin=FF0008AE placed=1 at=(9000,1000,0)" ;; "ref 000C0005") echo "ref=000C0005 kind=mechanism at=(1000,1000,0) name=Claw_Door" ;; esac; }; run 17 claws_use vm 000C0005 000999B3 Claw_Door)
	t "reach: a stand-in out of reach is RIG, and no use is made" "$(grep -c '^RIG .*host: its stand-in of the vm comes within reach' <<<"$o"),$(fails "$o")" 1,0

	# the step is wired into the harness: in STEPS before jobsreset, a session step, in the default run
	t "claws is a step, before jobsreset" "$(case " $STEPS " in *" claws jobsreset "*) echo yes;; esac)" yes
	t "claws needs a session" "$(case " $SESSION_STEPS " in *" claws "*) echo yes;; esac)" yes
	t "claws runs by default" "$(case " $OPTIN_STEPS " in *" claws "*) echo no;; *) echo yes;; esac)" yes
	t "claws has its line" "$([ -n "${STEP_INFO[claws]:-}" ] && echo yes)" yes
	t "claws is not re-run (the VM saves)" "$(grep -c 'RERUN_STEPS=.*claws' "$(dirname "${BASH_SOURCE[0]}")/../smoke.sh")" 0
	[ $bad -eq 0 ] && echo "claws.sh selftest: all ok" || echo "claws.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
