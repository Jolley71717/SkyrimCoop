#!/bin/bash
# steps/forms.sh: the rig step `forms` (protocol 4, docs/DESIGN.md "Form ids across load orders"), sourced by steps.sh.
# Two games whose plugins load in another order play together, and an armor one player puts on shows on the other's
# stand-in as that game's own form, both ways. Not run in a game yet.
#
#   step_forms         the step (smoke.sh runs it only when named: smoke.sh --only join,forms). It changes the VM's
#                      load order for a while (below), which the maintainer has to agree to before it runs by default.
#   forms_heal         smoke.sh's set-up: a load order a run left changed (it died mid-step: the harness killed, the ssh
#                      link or the host down) is put back from the VM's backup before anything else runs (a HEAL), or
#                      the run stops (never run on a changed game without saying so)
#   forms_restore      the end of the step, and smoke.sh's restore (its trap too): puts the VM's files back byte for
#                      byte if the step changed them; nothing otherwise
#   bash tools/rig/steps/forms.sh selftest      the step's reading and file logic, no game and no rig (CI runs it)
#
# What it does:
#   1. Both games load FORMS_A and FORMS_B (two full Creation Club masters every Anniversary Edition game has) in the
#      same places. The VM's game is closed, the two are swapped in its Skyrim.ccc (the Creation Club's load order) and,
#      when it lists both, in its plugins.txt too (which of the two files decides is not certain: both are swapped, and
#      the step reads the places the game then gives), each backed up on the VM first with its SHA-256
#      ($VM_DIR/forms-backup) and copied to this run's folder. The VM's game is started again in FORMS_CELL (coc, as
#      start.sh does) and joins the host's session again.
#   2. The VM's game now loads the two in each other's place (else a rig fault: the rig could not make the orders
#      differ, and the reply says which files were swapped). It is let in, and its `forms` line says it translates
#      (identity=0) with nothing lacking either way.
#   3. Gear both ways: the VM's player puts on the first armor of FORMS_ARMOR_PLUGIN a player can wear (console additem
#      and equipitem: set-up, not movement), and the host's stand-in of them must be placed, dressed, and have that armor
#      on, as the host's game numbers it (another form ID than the VM's: each game finds it by the plugin's file name and
#      the form's number). It is taken off and must leave the stand-in. Then the host does the same with the VM watching.
#   4. Neither game skipped a form: both have every plugin.
#   5. Restore: the VM's game closed, its files put back (hashes compared), started again and joined again, and its
#      places checked to be the host's again, so the steps after this one see the rig as before.
#
# The extra-plugin case (a plugin only one game has) is not done here: it needs a plugin installed in one game only,
# and mods go on the rig only through install.sh from the maintainer's downloads. The unit tests cover it
# (tests/FormTranslateTests.cpp, tests/ServerTests.cpp).

