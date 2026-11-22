#!/bin/bash
# start.sh [--cell <cell>] [--host-only | --vm-only]   starts Skyrim through SKSE on both machines, unattended, and
#                                                       waits for the plugin; then makes the VM's dev channel reachable
# start.sh stop [--host-only | --vm-only]              closes Skyrim on both (and puts Skyrim.ini back after a --cell start)
#
# The host's game is started here (this shell runs on the host's desktop); the VM's through ssh and the rig's
# scheduled-task helper ($VM_RUN), since an ssh session can't show a window. Without --cell both stay at the main
# menu, which is enough for the Steam test; --cell qasmoke (Skyrim's test cell) puts each game's player there with
# coc from the main menu over the developer channel (a default character, no character creation), and the start
# passes only when each game says (`where`) its player is in that world with no box open over it (menus ... modal=0;
# a new game's one-time questions, such as Survival Mode's, are answered by the plugin on the rig: main.cpp).
# Steam must be running and signed in on both machines.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
set -u
VM_RUN=${VM_RUN:-C:/F4MP/in-vm-run.ps1}
what=start; cell=""; host=1; vmside=1
while [ $# -gt 0 ]; do
	case "$1" in
	stop) what=stop ;;
	--cell) cell=$2; shift ;;
	--host-only) vmside=0 ;;
	--vm-only) host=0 ;;
	*) sed -n '2,4p' "$0"; exit 2 ;;
	esac
	shift
done
# checked here as start.ps1 checks it: the cell goes into the VM's remote command line unquoted
[ -z "$cell" ] || [[ $cell =~ ^[A-Za-z0-9_]+$ ]] || { echo "--cell takes a cell's editor id (letters, digits and _ only)"; exit 2; }
rig_guard || exit 1
lock_keepalive

if [ $what = stop ]; then
	# Stop-Process, as the Fallout rig's restarts do: a scripted quit through the menus would need input this PR doesn't have
	[ $host = 1 ] && { taskkill //IM "$GAME_EXE.exe" //F >/dev/null 2>&1; sleep 3; powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$RIG/start.ps1")" -RestoreIni; say "host: stopped"; }
	[ $vmside = 1 ] && { vmssh "Stop-Process -Name $GAME_EXE -Force -ErrorAction SilentlyContinue; Start-Sleep 3; powershell -NoProfile -ExecutionPolicy Bypass -File $VM_DIR/start.ps1 -RestoreIni"; say "vm: stopped"; }
	exit 0
fi

no_fallout || exit 1
cellarg=(); vmcell=""
# --cell is not given to start.ps1: Skyrim SE never runs Skyrim.ini's sStartingConsoleCommand (both games sat at the main
# menu). Each game is told "coc <cell>" over its developer channel once it is up (into_world, below).
rc=0
if [ $vmside = 1 ]; then
	# the VM first: it is the slower of the two, and starts while the host's game loads
	vmssh "New-Item -ItemType Directory -Force '$VM_DIR' | Out-Null" >/dev/null
	vmscp "$RIG/start.ps1" "$VM_SSH:$VM_DIR/" || { say "vm: could not copy start.ps1"; exit 1; }
	say "vm: starting"
	# The VM has no audio device: its game runs with the sound off (start.ps1 -NoAudio; VM_NO_AUDIO=0 to leave it on).
	vmaudio=""; [ "$VM_NO_AUDIO" = 1 ] && vmaudio="-NoAudio"
	vmssh "powershell -NoProfile -ExecutionPolicy Bypass -File $VM_DIR/start.ps1 -Game '$VM_GAME' $vmcell $vmaudio -ViaTask $VM_RUN -WaitSeconds 300; 'exit ' + \$LASTEXITCODE" > "$STAGE/skyrim-vm-start.log" &
	vmpid=$!
fi
if [ $host = 1 ]; then
	say "host: starting"
	powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$RIG/start.ps1")" -Game "$(cygpath -w "$HOST_GAME")" "${cellarg[@]}" || rc=1
	[ $rc = 0 ] && { r=$(host ping); [ "$r" = pong ] && say "host: dev channel answers" || { say "host: dev channel gives no answer ('$r')"; rc=1; }; }
fi
if [ $vmside = 1 ]; then
	wait "$vmpid"
	cat "$STAGE/skyrim-vm-start.log"
	grep -q '^exit 0' "$STAGE/skyrim-vm-start.log" || rc=1
	if vm_token_fetch && tunnel; then
		r=$(vm ping); [ "$r" = pong ] && say "vm: dev channel answers (through the tunnel on $VM_TUNNEL_PORT)" || { say "vm: dev channel gives no answer ('$r')"; rc=1; }
	else
		say "vm: no dev token read, or no tunnel made"; rc=1
	fi
fi
# into_world <host|vm>: coc to the cell from the main menu, then up to 90 s for the player in the world with no modal box
into_world() {
	local who=$1 r w i
	r=$($who "console coc $cell"); [ "$r" = ran ] || { say "$who: coc $cell refused ('$r')"; return 1; }
	for i in $(seq 1 90); do
		w=$($who where)
		if [[ $w == cell=* ]] && [[ $w == *" modal=0"* ]]; then say "$who: in the world: $w"; return 0; fi
		sleep 1; [ $((i % 30)) = 0 ] && lock_keepalive
	done
	say "$who: not in the world with no box open after 90 s: $w"; return 1
}
if [ $rc = 0 ] && [ -n "$cell" ]; then
	[ $host = 1 ] && { into_world host || rc=1; }
	[ $vmside = 1 ] && { into_world vm || rc=1; }
fi
exit $rc
