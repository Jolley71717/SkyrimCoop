#!/bin/bash
# steps/doors.sh: the rig step `doors` (protocol 5, docs/DESIGN.md "Doors, locks and mechanisms"), sourced by steps.sh.
# A door, a load door and a container one game locks are locked in the other, and unlocked the other way; a door one
# game opens is open in the other, and closed the other way; after a load and after a rejoin the VM's game takes the
# session's word; a mechanism one player uses is used by their stand-in in the other game. Not run in a game yet.
#
#   step_doors                                  the step (in the default run, after forms, before final)
#   bash tools/rig/steps/doors.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `refs near`, `ref <id>`, `use <id>`, `script <id>` and `refs`):
#   1. Both players are put in DOORS_CELL with coc (console placement, said in the report), each with its stand-in of the
#      other. Each lists this class's references within DOORS_NEAR of it; the ones picked are in both lists, so within
#      the plugin's report reach (RefRules::REPORT_REACH, 2000 u) of both players: a change made by the console there is
#      reported by the game that made it.
#   2. Locks, one of each kind found (door, loaddoor, container): the VM locks it (console `<id>.lock`, set-up, as a
#      script or a key would), and the host must read it locked at the VM's level; the host unlocks it, and the VM must
#      read it unlocked. One locked to begin with is locked again at the end.
#   3. Doors: the VM uses an unlocked plain door (`use`, the activate key's own call, from where the player stands; a
#      player's own use is shared at any distance), and the host must read it open (or closed, whichever it went to);
#      the host uses it, and the VM must read it as it was.
#   4. The session's word after a load and a rejoin: with the door open, the VM saves ($SAVE_NAME, as save_rejoin); the
#      host closes it; the VM loads its save (the door open in it) and must read it closed (it asks the session again
#      after a load); the VM leaves, the host opens the door, the VM joins again and must read it open.
#   5. Mechanisms: both are put in DOORS_MECH_CELL (coc). For each player in turn, the player is put at the mechanism
#      named DOORS_MECH_NAME (console `player.moveto`: placement to reach it, said), the other game's stand-in of them
#      must come within the use's reach (DOORS_USE_REACH, RefRules::USE_REACH), the player uses it, and the other game's
#      count of uses made by a stand-in must go up with none refused (a use the game refused counts as refused). Then its
#      effect, read in both games: the mechanism's own script (`script <id>`: its state and variables) and the open
#      states of everything near it that has one (a gate it raises). What changed in the user's game must read the same
#      in the other; a use that changed nothing the rig reads is a WARN. The default is a pillar (Bleak Falls' puzzle):
#      the lever there sets off darts at whoever pulls it.
#   6. Both are put back in the harness's cell (coc).

DOORS_CELL=${DOORS_CELL:-RiverwoodSleepingGiantInn}  # plain doors, the inn's load door, containers
DOORS_MECH_CELL=${DOORS_MECH_CELL:-BleakFallsBarrow01}
DOORS_MECH_NAME=${DOORS_MECH_NAME:-Pillar}           # the mechanism's name as the line has it (spaces as _)
DOORS_LOCK=${DOORS_LOCK:-50}                         # the console lock's number: adept (the wire's level 2)
DOORS_NEAR=${DOORS_NEAR:-1800}                       # u: how far from each player the doors and containers are looked for
                                                     # (under RefRules::REPORT_REACH, 2000 u: a console change further off is not reported)