FORMS_A=${FORMS_A:-ccBGSSSE001-Fish.esm}
FORMS_B=${FORMS_B:-ccBGSSSE025-AdvDSGS.esm}
FORMS_ARMOR_PLUGIN=${FORMS_ARMOR_PLUGIN:-$FORMS_B}
FORMS_CELL=${FORMS_CELL:-}              # where the VM's game is put after each restart; empty: the harness's cell (forms_cell)
FORMS_WAIT=${FORMS_WAIT:-30}        # s: for gear to show on (or leave) a stand-in, a new stand-in placed in between
FORMS_STATE=${FORMS_STATE:-}        # this run's copies of the VM's files, while the VM's are changed
forms_cell() { echo "${FORMS_CELL:-${SMOKE_CELL:-Riverwood}}"; }  # read when used: smoke.sh sets --cell after sourcing

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# forms_swap <file> <a> <b>: the file with the lines naming plugins a and b swapped (any case; a plugins.txt line's
# leading * and a CR at a line's end go with the line's own place), every other byte as it was; to stdout. 1 when
# either line is missing.
forms_swap() {
	local file=$1 a=$2 b=$3 out
	# BINMODE=3: Windows' gawk (Git Bash, where the rig runs) drops each CR otherwise; elsewhere it changes nothing
	out=$(awk -v BINMODE=3 -v a="$a" -v b="$b" '
		function bare(s) { sub(/\r$/, "", s); sub(/^\*/, "", s); return tolower(s) }
		{ line[NR] = $0; name = bare($0); if (name == tolower(a)) ia = NR; if (name == tolower(b)) ib = NR }
		END {
			if (!ia || !ib) exit 1
			# the names change places; each line keeps its own mark (*) and its own end (CR)
			na = line[ia]; nb = line[ib]
			ca = sub(/\r$/, "", na); cb = sub(/\r$/, "", nb)
			sa = sub(/^\*/, "", na); sb = sub(/^\*/, "", nb)
			line[ia] = (sa ? "*" : "") nb (ca ? "\r" : "")
			line[ib] = (sb ? "*" : "") na (cb ? "\r" : "")
			for (i = 1; i <= NR; i++) printf "%s\n", line[i]
			printf "x"
		}' "$file") || return 1
	out=${out%x}
	# awk ends every line with a newline: drop the one the file did not have
	if [ -s "$file" ] && [ "$(tail -c1 "$file" | od -An -c | tr -d ' ')" != '\n' ]; then
		out=${out%$'\n'}
	fi
	printf '%s' "$out"
}
# forms_lists <file> <plugin>: 0 when the file has a line naming that plugin (a plugins.txt line may start with *)
forms_lists() { awk -v p="$2" '{ s = $0; sub(/\r$/, "", s); sub(/^\*/, "", s); if (tolower(s) == tolower(p)) f = 1 } END { exit !f }' "$1"; }

# forms_place <reply of `forms place`>: full:N, light:N or none
forms_place() { sed -n 's/^place=\([^ ]*\).*/\1/p' <<<"$1"; }
# forms_id <reply of `forms find|armor|id`>: the 8-hex-digit id, or "" (none, or an error)
forms_id() { sed -n 's/^id=\([0-9A-F]\{8\}\) .*/\1/p' <<<"$1"; }
# forms_local <reply of `forms armor|id`>: the form's number within its plugin, hex
forms_local() { tr ' ' '\n' <<<"$1" | sed -n 's/^local=\([0-9A-F]*\)$/\1/p' | head -1; }

# forms_standin_why <reply of `standins`> <id>: 0 when the first friend's stand-in is in the world, dressed, and has
# the armor with that ID on (equippedIds: what the actor wears, not what the looks named). Says why not otherwise.
forms_standin_why() {
	local s=$1 id=$2 standin on
	standin=$(field standin "$s")
	if [ -z "$standin" ] || [ "$standin" = 00000000 ]; then echo "no stand-in in the world (standin=${standin:-none})"; return 1; fi
	[ "$(field dressed "$s")" = 1 ] || { echo "the stand-in $standin is not dressed yet (dressed=$(field dressed "$s"))"; return 1; }
	on=$(field equippedIds "$s")
	[[ ",$on," == *",$id,"* ]] && return 0
	echo "the stand-in $standin has on ${on:-none}, not $id (its friend's looks named $(field wornIds "$s"))"
	return 1
}
# forms_standin_lacks <reply of `standins`> <id>: 0 when a stand-in is in the world, dressed, without that armor on
forms_standin_lacks() {
	local s=$1 standin
	standin=$(field standin "$s")
	[ -n "$standin" ] && [ "$standin" != 00000000 ] && [ "$(field dressed "$s")" = 1 ] && [[ ",$(field equippedIds "$s")," != *",$2,"* ]]
}

# ---- the VM's files -----------------------------------------------------------------------------------------------
# Two files the VM's game reads its load order from, by short name: ccc (Skyrim.ccc in its game folder) and plugins
# (plugins.txt in the player's AppData). Backed up in $VM_DIR/forms-backup with hashes.txt ("<name> <SHA-256>" per file),
# written only once each copy is whole, so a run that dies at any point leaves either nothing changed or a full backup.

# forms_ps <PowerShell assignments> <PowerShell body>: runs on the VM (-EncodedCommand: no quoting between bash, ssh
# and PowerShell), with $ccc, $plugins, $bk, PathOf and Sha defined; its output
forms_ps() {
	local script
	script="\$ProgressPreference = 'SilentlyContinue'
\$ccc = '$VM_GAME/Skyrim.ccc'
\$plugins = Join-Path \$env:LOCALAPPDATA 'Skyrim Special Edition\\plugins.txt'
\$bk = '$VM_DIR/forms-backup'
function PathOf(\$n) { if (\$n -eq 'ccc') { \$ccc } else { \$plugins } }
function Sha(\$p) { if (Test-Path -LiteralPath \$p) { (Get-FileHash -Algorithm SHA256 -LiteralPath \$p).Hash } else { 'missing' } }
$1
$2"
	vmssh "powershell -NoProfile -NonInteractive -EncodedCommand $(printf '%s' "$script" | iconv -f UTF-8 -t UTF-16LE | base64 -w0)"
}
# forms_vm_read <name> <local file>: the VM's file copied here; 1 when it is not there or can't be read
forms_vm_read() {
	local b64
	b64=$(forms_ps "\$n = '$1'" '$p = PathOf $n; if (Test-Path -LiteralPath $p) { "b64:" + [Convert]::ToBase64String([IO.File]::ReadAllBytes($p)) } else { "missing" }' | tr -d '\r\n ')
	[[ $b64 == b64:* ]] || return 1
	printf '%s' "${b64#b64:}" | base64 -d > "$2"
}
# forms_vm_write <name> <local file>: the file's bytes become the VM's (copied over with scp: a load order can be
# longer than a command line); prints the VM's SHA-256 of it after
forms_vm_write() {
	vmscp "$2" "$VM_SSH:$VM_DIR/forms-new-$1" || return 1
	forms_ps "\$n = '$1'" '$new = Join-Path (Split-Path $bk) "forms-new-$n"; Copy-Item -Force -LiteralPath $new (PathOf $n); Remove-Item -Force -LiteralPath $new; Sha (PathOf $n)' | tr -d '\r\n '
}
# forms_vm_sha <name>: the VM's file's SHA-256 (missing when it is not there)
forms_vm_sha() { forms_ps "\$n = '$1'" 'Sha (PathOf $n)' | tr -d '\r\n '; }
forms_sha() { sha256sum "$1" | cut -d' ' -f1 | tr 'a-f' 'A-F'; }
# forms_vm_backup <name...>: each file copied into the VM's backup folder, its hash recorded once the copy matches it.
# Prints ok, exists (a backup is already there: never written over), or bad <name>.
forms_vm_backup() {
	local names; names=$(printf "'%s'," "$@"); names=${names%,}
	forms_ps "\$names = @($names)" '
if (Test-Path -LiteralPath $bk) { "exists"; return }
New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($n in $names) {
	$copy = Join-Path $bk $n
	Copy-Item -Force -LiteralPath (PathOf $n) $copy
	$h = Sha $copy
	if ($h -ne (Sha (PathOf $n))) { "bad $n"; return }
	Add-Content -LiteralPath (Join-Path $bk "hashes.txt") "$n $h"
}
"ok"' | tr -d '\r'
}
# forms_vm_heal: the VM's backup put back, each file's hash compared, the backup folder removed once all match. Prints
# none (no backup: nothing was changed), restored <names>, bad-backup <name> or bad-copy <name> (nothing removed then).
forms_vm_heal() {
	forms_ps "" '
$list = Join-Path $bk "hashes.txt"
if (-not (Test-Path -LiteralPath $list)) {
	# a backup that died before its first hash was written: nothing was changed yet
	if (Test-Path -LiteralPath $bk) { Remove-Item -Recurse -Force -LiteralPath $bk }
	"none"; return
}
$done = @()
foreach ($line in Get-Content -LiteralPath $list) {
	$n, $want = $line -split " ", 2
	$copy = Join-Path $bk $n
	if ((Sha $copy) -ne $want) { "bad-backup $n"; return }
	if ((Sha (PathOf $n)) -ne $want) { Copy-Item -Force -LiteralPath $copy (PathOf $n) }
	if ((Sha (PathOf $n)) -ne $want) { "bad-copy $n"; return }
	$done += $n
}
Remove-Item -Recurse -Force -LiteralPath $bk
"restored " + ($done -join ",")' | tr -d '\r' | tail -1
}

# ---- the step -----------------------------------------------------------------------------------------------------

# forms_heal: smoke.sh's set-up, after its backup and before the games start. The VM's game is closed first when the
# backup is there (it read the changed order when it started), and started again by the set-up.
forms_heal() {
	local r
	r=$(forms_ps "" 'if (Test-Path -LiteralPath $bk) { "backup" } else { "none" }' | tr -d '\r\n ')
	case "$r" in
	none) return 0 ;;
	backup) ;;
	*) warn "set-up: the VM's load order is as the forms step found it" "no answer over ssh ('$r'): whether a forms step left it changed can't be told"; return 0 ;;
	esac
	running_vm "$GAME_EXE" && bash "$RIG/start.sh" stop --vm-only >/dev/null 2>&1
	r=$(forms_vm_heal)
	case "$r" in
	"restored "*) healed "set-up: the VM's load order is as the forms step found it" "a forms step that did not finish left ${r#restored } changed: put back from $VM_DIR/forms-backup, SHA-256 compared" ;;
	none) ;;
	*) _rig_stop "the VM's load order is changed" "a forms step that did not finish left the VM's files changed, and its backup could not put them back ($r): see $VM_DIR/forms-backup on the VM" ;;
	esac
}

