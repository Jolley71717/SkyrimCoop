#!/bin/bash
# assert.sh: turns a rig run into named checks with a verdict. Sourced by smoke.sh (and by any script that wants checks);
# carried from F4Multiplayer's tools/rig/assert.sh (commit 14ee2c47f) and cut to what a Skyrim run reads today.
#
#     . tools/rig/assert.sh
#     a_init smoke                         # a run folder under $CHECKS (skyrim-smoke-<time>), the clock started
#     section "join"                       # a heading; the summary lists the time each section took
#     pass "name" "reason"; fail "name" "reason"; warn "name" "reason"      # one check by hand
#     expect_eq "label" "$got" "$want"     # strings equal
#     expect_le "label" "$got" 10 u        # a number at most (an empty or non-number "got" is a FAIL, never a pass)
#     expect_ge "label" "$got" 1           # a number at least
#     expect_between "label" "$got" 0 100  # a number in a range
#     expect_match "label" "$got" 'regex'  # extended regex
#     expect_zero "label" "$got"           # expect_eq "$got" 0
#     warn_gt "label" "$got" 5 u           # above a soft limit: WARN (listed in the summary), never a FAIL
#     rig_broken game|input|ssh "name" "reason"   # a FAIL that says the RIG is broken; two of them end the run (the breaker)
#     rig_fault walk|input|state "name" "reason"  # a check the rig could not make this time (RIG, not FAIL)
#     healed "name" "how"                  # a rig state put right before a step (HEAL: no verdict, listed)
#     rig_unhealed state "name" "reason"   # a rig state that stayed wrong: the step's checks not made (RIGFAULT, never a PASS)
#     a_attempt_mark; a_attempt_rig; a_attempt_void [first]  # smoke.sh's one re-run of a step that met a rig fault
#     a_summary                            # one screen: counts, every FAIL, RIGFAULT, RIG, HEAL, VOID and WARN, the times;
#                                          # exits with the number of failed checks (0 = PASS)
#     trap a_interrupted INT TERM          # a run stopped by a signal is a FAIL ("run interrupted"), never a short PASS
#     a_log_to <file>                      # all output also into that file, through a tee that Ctrl-C does not kill
#
# Reading the games (env.sh's host and vm send one developer-channel command):
#     dc host|vm "<command>"               # that game's reply line (empty when it gives none)
#     field <key> "<text>"                 # env.sh: the value of key=value in a reply ("" when absent)
#     xyz <key> "<text>"                   # "x y z" of key=(x,y,z) in a reply ("" when absent)
#     dist2 "x y z" "x y z"                # the distance on the ground between two places, whole units
#
# The plugin log (the lines since a mark, so an old line never fails a run):
#     log_mark host|vm                     # where each game's SkyrimCoop.log is now (survives the game rotating it)
#     log_check host|vm "label"            # since the mark: an [E] or [C] line or assertion text FAILs; [W] lines are a WARN,
#                                          # counted per prefix (never their text)
#     log_privacy host|vm "label"          # since the mark: Steam ids, IP addresses, the rig's private patterns (counts only)
#     log_since_mark host|vm "<regex>"     # since the mark: the matching lines (for a step's own log checks)
#
# `bash tools/rig/assert.sh selftest` checks all of this with no game and no rig (the Linux tests run it on every push).
# Every check is appended to $A_OUT/results.tsv as it is made, so a check made in a subshell is not lost.

# The self-test needs no rig: env.sh insists on VM_SSH, which only a real run has.
[ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ] && : "${VM_SSH:=selftest-no-vm}" && export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

A_SECTION=""; A_RIG_STOPPED=""; A_ROUND=""; A_AGAIN=""
declare -A MARK_SIZE MARK_FIRST

