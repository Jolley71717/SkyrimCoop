#!/bin/bash
# steps/jobs.sh: the rig step `jobs` (protocol 12, docs/DESIGN.md "Quests", "Runs and aliases"), sourced by steps.sh. A
# radiant job and a shared quest the host's game starts run in the VM's game as the host's run, with the references the
# host's aliases hold; their stages go both ways; the host's stop of the job stops it in the VM, and the job begun again
# is a new run; a VM that joins again takes the job its game runs as the host's run; a radiant job the VM's game starts
# on its own stays its own. Not run in a game yet.
#
#   step_jobs                                  the step (in the default run, after quests, before clock)
#   bash tools/rig/steps/jobs.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `quests`, `quests runpick <radiant|shared> <n>`, `quests start
# <quest>`, `quests run <quest>` and `quest <quest>`):
#   1. Both games say quest runs (`quests` has runs=).
#   2. A radiant job: the host's `quests runpick radiant <n>` names one that a console start can begin (an alias filled
#      by its conditions, none that needs an event's data); the first that runs in neither game is taken. The host starts
#      it with `quests start` (the engine's own start, as the Story Manager starts a job: the input, said). A pick the
#      engine would not start there (its aliases did not fill where the players stand) is stopped and passed over for the
#      next, JOBS_PICKS of them (RIG after). Within JOBS_WAIT the host says the run with its aliases' references (live=1,
#      fills= not -), and the VM runs the job as the host's run (phase=running, running=1) with every reference the
#      host's aliases hold held by its own (same=1): started there for the run, its plugin log says "Quests: started
#      <quest> here for the host's run ...: starting" (the VM's own start of the job never passes).
#   3. Its stages both ways: the host sets the next stage (console setstage), the VM makes it from the session's word;
#      the VM sets the next, the host makes it (steps/quests.sh's leg: made=, and the plugin log's line).
#   4. The host stops the job (console stopquest: the input): within JOBS_WAIT it stops in the VM (stopsMade= up, its log
#      "Quests: stopped <quest> here"), and the VM's session word on it is gone (session=-).
#   5. The host starts the job again: a new run (run= higher), and the VM runs it again as that run, with the host's
#      references.
#   6. The VM leaves and joins again while the job runs in both: it takes the job as the host's run (runsAdopted= up,
#      its log "Quests: <quest> ran here already: it is the host's run"), with the host's references. The host stops the
#      job after (it runs in neither game for the rest of the run).
#   7. A radiant job the VM starts (`quests start`): JOBS_QUIET later it does not run in the host's game, and the VM's
#      `quests run` says it is no run of the session's (phase=none). The VM stops it after.
#   8. A shared quest the host starts (`quests runpick shared <n>`, `quests start`): the VM runs it as the host's run
#      with the host's references, and its stages go both ways (3). It stays started for the rest of the run.
#   9. The counts: a start for a run the VM's game would not make (startFailed=), a run given up (runsGaveUp=: no
#      references, or no start, within the plugin's START_WAIT) and a reference not loaded when forced (notLoaded=) are
#      WARNs, named.

JOBS_WAIT=${JOBS_WAIT:-20}    # s: a run to reach the VM and its quest to run there with the host's references (START_WAIT is 10 s)
JOBS_QUIET=${JOBS_QUIET:-8}   # s: how long a game's own job is watched for not reaching the other game
JOBS_TRIES=${JOBS_TRIES:-30}  # `quests runpick <class> <n>`: how many to look through for one that runs in neither game
JOBS_PICKS=${JOBS_PICKS:-5}   # picks the engine would not start in the game that starts it, before the step gives up (RIG)
JOBS_SKIP=""                  # the quests picked already in this run (passed over by the next pick)

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# jobs_refs <fills>: the aliases that hold a reference of the load order ("alias:XXXXXXXX"), by alias, one space after each
jobs_refs() { tr ',' '\n' <<<"$1" | grep -E '^[0-9]+:[0-9A-F]{8}$' | sort -t: -k1,1n | tr '\n' ' '; }
# jobs_stages_after <quest reply>: the quest's stages past its current one, ascending, one space after each
jobs_stages_after() {
	local cur s; cur=$(field current "$1"); is_num "$cur" || cur=0
	for s in $(quests_sorted "$(field stages "$1")"); do [ "$s" -gt "$cur" ] && echo -n "$s "; done
}

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