DOORS_MECH_NEAR=${DOORS_MECH_NEAR:-20000}            # u: the mechanism, from the cell's coc marker (the most refs near takes)
DOORS_USE_REACH=${DOORS_USE_REACH:-1000}             # u: RefRules::USE_REACH, the stand-in's reach for a friend's use
DOORS_WAIT=${DOORS_WAIT:-10}                         # s: for a word to reach the other game and be applied there
DOORS_EFFECT_NEAR=${DOORS_EFFECT_NEAR:-3000}         # u: the references near the mechanism whose open states are compared
DOORS_SETTLE=${DOORS_SETTLE:-6}                      # s: after a use, for its script to run in both games (RefRules::SETTLE is 3 s)

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# doors_lines <refs near reply>: its reference lines, one a line ("near=N | line | line ...": names never hold " | ")
doors_lines() { sed 's/ | /\n/g' <<<"$1" | tail -n +2; }
# doors_pick <refs near reply> <kind> [name] [unlocked]: the id of the nearest loaded reference of that kind (and that
# name, and unlocked, when asked); 1 when there is none
doors_pick() {
	local line
	while IFS= read -r line; do
		[ "$(field kind "$line")" = "$2" ] && [ "$(field loaded "$line")" = 1 ] || continue
		[ -z "${3:-}" ] || [ "$(field name "$line")" = "$3" ] || continue
		[ "${4:-}" != unlocked ] || [ "$(field locked "$line")" = 0 ] || continue
		field ref "$line"; return 0
	done < <(doors_lines "$1")
	return 1
}
# doors_both <host's refs near> <vm's refs near> <kind> [name] [unlocked]: as doors_pick from the host's list, the
# nearest one the VM's list holds too, loaded there (so near both players); 1 when there is none
doors_both() {
	local line id
	while IFS= read -r line; do
		[ "$(field kind "$line")" = "$3" ] && [ "$(field loaded "$line")" = 1 ] || continue
		[ -z "${4:-}" ] || [ "$(field name "$line")" = "$4" ] || continue
		[ "${5:-}" != unlocked ] || [ "$(field locked "$line")" = 0 ] || continue
		id=$(field ref "$line")
		doors_lines "$2" | grep -q "^ref=$id .*loaded=1 " || continue
		echo "$id"; return 0
	done < <(doors_lines "$1")
	return 1
}
# doors_opens <refs near reply>: the ids in it that have an open state (open=0 or 1), one a line
doors_opens() { local line; while IFS= read -r line; do [[ $(field open "$line") == [01] ]] && field ref "$line"; done < <(doors_lines "$1"); return 0; }
# doors_kinds <refs near reply>: what the listing holds, "kind:name" each, for a report when the one wanted is not there
doors_kinds() { local line; while IFS= read -r line; do echo -n "$(field kind "$line"):$(field name "$line") "; done < <(doors_lines "$1"); }
# doors_uses <refs status> <n>: the uses count n (1 sent, 2 made by a stand-in, 3 refused) of `uses=sent/made/refused`
doors_uses() { field uses "$1" | cut -d/ -f"$2"; }
# doors_effect <user> <watcher> <what>: a use's effect, from E_S0U E_S0W E_S1U E_S1W (the mechanism's `script` reply
# before and after, in the user's and the watcher's game) and E_O0U E_O0W E_O1U E_O1W (the open states near it, as
# doors_states gives them). Before, both must read the same (or the comparison after means nothing: a RIG verdict);
# after, what changed in the user's game must read the same in the watcher's (FAIL otherwise); nothing changed is a WARN.
doors_effect() {
	local user=$1 watcher=$2 what=$3 changed=0
	if [ "$E_S0U" != "$E_S0W" ] || [ "$E_O0U" != "$E_O0W" ]; then
		rig_fault state "both: the $what and what is near it read the same before the $user's use" "script $user [$E_S0U] $watcher [$E_S0W]; open $user [$E_O0U] $watcher [$E_O0W]"
		return 1
	fi
	[ "$E_S1U" != "$E_S0U" ] && changed=1
	[ "$E_O1U" != "$E_O0U" ] && changed=1
	if [ $changed = 0 ]; then
		warn "$user: its use of the $what changed something the rig reads (its script, an open state near it)" "nothing changed: script [$E_S1U]; open [$E_O1U]"
		return 0
	fi
	expect_eq "$watcher: the $what's script reads as the $user's after the $user's use" "$E_S1W" "$E_S1U"
	expect_eq "$watcher: the open states near the $what read as the $user's after the $user's use" "$E_O1W" "$E_O1U"
}

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

