#!/bin/bash
# lobbytest.sh [--join dev | --join url]: the first test in a game (docs/GETTING-STARTED.md), on the two running games
# start.sh started. Ends with one PASS or FAIL line per point, and the logs of both machines beside the result:
#   point 0  each game: version 1.7.104, the Address Library resolved, the tick runs
#   point 1  each game: Steam usable from the plugin (the library's functions found, logged on, app 489830)
#   point 2  the host opens a friends-only lobby
#   point 3  the VM joins it by its lobby id: both games see two members in the same lobby
#   point 4  messages over Steam's relay, the VM to the host and the host to the VM, each answered, with round trips
# --join dev (the default): the VM's game is told the lobby id through its dev channel (Steam::Join, the plugin's
#   own call). --join url: the VM's Steam client is given steam://joinlobby/489830/<lobby>/<host> on its desktop, as
#   F4Multiplayer's rig joins, which goes through Steam's own join path (the same one an accepted invite takes). The
#   host's Steam id in that address is passed from one game to the other and never printed or written.
# Hold the rig (rigq.sh take <name>; SKYRIMCOOP_RIG_OWNER=<name>). Nothing is placed, moved or typed in either game.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
set -u
VM_RUN=${VM_RUN:-C:/F4MP/in-vm-run.ps1}
join=dev; pings=${PINGS:-5}
while [ $# -gt 0 ]; do
	case "$1" in
	--join) join=$2; shift ;;
	*) sed -n '2,14p' "$0"; exit 2 ;;
	esac
	shift
done
[ "$join" = dev ] || [ "$join" = url ] || { echo "--join dev or --join url"; exit 2; }
rig_guard || exit 1
no_fallout || exit 1

run=$CHECKS/skyrim-lobby-$(date +%Y%m%d-%H%M%S); mkdir -p "$run" || exit 1
exec > >(tee "$run/run.log") 2>&1
declare -A verdict
pass() { verdict[$1]="PASS point $1: $2"; say "PASS point $1: $2"; }
fail() { verdict[$1]="FAIL point $1: $2"; say "FAIL point $1: $2"; }
# wait_for <seconds> <command...>: until the command succeeds, every second
wait_for() { local t=$1 i; shift; for i in $(seq 1 "$t"); do "$@" && return 0; sleep 1; [ $((i % 30)) = 0 ] && lock_keepalive; done; return 1; }
# logline <host|vm> <pattern>: the last line of that game's plugin log matching the pattern (logs are read where they are)
logline() {
	if [ "$1" = host ]; then grep -a -- "$2" "$HOST_SKSE/SkyrimCoop.log" 2>/dev/null | tail -1 | tr -d '\r'
	else vmssh "Select-String -Path \"\$env:USERPROFILE\\Documents\\My Games\\Skyrim Special Edition\\SKSE\\SkyrimCoop.log\" -SimpleMatch -Pattern '$2' | Select-Object -Last 1 | ForEach-Object { \$_.Line }"; fi
}
finish() {
	say "cleaning up: the host stops hosting, the VM leaves"
	host "steam stophost" >/dev/null; vm "steam leave" >/dev/null
	bash "$RIG/fetchlogs.sh" "$run/logs" >/dev/null
	echo; echo "Result ($run):"
	for p in 0 1 2 3 4; do echo "  ${verdict[$p]:-FAIL point $p: not reached (an earlier point failed)}"; done | tee "$run/result.txt"
	grep -q FAIL "$run/result.txt" && exit 1; exit 0
}
lock_keepalive

say "the two games: dev channels"
running_host "$GAME_EXE" || { fail 0 "Skyrim does not run on the host (start.sh first)"; finish; }
running_vm "$GAME_EXE" || { fail 0 "Skyrim does not run on the VM (start.sh first)"; finish; }
vm_token_fetch && tunnel || { fail 0 "the VM's dev channel can't be reached (no token, or no tunnel)"; finish; }
hs=$(host status); vs=$(vm status)
say "host: $hs"; say "vm:   $vs"
[ -n "$hs" ] && [ -n "$vs" ] || { fail 0 "a game's dev channel gives no status (host '${hs:0:20}', vm '${vs:0:20}')"; finish; }

# point 0: the game's version, the Address Library and the tick, from both games
p0=""
for who in host vm; do
	s=$([ $who = host ] && echo "$hs" || echo "$vs")
	[ "$(field game "$s")" = 1.7.104.0 ] || p0+="$who game=$(field game "$s"); "
	[ "$(field addrlib "$s")" = ok ] || p0+="$who addrlib=$(field addrlib "$s"); "
	[ "$(field ticks "$s")" -gt 0 ] 2>/dev/null || p0+="$who ticks=$(field ticks "$s") (the tick does not run: SKSE's task queue?); "
done
[ -z "$p0" ] && pass 0 "both games are 1.7.104.0, the Address Library resolved, the tick runs ($(logline host 'Address Library:' | sed 's/.*Address Library: //'))" || fail 0 "$p0"

