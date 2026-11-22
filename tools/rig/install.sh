#!/bin/bash
# install.sh [--build <folder>] [--host-only | --vm-only]   puts SKSE64, the Address Library file, Crash Logger and
#                                                            the plugin into both machines' Skyrim folders
# install.sh undo [--host-only | --vm-only]                  takes out exactly what the last install put in
#
# Steps: check the three downloads' hashes; unpack them with 7-Zip into a new folder of their own under
# $STAGE/skyrim-install (nothing from them is run); pick the files the game needs into a payload laid out like the game
# folder, with the plugin from the build and the rig's ini (the shipped one with bDevChannel = true); list its hashes;
# then install.ps1 on the host and, over ssh, on the VM (it checks the list, backs up what it replaces and records
# every file it writes). Both games must be closed. Hold the rig (rigq.sh take <name>, SKYRIMCOOP_RIG_OWNER=<name>).
#
# What goes where (the game folder on each machine):
#   skse64_loader.exe, skse64_1_7_104.dll        <game>\                  (SKSE64 2.3.1)
#   Data\Scripts\*.pex, Data\Scripts\Source\*.psc <game>\Data\Scripts\     (SKSE's own scripts)
#   versionlib-1-7-104-0.bin                      <game>\Data\SKSE\Plugins\ (Address Library v13; only this game's file)
#   CrashLogger.dll, CrashLogger.pdb, msdia140.dll <game>\Data\SKSE\Plugins\ (Crash Logger 1.27.0)
#   SkyrimCoop.dll, SkyrimCoop.pdb, SkyrimCoop.ini <game>\Data\SKSE\Plugins\
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
set -u

DL=$STAGE/skyrim-downloads
SEVENZIP=${SEVENZIP:-/c/Program Files/7-Zip/7z.exe}
# The files as the maintainer downloaded them on 2026-11-21. A new version is a new hash here, read and checked first.
SKSE_SHA=7bad616ed360823a027f8828801d91e3a4aa2eacd952023af7f3e31adb2ae250        # SKSE64 2.3.1 (Nexus 30379)
ADDR_SHA=be0c7c07fb63fd1c1403690d4797c7a8fdf8799118acceb605f285d7b0ee055f        # Address Library All in One (1.7.104.0) v13 (Nexus 32444)
CRASH_SHA=eb112aa69b4e4f595a0230e43f7f080dbdc533647d4986fc042692f461baaad3       # CrashLogger_1.27.0.7z (GitHub, MIT)
SKSE_DIR=skse64_2_03_01
ADDR_FILE=versionlib-1-7-104-0.bin
WORK=$STAGE/skyrim-install
HOST_RECORD=$WORK/host-record.tsv
VM_RECORD=$VM_DIR/vm-record.tsv

mode=install; build=$REPO/build/windows/x64/releasedbg; host=1; vmside=1
while [ $# -gt 0 ]; do
	case "$1" in
	undo) mode=undo ;;
	--build) build=$2; shift ;;
	--host-only) vmside=0 ;;
	--vm-only) host=0 ;;
	*) sed -n '2,4p' "$0"; exit 2 ;;
	esac
	shift
done

rig_guard || exit 1
no_fallout || exit 1
if [ $host = 1 ] && running_host "$GAME_EXE"; then say "refusing: Skyrim runs on the host"; exit 1; fi
if [ $vmside = 1 ] && running_vm "$GAME_EXE"; then say "refusing: Skyrim runs on the VM"; exit 1; fi
winpath() { cygpath -w "$1"; }

if [ $mode = undo ]; then
	rc=0
	if [ $host = 1 ]; then
		powershell -NoProfile -ExecutionPolicy Bypass -File "$(winpath "$RIG/install.ps1")" -Game "$(winpath "$HOST_GAME")" -Undo -Record "$(winpath "$HOST_RECORD")" || rc=1
		[ $rc = 0 ] && mv -f "$HOST_RECORD" "$HOST_RECORD.undone-$(date +%Y%m%d-%H%M%S)"
	fi
	if [ $vmside = 1 ]; then
		vmssh "powershell -NoProfile -ExecutionPolicy Bypass -File $VM_DIR/install.ps1 -Game '$VM_GAME' -Undo -Record $VM_RECORD; if (\$LASTEXITCODE -eq 0) { Move-Item $VM_RECORD ('$VM_RECORD.undone-' + (Get-Date -Format yyyyMMdd-HHmmss)); 'vm undo ok' } else { 'vm undo FAILED' }" | tee /dev/stderr | grep -q 'vm undo ok' || rc=1
	fi
	exit $rc
fi