# doors_has <who> <id> <key> <want>: that game's line of the reference reads key=want
doors_has() { [ "$(field "$3" "$(dc "$1" "ref $2")")" = "$4" ]; }
# doors_expect <check> <who> <id> <key> <want>: within DOORS_WAIT, that game reads it so (PASS), or FAIL with its line
doors_expect() {
	if wait_until "$DOORS_WAIT" doors_has "$2" "$3" "$4" "$5"; then pass "$1" "$(dc "$2" "ref $3" | cut -c1-120)"
	else fail "$1" "want $4=$5, $2 reads: $(dc "$2" "ref $3" | cut -c1-200)"; return 1; fi
}
# doors_own <check> <who> <id> <key> <want>: the game that made a change reads it itself (PASS); when not, the console
# or the activation did not take (the rig's input, not the mod: a RIG verdict, and the rest of that check is not made)
doors_own() {
	if wait_until "$DOORS_WAIT" doors_has "$2" "$3" "$4" "$5"; then pass "$1" "$2 reads $4=$5"; return 0; fi
	rig_fault input "$1" "$2's own reference did not change (want $4=$5): $(dc "$2" "ref $3" | cut -c1-200)"; return 1
}
# doors_run <check> <who> <command> <want>: a dev command whose reply must be <want>; a rig fault otherwise
doors_run() {
	local r; r=$(dc "$2" "$3")
	[ "$r" = "$4" ] && return 0
	rig_broken input "$1" "$2: '$3' replied '$r'"; return 1
}

# doors_coc <cell>: both players put in that cell with coc, in one space, each with its stand-in of the other
doors_coc() {
	local w
	for w in host vm; do doors_run "both: put in $1 (coc)" $w "console coc $1" ran || return 1; done
	sleep 5
	for w in host vm; do
		wait_until 90 in_world $w || { rig_unhealed state "both: put in $1 (coc)" "$w is not in the world with no box open: $(dc $w where | cut -c1-120)"; return 1; }
	done
	if [ "$(space_in "$(dc host where)")" != "$(space_in "$(dc vm where)")" ]; then
		rig_unhealed state "both: put in $1 (coc)" "not in one space: host $(space_in "$(dc host where)"), vm $(space_in "$(dc vm where)")"; return 1
	fi
	if wait_until 30 placed host && wait_until 30 placed vm; then pass "both: put in $1 with coc (console placement), each with its stand-in of the other" "$(space_in "$(dc host where)")"
	else fail "both: each has its stand-in of the other in $1" "host $(dc host standins | cut -c1-120); vm $(dc vm standins | cut -c1-120)"; return 1; fi
}

# doors_lock <kind> <id>: the VM locks it, the host reads it locked at the VM's level; the host unlocks it, the VM reads
# it unlocked; one locked before is locked again (by the host) at the end
doors_lock() {
	local kind=$1 id=$2 was level
	[ "$(field kind "$(dc vm "ref $id")")" = "$kind" ] || { fail "vm: has the host's $kind $id" "$(dc vm "ref $id" | cut -c1-160)"; return; }
	was=$(field locked "$(dc host "ref $id")")
	doors_expect "both: read the $kind $id's lock the same before (locked=$was)" vm "$id" locked "$was" || return
	doors_run "vm: locks the $kind $id (console lock $DOORS_LOCK)" vm "console $id.lock $DOORS_LOCK" ran || return
	doors_own "vm: locks the $kind $id (console lock $DOORS_LOCK)" vm "$id" locked 1 || return
	level=$(field level "$(dc vm "ref $id")")
	doors_expect "host: the $kind the VM locked is locked" host "$id" locked 1 &&
		doors_expect "host: ... at the VM's level ($level)" host "$id" level "$level"
	doors_run "host: unlocks the $kind $id (console unlock)" host "console $id.unlock" ran || return
	doors_own "host: unlocks the $kind $id (console unlock)" host "$id" locked 0 || return
	doors_expect "vm: the $kind the host unlocked is unlocked" vm "$id" locked 0
	if [ "$was" = 1 ]; then
		doors_run "host: locks the $kind $id again, as it was" host "console $id.lock $DOORS_LOCK" ran &&
			doors_own "host: locks the $kind $id again, as it was" host "$id" locked 1 &&
			doors_expect "vm: the $kind the host locked again is locked" vm "$id" locked 1
	fi
}