# point 1: Steam from inside the game
p1=""
for who in host vm; do
	s=$([ $who = host ] && echo "$hs" || echo "$vs")
	[ "$(field steam "$s")" = ready ] || p1+="$who steam: $($who "steam why"); "
	[ "$(field loggedon "$s")" = yes ] || p1+="$who loggedon=$(field loggedon "$s"); "
	[ "$(field app "$s")" = $APP_ID ] || p1+="$who app=$(field app "$s"); "
done
[ -z "$p1" ] && pass 1 "both: Steam ready, logged on, app $APP_ID; relay host=$(field relay "$hs") vm=$(field relay "$vs") (100 = usable)" || { fail 1 "$p1"; finish; }

# point 2: the host's lobby
say "host: asking for a lobby"
host "steam stophost" >/dev/null; vm "steam leave" >/dev/null; sleep 2
r=$(host "steam host"); [ "$r" = hosting ] || { fail 2 "the host's game refused: $r"; finish; }
lobby_up() { lobby=$(field lobby "$(host status)"); [ -n "$lobby" ] && [ "$lobby" != 0 ]; }
if wait_for 90 lobby_up; then
	pass 2 "lobby $lobby open: $(logline host 'First test: lobby open' | sed 's/.*First test: //')"
else
	fail 2 "no lobby in 90 s; the host's log: $(logline host 'Steam:' | cut -c1-200)"; finish
fi

# point 3: the VM joins by lobby id
host "probe reset" >/dev/null; vm "probe reset" >/dev/null
if [ "$join" = dev ]; then
	say "vm: joining lobby $lobby through its dev channel"
	r=$(vm "steam join $lobby"); [ "$r" = joining ] || { fail 3 "the VM's game refused: $r"; finish; }
else
	url=$(host "steam joinurl")
	[[ $url =~ ^steam://joinlobby/$APP_ID/$lobby/[0-9]+$ ]] || { fail 3 "the host gave no join address"; finish; }
	vm "steam expectjoin" >/dev/null
	say "vm: giving its Steam client the lobby's steam://joinlobby address"
	vmssh "& $VM_RUN -Name \"SkyrimCoop-Join-\$(Get-Date -Format HHmmss)\" -Exe 'C:\Program Files (x86)\Steam\steam.exe' -Arguments '$url'" | grep -q started \
		|| { url=""; fail 3 "could not start the VM's Steam with the join address"; finish; }
	url=""
fi
both_in() { local h v; h=$(host status); v=$(vm status); [ "$(field lobby "$v")" = "$lobby" ] && [ "$(field members "$v")" = 2 ] && [ "$(field members "$h")" = 2 ]; }
if wait_for 90 both_in; then
	pass 3 "both games in lobby $lobby with 2 members. vm: $(logline vm 'First test: joined lobby' | sed 's/.*First test: //'). host: $(logline host 'First test: a player joined' | sed 's/.*First test: //')"
else
	fail 3 "not both in lobby $lobby after 90 s: host $(host status | grep -o 'lobby=[0-9]* members=[0-9]*'), vm $(vm status | grep -o 'lobby=[0-9]* members=[0-9]*'); the VM's log: $(logline vm 'Steam:' | cut -c1-200)"
	finish
fi

# point 4: the relay, the VM first (a friend's Steam lets in only those it has sent to), then the host
say "relay: the VM pings the host ($pings), then the host pings the VM ($pings)"
r=$(vm "probe send $pings"); [[ $r == sending* ]] || { fail 4 "the VM's game won't send: $r"; finish; }
answered() { [ "$(field answered "$($1 'probe report')")" = "$pings" ]; }
wait_for 30 answered vm
r=$(host "probe send $pings"); [[ $r == sending* ]] || { fail 4 "the host's game won't send: $r"; finish; }
wait_for 30 answered host
hr=$(host "probe log"); vr=$(vm "probe log")
say "host: $hr"; say "vm:   $vr"
p4=""
[ "$(field answered "$vr")" = "$pings" ] || p4+="the VM's pings to the host: $(field answered "$vr") of $pings answered; "
[ "$(field answered "$hr")" = "$pings" ] || p4+="the host's pings to the VM: $(field answered "$hr") of $pings answered; "
[ "$(field pingsAnswered "$hr")" -ge "$pings" ] 2>/dev/null || p4+="the host answered $(field pingsAnswered "$hr") pings; "
[ "$(field pingsAnswered "$vr")" -ge "$pings" ] 2>/dev/null || p4+="the VM answered $(field pingsAnswered "$vr") pings; "
rtt() { sed -n 's/.*rttMs \(.*\)/\1/p' <<<"$1"; }
[ -z "$p4" ] && pass 4 "both ways over the relay: VM to host round trip $(rtt "$vr") ms; host to VM $(rtt "$hr") ms" || fail 4 "$p4"
finish
