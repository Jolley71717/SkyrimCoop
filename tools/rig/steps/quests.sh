#!/bin/bash
# steps/quests.sh: the rig step `quests` (protocol 7, docs/DESIGN.md "Quests"), sourced by steps.sh. A stage one game
# sets of a shared quest is done in the other, both ways; the quest's objectives read the same in both after its stages;
# after a load and after a rejoin the VM's game takes the session's word; the Helgen opening is each player's own; no
# stage the session's word made moved a player, and none stuck. Not run in a game yet.
#
#   step_quests                                  the step (in the default run, after doors, before final)
#   bash tools/rig/steps/quests.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `quests`, `quests pick <n>` and `quest <editor id>`):
#   1. Both games' quest sync is installed, and each says whether the stage done bit reads as it must (doneBit=trusted:
#      no quest never run reads a stage done, the running ones read their current stage done); untrusted is a WARN, the
#      plugin then goes by what it saw set and by current stages. MQ101 (the Helgen opening) is class=own in both.
#   2. A quest: the VM's `quests pick <n>` names a shared quest not started there with four stages or more past its
#      start-up stage; the first one the host has not started either is taken (QUESTS_QUEST names one instead). Its
#      stages are set in ascending order with the console (`setstage <quest> <stage>`: the input, as a quest's script or
#      a dialogue would set it, said in the report), every time in the game that does the step; the game that set it
#      must read it done itself (RIG otherwise: the console did not take), and the other within QUESTS_WAIT.
#   3. The first stage the VM sets, the host reads done; the second the host sets, the VM reads done.
#   4. Objectives: after QuestRules::SETTLE, the quest's objectives that are not dormant read the same in both games
#      (none shown in either is a WARN: nothing compared).
#   5. The session's word after a load and a rejoin: the VM saves ($SAVE_NAME, as save_rejoin); the host sets the third
#      stage; the VM loads its save (the stage not done in it) and must read it done again (it asked the session again);
#      the VM leaves, the host sets the fourth stage, the VM joins again and must read it done.
#   6. The counts: a stage the session's word made here that moved a player (moved=, the MoveGuard) is a WARN with the
#      stage named (it is a row for QuestRules::STAGE_RULES); a stage made and never read done (stuck=) is a WARN; the
#      console commands run off the game's main thread and the ones the console refused are said.
# The quest stays started in both games for the rest of the run: the host's game never saves, and the VM's test save is
# moved aside at the end as always. A second run picks another quest (the first is started now).

QUESTS_WAIT=${QUESTS_WAIT:-20}      # s: a stage to reach the other game and be made there (STAGE_WAIT is 5 s, a stage at a time)
QUESTS_SETTLE=${QUESTS_SETTLE:-15}  # s: objectives (QuestRules::SETTLE, 5 s after the quest's last stage made, polled every second)
QUESTS_TRIES=${QUESTS_TRIES:-40}    # `quests pick <n>`: how many to look through for one the host has not started either
QUESTS_QUEST=${QUESTS_QUEST:-}      # an editor ID to take instead of the pick (not started in either game)

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# quests_csv_has <a,b,c> <x>: x is one of the list's items
quests_csv_has() { [[ ",$1," == *",$2,"* ]]; }
# quests_sorted <a,b,c>: the list's numbers in ascending order, one space after each
quests_sorted() { tr ',' '\n' <<<"$1" | grep -E '^[0-9]+$' | sort -n | tr '\n' ' '; }
# quests_stage_done <quest reply> <stage>: that game reads the stage done: by the done bit, as seen set there, or as its
# current stage
quests_stage_done() { quests_csv_has "$(field done "$1")" "$2" || quests_csv_has "$(field seen "$1")" "$2" || [ "$(field current "$1")" = "$2" ]; }
# quests_fresh <quest reply>: never started in that game (not running, not completed, at stage 0, no stage done or seen)
quests_fresh() {
	[ "$(field enabled "$1")" = 0 ] && [ "$(field completed "$1")" = 0 ] && [ "$(field current "$1")" = 0 ] &&
		[ "$(field done "$1")" = - ] && [ "$(field seen "$1")" = - ]
}
# quests_shown <objectives=index:state,...>: the objectives that are not dormant, by index, one space after each
quests_shown() { tr ',' '\n' <<<"$1" | grep -E '^[0-9]+:[1-5]$' | sort -t: -k1,1n | tr '\n' ' '; }

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

