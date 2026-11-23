#!/bin/bash
# steps.sh: the named steps of smoke.sh, each a function step_<name> that makes asserting checks (assert.sh) from what
# both games say over their developer channels and write in their plugin logs. Sourced by smoke.sh after assert.sh and
# crashlib.sh; tools/rig/README.md says how a new feature adds its step.
#
# Every step reads both games, and every step that moves something does it both ways: the VM's player while the host
# watches, then the host's while the VM watches. Movement is the game's own: the virtual pad (dev `pad`, `walkto`)
# pushes a stick or a button and the game moves the player; nothing is placed or moved by a script, except `coc` from
# the main menu to reach the scene (console placement, said in the run's log as a HEAL).
#
# The host's game never saves and never loads (it may hold the maintainer's own saves): the save steps save and load on
# the VM only, under $SAVE_NAME, and smoke.sh moves that save aside (never deletes it) when the run ends.
#
# `bash tools/rig/steps.sh selftest` checks the steps' reading of the games' replies with both games faked.
[ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ] && : "${VM_SSH:=selftest-no-vm}" && export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
declare -F record >/dev/null || . "$(dirname "${BASH_SOURCE[0]}")/assert.sh"
declare -F crash_scan >/dev/null || . "$RIG/crashlib.sh"

VM_RUN=${VM_RUN:-C:/F4MP/in-vm-run.ps1}
SMOKE_CELL=${SMOKE_CELL:-Riverwood}      # where both players are put with coc when a game is at its main menu (open ground)
JOIN=${JOIN:-url}                        # url: the VM's Steam is given steam://joinlobby (an invite's path); dev: steam join <id>
SAVE_NAME=${SAVE_NAME:-SkyrimCoopRig}    # the VM's test save (never the host's)
NEAR=${NEAR:-1500}                       # units: two players further apart than this are put together again with coc
# Each mover's legs, "dx,dy,speed[,sprint]" (pad stick 0-1; sprint: lb held the whole leg): a run, a sprint, then two
# walks, round a rectangle so the scene ends where it began. Each leg ends facing a new way (the facing checks at rest).
# The sprint leg is twice as long as the run (about 3 s at a sprint), so it gets enough samples to judge.
MOVE_LEGS_VM=${MOVE_LEGS_VM:-"0,700,1.0 1400,0,1.0,sprint 0,-700,0.35 -1400,0,0.35"}
MOVE_LEGS_HOST=${MOVE_LEGS_HOST:-"1400,0,1.0 0,-700,1.0,sprint -1400,0,0.35 0,700,0.35"}
# The limits a stand-in is held to. A friend's state is drawn about 100 ms late (src/game/StateBuffer.h), about 40 u at a
# run; the samples of the two games are not taken at the same moment either.
LIMIT_REST=${LIMIT_REST:-60}            # u: the stand-in from its friend once both stand still
LIMIT_MEAN=${LIMIT_MEAN:-150}           # u: the mean distance while moving (a FAIL above it)
SOFT_MEAN=${SOFT_MEAN:-80}              # u: ... a WARN above it
LIMIT_MAX=${LIMIT_MAX:-400}             # u: the largest distance while moving
LIMIT_FOLLOW=${LIMIT_FOLLOW:-80}        # %: the stand-in covers at least this much of its friend's way
LIMIT_LEGS=${LIMIT_LEGS:-50}            # %: of the samples where the friend moves, the stand-in's graph Speed is above 0 in this many
LIMIT_JOIN_MS=${LIMIT_JOIN_MS:-20000}   # ms: a join slower than this is a WARN
LIMIT_SPRINT=${LIMIT_SPRINT:-50}        # %: of the samples where the friend sprints, the stand-in shows a sprint (shown r) in this many
MIN_SPRINT_SAMPLES=${MIN_SPRINT_SAMPLES:-4}  # a sprint leg with fewer samples where the friend sprints is not judged (RIG)
LIMIT_FALSE_SPRINT=${LIMIT_FALSE_SPRINT:-10}  # %: in a leg with no sprint, the stand-in shows one in at most this many samples
LIMIT_HEADING=${LIMIT_HEADING:-0.15}    # rad: at rest, the stand-in faces within this of the way its friend faces
MIN_TICK_RATE=${MIN_TICK_RATE:-20}      # ticks/s: a game slower than this (a display asleep, a starved machine) is the rig's fault

# Each step's one line, for smoke.sh --list and the README.
declare -A STEP_INFO=(
	[preflight]="both games run the same build, 1.7.104, the Address Library, Steam, the tick; the host loaded no save"
	[world]="both players in the world with no box open, near each other (coc $SMOKE_CELL from the main menu if not)"
	[join]="the host hosts, the VM joins over Steam (an invite's path), both say so; the join's time"
	[see]="each game has a stand-in of the other, placed, dressed and named, where its friend stands and facing its way; no strays"
	[move_vm]="the VM's player runs, sprints and walks (virtual pad); the host's stand-in follows, legs moving, sprinting when it does, facing its way at rest"
	[move_host]="the host's player runs, sprints and walks; the VM's stand-in of it follows the same way"
	[gait]="sneak and jump, each way: the watcher's stand-in shows it"
	[save_rejoin]="the VM saves mid-session (no stand-in in the save), leaves, rejoins, loads that save; the host ends the session and hosts again"
	[npcs]="every NPC near both players has one runner both games agree on; copies stand where their runners have them and wear their gear; a fighting NPC's copy plays its actions, a summon cast with the pad is shared, both ways; walking the road away hands NPCs over, both ways (steps/npcs.sh)"
	[npccombat]="a player's hits on a copy of an NPC the other game runs reach that game and are applied there; both show the same health; the hits kill it there, dead in both with the hitter its killer; raised, it lives in both; the runner's own kill kills the copy; both ways (steps/npccombat.sh)"
	[forms]="(only when named) the VM's plugins in another order: it joins, and an armor each puts on is on the other's stand-in as that game's own form (steps/forms.sh)"
	[final]="both games still up, no stray stand-ins, the plugin logs (errors, privacy), the host never saved or loaded, no crash"
)
STEPS="preflight world join see move_vm move_host gait save_rejoin npcs npccombat forms final"
# Steps that need the two games in one session; the state guard puts them back in one first (smoke.sh run()).
SESSION_STEPS="see move_vm move_host gait save_rejoin npcs npccombat forms"
# Steps a run makes only when --only names them: they change a game's files for a while (each puts them back, and the
# set-up heals what a run that died left), which the maintainer agrees to before they run by default.
OPTIN_STEPS="forms"

