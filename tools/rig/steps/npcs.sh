#!/bin/bash
# steps/npcs.sh: the rig step `npcs` (protocol 5, docs/DESIGN.md "NPCs"), sourced by steps.sh. Every NPC near the two
# players is run by one game and shown as a copy in the other, and it changes hands when its runner's player walks
# away. Not run in a game yet.
#
#   step_npcs          the step
#   bash tools/rig/steps/npcs.sh selftest      the step's reading, no game and no rig (CI runs it)
#
# What it does (both players in one session, near each other in the harness's cell):
#   1. Both games share NPCs: the graph event hook is in (hooked=1), each has NPCs loaded, and every NPC loaded in both
#      has one runner both games agree on (`npcs list`: owner=, the same in both, never 0).
#   2. Copies stand where their runners have them: each NPC one game runs is, in the other, a copy following its
#      runner (role=copy) within NPC_COPY_LIMIT of where the runner's game has it, for NPC_COPY_SHARE % of them (the two
#      games are read a few ms apart and a copy is drawn about 120 ms behind: a walking NPC is a few dozen units off).
#   3. The kinds seen, in either game (`npcs`: kinds=): people are required (the cell has them); a creature missing is a
#      WARN (none may be near); dragons, horses, followers and summons are said, not required (no scene for them here).
#   4. Hand-over, both ways, by real movement: the host's player walks NPC_WALK units away (the virtual pad's walkto)
#      and every NPC the host ran near the VM's player goes to the VM's game, in both games' books; the host walks back.
#      Then the same with the VM walking away and the host taking its NPCs. A summon or a follower of the walker goes
#      with it, so it is not followed.
#
# Nothing is placed or moved by a script: the players walk, and the NPCs are the cell's own.