# quests_pick: "<editor id> <stage> <stage> ..." of a shared quest neither game has started, its stages ascending; 1 when
# there is none
quests_pick() {
	local n r h
	if [ -n "$QUESTS_QUEST" ]; then
		r=$(dc vm "quest $QUESTS_QUEST"); h=$(dc host "quest $QUESTS_QUEST")
		[ "$(field class "$r")" = shared ] && quests_fresh "$r" && quests_fresh "$h" || return 1
		echo "$(field quest "$r") $(quests_sorted "$(field stages "$r" | sed 's/^0,//')")"; return 0
	fi
	for ((n = 0; n < QUESTS_TRIES; n++)); do
		r=$(dc vm "quests pick $n")
		[[ $r == quest=* ]] || return 1
		h=$(dc host "quest $(field quest "$r")")
		[ "$(field class "$h")" = shared ] && quests_fresh "$h" || continue
		echo "$(field quest "$r") $(quests_sorted "$(field stages "$r")")"; return 0
	done
	return 1
}

# quests_run <check> <who> <command> <want>: a dev command whose reply must be <want>; a rig fault otherwise
quests_run() {
	local r; r=$(dc "$2" "$3")
	[ "$r" = "$4" ] && return 0
	rig_broken input "$1" "$2: '$3' replied '$r'"; return 1
}
# quests_has <who> <quest> <stage>: that game reads the stage done
quests_has() { quests_stage_done "$(dc "$1" "quest $2")" "$3"; }
# quests_line <who> <quest>: that game's line of the quest, cut for a report
quests_line() { dc "$1" "quest $2" | cut -c1-240; }
# quests_own <check> <who> <quest> <stage>: the game that set the stage reads it done itself (PASS); when not, the console
# did not take (the rig's input, not the mod: RIG, and the rest of that check is not made)
quests_own() {
	if wait_until "$QUESTS_WAIT" quests_has "$2" "$3" "$4"; then pass "$1" "$(quests_line "$2" "$3" | cut -c1-120)"; return 0; fi
	rig_fault input "$1" "$2 does not read its own stage $4 done: $(quests_line "$2" "$3")"; return 1
}
# quests_expect <check> <who> <quest> <stage>: within QUESTS_WAIT that game reads the stage done (PASS), or FAIL with its line
quests_expect() {
	if wait_until "$QUESTS_WAIT" quests_has "$2" "$3" "$4"; then pass "$1" "$(quests_line "$2" "$3" | cut -c1-120)"; return 0; fi
	fail "$1" "$2 reads: $(quests_line "$2" "$3")"; return 1
}

# quests_move <doer> <quest> <stage>: the doer sets the stage by the console and reads it done; the other game reads it
# done too. When the doer's own game never took it, the other's is not checked (it would only say the same).
quests_move() {
	local doer=$1 quest=$2 stage=$3 watcher; watcher=$(other "$doer")
	quests_run "$doer: sets $quest's stage $stage (console setstage: the input)" "$doer" "console setstage $quest $stage" ran || return 1
	quests_own "$doer: reads $quest's stage $stage done (console setstage: the input)" "$doer" "$quest" "$stage" || return 1
	quests_expect "$watcher: $quest's stage $stage the $doer set is done here too" "$watcher" "$quest" "$stage"
}

# quests_objectives <quest>: once the quest's stages have settled, its objectives that are not dormant read the same in
# both games
_quests_same_objectives() { [ "$(quests_shown "$(field objectives "$(dc host "quest $1")")")" = "$(quests_shown "$(field objectives "$(dc vm "quest $1")")")" ]; }
quests_objectives() {
	local h v
	wait_until "$QUESTS_SETTLE" _quests_same_objectives "$1"
	h=$(quests_shown "$(field objectives "$(dc host "quest $1")")"); v=$(quests_shown "$(field objectives "$(dc vm "quest $1")")")
	if [ -z "$h" ] && [ -z "$v" ]; then
		warn "both: $1 shows objectives after its stages" "none shown in either game: nothing compared"; return 0
	fi
	expect_eq "vm: $1's objectives read as the host's (shown, completed, failed)" "$v" "$h"
}

# quests_status <who>: its quest sync is installed, and whether its stage done bit reads as it must
quests_status() {
	local s; s=$(dc "$1" quests)
	[ "$(field installed "$s")" = 1 ] || { rig_broken game "$1: its quest sync is installed" "$(cut -c1-160 <<<"$s")"; return 1; }
	pass "$1: its quest sync is installed" "$(cut -c1-120 <<<"$s")"
	if [ "$(field doneBit "$s")" = trusted ]; then
		pass "$1: the stage done bit reads as it must (no quest never run has a stage done, running ones their current stage)" "doneBit=trusted"
	else
		warn "$1: the stage done bit reads as it must" "doneBit=$(field doneBit "$s"): the plugin goes by the stages it saw set and the current ones"
	fi
}

