#!/bin/bash
# env.sh: where things are on the rig, for Skyrim. Sourced by every script in tools/rig; nothing in this repository
# names a person or a machine. Ported from F4Multiplayer's tools/rig/env.sh (commit 5b535694c): the same rig (a host
# PC and a Hyper-V VM, each with its own Steam account), the same lock, the same rig folder; Skyrim's paths, process,
# ports and file names.
#
# What differs per rig lives in the rig's own folder ($STAGE), shared with F4Multiplayer's rig:
#   rig.conf              shell assignments, read if present: VM_SSH=<user>@<vm host name> (required), and optionally
#                         HOST_GAME, VM_GAME, HOST_SKSE (below)
#   vm_ssh_key            the key for that ssh login
#   rig.lock, rigq.sh     the one lock both projects take (AGENTS.md: one user at a time, one game at a time)
#   in-vm-run.ps1         copied to C:\F4MP in the VM: runs a program on the VM's desktop (an ssh session can't show windows)
#   skyrim-downloads/     SKSE64, the Address Library and Crash Logger as downloaded (install.sh checks their hashes)
# Files these scripts write there all start with "skyrim": skyrim-vm-dev.token, skyrim-install/, checks/skyrim-*.
RIG=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$RIG/../.." && pwd)
STAGE=${SKYRIMCOOP_STAGE:-$HOME/F4MP-VM/stage}
[ -f "$STAGE/rig.conf" ] && . "$STAGE/rig.conf"
: "${VM_SSH:?set VM_SSH=<user>@<host> in $STAGE/rig.conf}"
HOST_GAME=${HOST_GAME:-F:/SteamLibrary/steamapps/common/Skyrim Special Edition}          # the host game's folder
VM_GAME=${VM_GAME:-C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition}  # the VM game's folder
HOST_MYGAMES=${HOST_MYGAMES:-$HOME/Documents/My Games/Skyrim Special Edition}           # Skyrim.ini, SkyrimPrefs.ini, Saves
HOST_SKSE=${HOST_SKSE:-$HOST_MYGAMES/SKSE}                                               # SKSE's and the plugin's logs, the dev token
VM_MYGAMES=${VM_MYGAMES:-Documents/My Games/Skyrim Special Edition}                     # the same on the VM, from its user profile (scp form)
VM_SKSE=${VM_SKSE:-$VM_MYGAMES/SKSE}
VM_KEY=${VM_KEY:-$STAGE/vm_ssh_key}
VM_DIR=${VM_DIR:-C:/SkyrimCoop}           # the VM's folder for this project's scripts, payloads and install records
CHECKS=${SKYRIMCOOP_CHECKS:-$STAGE/checks} # where runs write their logs and results
GAME_EXE=SkyrimSE                         # the process name on both machines
APP_ID=489830
# The developer channel (src/DevChannel.h): the host's game on 7791; the VM's game's 7791 is reached through an ssh
# tunnel on this PC's 7793. Fallout 4's are 7790 and 7792: neither project's tools can reach the other's game.
DEV_PORT=7791
VM_TUNNEL_PORT=7793
HOST_TOKEN="$HOST_SKSE/SkyrimCoop_dev.token"
VM_TOKEN="$STAGE/skyrim-vm-dev.token"

# say <text>: one line with the time, to stdout (a run's own log keeps it)
say() { printf '%s %s\n' "$(date +%T)" "$*"; }

vmssh() { ssh -i "$VM_KEY" -o LogLevel=ERROR -o ConnectTimeout=20 -o BatchMode=yes "$VM_SSH" "$@" 2>/dev/null | tr -d '\r'; }
vmscp() { scp -q -p -i "$VM_KEY" -o LogLevel=ERROR -o BatchMode=yes "$@"; }

