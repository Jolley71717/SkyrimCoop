#!/bin/bash
# smoke.sh [--only step,...] [--skip step,...] [--build <folder>] [--cell <cell>] [--join url|dev] [--keep]
# smoke.sh --list | --restore <run folder> | --selftest
#
# The functional test of two games of Skyrim with a verdict: named steps (steps.sh), each a set of asserting checks read
# from both games' developer channels and plugin logs, both directions wherever something moves; a summary that ends in
# VERDICT: PASS, FAIL or RIG (assert.sh); an exit code that is the number of failed checks (0 = PASS). Fast checks of
# what the mod does, not a soak: a full run is about ten minutes. tools/rig/README.md says how to read it, and how each
# new feature adds its own step.
#
# What a run does, in order:
#   1. backs up both machines' Skyrim.ini and SkyrimPrefs.ini, and lists both Saves folders and both SkyrimCoop.dll
#   2. --build <folder>: installs that build on both machines (install.sh; both games closed first)
#   3. starts the games that are not running (start.sh --cell <cell>: coc from the main menu, never Continue or Load)
#   4. marks both plugin logs and the crash logs, then runs the steps, each behind a state guard: a player out of the
#      world is put back with coc, a walk left going is stopped, games not in one session are joined again (each a HEAL);
#      a state that stays wrong is a RIGFAULT and the step is not run. A step that meets a rig fault (a walk that never
#      got going, a press that never reached the game) runs once more, its lines from the fault on set aside (VOID).
#      Two rig-broken failures stop the run (the breaker).
#   5. restores, always, even after a stop or Ctrl-C: pads off, sessions left, the VM's test save moved aside (never
#      deleted), leftover join tasks removed; then (unless --keep) both games closed, both ini files put back byte for
#      byte, --build's install undone and the DLL checked to be the one from before, and both Saves listings compared.
#   6. the summary and the verdict.
#
# --only/--skip pick steps by name (--list shows them). --cell (default Riverwood) is where coc puts the players.
# --join url (default) gives the VM's Steam the lobby's steam://joinlobby address, the path an accepted invite takes;
# --join dev tells the VM's game the lobby id. --keep leaves both games running and in the world after the run (the ini
# files are then put back later with --restore <run folder>, once both games are closed). CHECK_DLL=<md5> fails the run
# unless both machines run that build.
#
# Hold the rig: bash $STAGE/rigq.sh take <name>, then SKYRIMCOOP_RIG_OWNER=<name>. Fallout 4 must not be running.
. "$(dirname "${BASH_SOURCE[0]}")/assert.sh"
. "$RIG/crashlib.sh"
. "$RIG/steps.sh"
set -u

only=""; skip=""; build=""; keep=""; mode=run; restore_from=""
while [ $# -gt 0 ]; do
	case "$1" in
	--only) only=${2//,/ }; shift ;;
	--skip) skip=${2//,/ }; shift ;;
	--build) build=$2; shift ;;
	--cell) SMOKE_CELL=$2; shift ;;
	--join) JOIN=$2; shift ;;
	--keep) keep=1 ;;
	--list) mode=list ;;
	--restore) mode=restore; restore_from=$2; shift ;;
	--selftest) mode=selftest ;;
	*) sed -n '2,3p' "$0"; exit 2 ;;
	esac
	shift
done
for s in $only $skip; do case " $STEPS " in *" $s "*) ;; *) echo "no step '$s' (steps: $STEPS)"; exit 2 ;; esac; done
[ "$JOIN" = url ] || [ "$JOIN" = dev ] || { echo "--join url or --join dev"; exit 2; }
[[ $SMOKE_CELL =~ ^[A-Za-z0-9_]+$ ]] || { echo "--cell takes a cell's editor id (letters, digits and _ only)"; exit 2; }
[ -z "$build" ] || [ -f "$build/SkyrimCoop.dll" ] || { echo "--build: no SkyrimCoop.dll in '$build'"; exit 2; }
selected() { case " $skip " in *" $1 "*) return 1 ;; esac; [ -z "$only" ] && return 0; case " $only " in *" $1 "*) return 0 ;; esac; return 1; }

