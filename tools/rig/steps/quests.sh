#!/bin/bash
# steps/quests.sh: the rig step `quests` (protocol 7, docs/DESIGN.md "Quests"), sourced by steps.sh. A stage, an objective
# and a quest's end one game reaches are made in the other by the session's word, both ways; after a load, a rejoin and
# the host hosting again the VM's game takes the session's word; the Helgen opening is each player's own; no stage the
# session's word made moved a player, and none stuck. Not run in a game yet.
#
#   step_quests                                  the step (in the default run, after doors, before final)
#   bash tools/rig/steps/quests.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `quests`, `quests pick <n>` and `quest <editor id>`). Every
# "made here" check reads both the watcher's dev channel (its made= count went up) and its plugin log (the line "Quests:
# made <quest> stage <n> here", "... objective <i> ...", "Quests: completed <quest> here"), so a watcher whose own
# scripts did the same (a stage's fragment setting the next stage) never passes for the session's word.
#   1. Both games' quest sync is installed, and each says whether the stage done bit reads as it must (doneBit=trusted:
#      no quest never run reads a stage done, the running ones read their current stage done); untrusted is a WARN, the
#      plugin then goes by what it saw set and by current stages. MQ101 (the Helgen opening) is class=own in both.
#   2. A quest: the VM's `quests pick <n>` names a shared quest not started there with six stages or more past its
#      start-up stage; the first one the host has not started either is taken (QUESTS_QUEST names one instead). Its
#      stages are set in ascending order with the console (`setstage <quest> <stage>`: the input, as a quest's script or
#      a dialogue would set it, said in the report), every time in the game that does the step; a stage one of the two
#      games has done already (its fragments set it) is passed over. When the VM's first stage does not take there (its
#      aliases may not fill in a world reached by coc), the next pick is tried, QUESTS_PICKS of them.
#   3. The first stage the VM sets, the host makes; the next the host sets, the VM makes.
#   4. Objectives: after QuestRules::SETTLE the quest's objectives that are not dormant read the same in both (none shown
#      is a WARN). Then two objectives dormant in both: the VM shows the first (`setobjectivedisplayed`) and completes it
#      (`setobjectivecompleted`), and the host makes each; the host does the same with the second, and the VM makes each.
#   5. The session's word after a load and a rejoin: the VM saves ($SAVE_NAME, as save_rejoin); the host sets the next
#      stage (the VM makes it); the VM loads its save (the stage not done in it) and makes it again from the session's
#      word (it asked again); the VM leaves, the host sets the next stage, the VM joins again and makes it.
#   6. The host's snapshot: the host ends the session and sets the next stage with no session; it hosts again (its
#      snapshot: snapshots= up) and the VM joins and makes that stage from it.
#   7. Ends, both ways: the VM completes the quest (`completequest`) and the host makes it; a second quest is started by
#      the host (the VM makes its first stage) and completed by the host, and the VM makes that.
#   8. The counts: a stage the session's word made here that moved a player (moved=, the MoveGuard) is a WARN with the
#      stage named (it is a row for QuestRules::STAGE_RULES); a stage made and never read done (stuck=) and something made
#      that the game did not read so after (unmade=: a command the console could not compile) are WARNs; the console
#      commands run off the game's main thread and the ones the game made no script for are said.
# The quests stay started (or completed) in both games for the rest of the run: the host's game never saves, and the
# VM's test save is moved aside at the end as always. A second run picks other quests (these are started now).

QUESTS_WAIT=${QUESTS_WAIT:-20}      # s: a stage to reach the other game and be made there (STAGE_WAIT is 5 s, a stage at a time)
QUESTS_SETTLE=${QUESTS_SETTLE:-15}  # s: objectives and ends (QuestRules::SETTLE, 5 s after the quest's last stage made, polled every second)
QUESTS_TRIES=${QUESTS_TRIES:-40}    # `quests pick <n>`: how many to look through for one the host has not started either
QUESTS_PICKS=${QUESTS_PICKS:-5}     # picks whose first stage does not take in the game that sets it, before the step gives up (RIG)
QUESTS_QUEST=${QUESTS_QUEST:-}      # an editor ID to take instead of the first pick (not started in either game)
QUESTS_SKIP=""                      # the quests picked already in this run (passed over by the next pick)

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
# quests_obj_state <objectives=index:state,...> <index>: that objective's state (0 when not listed)
quests_obj_state() { tr ',' '\n' <<<"$1" | awk -F: -v i="$2" '$1 == i { s = $2 } END { print (s == "" ? 0 : s) }'; }
# quests_dormant <objectives> <objectives>: the objectives listed dormant in the first and dormant (or not listed) in the
# second, by index ascending, one space after each
quests_dormant() {
	local i
	for i in $(tr ',' '\n' <<<"$1" | grep -E '^[0-9]+:0$' | cut -d: -f1 | sort -n); do
		[ "$(quests_obj_state "$2" "$i")" = 0 ] && echo -n "$i "
	done
}
# quests_number <a/b>: the a of a count written "a/b" (snapshots=)
quests_number() { echo "${1%%/*}"; }

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