# forms_gear <wearer> <watcher>: the wearer puts on the first wearable armor of FORMS_ARMOR_PLUGIN; the watcher's
# stand-in of them must have the watcher's own form of it on, then lose it when it comes off. Checks made here.
forms_gear() {
	local wearer=$1 watcher=$2 reply id localid theirs n0 label why
	reply=$(dc "$wearer" "forms armor $FORMS_ARMOR_PLUGIN")
	id=$(forms_id "$reply"); localid=$(forms_local "$reply")
	label="$wearer puts on $FORMS_ARMOR_PLUGIN's armor ${localid:-?}"
	[ -n "$id" ] || { rig_fault state "$label" "the $wearer's game names no wearable armor of $FORMS_ARMOR_PLUGIN ($(cut -c1-120 <<<"$reply"))"; return; }
	theirs=$(forms_id "$(dc "$watcher" "forms find $FORMS_ARMOR_PLUGIN $localid")")
	[ -n "$theirs" ] || { fail "$label: the $watcher's game finds it" "no form $localid of $FORMS_ARMOR_PLUGIN in the $watcher's game"; return; }
	[ "$id" != "$theirs" ] && pass "$label: the two games number it differently" "$wearer $id, $watcher $theirs" || warn "$label: the two games number it differently" "both $id: the swap did not reach this plugin, so this check is like any other gear"
	n0=$(field looksSent "$(dc "$wearer" 'session status')")
	dc "$wearer" "console player.additem $id 1" >/dev/null
	dc "$wearer" "console player.equipitem $id" >/dev/null
	if ! wait_until 10 _forms_looks_grew "$wearer" "$n0"; then
		rig_fault input "$label" "the $wearer's game sent no new looks in 10 s (looksSent ${n0:-?}): the equip did not reach it"
		_forms_take_off "$wearer" "$id"; return
	fi
	# the looks change takes the watcher's stand-in away and places a new one: the wait covers both
	if wait_until "$FORMS_WAIT" _forms_on "$watcher" "$theirs"; then
		pass "$label: the $watcher's stand-in has it on" "as $theirs (the $wearer's $id), $(dc "$watcher" standins | grep -o 'standin=[0-9A-F]*' | head -1)"
	else
		why=$(forms_standin_why "$(dc "$watcher" standins)" "$theirs")
		fail "$label: the $watcher's stand-in has it on" "after $FORMS_WAIT s: $why"
	fi
	n0=$(field looksSent "$(dc "$wearer" 'session status')")
	_forms_take_off "$wearer" "$id"
	if ! wait_until 10 _forms_looks_grew "$wearer" "$n0"; then rig_fault input "$label: taken off" "the $wearer's game sent no new looks in 10 s: the unequip did not reach it"; return; fi
	if wait_until "$FORMS_WAIT" _forms_off "$watcher" "$theirs"; then pass "$label: taken off, it leaves the $watcher's stand-in" "$(dc "$watcher" standins | grep -o 'equippedIds=[^ ]*' | head -1)"
	else fail "$label: taken off, it leaves the $watcher's stand-in" "after $FORMS_WAIT s: $(dc "$watcher" standins | grep -o 'standin=[0-9A-F]*\|dressed=[0-9]\|equippedIds=[^ ]*' | tr '\n' ' ')"; fi
}
_forms_looks_grew() { local n; n=$(field looksSent "$(dc "$1" 'session status')"); is_num "$n" && is_num "$2" && [ "$n" -gt "$2" ]; }
_forms_on() { forms_standin_why "$(dc "$1" standins)" "$2" >/dev/null; }
_forms_off() { forms_standin_lacks "$(dc "$1" standins)" "$2"; }
_forms_take_off() { dc "$1" "console player.unequipitem $2" >/dev/null; dc "$1" "console player.removeitem $2 1" >/dev/null; }