# dc_one <port> <token file> <command>: one developer-channel command over a raw socket; prints the reply line. The token
# is read from its file and sent, never printed. Fails when nothing answers. (F4Multiplayer's dcfast.sh, cut down: one
# game at a time, three tries, no hang bookkeeping yet.)
dc_one() {
	local port=$1 file=$2 cmd=$3 tok reply out i
	[ -s "$file" ] || return 1
	tok=$(tr -d '\r\n ' < "$file")
	for i in 1 2 3; do
		if exec 3<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
			echo "auth $tok" >&3
			if read -t 5 -r reply <&3 && [ "${reply%$'\r'}" = ok ]; then
				echo "$cmd" >&3
				read -t 20 -r out <&3
				exec 3>&-
				printf '%s\n' "${out%$'\r'}"
				return 0
			fi
			exec 3>&-
		fi
		sleep 0.5
	done
	return 1
}
host() { dc_one "$DEV_PORT" "$HOST_TOKEN" "$*"; }
vm() { dc_one "$VM_TUNNEL_PORT" "$VM_TOKEN" "$*"; }

# field <key> <status line>: the value of key=value in a status reply ("" when absent)
field() { tr ' ' '\n' <<<"$2" | sed -n "s/^$1=//p" | head -1; }

# tunnel: an ssh tunnel from this PC's 7793 to the VM's 7791, made when nothing listens on 7793
tunnel_pid() { netstat -ano 2>/dev/null | tr -d '\r' | awk -v p="127.0.0.1:$VM_TUNNEL_PORT" '$2 == p && $4 == "LISTENING" {print $5; exit}'; }
tunnel() {
	local i; [ -n "$(tunnel_pid)" ] && return 0
	(ssh -i "$VM_KEY" -N -L "$VM_TUNNEL_PORT:127.0.0.1:$DEV_PORT" -o ServerAliveInterval=30 -o ExitOnForwardFailure=yes -o BatchMode=yes \
		-o ConnectTimeout=20 "$VM_SSH" >/dev/null 2>&1 &)
	for i in $(seq 1 20); do sleep 0.5; [ -n "$(tunnel_pid)" ] && return 0; done; return 1
}
# vm_token_fetch: the VM game's token (a new one at each start) copied into $VM_TOKEN, never printed. 1 = none read.
vm_token_fetch() {
	local t; t=$(vmssh 'Get-Content "$env:USERPROFILE\Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop_dev.token" -Raw' | tr -d '[:space:]')
	[[ $t =~ ^[0-9a-f]{64}$ ]] || return 1
	( umask 077; printf '%s' "$t" > "$VM_TOKEN" )
}

# running_host <exe name> / running_vm <exe name>: 0 when that program runs there
running_host() { tasklist //FI "IMAGENAME eq $1.exe" //NH 2>/dev/null | grep -qi "^$1\.exe"; }
running_vm() { vmssh "if (Get-Process $1 -ErrorAction SilentlyContinue) { 'up' }" | grep -q up; }

# no_fallout: refuses (1) while Fallout 4 runs on either machine: the two games never run on a machine at once
no_fallout() {
	if running_host Fallout4; then say "refusing: Fallout 4 runs on the host"; return 1; fi
	if running_vm Fallout4; then say "refusing: Fallout 4 runs on the VM"; return 1; fi
	return 0
}

# rig_guard: 0 when $STAGE/rig.lock is held by $SKYRIMCOOP_RIG_OWNER (taken with rigq.sh take <that name>). The owner is
# never read from the lock to fill the variable: a caller that never took the rig must not be handed a name to use.
rig_guard() {
	[ "${SKYRIMCOOP_SKIP_LOCK_CHECK:-}" = 1 ] && return 0  # :- because the scripts run with set -u
	local owner; owner=$(head -1 "$STAGE/rig.lock/owner" 2>/dev/null | cut -d' ' -f1)
	if [ -z "$owner" ]; then say "refusing: nobody holds the rig. Take it: bash $STAGE/rigq.sh take <name>; then run with SKYRIMCOOP_RIG_OWNER=<name>"; return 1; fi
	if [ "$owner" != "${SKYRIMCOOP_RIG_OWNER:-}" ]; then say "refusing: the rig is held by someone else (or SKYRIMCOOP_RIG_OWNER is not the name you took it with)"; return 1; fi
	return 0
}
# lock_keepalive: rigq.sh frees a lock as stale after 15 minutes when Fallout 4 is down on both machines and its log is idle,
# which is how every Skyrim run looks to it; touching the lock's folder keeps a Skyrim run's turn. Call it now and then.
lock_keepalive() { [ -d "$STAGE/rig.lock" ] && touch "$STAGE/rig.lock"; return 0; }
