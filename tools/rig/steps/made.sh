#!/bin/bash
# steps/made.sh: the rig step `made` (protocol 34, docs/DESIGN.md "Run-time NPCs"), sourced by steps.sh. An NPC a game
# makes as it runs that no player commands (a script's PlaceAtMe, a quest's created alias, an encounter) is one NPC in
# every game: the game that made it first runs it and the others show it; one the other game made too, at the same
# spot, shows the first through its own NPC (a twin); a copy kept when its maker leaves; nothing of it in a save. Not run
# in a game yet.
#
#   step_made      the step
#   bash tools/rig/steps/made.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# The console's `placeatme` stands for a script's PlaceAtMe: it makes a run-time NPC nobody commands, as a script does
# (a summon or a mount is commanded; those are the `npcs` and `ride` steps'). MADE_BASE is a plain horse of Skyrim.esm
# (ride.sh's), so nothing placed fights anyone.
#   1. Shared (made_way <maker> <watcher>): the maker's console places one by its player (set-up). Within MADE_WAIT its
#      `npcs made` lists it (ours local=<id> made=<base>) as shared (phase=shared; a twin, with no other game's there, is
#      a FAIL; never answered a FAIL); the watcher hears what made it (theirs <id> spawner=<maker>) and shows a copy of it
#      (`npcs list`: <id>:<maker> role=copy) within MADE_NEAR of where the maker has it. Each way.
#   2. Paired (made_twin <first> <second>): the first's console places one at a load-order NPC both games show (set-up:
#      `<its local id>.placeatme`), then, once it is shared, the second's at the same NPC. Within MADE_WAIT the second's
#      own is a twin of the first's (phase=twin twin=<first's id>@<first>; shared is a FAIL: the NPC is twice in both
#      games), the second's `npcs list` row of the first's key has twin=1 and its own NPC as the local, and the first
#      shows no copy of the second's (a row <second's id>:<second> is a FAIL). No such NPC is a rig fault.
#   3. Saved (made_save): the VM saves while it holds a twin and a friend's copy (never the host): its game lives, its log
#      says it saved, no stray stand-in, and within MADE_WAIT the twin is a twin again (twin=1).
#   4. Kept (made_keep): the VM's one from 1 is shown by the host as a copy; the VM leaves the session: within MADE_WAIT
#      the host keeps it as its own (`npcs made` adoptedIn= up, ours ... adopted=1 for its copy's id); kept as a copy
#      that went is a FAIL. The VM joins again: within MADE_WAIT its own (still in its world) is a twin of the host's kept
#      one (phase=twin twin=<the host's local>@<the host>), so the horse is one NPC again.
#   5. A quest's created alias (made_quest): only when MADE_QUEST names a quest (`npcs made quests` lists those that
#      create an NPC at a fixed reference). Both consoles start it (`startquest <edid>`), the host's first: within
#      MADE_WAIT one game's alias NPC is shared (quest=<id>) and the other's a twin of it. Stopped after (`stopquest`).
#      Not set: a WARN, not tried (a quest's start can move the player or start a scene).
# Put back: every NPC the consoles placed disabled and deleted in the game that has it (its copies go with it: its maker
# says it gone). The quest stopped.

