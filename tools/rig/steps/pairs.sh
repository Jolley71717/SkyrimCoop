#!/bin/bash
# steps/pairs.sh: the rig step `pairs` (protocol 24, docs/DESIGN.md "Kill moves and paired animations"), sourced by
# steps.sh. A kill move (or any paired animation) begun by a game's player or an NPC it runs is said once as a pair, and
# the other game plays the same pair on the actors that show both there (the stand-in, the copies), holds them while it
# plays and lets them go after; its graph events are never replayed as actions; the victim dies once, its runner's
# death, shown in both. Not run in a game yet.
#
#   step_pairs     the step
#   bash tools/rig/steps/pairs.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover's game runs the actors, the watcher's shows them), both players near each other, after
# npcspells:
#   0. Set-up (said in the run): a person the mover runs, neither essential nor protected, that the watcher shows as a
#      copy (npccombat.sh's combat_foes and combat_pick, people first; none is a rig fault). A kill move's idle: PAIRS_IDLE
#      when set, else the mover's `pairs find kill` (the lowest-id paired idle whose event holds "KillMove"; none is a
#      rig fault). The victim brought to PAIRS_LOW of its health with the mover's console (`damageav`, as a blow would).
#      Then the injected attack: the mover's dev channel plays the idle on its player with the victim as its target
#      (`pairs play <idle> <victim>`: the engine's own paired idle, as a kill move the game chose); refused is a rig
#      fault.
#   1. Said: within PAIRS_WAIT the mover said the pair (`pairs` said= up; `pairs last`: said attacker=player:<mover>
#      victim=npc:<key> ... kind=kill or sneak). Not said is a FAIL.
#   2. Played: within PAIRS_WAIT the watcher played it on the stand-in and the copy (`pairs` played= up; `pairs last`:
#      played attacker=player:<mover> victim=npc:<key>). Not played is a FAIL, with what the watcher refused it for.
#   3. Took: within PAIRS_WAIT either actor is in the kill move in the watcher's game (took= up). Played and never in it
#      (noTake= up) is a FAIL.
#   4. Not replayed: the mover's `actions` byKind= killmove: did not rise (its graph's kill move events stayed in its
#      game: eventsKept= said). Up is a FAIL: the stand-in would play the halves again.
#   5. Not said back: the watcher said no pair (its said= unchanged: no copy or stand-in is its own).
#   6. One death: within PAIRS_DEATH_WAIT the mover's NPC is dead (dead=1) and the watcher's copy too, and both games
#      name the mover's player its killer. The copy dead while its runner's NPC lives, or the runner's dead and the copy
#      not, is a FAIL; neither dead is a WARN (the idle played without its kill: the engine's choice, not the sync's).
#   7. Let go: within PAIRS_HOLD_WAIT (the longest hold, MAX_HOLD 8 s, and more) the watcher holds no actor (heldNow=0;
#      ended= or noTake= up); held the whole MAX_HOLD (gaveUp= up: the kill move flag never fell) is a WARN; still held a
#      FAIL. Then the stand-in is placed by its states again: within PAIRS_WAIT the watcher's `standins` has it within
#      PAIRS_PLACED of where its player is drawn (at= to target=). Further is a FAIL.
#   Then an NPC's kill move (pairs_npc_way): the mover's dev channel plays the idle on one person it runs with another
#   it runs as the victim (`pairs play <idle> <victim> <attacker>`); 1, 2, 3, 5, 6 (no killer named: an NPC's) and 7
#   (no stand-in) as above, the attacker npc:<key>. Fewer than two such people is a WARN (that leg is not run).
#   Put back after each leg: the victim raised if dead (`resurrect 1`), its health restored, both out of combat, and the
#   mover's bounty in the hold paid (`paycrimegold`: a kill there is a crime).
# Not on the rig yet: a creature's kill move (a wolf's, a bear's, a dragon's bite-and-throw: one of the load order's
# creature idles on a creature the mover runs), a sneak kill (the sneak flag), a vampire's feeding (a pair that kills
# no one, held FEED_HOLD), a kill move on a stand-in (no actor here fights one), a kill move the game chose itself in a
# real fight (the injected idle stands for it).