# quests_counts <who>: no stage the session's word made moved its player, none stuck; what the console did, said
quests_counts() {
	local s; s=$(dc "$1" quests)
	if [ "$(field moved "$s")" = 0 ]; then pass "$1: no stage the session's word made moved its player" "moved=0"
	else warn "$1: no stage the session's word made moved its player" "moved=$(field moved "$s") last=$(field lastMoved "$s"): a row for the stage rules"; fi
	if [ "$(field stuck "$s")" = 0 ]; then pass "$1: every stage made here read done" "stuck=0"
	else warn "$1: every stage made here read done" "stuck=$(field stuck "$s") last=$(field lastStuck "$s")"; fi
	echo "     $1: $(tr ' ' '\n' <<<"$s" | grep -E '^(made|has|missing|nothere|busy|refused|asks|resets|snapshots|offmain|norun|wantsDropped|eventsDropped)=' | tr '\n' ' ')"
}

# quests_again <quest> <stage> <stage>: the session's word after the VM loads a save without the first stage, and after
# it rejoins while the host set the second
quests_again() {
	local quest=$1 s3=$2 s4=$3 asks0 r
	quests_run "vm: saves before the host's stage $s3" vm "console save $SAVE_NAME" ran || return
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return; }
	quests_move host "$quest" "$s3" || return

	asks0=$(field asks "$(dc vm quests)")
	quests_run "vm: loads its save (stage $s3 not done in it)" vm "console load $SAVE_NAME" ran || return
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; }
	wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return; }
	quests_expect "vm: after loading a save without it, $quest's stage $s3 is done again (the session's word)" vm "$quest" "$s3"
	if is_num "$asks0"; then expect_ge "vm: asked the session for its quests again after the load" "$(field asks "$(dc vm quests)")" $((asks0 + 1))
	else fail "vm: asked the session for its quests again after the load" "no asks= before: $(dc vm quests | cut -c1-160)"; fi

	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	gone_from host || fail "host: the VM's stand-in goes when it leaves" "$(dc host standins | cut -c1-160)"
	quests_run "host: sets $quest's stage $s4 while the VM is away (console setstage: the input)" host "console setstage $quest $s4" ran || return
	quests_own "host: reads $quest's stage $s4 done (console setstage: the input)" host "$quest" "$s4" || return
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	quests_expect "vm: after joining again, $quest's stage $s4 the host set while it was away is done" vm "$quest" "$s4"
}

