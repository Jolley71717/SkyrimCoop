#!/bin/bash
# steps/forms.sh: the rig step `forms` (protocol 4, docs/DESIGN.md "Form ids across load orders"). Two games whose
# plugins load in another order play together, and a form one names reaches the other as that game's own form, both
# ways. Not run in a game yet.
#
#   bash tools/rig/steps/forms.sh selftest      the step's own logic, no game and no rig (CI's Linux job can run it)
#   step_forms                                  sourced by the functional harness (tools/rig/smoke.sh), after its
#                                               join step: both games in the same cell, the host hosting, the VM in
#   forms_restore                               the harness's restore (and its trap) calls it: puts the VM's Skyrim.ccc
#                                               back byte for byte if this step changed it; harmless otherwise
#
# What it does (hold the rig; the games run as the harness started them):
#   1. Both games load FORMS_A and FORMS_B (two full Creation Club masters every Anniversary Edition game has) in the
#      same places. The VM's game is stopped, the two swapped in its Skyrim.ccc (the Creation Club's load order; a copy
#      is kept on this PC and on the VM first), and it is started again in FORMS_CELL (console placement, as start.sh
#      does) and joins the host's session again.
#   2. The VM's game now loads them in each other's place (else RIGFAULT: the rig could not make the orders differ).
#      Its `forms` line: translated (identity=0), nothing lacking either way, nothing skipped yet.
#   3. Gear both ways: the VM's player puts on the first armor of FORMS_ARMOR_PLUGIN a player can wear (console
#      additem + equipitem: setup, not movement); the host's stand-in of them must wear that armor as the host's game
#      numbers it (another form ID than the VM's, found on each side by the plugin's file name and the form's number).
#      Then the host does the same and the VM's stand-in is checked. The items are taken off and removed after.
#   4. Neither game logs a skipped plugin, and neither crashes (the harness's final step reads the crash logs).
#   5. Restore: the VM's game stopped, its Skyrim.ccc put back (hash compared), started again and joined again, and its
#      places checked to be the host's again, so the steps after this one see the rig as before.
#
# Verdict words come from the harness (assert.sh): pass, fail, warn, rig_fault. Run alone (no harness), this file
# prints them itself.
#
# The extra-plugin case (a plugin only one game has) is not done here: it needs a plugin installed in one game only, and
# mods go on the rig only through install.sh from the maintainer's downloads. The unit tests cover it
# (tests/FormTranslateTests.cpp, tests/ServerTests.cpp).

FORMS_A=${FORMS_A:-ccBGSSSE001-Fish.esm}
FORMS_B=${FORMS_B:-ccBGSSSE025-AdvDSGS.esm}
FORMS_ARMOR_PLUGIN=${FORMS_ARMOR_PLUGIN:-$FORMS_B}
FORMS_CELL=${FORMS_CELL:-Riverwood}
FORMS_WAIT=${FORMS_WAIT:-20}        # seconds for gear to show on a stand-in
FORMS_JOIN_WAIT=${FORMS_JOIN_WAIT:-120}

# ---- the logic, free of the rig (selftest) ------------------------------------------------------------------------