# one download each, with the hash it had when it was approved
pick() {  # pick <folder> <pattern> <sha256>: prints the file; fails unless exactly one matches and its hash is right
	local found=("$DL/$1"/$2) h
	[ ${#found[@]} = 1 ] && [ -f "${found[0]}" ] || { say "want exactly one $2 in $DL/$1, found ${#found[@]}" >&2; return 1; }
	h=$(sha256sum "${found[0]}" | cut -c1-64)
	[ "$h" = "$3" ] || { say "$(basename "${found[0]}"): sha256 $h, not the approved $3: not installed" >&2; return 1; }
	printf '%s\n' "${found[0]}"
}
skse=$(pick skse64 '*.7z' $SKSE_SHA) || exit 1
addr=$(pick addresslibrary '*.zip' $ADDR_SHA) || exit 1
crash=$(pick crashlogger 'CrashLogger_*.7z' $CRASH_SHA) || exit 1
for f in SkyrimCoop.dll SkyrimCoop.pdb; do [ -f "$build/$f" ] || { say "no $f in $build (build first, or --build <folder of a build.yml artifact>)"; exit 1; }; done
[ -x "$SEVENZIP" ] || { say "no 7-Zip at $SEVENZIP"; exit 1; }
say "downloads checked: SKSE64, Address Library, Crash Logger (sha256 as approved)"

stamp=$(date +%Y%m%d-%H%M%S)
unpack=$WORK/unpack-$stamp; payload=$WORK/payload-$stamp
mkdir -p "$unpack/skse" "$unpack/addr" "$unpack/crash" "$payload/Data/SKSE/Plugins" "$payload/Data/Scripts" || exit 1
"$SEVENZIP" x -y -bso0 -bsp0 -o"$(winpath "$unpack/skse")" "$(winpath "$skse")" || exit 1
"$SEVENZIP" x -y -bso0 -bsp0 -o"$(winpath "$unpack/addr")" "$(winpath "$addr")" || exit 1
"$SEVENZIP" x -y -bso0 -bsp0 -o"$(winpath "$unpack/crash")" "$(winpath "$crash")" || exit 1

need() { [ -f "$1" ] || { say "the archive does not hold $1 where expected: its layout changed; nothing installed"; exit 1; }; }
need "$unpack/skse/$SKSE_DIR/skse64_loader.exe"; need "$unpack/skse/$SKSE_DIR/skse64_1_7_104.dll"
need "$unpack/addr/SKSE/Plugins/$ADDR_FILE"
for f in CrashLogger.dll CrashLogger.pdb msdia140.dll; do need "$unpack/crash/SKSE/Plugins/$f"; done
cp -p "$unpack/skse/$SKSE_DIR/skse64_loader.exe" "$unpack/skse/$SKSE_DIR/skse64_1_7_104.dll" "$payload/"
cp -rp "$unpack/skse/$SKSE_DIR/Data/Scripts/." "$payload/Data/Scripts/"
cp -p "$unpack/addr/SKSE/Plugins/$ADDR_FILE" "$payload/Data/SKSE/Plugins/"
cp -p "$unpack/crash/SKSE/Plugins/CrashLogger.dll" "$unpack/crash/SKSE/Plugins/CrashLogger.pdb" "$unpack/crash/SKSE/Plugins/msdia140.dll" "$payload/Data/SKSE/Plugins/"
cp -p "$build/SkyrimCoop.dll" "$build/SkyrimCoop.pdb" "$payload/Data/SKSE/Plugins/"
# the rig's ini: the shipped one with the developer channel on
sed 's/^bDevChannel = false/bDevChannel = true/' "$REPO/dist/SkyrimCoop.ini" > "$payload/Data/SKSE/Plugins/SkyrimCoop.ini"
grep -q '^bDevChannel = true' "$payload/Data/SKSE/Plugins/SkyrimCoop.ini" || { say "could not turn the developer channel on in the rig's ini"; exit 1; }
nscripts=$(find "$payload/Data/Scripts" -type f | wc -l)
[ "$nscripts" -gt 50 ] || { say "only $nscripts SKSE script files: the archive's layout changed?"; exit 1; }
(cd "$payload" && find . -type f | sed 's|^\./||' | sort | while read -r f; do sha256sum "$f"; done) > "$WORK/payload-$stamp.sha256"
say "payload ready: $(wc -l < "$WORK/payload-$stamp.sha256") files ($nscripts SKSE scripts), list $WORK/payload-$stamp.sha256"

rc=0
if [ $host = 1 ]; then
	say "host: installing into the game folder"
	powershell -NoProfile -ExecutionPolicy Bypass -File "$(winpath "$RIG/install.ps1")" -Game "$(winpath "$HOST_GAME")" -Payload "$(winpath "$payload")" \
		-Expect "$(winpath "$WORK/payload-$stamp.sha256")" -Record "$(winpath "$HOST_RECORD")" || { say "host install FAILED (above)"; rc=1; }
fi
if [ $vmside = 1 ] && [ $rc = 0 ]; then
	say "vm: copying the payload and installing"
	vmssh "New-Item -ItemType Directory -Force '$VM_DIR' | Out-Null; 'ok'" | grep -q ok || { say "vm: ssh gives no answer"; exit 1; }
	vmscp -r "$payload" "$VM_SSH:$VM_DIR/" && vmscp "$WORK/payload-$stamp.sha256" "$RIG/install.ps1" "$VM_SSH:$VM_DIR/" || { say "vm: copying FAILED"; exit 1; }
	vmssh "powershell -NoProfile -ExecutionPolicy Bypass -File $VM_DIR/install.ps1 -Game '$VM_GAME' -Payload $VM_DIR/payload-$stamp -Expect $VM_DIR/payload-$stamp.sha256 -Record $VM_RECORD; 'exit ' + \$LASTEXITCODE" \
		| tee "$WORK/vm-install-$stamp.log" | grep -q '^exit 0' || { say "vm install FAILED: $WORK/vm-install-$stamp.log"; rc=1; }
	cat "$WORK/vm-install-$stamp.log"
fi
[ $rc = 0 ] && say "installed. Undo: bash tools/rig/install.sh undo" || say "install did not finish; what was written is in the records, and undo takes it out"
exit $rc