if [ $mode = list ]; then
	for s in $STEPS; do printf '  %-12s %s\n' "$s" "${STEP_INFO[$s]}"; done
	exit 0
fi

# --- backup and restore ----------------------------------------------------------------------------------------
INIS="Skyrim.ini SkyrimPrefs.ini"
_md5s_host() { (cd "$HOST_MYGAMES" && md5sum $INIS 2>/dev/null) | awk '{print $2, $1}'; }
_md5s_vm() { vmssh "\$d = \"\$env:USERPROFILE\\Documents\\My Games\\Skyrim Special Edition\"; foreach (\$f in 'Skyrim.ini', 'SkyrimPrefs.ini') { if (Test-Path \"\$d\\\$f\") { \$f + ' ' + (Get-FileHash -Algorithm MD5 -LiteralPath \"\$d\\\$f\").Hash.ToLower() } }"; }
# Names, sizes and times of a Saves folder, sorted; kept in the run's folder and compared, never printed (a save's name
# can hold a character's name).
_saves_host() { (cd "$HOST_MYGAMES/Saves" 2>/dev/null && find . -maxdepth 1 -type f -printf '%f\t%s\t%T@\n' | sort); }
_saves_vm() { vmssh '$d = "$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\Saves"; Get-ChildItem $d -File -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { "{0}`t{1}`t{2}" -f $_.Name, $_.Length, $_.LastWriteTimeUtc.Ticks }' | sort; }
smoke_backup() {
	local b=$A_OUT/backup f
	mkdir -p "$b/host" "$b/vm"
	for f in $INIS; do cp -p "$HOST_MYGAMES/$f" "$b/host/" 2>/dev/null; vmscp "$VM_SSH:$VM_MYGAMES/$f" "$b/vm/" 2>/dev/null; done
	_md5s_host > "$b/host.md5"; _md5s_vm > "$b/vm.md5"
	_saves_host > "$b/host-saves.tsv"; _saves_vm > "$b/vm-saves.tsv"
	dll_md5 host > "$b/host-dll.md5"; dll_md5 vm > "$b/vm-dll.md5"
	if [ "$(wc -l < "$b/host.md5")" = 2 ] && [ "$(wc -l < "$b/vm.md5")" = 2 ] && [ -s "$b/host/Skyrim.ini" ] && [ -s "$b/vm/Skyrim.ini" ]; then
		pass "backup: both machines' Skyrim.ini and SkyrimPrefs.ini" "$b ($(wc -l < "$b/host-saves.tsv") host saves and $(wc -l < "$b/vm-saves.tsv") VM saves listed)"
	else
		_rig_stop "no backup" "the ini files of both machines could not all be copied into $b: nothing is changed without a backup"
	fi
}
# restore_ini <backup folder>: puts back each ini that differs, byte for byte, then compares; a check per machine
restore_ini() {
	local b=$1 f diff
	for f in $INIS; do
		[ -s "$b/host/$f" ] && ! cmp -s "$b/host/$f" "$HOST_MYGAMES/$f" && cp -p "$b/host/$f" "$HOST_MYGAMES/$f"
		grep -q "^$f $(awk -v f="$f" '$1 == f {print $2}' "$b/vm.md5")\$" <(_md5s_vm) || { [ -s "$b/vm/$f" ] && vmscp "$b/vm/$f" "$VM_SSH:$VM_MYGAMES/$f"; }
	done
	diff=$(diff <(_md5s_host) "$b/host.md5" >/dev/null && echo same || echo differ)
	[ "$diff" = same ] && pass "restore: the host's ini files are as before the run" "byte for byte" || fail "restore: the host's ini files are as before the run" "their md5 differ from $b/host.md5 after putting the backup back"
	diff=$(diff <(_md5s_vm) "$b/vm.md5" >/dev/null && echo same || echo differ)
	[ "$diff" = same ] && pass "restore: the VM's ini files are as before the run" "byte for byte" || fail "restore: the VM's ini files are as before the run" "their md5 differ from $b/vm.md5 after putting the backup back"
}
games_down() { ! running_host "$GAME_EXE" && ! running_vm "$GAME_EXE"; }