# forms_vm_back_in: the VM's game started in FORMS_CELL and in the host's session again; prints why not
forms_vm_back_in() {
	local r
	bash "$RIG/start.sh" --vm-only --cell "$(forms_cell)" > "$A_OUT/forms-vm-start.log" 2>&1 || { echo "start.sh --vm-only failed: $(tail -2 "$A_OUT/forms-vm-start.log" | tr '\n' ' ' | cut -c1-160)"; return 1; }
	vm_token_fetch && tunnel || { echo "the VM's dev channel can't be reached after its start"; return 1; }
	r=$(vm_join) || { echo "$r"; return 1; }
	wait_until 90 joined_both || { echo "not in the host's session 90 s after joining: $(dc vm 'session status' | cut -c1-120)"; return 1; }
}

step_forms() {
	section "forms: the VM's plugins in another order; gear crosses both ways as each game's own form"
	local hostA hostB vmA vmB names n swapped r out w
	lock_keepalive
	r=$(forms_ps "" 'if (Test-Path -LiteralPath $bk) { "backup" } else { "none" }' | tr -d '\r\n ')
	[ "$r" = none ] || { rig_unhealed state "forms: no backup of an earlier forms step on the VM" "$VM_DIR/forms-backup is there ('$r'): the set-up should have put it back"; return; }
	hostA=$(forms_place "$(dc host "forms place $FORMS_A")"); hostB=$(forms_place "$(dc host "forms place $FORMS_B")")
	vmA=$(forms_place "$(dc vm "forms place $FORMS_A")"); vmB=$(forms_place "$(dc vm "forms place $FORMS_B")")
	if [[ $hostA != full:* || $hostB != full:* || $vmA != "$hostA" || $vmB != "$hostB" ]]; then
		rig_unhealed state "forms: both games load $FORMS_A and $FORMS_B as full plugins in the same places" "host $hostA/$hostB, vm $vmA/$vmB"
		return
	fi

	# 1. the VM's files: read, backed up on the VM and here, the two swapped in each that lists both
	FORMS_STATE=$A_OUT/forms-backup; mkdir -p "$FORMS_STATE"
	forms_vm_read ccc "$FORMS_STATE/ccc" && [ -s "$FORMS_STATE/ccc" ] || { rig_fault state "forms: the VM's Skyrim.ccc is read" "it could not be read over ssh"; FORMS_STATE=""; return; }
	forms_lists "$FORMS_STATE/ccc" "$FORMS_A" && forms_lists "$FORMS_STATE/ccc" "$FORMS_B" || { rig_unhealed state "forms: the VM's Skyrim.ccc lists both" "it lacks $FORMS_A or $FORMS_B"; FORMS_STATE=""; return; }
	names="ccc"
	if forms_vm_read plugins "$FORMS_STATE/plugins"; then
		if forms_lists "$FORMS_STATE/plugins" "$FORMS_A" && forms_lists "$FORMS_STATE/plugins" "$FORMS_B"; then names="ccc plugins"
		elif forms_lists "$FORMS_STATE/plugins" "$FORMS_A" || forms_lists "$FORMS_STATE/plugins" "$FORMS_B"; then
			warn "forms: the VM's plugins.txt names both or neither" "it names one of the two: only Skyrim.ccc is swapped"
		fi
	fi
	r=$(forms_vm_backup $names)
	[ "$r" = ok ] || { rig_unhealed state "forms: the VM's files are backed up" "$r"; [ "$r" = exists ] || forms_vm_heal >/dev/null; FORMS_STATE=""; return; }
	pass "forms: the VM's files are backed up" "$names in $VM_DIR/forms-backup with their SHA-256, and in $FORMS_STATE"
	dc vm 'session leave' >/dev/null
	bash "$RIG/start.sh" stop --vm-only >/dev/null 2>&1
	for n in $names; do
		swapped=$FORMS_STATE/$n.swapped
		forms_swap "$FORMS_STATE/$n" "$FORMS_A" "$FORMS_B" > "$swapped" || { rig_fault state "forms: the swap" "$n does not list both"; forms_restore rejoin; return; }
		r=$(forms_vm_write "$n" "$swapped")
		[ "$r" = "$(forms_sha "$swapped")" ] || { rig_broken ssh "forms: the VM's $n is written" "its SHA-256 after the write is '$r'"; forms_restore rejoin; return; }
	done
	say "forms: the VM's $names list $FORMS_A and $FORMS_B in each other's place; starting its game in $(forms_cell)"
	if ! r=$(forms_vm_back_in); then
		# the join is the mod's; a game that does not start is the rig's
		case "$r" in "start.sh"*|*"dev channel"*) rig_broken game "forms: the VM's game with the two swapped starts" "$r" ;; *) fail "forms: the VM with its plugins in another order is let into the session" "$r" ;; esac
		forms_restore rejoin; return
	fi

	# 2. the orders differ, the VM is in, and translates
	vmA=$(forms_place "$(dc vm "forms place $FORMS_A")"); vmB=$(forms_place "$(dc vm "forms place $FORMS_B")")
	if [ "$vmA" != "$hostB" ] || [ "$vmB" != "$hostA" ]; then
		rig_fault state "forms: the VM's game loads the two in each other's place" "$FORMS_A at $vmA, $FORMS_B at $vmB (host $hostA, $hostB) with $names swapped: another file decides the order"
		forms_restore rejoin; return
	fi
	pass "forms: the VM's game loads the two in each other's place" "$FORMS_A at $vmA, $FORMS_B at $vmB; the host's at $hostA, $hostB"
	pass "forms: the VM with its plugins in another order is let into the session" "$(dc vm 'session status' | cut -c1-80)"
	out=$(dc vm forms)
	expect_eq "forms: vm: translates by plugin name (identity)" "$(field identity "$out")" 0
	expect_zero "forms: vm: none of its plugins lacking from the session's table" "$(field oursOnly "$out")"
	expect_zero "forms: vm: none of the session's plugins lacking from its game" "$(field theirsOnly "$out")"
	for w in host vm; do wait_until 30 _forms_placed "$w" || fail "forms: $w: a stand-in of its friend is in the world" "$(dc "$w" standins | cut -c1-160)"; done

	# 3. gear both ways, each a form of a plugin the two games number differently
	forms_gear vm host
	forms_gear host vm

	# 4. nothing skipped: both games have every plugin
	for w in host vm; do expect_zero "forms: $w: no form skipped" "$(field skipped "$(dc "$w" forms)")"; done

	# 5. the VM as it was
	forms_restore rejoin
}
_forms_placed() { local s; s=$(field standin "$(dc "$1" standins)"); [ -n "$s" ] && [ "$s" != 00000000 ]; }