# --- small readers ---------------------------------------------------------------------------------------------
# wait_until <seconds> <command...>: until the command succeeds, every second
wait_until() { local t=$1 i; shift; for ((i = 0; i < t; i++)); do "$@" && return 0; sleep 1; [ $((i % 30)) = 29 ] && lock_keepalive; done; "$@"; }
in_world() { local w; w=$(dc "$1" where); [[ $w == cell=* ]] && [[ $w == *" modal=0"* ]]; }
pos_of() { xyz 'pos=' "$(dc "$1" where)"; }               # "x y z" of that game's player
standin_at() { xyz ' at=' "$(dc "$1" standins)"; }         # "x y z" of that game's stand-in of its friend
# placed <who>: that game's stand-in of its friend is in the world now: it has a form id and a place. The reply's placed=
# is not that: it counts the placements and never goes down (2 after a save took the stand-in out and placed it again),
# and a stand-in taken away reads standin=00000000 at=none with its count unchanged.
placed() { placed_in "$(dc "$1" standins)"; }
placed_in() { local id; id=$(field standin "$1"); [[ $id =~ ^[0-9A-F]{8}$ ]] && [ "$id" != 00000000 ] && [[ $(field at "$1") == \(* ]]; }
placements() { field placed "$(dc "$1" standins)"; }      # how many times that game placed a stand-in of its friend
# gone_in <standins reply>: no friend, no stand-in, and every stand-in sent out of the world is deleted. leaving= counts
# the deleted ones too, which the game holds until it frees them (minutes, on the rig): leaving=0 does not come soon. What
# must be 0 is the rest: leaving minus deleted (those still waiting for their disable, and those disabled and not yet
# deleted, the "(disabled=" count).
gone_in() {
	local l d; l=$(field leaving "$1"); d=$(sed -n 's/.* deleted=\([0-9]*\)).*/\1/p' <<<"$1" | head -1)
	[ "$(field players "$1")" = 0 ] && [ "$(field standins "$1")" = 0 ] && is_num "$l" && is_num "$d" && [ "$l" -eq "$d" ]
}
# space_in <where reply>: the space a player is in, "world <id>" outside, "cell <id>" inside (every interior reads
# world=00000000, so two interiors are told apart by their cell); empty when not in the world
space_in() { local w; [[ $1 == cell=* ]] || return 0; w=$(field world "$1"); if [ "$w" = 00000000 ]; then echo "cell $(field cell "$1")"; else echo "world $w"; fi; }
# angle_diff <a> <b>: the angle between two headings in radians (0 to pi), 3 decimals; empty when either is not a number
angle_diff() { is_num "$1" && is_num "$2" || return 0; awk -v a="$1" -v b="$2" 'BEGIN{pi = atan2(0, -1); d = a - b; d -= 2 * pi * int(d / (2 * pi)); if (d < 0) d += 2 * pi; if (d > pi) d = 2 * pi - d; printf "%.3f\n", d}'; }
# facing <check> <watcher>: at rest, the watcher's stand-in faces the way its friend faces (both read now)
facing() {
	local a b; a=$(field heading "$(dc "$2" standins)"); b=$(field heading "$(dc "$(other "$2")" where)")
	expect_le "$1" "$(angle_diff "$a" "$b")" "$LIMIT_HEADING" "rad (the stand-in ${a:-none}, its friend ${b:-none})"
}
# The frame rate. The plugin's tick runs at most once a frame (and every 16 ms at most), so a game drawing few frames
# (its display asleep, its machine starved) ticks few times: its stand-ins lag and its walks stutter, and a step's
# timing checks would measure the rig, not the mod. _now_ns is the clock (the self-test fakes it).
_now_ns() { date +%s%N; }
# tick_rates: "host=N vm=N", each game's ticks a second over about 3 s (a value is empty when that game gives none)
tick_rates() {
	local w t b out="" ; declare -A a0 t0
	for w in host vm; do t=$(_now_ns); a0[$w]=$(field ticks "$(dc $w status)"); t0[$w]=$(( (t + $(_now_ns)) / 2 )); done
	sleep 3
	for w in host vm; do
		t=$(_now_ns); b=$(field ticks "$(dc $w status)"); t=$(( (t + $(_now_ns)) / 2 ))
		out+="$w=$(is_num "${a0[$w]}" && is_num "$b" && awk -v a="${a0[$w]}" -v b="$b" -v s="$(( t - t0[$w] ))" 'BEGIN{if (s > 0) printf "%.0f", (b - a) / (s / 1e9)}') "
	done
	echo "${out% }"
}
# guard_rate <step>: both games tick at least $MIN_TICK_RATE a second; a slower one is a rig fault not healed (it is
# told, never put right: the machine's power settings are not the harness's to change) and the step is not run
guard_rate() {
	local rates w r
	rates=$(tick_rates)
	for w in host vm; do
		r=$(field $w "$rates")
		if ! is_num "$r"; then rig_broken game "state before $1: $w ticks" "its status gives no ticks"; return 1; fi
		if [ "$r" -lt "$MIN_TICK_RATE" ]; then
			rig_unhealed state "state before $1: $w frame rate" "$w runs at $r ticks/s, under $MIN_TICK_RATE (its display asleep? the machine starved?): the step's timing checks would measure the rig"
			return 1
		fi
	done
	return 0
}
welcomed() { local s; s=$(dc "$1" 'session status'); [ "$(field welcomed "$s")" = 1 ] && [ "$(field connection "$s")" = up ]; }
joined_both() { welcomed vm && [ "$(field players "$(dc host 'session status')")" -ge 1 ] 2>/dev/null; }
still() { dc "$1" 'walkto stop' >/dev/null; dc "$1" 'pad off' >/dev/null; }
alive() { local w; for w in host vm; do if [ "$(dc $w ping)" = pong ]; then echo -n "$w=up "; else echo -n "$w=DOWN "; fi; done; }

# vm_join: the VM joins the host's lobby: JOIN=url gives the VM's Steam client the lobby's steam://joinlobby address (the
# path an accepted invite takes), JOIN=dev tells its game the lobby id. The address names the host's Steam id: it is
# passed from one game to the other and never printed or written; the VM's scheduled task that held it is removed.
vm_join() {
	local lobby url task started gone
	lobby=$(field lobby "$(dc host status)")
	[ -n "$lobby" ] && [ "$lobby" != 0 ] || { echo "the host has no lobby"; return 1; }
	if [ "$JOIN" = dev ]; then
		[ "$(dc vm "steam join $lobby")" = joining ] || { echo "the VM's game refused to join"; return 1; }
		return 0
	fi
	url=$(dc host "steam joinurl")
	[[ $url =~ ^steam://joinlobby/$APP_ID/$lobby/[0-9]+$ ]] || { echo "the host gave no join address"; return 1; }
	dc vm "steam expectjoin" >/dev/null
	task=SkyrimCoop-Join-$(date +%H%M%S)
	started=$(vmssh "& $VM_RUN -Name '$task' -Exe 'C:\Program Files (x86)\Steam\steam.exe' -Arguments '$url'")
	url=""
	gone=$(vmssh "\$n = '$task'; for (\$i = 0; \$i -lt 30 -and (Get-ScheduledTask -TaskName \$n -ErrorAction SilentlyContinue).State -eq 'Running'; \$i++) { Start-Sleep 1 }; if ((Get-ScheduledTask -TaskName \$n -ErrorAction SilentlyContinue).State -ne 'Running') { Unregister-ScheduledTask -TaskName \$n -Confirm:\$false -ErrorAction SilentlyContinue }; if (Get-ScheduledTask -TaskName \$n -ErrorAction SilentlyContinue) { 'kept' } else { 'gone' }")
	[ "$gone" = gone ] || echo "WARN: the VM's scheduled task $task is still there (it names the host's Steam id); smoke.sh removes it at the end"
	grep -q started <<<"$started" || { echo "could not start the VM's Steam with the join address"; return 1; }
	return 0
}
# session_up: the host hosts and the VM joins; 0 when both say so within 90 s. Prints why not.
session_up() {
	local r
	r=$(dc host 'session host'); [[ $r == hosting* ]] || { echo "the host's game would not host: $r"; return 1; }
	wait_until 60 lobby_open || { echo "no Steam lobby in 60 s"; return 1; }
	r=$(vm_join) || { echo "$r"; return 1; }
	[ -n "$r" ] && echo "$r"
	wait_until 90 joined_both || { echo "not both in the session after 90 s: host $(dc host 'session status' | cut -c1-120); vm $(dc vm 'session status' | cut -c1-120)"; return 1; }
}
lobby_open() { local l; l=$(field lobby "$(dc host status)"); [ -n "$l" ] && [ "$l" != 0 ]; }

# move_stats <csv>: what a leg's samples show, one line of key=value. The CSV's columns: i, the mover's x y z and speed,
# the watcher's stand-in x y z and graph Speed, the mover's graphSprint (1 sprinting) and the stand-in's shown= (r when
# it shows a sprint). "moving" samples are those whose mover speed is above 10. sprinting: samples where the mover
# sprints; sprintshown: the % of those where the stand-in shows it; falsesprint: the % of the others where it shows one.
move_stats() {
	awk -F, 'NR > 1 && $2 != "" && $6 != "" {
			n++; e = sqrt(($2-$6)^2 + ($3-$7)^2); s += e; if (e > mx) mx = e
			if (n == 1) { x0 = $2; y0 = $3; sx0 = $6; sy0 = $7 }
			x1 = $2; y1 = $3; sx1 = $6; sy1 = $7
			if ($5 > 10) { mv++; if ($9 != "none" && $9 > 0) legs++ }
			if ($10 == "1") { sp++; if ($11 ~ /r/) spshown++ } else { nsp++; if ($11 ~ /r/) falsesp++ }
		}
		END {
			moved = sqrt((x1-x0)^2 + (y1-y0)^2); smoved = sqrt((sx1-sx0)^2 + (sy1-sy0)^2)
			printf "n=%d moved=%.0f smoved=%.0f follow=%.0f mean=%.0f max=%.0f moving=%d legs=%.0f sprinting=%d sprintshown=%.0f falsesprint=%.0f\n", n, moved, smoved,
				(moved > 0 ? 100 * smoved / moved : 0), (n ? s / n : -1), mx, mv, (mv ? 100 * legs / mv : 0),
				sp, (sp ? 100 * spshown / sp : 0), (nsp ? 100 * falsesp / nsp : 0)
		}' "$1"
}
# shown_has <letter> <standins reply>: the stand-in shows that gait (shown= m moving, s sneaking, r sprinting, j in the air)
shown_has() { [[ $(field shown "$2") == *"$1"* ]]; }

# --- the steps -------------------------------------------------------------------------------------------------
step_preflight() {
	section "preflight: both games, the build, the host's saves"
	local w s hs vs a b
	no_fallout || { _rig_stop "Fallout 4 runs" "Fallout 4 runs on the rig: one game at a time"; return; }
	for w in host vm; do
		s=$(dc $w status)
		if [ -z "$s" ]; then
			if [ $w = vm ] && [ "$(vmssh 'echo up' | tr -d '[:space:]')" != up ]; then rig_broken ssh "$w: the dev channel answers" "ssh to the VM does not answer"
			else rig_broken game "$w: the dev channel answers" "no status: the game is not running, or its dev channel is off (bDevChannel)"; fi
			continue
		fi
		pass "$w: the dev channel answers" "protocol $(field protocol "$s")"
		expect_eq "$w: game version" "$(field game "$s")" 1.7.104.0
		expect_eq "$w: the Address Library resolved" "$(field addrlib "$s")" ok
		expect_ge "$w: the tick runs" "$(field ticks "$s")" 1
		expect_eq "$w: Steam usable" "$(field steam "$s")" ready
		expect_eq "$w: Steam app" "$(field app "$s")" "$APP_ID"
		[ $w = host ] && hs=$s || vs=$s
	done
	[ -n "${hs:-}" ] && [ -n "${vs:-}" ] && expect_eq "both games speak the same protocol" "$(field protocol "$vs")" "$(field protocol "$hs")"
	# the frame rate: told here; the guard before each step that times something stops that step when it is low
	s=$(tick_rates)
	for w in host vm; do
		a=$(field $w "$s")
		if is_num "$a" && [ "$a" -ge "$MIN_TICK_RATE" ]; then pass "$w: its frame rate" "$a ticks/s (at least $MIN_TICK_RATE)"
		else warn "$w: its frame rate" "'$a' ticks/s, under $MIN_TICK_RATE: the steps that time something will not run while it stays so"; fi
	done
	a=$(dll_md5 host); b=$(dll_md5 vm)
	if [ -n "$a" ] && [ "$a" = "$b" ]; then pass "the same SkyrimCoop.dll on both" "md5 $a"; else fail "the same SkyrimCoop.dll on both" "host '${a:-none}', vm '${b:-none}'"; fi
	if [ -n "${CHECK_DLL:-}" ]; then expect_eq "the DLL is the build asked for (CHECK_DLL)" "$a" "$CHECK_DLL"; fi
	no_host_load ""
}
# no_host_load <label prefix>: the host's game has loaded no save since it started (turn 1 of 2026-11-21 loaded the
# maintainer's own through Continue). One that did is not run on: whatever happens next would happen to that save's
# world. smoke.sh's set-up calls it on every run, whatever steps are picked; preflight lists it again.
no_host_load() {
	if [ ! -s "$HOST_SKSE/SkyrimCoop.log" ]; then
		_rig_stop "the host's plugin log is not there" "no $HOST_SKSE/SkyrimCoop.log: whether the host's game loaded a save can't be told"
		return 1
	fi
	if grep -aq 'Load: about to load' "$HOST_SKSE/SkyrimCoop.log"; then
		_rig_stop "the host's game loaded a save" "its log says 'Load: about to load' since it started: reach the world with start.sh --cell (coc), never Continue or Load"
		return 1
	fi
	pass "${1}the host's game loaded no save since it started" "its log has no 'Load: about to load'"
}
# dll_md5 host|vm: the md5 of the SkyrimCoop.dll the game loads
dll_md5() {
	if [ "$1" = host ]; then md5sum "$HOST_GAME/Data/SKSE/Plugins/SkyrimCoop.dll" 2>/dev/null | cut -d' ' -f1
	else vmssh "(Get-FileHash -Algorithm MD5 -LiteralPath '$VM_GAME/Data/SKSE/Plugins/SkyrimCoop.dll').Hash" | tr -d '[:space:]' | tr 'A-F' 'a-f'; fi
}

step_world() {
	section "world: both players in the world, near each other"
	local w a b d
	for w in host vm; do
		if in_world $w; then pass "$w: in the world with no box open" "$(dc $w where | cut -c1-90)"; continue; fi
		place $w "$w: in the world with no box open"
	done
	a=$(pos_of host); b=$(pos_of vm); d=$(dist2 "$a" "$b")
	if same_space && [ -n "$d" ] && [ "$d" -le "$NEAR" ]; then
		pass "the two players are near each other" "$d u apart in one $(space_in "$(dc host where)" | cut -d' ' -f1)"
	else
		place host "the two players are near each other" && place vm "the two players are near each other"
		a=$(pos_of host); b=$(pos_of vm); d=$(dist2 "$a" "$b")
		if same_space; then expect_le "the two players are near each other, after coc" "$d" "$NEAR" u
		else fail "the two players are near each other, after coc" "not in one space: host $(space_in "$(dc host where)"), vm $(space_in "$(dc vm where)")"; fi
	fi
}
# same_space: both players in one worldspace outside, or in one cell inside
same_space() { local a; a=$(space_in "$(dc host where)"); [ -n "$a" ] && [ "$a" = "$(space_in "$(dc vm where)")" ]; }
# place <who> <check>: coc $SMOKE_CELL (from the main menu, or to bring the players together), then up to 90 s for that
# player in the world with no box open. A HEAL when it works (console placement, said); not healed otherwise.
place() {
	local r
	r=$(dc "$1" "console coc $SMOKE_CELL")
	if [ "$r" = ran ] && wait_until 90 in_world "$1"; then healed "$2" "$1: put in $SMOKE_CELL with coc (console placement): $(dc "$1" where | cut -c1-80)"; return 0; fi
	rig_unhealed state "$2" "$1: coc $SMOKE_CELL ('$r') did not leave the player in the world with no box open: $(dc "$1" where | cut -c1-120)"
	return 1
}

step_join() {
	section "join: the host hosts, the VM joins over Steam ($JOIN)"
	local r ms
	dc vm 'session leave' >/dev/null; dc host 'session leave' >/dev/null; dc host 'steam stophost' >/dev/null; sleep 3
	if r=$(session_up); then
		pass "the VM joins the host's session ($([ "$JOIN" = url ] && echo "steam://joinlobby, an invite's path" || echo "by lobby id"))" "vm: $(dc vm 'session status' | cut -c1-80)"
	else
		fail "the VM joins the host's session" "$r"; return
	fi
	[[ $r == WARN:* ]] && warn "the VM's join task is removed" "${r#WARN: }"
	expect_eq "host: its role" "$(field role "$(dc host 'session status')")" host
	expect_eq "vm: its role" "$(field role "$(dc vm 'session status')")" friend
	ms=$(log_since_mark vm 'Session: in the session as player' | tail -1 | sed -n 's/.*, \([0-9]*\) ms after asking.*/\1/p')
	warn_gt "the join's time (asked to in the session)" "${ms:-}" "$LIMIT_JOIN_MS" ms
	expect_zero "vm: the host turned it away" "$(log_since_mark vm 'Session: the host turned us away' | wc -l)"
}

step_see() {
	section "see: each game's stand-in of the other"
	local w o s a b
	for w in host vm; do still $w; done
	for w in host vm; do
		o=$(other $w)
		if wait_until 30 placed $w; then pass "$w: a stand-in of the $o's player is placed" "$(dc $w standins | grep -o 'standin=[0-9A-F]*' | head -1)"
		else fail "$w: a stand-in of the $o's player is placed" "$(dc $w standins | cut -c1-200)"; continue; fi
		s=$(dc $w standins)
		expect_eq "$w: the stand-in is dressed as its friend" "$(field dressed "$s")" 1
		expect_eq "$w: the stand-in shows its friend's name" "$(field named "$s")" friend
	done
	sleep 2
	for w in host vm; do
		o=$(other $w)
		a=$(standin_at $w); b=$(pos_of $o)
		expect_le "$w: the stand-in stands where the $o's player stands" "$(dist2 "$a" "$b")" "$LIMIT_REST" u
		facing "$w: the stand-in faces the way the $o's player faces" "$w"
		expect_zero "$w: no stray stand-ins" "$(field strays "$(dc $w strays)")"
		a=$(field received "$(dc $w 'session status')"); sleep 3; b=$(field received "$(dc $w 'session status')")
		is_num "$a" && is_num "$b" && expect_ge "$w: its friend's states keep coming (3 s)" "$((b - a))" 20 states || fail "$w: its friend's states keep coming (3 s)" "received '$a' then '$b'"
	done
}

# step_move <mover>: the mover's legs ($MOVE_LEGS_<MOVER>), the other game watching its stand-in
step_move() {
	local mover=$1 watcher legs leg i=0
	watcher=$(other "$mover")
	section "move: the $mover's player moves, the $watcher watches"
	[ "$mover" = vm ] && legs=$MOVE_LEGS_VM || legs=$MOVE_LEGS_HOST
	for leg in $legs; do i=$((i + 1)); move_leg "$mover" "$watcher" "$leg" "$mover leg $i"; done
}
step_move_vm() { step_move vm; }
step_move_host() { step_move host; }
# move_leg <mover> <watcher> "dx,dy,speed[,sprint]" <label>
move_leg() {
	local mover=$1 watcher=$2 label=$4 dx dy speed how p0 tx ty r f k mp sp m s st want st0 stats a b radius=20
	IFS=, read -r dx dy speed how <<<"$3"
	[ "$how" = sprint ] && { label="$label (sprint)"; radius=80; }   # a sprint turns wide: a wider arrival, no circling
	p0=$(pos_of "$mover"); [ -n "$p0" ] || { rig_broken game "$label: the walk" "$mover's player has no place ($(dc "$mover" where | cut -c1-80))"; return; }
	read -r tx ty < <(awk -v p="$p0" -v dx="$dx" -v dy="$dy" 'BEGIN{split(p, a, " "); printf "%.0f %.0f\n", a[1]+dx, a[2]+dy}')
	want=$(dist2 "$p0" "$tx $ty 0")
	r=$(dc "$mover" "walkto $tx $ty $speed 60 $radius")
	[[ $r == walking* ]] || { rig_broken input "$label: the walk" "$mover's walkto refused: $r"; return; }
	# sprint: lb held for the whole leg, over the walk's stick (the walk's end lets go of every button)
	if [ "$how" = sprint ]; then
		r=$(dc "$mover" 'pad button lb 30000')
		[[ $r == button* ]] || { still "$mover"; rig_broken input "$label: the sprint" "$mover's pad refused lb: $r"; return; }
	fi
	f=$A_OUT/samples-${label//[ ()]/-}.csv; echo "i,mx,my,mz,mspeed,sx,sy,sz,sgraph,msprint,sshown" > "$f"
	# a sprint leg is short in time: no pause between its samples (each costs three dev-channel calls)
	local pause=0.5 most=60; [ "$how" = sprint ] && { pause=0; most=150; }
	for ((k = 1; k <= most; k++)); do
		mp=$(dc "$mover" where); sp=$(dc "$watcher" standins)
		m=$(xyz 'pos=' "$mp"); s=$(xyz ' at=' "$sp"); m=${m:-"  "}; s=${s:-"  "}   # a missing place keeps its three columns
		echo "$k,${m// /,},$(field speed "$mp"),${s// /,},$(field graphSpeed "$sp"),$(field graphSprint "$mp"),$(field shown "$sp")" >> "$f"
		st=$(dc "$mover" walkto); [[ $st == "not walking"* ]] && break
		[ "$pause" = 0 ] || sleep "$pause"
	done
	still "$mover"; sleep 2
	st0=$(dc "$mover" walkto)
	stats=$(move_stats "$f"); echo "     $label: $stats (samples in $f)"
	# the walk is the rig's input: one that never got going is a rig fault, and the step runs once more
	r=$(dist2 "$p0" "$(pos_of "$mover")")
	if ! is_num "$r" || [ "$r" -lt $((want / 2)) ]; then
		rig_fault walk "$label: the walk" "$mover moved '$r' of $want u ($st0)"; return
	fi
	pass "$label: the walk" "$mover moved $r of $want u at speed $speed ($(cut -c1-60 <<<"$st0"))"
	[ "$(field n "$stats")" -gt 0 ] 2>/dev/null || { fail "$label: the $watcher's stand-in is seen while its friend moves" "no sample had both places: $(dc "$watcher" standins | cut -c1-160)"; return; }
	expect_ge "$label: the stand-in covers its friend's way" "$(field follow "$stats")" "$LIMIT_FOLLOW" %
	expect_le "$label: mean distance stand-in to friend while moving" "$(field mean "$stats")" "$LIMIT_MEAN" u
	warn_gt "$label: mean distance stand-in to friend while moving (soft)" "$(field mean "$stats")" "$SOFT_MEAN" u
	expect_le "$label: largest distance stand-in to friend while moving" "$(field max "$stats")" "$LIMIT_MAX" u
	expect_ge "$label: the stand-in's legs move (graph Speed above 0 while its friend moves)" "$(field legs "$stats")" "$LIMIT_LEGS" %
	if [ "$how" = sprint ]; then sprint_judge "$label" "$mover" "$stats"
	else
		expect_le "$label: the stand-in shows no sprint while its friend does not" "$(field falsesprint "$stats")" "$LIMIT_FALSE_SPRINT" %
	fi
	a=$(standin_at "$watcher"); b=$(pos_of "$mover")
	expect_le "$label: at rest, the stand-in where its friend stopped" "$(dist2 "$a" "$b")" "$LIMIT_REST" u
	facing "$label: at rest, the stand-in faces the way its friend faces" "$watcher"
	r=$(field graphSpeed "$(dc "$watcher" standins)")
	expect_le "$label: at rest, the stand-in's legs stop (graph Speed)" "$r" 5
	expect_eq "$label: at rest, the stand-in shows no sprint" "$(field shown "$(dc "$watcher" standins)" | tr -cd 'r')" ""
}

# sprint_judge <label> <mover> <move_stats line>: a sprint leg's check. The hold is the rig's input: a mover that never
# sprinted is a rig fault, as a walk that never got going, and so are too few sprinting samples to judge a share (one or
# two samples that fall in the ~100 ms before the stand-in shows it would be a coin toss, not the mod's failure).
sprint_judge() {
	local n; n=$(field sprinting "$3")
	if ! is_num "$n" || [ "$n" = 0 ]; then rig_fault input "$1: the sprint" "lb held, but the $2's graphSprint never read 1"
	elif [ "$n" -lt "$MIN_SPRINT_SAMPLES" ]; then rig_fault input "$1: the sprint" "too few sprinting samples ($n, fewer than $MIN_SPRINT_SAMPLES) to judge"
	else expect_ge "$1: the stand-in sprints while its friend sprints (shown r)" "$(field sprintshown "$3")" "$LIMIT_SPRINT" "% of $n samples"; fi
}

step_gait() {
	local mover watcher
	for mover in vm host; do
		watcher=$(other $mover)
		section "gait: the $mover's player sneaks and jumps, the $watcher watches"
		gait_sneak "$mover" "$watcher"
		gait_jump "$mover" "$watcher"
	done
}
gait_sneak() {
	local mover=$1 watcher=$2 k seen=""
	still "$mover"
	dc "$mover" 'pad button ls 150' >/dev/null; sleep 1.5
	if [ "$(field graphSneak "$(dc "$mover" where)")" != 1 ]; then rig_fault input "$mover sneaks: the press" "the left stick press left the $mover's player standing ($(dc "$mover" where | grep -o 'graphSneak=[^ ]*'))"; return; fi
	for ((k = 0; k < 15; k++)); do shown_has s "$(dc "$watcher" standins)" && { seen=1; break; }; sleep 0.2; done
	[ -n "$seen" ] && pass "$mover sneaks: the $watcher's stand-in sneaks" "shown=$(field shown "$(dc "$watcher" standins)")" || fail "$mover sneaks: the $watcher's stand-in sneaks" "shown=$(field shown "$(dc "$watcher" standins)") for 3 s"
	dc "$mover" 'pad button ls 150' >/dev/null; sleep 1.5; seen=""
	# the press to stand up must have reached the mover's game, or a stand-in still sneaking is right, not the mod's FAIL
	if [ "$(field graphSneak "$(dc "$mover" where)")" = 1 ]; then rig_fault input "$mover stands up: the press" "the left stick press left the $mover's player sneaking"; still "$mover"; dc "$mover" 'pad button ls 150' >/dev/null; sleep 1; return; fi
	for ((k = 0; k < 15; k++)); do shown_has s "$(dc "$watcher" standins)" || { seen=1; break; }; sleep 0.2; done
	[ -n "$seen" ] && pass "$mover stands up: the $watcher's stand-in stands up" "shown=$(field shown "$(dc "$watcher" standins)")" || fail "$mover stands up: the $watcher's stand-in stands up" "still sneaking 3 s after"
}
gait_jump() {
	local mover=$1 watcher=$2 k z0 z top="" seen=""
	still "$mover"; sleep 1
	z0=$(pos_of "$mover" | cut -d' ' -f3)
	dc "$mover" 'pad button y 100' >/dev/null
	for ((k = 0; k < 12; k++)); do
		shown_has j "$(dc "$watcher" standins)" && seen=1
		z=$(pos_of "$mover" | cut -d' ' -f3); is_num "$z" && is_num "$z0" && _cmp "$z" '>' "$((${z0%.*} + 20))" && top=1
		sleep 0.15
	done
	if [ -z "$top" ]; then rig_fault input "$mover jumps: the press" "the Y press did not lift the $mover's player 20 u"; return; fi
	[ -n "$seen" ] && pass "$mover jumps: the $watcher's stand-in shows the jump" "seen in the air" || fail "$mover jumps: the $watcher's stand-in shows the jump" "no j in its shown= while the $mover's player was in the air"
}

step_save_rejoin() {
	section "save: the VM saves mid-session (the host never saves)"
	local r s saves n0
	# the run's mark stays where it is (final's log checks read from it): the save's line is counted before and after
	saves=$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)
	n0=$(placements vm)
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { fail "vm: saves mid-session" "the console refused: $r"; return; }
	sleep 8
	if [ "$(dc vm ping)" != pong ]; then fail "vm: the game lives through its save" "its dev channel went silent after the save (a crash? see the crash scan)"; _rig_stop "the VM's game went down" "after its save mid-session"; return; fi
	pass "vm: the game lives through its save" "its dev channel answers"
	expect_ge "vm: its log says it saved" "$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)" $((saves + 1))
	expect_zero "vm: no stray stand-ins after its save" "$(field strays "$(dc vm strays)")"
	wait_until 15 placed vm && pass "vm: its stand-in of the host is back after the save" "$(dc vm standins | grep -o 'standin=[0-9A-F]* placed=[0-9]*' | head -1)" || fail "vm: its stand-in of the host is back after the save" "$(dc vm standins | cut -c1-160)"
	# the save took it out of the world (none is written into a save) and placed it again: its placements went up
	if is_num "$n0"; then expect_ge "vm: its save took the stand-in out and placed it again (placements)" "$(placements vm)" $((n0 + 1))
	else fail "vm: its save took the stand-in out and placed it again (placements)" "no placed= count before the save: $(dc vm standins | cut -c1-160)"; fi
	placed host && pass "host: its stand-in of the VM stays through the VM's save" "$(dc host standins | grep -o 'standin=[0-9A-F]*' | head -1)" || fail "host: its stand-in of the VM stays through the VM's save" "$(dc host standins | cut -c1-160)"

	section "leave: the VM leaves, then joins again"
	r=$(dc vm 'session leave'); [[ $r == left* ]] || fail "vm: leaves" "$r"
	gone_from host && pass "host: the VM's stand-in goes when it leaves" "$(dc host standins | cut -c1-80)" || fail "host: the VM's stand-in goes when it leaves" "$(dc host standins | cut -c1-200)"
	expect_zero "host: no stray stand-ins after the VM left" "$(field strays "$(dc host strays)")"
	expect_zero "vm: no stray stand-ins after it left" "$(field strays "$(dc vm strays)")"
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	wait_until 30 placed host && wait_until 30 placed vm && pass "both: stand-ins placed again after the rejoin" "" || fail "both: stand-ins placed again after the rejoin" "host $(dc host standins | grep -o 'standin=[0-9A-F]* placed=[0-9]* dressed=[01] at=[^ ]*' | head -1), vm $(dc vm standins | grep -o 'standin=[0-9A-F]* placed=[0-9]* dressed=[01] at=[^ ]*' | head -1)"

	section "load: the VM loads its own save in the session"
	r=$(dc vm "console load $SAVE_NAME"); [ "$r" = ran ] || { fail "vm: loads its save" "the console refused: $r"; return; }
	sleep 5
	if wait_until 90 in_world vm; then pass "vm: back in the world after its load" "$(dc vm where | cut -c1-80)"; else fail "vm: back in the world after its load" "$(dc vm where | cut -c1-160)"; return; fi
	sleep 5
	expect_zero "vm: no stray stand-ins after the load (none came back from the save)" "$(field strays "$(dc vm strays)")"
	wait_until 30 welcomed vm && pass "vm: still in the session after its load" "" || fail "vm: still in the session after its load" "$(dc vm 'session status' | cut -c1-160)"
	wait_until 30 placed vm && pass "vm: its stand-in of the host is back after the load" "" || fail "vm: its stand-in of the host is back after the load" "$(dc vm standins | cut -c1-160)"

	section "end: the host ends the session, then hosts again"
	r=$(dc host 'session leave'); [[ $r == left* ]] || fail "host: ends the session" "$r"
	gone_from vm && pass "vm: the host's stand-in goes when the host ends the session" "" || fail "vm: the host's stand-in goes when the host ends the session" "$(dc vm standins | cut -c1-200)"
	s=$(dc vm 'session status')
	[ "$(field connection "$s")" != up ] && pass "vm: its connection is down" "$(field connection "$s")" || fail "vm: its connection is down" "$s"
	expect_zero "host: no stray stand-ins after ending" "$(field strays "$(dc host strays)")"
	expect_zero "vm: no stray stand-ins after the host ended" "$(field strays "$(dc vm strays)")"
	dc vm 'session leave' >/dev/null; dc host 'steam stophost' >/dev/null; sleep 3
	if r=$(session_up); then pass "both: in one session again" ""; else fail "both: in one session again" "$r"; fi
}
# gone_from <who>: within 30 s, that game has no friend, no stand-in, and every stand-in it sent out is deleted (gone_in;
# the game frees the deleted ones much later, so leaving= itself does not fall to 0 in a run)
gone_from() { wait_until 30 _gone "$1"; }
_gone() { gone_in "$(dc "$1" standins)"; }

step_final() {
	section "final: both games, the logs, no crash"
	local w
	for w in host vm; do
		[ "$(dc $w ping)" = pong ] && pass "$w: still running" "" || fail "$w: still running" "its dev channel gives no answer"
		expect_zero "$w: no stray stand-ins" "$(field strays "$(dc $w strays)")"
		log_check $w "$w"
		log_privacy $w "$w"
	done
	expect_zero "host: never saved in the run" "$(grep -caE 'Save: saving' "$A_OUT/logdelta-host.txt")"
	expect_zero "host: never loaded in the run" "$(grep -caE 'Load: about to load' "$A_OUT/logdelta-host.txt")"
	crash_scan "final"
}

# Steps in files of their own (steps/<name>.sh), each with its own self-test
declare -F step_forms >/dev/null || . "$RIG/steps/forms.sh"
declare -F step_npcs >/dev/null || . "$RIG/steps/npcs.sh"
declare -F step_npccombat >/dev/null || . "$RIG/steps/npccombat.sh"

# --- self-test: the steps' reading of the games, both games faked ------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init stepstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	# replies in the formats the plugin writes (LocalPlayer::Describe, RemotePlayers::Describe, Session::Describe, Strays)
	WHERE='cell=00000000 world=0000003C pos=(17225,-47204,-128) heading=1.000 speed=0 graphSpeed=0 dir=0 pitch=0 flags=0x00 graphSprint=0 graphSneak=0 menus=HUD_Menu modal=0'
	STANDINS='players=1 standins=1 leaving=0 (disabled=0 deleted=0) driven speed=10 direction=10 channels=2 from=hook | player 2 standin=FF0008AE placed=1 dressed=1 at=(17230,-47200,-128) heading=1.000 target=(17230,-47200,-128) speed=0 dir=0.000 flags=0x00 graphSpeed=0 graphDir=0.000 graphSampled=0 shown=---- drawn=10 refused=0 taken=1 states=5 newest=(17230,-47200,-128) in 0000003C dist=6 named=friend aimed=0'
	# a stand-in taken away (a save, out of range): its id 0, no place, its count of placements unchanged
	TAKEN='players=1 standins=0 leaving=1 (disabled=1 deleted=0) driven speed=10 direction=10 channels=2 from=hook | player 2 standin=00000000 placed=1 dressed=0 at=none target=(17230,-47200,-128) speed=0 dir=0.000 flags=0x00 graphSpeed=none graphDir=none graphSampled=none shown=---- drawn=10 refused=0 taken=1 states=5 newest=(17230,-47200,-128) in 0000003C dist=6 named=none aimed=0'
	NEVER='players=1 standins=0 leaving=0 (disabled=0 deleted=0) driven speed=0 direction=0 channels=2 from=hook | player 2 standin=00000000 placed=0 dressed=0 at=none target=none graphSpeed=none graphDir=none graphSampled=none shown=---- drawn=0 refused=0 taken=0 states=0 newest=none dist=? named=none aimed=0'
	t "in_world" "$(dc() { echo "$WHERE"; }; in_world host && echo yes)" yes
	t "in_world: a modal box is not" "$(dc() { echo "${WHERE/modal=0/modal=1}"; }; in_world host || echo no)" no
	t "in_world: the main menu is not" "$(dc() { echo 'not in world menus=Main_Menu modal=0'; }; in_world host || echo no)" no
	t "pos_of" "$(dc() { echo "$WHERE"; }; pos_of host)" "17225 -47204 -128"
	t "standin_at: the stand-in's own place" "$(dc() { echo "$STANDINS"; }; standin_at host)" "17230 -47200 -128"
	t "placed" "$(dc() { echo "$STANDINS"; }; placed host && echo yes)" yes
	t "placed: placed again (placed=2, a count) is placed" "$(dc() { echo "${STANDINS/placed=1/placed=2}"; }; placed host && echo yes)" yes
	t "placed: taken away (standin=00000000 at=none, placed=1) is not" "$(dc() { echo "$TAKEN"; }; placed host || echo no)" no
	t "placed: never placed is not" "$(dc() { echo "$NEVER"; }; placed host || echo no)" no
	t "placements: the count" "$(dc() { echo "${STANDINS/placed=1/placed=3}"; }; placements host)" 3
	# gone_in: deleted stand-ins the game still holds (leaving=deleted) are gone; one disabled or still waiting is not
	G='players=0 standins=0 leaving=3 (disabled=0 deleted=3) driven speed=10 direction=10 channels=2 from=hook'
	t "gone: no friend, deleted ones held (leaving=3 deleted=3)" "$(gone_in "$G" && echo yes)" yes
	t "gone: one disabled, not yet deleted, is not" "$(gone_in "${G/(disabled=0 deleted=3)/(disabled=1 deleted=2)}" || echo no)" no
	t "gone: one waiting for its disable is not" "$(gone_in "${G/(disabled=0 deleted=3)/(disabled=0 deleted=2)}" || echo no)" no
	t "gone: a friend still there is not" "$(gone_in "${G/players=0/players=1}" || echo no)" no
	t "gone: a stand-in still placed is not" "$(gone_in "${G/standins=0/standins=1}" || echo no)" no
	t "gone: the 2026-11-22 rig reply after a leave" "$(gone_in 'players=0 standins=0 leaving=4 (disabled=0 deleted=4) driven speed=1 direction=1 channels=2 from=hook' && echo yes)" yes
	# space_in: the world outside, the cell inside
	t "space_in: outside" "$(space_in "$WHERE")" "world 0000003C"
	t "space_in: inside" "$(space_in 'cell=0001A26F world=00000000 pos=(1,2,3) heading=0 modal=0')" "cell 0001A26F"
	t "space_in: two interiors differ" "$([ "$(space_in 'cell=0001A26F world=00000000 pos=(1,2,3)')" = "$(space_in 'cell=00016BCF world=00000000 pos=(1,2,3)')" ] || echo differ)" differ
	t "space_in: not in the world" "$(space_in 'not in world menus=Main_Menu')" ""
	# angle_diff: radians, wrapped
	t "angle_diff: near" "$(angle_diff 1.000 1.050)" 0.050
	t "angle_diff: across 0" "$(angle_diff 0.100 6.200)" 0.183
	t "angle_diff: opposite" "$(angle_diff 0 3.14159)" 3.142
	t "angle_diff: not a number" "$(angle_diff none 1)" ""
	# guard_rate: the frame rate from two reads of ticks 3 s apart (the clock faked: each read takes 0.75 s)
	rate_case() {  # rate_case <host ticks per read> <vm ticks per read>: the guard's checks, one line
		local rcd; rcd=$(mktemp -d); echo 0 > "$rcd/clock"; echo 0 > "$rcd/host"; echo 0 > "$rcd/vm"
		_now_ns() { local c; c=$(cat "$rcd/clock"); echo $((c + 750000000)) > "$rcd/clock"; echo "$c"; }
		RATE_STEP_host=$1; RATE_STEP_vm=$2
		dc() { local n v=RATE_STEP_$1; n=$(cat "$rcd/$1"); echo $((n + ${!v})) > "$rcd/$1"; echo "skyrimcoop protocol=3 game=1.7.104.0 ticks=$n steam=ready"; }
		guard_rate x && echo "ran"; tick_rates; rm -rf "$rcd"
	}
	sd=$d/rate1; mkdir -p "$sd"; o=$( ( A_OUT=$sd; a_init rate >/dev/null; rate_case 180 165 ) 2>&1 )
	t "guard_rate: 60 and 55 ticks/s run the step" "$(grep -c '^ran$' <<<"$o")" 1
	t "tick_rates: the rates" "$(tail -1 <<<"$o")" "host=60 vm=55"
	sd=$d/rate2; mkdir -p "$sd"; o=$( ( A_OUT=$sd; a_init rate >/dev/null; rate_case 180 21 ) 2>&1 )
	t "guard_rate: a VM at 7 ticks/s is a rig fault, not the mod's FAIL" "$(grep -c '^RIGFAULT .*vm runs at 7 ticks/s' <<<"$o")" 1
	t "guard_rate: ... and the step is not run" "$(grep -c '^ran$' <<<"$o")" 0
	t "guard_rate: ... no FAIL" "$(grep -c '^FAIL' <<<"$o")" 0
	t "welcomed" "$(dc() { echo 'role=friend connection=up welcomed=1 player=2 host=1 players=1 sent=5 received=9 looksSent=1 rtt=40 serverPlayers=- lastEnd=none'; }; welcomed vm && echo yes)" yes
	t "welcomed: connecting is not" "$(dc() { echo 'role=friend connection=connecting welcomed=0 player=0'; }; welcomed vm || echo no)" no
	t "shown_has sneak" "$(shown_has s 'x shown=ms-- drawn=1' && echo yes)" yes
	t "shown_has jump: no" "$(shown_has j 'x shown=m--- drawn=1' || echo no)" no
	printf '%s\n' i,mx,my,mz,mspeed,sx,sy,sz,sgraph 1,0,0,0,0,0,0,0,0 2,0,100,0,300,0,60,0,250 3,0,300,0,300,0,260,0,250 4,0,500,0,300,0,460,0,none 5,0,700,0,0,0,690,0,0 > "$d/legs.csv"
	st=$(move_stats "$d/legs.csv")
	t "move_stats: moved" "$(field moved "$st")" 700
	t "move_stats: the stand-in's way" "$(field smoved "$st")" 690
	t "move_stats: follow %" "$(field follow "$st")" 99
	t "move_stats: mean" "$(field mean "$st")" 26
	t "move_stats: max" "$(field max "$st")" 40
	t "move_stats: moving samples" "$(field moving "$st")" 3
	t "move_stats: legs % (a 'none' graph read counts as still)" "$(field legs "$st")" 67
	printf 'i,mx,my,mz,mspeed,sx,sy,sz,sgraph\n' > "$d/none.csv"
	t "move_stats: no samples" "$(field n "$(move_stats "$d/none.csv")")" 0
	# step_see on two faked games: good replies pass, a stand-in far off and a stray fail
	fake() {  # fake <standins> <strays>: both games give these replies
		FS_STANDINS=$1; FS_STRAYS=$2; FS_N=$(mktemp); echo 0 > "$FS_N"   # a file: each reply is read in a subshell
		dc() { local n; case "$2" in where) echo "$WHERE";; standins) echo "$FS_STANDINS";; strays) echo "$FS_STRAYS";;
			'session status') n=$(( $(cat "$FS_N") + 30 )); echo "$n" > "$FS_N"; echo "role=x connection=up welcomed=1 received=$n";; *) echo ok;; esac; }
	}
	sd=$d/see1; mkdir -p "$sd"; o=$( ( A_OUT=$sd; a_init see >/dev/null; fake "$STANDINS" 'strays=0 standins=1 presetActors=1'; step_see; a_summary ) 2>&1 ); r=$?
	t "step_see: good replies pass" "$r" 0
	t "step_see: ... both ways" "$(grep -c '^PASS .*: the stand-in stands where' <<<"$o")" 2
	sd=$d/see2; mkdir -p "$sd"; o=$( ( A_OUT=$sd; a_init see >/dev/null; fake "${STANDINS/at=(17230,-47200,-128)/at=(17600,-47200,-128)}" 'strays=1 standins=1 presetActors=2 FF000900'; step_see; a_summary ) 2>&1 ); r=$?
	t "step_see: a stand-in 375 u off fails" "$(grep -c '^FAIL .*the stand-in stands where.*375 u, more than 60' <<<"$o")" 2
	t "step_see: a stray fails" "$(grep -c '^FAIL .*no stray stand-ins' <<<"$o")" 2
	sd=$d/see3; mkdir -p "$sd"; o=$( ( A_OUT=$sd; a_init see >/dev/null; fake "$NEVER" 'strays=0'; step_see; a_summary ) 2>&1 )
	t "step_see: a stand-in never placed fails" "$(grep -c '^FAIL .*a stand-in of the .* is placed' <<<"$o")" 2
	sd=$d/see3b; mkdir -p "$sd"; o=$( ( A_OUT=$sd; a_init see >/dev/null; fake "$TAKEN" 'strays=0'; step_see; a_summary ) 2>&1 )
	t "step_see: a stand-in taken away (placed=1 still) fails" "$(grep -c '^FAIL .*a stand-in of the .* is placed' <<<"$o")" 2
	sd=$d/see4; mkdir -p "$sd"; o=$( ( A_OUT=$sd; a_init see >/dev/null; fake "${STANDINS/heading=1.000/heading=2.571}" 'strays=0 standins=1 presetActors=1'; step_see; a_summary ) 2>&1 )
	t "step_see: a stand-in facing another way fails, both ways" "$(grep -c '^FAIL .*the stand-in faces the way.*1.571 rad' <<<"$o")" 2
	t "step_see: ... and only that fails" "$(grep -c '^FAIL ' <<<"$o")" 2
	# move_stats: the sprint columns
	printf '%s\n' i,mx,my,mz,mspeed,sx,sy,sz,sgraph,msprint,sshown 1,0,0,0,400,0,0,0,300,1,m--- 2,0,100,0,600,0,60,0,500,1,m-r- 3,0,300,0,600,0,260,0,500,1,m-r- 4,0,500,0,600,0,460,0,500,1,m-r- 5,0,700,0,0,0,690,0,0,0,---- > "$d/sprint.csv"
	st=$(move_stats "$d/sprint.csv")
	t "move_stats: sprinting samples" "$(field sprinting "$st")" 4
	t "move_stats: the stand-in shows the sprint (%)" "$(field sprintshown "$st")" 75
	t "move_stats: no sprint shown while not sprinting" "$(field falsesprint "$st")" 0
	printf '%s\n' i,mx,my,mz,mspeed,sx,sy,sz,sgraph,msprint,sshown 1,0,0,0,300,0,0,0,300,0,m-r- 2,0,100,0,300,0,60,0,300,0,m--- 3,0,200,0,300,0,160,0,300,0,m--- 4,0,300,0,300,0,260,0,300,0,m-r- > "$d/falsesprint.csv"
	t "move_stats: a sprint shown while its friend runs (%)" "$(field falsesprint "$(move_stats "$d/falsesprint.csv")")" 50
	t "move_stats: the mover never sprinted" "$(field sprinting "$(move_stats "$d/falsesprint.csv")")" 0
	# sprint_judge: enough samples are judged; one sprinting sample (shown late) or none is RIG, never FAIL
	sj() { local sd=$d/sj$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init sj >/dev/null; sprint_judge "vm leg 2 (sprint)" vm "$(move_stats "$2")"; a_summary ) 2>&1; }
	o=$(sj 1 "$d/sprint.csv"); t "sprint_judge: 4 samples, 75% shown, passes" "$(grep -c '^PASS .*sprints while its friend sprints' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0
	printf '%s