MADE_WAIT=${MADE_WAIT:-15}        # s: for an NPC to be said, answered, shared and copied (ASK_AGAIN 5 s, and more)
MADE_NEAR=${MADE_NEAR:-200}       # u: a copy this near where its maker has it
MADE_BASE=${MADE_BASE:-00068D5C}  # what the consoles place: a plain horse of Skyrim.esm (ride.sh's)
MADE_QUEST=${MADE_QUEST:-}        # a quest's form id (hex) from `npcs made quests` to start in both games; empty: not tried
MADE_PLACED=()                    # "<who> <local id>" of each NPC the consoles placed, for the put-back

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# mr_ours <npcs made reply>: one line per NPC the game made: local phase adopted twin made quest (quest - when none)
mr_ours() {
	awk 'BEGIN { RS = " [|] " }
		$1 == "ours" {
			sub(/\n$/, ""); local = "-"; phase = "-"; adopted = "-"; twin = "-"; made = "-"; quest = "-"
			for (i = 2; i <= NF; i++) {
				split($i, kv, "=")
				if (kv[1] == "local") local = kv[2]; else if (kv[1] == "phase") phase = kv[2]; else if (kv[1] == "adopted") adopted = kv[2]
				else if (kv[1] == "twin") twin = kv[2]; else if (kv[1] == "made") made = kv[2]; else if (kv[1] == "quest") quest = kv[2]
			}
			print local, phase, adopted, twin, made, quest
		}' <<<"$1"
}
# mr_theirs <npcs made reply>: one line per friend's NPC heard of: key (<id>:<spawner>) made quest gone
mr_theirs() {
	awk 'BEGIN { RS = " [|] " }
		$1 == "theirs" {
			sub(/\n$/, ""); spawner = "-"; made = "-"; quest = "-"; gone = 0
			for (i = 3; i <= NF; i++) {
				split($i, kv, "=")
				if (kv[1] == "spawner") spawner = kv[2]; else if (kv[1] == "made") made = kv[2]; else if (kv[1] == "quest") quest = kv[2]; else if (kv[1] == "gone") gone = kv[2]
			}
			print $2 ":" spawner, made, quest, gone
		}' <<<"$1"
}
# mr_col <ours lines> <local id> <column>: that column of that NPC (2 phase, 3 adopted, 4 twin, 5 made, 6 quest)
mr_col() { awk -v k="$2" -v c="$3" '$1 == k { print $c; exit }' <<<"$1"; }
# mr_new <ours lines before> <ours lines after> <made>: the local ids made of that since (one per line)
mr_new() { awk -v m="$3" 'NR == FNR { seen[$1] = 1; next } $5 == m && !seen[$1] { print $1 }' <(echo "$1") <(echo "$2"); }
# mr_of_quest <ours lines> <quest>: local, phase and twin of each NPC that quest's alias made
mr_of_quest() { awk -v q="$2" '$6 == q { print $1, $2, $4 }' <<<"$1"; }
# mr_row <npcs list reply> <key> <field>: that field of that NPC's row (key <id> or <id>:<spawner>); empty when not listed
mr_row() {
	awk -v k="$2" -v f="$3" 'BEGIN { RS = " [|] " }
		NR > 1 {
			sub(/\n$/, ""); key = $1
			for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "spawner") key = key ":" kv[2] }
			if (key != k) next
			for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == f) { print kv[2]; exit } }
			exit
		}' <<<"$1"
}
# mr_at <npcs list reply> <key>: "x y z" of that NPC's actor in that game (empty when none)
mr_at() { local a; a=$(mr_row "$1" "$2" at); [[ $a == \(* ]] || return 0; a=${a#(}; a=${a%)}; tr ',' ' ' <<<"$a"; }
# mr_apart <"x y z"> <"x y z">: u between, rounded; empty when either is missing
mr_apart() {
	[ -n "$1" ] && [ -n "$2" ] || return 0
	awk -v a="$1" -v b="$2" 'BEGIN { split(a, x, " "); split(b, y, " "); printf "%.0f\n", sqrt((x[1] - y[1]) ^ 2 + (x[2] - y[2]) ^ 2 + (x[3] - y[3]) ^ 2) }'
}
# mr_quest_edid <npcs made quests reply> <quest id>: that quest's editor id; empty when not listed
mr_quest_edid() {
	awk -v q="$2" 'BEGIN { RS = " [|] " } { for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] } if (v["quest"] == q) { print v["edid"]; exit } delete v }' <<<"$1"
}
# mr_shared_npc <list A> <list B>: a load-order NPC (no spawner) both games list with an actor, and its local ids in
# each: "key localA localB"; nothing when none
mr_shared_npc() {
	local a b
	a=$(awk 'BEGIN { RS = " [|] " } NR > 1 && $0 !~ / spawner=/ && $0 ~ / at=\(/ && $0 ~ / dead=0/ { for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "local") print $1, kv[2] } }' <<<"$1")
	b=$(awk 'BEGIN { RS = " [|] " } NR > 1 && $0 !~ / spawner=/ && $0 ~ / at=\(/ && $0 ~ / dead=0/ { for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "local") print $1, kv[2] } }' <<<"$2")
	awk 'NR == FNR { l[$1] = $2; next } ($1 in l) { print $1, l[$1], $2; exit }' <(echo "$a") <(echo "$b")
}

# ---- the rig ------------------------------------------------------------------------------------------------------

_mr_ours() { mr_ours "$(dc "$1" 'npcs made')"; }
_mr_pid() { field player "$(dc "$1" 'session status')"; }
# _mr_phase <who> <local> <phase>: that game's NPC is in that phase
_mr_phase() { [ "$(mr_col "$(_mr_ours "$1")" "$2" 2)" = "$3" ]; }
# _mr_answered <who> <local>: that game's NPC is answered (shared or twin)
_mr_answered() { local p; p=$(mr_col "$(_mr_ours "$1")" "$2" 2); [ "$p" = shared ] || [ "$p" = twin ]; }
# _mr_copy <who> <key>: that game shows that NPC as a copy
_mr_copy() { [ "$(mr_row "$(dc "$1" 'npcs list')" "$2" role)" = copy ]; }
# _mr_twin_row <who> <key> <local>: that game shows that NPC through its own (twin=1, local=<its own>)
_mr_twin_row() { local l; l=$(dc "$1" 'npcs list'); [ "$(mr_row "$l" "$2" twin)" = 1 ] && [ "$(mr_row "$l" "$2" local)" = "$3" ]; }
_mr_adopted() { [ "$(mr_col "$(_mr_ours "$1")" "$2" 3)" = 1 ]; }

# _mr_place <who> <at: player or a local id> <label>: the console places MADE_BASE there; prints its local id (the
# one new NPC of that base in `npcs made`; run in a subshell: the caller adds it to MADE_PLACED); nothing (and a rig
# fault) when it made none
_mr_place() {
	local before after r id i
	before=$(_mr_ours "$1")
	r=$(dc "$1" "console $2.placeatme $MADE_BASE 1")
	if [ "$r" != ran ]; then
		rig_fault state "$3: the $1's console places $MADE_BASE (set-up)" "the console refused $2.placeatme $MADE_BASE 1: $r" >&2
		return 1
	fi
	for ((i = 0; i < MADE_WAIT; i++)); do
		after=$(_mr_ours "$1")
		id=$(mr_new "$before" "$after" "$MADE_BASE" | head -1)
		[ -n "$id" ] && break
		sleep 1
	done
	if [ -z "$id" ]; then
		rig_fault state "$3: the NPC the $1's console placed is in its \`npcs made\` (set-up)" "none of $MADE_BASE new after $MADE_WAIT s: $(dc "$1" 'npcs made' | cut -c1-240)" >&2
		return 1
	fi
	echo "$id"
}

# made_way <maker> <watcher>: one placed by the maker's console is shared and copied in the watcher's game
made_way() {
	local m=$1 w=$2 label="made ($1 places, $2 shows)" mid id key apart
	section "made: an NPC the $m's game made as it ran, shown in the $w"
	mid=$(_mr_pid "$m")
	id=$(_mr_place "$m" player "$label") || return 0
	MADE_PLACED+=("$m $id")
	key="$id:$mid"
	say "$label: set-up (console): the $m placed $id ($MADE_BASE) by its player"
	if wait_until "$MADE_WAIT" _mr_answered "$m" "$id" && _mr_phase "$m" "$id" shared; then
		pass "$label: the $m shares it as its own" "$(dc "$m" 'npcs made' | tr '|' '\n' | grep "local=$id" | cut -c1-200)"
	else
		fail "$label: the $m shares it as its own" "phase $(mr_col "$(_mr_ours "$m")" "$id" 2) after $MADE_WAIT s (twin: no other game made one there; asked: never answered)"
		return 0
	fi
	if wait_until "$MADE_WAIT" _mr_copy "$w" "$key"; then
		apart=$(mr_apart "$(mr_at "$(dc "$m" 'npcs list')" "$key")" "$(mr_at "$(dc "$w" 'npcs list')" "$key")")
		if is_num "$apart" && _cmp "$apart" "<=" "$MADE_NEAR"; then
			pass "$label: the $w shows a copy of it where the $m has it" "${apart} u apart; heard: $(mr_theirs "$(dc "$w" 'npcs made')" | grep "^$key " | head -1)"
		else
			fail "$label: the $w shows a copy of it where the $m has it" "apart=${apart:-?} (at most $MADE_NEAR)"
		fi
	else
		fail "$label: the $w shows a copy of it" "no copy of $key after $MADE_WAIT s: $(mr_row "$(dc "$w" 'npcs list')" "$key" role) / heard: $(mr_theirs "$(dc "$w" 'npcs made')" | grep -c "^$key ")"
	fi
}

# made_twin <first> <second>: both games place one at one load-order NPC; the second's shows the first's
made_twin() {
	local a=$1 b=$2 label="made (the $1 then the $2 place one at one spot)" aid bid found key at bat ida idb
	section "made: one NPC placed in both games at one spot is one NPC"
	aid=$(_mr_pid "$a"); bid=$(_mr_pid "$b")
	found=$(mr_shared_npc "$(dc "$a" 'npcs list')" "$(dc "$b" 'npcs list')")
	if [ -z "$found" ]; then
		rig_fault state "$label: a load-order NPC both games show, to place at (set-up)" "none listed with an actor in both"
		return 0
	fi
	read -r key at bat <<<"$found"
	ida=$(_mr_place "$a" "$at" "$label") || return 0
	MADE_PLACED+=("$a $ida")
	say "$label: set-up (console): the $a placed $ida at $key ($at there)"
	if ! wait_until "$MADE_WAIT" _mr_phase "$a" "$ida" shared; then
		fail "$label: the $a's is shared first" "phase $(mr_col "$(_mr_ours "$a")" "$ida" 2) after $MADE_WAIT s"
		return 0
	fi
	idb=$(_mr_place "$b" "$bat" "$label") || return 0
	MADE_PLACED+=("$b $idb")
	say "$label: set-up (console): the $b placed $idb at $key ($bat there)"
	if wait_until "$MADE_WAIT" _mr_answered "$b" "$idb" && _mr_phase "$b" "$idb" twin; then
		local twin; twin=$(mr_col "$(_mr_ours "$b")" "$idb" 4)
		if [ "$twin" = "$ida@$aid" ]; then
			pass "$label: the $b's own is the twin of the $a's" "twin=$twin"
		else
			fail "$label: the $b's own is the twin of the $a's" "twin=$twin, not $ida@$aid"
		fi
	else
		fail "$label: the $b's own is the twin of the $a's" "phase $(mr_col "$(_mr_ours "$b")" "$idb" 2) after $MADE_WAIT s (shared: the horse is there twice in both games)"
		return 0
	fi
	if wait_until "$MADE_WAIT" _mr_twin_row "$b" "$ida:$aid" "$idb"; then
		pass "$label: the $b shows the $a's through its own" "$(dc "$b" 'npcs list' | tr '|' '\n' | grep "^ *$ida spawner=$aid" | grep -o 'local=[^ ]*\|role=[^ ]*\|twin=[^ ]*' | tr '\n' ' ')"
	else
		fail "$label: the $b shows the $a's through its own" "row: $(dc "$b" 'npcs list' | tr '|' '\n' | grep "^ *$ida spawner=$aid" | cut -c1-200)"
	fi
	if [ -z "$(mr_row "$(dc "$a" 'npcs list')" "$idb:$bid" role)" ]; then
		pass "$label: the $a shows no copy of the $b's" ""
	else
		fail "$label: the $a shows no copy of the $b's" "the $a lists $idb:$bid"
	fi
	MADE_TWIN="$ida:$aid $idb"
}

# made_save: the VM saves while it holds a twin or a copy (never the host)
made_save() {
	local label="made: the VM saves holding run-time NPCs of the host's" saves r key own
	section "$label"
	saves=$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { fail "$label: the VM saves" "the console refused: $r"; return 0; }
	sleep 8
	if [ "$(dc vm ping)" != pong ]; then
		fail "$label: the VM's game lives through its save" "its dev channel went silent after the save (a crash? see the crash scan)"
		_rig_stop "the VM's game went down" "after its save holding run-time NPCs"
		return 0
	fi
	pass "$label: the VM's game lives through its save" "its dev channel answers"
	expect_ge "$label: the VM's log says it saved" "$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)" $((saves + 1))
	expect_zero "$label: no stray stand-in after the save (strays=)" "$(field strays "$(dc vm strays)")"
	if [ -n "${MADE_TWIN:-}" ]; then
		read -r key own <<<"$MADE_TWIN"
		if [ "${key#*:}" != "$(_mr_pid vm)" ] && wait_until "$MADE_WAIT" _mr_twin_row vm "$key" "$own"; then
			pass "$label: its twin shows the host's again after the save" "$key through $own"
		elif [ "${key#*:}" != "$(_mr_pid vm)" ]; then
			fail "$label: its twin shows the host's again after the save" "$(dc vm 'npcs list' | tr '|' '\n' | grep "^ *${key%%:*} spawner=" | cut -c1-200)"
		fi
	fi
}

# made_keep <the VM's local id>: the VM leaves; the host keeps its copy of the VM's NPC; joined again, the VM's own is a
# twin of the host's kept one
made_keep() {
	local id=$1 label="made: the VM leaves; the host keeps the VM's NPC" vid hid key a0 hlocal r
	section "$label"
	vid=$(_mr_pid vm); hid=$(_mr_pid host); key="$id:$vid"
	if ! _mr_copy host "$key"; then
		rig_fault state "$label: the host shows the VM's $id as a copy (set-up)" "$(mr_row "$(dc host 'npcs list')" "$key" role)"
		return 0
	fi
	hlocal=$(mr_row "$(dc host 'npcs list')" "$key" local)
	a0=$(field adoptedIn "$(dc host 'npcs made')"); is_num "$a0" || a0=0
	r=$(dc vm 'session leave'); if [[ $r != left* ]]; then fail "$label: the VM leaves" "$r"; return 0; fi
	if wait_until "$MADE_WAIT" _mr_adopted host "$hlocal"; then
		pass "$label: the host keeps it as its own" "adoptedIn $a0 -> $(field adoptedIn "$(dc host 'npcs made')"); $hlocal adopted=1"
	else
		fail "$label: the host keeps it as its own" "after $MADE_WAIT s: $(dc host 'npcs made' | cut -c1-240)"
	fi
	_combat_rejoin_vm "$label" || return 0
	if wait_until "$MADE_WAIT" _mr_phase vm "$id" twin; then
		local twin; twin=$(mr_col "$(_mr_ours vm)" "$id" 4)
		[ "$twin" = "$hlocal@$hid" ] && pass "$label: joined again, the VM's own shows the host's kept one" "twin=$twin" ||
			fail "$label: joined again, the VM's own shows the host's kept one" "twin=$twin, not $hlocal@$hid"
	else
		fail "$label: joined again, the VM's own shows the host's kept one" "phase $(mr_col "$(_mr_ours vm)" "$id" 2) after $MADE_WAIT s (shared: the horse is there twice)"
	fi
	MADE_PLACED+=("host $hlocal")
}

# made_quest: a quest's created alias started in both games is one NPC
made_quest() {
	local label="made: a quest's created alias, started in both games" edid h v hs vs
	section "$label"
	if [ -z "$MADE_QUEST" ]; then
		warn "$label" "not tried: MADE_QUEST names no quest (\`npcs made quests\` lists those that create an NPC at a fixed reference)"
		return 0
	fi
	edid=$(mr_quest_edid "$(dc host 'npcs made quests')" "$MADE_QUEST")
	if [ -z "$edid" ]; then rig_fault state "$label: the quest $MADE_QUEST is one that creates an NPC and is not running (set-up)" "$(dc host 'npcs made quests' | cut -c1-240)"; return 0; fi
	dc host "console startquest $edid" >/dev/null; sleep 3; dc vm "console startquest $edid" >/dev/null
	say "$label: set-up (console): both started $edid ($MADE_QUEST), the host's first"
	for ((i = 0; i < MADE_WAIT; i++)); do
		h=$(mr_of_quest "$(_mr_ours host)" "$MADE_QUEST"); v=$(mr_of_quest "$(_mr_ours vm)" "$MADE_QUEST")
		hs=$(awk '{ print $2 }' <<<"$h" | sort -u | paste -sd,); vs=$(awk '{ print $2 }' <<<"$v" | sort -u | paste -sd,)
		{ [ "$hs" = shared ] && [ "$vs" = twin ]; } || { [ "$hs" = twin ] && [ "$vs" = shared ]; } && break
		sleep 1
	done
	if { [ "$hs" = shared ] && [ "$vs" = twin ]; } || { [ "$hs" = twin ] && [ "$vs" = shared ]; }; then
		pass "$label: one game's alias NPC is shared, the other's its twin" "host: $h; vm: $v"
	else
		fail "$label: one game's alias NPC is shared, the other's its twin" "host: ${h:-none}; vm: ${v:-none}"
	fi
	dc host "console stopquest $edid" >/dev/null; dc vm "console stopquest $edid" >/dev/null
	say "made: put back (console): $edid stopped in both"
}

made_putback() {
	local one who id
	for one in "${MADE_PLACED[@]}"; do
		read -r who id <<<"$one"
		dc "$who" "console $id.disable" >/dev/null; dc "$who" "console $id.markfordelete" >/dev/null
	done
	[ ${#MADE_PLACED[@]} -gt 0 ] && say "made: put back (console): the ${#MADE_PLACED[@]} NPC(s) placed disabled and deleted"
	MADE_PLACED=()
}

step_made() {
	local g vmid s
	for g in host vm; do still $g; done
	MADE_PLACED=(); MADE_TWIN=""
	made_way host vm
	made_way vm host
	vmid=""
	for s in "${MADE_PLACED[@]}"; do [[ $s == vm\ * ]] && vmid=${s#vm }; done
	made_twin host vm
	made_save
	[ -n "$vmid" ] && made_keep "$vmid"
	made_quest
	made_putback
	section "made: both games"
	for g in host vm; do say "made: $g: $(dc "$g" 'npcs made' | cut -d'|' -f1)"; done
}

# ---- self-test: the reading ---------------------------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }

	# replies in the formats the plugin writes (NpcSync's `npcs made`, `npcs list`, `npcs made quests`)
	M='ours=3 adopted=1 theirs=1 said=4 shared=2 twins=1 waits=0 paired=1 unpaired=0 copiesReplaced=0 adoptedIn=1 adoptedOut=0 gone=0 theirsGone=0 theirsHeard=1 noSource=0 staleAnswers=0 | ours local=FF000801 phase=shared adopted=0 twin=none made=00068D5C pick=00068D5C body=000131FD cell=00000000 ws=0000003C pos=(100,200,0) | ours local=FF000802 phase=twin adopted=0 twin=FF000901@2 quest=00039F82 alias=3 made=000E5F1C pick=000E5F1C body=00013794 cell=00000000 ws=0000003C pos=(0,0,0) | ours local=FF000803 phase=shared adopted=1 twin=none made=00068D5C pick=00068D5C body=000131FD cell=00000000 ws=0000003C pos=(5,5,5) | theirs FF000901 spawner=2 quest=00039F82 alias=3 made=000E5F1C pick=000E5F1C body=00013794 cell=00000000 ws=0000003C pos=(0,0,0) gone=1'
	O=$(mr_ours "$M")
	t "ours: three" "$(wc -l <<<"$O" | tr -d ' ')" 3
	t "ours: a shared one" "$(mr_col "$O" FF000801 2)" shared
	t "ours: a twin's twin" "$(mr_col "$O" FF000802 4)" FF000901@2
	t "ours: its quest" "$(mr_col "$O" FF000802 6)" 00039F82
	t "ours: no quest" "$(mr_col "$O" FF000801 6)" -
	t "ours: kept" "$(mr_col "$O" FF000803 3)" 1
	t "ours: not listed" "$(mr_col "$O" FF000999 2)" ""
	t "theirs" "$(mr_theirs "$M")" "FF000901:2 000E5F1C 00039F82 1"
	t "new: of that base since" "$(mr_new "$(mr_ours "${M%% | ours local=FF000803*}")" "$O" 00068D5C)" FF000803
	t "new: none" "$(mr_new "$O" "$O" 00068D5C)" ""
	t "of quest" "$(mr_of_quest "$O" 00039F82)" "FF000802 twin FF000901@2"
	t "counts" "$(field adoptedIn "$M")" 1

	L='npcs=3 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(10,20,30) dead=0 child=0 weapon=0 combat=0 hp=1.000 sessionDead=0 | FF000901 spawner=2 local=FF000802 owner=2 role=copy kind=? at=(13,24,30) dead=0 child=0 weapon=0 combat=0 sessionDead=0 runner=2 mode=walk target=(13,24,30) apart=0 base=000E5F1C states=4 drawn=4 played=0 refused=0 walkRefused=0 gear=0 geared=0 wears=? dressed=? graphSpeed=0 twin=1 adopted=0 | FF000A01 spawner=2 local=FF000C00 owner=2 role=copy kind=? at=none dead=0 child=0 weapon=0 combat=0 sessionDead=0 twin=0 adopted=0'
	t "row: a spawn's twin" "$(mr_row "$L" FF000901:2 twin)" 1
	t "row: its local" "$(mr_row "$L" FF000901:2 local)" FF000802
	t "row: a load-order NPC" "$(mr_row "$L" 0001A67C role)" run
	t "row: a spawn by its id alone is not it" "$(mr_row "$L" FF000901 role)" ""
	t "row: not listed" "$(mr_row "$L" FF000999:1 role)" ""
	t "at" "$(mr_at "$L" FF000901:2)" "13 24 30"
	t "at: none" "$(mr_at "$L" FF000A01:2)" ""
	t "apart" "$(mr_apart "10 20 30" "13 24 30")" 5
	t "apart: one missing" "$(mr_apart "" "13 24 30")" ""
	L2='npcs=2 | 0001A67C local=0001A67C owner=2 role=copy kind=human at=(1,2,3) dead=0 sessionDead=0 | 0001B111 local=0001B111 owner=2 role=run kind=human at=none dead=0 sessionDead=0'
	t "shared npc: both have it with an actor" "$(mr_shared_npc "$L" "$L2")" "0001A67C 0001A67C 0001A67C"
	t "shared npc: none" "$(mr_shared_npc "$L" 'npcs=0')" ""
	Q='quests=2 | quest=00039F82 edid=WICourier alias=3 creates=000E5F1C at=2 atFill=1 forced=1 forcedRef=0001A2B3 | quest=000AAAAA edid= alias=1 creates=000E5F1D at=0 atFill=0 forced=0 forcedRef=00000000'
	t "quest edid" "$(mr_quest_edid "$Q" 00039F82)" WICourier
	t "quest edid: not listed" "$(mr_quest_edid "$Q" 00012345)" ""

	[ $bad -eq 0 ] && echo "made.sh selftest: all ok" || echo "made.sh selftest: FAILED"
	exit $bad
fi