# doors_open <id>: the VM uses the door, the host reads it as the VM's reads; the host uses it, the VM reads it as before.
# When the host never read the VM's change, the other half is not made (the host's use would only take its own door
# where the VM's is already, and pass by chance).
doors_open() {
	local id=$1 was now
	was=$(field open "$(dc host "ref $id")")
	[[ $was == [01] ]] || { fail "host: reads the door $id's open state" "$(dc host "ref $id" | cut -c1-160)"; return 1; }
	doors_expect "both: read the door $id the same before (open=$was)" vm "$id" open "$was" || return 1
	now=$((1 - was))
	doors_run "vm: uses the door $id" vm "use $id" used || return 1
	doors_own "vm: uses the door $id" vm "$id" open "$now" || return 1
	doors_expect "host: the door the VM used reads open=$now" host "$id" open "$now" || return 1
	doors_run "host: uses the door $id" host "use $id" used || return 1
	doors_own "host: uses the door $id" host "$id" open "$was" || return 1
	doors_expect "vm: the door the host used reads open=$was" vm "$id" open "$was"
}

# doors_again <id>: the session's word after the VM loads a save that holds the door otherwise, and after it rejoins
doors_again() {
	local id=$1 asks0 r
	if ! doors_has vm "$id" open 1; then
		doors_run "vm: opens the door $id" vm "use $id" used && doors_own "vm: opens the door $id" vm "$id" open 1 || return
	fi
	doors_expect "host: the door $id is open before the VM saves" host "$id" open 1 || return
	doors_run "vm: saves with the door open" vm "console save $SAVE_NAME" ran || return
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return; }
	doors_run "host: closes the door $id" host "use $id" used && doors_own "host: closes the door $id" host "$id" open 0 || return
	doors_expect "vm: the door the host closed is closed" vm "$id" open 0 || return

	asks0=$(field asks "$(dc vm refs)")
	doors_run "vm: loads its save (the door open in it)" vm "console load $SAVE_NAME" ran || return
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; }
	wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return; }
	doors_expect "vm: after loading a save that has the door open, it reads the session's word (closed)" vm "$id" open 0
	if is_num "$asks0"; then expect_ge "vm: asked the session for its word again after the load" "$(field asks "$(dc vm refs)")" $((asks0 + 1))
	else fail "vm: asked the session for its word again after the load" "no asks= before: $(dc vm refs | cut -c1-160)"; fi

	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	gone_from host || fail "host: the VM's stand-in goes when it leaves" "$(dc host standins | cut -c1-160)"
	doors_run "host: opens the door $id while the VM is away" host "use $id" used && doors_own "host: opens the door $id while the VM is away" host "$id" open 1 || return
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	doors_expect "vm: after joining again, the door the host opened while it was away is open" vm "$id" open 1
}

# doors_near <watcher> <id>: the watcher's stand-in of its friend is within the use's reach of the reference (with a
# margin: the reach the plugin judges is in three dimensions, this one on the ground)
doors_near() {
	local d; d=$(dist2 "$(standin_at "$1")" "$(xyz ' at=' "$(dc "$1" "ref $2")")")
	is_num "$d" && [ "$d" -lt $((DOORS_USE_REACH * 4 / 5)) ]
}

# doors_states <who> <ids...>: "id=open" for each, as that game reads them now
doors_states() { local w=$1 id out=""; shift; for id in "$@"; do out+="$id=$(field open "$(dc "$w" "ref $id")") "; done; echo "$out"; }