' i,mx,my,mz,mspeed,sx,sy,sz,sgraph,msprint,sshown 1,0,0,0,400,0,0,0,300,0,m--- 2,0,100,0,600,0,60,0,500,1,m--- 3,0,300,0,600,0,260,0,500,0,m-r- 4,0,500,0,0,0,490,0,0,0,---- > "$d/onesprint.csv"
	o=$(sj 2 "$d/onesprint.csv"); t "sprint_judge: one sprinting sample, not shown yet, is RIG and not FAIL" "$(grep -c '^RIG .*too few sprinting samples (1' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0
	o=$(sj 3 "$d/falsesprint.csv"); t "sprint_judge: no sprinting sample is RIG" "$(grep -c '^RIG .*never read 1' <<<"$o"),$(grep -c '^FAIL ' <<<"$o")" 1,0
	printf '%s
' i,mx,my,mz,mspeed,sx,sy,sz,sgraph,msprint,sshown 1,0,0,0,600,0,0,0,500,1,m--- 2,0,100,0,600,0,60,0,500,1,m--- 3,0,300,0,600,0,260,0,500,1,m--- 4,0,500,0,600,0,460,0,500,1,m-r- 5,0,700,0,600,0,660,0,500,1,m--- > "$d/sprintnotshown.csv"
	o=$(sj 4 "$d/sprintnotshown.csv"); t "sprint_judge: 5 samples, 20% shown, fails" "$(grep -c '^FAIL .*sprints while its friend sprints.*20' <<<"$o")" 1
	# the step list: every step has its function and its line
	for s in $STEPS; do
		declare -F "step_$s" >/dev/null || { echo "FAIL step $s has no step_$s"; bad=1; }
		[ -n "${STEP_INFO[$s]:-}" ] || { echo "FAIL step $s has no STEP_INFO line"; bad=1; }
	done
	echo "ok   every step has a function and a line"
	[ $bad -eq 0 ] && echo "steps.sh selftest: all ok" || echo "steps.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