SMOKE_ENDED=""
smoke_restore() {
	[ -n "$SMOKE_ENDED" ] && return; SMOKE_ENDED=1
	local b=$A_OUT/backup w r stamp
	section "restore"
	if [ ! -s "$b/host.md5" ]; then warn "restore: nothing to put back" "the run stopped before its backup was made: nothing on either machine was changed by it"; return; fi
	for w in host vm; do still $w; done
	dc vm 'session leave' >/dev/null; dc host 'session leave' >/dev/null; dc host 'steam stophost' >/dev/null
	stamp=$(date +%Y%m%d-%H%M%S)
	# the VM's test save: moved aside into the project's folder on the VM, never deleted
	r=$(vmssh "\$d = \"\$env:USERPROFILE\\Documents\\My Games\\Skyrim Special Edition\\Saves\"; \$m = '$VM_DIR\\saves-moved-$stamp'; \$f = @(Get-ChildItem \$d -File -Filter '$SAVE_NAME.*' -ErrorAction SilentlyContinue); if (\$f.Count) { New-Item -ItemType Directory -Force \$m | Out-Null; \$f | Move-Item -Destination \$m }; 'moved ' + \$f.Count")
	[[ $r == moved* ]] && pass "restore: the VM's test save is moved aside" "${r#moved } file(s) into $VM_DIR\\saves-moved-$stamp" || warn "restore: the VM's test save is moved aside" "no answer over ssh: look in the VM's Saves for $SAVE_NAME.*"
	r=$(vmssh "\$t = @(Get-ScheduledTask -TaskName 'SkyrimCoop-Join-*' -ErrorAction SilentlyContinue); \$t | Unregister-ScheduledTask -Confirm:\$false -ErrorAction SilentlyContinue; 'removed ' + \$t.Count")
	[[ $r == removed* ]] && pass "restore: no join task left on the VM" "${r#removed } removed" || warn "restore: no join task left on the VM" "no answer over ssh"
	if [ -n "$keep" ]; then
		echo "--keep: both games left running. Once they are closed: bash tools/rig/smoke.sh --restore $A_OUT"
	else
		bash "$RIG/start.sh" stop >/dev/null 2>&1
		wait_until 30 games_down && pass "restore: both games closed" "" || fail "restore: both games closed" "a game still runs 30 s after start.sh stop"
		restore_ini "$b"
		if [ -n "$build" ]; then
			bash "$RIG/install.sh" undo > "$A_OUT/install-undo.log" 2>&1 && pass "restore: the build's install undone" "" || fail "restore: the build's install undone" "install.sh undo failed: $A_OUT/install-undo.log"
		fi
		expect_eq "restore: the host's SkyrimCoop.dll is the one from before" "$(dll_md5 host)" "$(cat "$b/host-dll.md5")"
		expect_eq "restore: the VM's SkyrimCoop.dll is the one from before" "$(dll_md5 vm)" "$(cat "$b/vm-dll.md5")"
		r=$(tunnel_pid); [ -n "$r" ] && taskkill //PID "$r" //F >/dev/null 2>&1
	fi
	# the host's saves never change in a run; the VM's only by its test save, moved aside above
	if diff -q <(_saves_host) "$b/host-saves.tsv" >/dev/null; then pass "restore: the host's Saves folder is as before" "$(wc -l < "$b/host-saves.tsv") files, unchanged"
	else fail "restore: the host's Saves folder is as before" "it changed in the run (the host's game must never save): compare $b/host-saves.tsv"; fi
	if diff -q <(_saves_vm) "$b/vm-saves.tsv" >/dev/null; then pass "restore: the VM's Saves folder is as before" "$(wc -l < "$b/vm-saves.tsv") files, unchanged"
	else warn "restore: the VM's Saves folder is as before" "it differs from $b/vm-saves.tsv (an autosave?): look before the next run"; fi
}
a_on_stop() {  # a run stopped by the breaker or a signal: the crash scan, then the restore
	[ -n "${CRASH_SCANNED:-}" ] || [ ! -f "$A_OUT/crash-before-host.txt" ] || { CRASH_SCANNED=1; crash_scan "at the stop"; }
	smoke_restore
}

