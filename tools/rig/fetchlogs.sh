#!/bin/bash
# fetchlogs.sh [<folder>]: copies both machines' Skyrim logs into <folder>/host and <folder>/vm (default
# $CHECKS/skyrim-logs-<time>) and lists what came: the plugin's (SkyrimCoop.log and the three kept before it), SKSE's
# (skse64.log, skse64_loader.log), Crash Logger's (crash-*.log, in the same SKSE folder) and the game's Papyrus log
# when there is one. Reading only: nothing on either machine is changed, and it runs while the games run.
#
# The plugin's log names no player and no account (Redact.h), but SKSE's and Crash Logger's logs hold the Windows user's
# folder paths: the fetched copies stay in the rig's folder; never attach or commit them as they are.
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
set -u
out=${1:-$CHECKS/skyrim-logs-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out/host" "$out/vm" || exit 1
n=0
for f in "$HOST_SKSE"/SkyrimCoop*.log "$HOST_SKSE"/skse64*.log "$HOST_SKSE"/crash-*.log "$HOST_MYGAMES/Logs/Script/Papyrus.0.log"; do
	[ -f "$f" ] && cp -p "$f" "$out/host/" && n=$((n + 1))
done
say "host: $n files"
# one ssh for the list, then one scp of the files that exist (a pattern that matches nothing is no error)
list=$(vmssh '$d = "$env:USERPROFILE\Documents\My Games\Skyrim Special Edition"; @(Get-ChildItem "$d\SKSE" -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "SkyrimCoop*.log" -or $_.Name -like "skse64*.log" -or $_.Name -like "crash-*.log" } | ForEach-Object { "SKSE/" + $_.Name }) + @(if (Test-Path "$d\Logs\Script\Papyrus.0.log") { "Logs/Script/Papyrus.0.log" }) | ForEach-Object { $_ }')
m=0
while read -r rel; do
	[ -n "$rel" ] || continue
	vmscp "$VM_SSH:$VM_MYGAMES/$rel" "$out/vm/" && m=$((m + 1))
done <<<"$list"
say "vm: $m files"
ls -l "$out/host" "$out/vm" | sed 's/^/  /'
echo "$out"