jobs_q() { dc "$1" "quests run $2"; }
jobs_line() { jobs_q "$1" "$2" | cut -c1-200; }
jobs_running() { [ "$(field running "$(jobs_q "$1" "$2")")" = 1 ]; }
jobs_still() { [ "$(field running "$(jobs_q "$1" "$2")")" = 0 ]; }
# jobs_said <who>: the host says the quest's run, with its aliases' references
jobs_said() { local r; r=$(jobs_q host "$1"); [ "$(field live "$r")" = 1 ] && [ -n "$(field fills "$r")" ] && [ "$(field fills "$r")" != - ]; }
# jobs_ours <quest>: the VM runs it as the host's run, every reference the host's aliases hold held by its own
jobs_ours() { local r; r=$(jobs_q vm "$1"); [ "$(field phase "$r")" = running ] && [ "$(field running "$r")" = 1 ] && [ "$(field same "$r")" = 1 ]; }
# jobs_count <who> <field>: a count of that game's `quests`
jobs_count() { field "$2" "$(dc "$1" quests)"; }

jobs_status() {
	local s; s=$(dc "$1" quests)
	if is_num "$(field runs "$s")"; then pass "$1: says quest runs" "runs=$(field runs "$s") runsSent=$(field runsSent "$s") runsHeard=$(field runsHeard "$s")"; return 0; fi
	rig_broken game "$1: says quest runs" "no runs= in its quests reply (a build before protocol 12?): $(cut -c1-120 <<<"$s")"; return 1
}

# jobs_pick <class> <who>: the editor ID of a quest of the class that game's `quests runpick` names, running in neither
# game and not picked before in this run; 1 when there is none
jobs_pick() {
	local n r name
	for ((n = 0; n < JOBS_TRIES; n++)); do
		r=$(dc "$2" "quests runpick $1 $n")
		[[ $r == quest=* ]] || return 1
		name=$(field quest "$r")
		case " $JOBS_SKIP " in *" $name "*) continue ;; esac
		jobs_still host "$name" && jobs_still vm "$name" || continue
		echo "$name"; return 0
	done
	return 1
}

# jobs_begin <class> <who>: JOB = a quest of the class that game started (quests start), running there. A pick the
# engine would not start is stopped and passed over for the next.
jobs_begin() {
	local cls=$1 who=$2 tries q r
	for ((tries = 0; tries < JOBS_PICKS; tries++)); do
		if ! q=$(jobs_pick "$cls" "$who"); then
			rig_fault state "$who: a $cls quest that runs in neither game" "none in its first $JOBS_TRIES picks (passed over:${JOBS_SKIP:- none}; $(dc "$who" "quests runpick $cls 0" | cut -c1-80))"
			return 1
		fi
		JOBS_SKIP="$JOBS_SKIP $q"
		r=$(dc "$who" "quests start $q")
		[[ $r == quest=* ]] || { rig_broken input "$who: starts $q (quests start: the input)" "replied '$r'"; return 1; }
		if wait_until 10 jobs_running "$who" "$q"; then
			JOB=$q
			echo "     $cls quest: $q, started by the $who's game (quests start: the engine's own start, as the Story Manager starts a job)"
			pass "$who: starts the $cls quest $q (quests start: the input)" "$(jobs_line "$who" "$q" | cut -c1-120)"
			return 0
		fi
		echo "     $q: the engine would not start it in the $who's game (its aliases may not fill where the players stand): the next pick"
		dc "$who" "console stopquest $q" >/dev/null
	done
	rig_fault input "$who: starts a $cls quest" "$JOBS_PICKS picks the engine would not start:$JOBS_SKIP"
	return 1
}