if [ $mode = restore ]; then
	[ -f "$restore_from/backup/host.md5" ] || { echo "--restore: no backup in '$restore_from'"; exit 2; }
	rig_guard || exit 2
	games_down || { echo "--restore: close both games first (start.sh stop): a game writes its ini as it quits"; exit 2; }
	A_OUT=$restore_from/restore-$(date +%H%M%S); a_init restore >/dev/null; section "restore"
	restore_ini "$restore_from/backup"; a_summary
fi

# --- the run ---------------------------------------------------------------------------------------------------
if [ $mode = run ]; then
	rig_guard || exit 2
	a_init smoke
	exec > >(tee "$A_OUT/run.log") 2>&1
	trap a_interrupted INT TERM
	lock_keepalive
	section "set-up"
	no_fallout || _rig_stop "Fallout 4 runs" "Fallout 4 runs on the rig: one game at a time"
	smoke_backup
	if [ -n "$build" ]; then
		games_down || bash "$RIG/start.sh" stop > "$A_OUT/stop-before-install.log" 2>&1
		if bash "$RIG/install.sh" --build "$build" > "$A_OUT/install.log" 2>&1; then pass "set-up: the build is installed on both" "$(md5sum "$build/SkyrimCoop.dll" | cut -d' ' -f1)"
		else _rig_stop "install failed" "install.sh --build $build failed: $A_OUT/install.log"; fi
		CHECK_DLL=${CHECK_DLL:-$(md5sum "$build/SkyrimCoop.dll" | cut -d' ' -f1)}
	fi
	sides=""
	running_host "$GAME_EXE" || sides="host"
	running_vm "$GAME_EXE" || sides="$sides vm"
	if [ -n "$sides" ]; then
		flag=""; [ "$sides" = host ] && flag=--host-only; [ "$sides" = " vm" ] && flag=--vm-only
		if bash "$RIG/start.sh" --cell "$SMOKE_CELL" $flag > "$A_OUT/start.log" 2>&1; then healed "set-up: both games running" "started:$sides (start.sh --cell $SMOKE_CELL)"
		else rig_unhealed game "set-up: both games running" "start.sh failed for$sides: $(tail -3 "$A_OUT/start.log" | tr '\n' ' ' | cut -c1-200)"; _rig_stop "the games did not start" "see $A_OUT/start.log"; fi
	fi
	vm_token_fetch && tunnel || rig_broken ssh "set-up: the VM's dev channel is reachable" "no token read, or no tunnel made"
	log_mark host; log_mark vm; crash_mark

	# run <step>: one step behind the state guard; a rig fault in a step that can run again runs it once more
	RERUN_STEPS="see move_vm move_host gait"
	run() {
		local s=$1 first sig
		selected "$s" || return 0
		[ -n "$A_RIG_STOPPED" ] && return 0
		lock_keepalive
		case " world join preflight final " in *" $s "*) ;; *) guard_world "$s" || return 0 ;; esac
		case " $SESSION_STEPS " in *" $s "*) guard_session "$s" || return 0 ;; esac
		a_attempt_mark; first=$A_ATTEMPT_LINE; "step_$s"
		case " $RERUN_STEPS " in *" $s "*) ;; *) return 0 ;; esac
		if [ -z "$A_RIG_STOPPED" ] && sig=$(a_attempt_rig); then
			a_attempt_void
			healed "$s: run again after a rig fault" "$sig"
			A_AGAIN=1; a_attempt_mark; "step_$s"; A_AGAIN=""
			if [ -z "$A_RIG_STOPPED" ] && sig=$(a_attempt_rig); then
				a_attempt_void "$first"
				rig_unhealed walk "$s: a rig fault in both attempts" "$sig"
			fi
		fi
	}
	# guard_world <step>: both players in the world with no box open and nothing walking; put right where it can be
	guard_world() {
		local w
		for w in host vm; do
			[ "$(dc $w ping)" = pong ] || { rig_broken game "state before $1: $w answers" "its dev channel gives no answer"; return 1; }
			[[ $(dc $w walkto) == walking* ]] && { still $w; healed "state before $1" "$w: a walk left going was stopped"; }
			dc $w 'pad off' >/dev/null
			in_world $w && continue
			wait_until 20 in_world $w && continue
			if [[ $(dc $w where) == "not in world"* ]]; then place $w "state before $1: $w in the world" || return 1
			else rig_unhealed state "state before $1: $w in the world" "a box stays open: $(dc $w where | grep -o 'menus=.*')"; return 1; fi
		done
		return 0
	}
	# guard_session <step>: both games in one session; joined again once if not (a HEAL), else the run stops: nothing
	# after this point can be checked without a session
	guard_session() {
		local r
		joined_both && return 0
		dc vm 'session leave' >/dev/null; dc host 'session leave' >/dev/null; dc host 'steam stophost' >/dev/null; sleep 3
		if r=$(session_up); then healed "state before $1" "the two games put in one session again"; return 0; fi
		_rig_stop "no session" "the two games could not be put in one session before $1 ($r)"
		return 1
	}

	for s in $STEPS; do run "$s"; done
	[ -z "${CRASH_SCANNED:-}" ] && ! selected final && { CRASH_SCANNED=1; section "crash logs"; crash_scan "end"; }
	selected final && CRASH_SCANNED=1
	smoke_restore
	a_summary