a_init() {  # a_init <name>: $A_OUT = $CHECKS/skyrim-<name>-<stamp>/, results.tsv inside
	A_NAME=$1; A_T0=$SECONDS; A_START=$(date +%T)
	A_OUT=${A_OUT:-$CHECKS/skyrim-$1-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$A_OUT"
	A_RESULTS=$A_OUT/results.tsv; : > "$A_RESULTS"; A_SECLOG=$A_OUT/sections.tsv; : > "$A_SECLOG"
	echo "$1 run folder: $A_OUT"
}

section() {  # section <title>
	[ -z "$A_RIG_STOPPED" ] && [ -n "${A_RESULTS:-}" ] && [ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && _rig_stop "$(_rig_last_signal)"
	[ -n "$A_SECTION" ] && printf '%s\t%s\n' "$A_SECTION" "$((SECONDS - A_SEC_T0))" >> "$A_SECLOG"
	A_SECTION="$1${A_AGAIN:+ (again, after a rig fault)}"; A_SEC_T0=$SECONDS; echo; echo "=== $A_SECTION ($(date +%T))"
}

record() {  # record <PASS|FAIL|WARN|RIG|HEAL|RIGFAULT> <name> <reason>
	printf '%s\t%s\t%s\t%s\n' "$1" "$A_SECTION" "$2" "$3" >> "$A_RESULTS"
	printf '%-4s %s%s\n' "$1" "$2" "${3:+: $3}"
}
pass() { record PASS "$1" "$2"; }
fail() { record FAIL "$1" "$2"; }
warn() { record WARN "$1" "$2"; }

# --- the circuit breaker: a broken rig ends the run ------------------------------------------------------------
# A check that finds the RIG broken, not the mod (a game not running, its dev channel silent, ssh to the VM down, the
# virtual pad moving nobody), says so with rig_broken. It is a FAIL like any other, and RIG_BREAK_AT of them (2) end the
# run at once: the rest would measure the rig (F4Multiplayer, 2026-11-01: a VM whose mouse never came up made 18
# failures that proved nothing). A failure of the mod is a plain fail and never trips it.
RIG_BREAK_AT=${RIG_BREAK_AT:-2}
_rig_last_signal() { awk -F'\t' '($1=="FAIL" && index($4, "rig broken (") == 1) || ($1=="VOID" && index($4, "[was FAIL] rig broken (") == 1) || ($1=="RIGFAULT" && index($4, "rig fault (") == 1) {s=substr($4, index($4, "(") + 1); s=substr(s, 1, index(s, ")") - 1)} END{print s}' "$A_RESULTS" 2>/dev/null; }
_rig_broken_count() { awk -F'\t' '($1=="FAIL" && index($4, "rig broken (") == 1) || ($1=="VOID" && index($4, "[was FAIL] rig broken (") == 1) || $1=="RIGFAULT" {n++} END{print n+0}' "$A_RESULTS" 2>/dev/null; }
_rig_stop() {  # _rig_stop <signal> [why]: the run ends here, its verdict FAIL
	A_RIG_STOPPED=1
	if [ -n "${2:-}" ]; then
		record FAIL "run stopped: $1" "$2. The checks after this point were not made"; echo "STOP: $1: $2"
	else
		record FAIL "run stopped: rig broken ($1)" "$(_rig_broken_count) rig-broken failure(s), the last: $1. The checks after this point were not made; fix the rig and run again"
		echo "STOP: rig broken ($1)"
	fi
	declare -F a_on_stop >/dev/null && a_on_stop   # smoke.sh: the crash scan and the restore still run
	a_summary
}
rig_broken() {  # rig_broken <signal: game|input|ssh> <check name> <reason>
	record FAIL "$2" "rig broken ($1): $3"
	[ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && [ -z "$A_RIG_STOPPED" ] && _rig_stop "$1"
	return 0
}
# A rig fault is a check the rig could not make this time with the rest of the rig fine (a scripted walk that stuck on
# a fence before it got anywhere): RIG, not FAIL, and no verdict either way. smoke.sh runs a step that met one once more.
rig_fault() {  # rig_fault <signal: walk|input|state> <check name> <reason>
	record RIG "$2" "rig fault ($1): $3"
}
healed() {  # healed <what> <how>
	record HEAL "$1" "$2"
}
rig_unhealed() {  # rig_unhealed <signal> <what> <reason>
	record RIGFAULT "$2" "rig fault ($1), not healed: $3"
	[ "$(_rig_broken_count)" -ge "$RIG_BREAK_AT" ] && [ -z "$A_RIG_STOPPED" ] && _rig_stop "$1"
	return 0
}
# An attempt at a step: a_attempt_mark before it; a_attempt_rig says whether it met a rig fault (prints the signals); and
# a_attempt_void sets aside (VOID) its lines from its first rig line on. Lines before the rig line stand (the rig was
# still sound when they were made); a FAIL of a check that failed in the first attempt too stands ("in both attempts").
a_attempt_mark() { A_ATTEMPT_LINE=$(wc -l < "$A_RESULTS" 2>/dev/null || echo 0); }
a_attempt_rig() {
	awk -F'\t' -v s="$A_ATTEMPT_LINE" 'NR > s && ($1 == "RIG" || ($1 == "FAIL" && index($4, "rig broken (") == 1)) { printf "%s%s: %s", (n++ ? "; " : ""), $3, substr($4, 1, 160) } END { exit !n }' "$A_RESULTS"
}
a_attempt_void() {
	local t; t=$(mktemp)
	awk -F'\t' -v OFS='\t' -v s="$A_ATTEMPT_LINE" -v first="${1:-}" '
		function rigline() { return $1 == "RIG" || ($1 == "FAIL" && index($4, "rig broken (") == 1) }
		FNR == NR { if (first != "" && FNR > first && FNR <= s && $1 == "VOID" && index($4, "[was FAIL] ") == 1 && index($4, "[was FAIL] rig broken (") != 1) was[$3] = 1; next }
		FNR > s && !on && rigline() { on = 1 }
		on && $1 != "VOID" && $1 != "HEAL" && $1 != "RIGFAULT" {
			if ($1 == "FAIL" && !rigline() && ($3 in was)) $4 = "in both attempts: " $4
			else { $4 = "[was " $1 "] " $4; $1 = "VOID" }
		}
		{ print }' "$A_RESULTS" "$A_RESULTS" > "$t" && cat "$t" > "$A_RESULTS"
	rm -f "$t"
}

# --- comparisons -----------------------------------------------------------------------------------------------
is_num() { [[ $1 =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; }
_cmp() { awk -v a="$1" -v b="$3" "BEGIN{exit !(a $2 b)}"; }  # _cmp a <= b
expect_eq() { if [ "$2" = "$3" ]; then pass "$1" "$2"; else fail "$1" "got '$2', want '$3'"; fi; }
expect_zero() { expect_eq "$1" "$2" 0; }
expect_match() { if [[ $2 =~ $3 ]]; then pass "$1" "$2"; else fail "$1" "got '$2', want /$3/"; fi; }
expect_le() {  # expect_le <label> <got> <max> [unit]
	if ! is_num "$2"; then fail "$1" "got '$2', not a number (want at most $3${4:+ $4})"; elif _cmp "$2" '<=' "$3"; then pass "$1" "$2${4:+ $4} (at most $3)"; else fail "$1" "$2${4:+ $4}, more than $3"; fi
}
expect_ge() {  # expect_ge <label> <got> <min> [unit]
	if ! is_num "$2"; then fail "$1" "got '$2', not a number (want at least $3${4:+ $4})"; elif _cmp "$2" '>=' "$3"; then pass "$1" "$2${4:+ $4} (at least $3)"; else fail "$1" "$2${4:+ $4}, less than $3"; fi
}
expect_between() {  # expect_between <label> <got> <lo> <hi> [unit]
	if ! is_num "$2"; then fail "$1" "got '$2', not a number"; elif _cmp "$2" '>=' "$3" && _cmp "$2" '<=' "$4"; then pass "$1" "$2${5:+ $5} (in $3..$4)"; else fail "$1" "$2${5:+ $5}, outside $3..$4"; fi
}
warn_gt() {  # warn_gt <label> <got> <soft limit> [unit]
	if ! is_num "$2"; then warn "$1" "got '$2', not a number"; elif _cmp "$2" '>' "$3"; then warn "$1" "$2${4:+ $4}, above $3"; else pass "$1" "$2${4:+ $4} (at most $3)"; fi
}

# --- reading the games -----------------------------------------------------------------------------------------
dc() { case "$1" in host) host "${@:2}" ;; vm) vm "${@:2}" ;; *) return 2 ;; esac; }
xyz() { sed -n "s/.*$1(\([-0-9.]*\),\([-0-9.]*\),\([-0-9.]*\)).*/\1 \2 \3/p" <<<"$2" | head -1; }
dist2() {  # dist2 "x y z" "x y z": the distance on the ground, whole units; empty when either is missing
	[ -n "$1" ] && [ -n "$2" ] || return 0
	awk -v a="$1" -v b="$2" 'BEGIN{split(a, p, " "); split(b, q, " "); printf "%.0f\n", sqrt((p[1]-q[1])^2 + (p[2]-q[2])^2)}'
}
# other <host|vm>: the other game
other() { [ "$1" = host ] && echo vm || echo host; }

# --- the plugin log --------------------------------------------------------------------------------------------
# A game's log is SkyrimCoop.log; each start moves it to .1, .2 and .3 (src/LogRotation.h). A mark keeps the size and the
# first line of the current file; the delta is whatever came after, even when the game started again (and rotated) since.
# Its lines read "[hh:mm:ss.mmm] [thread] [L] Prefix: text", L one of D I W E C.
_log_names="SkyrimCoop.log SkyrimCoop.1.log SkyrimCoop.2.log SkyrimCoop.3.log"
_log_fetch() {  # _log_fetch <who> <dir>: newest first, as 0.log 1.log 2.log 3.log
	local n=0 f; mkdir -p "$2"; rm -f "$2"/?.log
	for f in $_log_names; do
		if [ "$1" = host ]; then cp "$HOST_SKSE/$f" "$2/$n.log" 2>/dev/null
		else vmscp "$VM_SSH:$VM_SKSE/$f" "$2/$n.log" 2>/dev/null; fi
		n=$((n + 1))
	done
}
_mark_from() {  # _mark_from <who> <dir>: the mark of that log as the folder holds it now
	MARK_SIZE[$1]=$(stat -c %s "$2/0.log" 2>/dev/null || echo 0)
	MARK_FIRST[$1]=$(head -1 "$2/0.log" 2>/dev/null)
}
log_mark() { _log_fetch "$1" "$A_OUT/logmark-$1"; _mark_from "$1" "$A_OUT/logmark-$1"; }
_log_delta() {  # _log_delta <who> <dir with 0.log..3.log>: the lines since the mark, to stdout
	local d=$2 i hit=-1
	for i in 0 1 2 3; do [ -s "$d/$i.log" ] && [ "$(head -1 "$d/$i.log")" = "${MARK_FIRST[$1]-}" ] && { hit=$i; break; }; done
	if [ $hit -lt 0 ]; then cat "$d/0.log" 2>/dev/null; return; fi   # rotated out of sight: all of the newest file
	tail -c +$((${MARK_SIZE[$1]-0} + 1)) "$d/$hit.log"
	for ((i = hit - 1; i >= 0; i--)); do cat "$d/$i.log"; done
}
log_delta_file() {  # log_delta_file <who>: fetches the log now; the delta goes to $A_OUT/logdelta-<who>.txt (its name printed)
	_log_fetch "$1" "$A_OUT/lognow-$1"; _log_delta "$1" "$A_OUT/lognow-$1" > "$A_OUT/logdelta-$1.txt"
	echo "$A_OUT/logdelta-$1.txt"
}
log_since_mark() { grep -aE -- "$2" "$(log_delta_file "$1")"; }
_prefixes() {  # _prefixes <file> <level>: "Prefix xN" per distinct prefix, most first (never the message text)
	grep -aE "^\[[^]]*\] \[[^]]*\] \[$2\] " "$1" | sed -E "s/^\[[^]]*\] \[[^]]*\] \[$2\] ([A-Za-z0-9_ .-]+):.*/\1/;t;s/^.*\] \[$2\] (.{0,24}).*/\1/" | sort | uniq -c | sort -rn | awk '{c=$1; $1=""; sub(/^ /,""); printf "%s x%d; ", $0, c}'
}
_log_judge() {  # _log_judge <file> <label>: the checks of log_check on a delta already fetched
	local f=$1 e a w
	if [ ! -s "$f" ]; then fail "$2 plugin log" "no new lines since the mark: the log was not read (or the game wrote nothing)"; return; fi
	e=$(grep -caE '^\[[^]]*\] \[[^]]*\] \[(E|C)\] ' "$f")
	a=$(grep -caiE 'assert(ion)? (failed|failure)|ASSERT:' "$f")
	w=$(grep -caE '^\[[^]]*\] \[[^]]*\] \[W\] ' "$f")
	if [ "$e" -gt 0 ]; then fail "$2 plugin log errors" "$e new [E]/[C] line(s): $(_prefixes "$f" E)$(_prefixes "$f" C)(text in $f)"; else pass "$2 plugin log errors" "none in $(wc -l < "$f") new lines"; fi
	if [ "$a" -gt 0 ]; then fail "$2 plugin log asserts" "$a line(s) with assertion text (in $f)"; else pass "$2 plugin log asserts" "none"; fi
	if [ "$w" -gt 0 ]; then warn "$2 plugin log warnings" "$w new [W] line(s): $(_prefixes "$f" W)"; else pass "$2 plugin log warnings" "none"; fi
}
log_check() { _log_judge "$(log_delta_file "$1")" "$2"; }
# The privacy scan (AGENTS.md: no names, Steam ids or IPs in logs). The private patterns are the rig's own file, one
# fixed string a line (the rig's account and machine names); the scan prints counts only, never what matched.
PRIVATE_PATTERNS=${SKYRIMCOOP_PRIVATE_PATTERNS:-$HOME/.f4mp-private-patterns}
_privacy_judge() {  # _privacy_judge <file> <label>
	local f=$1 p s i
	if [ -s "$PRIVATE_PATTERNS" ]; then p=$(grep -caiF -f "$PRIVATE_PATTERNS" "$f"); else p=""; fi
	s=$(grep -caE '765[6][0-9]{13}' "$f")
	i=$(grep -aoE '\b([0-9]{1,3}\.){3}[0-9]{1,3}\b' "$f" | grep -vE '^127\.0\.0\.1$|^0\.0\.0\.0$' | grep -vE '^1\.7\.104\.[0-9]+$' | sort -u | wc -l)
	if [ -z "$p" ]; then warn "$2 privacy: patterns file" "no private-patterns file at $PRIVATE_PATTERNS: that scan was skipped"; else expect_zero "$2 privacy: private patterns" "$p"; fi
	expect_zero "$2 privacy: Steam ids" "$s"; expect_zero "$2 privacy: IP addresses other than 127.0.0.1" "$i"
}
log_privacy() {
	local f=$A_OUT/logdelta-$1.txt
	[ -s "$f" ] || f=$(log_delta_file "$1")
	_privacy_judge "$f" "$2"
}

# --- the verdict -----------------------------------------------------------------------------------------------
a_summary() {  # prints the one-screen summary, writes $A_OUT/summary.txt, exits with the failures (0 = PASS)
	[ -n "$A_SECTION" ] && printf '%s\t%s\n' "$A_SECTION" "$((SECONDS - A_SEC_T0))" >> "$A_SECLOG"; A_SECTION=""
	local np nf nw nr nu nx nh nv nvf vnote mins
	np=$(grep -c '^PASS' "$A_RESULTS"); nf=$(grep -c '^FAIL' "$A_RESULTS"); nw=$(grep -c '^WARN' "$A_RESULTS"); nr=$(grep -c $'^RIG\t' "$A_RESULTS")
	nx=$(grep -c '^RIGFAULT' "$A_RESULTS"); nh=$(grep -c '^HEAL' "$A_RESULTS"); nv=$(grep -c '^VOID' "$A_RESULTS")
	nvf=$(awk -F'\t' '$1=="VOID" && index($4, "[was FAIL]") == 1 && index($4, "[was FAIL] rig broken (") != 1 {n++} END{print n+0}' "$A_RESULTS")
	vnote=""; [ "$nvf" -gt 0 ] && vnote="; $nvf failure(s) set aside after a rig fault: read them under \"set aside\""
	# rig faults whose check never passed in this run: those checks were never made
	nu=$(awk -F'\t' '$1=="PASS"{p[$3]=1} $1=="RIG"{r[$3]=1} END{n=0; for (k in r) if (!(k in p)) n++; print n}' "$A_RESULTS")
	mins=$(awk -v s=$((SECONDS - A_T0)) 'BEGIN{printf "%.1f", s/60}')
	local empty=0; [ "$np" -eq 0 ] && [ "$nf" -eq 0 ] && [ "$nx" -eq 0 ] && [ "$nu" -eq 0 ] && empty=1   # a run that made no check never passes
	{
		echo; echo "================ $A_NAME summary ================"
		echo "started $A_START, ran $mins min: $np passed, $nf FAILED, $nw warnings$([ "$nr" -gt 0 ] && echo ", $nr rig fault(s)")$([ "$nx" -gt 0 ] && echo ", $nx RIG FAULT(S) NOT HEALED")$([ "$nh" -gt 0 ] && echo ", $nh healed")$([ "$nv" -gt 0 ] && echo ", $nv set aside")"
		awk -F'\t' '{printf "  %-34s %3d s\n", $1, $2}' "$A_SECLOG"
		if [ "$nf" -gt 0 ]; then echo "FAILED:"; awk -F'\t' '$1=="FAIL"{printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nx" -gt 0 ]; then echo "RIG FAULTS NOT HEALED (the rig was broken; these checks were not made):"; awk -F'\t' '$1=="RIGFAULT"{printf "  %s%s: %s\n", ($2 == "" ? "" : "[" $2 "] "), $3, substr($4,1,230)}' "$A_RESULTS"; fi
		if [ "$nr" -gt 0 ]; then echo "rig faults (checks the rig could not make that time; no verdict on the mod):"; awk -F'\t' '$1=="RIG"{printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nh" -gt 0 ]; then echo "healed (the rig found in a wrong state and put right):"; awk -F'\t' '$1=="HEAL"{printf "  %s%s: %s\n", ($2 == "" ? "" : "[" $2 "] "), $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nv" -gt 0 ]; then echo "set aside (an attempt that met a rig fault; the step ran again): $nv line(s):"; awk -F'\t' '$1=="VOID" && index($4, "[was FAIL]") == 1 {printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nw" -gt 0 ]; then echo "warnings:"; awk -F'\t' '$1=="WARN"{printf "  [%s] %s: %s\n", $2, $3, substr($4,1,170)}' "$A_RESULTS"; fi
		if [ "$nf" -gt 0 ]; then echo "VERDICT: FAIL ($A_NAME: $nf failed check(s)$([ "$nx" -gt 0 ] && echo "; and $nx rig fault(s) not healed"))"
		elif [ "$nx" -gt 0 ]; then echo "VERDICT: RIG ($A_NAME: no check failed, but $nx rig fault(s) were not healed and their checks were not made: fix the rig and run again$vnote)"
		elif [ "$nu" -gt 0 ]; then echo "VERDICT: RIG ($A_NAME: no check failed; $nu check(s) the rig never made, listed under rig faults: run them again$vnote)"
		elif [ "$empty" = 1 ]; then echo "VERDICT: FAIL ($A_NAME: no check was made)"
		else echo "VERDICT: PASS ($A_NAME$([ "$nh" -gt 0 ] && echo "; $nh rig state(s) healed, every check made")$vnote)"; fi
		echo "details: $A_OUT"
	} | tee "$A_OUT/summary.txt"
	nf=$((nf + nu + nx + empty)); [ "$nf" -gt 125 ] && nf=125
	exit "$nf"
}

a_interrupted() {  # the INT/TERM trap of a run: an interrupted run is a FAILED run, never a PASS with fewer checks
	# Nothing stops the stop work once it has begun: a second Ctrl-C is ignored, and so is a dead pipe (a write to a tee
	# that died fails, it no longer ends the run before its restore)
	trap '' INT TERM PIPE
	fail "run interrupted" "stopped by a signal before the end: the checks after this point were not made"
	echo "interrupted"
	declare -F a_on_stop >/dev/null && a_on_stop
	a_summary
}
# a_log_to <file>: from here on, all the run prints goes to the terminal and to that file. Ctrl-C sends INT to every
# process of the terminal's group, the tee too: a tee that died with it would take the run down at the trap's first
# line (SIGPIPE) before the restore ran. So the tee ignores INT and TERM, and ends when the run does (its pipe closed).
a_log_to() {
	exec > >(trap '' INT TERM; exec tee -i "$1") 2>&1
}

# --- self-test: the helpers without a game ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	CHECKS=$(mktemp -d); A_OUT=""; a_init selftest >/dev/null; bad=0
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	last() { tail -1 "$A_RESULTS" | cut -f1; }
	section s1 >/dev/null
	t "field plain" "$(field hp 'dead=false hp=57 owner=2')" 57
	t "field not a suffix" "$(field dead 'undead=1 dead=0')" 0
	t "field absent" "$(field b 'a=1')" ""
	t "field place" "$(field at 'player 1 at=(1,-2,3) heading=0.5')" "(1,-2,3)"
	t "xyz" "$(xyz 'pos=' 'cell=00000000 world=0000003C pos=(17225,-47204,-128) heading=1.0')" "17225 -47204 -128"
	t "xyz absent" "$(xyz 'at=' 'at=none')" ""
	t "xyz the stand-in's place, not its drawn one" "$(xyz ' at=' 'placed=true dressed=1 at=(10,20,30) heading=0.1 target=(11,21,31) speed=0')" "10 20 30"
	t "dist2" "$(dist2 '0 0 0' '30 40 999')" 50
	t "dist2 missing" "$(dist2 '' '1 2 3')" ""
	t "other" "$(other host)/$(other vm)" vm/host
	expect_le a 3 5 u >/dev/null; t "le pass" "$(last)" PASS
	expect_le a 6 5 u >/dev/null; t "le fail" "$(last)" FAIL
	expect_le a "" 5 >/dev/null; t "le empty is a fail" "$(last)" FAIL
	expect_le a abc 5 >/dev/null; t "le text is a fail" "$(last)" FAIL
	expect_ge a 2.5 2 >/dev/null; t "ge float pass" "$(last)" PASS
	expect_ge a 1 2 >/dev/null; t "ge fail" "$(last)" FAIL
	expect_ge a -1 2 >/dev/null; t "ge negative fail" "$(last)" FAIL
	expect_between a 3 1 5 >/dev/null; t "between pass" "$(last)" PASS
	expect_between a 9 1 5 >/dev/null; t "between fail" "$(last)" FAIL
	expect_eq a x x >/dev/null; t "eq pass" "$(last)" PASS
	expect_eq a x y >/dev/null; t "eq fail" "$(last)" FAIL
	expect_match a joined '^join' >/dev/null; t "match pass" "$(last)" PASS
	expect_zero a 0 >/dev/null; t "zero pass" "$(last)" PASS
	warn_gt a 9 5 >/dev/null; t "warn_gt over" "$(last)" WARN
	warn_gt a 4 5 >/dev/null; t "warn_gt under" "$(last)" PASS
	# the log delta, with a rotation in between (the plugin's own line format)
	d=$(mktemp -d); mk() { printf '%s\n' "$@"; }
	mk '[10:00:00.000] [5316 ] [I] SkyrimCoop v0-0-1-0' '[10:00:01.000] [5316 ] [I] A: one' > "$d/0.log"
	_mark_from t "$d"
	mk '[10:00:00.000] [5316 ] [I] SkyrimCoop v0-0-1-0' '[10:00:01.000] [5316 ] [I] A: one' '[10:00:02.000] [1912 ] [E] Players: boom' '[10:00:03.000] [1912 ] [W] Session: hmm' > "$d/0.log"
	t "delta, same file" "$(_log_delta t "$d" | wc -l)" 2
	cp "$d/0.log" "$d/1.log"; mk '[11:00:00.000] [5316 ] [I] SkyrimCoop v0-0-1-0 again' '[11:00:01.000] [5316 ] [W] Session: again' > "$d/0.log"
	t "delta across a rotation" "$(_log_delta t "$d" | wc -l)" 4
	_log_delta t "$d" > "$d/delta"
	t "prefixes" "$(_prefixes "$d/delta" W)" "Session x2; "
	_log_judge "$d/delta" "t" >/dev/null
	t "log check: an [E] line fails" "$(grep -c $'^FAIL\ts1\tt plugin log errors' "$A_RESULTS")" 1
	t "log check: [W] lines warn" "$(grep -c $'^WARN\ts1\tt plugin log warnings' "$A_RESULTS")" 1
	mk '[10:00:00.000] [1 ] [I] Session: fine' '[10:00:00.000] [1 ] [C] Main: fatal' > "$d/crit"; _log_judge "$d/crit" "c" >/dev/null
	t "log check: a [C] line fails" "$(grep -c $'^FAIL\ts1\tc plugin log errors' "$A_RESULTS")" 1
	: > "$d/empty"; _log_judge "$d/empty" "e" >/dev/null
	t "log check: an empty delta fails (the log was not read)" "$(last)" FAIL
	rm -f "$d/1.log"; t "delta, mark rotated out of sight: the newest file" "$(_log_delta t "$d" | wc -l)" 2
	# the privacy scan: counts, never the text
	PRIVATE_PATTERNS=$d/patterns; printf 'SecretMachine\n' > "$PRIVATE_PATTERNS"
	mk '[1] [1 ] [I] Game version 1.7.104.0' '[1] [1 ] [I] Session: bound 127.0.0.1:7791' > "$d/clean"
	o=$(_privacy_judge "$d/clean" clean); t "privacy: a clean log passes" "$(grep -c '^FAIL' <<<"$o")" 0
	sid=$((7655 + 1))1198000000000   # a made-up Steam id, built so that this file holds no id-shaped number
	mk "[1] [1 ] [I] Steam: $sid on secretmachine at 192.168.1.20" > "$d/dirty"
	o=$(_privacy_judge "$d/dirty" dirty); t "privacy: an id, a name and an address fail" "$(grep -c '^FAIL' <<<"$o")" 3
	case "$o" in *"$sid"*|*192.168.1.20*|*ecretmachine*) echo "FAIL privacy: the scan printed what it found"; bad=1;; *) echo "ok   privacy: counts only";; esac
	PRIVATE_PATTERNS=$d/none; o=$(_privacy_judge "$d/clean" clean); t "privacy: no patterns file is a WARN" "$(grep -c '^WARN' <<<"$o")" 1
	( a_summary >/dev/null ); r=$?; t "summary exit code = failures" "$r" "$(grep -c '^FAIL' "$A_RESULTS")"
	# an interrupted run must fail
	sd=$(mktemp -d); sigout=$( ( A_OUT=$sd; a_init sigtest >/dev/null; section s2 >/dev/null; pass "a check" ok >/dev/null; trap a_interrupted INT TERM; kill -TERM "$BASHPID"; sleep 1; echo "not reached" ) 2>&1 ); r=$?
	rm -rf "$sd"
	[ "$r" -ge 1 ] && echo "ok   interrupted run exits non-zero ($r)" || { echo "FAIL interrupted run exits non-zero: got $r"; bad=1; }
	case "$sigout" in *"run interrupted"*"VERDICT: FAIL"*) echo "ok   interrupted run says VERDICT FAIL";; *) echo "FAIL interrupted run says VERDICT FAIL: [$sigout]"; bad=1;; esac
	# Ctrl-C in a terminal: INT (or TERM) to the run's whole process group, its tee included. The stop work (smoke.sh's
	# restore) must still run, and the log keep the end. The run is started in a group of its own with INT and TERM at
	# their defaults (a background job of a script starts with INT ignored), then the group is signalled as a terminal does.
	for sig in INT TERM; do
		sd=$(mktemp -d)
		cat > "$sd/run.sh" <<-EOF
			export VM_SSH=selftest-no-vm SKYRIMCOOP_STAGE="$SKYRIMCOOP_STAGE"
			. "$RIG/assert.sh"
			A_OUT="$sd/out"; a_init grouprun >/dev/null
			a_on_stop() { echo ran > "$sd/stopwork"; echo "the stop work ran"; }
			a_log_to "$sd/run.log"
			trap a_interrupted INT TERM
			pass "a check" ok
			: > "$sd/ready"
			sleep 30
		EOF
		perl -e '$SIG{INT} = $SIG{TERM} = "DEFAULT"; setpgrp(0, 0); exec @ARGV' bash "$sd/run.sh" >/dev/null 2>&1 &
		pid=$!
		for ((i = 0; i < 100; i++)); do [ -e "$sd/ready" ] && break; command sleep 0.1; done
		command sleep 0.3
		kill -"$sig" -- -"$pid" 2>/dev/null
		for ((i = 0; i < 100; i++)); do grep -q 'VERDICT' "$sd/run.log" 2>/dev/null && break; command sleep 0.1; done
		wait "$pid" 2>/dev/null
		t "Ctrl-C ($sig to the whole group, the tee too): the stop work still runs" "$(cat "$sd/stopwork" 2>/dev/null)" ran
		t "Ctrl-C ($sig to the whole group): the log keeps the end" "$(grep -c 'the stop work ran\|VERDICT: FAIL' "$sd/run.log" 2>/dev/null)" 2
		rm -rf "$sd"
	done
	# each case below is a run of its own (a subshell with its own folder): "<exit code>|<output>"
	brk() {
		local o r sd; sd=$(mktemp -d); o=$( ( A_OUT=$sd; A_RIG_STOPPED=""; A_SECTION=""; a_init brk >/dev/null; section b1 >/dev/null; eval "$1"; echo "REACHED"; a_summary ) 2>&1 ); r=$?; rm -rf "$sd"; echo "$r|$o"
	}
	has() { case "$2" in *"$3"*) echo "ok   $1";; *) echo "FAIL $1: no [$3] in [${2:0:300}]"; bad=1;; esac; }
	hasnt() { case "$2" in *"$3"*) echo "FAIL $1: [$3] in [${2:0:300}]"; bad=1;; *) echo "ok   $1";; esac; }
	code() { [ "${2%%|*}" = "$3" ] && echo "ok   $1" || { echo "FAIL $1: exit [${2%%|*}] want [$3]"; bad=1; }; }
	o=$(brk ':'); has "no check made is a FAIL, never a PASS" "$o" "VERDICT: FAIL (brk: no check was made)"; code "... exit 1" "$o" 1
	o=$(brk 'pass a ok >/dev/null'); has "one pass: PASS" "$o" "VERDICT: PASS (brk)"; code "... exit 0" "$o" 0
	o=$(brk 'rig_broken input "c1" "the pad moved nobody" >/dev/null')
	has "breaker: one rig-broken failure lets the run go on" "$o" REACHED; has "breaker: ... and it is a FAIL" "$o" "rig broken (input): the pad moved nobody"
	o=$(brk 'fail "a mod check" "x" >/dev/null; fail "another" "y" >/dev/null; rig_broken game "c1" "down" >/dev/null')
	has "breaker: the mod's failures never trip it" "$o" REACHED
	o=$(brk 'rig_broken game "c1" "host down" >/dev/null; rig_broken input "c2" "no move"')
	hasnt "breaker: two rig-broken failures stop the run at once" "$o" REACHED; has "breaker: the stop names the signal" "$o" "STOP: rig broken (input)"
	has "breaker: the verdict is FAIL" "$o" "VERDICT: FAIL"
	o=$(brk 'a_on_stop() { echo ON-STOP-RAN; }; rig_broken game "c1" "x" >/dev/null; rig_broken game "c2" "y"')
	has "breaker: the run's own stop work (crash scan, restore) still runs" "$o" ON-STOP-RAN
	o=$(brk '( rig_broken ssh "c1" "x" >/dev/null ); ( rig_broken ssh "c2" "y" >/dev/null ); section b2')
	hasnt "breaker: trips made in subshells stop the run at the next section" "$o" REACHED; has "breaker: ... naming the signal" "$o" "STOP: rig broken (ssh)"
	o=$(brk 'pass "w" ok >/dev/null; rig_fault walk "w" "stuck" >/dev/null')
	has "rig_fault: made in another attempt is a PASS" "$o" "VERDICT: PASS"
	o=$(brk 'pass "other" ok >/dev/null; rig_fault walk "w" "stuck" >/dev/null')
	has "rig_fault: never made is VERDICT RIG" "$o" "VERDICT: RIG (brk: no check failed; 1 check(s)"; code "... exit 1" "$o" 1
	o=$(brk 'healed "state before x" "closed a box" >/dev/null; pass "c" ok >/dev/null')
	has "healed: listed" "$o" "closed a box"; has "healed: a PASS" "$o" "VERDICT: PASS"; code "... exit 0" "$o" 0
	o=$(brk 'a_attempt_mark; fail "trail" "3 of 6" >/dev/null; rig_fault walk "walk" "stuck" >/dev/null; a_attempt_rig >/dev/null && echo SAW-RIG; a_attempt_void; a_attempt_mark; pass "trail" "6 of 6" >/dev/null; pass "walk" ok >/dev/null')
	has "attempt: a rig fault since the mark is seen" "$o" SAW-RIG; has "attempt: a FAIL before the rig line stands" "$o" "trail: 3 of 6"; has "attempt: ... the run is a FAIL" "$o" "VERDICT: FAIL"
	o=$(brk 'a_attempt_mark; rig_fault walk "walk" "stuck" >/dev/null; fail "seen" "stand-in 400 u off" >/dev/null; a_attempt_void; a_attempt_mark; pass "walk" ok >/dev/null; pass "seen" ok >/dev/null')
	has "attempt: a FAIL after the rig line is set aside" "$o" "[was FAIL] stand-in 400 u off"; has "attempt: ... made again, a PASS" "$o" "VERDICT: PASS"
	has "attempt: ... the PASS names it" "$o" "1 failure(s) set aside after a rig fault"
	o=$(brk 'a_attempt_mark; f1=$A_ATTEMPT_LINE; rig_fault walk "w" "y" >/dev/null; fail "seen" "off" >/dev/null; a_attempt_void; a_attempt_mark; rig_fault walk "w" "y" >/dev/null; fail "seen" "off" >/dev/null; a_attempt_void "$f1"; rig_unhealed walk "move: both attempts" "w" >/dev/null')
	has "attempt: a FAIL in both attempts stands" "$o" "in both attempts: off"; has "attempt: ... the run is a FAIL" "$o" "VERDICT: FAIL"
	o=$(brk 'rig_unhealed state "state before move" "vm: a box stays open" >/dev/null; pass "c" ok >/dev/null')
	has "rig_unhealed: listed" "$o" "rig fault (state), not healed: vm: a box stays open"; has "rig_unhealed: VERDICT RIG" "$o" "VERDICT: RIG"
	o=$(brk 'rig_unhealed state "s1" "x" >/dev/null; rig_unhealed state "s2" "y" >/dev/null; section b2')
	hasnt "rig_unhealed: two stop the run" "$o" REACHED
	[ $bad -eq 0 ] && echo "assert.sh selftest: all ok" || echo "assert.sh selftest: FAILED"
	rm -rf "$d" "$CHECKS"; exit $bad
fi