NPC_COPY_LIMIT=${NPC_COPY_LIMIT:-150}  # u: a copy from where its runner's game has the NPC
NPC_COPY_SHARE=${NPC_COPY_SHARE:-80}   # %: of the copies, at least this many within NPC_COPY_LIMIT
NPC_NEAR=${NPC_NEAR:-2000}             # u: the NPCs near the staying player that the hand-over check follows
NPC_WALK=${NPC_WALK:-9000}             # u: how far the walker goes (NpcRules.h: a runner lets go past 6000 with a friend 1000 nearer)
NPC_WAIT=${NPC_WAIT:-20}               # s: for the books to agree, and for a hand-over

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# npc_rows <reply of `npcs list`>: one line per NPC: "<key> <owner> <role> <kind> <x> <y> <z>" (the key is its id, and
# :<spawner> for a spawn; x y z are - when its actor is not in this game's world)
npc_rows() {
	awk 'BEGIN { RS = " [|] " }
		NR > 1 {
			sub(/\n$/, "")
			n = split($0, f, " "); key = f[1]; if (length(key) != 8 || key !~ /^[0-9A-F]+$/) next
			owner = "-"; role = "-"; kind = "-"; x = "-"; y = "-"; z = "-"
			for (i = 2; i <= n; i++) {
				split(f[i], kv, "=")
				if (kv[1] == "spawner") key = key ":" kv[2]
				else if (kv[1] == "owner" && owner == "-") owner = kv[2]
				else if (kv[1] == "role") role = kv[2]
				else if (kv[1] == "kind") kind = kv[2]
				else if (kv[1] == "at" && f[i] ~ /^at=\(/) { s = substr(f[i], 5, length(f[i]) - 5); split(s, p, ","); x = p[1]; y = p[2]; z = p[3] }
			}
			print key, owner, role, kind, x, y, z
		}' <<<"$1"
}

# npcs_agree <host rows> <vm rows>: "both=N agreed=N bad=<key>:<host owner>/<vm owner>,..." over the NPCs both list
npcs_agree() {
	awk 'NF == 0 { next } FILENAME == ARGV[1] { owner[$1] = $2; next }
		($1 in owner) { both++; if (owner[$1] == $2 && $2 != "0" && $2 != "-") agreed++; else bad = bad (bad ? "," : "") $1 ":" owner[$1] "/" $2 }
		END { printf "both=%d agreed=%d bad=%s\n", both, agreed, bad ? bad : "none" }' <(printf '%s\n' "$1") <(printf '%s\n' "$2")
}

# npcs_copies <runner rows> <watcher rows> <runner's player id>: "copies=N within=N share=P mean=D max=D off=<key>:<d>,...":
# each NPC the runner runs and the watcher lists, how far the watcher's copy is from the runner's NPC (on the ground)
npcs_copies() {
	awk -v me="$3" -v lim="$NPC_COPY_LIMIT" 'NF == 0 { next } FILENAME == ARGV[1] { if ($2 == me && $3 == "run" && $5 != "-") { rx[$1] = $5; ry[$1] = $6 } next }
		($1 in rx) {
			n++
			if ($3 != "copy" || $5 == "-") { off = off (off ? "," : "") $1 ":" ($3 == "copy" ? "nowhere" : $3); next }
			d = sqrt(($5 - rx[$1]) ^ 2 + ($6 - ry[$1]) ^ 2); sum += d; if (d > max) max = d
			if (d <= lim) within++; else off = off (off ? "," : "") $1 ":" sprintf("%.0f", d)
			placed++
		}
		END { printf "copies=%d within=%d share=%d mean=%.0f max=%.0f off=%s\n", n, within, n ? int(100 * within / n) : 0, placed ? sum / placed : 0, max, off ? off : "none" }' \
		<(printf '%s\n' "$1") <(printf '%s\n' "$2")
}

# npcs_kind <kind> <npcs reply> [<npcs reply>...]: how many NPCs of that kind the replies count (kinds=human:3,...)
npcs_kind() {
	local k=$1 r total=0 n; shift
	for r in "$@"; do n=$(field kinds "$r" | tr ',' '\n' | sed -n "s/^$k://p"); is_num "$n" && total=$((total + n)); done
	echo "$total"
}

# npcs_near <rows> <x y z> <runner id>: the keys that runner runs within NPC_NEAR of that place, space-separated; not
# its spawns, summons or followers (they go with its player)
npcs_near() {
	awk -v p="$2" -v me="$3" -v lim="$NPC_NEAR" 'BEGIN { split(p, q, " ") }
		$2 == me && $1 !~ /:/ && $4 != "follower" && $4 != "summon" && $5 != "-" && sqrt(($5 - q[1]) ^ 2 + ($6 - q[2]) ^ 2) <= lim { printf "%s ", $1 }' <<<"$1" | sed 's/ $//'
}

# npcs_owned_by <rows> <id> <keys...>: 0 when every key is in the rows with that owner; else prints the ones not
npcs_owned_by() {
	local rows=$1 id=$2 k o not=""; shift 2
	for k in "$@"; do o=$(awk -v k="$k" '$1 == k { print $2; exit }' <<<"$rows"); [ "$o" = "$id" ] || not+="$k:${o:-gone} "; done
	[ -z "$not" ] && return 0
	echo "${not% }"; return 1
}

# ---- the step -----------------------------------------------------------------------------------------------------

_npcs_rows() { npc_rows "$(dc "$1" 'npcs list')"; }
_npcs_agreed() {
	local a; a=$(npcs_agree "$(_npcs_rows host)" "$(_npcs_rows vm)")
	[ "$(field bad "$a")" = none ] && [ "$(field both "$a")" != 0 ]
}
_npcs_handed() { npcs_owned_by "$(_npcs_rows host)" "$1" "${@:2}" >/dev/null && npcs_owned_by "$(_npcs_rows vm)" "$1" "${@:2}" >/dev/null; }
_npcs_walk_over() { [[ $(dc "$1" walkto) == "not walking"* ]]; }
# _npcs_walk_back <who> <x y z>: no check: the next step puts the players together again if it falls short
_npcs_walk_back() {
	dc "$1" "walkto $(awk '{print $1, $2}' <<<"$2") 1.0 150 120" >/dev/null
	wait_until 130 _npcs_walk_over "$1"; still "$1"
}

# npcs_handover <walker> <stayer>: the walker's player walks NPC_WALK away; the NPCs it ran near the stayer go to the
# stayer's game in both books; the walker walks back
npcs_handover() {
	local walker=$1 stayer=$2 label="npcs: the $1 walks away, the $2 takes its NPCs" wid sid p0 sp keys tx ty r moved why
	section "$label"
	wid=$(field player "$(dc "$walker" 'session status')"); sid=$(field player "$(dc "$stayer" 'session status')")
	if ! is_num "$wid" || ! is_num "$sid"; then rig_broken game "$label" "no player ids (walker '$wid', stayer '$sid')"; return; fi
	p0=$(pos_of "$walker"); sp=$(pos_of "$stayer")
	if [ -z "$p0" ] || [ -z "$sp" ]; then rig_broken game "$label" "no place for a player (walker '$p0', stayer '$sp')"; return; fi
	keys=$(npcs_near "$(_npcs_rows "$stayer")" "$sp" "$wid")
	if [ -z "$keys" ]; then
		warn "$label: NPCs to hand over" "the $walker runs none within $NPC_NEAR u of the $stayer's player: nothing to follow this way"
		return
	fi
	say "$label: following $(wc -w <<<"$keys") NPC(s): $keys"
	# away from the stayer, straight on
	read -r tx ty < <(awk -v a="$p0" -v b="$sp" -v w="$NPC_WALK" 'BEGIN { split(a, p, " "); split(b, s, " "); dx = p[1] - s[1]; dy = p[2] - s[2]; l = sqrt(dx * dx + dy * dy); if (l < 1) { dx = 1; dy = 0; l = 1 } printf "%.0f %.0f\n", s[1] + dx / l * w, s[2] + dy / l * w }')
	r=$(dc "$walker" "walkto $tx $ty 1.0 150 120")
	[[ $r == walking* ]] || { rig_broken input "$label: the walk" "the $walker's walkto refused: $r"; return; }
	wait_until 130 _npcs_walk_over "$walker"
	still "$walker"
	moved=$(dist2 "$(pos_of "$walker")" "$sp")
	if ! is_num "$moved" || [ "$moved" -lt $((NPC_WALK * 3 / 4)) ]; then
		rig_fault walk "$label: the walk" "the $walker is ${moved:-?} u from the $stayer after the walk, not about $NPC_WALK ($(dc "$walker" walkto | cut -c1-80))"
		_npcs_walk_back "$walker" "$p0"; return
	fi
	pass "$label: the walk" "the $walker is $moved u from the $stayer"
	# shellcheck disable=SC2086
	if wait_until "$NPC_WAIT" _npcs_handed "$sid" $keys; then
		pass "$label: both books say the $stayer runs them" "$(wc -w <<<"$keys") NPC(s)"
	else
		# shellcheck disable=SC2086
		why="host: $(npcs_owned_by "$(_npcs_rows host)" "$sid" $keys); vm: $(npcs_owned_by "$(_npcs_rows vm)" "$sid" $keys)"
		fail "$label: both books say the $stayer runs them" "after $NPC_WAIT s, not the $stayer's (key:owner): $why"
	fi
	_npcs_walk_back "$walker" "$p0"
}

step_npcs() {
	section "npcs: one runner per NPC, copies where their runners have them"
	local w s a hostRows vmRows hid vid c total=0 hs vs
	for w in host vm; do still $w; done
	for w in host vm; do
		s=$(dc $w npcs)
		expect_eq "$w: the graph event hook is in" "$(field hooked "$s")" 1
		expect_eq "$w: sharing NPCs (in a session)" "$(field active "$s")" 1
	done
	if wait_until "$NPC_WAIT" _npcs_agreed; then
		pass "npcs: every NPC loaded in both has one runner both agree on" "$(npcs_agree "$(_npcs_rows host)" "$(_npcs_rows vm)")"
	else
		a=$(npcs_agree "$(_npcs_rows host)" "$(_npcs_rows vm)")
		if [ "$(field both "$a")" = 0 ]; then
			rig_fault state "npcs: both games have NPCs loaded" "no NPC listed in both ($(dc host npcs | cut -c1-80) / $(dc vm npcs | cut -c1-80)): the cell has none near"
			return
		fi
		fail "npcs: every NPC loaded in both has one runner both agree on" "$a"
	fi
	sleep 2
	hid=$(field player "$(dc host 'session status')"); vid=$(field player "$(dc vm 'session status')")
	hostRows=$(_npcs_rows host); vmRows=$(_npcs_rows vm)
	for w in host vm; do
		if [ $w = host ]; then c=$(npcs_copies "$hostRows" "$vmRows" "$hid"); else c=$(npcs_copies "$vmRows" "$hostRows" "$vid"); fi
		if [ "$(field copies "$c")" = 0 ]; then
			say "npcs: the $w runs none the $(other $w) lists: no copies that way"
			continue
		fi
		total=$((total + $(field copies "$c")))
		expect_ge "npcs: the $(other $w)'s copies of the $w's NPCs stand where the $w has them (within $NPC_COPY_LIMIT u)" "$(field share "$c")" "$NPC_COPY_SHARE" "% ($c)"
	done
	[ "$total" -gt 0 ] || fail "npcs: some NPC is shown as a copy" "neither game shows a copy of an NPC the other runs"
	hs=$(dc host npcs); vs=$(dc vm npcs)
	expect_ge "npcs: people are among the NPCs" "$(npcs_kind human "$hs" "$vs")" 1
	if [ "$(npcs_kind creature "$hs" "$vs")" -ge 1 ]; then pass "npcs: creatures are among the NPCs" "host $(field kinds "$hs")"
	else warn "npcs: creatures are among the NPCs" "none near: host $(field kinds "$hs"), vm $(field kinds "$vs")"; fi
	say "npcs: kinds seen: host $(field kinds "$hs"), vm $(field kinds "$vs"); runtime actors not shared: host $(field unshared "$hs"), vm $(field unshared "$vs")"
	npcs_handover host vm
	npcs_handover vm host
	for w in host vm; do expect_zero "$w: no graph event of an NPC it runs lost" "$(field eventsLost "$(dc $w npcs)")"; done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init npcstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# replies in the format NpcSync::Describe writes
	HOST_LIST='npcs=3 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(100,200,0) dead=0 | 0001A680 local=0001A680 owner=2 role=copy kind=creature at=(500,500,0) dead=0 runner=2 mode=mirror target=(505,500,0) apart=5 base=00023A8F states=2 drawn=40 played=1 refused=0 geared=1 graphSpeed=0 | FF000801 spawner=1 local=FF000801 owner=1 role=run kind=summon at=(150,250,0) dead=0'
	VM_LIST='npcs=3 | 0001A67C local=0001A67C owner=1 role=copy kind=human at=(130,200,0) dead=0 runner=1 mode=mirror target=(128,200,0) apart=2 base=00013BA2 states=2 drawn=40 played=3 refused=1 last=IdleChairSitting geared=1 graphSpeed=150 | 0001A680 local=0001A680 owner=2 role=run kind=creature at=(510,500,0) dead=0 | FF000801 spawner=1 local=FF000AAA owner=1 role=copy kind=? at=(450,250,0) dead=0 runner=1 mode=mirror target=(160,250,0) apart=290 base=000204C0 states=2 drawn=4 played=0 refused=0 geared=0 graphSpeed=none'
	NEAR_LIST=${VM_LIST/at=(450,250,0)/at=(170,250,0)}  # the spawn's copy where the host has it
	H=$(npc_rows "$HOST_LIST"); V=$(npc_rows "$VM_LIST")
	t "rows: one per NPC" "$(wc -l <<<"$H" | tr -d ' ')" 3
	t "rows: a load-order NPC" "$(head -1 <<<"$H")" "0001A67C 1 run human 100 200 0"
	t "rows: a spawn's key names its spawner" "$(sed -n 3p <<<"$H" | cut -d' ' -f1-3)" "FF000801:1 1 run"
	t "rows: a copy" "$(sed -n 2p <<<"$H")" "0001A680 2 copy creature 500 500 0"
	t "rows: an actor not in the world" "$(npc_rows 'npcs=1 | 0001A67C local=00000000 owner=0 role=none kind=? at=none dead=0')" "0001A67C 0 none ? - - -"
	t "rows: the list's end" "$(npc_rows "npcs=61 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(1,2,3) dead=0 | ..." | wc -l | tr -d ' ')" 1
	t "rows: none" "$(npc_rows 'npcs=0')" ""
	t "agree: both books the same" "$(npcs_agree "$H" "$V")" "both=3 agreed=3 bad=none"
	t "agree: a different owner" "$(field bad "$(npcs_agree "$H" "$(sed 's/^0001A680 2/0001A680 1/' <<<"$V")")")" "0001A680:2/1"
	t "agree: nobody's is not agreed" "$(field bad "$(npcs_agree "$(sed 's/^0001A67C 1/0001A67C 0/' <<<"$H")" "$(sed 's/^0001A67C 1/0001A67C 0/' <<<"$V")")")" "0001A67C:0/0"
	t "agree: one only the host lists is not counted" "$(field both "$(npcs_agree "$H" "$(head -1 <<<"$V")")")" 1
	t "agree: no NPCs" "$(npcs_agree "" "")" "both=0 agreed=0 bad=none"
	t "agree: the VM lists none" "$(npcs_agree "$H" "")" "both=0 agreed=0 bad=none"
	t "copies: host's NPCs in the VM (one 30 u off, the spawn's copy 300 off)" "$(npcs_copies "$H" "$V" 1)" "copies=2 within=1 share=50 mean=165 max=300 off=FF000801:1:300"
	t "copies: the VM's NPC in the host" "$(npcs_copies "$V" "$H" 2)" "copies=1 within=1 share=100 mean=10 max=10 off=none"
	t "copies: one not shown as a copy" "$(field off "$(npcs_copies "$V" "$(sed 's/^0001A680 2 copy/0001A680 2 other/' <<<"$H")" 2)")" "0001A680:other"
	S1='active=1 loaded=4 run=2 owners=4 copies=2 shown=2 mirrored=2 spawnCopies=1 kinds=human:3,creature:1,dragon:0,horse:0,follower:0,summon:1 claims=9 eventsLost=0 unshared=0 hooked=1'
	S2='active=1 loaded=3 run=1 owners=4 kinds=human:2,creature:0,dragon:0,horse:1,follower:0,summon:0 hooked=1'
	t "kinds: summed over the games" "$(npcs_kind human "$S1" "$S2")" 5
	t "kinds: horse" "$(npcs_kind horse "$S1" "$S2")" 1
	t "kinds: none" "$(npcs_kind dragon "$S1" "$S2")" 0
	t "near: the host's NPCs near the VM's player, its summon not (it follows the host)" "$(npcs_near "$(npc_rows "$NEAR_LIST")" "120 200 0" 1)" "0001A67C"
	t "near: none that near" "$(NPC_NEAR=5; npcs_near "$V" "120 200 0" 1)" ""
	t "owned: all the VM's" "$(npcs_owned_by "$V" 2 0001A680 && echo yes)" yes
	t "owned: one not" "$(npcs_owned_by "$V" 2 0001A680 0001A67C)" "0001A67C:1"
	t "owned: one gone from the list" "$(npcs_owned_by "$V" 2 00099999)" "00099999:gone"

	# step_npcs on two faked games (the hand-over left out): agreed books and close copies pass; a split book fails
	fake() {  # fake <host list> <vm list>
		FN_H=$1; FN_V=$2
		dc() { case "$2" in
			'npcs list') if [ "$1" = host ]; then echo "$FN_H"; else echo "$FN_V"; fi ;;
			npcs) echo "$S1" ;;
			'session status') if [ "$1" = host ]; then echo 'role=host connection=up welcomed=1 player=1'; else echo 'role=friend connection=up welcomed=1 player=2'; fi ;;
			where) echo 'cell=00000000 world=0000003C pos=(120,200,0) heading=0 modal=0' ;;
			walkto) echo 'not walking' ;;
			*) echo ok ;; esac; }
	}
	run_step() {  # run_step <dir> <host list> <vm list>: the step's lines, then its summary
		mkdir -p "$1"; ( A_OUT=$1; a_init npcs >/dev/null; fake "$2" "$3"; npcs_handover() { :; }; step_npcs >/dev/null; cut -f1,3 "$A_RESULTS" )
	}
	o=$(run_step "$d/n1" "$HOST_LIST" "$NEAR_LIST")
	t "step: agreed books and close copies pass" "$(grep -c '^FAIL' <<<"$o")" 0
	t "step: ... the copies both ways" "$(grep -c $'^PASS\t.*copies of the .* stand where' <<<"$o")" 2
	o=$(run_step "$d/n2" "$HOST_LIST" "${NEAR_LIST/0001A680 local=0001A680 owner=2/0001A680 local=0001A680 owner=1}")
	t "step: two games that disagree on a runner fail" "$(grep -c $'^FAIL\t.*one runner both agree on' <<<"$o")" 1
	o=$(run_step "$d/n3" "$HOST_LIST" "$VM_LIST")
	t "step: a copy 300 u from its runner's NPC fails" "$(grep -c $'^FAIL\t.*the vm.s copies of the host.s NPCs stand where' <<<"$o")" 1
	o=$(run_step "$d/n4" 'npcs=0' 'npcs=0')
	t "step: no NPCs near is the rig's state, not the mod's FAIL" "$(grep -c $'^RIG\t.*both games have NPCs loaded' <<<"$o")" 1
	t "step: ... no FAIL" "$(grep -c '^FAIL' <<<"$o")" 0

	# the hand-over, faked: the walker walks off, then the books say the stayer runs what it ran
	ho() {  # ho <host list after the walk> <vm list after the walk>: the hand-over's verdicts
		HO_N=$d/ho_n; echo 0 > "$HO_N"; HO_AFTER_H=$1; HO_AFTER_V=$2
		dc() { local n; n=$(cat "$HO_N"); case "$2" in
			'npcs list') if [ "$n" -ge 1 ]; then if [ "$1" = host ]; then echo "$HO_AFTER_H"; else echo "$HO_AFTER_V"; fi
				elif [ "$1" = host ]; then echo "$HOST_LIST"; else echo "$NEAR_LIST"; fi ;;
			'session status') if [ "$1" = host ]; then echo 'player=1'; else echo 'player=2'; fi ;;
			where) if [ "$1" = host ] && [ "$n" -ge 1 ]; then echo 'cell=00000000 world=0000003C pos=(9200,200,0) modal=0'; else echo 'cell=00000000 world=0000003C pos=(120,200,0) modal=0'; fi ;;
			'walkto stop') echo stopped ;;
			walkto\ *) echo 1 > "$HO_N"; echo "walking to (x)" ;;
			walkto) echo 'not walking' ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/ho$RANDOM; mkdir -p "$A_OUT"; a_init ho >/dev/null; npcs_handover host vm >/dev/null; cut -f1 "$A_RESULTS" | tr '\n' ' ' )
	}
	HANDED_H=${HOST_LIST/0001A67C local=0001A67C owner=1 role=run/0001A67C local=0001A67C owner=2 role=copy}
	HANDED_V=${NEAR_LIST/0001A67C local=0001A67C owner=1 role=copy/0001A67C local=0001A67C owner=2 role=run}
	t "handover: the walk, then both books the VM's: passes" "$(ho "$HANDED_H" "$HANDED_V")" "PASS PASS "
	t "handover: the host kept it: FAIL" "$(ho "$HOST_LIST" "$NEAR_LIST")" "PASS FAIL "
	t "handover: one book only: FAIL" "$(ho "$HOST_LIST" "$HANDED_V")" "PASS FAIL "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "npcs is a step" "$(case " $STEPS " in *" npcs "*) echo yes ;; esac)" yes
	t "npcs comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' npcs')" 1
	t "npcs needs a session" "$(case " $SESSION_STEPS " in *" npcs "*) echo yes ;; esac)" yes
	t "npcs runs by default" "$(case " $OPTIN_STEPS " in *" npcs "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "npcs.sh selftest: all ok" || echo "npcs.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