fi

# --- self-test: the script's own wiring, no rig ------------------------------------------------------------------
if [ $mode = selftest ]; then
	bad=0; me=$RIG/smoke.sh
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	export VM_SSH SKYRIMCOOP_STAGE
	t "--list names every step" "$(bash "$me" --list | awk '{print $1}' | tr '\n' ' ')" "$STEPS "
	bash "$me" --only nosuchstep >/dev/null 2>&1; t "--only with an unknown step is refused" "$?" 2
	bash "$me" --skip see,nosuch >/dev/null 2>&1; t "--skip with an unknown step is refused" "$?" 2
	bash "$me" --join carrier >/dev/null 2>&1; t "--join other than url or dev is refused" "$?" 2
	bash "$me" --cell 'Riverwood;x' >/dev/null 2>&1; t "--cell with other characters is refused" "$?" 2
	bash "$me" --build /nonexistent >/dev/null 2>&1; t "--build without a DLL is refused" "$?" 2
	o=$(SKYRIMCOOP_RIG_OWNER=someone bash "$me" --only see 2>&1); r=$?
	t "a run without the rig's lock is refused" "$r" 2; case "$o" in *"nobody holds the rig"*) echo "ok   ... saying why";; *) echo "FAIL ... saying why: [$o]"; bad=1;; esac
	for s in $STEPS; do grep -q "\`$s\`" "$RIG/README.md" || { echo "FAIL README.md does not describe step $s"; bad=1; }; done
	echo "ok   README.md names every step"
	only=""; skip="see"; selected see && { echo "FAIL selected: a skipped step"; bad=1; } || echo "ok   selected: a skipped step is not run"
	only="see move_vm"; skip=""; selected move_vm && ! selected join && echo "ok   selected: --only" || { echo "FAIL selected: --only"; bad=1; }
	[ $bad -eq 0 ] && echo "smoke.sh selftest: all ok" || echo "smoke.sh selftest: FAILED"
	exit $bad
fi