# quests_pick: "<editor id> <stage> <stage> ..." of a shared quest neither game has started and not picked before in
# this run, its stages ascending; 1 when there is none
quests_pick() {
	local n r h name
	if [ -n "$QUESTS_QUEST" ] && ! quests_csv_has "$(tr ' ' ',' <<<"$QUESTS_SKIP")" "$QUESTS_QUEST"; then
		r=$(dc vm "quest $QUESTS_QUEST"); h=$(dc host "quest $QUESTS_QUEST")
		[ "$(field class "$r")" = shared ] && quests_fresh "$r" && quests_fresh "$h" || return 1
		echo "$(field quest "$r") $(quests_sorted "$(field stages "$r" | sed 's/^0,//')")"; return 0
	fi
	for ((n = 0; n < QUESTS_TRIES; n++)); do
		r=$(dc vm "quests pick $n")
		[[ $r == quest=* ]] || return 1
		name=$(field quest "$r")
		case " $QUESTS_SKIP " in *" $name "*) continue ;; esac
		h=$(dc host "quest $name")
		[ "$(field class "$h")" = shared ] && quests_fresh "$h" || continue
		echo "$name $(quests_sorted "$(field stages "$r")")"; return 0
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
# quests_past <who> <quest> <stage>: that game's quest is at that stage or a later one (setting it would be out of order)
quests_past() { local c; c=$(field current "$(dc "$1" "quest $2")"); is_num "$c" && [ "$c" -ge "$3" ]; }
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

# quests_made <who>: that game's made= count; quests_said <who> <pattern>: the lines of its plugin log since the run's mark
# that match
quests_made() { field made "$(dc "$1" quests)"; }
quests_said() { log_since_mark "$1" "$2" | wc -l | tr -d ' '; }
# quests_before <who> <pattern>: "made lines" now, to compare after (quests_by_session)
quests_before() { echo "$(quests_made "$1") $(quests_said "$1" "$2")"; }
# quests_by_session <who> <what> <before: "made lines"> <pattern>: that game made it from the session's word: its made=
# went up and its plugin log has one more line saying so
quests_by_session() {
	local made lines made0 lines0
	read -r made0 lines0 <<<"$3"
	made=$(quests_made "$1"); lines=$(quests_said "$1" "$4")
	if is_num "$made0" && is_num "$made" && [ "$made" -gt "$made0" ] && is_num "$lines" && [ "$lines" -gt "${lines0:-0}" ]; then
		pass "$1: $2 was made here by the session's word (not by its own scripts)" "made=$made0->$made; its log: '$4'"
	else
		fail "$1: $2 was made here by the session's word (not by its own scripts)" "made=$made0->$made; log lines '$4': ${lines0:-?}->$lines"
	fi
}