PAIRS_WAIT=${PAIRS_WAIT:-6}                # s: for the pair to be said, played, taken, and the stand-in placed again
PAIRS_DEATH_WAIT=${PAIRS_DEATH_WAIT:-12}   # s: for the victim to die in both games (the move takes about five)
PAIRS_HOLD_WAIT=${PAIRS_HOLD_WAIT:-12}     # s: for the watcher to let the actors go (PairRules' MAX_HOLD 8 s, and more)
PAIRS_PLACED=${PAIRS_PLACED:-150}          # u: the stand-in this near where its player is drawn, once let go
PAIRS_LOW=${PAIRS_LOW:-0.1}                # the share of its health the victim is brought to first
PAIRS_IDLE=${PAIRS_IDLE:-}                 # a kill move idle's id to play (hex); empty: the mover's `pairs find kill`

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# pr_found <pairs find reply>: the idle's id; nothing when none
pr_found() { [[ $1 == "idle id="* ]] && field id "$1"; }
# pr_kind <list> <kind>: how many of that kind ("attack:3,killmove:1"), 0 when none
pr_kind() { local n; n=$(tr ',' '\n' <<<"$1" | sed -n "s/^$2://p" | head -1); echo "${n:-0}"; }
# pr_said_part <pairs last reply>: what was said last ("attacker=... victim=... ..."); nothing when none
pr_said_part() { local s=${1%% | played *}; s=${s#said }; [[ $s == attacker=* ]] && echo "$s"; }
# pr_played_part <pairs last reply>: what was played last; nothing when none
pr_played_part() { local s; [[ $1 == *" | played attacker="* ]] || return 0; s=${1#* | played }; echo "$s"; }
# pr_names <part> <attacker> <victim>: that part names those sides
pr_names() { [[ $1 == "attacker=$2 victim=$3 "* ]]; }
# pr_gap <standins reply> <player>: u between that player's stand-in and where it is drawn (at= and target=); nothing
# when either is not there
pr_gap() {
	awk -v p="$2" 'BEGIN { RS = " [|] " }
		$1 == "player" && $2 == p {
			if (match($0, / at=\([-0-9.]+,[-0-9.]+,[-0-9.]+\)/)) a = substr($0, RSTART + 5, RLENGTH - 6)
			if (match($0, / target=\([-0-9.]+,[-0-9.]+,[-0-9.]+\)/)) t = substr($0, RSTART + 9, RLENGTH - 10)
			if (a != "" && t != "") { split(a, x, ","); split(t, y, ","); printf "%.0f\n", sqrt((x[1] - y[1]) ^ 2 + (x[2] - y[2]) ^ 2 + (x[3] - y[3]) ^ 2) }
			exit
		}' <<<"$1"
}
# pr_rose_n <before> <after>: both numbers and the second larger
pr_rose_n() { is_num "$1" && is_num "$2" && _cmp "$2" ">" "$1"; }

# pr_get <who> <field>: a count of that game's `pairs`
pr_get() { field "$2" "$(dc "$1" pairs)"; }
# pr_rose <who> <field> <before>: that count rose since
pr_rose() { pr_rose_n "$3" "$(pr_get "$1" "$2")"; }
# pr_said <who> <said before> <attacker> <victim>: that game said a pair since, the last one those sides
pr_said() { pr_rose "$1" said "$2" && pr_names "$(pr_said_part "$(dc "$1" 'pairs last')")" "$3" "$4"; }
# pr_played <who> <played before> <attacker> <victim>: that game played a pair since, the last one those sides
pr_played() { pr_rose "$1" played "$2" && pr_names "$(pr_played_part "$(dc "$1" 'pairs last')")" "$3" "$4"; }
# pr_dead <who> <local id>: that actor is dead in that game
pr_dead() { [ "$(combat_one "$(_combat_one "$1" "$2")" dead)" = 1 ]; }
# pr_both_dead <runner> <its NPC> <watcher> <the copy>: dead in both
pr_both_dead() { pr_dead "$1" "$2" && pr_dead "$3" "$4"; }
# pr_let_go <who>: that game holds no actor for a pair
pr_let_go() { [ "$(pr_get "$1" heldNow)" = 0 ]; }
# pr_placed <who> <player>: that player's stand-in is within PAIRS_PLACED of where it is drawn
pr_placed() { local g; g=$(pr_gap "$(dc "$1" standins)" "$2"); is_num "$g" && _cmp "$g" "<=" "$PAIRS_PLACED"; }
# pr_killmoves <who>: the kill move events that game sent as its player's actions so far
pr_killmoves() { pr_kind "$(field byKind "$(dc "$1" actions)")" killmove; }

# pr_low <who> <local id>: that NPC brought to PAIRS_LOW of its health (console set-up)
pr_low() {
	local one h m d; one=$(_combat_one "$1" "$2"); h=$(combat_one "$one" health); m=$(combat_one "$one" max)
	if is_num "$h" && is_num "$m"; then
		d=$(combat_damage_to "$h" "$m" "$PAIRS_LOW")
		[ "$d" != 0 ] && dc "$1" "console $2.damageav health $d" >/dev/null
		say "pairs: set-up (console): the $1's $2 brought to $PAIRS_LOW of its health (damageav $d of $h/$m)"
	else
		say "pairs: set-up: the $1's $2's health not read (health=${h:-?} max=${m:-?}): played at the health it has"
	fi
}

# pr_putback <mover> <victim local> [<attacker local>] [<pay 1|0>]: the victim raised if dead and at all its health,
# both out of combat, and the mover's bounty in the hold paid
pr_putback() {
	pr_dead "$1" "$2" && dc "$1" "console $2.resurrect 1" >/dev/null
	dc "$1" "console $2.restoreav health $COMBAT_RESTORE" >/dev/null
	dc "$1" "console $2.stopcombat" >/dev/null
	[ -n "${3:-}" ] && dc "$1" "console $3.stopcombat" >/dev/null
	[ "${4:-1}" = 1 ] && dc "$1" "console player.paycrimegold 0 0 $COMBAT_CRIME_FACTION" >/dev/null
	say "pairs: put back (console): the $1's $2 alive at all its health${3:+, $3 and it} out of combat, the $1's bounty in the hold paid"
}

# pr_leg <mover> <watcher> <label> <idle> <victim local> <victim key> <copy local> <attacker local|""> <attacker side>
# <player leg 1|0>: plays the idle in the mover's game and reads both; false (a rig fault said) when the play was
# refused
pr_leg() {
	local m=$1 w=$2 label=$3 idle=$4 vl=$5 vkey=$6 wl=$7 al=$8 aside=$9 player=${10} mid r
	local ms0 ws0 wp0 wt0 we0 wg0 wn0 mk0 mk1 played=0 rdead cdead
	mid=$(field player "$(dc "$m" 'session status')")
	pr_low "$m" "$vl"
	ms0=$(pr_get "$m" said); ws0=$(pr_get "$w" said); wp0=$(pr_get "$w" played); wt0=$(pr_get "$w" took)
	we0=$(pr_get "$w" ended); wg0=$(pr_get "$w" gaveUp); wn0=$(pr_get "$w" noTake); mk0=$(pr_killmoves "$m")
	r=$(dc "$m" "pairs play $idle $vl${al:+ $al}")
	if [[ $r != played* ]]; then
		rig_fault input "$label: the $m plays the kill move $idle on $vl (dev set-up)" "$r"
		return 1
	fi
	say "$label: set-up (dev): the $m's ${al:-player} plays $idle on $vl ($vkey): $r"

	# 1. said
	if wait_until "$PAIRS_WAIT" pr_said "$m" "$ms0" "$aside" "npc:$vkey"; then
		pass "$label: the $m says the pair" "said $ms0 -> $(pr_get "$m" said): $(pr_said_part "$(dc "$m" 'pairs last')" | cut -c1-160)"
	else
		fail "$label: the $m says the pair" "said $ms0 -> $(pr_get "$m" said), last: $(dc "$m" 'pairs last' | cut -c1-200); $(dc "$m" pairs | cut -d'|' -f1 | cut -c1-260)"
	fi
	# 2. played
	if wait_until "$PAIRS_WAIT" pr_played "$w" "$wp0" "$aside" "npc:$vkey"; then
		played=1
		pass "$label: the $w plays it on the actors that show both" "played $wp0 -> $(pr_get "$w" played): $(pr_played_part "$(dc "$w" 'pairs last')" | cut -c1-180)"
	else
		fail "$label: the $w plays it on the actors that show both" "played $wp0 -> $(pr_get "$w" played) heard=$(pr_get "$w" heard) refused=$(pr_get "$w" refused) failed=$(pr_get "$w" failed) late=$(pr_get "$w" late); last: $(dc "$w" 'pairs last' | cut -c1-160)"
	fi
	# 3. took
	if [ "$played" = 1 ]; then
		if wait_until "$PAIRS_WAIT" pr_rose "$w" took "$wt0"; then
			pass "$label: the $w's actors are in it" "took $wt0 -> $(pr_get "$w" took); $(dc "$w" 'pairs list' | cut -c1-200)"
		else
			fail "$label: the $w's actors are in it" "took $wt0 -> $(pr_get "$w" took), noTake $wn0 -> $(pr_get "$w" noTake): the engine played neither half"
		fi
	fi
	# 4. not replayed (the player's leg: its graph's events are the ones actions would send)
	if [ "$player" = 1 ]; then
		mk1=$(pr_killmoves "$m")
		if [ "$mk1" = "$mk0" ]; then
			pass "$label: the $m sends none of the kill move's events as actions" "killmove $mk0 -> $mk1, eventsKept=$(pr_get "$m" eventsKept)"
		else
			fail "$label: the $m sends none of the kill move's events as actions" "killmove $mk0 -> $mk1: the stand-in plays the halves again"
		fi
	fi
	# 5. not said back
	expect_eq "$label: the $w says no pair of its own back" "$(pr_get "$w" said)" "$ws0"
	# 6. one death
	wait_until "$PAIRS_DEATH_WAIT" pr_both_dead "$m" "$vl" "$w" "$wl"
	rdead=0; cdead=0; pr_dead "$m" "$vl" && rdead=1; pr_dead "$w" "$wl" && cdead=1
	if [ "$rdead$cdead" = 11 ]; then
		pass "$label: the victim dies once, in both games" "the $m's $(_combat_one "$m" "$vl" | grep -o 'dead=[01]\|sessionDead=[01] killer=[0-9]*' | tr '\n' ' ')/ the $w's copy $(_combat_one "$w" "$wl" | grep -o 'dead=[01]\|sessionDead=[01]' | tr '\n' ' ')"
	elif [ "$rdead$cdead" = 10 ]; then
		fail "$label: the victim dies once, in both games" "the $m's NPC is dead, the $w's copy lives after $PAIRS_DEATH_WAIT s: $(_combat_one "$w" "$wl" | cut -c1-200)"
	elif [ "$rdead$cdead" = 01 ]; then
		fail "$label: the victim dies once, in both games" "the $w's copy died while the $m's NPC lives: a death of the $w's own"
	else
		warn "$label: the victim dies once, in both games" "neither died in $PAIRS_DEATH_WAIT s: the idle played without its kill (hp $(combat_one "$(_combat_one "$m" "$vl")" hp))"
	fi
	if [ "$player" = 1 ]; then
		[ "$rdead" = 1 ] && expect_eq "$label: the $m's game names its player the killer" "$(combat_one "$(_combat_one "$m" "$vl")" killer)" "$mid"
		[ "$cdead" = 1 ] && expect_eq "$label: the $w's game names the $m's player the killer" "$(combat_one "$(_combat_one "$w" "$wl")" killer)" "$mid"
	else
		say "$label: killers: the $m's $(combat_one "$(_combat_one "$m" "$vl")" killer), the $w's $(combat_one "$(_combat_one "$w" "$wl")" killer) (an NPC's kill names no player)"
	fi
	# 7. let go, the stand-in placed again
	if [ "$played" = 1 ]; then
		if wait_until "$PAIRS_HOLD_WAIT" pr_let_go "$w"; then
			if pr_rose "$w" gaveUp "$wg0"; then
				warn "$label: the $w lets the actors go" "held the whole MAX_HOLD (gaveUp $wg0 -> $(pr_get "$w" gaveUp)): the kill move flag never fell there"
			else
				pass "$label: the $w lets the actors go" "heldNow=0, ended $we0 -> $(pr_get "$w" ended), noTake $wn0 -> $(pr_get "$w" noTake), released=$(pr_get "$w" released)"
			fi
		else
			fail "$label: the $w lets the actors go" "still held after $PAIRS_HOLD_WAIT s: $(dc "$w" 'pairs list' | cut -c1-240)"
		fi
		if [ "$player" = 1 ]; then
			if wait_until "$PAIRS_WAIT" pr_placed "$w" "$mid"; then
				pass "$label: the $w's stand-in is placed by its states again" "$(pr_gap "$(dc "$w" standins)" "$mid") u from where it is drawn"
			else
				fail "$label: the $w's stand-in is placed by its states again" "$(pr_gap "$(dc "$w" standins)" "$mid") u from where it is drawn after $PAIRS_WAIT s (at most $PAIRS_PLACED)"
			fi
		fi
	fi
	return 0
}

# pr_idle <mover> <label>: the kill move idle to play (PAIRS_IDLE, else the mover's `pairs find kill`); a rig fault
# said and nothing when none
pr_idle() {
	local r
	if [ -n "$PAIRS_IDLE" ]; then echo "$PAIRS_IDLE"; return; fi
	r=$(dc "$1" 'pairs find kill')
	pr_found "$r" || rig_fault state "$2: a kill move idle in the load order (set-up)" "$r" >&2
}

# pairs_way <mover> <watcher>: the mover's player's kill move on a person the mover runs
pairs_way() {
	local m=$1 w=$2 label="pairs ($1's player kills, $2 watches)" mid foes idle
	section "pairs: the $m's player's kill move on a person the $m runs, shown in the $w"
	mid=$(field player "$(dc "$m" 'session status')")
	foes=$(combat_foes "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")" human)
	if ! combat_pick "$m" "$foes" "$label"; then
		rig_fault state "$label: a person the $m runs, neither essential nor protected, that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"
		return 0
	fi
	[ "$COMBAT_KIND" = human ] || say "$label: no person: the $COMBAT_KIND $COMBAT_KEY is the victim (a person's kill move may not fit it)"
	idle=$(pr_idle "$m" "$label") || true
	[ -n "$idle" ] || return 0
	local key=$COMBAT_KEY rl=$COMBAT_R wl=$COMBAT_W
	say "$label: set-up: the victim $key (the $m's $rl, the $w's copy $wl), the idle $idle"
	pr_leg "$m" "$w" "$label" "$idle" "$rl" "$key" "$wl" "" "player:$mid" 1 || true
	pr_putback "$m" "$rl"
}

# pairs_npc_way <mover> <watcher>: a person the mover runs kills another it runs
pairs_npc_way() {
	local m=$1 w=$2 label="pairs ($1's NPC kills, $2 watches)" mid foes idle k r cw kd one n=0
	local ak="" al="" vk="" vl="" vw=""
	section "pairs: an NPC's kill move on another, both the $m's, shown in the $w"
	mid=$(field player "$(dc "$m" 'session status')")
	foes=$(combat_foes "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")" human)
	while read -r k r cw kd; do
		[ -n "$k" ] && [ "$kd" = human ] || continue
		one=$(_combat_one "$m" "$r")
		[ "$(combat_one "$one" essential)" = 0 ] && [ "$(combat_one "$one" protected)" != 1 ] || continue
		if [ $n = 0 ]; then ak=$k; al=$r; else vk=$k; vl=$r; vw=$cw; break; fi
		n=$((n + 1))
	done <<<"$foes"
	if [ -z "$vk" ]; then
		warn "$label: two people the $m runs that the $w shows" "fewer than two, neither essential nor protected: an NPC's kill move is not tried"
		return 0
	fi
	idle=$(pr_idle "$m" "$label") || true
	[ -n "$idle" ] || return 0
	say "$label: set-up: $ak (the $m's $al) kills $vk (the $m's $vl, the $w's copy $vw) with $idle"
	pr_leg "$m" "$w" "$label" "$idle" "$vl" "$vk" "$vw" "$al" "npc:$ak" 0 || true
	pr_putback "$m" "$vl" "$al"
}

step_pairs() {
	local g
	for g in host vm; do still $g; done
	pairs_way vm host
	pairs_way host vm
	pairs_npc_way vm host
	pairs_npc_way host vm
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init pairstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# replies in the formats the plugin writes (Pairs' Command, RemotePlayers' StandIns)
	P='pairs ours attackerEvents=3 victimEvents=1 endEvents=1 said=2 byEvent=2 byFlag=0 saidKinds=kill:2 coalesced=1 outboxDropped=0 ambiguous=0 notPaired=0 noPartner=0 victimStandIn=0 unshared=0 gone=0 notedLost=0 eventsKept=14 | theirs heard=1 played=1 playedKinds=kill:1 refused=far:1 failed=0 second=0 took=1 ended=1 noTake=0 gaveUp=0 released=0 playedOnOurs=0 heldNow=0 waiting=0 late=0 dropped=0 | idles=40 events=38'
	L='said attacker=player:2 victim=npc:0001A67C idle=000E1E6D event=pa_KillMove1HMDecap kind=kill | played attacker=npc:00023456 victim=npc:FF000801@1 idle=000E1E6D event=pa_KillMove1HMDecap kind=kill on FF000AAA and 0001A67C'
	t "get: ours" "$(field said "$P")" 2
	t "get: theirs" "$(field played "$P")" 1
	t "get: not a kind's" "$(field playedKinds "$P")" kill:1
	t "get: held now" "$(field heldNow "$P")" 0
	t "found: the id" "$(pr_found 'idle id=000E1E6D event=pa_KillMove1HMDecap editor=KillMove1HMDecap of=12')" 000E1E6D
	t "found: none" "$(pr_found 'none of=0 idles=0')" ""
	t "kind: killmove" "$(pr_kind attack:3,killmove:2 killmove)" 2
	t "kind: none" "$(pr_kind none killmove)" 0
	t "said part" "$(pr_said_part "$L")" "attacker=player:2 victim=npc:0001A67C idle=000E1E6D event=pa_KillMove1HMDecap kind=kill"
	t "said part: none" "$(pr_said_part 'said none | played none')" ""
	t "played part" "$(pr_played_part "$L" | cut -d' ' -f1,2)" "attacker=npc:00023456 victim=npc:FF000801@1"
	t "played part: none" "$(pr_played_part 'said none | played none')" ""
	t "names: those sides" "$(pr_names "$(pr_said_part "$L")" player:2 npc:0001A67C && echo yes)" yes
	t "names: another victim" "$(pr_names "$(pr_said_part "$L")" player:2 npc:0001A67D || echo no)" no
	t "names: a spawn" "$(pr_names "$(pr_played_part "$L")" npc:00023456 npc:FF000801@1 && echo yes)" yes
	S='standins n=2 | player 2 standin=FF000AAA placed=1 dressed=1 at=(100,200,0) heading=1.500 target=(130,240,0) speed=0 dir=0.000 flags=0x00 graphSpeed=0 | player 3 standin=FF000BBB placed=0 dressed=0 at=none target=none'
	t "gap: at to target" "$(pr_gap "$S" 2)" 50
	t "gap: none placed" "$(pr_gap "$S" 3)" ""
	t "gap: not listed" "$(pr_gap "$S" 4)" ""
	t "rose" "$(pr_rose_n 1 2 && echo yes)" yes
	t "rose: not equal" "$(pr_rose_n 2 2 || echo no)" no

	# The VM runs two people (0001A67C, nearest, and 0001A6A0), the host shows their copies; the VM's player is player 2.
	# FP_MODE: ok; nopick (no person: the rig's; and no NPC leg: a WARN); lonely (one person: the NPC leg a WARN);
	# noidle (no kill move idle: the rig's, both legs); refused (the engine refuses the play: the rig's, both legs); nosay
	# (the VM says nothing: a FAIL each leg); noplay (the host plays nothing: a FAIL each leg, no took or let-go read);
	# notake (played, never in it: a FAIL each leg); replayed (the VM's player sent kill move actions: a FAIL); saidback
	# (the host said one: a FAIL each leg); nodeath (neither dies: a WARN each leg); copyalive (the copy never dies: a
	# FAIL each leg); gaveup (held the whole MAX_HOLD: a WARN each leg); stuck (never let go: a FAIL each leg, and the
	# stand-in not placed); unplaced (the stand-in left where the pair put it: a FAIL).
	fake_pr() {
		FP=$d/fp.$RANDOM; mkdir -p "$FP"; FP_MODE=$1
		local f; for f in msaid wsaid played took ended gaveup notake held mkill kept rdead wdead killer; do echo 0 > "$FP/$f"; done
		echo 0 > "$FP/gap"; : > "$FP/undo"
		fp_inc() { echo $(( $(cat "$FP/$1") + ${2:-1} )) > "$FP/$1"; }
		fp_set() { echo "$2" > "$FP/$1"; }
		fp_play() {  # the VM plays the idle: $1 the attacker (empty: its player)
			[ "$FP_MODE" = refused ] && { echo "refused: the engine would not play it"; return; }
			[ "$FP_MODE" = nosay ] || fp_inc msaid
			fp_inc kept 6
			[ "$FP_MODE" = replayed ] && fp_inc mkill 2
			[ "$FP_MODE" = saidback ] && fp_inc wsaid
			if [ "$FP_MODE" != noplay ]; then
				fp_inc played
				case "$FP_MODE" in
					notake) fp_inc notake ;;
					gaveup) fp_inc took; fp_inc gaveup ;;
					stuck) fp_inc took; fp_set held 2; fp_set gap 400 ;;
					*) fp_inc took; fp_inc ended ;;
				esac
			fi
			[ "$FP_MODE" = unplaced ] && fp_set gap 400
			if [ "$FP_MODE" != nodeath ]; then
				fp_set rdead 1; fp_set killer "$([ -z "$1" ] && echo 2 || echo 0)"
				[ "$FP_MODE" = copyalive ] || fp_set wdead 1
			fi
			echo "played idle=000E1E6D event=pa_KillMove1HMDecap attacker=${1:-00000014} victim=$2"
			echo "${1:-player} $2" > "$FP/last"
		}
		fp_report() {  # $1: vm or host
			local said; [ "$1" = vm ] && said=$(cat "$FP/msaid") || said=$(cat "$FP/wsaid")
			if [ "$1" = vm ]; then
				echo "pairs ours attackerEvents=1 victimEvents=0 endEvents=0 said=$said byEvent=$said byFlag=0 saidKinds=kill:$said coalesced=0 outboxDropped=0 ambiguous=0 notPaired=0 noPartner=0 victimStandIn=0 unshared=0 gone=0 notedLost=0 eventsKept=$(cat "$FP/kept") | theirs heard=0 played=0 playedKinds=none refused=none failed=0 second=0 took=0 ended=0 noTake=0 gaveUp=0 released=0 playedOnOurs=0 heldNow=0 waiting=0 late=0 dropped=0 | idles=40 events=38"
			else
				echo "pairs ours attackerEvents=0 victimEvents=0 endEvents=0 said=$said byEvent=0 byFlag=0 saidKinds=none coalesced=0 outboxDropped=0 ambiguous=0 notPaired=0 noPartner=0 victimStandIn=0 unshared=0 gone=0 notedLost=0 eventsKept=0 | theirs heard=$(cat "$FP/msaid") played=$(cat "$FP/played") playedKinds=kill:$(cat "$FP/played") refused=$([ "$FP_MODE" = noplay ] && echo far:1 || echo none) failed=0 second=0 took=$(cat "$FP/took") ended=$(cat "$FP/ended") noTake=$(cat "$FP/notake") gaveUp=$(cat "$FP/gaveup") released=0 playedOnOurs=0 heldNow=$(cat "$FP/held") waiting=0 late=0 dropped=0 | idles=40 events=38"
			fi
		}
		fp_last() {  # $1: vm or host
			local a v; read -r a v < "$FP/last" 2>/dev/null || { echo "said none | played none"; return; }
			[ "$a" = player ] && a=player:2 || a=npc:$a
			if [ "$1" = vm ]; then echo "said attacker=$a victim=npc:$v idle=000E1E6D event=pa_KillMove1HMDecap kind=kill | played none"
			else echo "said none | played attacker=$a victim=npc:$v idle=000E1E6D event=pa_KillMove1HMDecap kind=kill on FF000AAA and $v"; fi
		}
		fp_one() {  # $1: vm or host, $2: the NPC
			local dd=0 k=0
			if [ "$2" = "$(cut -d' ' -f2 "$FP/last" 2>/dev/null)" ]; then
				[ "$1" = vm ] && dd=$(cat "$FP/rdead") || dd=$(cat "$FP/wdead")
				[ "$dd" = 1 ] && k=$(cat "$FP/killer")
			fi
			echo "npcs $2 | $2 local=$2 owner=2 role=$([ "$1" = vm ] && echo run || echo copy) kind=human at=(100,200,0) dead=$dd child=0 weapon=0 combat=0 hp=0.100 sessionDead=$dd killer=$k confidence=1 aggression=0 health=150 max=200 essential=0 protected=0"
		}
		dc() { case "$1 $2" in
			"vm session status") echo "role=client connection=up welcomed=1 player=2" ;;
			"host session status") echo "role=host connection=up welcomed=1 player=1" ;;
			"vm where"|"host where") echo "cell=0000A1B2 world=0000003C pos=(100,200,0) modal=0" ;;
			"vm npcs list")
				case "$FP_MODE" in
					nopick) echo "npcs=0" ;;
					lonely) echo 'npcs=1 | 0001A67C local=0001A67C owner=2 role=run kind=human at=(100,200,0) dead=0 child=0' ;;
					*) echo 'npcs=2 | 0001A67C local=0001A67C owner=2 role=run kind=human at=(100,200,0) dead=0 child=0 | 0001A6A0 local=0001A6A0 owner=2 role=run kind=human at=(300,200,0) dead=0 child=0' ;;
				esac ;;
			"host npcs list") echo 'npcs=2 | 0001A67C local=0001A67C owner=2 role=copy kind=human at=(101,200,0) dead=0 child=0 runner=2 mode=mirror | 0001A6A0 local=0001A6A0 owner=2 role=copy kind=human at=(301,200,0) dead=0 child=0 runner=2 mode=mirror' ;;
			"vm npcs 0001A"*|"host npcs 0001A"*) fp_one "$1" "${2#npcs }" ;;
			"vm npcs") echo "npcs run=2 copies=0" ;;
			"vm pairs find kill") [ "$FP_MODE" = noidle ] && { echo "none of=0 idles=0"; return; }; echo "idle id=000E1E6D event=pa_KillMove1HMDecap editor=KillMove1HMDecap of=12" ;;
			"vm pairs play 000E1E6D "*) set -- $2; fp_play "${5:-}" "$4" ;;
			"vm pairs"|"host pairs") fp_report "$1" ;;
			"vm pairs last"|"host pairs last") fp_last "$1" ;;
			"host pairs list") echo "held=$(cat "$FP/held")" ;;
			"vm actions") echo "actions hooked=1 recording=1 heard=9 byKind=attack:3,killmove:$(cat "$FP/mkill") sent=9 dropped=0" ;;
			"host standins") echo "standins n=1 | player 2 standin=FF000AAA placed=1 dressed=1 at=(100,200,0) heading=1.500 target=($((100 + $(cat "$FP/gap"))),200,0) speed=0" ;;
			"vm console"*) echo "${2#console }" >> "$FP/undo"
				case "$2" in *resurrect*) fp_set rdead 0; fp_set wdead 0; fp_set killer 0 ;; esac; echo ran ;;
			*" console"*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fp$RANDOM; mkdir -p "$A_OUT"; a_init fp >/dev/null
			{ pairs_way vm host; pairs_npc_way vm host; } >/dev/null 2>&1
			cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' | sed 's/ $//'
			[ -n "${FP_SHOW:-}" ] && echo -n " undo=$(grep -v damageav "$FP/undo" | paste -sd, -)" )
	}
	# the player's leg: said, played, took, not replayed, not said back, one death, two killers, let go, placed (10);
	# the NPC's: said, played, took, not said back, one death, let go (6)
	t "way: all of it" "$(fake_pr ok)" "PASS:16"
	t "way: no person is the rig's, and the NPC leg a WARN" "$(fake_pr nopick)" "RIG:1 WARN:1"
	t "way: one person, the NPC leg a WARN" "$(fake_pr lonely)" "PASS:10 WARN:1"
	t "way: no idle is the rig's, both legs" "$(fake_pr noidle)" "RIG:2"
	t "way: a refused play is the rig's, both legs" "$(fake_pr refused)" "RIG:2"
	t "way: a pair not said fails" "$(fake_pr nosay)" "FAIL:2 PASS:14"
	t "way: a pair not played fails, neither took nor let go read" "$(fake_pr noplay)" "FAIL:2 PASS:9"
	t "way: a pair played that never took fails" "$(fake_pr notake)" "FAIL:2 PASS:14"
	t "way: the kill move's events sent as actions fail" "$(fake_pr replayed)" "FAIL:1 PASS:15"
	t "way: a pair said back fails" "$(fake_pr saidback)" "FAIL:2 PASS:14"
	t "way: no death is a WARN, no killer read" "$(fake_pr nodeath)" "PASS:12 WARN:2"
	t "way: a copy that lives on fails" "$(fake_pr copyalive)" "FAIL:2 PASS:13"
	t "way: held the whole MAX_HOLD is a WARN" "$(fake_pr gaveup)" "PASS:14 WARN:2"
	t "way: never let go fails, and the stand-in unplaced" "$(fake_pr stuck)" "FAIL:3 PASS:13"
	t "way: a stand-in left where the pair put it fails" "$(fake_pr unplaced)" "FAIL:1 PASS:15"
	t "put back: raised, restored, out of combat, the bounty paid" "$(FP_SHOW=1 fake_pr ok)" "PASS:16 undo=0001A67C.resurrect 1,0001A67C.restoreav health $COMBAT_RESTORE,0001A67C.stopcombat,player.paycrimegold 0 0 $COMBAT_CRIME_FACTION,0001A6A0.resurrect 1,0001A6A0.restoreav health $COMBAT_RESTORE,0001A6A0.stopcombat,0001A67C.stopcombat,player.paycrimegold 0 0 $COMBAT_CRIME_FACTION"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "pairs is a step" "$(case " $STEPS " in *" pairs "*) echo yes ;; esac)" yes
	t "pairs comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' pairs')" 1
	t "pairs needs a session" "$(case " $SESSION_STEPS " in *" pairs "*) echo yes ;; esac)" yes
	t "pairs runs by default" "$(case " $OPTIN_STEPS " in *" pairs "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "pairs.sh selftest: all ok" || echo "pairs.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