# forms_swap <file> <a> <b>: the file with the lines naming plugins a and b (any case, a CR at a line's end kept)
# swapped, every other byte as it was; to stdout. 1 when either line is missing.
forms_swap() {
	local file=$1 a=$2 b=$3 out
	# BINMODE=3: Windows' gawk (Git Bash, where the rig runs) drops each CR otherwise; elsewhere it changes nothing
	out=$(awk -v BINMODE=3 -v a="$a" -v b="$b" '
		{ line[NR] = $0; name = tolower($0); sub(/\r$/, "", name); if (name == tolower(a)) ia = NR; if (name == tolower(b)) ib = NR }
		END {
			if (!ia || !ib) exit 1
			t = line[ia]; line[ia] = line[ib]; line[ib] = t
			# the two lines keep their own ends: a CR moves with its line only if both had one or neither did
			ca = sub(/\r$/, "", line[ia]); cb = sub(/\r$/, "", line[ib])
			if (cb) line[ia] = line[ia] "\r"; if (ca) line[ib] = line[ib] "\r"
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

# forms_place <reply of `forms place`>: full:N, light:N or none
forms_place() { sed -n 's/^place=\([^ ]*\).*/\1/p' <<<"$1"; }

# forms_id <reply of `forms find|armor|id`>: the 8-hex-digit id, or "" (none, or an error)
forms_id() { sed -n 's/^id=\([0-9A-F]\{8\}\) .*/\1/p' <<<"$1"; }

# forms_local <reply of `forms armor|id`>: the form's number within its plugin, hex
forms_local() { tr ' ' '\n' <<<"$1" | sed -n 's/^local=\([0-9A-F]*\)$/\1/p' | head -1; }

# forms_wears <reply of `standins`> <id>: 0 when the first stand-in's worn items hold that id
forms_wears() { local ids; ids=$(tr ' ' '\n' <<<"$1" | sed -n 's/^wornIds=//p' | head -1); [[ ",$ids," == *",$2,"* ]]; }

forms_selftest() {
	local dir bad=0 got
	dir=$(mktemp -d) || return 1
	check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2', want '$3'"; bad=1; fi; }
	printf 'Skyrim.esm\r\nccBGSSSE001-Fish.esm\r\nccQDRSSE001-SurvivalMode.esl\r\nccBGSSSE025-AdvDSGS.esm\r\n' > "$dir/crlf"
	forms_swap "$dir/crlf" ccbgssse001-fish.ESM ccBGSSSE025-AdvDSGS.esm > "$dir/out"
	printf 'Skyrim.esm\r\nccBGSSSE025-AdvDSGS.esm\r\nccQDRSSE001-SurvivalMode.esl\r\nccBGSSSE001-Fish.esm\r\n' > "$dir/want"
	check "swap keeps CRLF lines and every other byte" "$(cmp -s "$dir/out" "$dir/want" && echo same)" same
	forms_swap "$dir/out" ccBGSSSE001-Fish.esm ccBGSSSE025-AdvDSGS.esm > "$dir/back"
	check "swapping again gives the file back" "$(cmp -s "$dir/back" "$dir/crlf" && echo same)" same
	printf 'A.esm\nB.esm\nC.esm' > "$dir/lf"
	forms_swap "$dir/lf" A.esm C.esm > "$dir/out2"
	check "swap keeps LF lines and no newline at the end" "$(od -An -c "$dir/out2" | tr -d ' \n')" 'C.esm\nB.esm\nA.esm'
	forms_swap "$dir/lf" A.esm Missing.esm > /dev/null; check "a missing plugin is an error" $? 1
	check "place" "$(forms_place 'place=full:6 plugin=ccBGSSSE001-Fish.esm')" full:6
	check "place none" "$(forms_place 'place=none plugin=X.esp')" none
	check "id" "$(forms_id 'id=06000D62 plugin=ccBGSSSE025-AdvDSGS.esm local=D62 session=05000D62 back=06000D62 armor=1')" 06000D62
	check "id none" "$(forms_id 'id=none plugin=X.esp local=801')" ""
	check "id error" "$(forms_id 'error: usage: forms')" ""
	check "local" "$(forms_local 'id=06000D62 plugin=P.esm local=D62 session=05000D62')" D62
	got=$(forms_wears 'players=1 standins=1 | player 2 standin=FF000801 named=friend aimed=0 race=00013746 worn=3/3 wornIds=00012E46,05000D62,0001BE1A' 05000D62 && echo yes)
	check "wears" "$got" yes
	got=$(forms_wears 'players=1 | player 2 worn=0/0 wornIds=none' 05000D62 || echo no)
	check "wears not" "$got" no
	got=$(forms_wears 'player 2 wornIds=105000D62' 05000D62 || echo no)
	check "wears a whole id only" "$got" no
	rm -rf "$dir"
	return $bad
}

# ---- the step (the rig) -------------------------------------------------------------------------------------------

FORMS_STATE=${FORMS_STATE:-}  # this run's copy of the VM's Skyrim.ccc, while the VM's is changed

# forms_ccc_read: the VM's Skyrim.ccc as base64 (one line), from its game folder
forms_ccc_read() { vmssh "[Convert]::ToBase64String([IO.File]::ReadAllBytes('$VM_GAME/Skyrim.ccc'))" | tr -d '\r\n '; }
# forms_ccc_write <file>: the file's bytes become the VM's Skyrim.ccc; prints the VM's SHA-256 of it after
forms_ccc_write() {
	local b64; b64=$(base64 -w0 < "$1")
	vmssh "[IO.File]::WriteAllBytes('$VM_GAME/Skyrim.ccc', [Convert]::FromBase64String('$b64')); (Get-FileHash -Algorithm SHA256 '$VM_GAME/Skyrim.ccc').Hash"
}
forms_sha() { sha256sum "$1" | cut -d' ' -f1 | tr 'a-f' 'A-F'; }

# forms_vm_restart: the VM's game stopped and started again in FORMS_CELL, in the world, its dev channel reachable
forms_vm_restart() {
	vm "session leave" >/dev/null
	bash "$RIG/start.sh" stop --vm-only >/dev/null || return 1
	bash "$RIG/start.sh" --vm-only --cell "$FORMS_CELL" >/dev/null || return 1
	vm_token_fetch && tunnel
}

# forms_vm_join: the VM's game joins the host's session again (the harness's own join when it has one, else by the
# lobby id over the VM's dev channel, as lobbytest.sh --join dev does); 0 once the VM is welcomed
forms_vm_join() {
	local lobby i
	if declare -F rig_join_vm >/dev/null; then
		rig_join_vm || return 1
	else
		lobby=$(field lobby "$(host status)")
		[ -n "$lobby" ] && [ "$lobby" != 0 ] || return 1
		vm "steam join $lobby" >/dev/null
	fi
	for i in $(seq 1 "$FORMS_JOIN_WAIT"); do
		[ "$(field welcomed "$(vm session)")" = 1 ] && return 0
		sleep 1
		[ $((i % 30)) = 0 ] && lock_keepalive
	done
	return 1
}

# forms_gear <wearer: host|vm> <watcher: host|vm>: the wearer puts on the first wearable armor of FORMS_ARMOR_PLUGIN; the
# watcher's stand-in of them must wear the watcher's own form of it. 0 = pass; the reason on stdout otherwise.
forms_gear() {
	local wearer=$1 watcher=$2 reply id localid theirs i seen
	reply=$($wearer "forms armor $FORMS_ARMOR_PLUGIN")
	id=$(forms_id "$reply"); localid=$(forms_local "$reply")
	[ -n "$id" ] || { echo "$wearer's game has no wearable armor of $FORMS_ARMOR_PLUGIN ($reply)"; return 2; }
	theirs=$(forms_id "$($watcher "forms find $FORMS_ARMOR_PLUGIN $localid")")
	[ -n "$theirs" ] || { echo "$watcher's game can't find $FORMS_ARMOR_PLUGIN's form $localid"; return 2; }
	[ "$id" != "$theirs" ] || echo "note: the armor has the same ID in both games ($id); the orders may not differ for it"
	$wearer "console player.additem $id 1" >/dev/null
	$wearer "console player.equipitem $id" >/dev/null
	seen=""
	for i in $(seq 1 "$FORMS_WAIT"); do
		seen=$($watcher standins)
		forms_wears "$seen" "$theirs" && break
		sleep 1
	done
	$wearer "console player.unequipitem $id" >/dev/null
	$wearer "console player.removeitem $id 1" >/dev/null
	forms_wears "$seen" "$theirs" || {
		echo "$watcher's stand-in of $wearer wears $(tr ' ' '\n' <<<"$seen" | sed -n 's/^wornIds=//p' | head -1), not $theirs ($wearer's $id, $FORMS_ARMOR_PLUGIN's $localid)"
		return 1
	}
	echo "$wearer wore $FORMS_ARMOR_PLUGIN's $localid as $id; $watcher's stand-in wears it as $theirs"
}

step_forms() {
	local hostA hostB vmA vmB orig swapped hash why out
	lock_keepalive
	hostA=$(forms_place "$(host "forms place $FORMS_A")"); hostB=$(forms_place "$(host "forms place $FORMS_B")")
	vmA=$(forms_place "$(vm "forms place $FORMS_A")"); vmB=$(forms_place "$(vm "forms place $FORMS_B")")
	if [[ $hostA != full:* || $hostB != full:* || $vmA != "$hostA" || $vmB != "$hostB" ]]; then
		rig_fault "forms: the games don't both load $FORMS_A and $FORMS_B as full plugins in the same places (host $hostA/$hostB, vm $vmA/$vmB)"
		return
	fi

	# 1. the VM's Skyrim.ccc: kept here and on the VM, then the two swapped
	FORMS_STATE="$CHECKS/skyrim-forms-ccc-$(date +%Y%m%d-%H%M%S)"
	mkdir -p "$FORMS_STATE" || { rig_fault "forms: no folder for the backup"; return; }
	orig="$FORMS_STATE/Skyrim.ccc"
	forms_ccc_read | base64 -d > "$orig" 2>/dev/null
	[ -s "$orig" ] || { rig_fault "forms: the VM's Skyrim.ccc could not be read"; FORMS_STATE=""; return; }
	vmssh "Copy-Item -Force '$VM_GAME/Skyrim.ccc' '$VM_DIR/Skyrim.ccc.forms-backup'" >/dev/null
	swapped="$FORMS_STATE/Skyrim.ccc.swapped"
	forms_swap "$orig" "$FORMS_A" "$FORMS_B" > "$swapped" || { rig_fault "forms: the VM's Skyrim.ccc does not list both $FORMS_A and $FORMS_B"; forms_restore; return; }
	vm "session leave" >/dev/null
	bash "$RIG/start.sh" stop --vm-only >/dev/null
	hash=$(forms_ccc_write "$swapped" | tr -d '\r\n ')
	[ "$hash" = "$(forms_sha "$swapped")" ] || { rig_fault "forms: the VM's Skyrim.ccc was not written as meant"; forms_restore; return; }
	say "forms: the VM loads $FORMS_A and $FORMS_B in each other's place now; starting its game in $FORMS_CELL"
	bash "$RIG/start.sh" --vm-only --cell "$FORMS_CELL" >/dev/null && vm_token_fetch && tunnel || { rig_fault "forms: the VM's game did not start again"; forms_restore; return; }

	# 2. the orders differ, and the VM is in the session again
	vmA=$(forms_place "$(vm "forms place $FORMS_A")"); vmB=$(forms_place "$(vm "forms place $FORMS_B")")
	if [ "$vmA" != "$hostB" ] || [ "$vmB" != "$hostA" ]; then
		rig_fault "forms: the VM's game did not take the order from Skyrim.ccc ($FORMS_A $vmA, $FORMS_B $vmB; host $hostA, $hostB)"
		forms_restore; return
	fi
	forms_vm_join || { fail "forms: the VM with its plugins in another order was not let into the session ($(vm session))"; forms_restore; return; }
	pass "forms: a game with $FORMS_A and $FORMS_B in each other's place joins (protocol 4; protocol 3 refused it)"
	out=$(vm forms)
	if [ "$(field identity "$out")" = 0 ] && [ "$(field oursOnly "$out")" = 0 ] && [ "$(field theirsOnly "$out")" = 0 ]; then
		pass "forms: the VM translates by plugin name, nothing lacking either way ($out)"
	else
		fail "forms: the VM's edge is not as expected ($out)"
	fi

	# 3. gear both ways, each a form of a plugin the two games number differently
	why=$(forms_gear vm host); case $? in 0) pass "forms: $why" ;; 1) fail "forms: $why" ;; *) rig_fault "forms: $why" ;; esac
	why=$(forms_gear host vm); case $? in 0) pass "forms: $why" ;; 1) fail "forms: $why" ;; *) rig_fault "forms: $why" ;; esac

	# 4. nothing skipped: both games have both plugins
	for who in host vm; do
		out=$($who forms)
		[ "$(field skipped "$out")" = 0 ] && pass "forms: $who skipped no form ($out)" || fail "forms: $who skipped forms though both games have every plugin ($out)"
	done

	# 5. the VM as it was
	forms_restore
}