# quests_take <quest>: QNEXT = the next stage of QSTAGES that neither game has done or gone past (a stage the quest's own
# fragments set is passed over, never set again); 1 when none is left
quests_take() {
	local s
	while [ ${#QSTAGES[@]} -gt 0 ]; do
		s=${QSTAGES[0]}; QSTAGES=("${QSTAGES[@]:1}")
		quests_has host "$1" "$s" || quests_has vm "$1" "$s" || quests_past host "$1" "$s" || quests_past vm "$1" "$s" || { QNEXT=$s; return 0; }
	done
	return 1
}
# quests_next <quest> <what for>: quests_take, or a rig fault (the quest's own scripts set the rest of its stages)
quests_next() {
	quests_take "$1" && return 0
	rig_fault state "both: $1 has a stage left to set ($2)" "none left: the quest's own scripts set the rest of the picked stages"; return 1
}

# quests_leg <doer> <quest> <stage>: the doer sets the stage by the console and reads it done; the other game reads it done
# too, made there by the session's word. When the doer's own game never took it, the other's is not checked.
quests_leg() {
	local doer=$1 quest=$2 stage=$3 watcher before pat
	watcher=$(other "$doer"); pat="Quests: made $quest stage $stage here"
	before=$(quests_before "$watcher" "$pat")
	quests_run "$doer: sets $quest's stage $stage (console setstage: the input)" "$doer" "console setstage $quest $stage" ran || return 1
	quests_own "$doer: reads $quest's stage $stage done (console setstage: the input)" "$doer" "$quest" "$stage" || return 1
	quests_expect "$watcher: $quest's stage $stage the $doer set is done here too" "$watcher" "$quest" "$stage" || return 1
	quests_by_session "$watcher" "$quest's stage $stage" "$before" "$pat"
}

# quests_start <doer>: QUEST = a shared quest neither game has started, its first stage set by the doer (and made in the
# other); QSTAGES its stages left. A pick whose first stage does not take in the doer is passed over for the next.
quests_start() {
	local doer=$1 watcher picked quest stages s tries before pat
	watcher=$(other "$doer")
	for ((tries = 0; tries < QUESTS_PICKS; tries++)); do
		if ! picked=$(quests_pick); then
			rig_unhealed state "both: a shared quest neither game has started" "none in the VM's first $QUESTS_TRIES picks (passed over:${QUESTS_SKIP:- none}; $(dc vm 'quests pick 0' | cut -c1-80))"
			return 1
		fi
		read -r quest stages <<<"$picked"
		QUESTS_SKIP="$QUESTS_SKIP $quest"
		# shellcheck disable=SC2206
		QSTAGES=($stages)
		[ ${#QSTAGES[@]} -ge 4 ] || continue
		s=${QSTAGES[0]}; QSTAGES=("${QSTAGES[@]:1}")
		pat="Quests: made $quest stage $s here"; before=$(quests_before "$watcher" "$pat")
		quests_run "$doer: sets $quest's stage $s (console setstage: the input)" "$doer" "console setstage $quest $s" ran || return 1
		if ! wait_until "$QUESTS_WAIT" quests_has "$doer" "$quest" "$s"; then
			echo "     $quest: the $doer's setstage $s did not take there (its aliases may not fill in this world): the next pick"
			continue
		fi
		QUEST=$quest
		echo "     quest: $quest, stages $s ${QSTAGES[*]}"
		expect_eq "$doer: $quest is shared" "$(field class "$(dc "$doer" "quest $quest")")" shared
		pass "$doer: reads $quest's stage $s done (console setstage: the input)" "$(quests_line "$doer" "$quest" | cut -c1-120)"
		quests_expect "$watcher: $quest's stage $s the $doer set is done here too" "$watcher" "$quest" "$s" &&
			quests_by_session "$watcher" "$quest's stage $s" "$before" "$pat"
		return 0
	done
	rig_unhealed state "both: a shared quest whose first stage takes in the $doer" "$QUESTS_PICKS picks did not take:$QUESTS_SKIP"
	return 1
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

# quests_obj <who> <quest> <index>: that objective's state in that game
quests_obj() { quests_obj_state "$(field objectives "$(dc "$1" "quest $2")")" "$3"; }
_quests_obj_is() { [[ $(quests_obj "$1" "$2" "$3") =~ ^($4)$ ]]; }
# quests_objective_leg <doer> <quest> <index>: the doer shows the objective, then completes it; the other game makes each
# from the session's word. Both games must be at one stage of the quest (an objective's shown part is made only there).
quests_objective_leg() {
	local doer=$1 quest=$2 i=$3 watcher before pat hc vc
	watcher=$(other "$doer")
	hc=$(field current "$(dc host "quest $quest")"); vc=$(field current "$(dc vm "quest $quest")")
	if [ "$hc" != "$vc" ]; then
		rig_fault state "both: at one stage of $quest for its objectives" "host at $hc, vm at $vc: an objective's shown part is made only at the stage it was read at"; return 1
	fi
	pat="Quests: made $quest objective $i state"
	before=$(quests_before "$watcher" "$pat")
	quests_run "$doer: shows $quest's objective $i (console setobjectivedisplayed: the input)" "$doer" "console setobjectivedisplayed $quest $i 1" ran || return 1
	if ! wait_until "$QUESTS_WAIT" _quests_obj_is "$doer" "$quest" "$i" '1|3|5'; then
		rig_fault input "$doer: reads $quest's objective $i shown (console setobjectivedisplayed: the input)" "$(quests_line "$doer" "$quest")"; return 1
	fi
	if wait_until "$QUESTS_SETTLE" _quests_obj_is "$watcher" "$quest" "$i" '1|3|5'; then
		pass "$watcher: $quest's objective $i the $doer showed is shown here too" "state $(quests_obj "$watcher" "$quest" "$i")"
		quests_by_session "$watcher" "$quest's objective $i shown" "$before" "$pat"
	else
		fail "$watcher: $quest's objective $i the $doer showed is shown here too" "$watcher reads: $(quests_line "$watcher" "$quest")"; return 1
	fi
	before=$(quests_before "$watcher" "$pat")
	quests_run "$doer: completes $quest's objective $i (console setobjectivecompleted: the input)" "$doer" "console setobjectivecompleted $quest $i 1" ran || return 1
	if ! wait_until "$QUESTS_WAIT" _quests_obj_is "$doer" "$quest" "$i" '2|3'; then
		rig_fault input "$doer: reads $quest's objective $i completed (console setobjectivecompleted: the input)" "$(quests_line "$doer" "$quest")"; return 1
	fi
	if wait_until "$QUESTS_SETTLE" _quests_obj_is "$watcher" "$quest" "$i" '2|3'; then
		pass "$watcher: $quest's objective $i the $doer completed is completed here too" "state $(quests_obj "$watcher" "$quest" "$i")"
		quests_by_session "$watcher" "$quest's objective $i completed" "$before" "$pat"
	else
		fail "$watcher: $quest's objective $i the $doer completed is completed here too" "$watcher reads: $(quests_line "$watcher" "$quest")"
	fi
}

# quests_objective_legs <quest>: two objectives dormant in both games, one each way
quests_objective_legs() {
	local quest=$1 dormant
	# shellcheck disable=SC2207
	dormant=($(quests_dormant "$(field objectives "$(dc host "quest $quest")")" "$(field objectives "$(dc vm "quest $quest")")"))
	if [ ${#dormant[@]} -lt 2 ]; then
		warn "both: $quest has two objectives dormant in both games" "${#dormant[@]} (${dormant[*]:-none}): the objective legs that need one are not made"
	fi
	[ ${#dormant[@]} -ge 1 ] && quests_objective_leg vm "$quest" "${dormant[0]}"
	[ ${#dormant[@]} -ge 2 ] && quests_objective_leg host "$quest" "${dormant[1]}"
	return 0
}

# quests_end_leg <doer> <quest>: the doer completes the quest (console completequest); the other game makes it
quests_end_leg() {
	local doer=$1 quest=$2 watcher before pat
	watcher=$(other "$doer"); pat="Quests: completed $quest here"
	before=$(quests_before "$watcher" "$pat")
	quests_run "$doer: completes $quest (console completequest: the input)" "$doer" "console completequest $quest" ran || return 1
	if ! wait_until "$QUESTS_WAIT" _quests_completed "$doer" "$quest"; then
		rig_fault input "$doer: reads $quest completed (console completequest: the input)" "$(quests_line "$doer" "$quest")"; return 1
	fi
	if wait_until $((QUESTS_SETTLE + QUESTS_WAIT)) _quests_completed "$watcher" "$quest"; then
		pass "$watcher: $quest the $doer completed is completed here too" "$(quests_line "$watcher" "$quest" | cut -c1-120)"
		quests_by_session "$watcher" "$quest completed" "$before" "$pat"
	else
		fail "$watcher: $quest the $doer completed is completed here too" "$watcher reads: $(quests_line "$watcher" "$quest")"
	fi
}
_quests_completed() { [ "$(field completed "$(dc "$1" "quest $2")")" = 1 ]; }

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

# quests_counts <who>: no stage the session's word made moved its player, none stuck, nothing made that did not take; what
# the console did, said
quests_counts() {
	local s; s=$(dc "$1" quests)
	if [ "$(field moved "$s")" = 0 ]; then pass "$1: no stage the session's word made moved its player" "moved=0"
	else warn "$1: no stage the session's word made moved its player" "moved=$(field moved "$s") last=$(field lastMoved "$s"): a row for the stage rules"; fi
	if [ "$(field stuck "$s")" = 0 ]; then pass "$1: every stage made here read done" "stuck=0"
	else warn "$1: every stage made here read done" "stuck=$(field stuck "$s") last=$(field lastStuck "$s")"; fi
	if [ "$(field unmade "$s")" = 0 ]; then pass "$1: every objective and end made here read so after" "unmade=0"
	else warn "$1: every objective and end made here read so after" "unmade=$(field unmade "$s") last=$(field lastUnmade "$s"): the console did not take the command"; fi
	echo "     $1: $(tr ' ' '\n' <<<"$s" | grep -E '^(made|has|missing|nothere|passed|notStarted|busy|refused|kept|asks|resets|rebases|snapshots|apart|apartStages|offmain|norun|wantsDropped|eventsDropped)=' | tr '\n' ' ')"
}

# quests_again <quest>: the session's word after the VM loads a save without the next stage, and after it rejoins while the
# host set the one after
quests_again() {
	local quest=$1 asks0 r before pat
	quests_run "vm: saves before the host's next stage" vm "console save $SAVE_NAME" ran || return
	sleep 8
	[ "$(dc vm ping)" = pong ] || { fail "vm: the game lives through its save" "its dev channel went silent"; return; }
	quests_next "$quest" "after the VM's save" || return
	local s3=$QNEXT
	quests_leg host "$quest" "$s3" || return

	asks0=$(field asks "$(dc vm quests)")
	pat="Quests: made $quest stage $s3 here"; before=$(quests_before vm "$pat")
	quests_run "vm: loads its save (stage $s3 not done in it)" vm "console load $SAVE_NAME" ran || return
	sleep 5
	wait_until 90 in_world vm || { fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; }
	wait_until 30 welcomed vm || { fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"; return; }
	quests_expect "vm: after loading a save without it, $quest's stage $s3 is done again (the session's word)" vm "$quest" "$s3" &&
		quests_by_session vm "$quest's stage $s3, after its load" "$before" "$pat"
	if is_num "$asks0"; then expect_ge "vm: asked the session for its quests again after the load" "$(field asks "$(dc vm quests)")" $((asks0 + 1))
	else fail "vm: asked the session for its quests again after the load" "no asks= before: $(dc vm quests | cut -c1-160)"; fi

	quests_next "$quest" "while the VM is away" || return
	local s4=$QNEXT
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	gone_from host || fail "host: the VM's stand-in goes when it leaves" "$(dc host standins | cut -c1-160)"
	pat="Quests: made $quest stage $s4 here"; before=$(quests_before vm "$pat")
	quests_run "host: sets $quest's stage $s4 while the VM is away (console setstage: the input)" host "console setstage $quest $s4" ran || return
	quests_own "host: reads $quest's stage $s4 done (console setstage: the input)" host "$quest" "$s4" || return
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	quests_expect "vm: after joining again, $quest's stage $s4 the host set while it was away is done" vm "$quest" "$s4" &&
		quests_by_session vm "$quest's stage $s4, after joining again" "$before" "$pat"
}

# quests_snapshot <quest>: the host ends the session, sets the next stage with none, and hosts again: its snapshot (the
# session's new book) carries the stage, and the VM that joins makes it
quests_snapshot() {
	local quest=$1 r snaps0 before pat s
	quests_next "$quest" "for the host's snapshot" || return
	s=$QNEXT
	snaps0=$(quests_number "$(field snapshots "$(dc host quests)")")
	r=$(dc host 'session leave'); [[ $r == left* ]] || { fail "host: ends the session" "$r"; return; }
	gone_from vm || fail "vm: the host's stand-in goes when the host ends the session" "$(dc vm standins | cut -c1-160)"
	quests_run "host: sets $quest's stage $s with no session (console setstage: the input)" host "console setstage $quest $s" ran || return
	quests_own "host: reads $quest's stage $s done (console setstage: the input)" host "$quest" "$s" || return
	pat="Quests: made $quest stage $s here"; before=$(quests_before vm "$pat")
	dc vm 'session leave' >/dev/null; dc host 'steam stophost' >/dev/null; sleep 3
	if r=$(session_up); then pass "both: in one session again (the host hosting anew)" ""; else fail "both: in one session again (the host hosting anew)" "$r"; return; fi
	if is_num "$snaps0"; then expect_ge "host: said its world's quests when it hosted again (its snapshot)" "$(quests_number "$(field snapshots "$(dc host quests)")")" $((snaps0 + 1))
	else fail "host: said its world's quests when it hosted again (its snapshot)" "no snapshots= before: $(dc host quests | cut -c1-160)"; fi
	quests_expect "vm: $quest's stage $s the host set with no session is done here (the host's snapshot)" vm "$quest" "$s" &&
		quests_by_session vm "$quest's stage $s, from the host's snapshot" "$before" "$pat"
}

step_quests() {
	local w quest
	QUESTS_SKIP=""
	section "quests: both games' quest sync, and which quests are each player's own"
	for w in host vm; do quests_status $w || return; done
	for w in host vm; do expect_eq "$w: MQ101 (the Helgen opening) is each player's own" "$(field class "$(dc $w 'quest MQ101')")" own; done

	section "quests: a stage one game sets is made in the other"
	if quests_start vm; then
		quest=$QUEST
		if quests_next "$quest" "the host's" && quests_leg host "$quest" "$QNEXT"; then
			section "quests: the quest's objectives read the same in both, and one each way is made in the other"
			quests_objectives "$quest"
			quests_objective_legs "$quest"
			section "quests: the session's word after the VM loads a save and after it joins again"
			quests_again "$quest"
			section "quests: the host's snapshot when it hosts again"
			quests_snapshot "$quest"
		fi
		section "quests: a quest's end, both ways"
		quests_end_leg vm "$quest"
		quests_start host && quests_end_leg host "$QUEST"
	fi

	section "quests: no stage moved a player, none stuck, nothing made that did not take"
	for w in host vm; do quests_counts $w; done
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init queststest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	QUESTS_WAIT=2; QUESTS_SETTLE=2

	Q='quest=DA03 id=00022F08 type=7 class=shared enabled=1 completed=0 failed=0 current=20 stages=0,10,20,30 done=10,20 seen=20 objectives=10:3,20:1,30:0 session=10,20 sessionObjectives=10:3@20 sessionEnd=- apart=0'
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
	F='quest=DA03 id=00022F08 type=7 class=shared enabled=0 completed=0 failed=0 current=0 stages=0,10,20,30 done=- seen=- objectives=- session=- sessionObjectives=- sessionEnd=- apart=0'
	t "fresh: never started" "$(quests_fresh "$F" && echo yes)" yes
	t "fresh: a stage read done is not" "$(quests_fresh "${F/done=-/done=10}" || echo no)" no
	t "shown: not the dormant ones, by index" "$(quests_shown 30:0,20:1,10:3,5:5)" "5:5 10:3 20:1 "
	t "shown: none" "$(quests_shown -)" ""
	t "objective state: listed" "$(quests_obj_state 10:3,20:1 20)" 1
	t "objective state: not listed is dormant" "$(quests_obj_state 10:3 30)" 0
	t "objective state: none listed" "$(quests_obj_state - 30)" 0
	t "dormant: in both, ascending" "$(quests_dormant 30:0,10:0,20:0,5:1 10:0,20:1)" "10 30 "
	t "dormant: none" "$(quests_dormant 10:1 -)" ""
	t "number: of a/b" "$(quests_number 3/120)" 3

	# ---- both games faked ----
	# Each game holds, per quest, its stages done, its objectives and whether it is completed; its made= count and its
	# plugin log. `console setstage|setobjectivedisplayed|setobjectivecompleted|completequest` does it in the game it is
	# sent to and, by SHARE: 1 the mod at work (made in the other game: made= up, its log line), 2 the other game's own
	# scripts do the same (done there, but nothing made: the check must not pass), 0 nothing in the other. The VM
	# saves, loads, leaves and joins; the host ends its session and hosts again (its snapshot).
	fake() {  # fake <share> [quest whose stages never take in the VM]
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; NOTAKE=${2:-}; QUESTS_SKIP=""
		for w in host vm; do echo 0 > "$FK/$w.made"; : > "$FK/$w.log"; echo 0 > "$FK/$w.snap"; echo 0 > "$FK/$w.asks"; done
		echo 0 > "$FK/away"
	}
	fget() { cat "$FK/$1.$2.$3" 2>/dev/null || echo "$4"; }   # fget <who> <quest> <what> <default>
	fput() { echo "$4" > "$FK/$1.$2.$3"; }
	fadd() { local cur; cur=$(fget "$1" "$2" done -); if [ "$cur" = - ]; then fput "$1" "$2" done "$3"; else fput "$1" "$2" done "$cur,$3"; fi; fput "$1" "$2" current "$3"; }
	fobj() {  # fobj <who> <quest> <index> <state>
		local cur; cur=$(fget "$1" "$2" obj "5:1,10:0,20:0,30:0")
		fput "$1" "$2" obj "$(tr ',' '\n' <<<"$cur" | awk -F: -v i="$3" -v s="$4" -v OFS=: '$1 == i { $2 = s } { print }' | paste -sd,)"
	}
	fmade() { echo $(($(cat "$FK/$1.made") + 1)) > "$FK/$1.made"; echo "[00:00:00.000] [1] [I] $2" >> "$FK/$1.log"; }
	away() { [ "$(cat "$FK/away")" = 1 ]; }
	# fdo <who> <quest> <what> <args...>: done in <who>; in the other as SHARE says
	fdo() {
		local w=$1 q=$2 what=$3 o; o=$(other "$w"); shift 3
		case $what in
			stage) fadd "$w" "$q" "$1" ;;
			show) fobj "$w" "$q" "$1" 1 ;;
			complete) fobj "$w" "$q" "$1" 3 ;;
			end) fput "$w" "$q" completed 1 ;;
		esac
		away && return 0
		case "$SHARE:$what" in
			1:stage|2:stage) fadd "$o" "$q" "$1" ;;
			1:show|2:show) fobj "$o" "$q" "$1" 1 ;;
			1:complete|2:complete) fobj "$o" "$q" "$1" 3 ;;
			1:end|2:end) fput "$o" "$q" completed 1 ;;
		esac
		[ "$SHARE" = 1 ] || return 0
		case $what in
			stage) fmade "$o" "Quests: made $q stage $1 here (the session's word)" ;;
			show) fmade "$o" "Quests: made $q objective $1 state 1 here" ;;
			complete) fmade "$o" "Quests: made $q objective $1 state 3 here" ;;
			end) fmade "$o" "Quests: completed $q here (the session's word)" ;;
		esac
	}
	# fsync: the VM makes what the host has that it lacks (a join, a load's ask, the host's snapshot), when the mod works
	fsync() {
		local q s
		[ "$SHARE" = 1 ] || return 0
		for q in DA03 DB05 DC07; do
			for s in $(fget host "$q" done - | tr ',' ' '); do
				[ "$s" = - ] && continue
				quests_csv_has "$(fget vm "$q" done -)" "$s" && continue
				fadd vm "$q" "$s"; fmade vm "Quests: made $q stage $s here (the session's word)"
			done
		done
	}
	dc() {
		local w=$1 c=$2 q done s i
		case "$c" in
			ping) echo pong ;;
			"quests pick 0") echo "quest=DA03 id=00022F08 type=7 stages=60,10,20,30,40,50" ;;
			"quests pick 1") echo "quest=DB05 id=0001EA51 type=4 stages=10,20,30,40,50,60" ;;
			"quests pick 2") echo "quest=DC07 id=0001EA52 type=8 stages=10,20,30,40,50,60" ;;
			"quests pick "*) echo "error: no ${c#quests pick }-th shared quest not started here with six stages" ;;
			"quest MQ101") echo "quest=MQ101 id=0003372B type=1 class=own enabled=0 completed=1 failed=0 current=0" ;;
			"quest "*)
				q=${c#quest }; done=$(fget "$w" "$q" done -)
				echo "quest=$q id=00022F08 type=7 class=shared enabled=$([ "$done" = - ] && echo 0 || echo 1) completed=$(fget "$w" "$q" completed 0) failed=0 current=$(fget "$w" "$q" current 0) stages=0,10,20,30,40,50,60 done=$done seen=- objectives=$(fget "$w" "$q" obj "5:1,10:0,20:0,30:0") session=- sessionObjectives=- sessionEnd=- apart=0" ;;
			"console setstage "*)
				read -r _ _ q s <<<"$c"
				[ "$w" = vm ] && [ "$q" = "$NOTAKE" ] && { echo ran; return; }
				fdo "$w" "$q" stage "$s"; echo ran ;;
			"console setobjectivedisplayed "*) read -r _ _ q i _ <<<"$c"; fdo "$w" "$q" show "$i"; echo ran ;;
			"console setobjectivecompleted "*) read -r _ _ q i _ <<<"$c"; fdo "$w" "$q" complete "$i"; echo ran ;;
			"console completequest "*) read -r _ _ q <<<"$c"; fdo "$w" "$q" end; echo ran ;;
			"console save "*) for q in DA03 DB05 DC07; do cp -f "$FK/vm.$q.done" "$FK/vm.$q.saved" 2>/dev/null; cp -f "$FK/vm.$q.current" "$FK/vm.$q.savedcur" 2>/dev/null; done; echo ran ;;
			"console load "*)
				for q in DA03 DB05 DC07; do [ -f "$FK/vm.$q.saved" ] && cp -f "$FK/vm.$q.saved" "$FK/vm.$q.done" && cp -f "$FK/vm.$q.savedcur" "$FK/vm.$q.current"; done
				echo $(($(cat "$FK/vm.asks") + 1)) > "$FK/vm.asks"; fsync; echo ran ;;
			"session leave") echo 1 > "$FK/away"; echo left ;;
			"steam stophost") echo stopped ;;
			quests) echo "installed=1 host=$([ "$w" = host ] && echo 1 || echo 0) quests=812 doneBit=trusted made=$(cat "$FK/$w.made") asks=$(cat "$FK/$w.asks") snapshots=$(cat "$FK/$w.snap")/40 moved=0 lastMoved=none stuck=0 lastStuck=none unmade=0 lastUnmade=none offmain=0 norun=0" ;;
			standins) echo "standin=00000000" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	log_since_mark() { grep -aE -- "$2" "$FK/$1.log"; }
	in_world() { :; }; welcomed() { :; }; joined_both() { :; }; gone_from() { :; }
	vm_join() { echo 0 > "$FK/away"; fsync; return 0; }
	session_up() { echo 0 > "$FK/away"; echo $(($(cat "$FK/host.snap") + 1)) > "$FK/host.snap"; fsync; return 0; }
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init qt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }
	count() { grep -cE "$2" <<<"$1"; }

	fake 1; t "pick: the first the host has not started either, stages ascending" "$(quests_pick)" "DA03 10 20 30 40 50 60 "
	fake 1; fadd host DA03 5; t "pick: one the host started is passed over" "$(quests_pick)" "DB05 10 20 30 40 50 60 "
	fake 1; QUESTS_SKIP=" DA03"; t "pick: one picked before in the run is passed over" "$(quests_pick)" "DB05 10 20 30 40 50 60 "
	fake 1; fadd host DA03 5; fadd host DB05 5; fadd host DC07 5; t "pick: none" "$(quests_pick || echo none)" none
	fake 1; QUESTS_QUEST=DB05; t "pick: the one named" "$(quests_pick)" "DB05 10 20 30 40 50 60 "; QUESTS_QUEST=

	fake 1; fadd vm DA03 10; QSTAGES=(10 20 30); quests_take DA03; t "take: a stage one game has done is passed over" "$QNEXT ${QSTAGES[*]}" "20 30"
	fake 1; fadd host DA03 30; QSTAGES=(10 20 40); quests_take DA03; t "take: ... and one below a game's current stage" "$QNEXT" 40
	fake 1; fadd host DA03 30; QSTAGES=(10 20); t "take: none left" "$(quests_take DA03 || echo none)" none

	fake 1; o=$(run 1 quests_leg vm DA03 10)
	t "leg: shared passes, made in the other by the session's word" "$(verdicts "$o")" "PASS=3 "
	fake 0; o=$(run 2 quests_leg vm DA03 10)
	t "leg: not shared fails on the watcher" "$(count "$o" '^FAIL host: DA03.s stage 10 the vm set is done here too')" 1
	fake 2; o=$(run 3 quests_leg host DA03 20)
	t "leg: done in the watcher by its own scripts, not made, fails" "$(count "$o" '^FAIL vm: DA03.s stage 20 was made here by the session.s word'),$(count "$o" '^PASS vm: DA03.s stage 20 the host set is done here too')" 1,1
	fake 1; NOTAKE=DA03; o=$(run 4 quests_leg vm DA03 10)
	t "leg: the VM's own stage not done is RIG, not FAIL" "$(count "$o" '^RIG .*vm: reads DA03.s stage 10 done'),$(count "$o" '^FAIL ')" 1,0
	fake 1; dcfake=$(declare -f dc); dc() { case "$2" in "console setstage"*) echo "error: not in the world" ;; *) echo x ;; esac; }
	o=$(run 5 quests_leg vm DA03 10)
	t "leg: a console the dev channel refused is the rig broken, and no more is checked" "$(count "$o" '^FAIL .*rig broken \(input\)'),$(count "$o" '^FAIL ')" 1,1
	eval "$dcfake"

	fake 1 DA03; o=$(run 6 quests_start vm)
	t "start: a pick whose first stage does not take in the VM is passed over for the next" "$(count "$o" 'quest: DB05'),$(count "$o" '^(RIG|FAIL) ')" 1,0
	fake 1; o=$(run 7 quests_start host)
	t "start: by the host, made in the VM" "$(count "$o" '^PASS vm: DA03.s stage 10 was made here by the session.s word')" 1

	obj() {  # obj <host objectives> <vm objectives> <n>
		O_H=$1 O_V=$2
		dc() { case "$2" in "quest DA03") if [ "$1" = host ]; then echo "quest=DA03 objectives=$O_H"; else echo "quest=DA03 objectives=$O_V"; fi ;; esac; }
		run "o$3" quests_objectives DA03
	}
	o=$(obj 10:3,20:1 20:1,10:3,30:0 1); t "objectives: the same shown ones pass, in any order, dormant ones aside" "$(verdicts "$o")" "PASS=1 "
	o=$(obj 10:3,20:1 10:1 2); t "objectives: behind fails" "$(count "$o" '^FAIL vm: DA03.s objectives')" 1
	o=$(obj - 10:0 3); t "objectives: none shown is a WARN, not a PASS" "$(verdicts "$o")" "WARN=1 "
	eval "$dcfake"

	fake 1; fadd vm DA03 20; fadd host DA03 20; o=$(run 8 quests_objective_legs DA03)
	t "objective legs: both ways, shown and completed, made by the session's word" "$(verdicts "$o")" "PASS=8 "
	fake 2; fadd vm DA03 20; fadd host DA03 20; o=$(run 9 quests_objective_legs DA03)
	t "objective legs: done by the watcher's own scripts fails each made check" "$(count "$o" '^FAIL .*was made here by the session.s word')" 4
	fake 0; fadd vm DA03 20; fadd host DA03 20; o=$(run 10 quests_objective_legs DA03)
	t "objective legs: with no sync fails" "$(count "$o" '^FAIL .* is shown here too')" 2
	fake 1; fadd vm DA03 20; fadd host DA03 30; o=$(run 11 quests_objective_legs DA03)
	t "objective legs: the games at two stages is a rig fault, never a PASS" "$(count "$o" '^RIG '),$(count "$o" '^PASS ')" 2,0

	fake 1; fadd vm DA03 20; fadd host DA03 20; o=$(run 12 quests_end_leg vm DA03)
	t "end: the VM's, made in the host" "$(verdicts "$o")" "PASS=2 "
	fake 0; fadd vm DA03 20; fadd host DA03 20; o=$(run 13 quests_end_leg host DA03)
	t "end: with no sync fails" "$(count "$o" '^FAIL vm: DA03 the host completed')" 1

	fake 1; fadd vm DA03 10; fadd host DA03 10; QSTAGES=(20 30 40); o=$(run 14 quests_snapshot DA03)
	t "snapshot: the stage the host set with no session is made in the VM from its snapshot" "$(verdicts "$o")" "PASS=5 "
	fake 0; fadd vm DA03 10; fadd host DA03 10; QSTAGES=(20 30 40); o=$(run 15 quests_snapshot DA03)
	t "snapshot: with no sync fails" "$(count "$o" '^FAIL vm: DA03.s stage 20 the host set with no session')" 1

	fake 1; fadd vm DA03 20; fadd host DA03 20; QSTAGES=(30 40 50); o=$(run 16 quests_again DA03)
	t "again: the load and the rejoin, made by the session's word" "$(count "$o" '^FAIL '),$(count "$o" '^PASS .*was made here by the session.s word')" 0,3

	# the whole step: all passes with the mod at work, and fails with each part of it missing
	fake 1; o=$(run 20 step_quests)
	t "step: all well, no FAIL, RIG or WARN" "$(count "$o" '^(FAIL|RIG|RIGFAULT|WARN) ')" 0
	t "step: ... every made check passed (stages, objectives, ends, load, rejoin, snapshot)" "$(count "$o" '^PASS .*was made here by the session.s word')" 13
	fake 2; o=$(run 21 step_quests)
	t "step: the other game's own scripts doing the same never pass the made checks" "$(count "$o" '^PASS .*was made here by the session.s word')" 0
	fake 0; o=$(run 22 step_quests)
	t "step: with no sync it fails" "$( [ "$(count "$o" '^FAIL ')" -ge 2 ] && echo yes)" yes

	st() {  # st <quests reply> <n>: quests_status and quests_counts of the host on that reply
		S_R=$1; dc() { echo "$S_R"; }
		mkdir -p "$d/s$2"; ( A_OUT=$d/s$2; a_init qs >/dev/null; quests_status host; quests_counts host; a_summary ) 2>&1
	}
	o=$(st 'installed=1 doneBit=trusted moved=0 stuck=0 unmade=0' 1); t "status: all well passes" "$(verdicts "$o")" "PASS=5 "
	o=$(st 'installed=1 doneBit=untrusted moved=2 lastMoved=000E3D8D:30 stuck=1 lastStuck=000E3D8D:20 unmade=1 lastUnmade=DA03_completed' 2)
	t "status: an untrusted bit, a move, a stuck stage and an unmade one are WARNs" "$(verdicts "$o")" "PASS=1 WARN=4 "
	t "status: ... the moving stage named" "$(count "$o" '^WARN host: no stage .* last=000E3D8D:30')" 1
	o=$(st 'error: no such command' 3); t "status: no quest sync is the rig broken" "$(count "$o" '^FAIL .*rig broken \(game\)')" 1
	eval "$dcfake"

	# the step is wired into the harness: in STEPS after doors and before final, a session step, in the default run
	t "quests is a step, after doors, before final" "$(case " $STEPS " in *" doors "*"quests "*"final "*) echo yes;; esac)" yes
	t "quests needs a session" "$(case " $SESSION_STEPS " in *" quests "*) echo yes;; esac)" yes
	t "quests runs by default" "$(case " $OPTIN_STEPS " in *" quests "*) echo no;; *) echo yes;; esac)" yes
	t "quests has its line" "$([ -n "${STEP_INFO[quests]:-}" ] && echo yes)" yes
	t "quests is not run twice (it sets stages for good)" "$(grep -qE 'RERUN_STEPS=.*quests' "$(dirname "${BASH_SOURCE[0]}")/../smoke.sh" && echo yes || echo no)" no
	[ $bad -eq 0 ] && echo "quests.sh selftest: all ok" || echo "quests.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