# jobs_follows <quest> <before: the VM's log lines> <pattern>: the host says the run with its references, and the VM runs
# the quest as the host's run with them, its log saying how (a start for the run, or taking the one it ran already)
jobs_follows() {
	local quest=$1 before=$2 pat=$3 lines
	if wait_until "$JOBS_WAIT" jobs_said "$quest"; then pass "host: says $quest's run and its aliases' references" "$(jobs_line host "$quest" | cut -c1-160)"
	else fail "host: says $quest's run and its aliases' references" "$(jobs_line host "$quest")"; return 1; fi
	if wait_until "$JOBS_WAIT" jobs_ours "$quest"; then pass "vm: runs $quest as the host's run, its aliases holding the host's references" "vm: $(jobs_line vm "$quest" | cut -c1-160)"
	else fail "vm: runs $quest as the host's run, its aliases holding the host's references" "vm: $(jobs_line vm "$quest"); host: $(jobs_line host "$quest" | cut -c1-120)"; return 1; fi
	lines=$(quests_said vm "$pat")
	if is_num "$lines" && [ "$lines" -gt "${before:-0}" ]; then pass "vm: $quest runs there by the host's run (not by its own start)" "its log: '$pat'"
	else fail "vm: $quest runs there by the host's run (not by its own start)" "log lines '$pat': ${before:-?}->$lines"; fi
}
jobs_started_pat() { echo "Quests: started $1 here for the host.s run .*: starting"; }
jobs_adopted_pat() { echo "Quests: $1 ran here already: it is the host.s run"; }

# jobs_stage_legs <quest>: the quest's next stage set by the host and made in the VM, then the next by the VM, made in the
# host (steps/quests.sh's leg)
jobs_stage_legs() {
	local quest=$1
	# shellcheck disable=SC2206
	QSTAGES=($(jobs_stages_after "$(dc host "quest $quest")"))
	quests_next "$quest" "the host's" && quests_leg host "$quest" "$QNEXT" || return 1
	quests_next "$quest" "the VM's" && quests_leg vm "$quest" "$QNEXT"
}

# jobs_stop_leg <job>: the host stops the radiant job; it stops in the VM, and the VM's session word on it goes
jobs_stop_leg() {
	local job=$1 stops0 pat before
	stops0=$(jobs_count vm stopsMade)
	pat="Quests: stopped $job here"; before=$(quests_said vm "$pat")
	quests_run "host: stops $job (console stopquest: the input)" host "console stopquest $job" ran || return 1
	wait_until 10 jobs_still host "$job" || { rig_fault input "host: stops $job (console stopquest: the input)" "it still runs: $(jobs_line host "$job")"; return 1; }
	if wait_until "$JOBS_WAIT" jobs_still vm "$job"; then pass "vm: $job stops there when the host's stops" "$(jobs_line vm "$job" | cut -c1-120)"
	else fail "vm: $job stops there when the host's stops" "$(jobs_line vm "$job")"; return 1; fi
	if is_num "$stops0"; then expect_ge "vm: stopped it for the host's stop (stopsMade=)" "$(jobs_count vm stopsMade)" $((stops0 + 1))
	else fail "vm: stopped it for the host's stop (stopsMade=)" "no stopsMade= before: $(dc vm quests | cut -c1-120)"; fi
	expect_ge "vm: its log says it stopped $job for the host's stop" "$(quests_said vm "$pat")" $((${before:-0} + 1))
	expect_eq "vm: the session's word on the run that ended is gone" "$(field session "$(dc vm "quest $job")")" -
}

