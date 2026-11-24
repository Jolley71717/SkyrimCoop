#!/bin/bash
# crashlib.sh: the crash logs a run leaves. Sourced after assert.sh (smoke.sh does).
#
#     crash_mark                 # the crash logs (Crash Logger's crash-*.log in each machine's SKSE folder) there now
#     crash_scan "label"         # a FAIL per machine with a crash log new since the mark, copied into the run's folder and
#                                # named with whether SkyrimCoop.dll is in its call stack; a PASS per machine with none
#     crash_classify <file>      # "ours" when SkyrimCoop.dll is in the crash's call stack (the lines before REGISTERS:),
#                                # "not ours" otherwise (the module list at the end always names it, so it is not read)
#
# A crash is a FAIL whoever's it is: a run that lost a game proves nothing after that point. "not ours" tells the reader
# where to look first (the VM's audio-thread crash of 2026-11-21 had no SkyrimCoop frame), it never makes it a pass.
# `bash tools/rig/crashlib.sh selftest` checks the reading with no game.
[ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ] && : "${VM_SSH:=selftest-no-vm}" && export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
declare -F record >/dev/null || . "$(dirname "${BASH_SOURCE[0]}")/assert.sh"

crash_classify() {
	awk '/^[[:space:]]*REGISTERS:/ { exit } /SkyrimCoop\.dll/ { hit = 1 } END { print (hit ? "ours" : "not ours") }' "$1"
}
# _crash_list host|vm: the crash logs' names, one a line, oldest first; 1 when the machine could not be read (the VM's
# listing ends in a line of its own, so ssh down is told apart from no crash log)
_crash_list() {
	local o
	if [ "$1" = host ]; then (cd "$HOST_SKSE" 2>/dev/null && ls -1tr crash-*.log 2>/dev/null); return 0; fi
	o=$(vmssh 'Get-ChildItem "$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\SKSE" -Filter "crash-*.log" -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | ForEach-Object { $_.Name }; "end of list"' | tr -d '\r')
	[ "$(tail -n1 <<<"$o")" = "end of list" ] || return 1
	grep -vx 'end of list' <<<"$o"; return 0
}
crash_mark() {
	local w; for w in host vm; do _crash_list $w > "$A_OUT/crash-before-$w.txt" || : > "$A_OUT/crash-unread-$w"; done
}
# _crash_new <before file> <now file>: the names in now and not in before
_crash_new() { grep -vxF -f "$1" "$2" 2>/dev/null || { [ -s "$1" ] || cat "$2"; }; }
crash_scan() {
	local w n f c list
	for w in host vm; do
		if ! _crash_list $w > "$A_OUT/crash-now-$w.txt"; then rig_broken ssh "$1: $w has no new crash log" "its crash logs could not be listed (ssh down?): look in its SKSE folder"; continue; fi
		if [ -e "$A_OUT/crash-unread-$w" ]; then warn "$1: $w has no new crash log" "its crash logs could not be listed at the mark, so none can be told new: look in its SKSE folder"; continue; fi
		list=$(_crash_new "$A_OUT/crash-before-$w.txt" "$A_OUT/crash-now-$w.txt" | grep -E '^crash-[0-9-]+\.log$')
		if [ -z "$list" ]; then pass "$1: $w has no new crash log" "$(wc -l < "$A_OUT/crash-now-$w.txt") older ones left alone"; continue; fi
		mkdir -p "$A_OUT/crash/$w"; n=""
		for f in $list; do
			if [ $w = host ]; then cp -p "$HOST_SKSE/$f" "$A_OUT/crash/$w/" 2>/dev/null; else vmscp "$VM_SSH:$VM_SKSE/$f" "$A_OUT/crash/$w/" 2>/dev/null; fi
			if [ -s "$A_OUT/crash/$w/$f" ]; then c=$(crash_classify "$A_OUT/crash/$w/$f"); else c="not copied"; fi
			n="$n $f (SkyrimCoop in its call stack: $([ "$c" = ours ] && echo yes || echo "$c"));"
		done
		fail "$1: $w has no new crash log" "the game crashed:${n%;} (copies in $A_OUT/crash/$w; they hold the machine's folder paths: never attach them as they are)"
	done
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	bad=0; d=$(mktemp -d)
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	printf '%s\n' 'CRASH TIME: x' 'PROBABLE CALL STACK:' '	[0] 0x7FF6 SkyrimSE.exe+07964F2' '	[1] 0x7FF9 SkyrimCoop.dll+008D6CB	test rax, rax' 'REGISTERS:' '	RAX 0x0' 'MODULES:' '	SkyrimCoop.dll 0x7FF9' > "$d/ours.log"
	printf '%s\n' 'CRASH TIME: x' 'PROBABLE CALL STACK:' '	[0] 0x7FF6 SkyrimSE.exe+07964F2' '	[1] 0x7FF9 XAudio2_7.dll+0001' 'REGISTERS:' '	RSP+4F8 (void* -> SkyrimCoop.dll+008D6CB)' 'MODULES:' '	SkyrimCoop.dll 0x7FF9' 'SKSE PLUGINS:' '	SkyrimCoop.dll v0.0.1' > "$d/theirs.log"
	t "a SkyrimCoop frame in the call stack is ours" "$(crash_classify "$d/ours.log")" ours
	t "SkyrimCoop only in the registers, stack and module list is not ours" "$(crash_classify "$d/theirs.log")" "not ours"
	printf 'crash-2026-11-22-00-40-31.log\n' > "$d/before"; printf 'crash-2026-11-22-00-40-31.log\ncrash-2026-11-22-01-00-00.log\n' > "$d/now"
	t "new since the mark" "$(_crash_new "$d/before" "$d/now")" crash-2026-11-22-01-00-00.log
	: > "$d/empty"; t "new since an empty mark: all" "$(_crash_new "$d/empty" "$d/now" | wc -l)" 2
	t "none new" "$(_crash_new "$d/now" "$d/now")" ""
	# crash_scan with both machines faked: a new host crash that is ours, nothing new on the VM
	CHECKS=$d; A_OUT=$d/run; mkdir -p "$A_OUT/crash"; a_init crashtest >/dev/null; section c >/dev/null
	HOST_SKSE=$d/hostskse; mkdir -p "$HOST_SKSE"; cp "$d/ours.log" "$HOST_SKSE/crash-2026-11-22-01-00-00.log"
	_crash_list() { if [ "$1" = host ]; then printf 'crash-2026-11-22-01-00-00.log\n'; else printf 'crash-2026-11-21-23-00-00.log\n'; fi; }
	printf '' > "$A_OUT/crash-before-host.txt"; printf 'crash-2026-11-21-23-00-00.log\n' > "$A_OUT/crash-before-vm.txt"
	o=$(crash_scan "end")
	t "crash_scan: a new host crash is a FAIL naming our frame" "$(grep -c '^FAIL end: host has no new crash log: the game crashed: crash-2026-11-22-01-00-00.log (SkyrimCoop in its call stack: yes)' <<<"$o")" 1
	t "crash_scan: an old VM crash is left alone" "$(grep -c '^PASS end: vm has no new crash log' <<<"$o")" 1
	# the VM's listing read with ssh faked: the end line tells an empty folder from no answer
	vmssh() { printf '%s\r\n' "$@" >/dev/null; [ -n "${FAKE_VM:-}" ] && printf "$FAKE_VM"; }
	unset -f _crash_list; eval "$(sed -n '/^_crash_list() {/,/^}/p' "${BASH_SOURCE[0]}")"
	FAKE_VM='crash-a.log\r\nend of list\r\n'; t "the VM's listing: names, without the end line" "$(_crash_list vm)" crash-a.log
	FAKE_VM='end of list\r\n'; _crash_list vm >/dev/null; t "the VM's listing: none is read" "$?" 0
	FAKE_VM=''; _crash_list vm >/dev/null; t "the VM's listing: no answer is not read as none" "$?" 1
	o=$(crash_scan "end2"); t "crash_scan: no answer from the VM is not a PASS" "$(grep -c '^PASS end2: vm' <<<"$o"),$(grep -c 'end2: vm has no new crash log.*could not be listed' <<<"$o")" 0,1
	[ $bad -eq 0 ] && echo "crashlib.sh selftest: all ok" || echo "crashlib.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