# forms_restore [rejoin]: the VM's files put back byte for byte from its backup (this run's copies if that fails) when
# a forms step changed them; with rejoin (the step's own end) its game is started again, joined again and its places
# checked to be the host's. Without (smoke.sh's restore), the VM's game is left closed, as the restore closes it anyway.
forms_restore() {
	local r n ok=1 want hostA vmA bad=fail
	r=$(forms_ps "" 'if (Test-Path -LiteralPath $bk) { "backup" } else { "none" }' | tr -d '\r\n ')
	[ -z "$FORMS_STATE" ] && [ "$r" != backup ] && return 0
	# in the step a broken rig counts towards the breaker; in smoke.sh's restore it must not stop the rest of the restore
	if [ "${1:-}" = rejoin ]; then bad=rig_broken; section "forms: the VM's load order put back"
	else [ -n "${keep:-}" ] && say "forms: the VM's game is closed to put its load order back (--keep left it running)"; fi
	dc vm 'session leave' >/dev/null
	bash "$RIG/start.sh" stop --vm-only >/dev/null 2>&1
	r=$(forms_vm_heal)
	case "$r" in "restored "*|none) ;; *) ok=0 ;; esac
	# each file this run changed is checked against this run's own copy, and put back from it if the VM's backup did not
	if [ -n "$FORMS_STATE" ]; then
		ok=1
		for n in ccc plugins; do
			[ -s "$FORMS_STATE/$n.swapped" ] || continue
			want=$(forms_sha "$FORMS_STATE/$n")
			[ "$(forms_vm_sha "$n")" = "$want" ] || [ "$(forms_vm_write "$n" "$FORMS_STATE/$n")" = "$want" ] || ok=0
		done
	fi
	if [ $ok = 1 ]; then
		pass "forms: the VM's files are as before the step" "${r:-put back} (SHA-256 compared)"
		[[ $r == "restored "* ]] || forms_ps "" 'if (Test-Path -LiteralPath $bk) { Remove-Item -Recurse -Force -LiteralPath $bk }' >/dev/null
		FORMS_STATE=""
	else
		[ $bad = rig_broken ] && set -- ssh || set --
		$bad "$@" "forms: the VM's files are as before the step" "NOT put back ($r): copy them from $VM_DIR/forms-backup on the VM (or ${FORMS_STATE:-the run folder}) by hand; the next run's set-up tries again"
		return 1
	fi
	[ $bad = rig_broken ] || return 0
	if r=$(forms_vm_back_in); then pass "forms: the VM's game is back in the session after the restore" ""; else rig_broken game "forms: the VM's game is back in the session after the restore" "$r"; return 1; fi
	hostA=$(forms_place "$(dc host "forms place $FORMS_A")"); vmA=$(forms_place "$(dc vm "forms place $FORMS_A")")
	expect_eq "forms: after the restore the VM loads $FORMS_A where the host does" "$vmA" "$hostA"
}