# jobs_again_leg <job>: the host starts the job again: a new run, and the VM runs it as that run
jobs_again_leg() {
	local job=$1 run0 run1 r before
	run0=$(field run "$(jobs_q host "$job")")
	before=$(quests_said vm "$(jobs_started_pat "$job")")
	r=$(dc host "quests start $job")
	[[ $r == quest=* ]] || { rig_broken input "host: starts $job again (quests start: the input)" "replied '$r'"; return 1; }
	wait_until 10 jobs_running host "$job" || { rig_fault input "host: starts $job again (quests start: the input)" "the engine would not: $(jobs_line host "$job")"; return 1; }
	run1=$(field run "$(jobs_q host "$job")")
	if is_num "$run0" && is_num "$run1" && [ "$run1" -gt "$run0" ]; then pass "host: $job begun again is a new run" "run $run0 -> $run1"
	else fail "host: $job begun again is a new run" "run $run0 -> $run1"; fi
	jobs_follows "$job" "$before" "$(jobs_started_pat "$job")" || return 1
	expect_eq "vm: runs it as the new run" "$(field run "$(jobs_q vm "$job")")" "$run1"
}

# jobs_rejoin_leg <job>: the VM leaves and joins again while the job runs in both; it takes the job as the host's run
jobs_rejoin_leg() {
	local job=$1 adopted0 r before
	adopted0=$(jobs_count vm runsAdopted)
	before=$(quests_said vm "$(jobs_adopted_pat "$job")")
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return 1; }
	gone_from host || fail "host: the VM's stand-in goes when it leaves" "$(dc host standins | cut -c1-160)"
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return 1; fi
	jobs_follows "$job" "$before" "$(jobs_adopted_pat "$job")" || return 1
	if is_num "$adopted0"; then expect_ge "vm: took the job its game ran as the host's run (runsAdopted=)" "$(jobs_count vm runsAdopted)" $((adopted0 + 1))
	else fail "vm: took the job its game ran as the host's run (runsAdopted=)" "no runsAdopted= before: $(dc vm quests | cut -c1-120)"; fi
}

# jobs_own_leg: a radiant job the VM starts stays its own: it does not run in the host's game, and is no run of the
# session's in the VM's
jobs_own_leg() {
	local job
	jobs_begin radiant vm || return 1
	job=$JOB
	sleep "$JOBS_QUIET"
	if jobs_still host "$job"; then pass "host: the VM's own job $job does not run here" "$(jobs_line host "$job" | cut -c1-120)"
	else fail "host: the VM's own job $job does not run here" "$(jobs_line host "$job")"; fi
	expect_eq "vm: its own job is no run of the session's" "$(field phase "$(jobs_q vm "$job")")" none
	dc vm "console stopquest $job" >/dev/null
}

jobs_counts() {
	local s; s=$(dc "$1" quests)
	if [ "$(field startFailed "$s")" = 0 ]; then pass "$1: every start for a run took" "startFailed=0"
	else warn "$1: every start for a run took" "startFailed=$(field startFailed "$s") lastRun=$(field lastRun "$s")"; fi
	if [ "$(field runsGaveUp "$s")" = 0 ]; then pass "$1: no run given up" "runsGaveUp=0"
	else warn "$1: no run given up" "runsGaveUp=$(field runsGaveUp "$s") lastRun=$(field lastRun "$s"): no references, or no start, within START_WAIT"; fi
	if [ "$(field notLoaded "$s")" = 0 ]; then pass "$1: every reference forced was loaded" "notLoaded=0"
	else warn "$1: every reference forced was loaded" "notLoaded=$(field notLoaded "$s"): a run named a reference this game had not loaded (tried again)"; fi
	echo "     $1: $(tr ' ' '\n' <<<"$s" | grep -E '^(runs|runsSent|runsHeard|runsStarted|runsAdopted|runsGaveUp|forced|forcedAtStart|ownAtStart|startFailed|stopsMade|notLoaded|rewritten|lastRun)=' | tr '\n' ' ')"
}