# forms_restore: the VM's Skyrim.ccc put back byte for byte and its game in the session again, if this step changed it
forms_restore() {
	[ -n "$FORMS_STATE" ] && [ -s "$FORMS_STATE/Skyrim.ccc" ] || return 0
	local orig="$FORMS_STATE/Skyrim.ccc" hash hostA vmA
	vm "session leave" >/dev/null
	bash "$RIG/start.sh" stop --vm-only >/dev/null
	hash=$(forms_ccc_write "$orig" | tr -d '\r\n ')
	if [ "$hash" != "$(forms_sha "$orig")" ]; then
		# the VM's own copy, as a second way back
		vmssh "Copy-Item -Force '$VM_DIR/Skyrim.ccc.forms-backup' '$VM_GAME/Skyrim.ccc'" >/dev/null
		hash=$(vmssh "(Get-FileHash -Algorithm SHA256 '$VM_GAME/Skyrim.ccc').Hash" | tr -d '\r\n ')
	fi
	if [ "$hash" = "$(forms_sha "$orig")" ]; then
		say "forms: the VM's Skyrim.ccc is back, byte for byte (kept in $FORMS_STATE)"
		vmssh "Remove-Item -Force '$VM_DIR/Skyrim.ccc.forms-backup' -ErrorAction SilentlyContinue" >/dev/null
		FORMS_STATE=""
	else
		rig_fault "forms: the VM's Skyrim.ccc could NOT be put back: copy $orig to the VM's game folder by hand"
		return 1
	fi
	bash "$RIG/start.sh" --vm-only --cell "$FORMS_CELL" >/dev/null && vm_token_fetch && tunnel || { rig_fault "forms: the VM's game did not start again after the restore"; return 1; }
	forms_vm_join || { rig_fault "forms: the VM did not join again after the restore"; return 1; }
	hostA=$(forms_place "$(host "forms place $FORMS_A")"); vmA=$(forms_place "$(vm "forms place $FORMS_A")")
	[ "$vmA" = "$hostA" ] || rig_fault "forms: after the restore the VM loads $FORMS_A at $vmA, the host at $hostA"
}

# Run alone: the selftest, or the step with plain verdict lines (the harness defines its own and only sources this).
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	set -u
	if [ "${1:-}" = selftest ]; then
		forms_selftest; exit $?
	fi
	. "$(dirname "${BASH_SOURCE[0]}")/../env.sh"
	declare -F pass >/dev/null || pass() { say "PASS $*"; }
	declare -F fail >/dev/null || fail() { say "FAIL $*"; }
	declare -F warn >/dev/null || warn() { say "WARN $*"; }
	declare -F rig_fault >/dev/null || rig_fault() { say "RIGFAULT $*"; }
	rig_guard || exit 1
	no_fallout || exit 1
	vm_token_fetch && tunnel || { rig_fault "forms: the VM's dev channel can't be reached"; exit 1; }
	trap forms_restore EXIT
	step_forms
fi