step_quests() {
	local w picked quest stages
	section "quests: both games' quest sync, and which quests are each player's own"
	for w in host vm; do quests_status $w || return; done
	for w in host vm; do expect_eq "$w: MQ101 (the Helgen opening) is each player's own" "$(field class "$(dc $w 'quest MQ101')")" own; done

	section "quests: a stage one game sets is done in the other"
	if ! picked=$(quests_pick); then
		rig_unhealed state "both: a shared quest neither game has started" "none in the VM's first $QUESTS_TRIES picks ($(dc vm 'quests pick 0' | cut -c1-120))"
		return
	fi
	read -r quest stages <<<"$picked"
	# shellcheck disable=SC2206
	stages=($stages)
	[ ${#stages[@]} -ge 4 ] || { rig_unhealed state "both: a quest with four stages to set" "$picked"; return; }
	echo "     quest: $quest, stages ${stages[*]:0:4}"
	expect_eq "vm: $quest is shared" "$(field class "$(dc vm "quest $quest")")" shared
	quests_move vm "$quest" "${stages[0]}" &&
		quests_move host "$quest" "${stages[1]}" && {
			section "quests: the quest's objectives read the same in both"
			quests_objectives "$quest"
			section "quests: the session's word after the VM loads a save and after it joins again"
			quests_again "$quest" "${stages[2]}" "${stages[3]}"
		}

	section "quests: no stage moved a player, none stuck"
	for w in host vm; do quests_counts $w; done
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init queststest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	QUESTS_WAIT=2; QUESTS_SETTLE=2

	Q='quest=DA03 id=00022F08 type=7 class=shared enabled=1 completed=0 failed=0 current=20 stages=0,10,20,30 done=10,20 seen=20 objectives=10:3,20:1,30:0 session=10,20 sessionObjectives=10:3 sessionEnd=-'
	t "csv: has an item" "$(quests_csv_has 10,20,30 20 && echo yes)" yes
	t "csv: not a part of one" "$(quests_csv_has 10,200 20 || echo no)" no
	t "csv: none" "$(quests_csv_has - 0 || echo no)" no
	t "sorted: numbers ascending" "$(quests_sorted 40,10,200,30)" "10 30 40 200 "
	t "sorted: none" "$(quests_sorted -)" ""
	t "done: by the bit" "$(quests_stage_done "$Q" 10 && echo yes)" yes
	t "done: not a stage never set" "$(quests_stage_done "$Q" 30 || echo no)" no
	t "done: as seen set" "$(quests_stage_done "${Q/done=10,20/done=-}" 20 && echo yes)" yes
	t "done: as the current stage" "$(quests_stage_done "${Q/seen=20/seen=-}" 20 && echo yes)" yes
	t "done: an error reply is not" "$(quests_stage_done 'error: no such quest here' 10 || echo no)" no
	t "fresh: a running quest is not" "$(quests_fresh "$Q" || echo no)" no
	F='quest=DA03 id=00022F08 type=7 class=shared enabled=0 completed=0 failed=0 current=0 stages=0,10,20,30 done=- seen=- objectives=- session=- sessionObjectives=- sessionEnd=-'
	t "fresh: never started" "$(quests_fresh "$F" && echo yes)" yes
	t "fresh: a stage read done is not" "$(quests_fresh "${F/done=-/done=10}" || echo no)" no
	t "shown: not the dormant ones, by index" "$(quests_shown 30:0,20:1,10:3,5:5)" "5:5 10:3 20:1 "
	t "shown: none" "$(quests_shown -)" ""

	# Both games faked: each holds the stages done of quest DA03 and its objectives; `console setstage` sets a stage in its
	# game, and with SHARE=1 (the mod at work) in the other too; HOSTFRESH=0 makes the host's DA03 started already (the
	# pick goes on to the next one, DB05).
	fake() {  # fake <share> [host fresh]
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; HOSTFRESH=${2:-1}
		for w in host vm; do echo - > "$FK/$w.done"; echo - > "$FK/$w.obj"; done
	}
	adds() { local f=$FK/$1.done; if [ "$(cat "$f")" = - ]; then echo "$2" > "$f"; else echo "$(cat "$f"),$2" > "$f"; fi; }
	setstage() {
		adds "$1" "$2"; [ "$SHARE" = 1 ] && adds "$(other "$1")" "$2"
		# stage 20's fragment shows objective 20, in each game that runs it
		if [ "$2" = 20 ]; then echo 10:3,20:1 > "$FK/$1.obj"; [ "$SHARE" = 1 ] && echo 10:3,20:1 > "$FK/$(other "$1").obj"; fi
		return 0
	}
	dc() {
		local w=$1 c=$2 done
		case "$c" in
			"quests pick 0") echo "quest=DA03 id=00022F08 type=7 stages=40,10,20,30" ;;
			"quests pick 1") echo "quest=DB05 id=0001EA51 type=4 stages=10,20,30,40,50" ;;
			"quests pick "*) echo "error: no ${c#quests pick }-th shared quest not started here with four stages" ;;
			"quest DA03"|"quest DB05")
				done=$(cat "$FK/$w.done")
				if [ "$w" = host ] && [ "$HOSTFRESH" = 0 ] && [ "$c" = "quest DA03" ]; then done=5; fi
				echo "quest=${c#quest } id=00022F08 type=7 class=shared enabled=$([ "$done" = - ] && echo 0 || echo 1) completed=0 failed=0 current=0 stages=0,10,20,30,40 done=$done seen=- objectives=$(cat "$FK/$w.obj") session=- sessionObjectives=- sessionEnd=-" ;;
			"quest MQ101") echo "quest=MQ101 id=0003372B type=1 class=own enabled=0 completed=1 failed=0 current=0" ;;
			"console setstage DA03 "*|"console setstage DB05 "*) setstage "$w" "${c##* }"; echo ran ;;
			quests) echo "installed=1 host=$([ "$w" = host ] && echo 1 || echo 0) quests=812 doneBit=trusted moved=0 lastMoved=- stuck=0 lastStuck=none asks=1 made=2 offmain=0 norun=0" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init qt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; t "pick: the first the host has not started either, stages ascending" "$(quests_pick)" "DA03 10 20 30 40 "
	fake 1 0; t "pick: one the host started is passed over" "$(quests_pick)" "DB05 10 20 30 40 50 "
	fake 1; QUESTS_TRIES=1; HOSTFRESH=0; t "pick: none within the tries" "$(quests_pick || echo none)" none; QUESTS_TRIES=40
	fake 1; QUESTS_QUEST=DB05; t "pick: the one named" "$(quests_pick)" "DB05 10 20 30 40 "; QUESTS_QUEST=

	fake 1; o=$(run 1 quests_move vm DA03 10)
	t "move: shared passes" "$(verdicts "$o")" "PASS=2 "
	t "move: ... both read it" "$(cat "$FK/host.done")$(cat "$FK/vm.done")" 1010
	fake 0; o=$(run 2 quests_move vm DA03 10)
	t "move: not shared fails on the watcher" "$(grep -c '^FAIL host: DA03.s stage 10 the vm set is done here too' <<<"$o")" 1
	fake 1; o=$(run 3 quests_move host DA03 20)
	t "move: the host's way" "$(grep -c '^PASS vm: DA03.s stage 20 the host set' <<<"$o")" 1
	# a console the game did not take (its own stage not done) is the rig's input: RIG, never FAIL
	fake 1; dc() { case "$2" in "console setstage"*) echo ran ;; "quest DA03") echo "quest=DA03 enabled=0 current=0 done=- seen=-" ;; esac; }
	o=$(run 4 quests_move vm DA03 10)
	t "move: the VM's own stage not done is RIG, not FAIL" "$(grep -c '^RIG .*vm: reads DA03.s stage 10 done' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0
	dc() { case "$2" in "console setstage"*) echo "error: not in the world" ;; esac; }
	o=$(run 5 quests_move vm DA03 10)
	t "move: a console the dev channel refused is the rig broken, and no more is checked" "$(grep -c '^FAIL .*rig broken (input)' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,1

	obj() {  # obj <host objectives> <vm objectives> <n>
		O_H=$1 O_V=$2
		dc() { case "$2" in "quest DA03") if [ "$1" = host ]; then echo "quest=DA03 objectives=$O_H"; else echo "quest=DA03 objectives=$O_V"; fi ;; esac; }
		run "o$3" quests_objectives DA03
	}
	o=$(obj 10:3,20:1 20:1,10:3,30:0 1); t "objectives: the same shown ones pass, in any order, dormant ones aside" "$(verdicts "$o")" "PASS=1 "
	o=$(obj 10:3,20:1 10:1 2); t "objectives: behind fails" "$(grep -c '^FAIL vm: DA03.s objectives' <<<"$o")" 1
	o=$(obj - 10:0 3); t "objectives: none shown is a WARN, not a PASS" "$(verdicts "$o")" "WARN=1 "

	st() {  # st <quests reply> <n>: quests_status and quests_counts of the host on that reply
		S_R=$1; dc() { echo "$S_R"; }
		mkdir -p "$d/s$2"; ( A_OUT=$d/s$2; a_init qs >/dev/null; quests_status host; quests_counts host; a_summary ) 2>&1
	}
	o=$(st 'installed=1 doneBit=trusted moved=0 stuck=0' 1); t "status: all well passes" "$(verdicts "$o")" "PASS=4 "
	o=$(st 'installed=1 doneBit=untrusted moved=2 lastMoved=000E3D8D:30 stuck=1 lastStuck=000E3D8D:20' 2)
	t "status: an untrusted bit, a move and a stuck stage are WARNs" "$(verdicts "$o")" "PASS=1 WARN=3 "
	t "status: ... the moving stage named" "$(grep -c '^WARN host: no stage .* last=000E3D8D:30' <<<"$o")" 1
	o=$(st 'error: no such command' 3); t "status: no quest sync is the rig broken" "$(grep -c '^FAIL .*rig broken (game)' <<<"$o")" 1

	# the step is wired into the harness: in STEPS after doors and before final, a session step, in the default run
	t "quests is a step, after doors, before final" "$(case " $STEPS " in *" doors "*" quests "*" final "*) echo yes;; esac)" yes
	t "quests needs a session" "$(case " $SESSION_STEPS " in *" quests "*) echo yes;; esac)" yes
	t "quests runs by default" "$(case " $OPTIN_STEPS " in *" quests "*) echo no;; *) echo yes;; esac)" yes
	t "quests has its line" "$([ -n "${STEP_INFO[quests]:-}" ] && echo yes)" yes
	t "quests is not run twice (it sets stages for good)" "$(grep -qE 'RERUN_STEPS=.*quests' "$(dirname "${BASH_SOURCE[0]}")/../smoke.sh" && echo yes || echo no)" no
	[ $bad -eq 0 ] && echo "quests.sh selftest: all ok" || echo "quests.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