# ---- self-test: the reading and the file logic, both games faked --------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init formstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	printf 'Skyrim.esm\r\nccBGSSSE001-Fish.esm\r\nccQDRSSE001-SurvivalMode.esl\r\nccBGSSSE025-AdvDSGS.esm\r\n' > "$d/crlf"
	forms_swap "$d/crlf" ccbgssse001-fish.ESM ccBGSSSE025-AdvDSGS.esm > "$d/out"
	printf 'Skyrim.esm\r\nccBGSSSE025-AdvDSGS.esm\r\nccQDRSSE001-SurvivalMode.esl\r\nccBGSSSE001-Fish.esm\r\n' > "$d/want"
	t "swap keeps CRLF lines and every other byte" "$(cmp -s "$d/out" "$d/want" && echo same)" same
	forms_swap "$d/out" ccBGSSSE001-Fish.esm ccBGSSSE025-AdvDSGS.esm > "$d/back"
	t "swapping again gives the file back" "$(cmp -s "$d/back" "$d/crlf" && echo same)" same
	printf 'A.esm\nB.esm\nC.esm' > "$d/lf"
	forms_swap "$d/lf" A.esm C.esm > "$d/out2"
	t "swap keeps LF lines and no newline at the end" "$(od -An -c "$d/out2" | tr -d ' \n')" 'C.esm\nB.esm\nA.esm'
	forms_swap "$d/lf" A.esm Missing.esm > /dev/null; t "a missing plugin is an error" $? 1
	# plugins.txt: a comment, an active (*) and an inactive line; each place keeps its own mark and its own end
	printf '# This file is used by Skyrim to keep track of your downloaded content.\r\n*ccBGSSSE001-Fish.esm\r\nMine.esp\r\nccBGSSSE025-AdvDSGS.esm\r\n' > "$d/plugins"
	forms_swap "$d/plugins" ccBGSSSE001-Fish.esm ccBGSSSE025-AdvDSGS.esm > "$d/pout"
	printf '# This file is used by Skyrim to keep track of your downloaded content.\r\n*ccBGSSSE025-AdvDSGS.esm\r\nMine.esp\r\nccBGSSSE001-Fish.esm\r\n' > "$d/pwant"
	t "plugins.txt: the names swap places, each place keeps its * mark" "$(cmp -s "$d/pout" "$d/pwant" && echo same)" same
	t "lists: a * line counts" "$(forms_lists "$d/plugins" ccbgssse001-fish.esm && echo yes)" yes
	t "lists: a name not there" "$(forms_lists "$d/plugins" Other.esp || echo no)" no
	t "place" "$(forms_place 'place=full:6 plugin=ccBGSSSE001-Fish.esm')" full:6
	t "place none" "$(forms_place 'place=none plugin=X.esp')" none
	t "id" "$(forms_id 'id=06000D62 plugin=ccBGSSSE025-AdvDSGS.esm local=D62 session=05000D62 back=06000D62 armor=1')" 06000D62
	t "id none" "$(forms_id 'id=none plugin=X.esp local=801')" ""
	t "id error" "$(forms_id 'error: usage: forms')" ""
	t "local" "$(forms_local 'id=06000D62 plugin=P.esm local=D62 session=05000D62')" D62

	# the stand-in check reads the actor's equippedIds, and needs a stand-in in the world, dressed
	S='players=1 standins=1 leaving=0 | player 2 standin=FF0008AE placed=1 dressed=1 at=(1,2,3) heading=0.000 named=friend aimed=0 race=00013746 worn=3/3 wornIds=00012E46,05000D62,0001BE1A equippedIds=00012E46,05000D62,0001BE1A'
	t "wears: on the actor" "$(forms_standin_why "$S" 05000D62 && echo yes)" yes
	t "wears: a whole id only" "$(forms_standin_why "${S//05000D62/105000D62}" 05000D62 >/dev/null || echo no)" no
	t "wears: named by the looks but not on the actor (Dress dropped it)" "$(forms_standin_why "${S/equippedIds=00012E46,05000D62,0001BE1A/equippedIds=00012E46,0001BE1A}" 05000D62 | grep -c 'has on 00012E46,0001BE1A, not 05000D62')" 1
	t "wears: no stand-in (another cell, or between two)" "$(forms_standin_why "${S/standin=FF0008AE/standin=00000000}" 05000D62 | grep -c 'no stand-in')" 1
	t "wears: not dressed yet" "$(forms_standin_why "${S/dressed=1/dressed=0}" 05000D62 | grep -c 'not dressed')" 1
	t "lacks: off the actor" "$(forms_standin_lacks "${S/equippedIds=00012E46,05000D62,0001BE1A/equippedIds=none}" 05000D62 && echo yes)" yes
	t "lacks: still on" "$(forms_standin_lacks "$S" 05000D62 || echo no)" no
	t "lacks: no stand-in is not 'off'" "$(forms_standin_lacks "${S/standin=FF0008AE/standin=00000000}" 05000D62 || echo no)" no

	# forms_gear with both games faked: the wearer's looks go out, the watcher's stand-in shows the armor, then not
	gear() {  # gear <standins with it on> <standins with it off>: both games' replies; the results' verdicts
		FG_ON=$1; FG_OFF=$2; FG=$(mktemp); echo 0 > "$FG"   # a file: each reply is read in a subshell
		dc() { local n; n=$(cat "$FG"); case "$2" in
			"forms armor"*) echo "id=06000D62 plugin=$FORMS_B local=D62 session=05000D62 back=06000D62 armor=1" ;;
			"forms find"*) echo "id=05000D62 plugin=$FORMS_B local=D62" ;;
			'session status') echo "role=x connection=up welcomed=1 looksSent=$n" ;;
			"console player.equipitem"*|"console player.unequipitem"*) echo $((n + 1)) > "$FG"; echo ran ;;
			standins) if [ $((n % 2)) = 1 ]; then echo "$FG_ON"; else echo "$FG_OFF"; fi ;;
			*) echo ran ;; esac; }
		( A_OUT=$d/gear$RANDOM; a_init gear >/dev/null; forms_gear vm host >/dev/null; cut -f1 "$A_RESULTS" | tr '\n' ' ' )
	}
	OFF=${S/equippedIds=00012E46,05000D62,0001BE1A/equippedIds=00012E46}
	t "gear: on, then off, passes" "$(gear "$S" "$OFF")" "PASS PASS PASS "
	t "gear: the looks named it but the actor has it not: FAIL" "$(gear "$OFF" "$OFF" | cut -d' ' -f1-2)" "PASS FAIL"
	t "gear: no stand-in placed: FAIL" "$(gear "${S/standin=FF0008AE/standin=00000000}" "$OFF" | cut -d' ' -f1-2)" "PASS FAIL"
	t "gear: still on after it came off: FAIL" "$(gear "$S" "$S")" "PASS PASS FAIL "

	# the step is wired into the harness: in STEPS before final, a session step, not run unless named
	t "forms is a step, before final" "$(case " $STEPS " in *" forms "*" final "*) echo yes;; esac)" yes
	t "forms needs a session" "$(case " $SESSION_STEPS " in *" forms "*) echo yes;; esac)" yes
	t "forms runs only when named" "$(case " $OPTIN_STEPS " in *" forms "*) echo yes;; esac)" yes
	t "the VM restarts in the harness's cell" "$(forms_cell)" "$SMOKE_CELL"
	[ $bad -eq 0 ] && echo "forms.sh selftest: all ok" || echo "forms.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