step_jobs() {
	local w job before
	JOBS_SKIP=""
	section "jobs: both games say quest runs"
	for w in host vm; do jobs_status $w || return; done

	section "jobs: a radiant job the host's game starts runs in the VM with the host's references"
	if jobs_begin radiant host; then
		job=$JOB
		before=$(quests_said vm "$(jobs_started_pat "$job")")
		# the start's line may be in the log already (the run comes fast): counted from before the start
		[ "${before:-0}" -gt 0 ] 2>/dev/null && before=$((before - 1))
		if jobs_follows "$job" "$before" "$(jobs_started_pat "$job")"; then
			section "jobs: the job's stages, both ways"
			jobs_stage_legs "$job"
			section "jobs: the host's stop of the job stops it in the VM"
			if jobs_stop_leg "$job"; then
				section "jobs: the job begun again is a new run, in the VM too"
				if jobs_again_leg "$job"; then
					section "jobs: the VM joins again and takes the job its game runs as the host's run"
					jobs_rejoin_leg "$job"
				fi
			fi
		fi
		dc host "console stopquest $job" >/dev/null  # runs in neither game for the rest of the run
	fi

	section "jobs: a radiant job the VM's game starts stays its own"
	jobs_own_leg

	section "jobs: a shared quest the host's game starts runs in the VM with the host's references, its stages both ways"
	if jobs_begin shared host; then
		job=$JOB
		before=$(quests_said vm "$(jobs_started_pat "$job")")
		[ "${before:-0}" -gt 0 ] 2>/dev/null && before=$((before - 1))
		jobs_follows "$job" "$before" "$(jobs_started_pat "$job")" && jobs_stage_legs "$job"
	fi

	section "jobs: every start took, no run given up, every reference forced was loaded"
	for w in host vm; do jobs_counts $w; done
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init jobstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	JOBS_WAIT=2; JOBS_QUIET=0; QUESTS_WAIT=2

	t "refs: the load order's references, by alias" "$(jobs_refs 3:0001A2B3,0:-,1:rt,2:?,10:00012345)" "3:0001A2B3 10:00012345 "
	t "refs: none" "$(jobs_refs -)" ""
	t "stages after: past the current one, ascending" "$(jobs_stages_after 'quest=BQ01 current=20 stages=0,40,10,20,30')" "30 40 "
	t "stages after: not started" "$(jobs_stages_after 'quest=BQ01 current=0 stages=0,10,20')" "10 20 "
	t "stages after: none listed" "$(jobs_stages_after 'error: no such quest here')" ""

	# ---- both games faked ----
	# Each game holds, per quest, whether it runs, its stages done, the session's word on it (the VM), its made= count
	# and its plugin log; the host its runs (numbered), the VM its phase of each. SHARE: 1 the mod at work (a run the
	# host starts is started in the VM for it, with the host's references, logged; its stop stops it there; a rejoin
	# takes it), 2 the VM's own scripts start the same quest (it runs, but by no run: phase none, nothing logged), 0
	# nothing reaches the VM. NOSTART: a quest the engine never starts in the host. LEAK=1: the VM's own job runs in the
	# host too (the mod sharing what it must not).
	JOBS=" BQ01 BQ02 BQ03 "
	fake() {  # fake <share> [quest the host's engine never starts] [leak]
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; NOSTART=${2:-}; LEAK=${3:-0}; JOBS_SKIP=""
		for w in host vm; do echo 0 > "$FK/$w.made"; : > "$FK/$w.log"; done
		echo 0 > "$FK/away"; echo 0 > "$FK/runs"; echo 0 > "$FK/stops"; echo 0 > "$FK/adopted"
	}
	fget() { cat "$FK/$1.$2.$3" 2>/dev/null || echo "$4"; }   # fget <who> <quest> <what> <default>
	fput() { echo "$4" > "$FK/$1.$2.$3"; }
	fapp() { local cur; cur=$(fget "$1" "$2" "$3" -); if [ "$cur" = - ]; then fput "$1" "$2" "$3" "$4"; else fput "$1" "$2" "$3" "$cur,$4"; fi; }
	fbump() { echo $(($(cat "$FK/$1") + 1)) > "$FK/$1"; }
	flog() { echo "[00:00:00.000] [1] [I] $2" >> "$FK/$1.log"; }
	away() { [ "$(cat "$FK/away")" = 1 ]; }
	isjob() { case "$JOBS" in *" $1 "*) return 0 ;; esac; return 1; }
	# the host's run of q is on and the VM runs it as that run
	asrun() { [ "$(fget host "$1" live 0)" = 1 ] && [ "$(fget vm "$1" phase none)" = running ]; }
	# shares <quest>: the other game takes a stage of it (a shared quest; a job only while the VM runs it as the host's run)
	shares() { [ "$SHARE" = 1 ] && ! away && { ! isjob "$1" || asrun "$1"; }; }
	fstart() {  # fstart <who> <quest>
		local w=$1 q=$2 n
		[ "$w" = host ] && [ "$q" = "$NOSTART" ] && return 0
		fput "$w" "$q" running 1; fput "$w" "$q" done -; fput "$w" "$q" current 0
		if [ "$w" = vm ]; then
			[ "$LEAK" = 1 ] && fput host "$q" running 1
			return 0
		fi
		fbump runs; n=$(cat "$FK/runs"); fput host "$q" run "$n"; fput host "$q" live 1
		away && return 0
		case $SHARE in
			1)
				fput vm "$q" running 1; fput vm "$q" done -; fput vm "$q" current 0; fput vm "$q" phase running; fput vm "$q" run "$n"; fput vm "$q" same 1
				flog vm "Quests: started $q here for the host's run $n with 2 of its aliases given the host's references (0 this game's own pick): starting" ;;
			2) fput vm "$q" running 1 ;;
		esac
	}
	fstop() {  # fstop <who> <quest>
		local w=$1 q=$2
		fput "$w" "$q" running 0
		[ "$w" = host ] || return 0
		fput host "$q" live 0
		if [ "$SHARE" = 1 ] && ! away && [ "$(fget vm "$q" phase none)" = running ]; then
			fput vm "$q" running 0; fput vm "$q" phase none; fput vm "$q" session -; fbump stops
			flog vm "Quests: stopped $q here (the host's run $(fget host "$q" run 0) of the radiant job is over)"
		fi
	}
	fstage() {  # fstage <who> <quest> <stage>
		local w=$1 q=$2 s=$3 o; o=$(other "$w")
		fapp "$w" "$q" done "$s"; fput "$w" "$q" current "$s"
		shares "$q" || return 0
		fapp "$o" "$q" done "$s"; fput "$o" "$q" current "$s"; fapp vm "$q" session "$s"
		echo $(($(cat "$FK/$o.made") + 1)) > "$FK/$o.made"; flog "$o" "Quests: made $q stage $s here (the session's word)"
	}
	dc() {
		local w=$1 c=$2 q s n
		case "$c" in
			ping) echo pong ;;
			quests) echo "installed=1 host=$([ "$w" = host ] && echo 1 || echo 0) made=$(cat "$FK/$w.made") moved=0 stuck=0 unmade=0 runs=1 runsSent=4 runsHeard=4 runsStarted=1 runsAdopted=$(cat "$FK/adopted") runsGaveUp=0 forced=2 forcedAtStart=2 ownAtStart=0 startFailed=0 stopsMade=$(cat "$FK/stops") notLoaded=0 rewritten=0 lastRun=none" ;;
			"quests runpick radiant "*)
				n=${c##* }; case $n in 0) echo "quest=BQ01 id=00012001 class=radiant refs=3 byConditions=2" ;; 1) echo "quest=BQ02 id=00012002 class=radiant refs=2 byConditions=1" ;;
					2) echo "quest=BQ03 id=00012003 class=radiant refs=2 byConditions=1" ;; *) echo "error: no $n-th radiant quest that a console start can begin here" ;; esac ;;
			"quests runpick shared "*)
				n=${c##* }; case $n in 0) echo "quest=DA05 id=0001BFC4 class=shared refs=4 byConditions=1" ;; 1) echo "quest=DA06 id=0001BFC5 class=shared refs=2 byConditions=1" ;;
					*) echo "error: no $n-th shared quest that a console start can begin here" ;; esac ;;
			"quests start "*) q=${c#quests start }; fstart "$w" "$q"; echo "quest=$q id=00012001 started=1 result=1" ;;
			"quests run "*)
				q=${c#quests run }
				if [ "$w" = host ]; then
					echo "quest=$q id=00012001 class=radiant role=host run=$(fget host "$q" run 0) live=$(fget host "$q" live 0) fills=$([ "$(fget host "$q" live 0)" = 1 ] && echo 0:00012345,1:- || echo -) here=- running=$(fget host "$q" running 0) locations=0"
				else
					echo "quest=$q id=00012001 class=radiant role=friend run=$(fget vm "$q" run 0) phase=$(fget vm "$q" phase none) heard=- here=- running=$(fget vm "$q" running 0) same=$(fget vm "$q" same 0) force=0 matched=1 runtime=0 unknown=0 extra=0 locations=0 wasOurs=0"
				fi ;;
			"quest "*)
				q=${c#quest }
				echo "quest=$q id=00012001 type=0 class=shared enabled=$(fget "$w" "$q" running 0) completed=0 failed=0 current=$(fget "$w" "$q" current 0) stages=0,10,20,30,40 done=$(fget "$w" "$q" done -) seen=- objectives=- session=$(fget "$w" "$q" session -) sessionObjectives=- sessionEnd=- apart=0" ;;
			"console setstage "*) read -r _ _ q s <<<"$c"; fstage "$w" "$q" "$s"; echo ran ;;
			"console stopquest "*) read -r _ _ q <<<"$c"; fstop "$w" "$q"; echo ran ;;
			"session leave") echo 1 > "$FK/away"; for q in $JOBS DA05 DA06; do [ "$(fget vm "$q" phase none)" = running ] && fput vm "$q" phase none; done; echo left ;;
			standins) echo "standin=00000000" ;;
			"session status") echo "role=x connection=up welcomed=1 players=1" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	log_since_mark() { grep -aE -- "$2" "$FK/$1.log"; }
	joined_both() { :; }; gone_from() { :; }
	vm_join() {  # the VM back in: with the mod at work it takes the host's live runs its game runs
		local q
		echo 0 > "$FK/away"
		[ "$SHARE" = 1 ] || return 0
		for q in $JOBS DA05 DA06; do
			if [ "$(fget host "$q" live 0)" = 1 ] && [ "$(fget vm "$q" running 0)" = 1 ]; then
				fput vm "$q" phase running; fput vm "$q" run "$(fget host "$q" run 0)"; fput vm "$q" same 1; fbump adopted
				flog vm "Quests: $q ran here already: it is the host's run $(fget host "$q" run 0) now, with the host's references"
			fi
		done
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init jt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }
	count() { grep -cE "$2" <<<"$1"; }

	fake 1; t "pick: the first that runs in neither game" "$(jobs_pick radiant host)" BQ01
	fake 1; fput vm BQ01 running 1; t "pick: one running in a game is passed over" "$(jobs_pick radiant host)" BQ02
	fake 1; JOBS_SKIP=" BQ01 BQ02"; t "pick: one picked before is passed over" "$(jobs_pick radiant host)" BQ03
	fake 1; JOBS_SKIP=" BQ01 BQ02 BQ03"; t "pick: none" "$(jobs_pick radiant host || echo none)" none

	fake 1 BQ01; o=$(run 1 jobs_begin radiant host)
	t "begin: a pick the engine would not start is passed over for the next" "$(count "$o" 'radiant quest: BQ02'),$(count "$o" '^(RIG|FAIL) ')" 1,0

	# the whole step: all passes with the mod at work, and fails with each part of it missing
	fake 1; o=$(run 2 step_jobs)
	t "step: all well, no FAIL, RIG or WARN" "$(count "$o" '^(FAIL|RIG|RIGFAULT|WARN) ')" 0
	t "step: ... the VM runs each run as the host's, with its references (job, again, rejoin, shared quest)" "$(count "$o" '^PASS vm: runs .* as the host.s run, its aliases holding')" 4
	t "step: ... each by the host's run (its log), never its own start" "$(count "$o" '^PASS vm: .* runs there by the host.s run')" 4
	t "step: ... stages both ways, on the job and the shared quest, made by the session's word" "$(count "$o" '^PASS .*was made here by the session.s word')" 4
	t "step: ... the host's stop stopped it in the VM" "$(count "$o" '^PASS vm: BQ01 stops there when the host.s stops')" 1
	t "step: ... begun again, a new run" "$(count "$o" '^PASS host: BQ01 begun again is a new run')" 1
	t "step: ... the rejoin took the job (runsAdopted=)" "$(count "$o" '^PASS vm: took the job its game ran as the host.s run')" 1
	t "step: ... the VM's own job stayed its own" "$(count "$o" '^PASS host: the VM.s own job BQ02 does not run here'),$(count "$o" '^PASS vm: its own job is no run of the session.s')" 1,1
	fake 2; o=$(run 3 step_jobs)
	t "step: the VM's own start of the same quest never passes for the host's run" "$(count "$o" '^PASS vm: runs .* as the host.s run'),$( [ "$(count "$o" '^FAIL vm: runs BQ01 as the host.s run')" -ge 1 ] && echo failed)" 0,failed
	fake 0; o=$(run 4 step_jobs)
	t "step: with nothing reaching the VM it fails" "$( [ "$(count "$o" '^FAIL ')" -ge 2 ] && echo yes)" yes
	fake 1 "" 1; o=$(run 5 jobs_own_leg)
	t "own: the VM's own job running in the host fails" "$(count "$o" '^FAIL host: the VM.s own job BQ01 does not run here')" 1

	st() {  # st <quests reply> <n>: jobs_status and jobs_counts of the host on that reply
		S_R=$1; dc() { echo "$S_R"; }
		mkdir -p "$d/s$2"; ( A_OUT=$d/s$2; a_init js >/dev/null; jobs_status host; jobs_counts host; a_summary ) 2>&1
	}
	o=$(st 'installed=1 runs=2 startFailed=0 runsGaveUp=0 notLoaded=0' 1); t "status: all well passes" "$(verdicts "$o")" "PASS=4 "
	o=$(st 'installed=1 runs=2 startFailed=1 runsGaveUp=2 notLoaded=3 lastRun=BQ01_run_4' 2); t "status: a failed start, a run given up, a reference not loaded are WARNs" "$(verdicts "$o")" "PASS=1 WARN=3 "
	o=$(st 'installed=1 made=0' 3); t "status: no runs= is the rig broken" "$(count "$o" '^FAIL .*rig broken \(game\)')" 1

	# the step is wired into the harness: in STEPS after quests and before final, a session step, in the default run
	t "jobs is a step, after quests, before final" "$(case " $STEPS " in *" quests "*"jobs "*"final "*) echo yes;; esac)" yes
	t "jobs needs a session" "$(case " $SESSION_STEPS " in *" jobs "*) echo yes;; esac)" yes
	t "jobs runs by default" "$(case " $OPTIN_STEPS " in *" jobs "*) echo no;; *) echo yes;; esac)" yes
	t "jobs has its line" "$([ -n "${STEP_INFO[jobs]:-}" ] && echo yes)" yes
	t "jobs is not run twice (it starts quests for good)" "$(grep -qE 'RERUN_STEPS=.*jobs' "$(dirname "${BASH_SOURCE[0]}")/../smoke.sh" && echo yes || echo no)" no
	[ $bad -eq 0 ] && echo "jobs.sh selftest: all ok" || echo "jobs.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