# doors_use <user> <id>: the user is put at the mechanism and uses it; the other game's stand-in of them uses it too, and
# what it changed in the user's game reads the same in the other
doors_use() {
	local user=$1 id=$2 watcher made0 refused0 sent0 ids
	watcher=$(other "$user")
	doors_run "$user: put at the $DOORS_MECH_NAME $id (console player.moveto: placement)" "$user" "console player.moveto $id" ran || return
	sleep 3
	wait_until 20 doors_near "$watcher" "$id" ||
		{ rig_fault state "$watcher: its stand-in of the $user comes within reach of the $DOORS_MECH_NAME" "stand-in at ($(standin_at "$watcher")), the $DOORS_MECH_NAME: $(dc "$watcher" "ref $id" | cut -c1-160)"; return; }
	# shellcheck disable=SC2207
	ids=($(doors_opens "$(dc "$user" "refs near $DOORS_EFFECT_NEAR")"))
	E_S0U=$(dc "$user" "script $id"); E_S0W=$(dc "$watcher" "script $id")
	E_O0U=$(doors_states "$user" ${ids[@]+"${ids[@]}"}); E_O0W=$(doors_states "$watcher" ${ids[@]+"${ids[@]}"})
	sent0=$(doors_uses "$(dc "$user" refs)" 1); made0=$(doors_uses "$(dc "$watcher" refs)" 2); refused0=$(doors_uses "$(dc "$watcher" refs)" 3)
	is_num "$sent0" && is_num "$made0" && is_num "$refused0" || { fail "both: count the uses" "$user $(dc "$user" refs | cut -c1-160); $watcher $(dc "$watcher" refs | cut -c1-160)"; return; }
	doors_run "$user: uses the $DOORS_MECH_NAME $id" "$user" "use $id" used || return
	sleep 2
	expect_ge "$user: reports its use of the $DOORS_MECH_NAME" "$(doors_uses "$(dc "$user" refs)" 1)" $((sent0 + 1))
	wait_until "$DOORS_WAIT" _doors_made "$watcher" $((made0 + 1))
	expect_ge "$watcher: its stand-in of the $user uses the $DOORS_MECH_NAME too" "$(doors_uses "$(dc "$watcher" refs)" 2)" $((made0 + 1))
	expect_eq "$watcher: refused none of the $user's uses" "$(doors_uses "$(dc "$watcher" refs)" 3)" "$refused0"
	sleep "$DOORS_SETTLE"
	E_S1U=$(dc "$user" "script $id"); E_S1W=$(dc "$watcher" "script $id")
	E_O1U=$(doors_states "$user" ${ids[@]+"${ids[@]}"}); E_O1W=$(doors_states "$watcher" ${ids[@]+"${ids[@]}"})
	doors_effect "$user" "$watcher" "$DOORS_MECH_NAME $id"
}
_doors_made() { local n; n=$(doors_uses "$(dc "$1" refs)" 2); is_num "$n" && [ "$n" -ge "$2" ]; }

step_doors() {
	local list vlist kind id door w
	section "doors: locks one game changes are so in the other (door, load door, container)"
	doors_coc "$DOORS_CELL" || return
	sleep 3
	list=$(dc host "refs near $DOORS_NEAR")
	[[ $list == near=* ]] || { rig_broken game "host: lists the doors near it" "$(cut -c1-160 <<<"$list")"; return; }
	vlist=$(dc vm "refs near $DOORS_NEAR")
	[[ $vlist == near=* ]] || { rig_broken game "vm: lists the doors near it" "$(cut -c1-160 <<<"$vlist")"; return; }
	echo "     host: $(cut -c1-600 <<<"$list")"
	for kind in door loaddoor container; do
		if id=$(doors_both "$list" "$vlist" "$kind"); then doors_lock "$kind" "$id"
		else warn "both: a $kind near both players to lock" "none within $DOORS_NEAR u of both in $DOORS_CELL: $(doors_kinds "$list" | cut -c1-200)"; fi
	done

	section "doors: a door one game opens or closes is so in the other"
	if door=$(doors_both "$list" "$vlist" door "" unlocked); then
		doors_open "$door" && {
			section "doors: the session's word after the VM loads a save and after it joins again"
			doors_again "$door"
		}
	else
		rig_unhealed state "host: an unlocked door near it" "none within $DOORS_NEAR u in $DOORS_CELL: $(doors_kinds "$list" | cut -c1-200)"
	fi

	section "mechanisms: a friend's use is made by their stand-in"
	if doors_coc "$DOORS_MECH_CELL"; then
		list=$(dc vm "refs near $DOORS_MECH_NEAR")
		if id=$(doors_pick "$list" mechanism "$DOORS_MECH_NAME"); then
			doors_use vm "$id"
			doors_use host "$id"
		else
			rig_unhealed state "vm: a mechanism named $DOORS_MECH_NAME near it" "none within $DOORS_MECH_NEAR u in $DOORS_MECH_CELL: $(doors_kinds "$list" | cut -c1-200)"
		fi
	fi

	# How the plugin made its game writes (until the frame hook, off the game's main thread: counted, for the record)
	for w in host vm; do echo "     $w: $(dc $w refs | tr ' ' '\n' | grep -E '^(offmain|notTaken|resent|settled|scanned|eventsDropped)=' | tr '\n' ' ')"; done

	section "doors: both back in $SMOKE_CELL"
	doors_coc "$SMOKE_CELL"
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init doorstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	DOORS_WAIT=2

	L1='ref=0001A6A4 kind=door open=0 locked=1 level=5 loaded=1 known=-/-/- dist=210 at=(10,20,30) name=Delphine'"'"'s_Room'
	L2='ref=0001A6A5 kind=door open=0 locked=0 level=- loaded=1 known=-/-/- dist=340 at=(40,50,60) name=Door'
	L3='ref=000B1234 kind=container open=- locked=0 level=1 loaded=1 known=-/-/- dist=400 at=(1,2,3) name=Chest'
	L4='ref=000C0001 kind=mechanism open=- locked=0 level=- loaded=0 known=-/-/- dist=500 at=(1,2,3) name=Pillar'
	L5='ref=000C0002 kind=mechanism open=- locked=0 level=- loaded=1 known=-/-/- dist=900 at=(7,8,9) name=Pillar'
	L6='ref=000C0003 kind=mechanism open=- locked=0 level=- loaded=1 known=-/-/- dist=800 at=(7,8,9) name=Lever'
	NEAR="near=6 | $L1 | $L2 | $L3 | $L4 | $L6 | $L5"
	t "lines: one a reference" "$(doors_lines "$NEAR" | wc -l | tr -d ' ')" 6
	t "lines: none" "$(doors_lines 'near=0' | wc -l | tr -d ' ')" 0
	t "pick: the nearest door" "$(doors_pick "$NEAR" door)" 0001A6A4
	t "pick: the nearest unlocked door" "$(doors_pick "$NEAR" door "" unlocked)" 0001A6A5
	t "pick: a container" "$(doors_pick "$NEAR" container)" 000B1234
	t "pick: a mechanism by its name, loaded" "$(doors_pick "$NEAR" mechanism Pillar)" 000C0002
	t "pick: never the lever when a pillar is wanted" "$(doors_pick "near=1 | $L6" mechanism Pillar || echo none)" none
	t "pick: no load door" "$(doors_pick "$NEAR" loaddoor || echo none)" none
	t "kinds" "$(doors_kinds "near=2 | $L2 | $L6")" "door:Door mechanism:Lever "
	t "uses: sent" "$(doors_uses 'installed=1 book=3 uses=4/2/1 lastRefused=-' 1)" 4
	t "uses: made" "$(doors_uses 'installed=1 book=3 uses=4/2/1 lastRefused=-' 2)" 2
	t "uses: refused" "$(doors_uses 'installed=1 book=3 uses=4/2/1 lastRefused=-' 3)" 1
	t "uses: an error reply" "$(doors_uses 'error: usage: refs' 2)" ""

	# Both games faked: each holds one reference's open and locked state, and the console, use and refs commands change
	# and read it; SHARE=1 is the mod at work (a change in one game reaches the other), 0 a mod that shares nothing.
	fake() {  # fake <share>: the fake's state files, fresh
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1
		for w in host vm; do echo 0 > "$FK/$w.open"; echo 0 > "$FK/$w.locked"; echo - > "$FK/$w.level"; echo 0 > "$FK/$w.sent"; echo 0 > "$FK/$w.made"; done
	}
	setf() { echo "$3" > "$FK/$1.$2"; [ "$SHARE" = 1 ] && echo "$3" > "$FK/$(other "$1").$2"; return 0; }
	dc() {
		local w=$1 c=$2
		case "$c" in
			"ref 0001A6A5") echo "ref=0001A6A5 kind=door open=$(cat "$FK/$w.open") locked=$(cat "$FK/$w.locked") level=$(cat "$FK/$w.level") loaded=1 known=-/-/- dist=300 at=(40,50,60) name=Door" ;;
			"console 0001A6A5.lock 50") setf "$w" locked 1; setf "$w" level 2; echo ran ;;
			"console 0001A6A5.unlock") setf "$w" locked 0; echo ran ;;
			"use 0001A6A5") setf "$w" open $((1 - $(cat "$FK/$w.open"))); echo used ;;
			refs) echo "installed=1 book=1 uses=$(cat "$FK/$w.sent")/$(cat "$FK/$w.made")/0 lastRefused=- asks=0" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init dt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; o=$(run 1 doors_lock door 0001A6A5)
	t "lock: shared both ways passes" "$(verdicts "$o")" "PASS=6 "
	t "lock: the host reads the VM's level" "$(grep -c '^PASS host: ... at the VM.s level (2)' <<<"$o")" 1
	fake 0; o=$(run 2 doors_lock door 0001A6A5)
	t "lock: nothing shared fails on the host, then on the VM" "$(grep -c '^FAIL host: the door the VM locked is locked' <<<"$o"),$(grep -c '^FAIL vm: the door the host unlocked is unlocked' <<<"$o")" 1,1
	fake 1; echo 1 > "$FK/host.locked"; echo 1 > "$FK/vm.locked"; o=$(run 3 doors_lock door 0001A6A5)
	t "lock: one locked before is locked again at the end" "$(verdicts "$o")" "PASS=8 "
	t "lock: ... and both read it locked" "$(cat "$FK/host.locked")$(cat "$FK/vm.locked")" 11

	fake 1; o=$(run 4 doors_open 0001A6A5)
	t "open: shared both ways passes" "$(verdicts "$o")" "PASS=5 "
	t "open: the door ends as it began" "$(cat "$FK/host.open")$(cat "$FK/vm.open")" 00
	fake 0; o=$(run 5 doors_open 0001A6A5)
	t "open: nothing shared fails on the host, and the other half is not made" "$(grep -c '^FAIL host: the door the VM used reads open=1' <<<"$o"),$(grep -c 'host: uses the door' <<<"$o")" 1,0
	# a use the game did not make (its own door unchanged) is the rig's input: RIG, never FAIL
	fake 1; dc() { case "$2" in "use 0001A6A5") echo used ;; "ref 0001A6A5") echo "ref=0001A6A5 kind=door open=0 locked=0 level=- loaded=1 known=-/-/- dist=300 at=(40,50,60) name=Door" ;; esac; }
	o=$(run 6 doors_open 0001A6A5)
	t "open: the VM's own door unchanged is RIG, not FAIL" "$(grep -c '^RIG .*vm: uses the door' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0

	# the mechanism: the watcher's stand-in within reach, its made count up with none refused
	mech() {  # mech <made after the use> <refused after>
		M_MADE=$1 M_REF=$2; M_USED=$d/used; rm -f "$M_USED"
		dc() {
			case "$2" in
				"console player.moveto 000C0002") echo ran ;;
				"use 000C0002") touch "$M_USED"; echo used ;;
				"ref 000C0002") echo "ref=000C0002 kind=mechanism open=- locked=0 level=- loaded=1 known=-/-/- dist=100 at=(1000,1000,0) name=Pillar" ;;
				standins) echo "players=1 standins=1 leaving=0 | player 2 standin=FF0008AE placed=1 dressed=1 at=(1100,1000,0) heading=0" ;;
				refs) if [ -e "$M_USED" ]; then
						if [ "$1" = vm ]; then echo "uses=1/0/0"; else echo "uses=0/$M_MADE/$M_REF"; fi
					else echo "uses=0/0/0"; fi ;;
			esac
		}
		run "m$1$2" doors_use vm 000C0002
	}
	o=$(mech 1 0); t "mechanism: the stand-in's use made passes (and a use that changed nothing read is a WARN)" "$(verdicts "$o")" "PASS=3 WARN=1 "
	o=$(mech 0 1); t "mechanism: refused fails" "$(grep -c '^FAIL host: its stand-in of the vm uses' <<<"$o"),$(grep -c '^FAIL host: refused none' <<<"$o")" 1,1

	# the effect: the pillar's script and the gate near it, before and after, in both games
	eff() { E_S0U=$1 E_S0W=$2 E_S1U=$3 E_S1W=$4 E_O0U=$5 E_O0W=$6 E_O1U=$7 E_O1W=$8; run "e$9" doors_effect vm host "Pillar 000C0002"; }
	S0='ref=000C0002 scripts=1 | PuzzlePillar state=- ::face_var=1'; S1='ref=000C0002 scripts=1 | PuzzlePillar state=- ::face_var=2'
	G0='000C0010=0 '; G1='000C0010=1 '
	o=$(eff "$S0" "$S0" "$S1" "$S1" "$G0" "$G0" "$G0" "$G0" 1); t "effect: the script followed passes" "$(verdicts "$o")" "PASS=2 "
	o=$(eff "$S0" "$S0" "$S1" "$S0" "$G0" "$G0" "$G0" "$G0" 2); t "effect: the script not followed fails" "$(grep -c '^FAIL host: the Pillar 000C0002.s script reads as' <<<"$o")" 1
	o=$(eff "$S0" "$S0" "$S0" "$S0" "$G0" "$G0" "$G1" "$G0" 3); t "effect: a gate raised in the user's game and not in the other fails" "$(grep -c '^FAIL host: the open states near' <<<"$o")" 1
	o=$(eff "$S0" "$S0" "$S0" "$S0" "$G0" "$G0" "$G1" "$G1" 4); t "effect: the gate followed passes" "$(verdicts "$o")" "PASS=2 "
	o=$(eff "$S0" "$S0" "$S0" "$S0" "$G0" "$G0" "$G0" "$G0" 5); t "effect: nothing changed is a WARN, not a PASS" "$(verdicts "$o")" "WARN=1 "
	o=$(eff "$S0" "$S1" "$S1" "$S1" "$G0" "$G0" "$G0" "$G0" 6); t "effect: games that differ before is RIG, not FAIL" "$(grep -c '^RIG ' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0

	# picking near both players: the host's nearest door the VM's list also holds
	V1="near=2 | $L2 | $L3"
	t "both: the nearest door both lists hold" "$(doors_both "$NEAR" "$V1" door)" 0001A6A5
	t "both: none when the VM's list lacks it" "$(doors_both "$NEAR" "near=1 | $L3" door || echo none)" none
	t "both: a container" "$(doors_both "$NEAR" "$V1" container)" 000B1234
	t "opens: the ids with an open state" "$(doors_opens "$NEAR" | tr '\n' ' ')" "0001A6A4 0001A6A5 "
	dc() { case "$2" in "console player.moveto"*) echo ran ;; standins) echo "standin=FF0008AE placed=1 at=(5000,1000,0)" ;; "ref 000C0002") echo "ref=000C0002 kind=mechanism at=(1000,1000,0) name=Pillar" ;; esac; }
	o=$(run m3 doors_use vm 000C0002)
	t "mechanism: a stand-in out of reach is RIG, and no use is made" "$(grep -c '^RIG .*host: its stand-in of the vm comes within reach' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0

	# the step is wired into the harness: in STEPS before final, a session step, in the default run
	t "doors is a step, before final" "$(case " $STEPS " in *" doors final "*) echo yes;; esac)" yes
	t "doors needs a session" "$(case " $SESSION_STEPS " in *" doors "*) echo yes;; esac)" yes
	t "doors runs by default" "$(case " $OPTIN_STEPS " in *" doors "*) echo no;; *) echo yes;; esac)" yes
	t "doors has its line" "$([ -n "${STEP_INFO[doors]:-}" ] && echo yes)" yes
	[ $bad -eq 0 ] && echo "doors.sh selftest: all ok" || echo "doors.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
