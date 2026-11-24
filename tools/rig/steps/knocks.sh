#!/bin/bash
# steps/knocks.sh: the rig step `knocks` (protocol 32, docs/DESIGN.md "Knockdowns"), sourced by steps.sh. A living actor
# knocked down or ragdolled (a push, a paralysis) in the game that runs it is said down and up, and the other game knocks
# the actor that shows it there (the copy, the stand-in) the same way, holds it while it is down (kept down while its
# runner's is, its Paralysis value taken back at the up and before a save) and lets it go after; its graph events are
# never sent as actions. A knock a game gives a copy of an NPC another game runs is asked of that NPC's runner. Not run in
# a game yet.
#
#   step_knocks     the step
#   bash tools/rig/steps/knocks.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover's game runs the actor, the watcher's shows it), both players near each other, after pairs:
#   Legs: a person the mover runs (knocks_npc_way human), a creature it runs (creature: none near is a WARN), the mover's
#   player (knocks_player_way), a person it runs paralysed (knocks_para_way), and a copy in the mover's game of a person
#   the watcher runs (knocks_ask_way). The NPCs: npccombat.sh's combat_foes and combat_pick (neither essential nor
#   protected, shown by the other game as a copy); no person is a rig fault, no copy of the watcher's a WARN.
#   0. Set-up (said in the run, as the rig's injected attacks are): the mover's dev channel knocks the actor with the
#      engine's own knock (`knocks push <local> KNOCKS_MAGNITUDE`, thrown backwards) or paralyses it (`knocks paralyze
#      <local> on`: its Paralysis value, the engine's paralysis knock); refused is a rig fault.
#   1. Said: within KNOCKS_WAIT the mover said it down (`knocks` saidDown= up; `knocks last`: said actor=<side> ...
#      kind=push or paralysis). Not said is a FAIL.
#   2. Played: within KNOCKS_WAIT the watcher played it on its copy or stand-in (played= up; `knocks last`: played
#      actor=<side>). Not played is a FAIL, with what it was refused for.
#   3. Took: within KNOCKS_WAIT the watcher's actor went down (took= up). Played and never down (noTake= up) is a FAIL.
#   4. Not said back: the watcher said no down of its own (saidDown= unchanged: its copy or stand-in is not its own).
#   5. Kept here: the mover's eventsKept= rose (the knocked actor's graph events stayed in its game). Not risen is a WARN
#      (the engine may send none while an actor is down).
#   For a paralysis, then:
#   P1. Kept down: within KNOCKS_WAIT the watcher's `knocks state <copy>` says held=1 kept=1 (its Paralysis value
#       raised while its runner's NPC is down). Not is a FAIL.
#   P2. When the VM watches: the VM saves (never the host) with its copy kept down: it lives, its log says it saved, it
#       took its Paralysis value back first (savedBack= up) and keeps the copy down again within KNOCKS_WAIT after.
#       Then the set-up again: `knocks paralyze <local> off` in the mover's game.
#   6. Up: within KNOCKS_UP_WAIT the mover said it up (saidUp= up) and, when it was played, the watcher heard it
#      (upHeard= up). Not is a FAIL.
#   7. Let go: within KNOCKS_HOLD_WAIT the watcher holds nothing (heldNow=0); gaveUp= up (still down KnockRules'
#      UP_WAIT after the up, or MAX_DOWN) is a WARN; still held a FAIL.
#   8. Rest (a push): the watcher's actor came to rest within KNOCKS_REST of where its runner's did (restGapLast=, the
#      engine's ragdoll in each game). Further, or not read, is a WARN: the actor is placed by its states once let go.
#   P3. (a paralysis) no Paralysis value of the watcher's left on its copy (`knocks state` paralysis=0.0 held=0).
#   9. (the player) the watcher's stand-in placed by its states again: within KNOCKS_WAIT its `standins` has it within
#      KNOCKS_PLACED of where its player is drawn. Further is a FAIL.
#   The ask (knocks_ask_way): the mover's dev channel knocks its copy of the watcher's NPC (a knock its world gave a
#   copy, as an explosion of its player's would):
#   A1. the mover holds the copy and asks the runner (native copies= and asked= up). Not asked is a FAIL.
#   A2. the watcher applies it to its NPC (asks applied= up). Not is a FAIL, with what it was refused for.
#   A3. the watcher says its NPC down (saidDown= up, `knocks last` said actor=npc:<key>). Not is a FAIL.
#   A4. the mover's held copy joins that down (joined= up). Not is a FAIL.
#   A5. the mover lets its copy go within KNOCKS_HOLD_WAIT (heldNow=0; gaveUp= up a WARN). Still held is a FAIL.
#   Put back after each leg: the NPC's paralysis off and out of combat.
# Not on the rig yet: a dragon's or a horse's knock, a summon's, a follower's, a knock the game chose itself in a real
# fight (Unrelenting Force, a giant's club, an explosion: the injected push stands for them), a knock of a stand-in by
# the watcher's world (held, never asked), a hand-over or a death while down.

KNOCKS_WAIT=${KNOCKS_WAIT:-6}               # s: for a knock to be said, played, taken, kept, and the stand-in placed again
KNOCKS_UP_WAIT=${KNOCKS_UP_WAIT:-20}        # s: for the knocked actor to get up and say so
KNOCKS_HOLD_WAIT=${KNOCKS_HOLD_WAIT:-15}    # s: for the watcher to let it go (KnockRules' UP_WAIT 8 s, and more)
KNOCKS_REST=${KNOCKS_REST:-200}             # u: the two games' actors rest this near each other
KNOCKS_PLACED=${KNOCKS_PLACED:-150}         # u: the stand-in this near where its player is drawn, once let go
KNOCKS_MAGNITUDE=${KNOCKS_MAGNITUDE:-20}    # the push's magnitude (the engine's knock)
KNOCKS_PARA_HOLD=${KNOCKS_PARA_HOLD:-3}     # s: a paralysed NPC kept so before it is let up

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# kn_part <knocks last reply> <said|played|asked>: what that part names ("actor=... phase=... ..."); nothing when none
kn_part() { local s; s=$(tr '|' '\n' <<<"$1" | sed 's/^ *//; s/ *$//' | sed -n "s/^$2 //p" | head -1); [[ $s == actor=* ]] && echo "$s"; }
# kn_names <part> <side> <kind>: that part names that actor and kind (its down or its up)
kn_names() { [[ $1 == "actor=$2 phase="*" kind=$3 "* ]]; }
# kn_names_actor <part> <side>: that part names that actor
kn_names_actor() { [[ $1 == "actor=$2 "* ]]; }

# kn_get <who> <field>: a count of that game's `knocks`
kn_get() { field "$2" "$(dc "$1" knocks)"; }
# kn_rose <who> <field> <before>: that count rose since
kn_rose() { pr_rose_n "$3" "$(kn_get "$1" "$2")"; }
# kn_said <who> <saidDown before> <side> <kind>: that game said a knock down since, the last said that actor and kind
kn_said() { kn_rose "$1" saidDown "$2" && kn_names "$(kn_part "$(dc "$1" 'knocks last')" said)" "$3" "$4"; }
# kn_played <who> <played before> <side>: that game played a knock since, the last one that actor's
kn_played() { kn_rose "$1" played "$2" && kn_names_actor "$(kn_part "$(dc "$1" 'knocks last')" played)" "$3"; }
# kn_state <who> <local id> <field>: that field of that game's `knocks state <local id>`
kn_state() { field "$3" "$(dc "$1" "knocks state $2")"; }
# kn_kept <who> <local id>: that actor is held and kept down there
kn_kept() { [ "$(kn_state "$1" "$2" held)$(kn_state "$1" "$2" kept)" = 11 ]; }
# kn_freed <who> <local id>: that actor is not held there and has no Paralysis value of that game's
kn_freed() { [ "$(kn_state "$1" "$2" held)" = 0 ] && [ "$(kn_state "$1" "$2" paralysis)" = 0.0 ]; }
# kn_let_go <who>: that game holds no actor for a knock
kn_let_go() { [ "$(kn_get "$1" heldNow)" = 0 ]; }
# kn_up <mover> <saidUp before> <watcher> <upHeard before> <played 1|0>: the mover said the up, and the watcher heard it
# when it played the down
kn_up() { kn_rose "$1" saidUp "$2" && { [ "$5" = 0 ] || kn_rose "$3" upHeard "$4"; }; }
# kn_rested <gap>: a rest gap read and within KNOCKS_REST
kn_rested() { is_num "$1" && _cmp "$1" "<=" "$KNOCKS_REST"; }
# kn_placed <who> <player>: that player's stand-in is within KNOCKS_PLACED of where it is drawn (pairs.sh's pr_gap)
kn_placed() { local g; g=$(pr_gap "$(dc "$1" standins)" "$2"); is_num "$g" && _cmp "$g" "<=" "$KNOCKS_PLACED"; }

# kn_save <watcher> <copy local> <label>: the VM saves with its copy kept down, lives, took its value back and keeps it
# down again after
kn_save() {
	local w=$1 wl=$2 label=$3 r saves sb0
	sb0=$(kn_get "$w" savedBack)
	saves=$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { fail "$label: the VM saves with its copy kept down" "the console refused: $r"; return 1; }
	sleep 8
	if [ "$(dc vm ping)" != pong ]; then
		fail "$label: the VM's game lives through its save with a copy kept down" "its dev channel went silent after the save (a crash? see the crash scan)"
		_rig_stop "the VM's game went down" "after its save with a copy kept down"
		return 1
	fi
	pass "$label: the VM's game lives through its save with a copy kept down" "its dev channel answers"
	expect_ge "$label: the VM's log says it saved" "$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)" $((saves + 1))
	if kn_rose "$w" savedBack "$sb0"; then
		pass "$label: the VM took its Paralysis value back before the save" "savedBack $sb0 -> $(kn_get "$w" savedBack)"
	else
		fail "$label: the VM took its Paralysis value back before the save" "savedBack $sb0 -> $(kn_get "$w" savedBack): the save holds a value the mod raised"
	fi
	if wait_until "$KNOCKS_WAIT" kn_kept "$w" "$wl"; then
		pass "$label: the VM keeps its copy down again after the save" "$(dc "$w" "knocks state $wl" | cut -c1-160)"
	else
		fail "$label: the VM keeps its copy down again after the save" "$(dc "$w" "knocks state $wl" | cut -c1-160): it gets up while its runner's NPC is down"
	fi
}

# kn_leg <mover> <watcher> <label> <side> <push|paralysis> <mover's local> <watcher's local|""> <player 1|0>: knocks the
# actor in the mover's game and reads both; false (a rig fault said) when the set-up was refused
kn_leg() {
	local m=$1 w=$2 label=$3 side=$4 kind=$5 ml=$6 wl=$7 player=$8 r mid
	local ms0 mu0 me0 ws0 wp0 wt0 wn0 wu0 wg0 played=0 released=0 g
	ms0=$(kn_get "$m" saidDown); mu0=$(kn_get "$m" saidUp); me0=$(kn_get "$m" eventsKept)
	ws0=$(kn_get "$w" saidDown); wp0=$(kn_get "$w" played); wt0=$(kn_get "$w" took); wn0=$(kn_get "$w" noTake)
	wu0=$(kn_get "$w" upHeard); wg0=$(kn_get "$w" gaveUp)
	if [ "$kind" = paralysis ]; then
		r=$(dc "$m" "knocks paralyze $ml on")
		[[ $r == on* ]] || { rig_fault input "$label: the $m paralyses $ml (dev set-up)" "$r"; return 1; }
	else
		r=$(dc "$m" "knocks push $ml $KNOCKS_MAGNITUDE")
		[[ $r == pushed* ]] || { rig_fault input "$label: the $m pushes $ml (dev set-up)" "$r"; return 1; }
	fi
	say "$label: set-up (dev): the $m's $ml ($side) knocked: $r"

	# 1. said
	if wait_until "$KNOCKS_WAIT" kn_said "$m" "$ms0" "$side" "$kind"; then
		pass "$label: the $m says it down" "saidDown $ms0 -> $(kn_get "$m" saidDown): $(kn_part "$(dc "$m" 'knocks last')" said | cut -c1-160)"
	else
		fail "$label: the $m says it down" "saidDown $ms0 -> $(kn_get "$m" saidDown), last: $(dc "$m" 'knocks last' | cut -c1-200); $(dc "$m" knocks | cut -d'|' -f1 | cut -c1-200)"
	fi
	# 2. played
	if wait_until "$KNOCKS_WAIT" kn_played "$w" "$wp0" "$side"; then
		played=1
		pass "$label: the $w knocks the actor that shows it" "played $wp0 -> $(kn_get "$w" played): $(kn_part "$(dc "$w" 'knocks last')" played | cut -c1-180)"
	else
		fail "$label: the $w knocks the actor that shows it" "played $wp0 -> $(kn_get "$w" played) heard=$(kn_get "$w" heard) refused=$(kn_get "$w" refused) failed=$(kn_get "$w" failed) late=$(kn_get "$w" late); last: $(dc "$w" 'knocks last' | cut -c1-160)"
	fi
	# 3. took
	if [ "$played" = 1 ]; then
		if wait_until "$KNOCKS_WAIT" kn_rose "$w" took "$wt0"; then
			pass "$label: the $w's actor goes down" "took $wt0 -> $(kn_get "$w" took); $(dc "$w" 'knocks list' | cut -c1-200)"
		else
			fail "$label: the $w's actor goes down" "took $wt0 -> $(kn_get "$w" took), noTake $wn0 -> $(kn_get "$w" noTake): the engine's knock did not take there"
		fi
	fi
	# 4. not said back
	expect_eq "$label: the $w says no knock of its own back" "$(kn_get "$w" saidDown)" "$ws0"
	# 5. kept here
	if kn_rose "$m" eventsKept "$me0"; then
		pass "$label: the $m sends none of the knocked actor's graph events as actions" "eventsKept $me0 -> $(kn_get "$m" eventsKept)"
	else
		warn "$label: the $m sends none of the knocked actor's graph events as actions" "eventsKept $me0 -> $(kn_get "$m" eventsKept): no event was held back (none came while it was down?)"
	fi
	if [ "$kind" = paralysis ]; then
		# P1. kept down
		if [ "$played" = 1 ]; then
			if wait_until "$KNOCKS_WAIT" kn_kept "$w" "$wl"; then
				pass "$label: the $w keeps its copy down while its runner's NPC is" "$(dc "$w" "knocks state $wl" | cut -c1-160)"
			else
				fail "$label: the $w keeps its copy down while its runner's NPC is" "$(dc "$w" "knocks state $wl" | cut -c1-160) keptDown=$(kn_get "$w" keptDown) byCall=$(kn_get "$w" byCall)"
			fi
			# P2. the VM saves with it kept down
			[ "$w" = vm ] && sleep "$KNOCKS_PARA_HOLD" && kn_save "$w" "$wl" "$label"
		fi
		sleep "$KNOCKS_PARA_HOLD"
		r=$(dc "$m" "knocks paralyze $ml off")
		say "$label: set-up (dev): the $m's $ml let up: $r"
	fi
	# 6. up
	if wait_until "$KNOCKS_UP_WAIT" kn_up "$m" "$mu0" "$w" "$wu0" "$played"; then
		pass "$label: the $m says it up and the $w hears it" "saidUp $mu0 -> $(kn_get "$m" saidUp), the $w's upHeard $wu0 -> $(kn_get "$w" upHeard)"
	else
		fail "$label: the $m says it up and the $w hears it" "saidUp $mu0 -> $(kn_get "$m" saidUp), the $w's upHeard $wu0 -> $(kn_get "$w" upHeard) upUnknown=$(kn_get "$w" upUnknown) after $KNOCKS_UP_WAIT s"
	fi
	# 7. let go
	if [ "$played" = 1 ]; then
		if wait_until "$KNOCKS_HOLD_WAIT" kn_let_go "$w"; then
			released=1
			if kn_rose "$w" gaveUp "$wg0"; then
				warn "$label: the $w lets its actor go" "still down after the up (gaveUp $wg0 -> $(kn_get "$w" gaveUp)): the engine kept it down there: $(dc "$w" 'knocks holds' | cut -c1-160)"
			else
				pass "$label: the $w lets its actor go" "heldNow=0, ended=$(kn_get "$w" ended) noTake=$(kn_get "$w" noTake): $(dc "$w" 'knocks holds' | cut -c1-160)"
			fi
		else
			fail "$label: the $w lets its actor go" "still held after $KNOCKS_HOLD_WAIT s: $(dc "$w" 'knocks list' | cut -c1-240)"
		fi
	fi
	if [ "$released" = 1 ]; then
		if [ "$kind" = paralysis ]; then
			# P3. no value left
			if kn_freed "$w" "$wl"; then
				pass "$label: the $w's copy has no Paralysis value of the mod's left" "$(dc "$w" "knocks state $wl" | cut -c1-160), letUp=$(kn_get "$w" letUp)"
			else
				fail "$label: the $w's copy has no Paralysis value of the mod's left" "$(dc "$w" "knocks state $wl" | cut -c1-160): kept down for good"
			fi
		else
			# 8. rest
			g=$(kn_get "$w" restGapLast)
			if kn_rested "$g"; then
				pass "$label: both games' actors come to rest together" "$g u apart (at most $KNOCKS_REST)"
			else
				warn "$label: both games' actors come to rest together" "restGapLast=${g:-?} u (at most $KNOCKS_REST): each engine threw it its own way; it is placed by its states now"
			fi
		fi
	fi
	# 9. the stand-in placed again
	if [ "$player" = 1 ] && [ "$played" = 1 ]; then
		mid=${side#player:}
		if wait_until "$KNOCKS_WAIT" kn_placed "$w" "$mid"; then
			pass "$label: the $w's stand-in is placed by its states again" "$(pr_gap "$(dc "$w" standins)" "$mid") u from where it is drawn"
		else
			fail "$label: the $w's stand-in is placed by its states again" "$(pr_gap "$(dc "$w" standins)" "$mid") u from where it is drawn after $KNOCKS_WAIT s (at most $KNOCKS_PLACED)"
		fi
	fi
	return 0
}

# kn_putback <who> <local>: that NPC's paralysis off (only what the dev channel raised) and out of combat
kn_putback() {
	dc "$1" "knocks paralyze $2 off" >/dev/null
	dc "$1" "console $2.stopcombat" >/dev/null
	say "knocks: put back: the $1's $2 has no paralysis of the rig's and is out of combat"
}

# knocks_npc_way <mover> <watcher> <human|creature>: an NPC of that kind the mover runs, pushed
knocks_npc_way() {
	local m=$1 w=$2 kind=$3 label mid foes
	label="knocks ($1's $3 pushed, $2 watches)"
	section "knocks: a $kind the $m runs pushed down, shown in the $w"
	mid=$(field player "$(dc "$m" 'session status')")
	foes=$(combat_foes "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")" "$kind")
	if ! combat_pick "$m" "$foes" "$label" || { [ "$kind" = creature ] && [ "$COMBAT_KIND" != creature ]; }; then
		if [ "$kind" = creature ]; then
			warn "$label: a creature the $m runs, neither essential nor protected, that the $w shows" "none near: a creature's knock is not tried"
		else
			rig_fault state "$label: an NPC the $m runs, neither essential nor protected, that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"
		fi
		return 0
	fi
	[ "$COMBAT_KIND" = "$kind" ] || say "$label: no $kind: the $COMBAT_KIND $COMBAT_KEY is knocked"
	local key=$COMBAT_KEY rl=$COMBAT_R wl=$COMBAT_W
	say "$label: set-up: $key (the $m's $rl, the $w's copy $wl)"
	kn_leg "$m" "$w" "$label" "npc:$key" push "$rl" "$wl" 0 || true
	kn_putback "$m" "$rl"
}

# knocks_player_way <mover> <watcher>: the mover's player pushed
knocks_player_way() {
	local m=$1 w=$2 label="knocks ($1's player pushed, $2 watches)" mid
	section "knocks: the $m's player pushed down, its stand-in in the $w"
	mid=$(field player "$(dc "$m" 'session status')")
	kn_leg "$m" "$w" "$label" "player:$mid" push 00000014 "" 1 || true
	dc "$m" 'console player.stopcombat' >/dev/null
}

# knocks_para_way <mover> <watcher>: a person the mover runs paralysed (when the VM watches it saves meanwhile)
knocks_para_way() {
	local m=$1 w=$2 label="knocks ($1's NPC paralysed, $2 watches)" mid foes
	section "knocks: a person the $m runs paralysed, kept down in the $w"
	mid=$(field player "$(dc "$m" 'session status')")
	foes=$(combat_foes "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")" human)
	if ! combat_pick "$m" "$foes" "$label"; then
		rig_fault state "$label: an NPC the $m runs, neither essential nor protected, that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"
		return 0
	fi
	local key=$COMBAT_KEY rl=$COMBAT_R wl=$COMBAT_W
	say "$label: set-up: $key (the $m's $rl, the $w's copy $wl)"
	kn_leg "$m" "$w" "$label" "npc:$key" paralysis "$rl" "$wl" 0 || true
	kn_putback "$m" "$rl"
}

# knocks_ask_way <mover> <watcher>: the mover's copy of a person the watcher runs pushed (a knock the mover's world gave
# it): asked of the watcher, which knocks its NPC, and the mover's copy joins that knock
knocks_ask_way() {
	local m=$1 w=$2 label="knocks ($1's copy of the $2's NPC pushed)" wid foes r
	local ma0 mc0 mj0 mg0 wa0 ws0 applied=0
	section "knocks: the $m's copy of a person the $w runs pushed: asked of the $w"
	wid=$(field player "$(dc "$w" 'session status')")
	foes=$(combat_foes "$(npc_rows "$(dc "$w" 'npcs list')")" "$(npc_rows "$(dc "$m" 'npcs list')")" "$wid" "$(pos_of "$m")" human)
	if ! combat_pick "$w" "$foes" "$label"; then
		warn "$label: a person the $w runs, neither essential nor protected, that the $m shows" "none (the session gave the $w none near here): the ask is not tried; $(dc "$w" npcs | cut -c1-160)"
		return 0
	fi
	local key=$COMBAT_KEY rl=$COMBAT_R cl=$COMBAT_W
	say "$label: set-up: $key (the $w's $rl, the $m's copy $cl)"
	ma0=$(kn_get "$m" asked); mc0=$(kn_get "$m" copies); mj0=$(kn_get "$m" joined); mg0=$(kn_get "$m" gaveUp)
	wa0=$(kn_get "$w" applied); ws0=$(kn_get "$w" saidDown)
	r=$(dc "$m" "knocks push $cl $KNOCKS_MAGNITUDE")
	if [[ $r != pushed* ]]; then
		rig_fault input "$label: the $m pushes its copy $cl (dev set-up)" "$r"
		return 0
	fi
	say "$label: set-up (dev): the $m's copy $cl pushed: $r"
	# A1. held and asked
	if wait_until "$KNOCKS_WAIT" kn_rose "$m" asked "$ma0"; then
		pass "$label: the $m holds its copy and asks the $w" "copies $mc0 -> $(kn_get "$m" copies), asked $ma0 -> $(kn_get "$m" asked): $(kn_part "$(dc "$m" 'knocks last')" asked | cut -c1-160)"
	else
		fail "$label: the $m holds its copy and asks the $w" "copies $mc0 -> $(kn_get "$m" copies), asked $ma0 -> $(kn_get "$m" asked): $(dc "$m" 'knocks list' | cut -c1-200)"
	fi
	# A2. applied
	if wait_until "$KNOCKS_WAIT" kn_rose "$w" applied "$wa0"; then
		applied=1
		pass "$label: the $w knocks its NPC as asked" "applied $wa0 -> $(kn_get "$w" applied)"
	else
		fail "$label: the $w knocks its NPC as asked" "applied $wa0 -> $(kn_get "$w" applied) askHeard=$(kn_get "$w" askHeard) askRefused=$(kn_get "$w" askRefused)"
	fi
	if [ "$applied" = 1 ]; then
		# A3. said as its own
		if wait_until "$KNOCKS_WAIT" kn_said "$w" "$ws0" "npc:$key" push; then
			pass "$label: the $w says its NPC down" "saidDown $ws0 -> $(kn_get "$w" saidDown)"
		else
			fail "$label: the $w says its NPC down" "saidDown $ws0 -> $(kn_get "$w" saidDown), last: $(dc "$w" 'knocks last' | cut -c1-200)"
		fi
		# A4. joined
		if wait_until "$KNOCKS_WAIT" kn_rose "$m" joined "$mj0"; then
			pass "$label: the $m's held copy joins that down" "joined $mj0 -> $(kn_get "$m" joined)"
		else
			fail "$label: the $m's held copy joins that down" "joined $mj0 -> $(kn_get "$m" joined) refused=$(kn_get "$m" refused): $(dc "$m" 'knocks list' | cut -c1-200)"
		fi
	fi
	# A5. let go
	if wait_until "$KNOCKS_HOLD_WAIT" kn_let_go "$m"; then
		if kn_rose "$m" gaveUp "$mg0"; then
			warn "$label: the $m lets its copy go" "still down after the up (gaveUp $mg0 -> $(kn_get "$m" gaveUp))"
		else
			pass "$label: the $m lets its copy go" "heldNow=0: $(dc "$m" 'knocks holds' | cut -c1-160)"
		fi
	else
		fail "$label: the $m lets its copy go" "still held after $KNOCKS_HOLD_WAIT s: $(dc "$m" 'knocks list' | cut -c1-240)"
	fi
	dc "$w" "console $rl.stopcombat" >/dev/null
	say "knocks: put back: the $w's $rl out of combat"
}

# knocks_ways <mover> <watcher>: every leg one way
knocks_ways() {
	knocks_npc_way "$1" "$2" human
	knocks_npc_way "$1" "$2" creature
	knocks_player_way "$1" "$2"
	knocks_para_way "$1" "$2"
	knocks_ask_way "$1" "$2"
}

step_knocks() {
	local g
	for g in host vm; do still $g; done
	knocks_ways vm host
	knocks_ways host vm
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init knockstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# replies in the formats the plugin writes (Knocks' Command)
	K='knocks ours watched=1 saidDown=3 saidUp=2 saidPush=2 saidParalysis=1 launchMax=640 | theirs heard=4 played=2 playedPush=1 playedParalysis=1 inPlace=0 refused=far:1,busy:1 failed=0 joined=1 took=2 keptDown=1 byValue=1 byCall=0 letUp=1 ended=2 noTake=0 gaveUp=0 released=0 died=0 handedOver=0 holdFull=0 upHeard=2 upUnknown=1 restGapLast=35 restGapMax=80 heldNow=0 waiting=0 late=0 dropped=0 | native copies=1 standIns=0 asked=1 | asks askHeard=1 applied=1 askRefused=none | scale perMagnitude=120 learnt=1 ignored=0 speedLast=610 wantedLast=640 | savedBack=1 eventsKept=12'
	L='said actor=npc:0001A67C phase=up kind=push at=(100,200,0) speed=0 | played actor=player:2 phase=down kind=push at=(5,6,7) speed=640 on FF000AAA | asked none'
	t "get: ours" "$(field saidDown "$K")" 3
	t "get: theirs, not its kinds" "$(field played "$K")" 2
	t "get: native" "$(field copies "$K")" 1
	t "get: asks, not heard" "$(field applied "$K")" 1
	t "get: the rest gap" "$(field restGapLast "$K")" 35
	t "part: said" "$(kn_part "$L" said | cut -d' ' -f1,2,3)" "actor=npc:0001A67C phase=up kind=push"
	t "part: played" "$(kn_part "$L" played | cut -d' ' -f1)" "actor=player:2"
	t "part: none" "$(kn_part "$L" asked)" ""
	t "names: that actor and kind, its up too" "$(kn_names "$(kn_part "$L" said)" npc:0001A67C push && echo yes)" yes
	t "names: another kind" "$(kn_names "$(kn_part "$L" said)" npc:0001A67C paralysis || echo no)" no
	t "names: another actor" "$(kn_names "$(kn_part "$L" said)" npc:0001A67 push || echo no)" no
	t "names: a spawn" "$(kn_names "actor=npc:FF000801@1 phase=down kind=push at=(0,0,0) speed=5" npc:FF000801@1 push && echo yes)" yes
	t "names actor" "$(kn_names_actor "$(kn_part "$L" played)" player:2 && echo yes)" yes
	t "names actor: not a prefix" "$(kn_names_actor "actor=player:21 phase=down" player:2 || echo no)" no
	t "rested: near" "$(kn_rested 35 && echo yes)" yes
	t "rested: far" "$(kn_rested 900 || echo no)" no
	t "rested: none" "$(kn_rested none || echo no)" no

	# The VM runs a person (0001A67C, nearest) and a creature (0001A6A0), the host shows their copies; the host runs a
	# person (0001B111) the VM shows as a copy. The VM's player is player 2, the host's player 1. A dev knock plays all of
	# it at once (sleep and the waits are instant): said, played, taken, up, let go; a paralysis holds until it is off.
	# FK_MODE: ok; nosay (no down said: a FAIL each leg but the ask); noplay (the watcher plays nothing: a FAIL each leg, no
	# took, kept, let-go, rest or placed read); notake (played, never down: a FAIL each leg); saidback (the watcher said
	# one: a FAIL each leg); noevents (no event kept: a WARN each leg); noup (never up: up and let go FAIL each leg, the
	# stand-in not placed); far (rest far apart: a WARN each push leg); gaveup (held to the end: a WARN each leg);
	# unplaced (the stand-in left where it fell: a FAIL); nokeep (the paralysed copy not kept down: a FAIL); noask (the
	# copy's knock not asked: A1 and A2 FAIL); noapply (asked, not applied: a FAIL); nojoin (applied, the mover's copy
	# not joined: a FAIL); refused (every dev knock refused: the rig's, each leg); nopick (no NPCs: the person's and the
	# paralysis's the rig's, the creature's and the ask's WARNs); nocreature (none: a WARN); nosaveback (the VM saved
	# with the value: a FAIL). FK_LEGS: the legs run (default: every leg, the VM the mover).
	fake_kn() {
		FK=$d/fk.$RANDOM; mkdir -p "$FK"; FK_MODE=$1
		local g f
		for g in vm host; do
			for f in saidDown saidUp eventsKept played took noTake upHeard gaveUp ended keptDown letUp savedBack asked copies joined applied heldNow held kept paraon; do echo 0 > "$FK/$g.$f"; done
			echo 40 > "$FK/$g.restGapLast"; echo 0.0 > "$FK/$g.paralysis"; echo none > "$FK/$g.lsaid"; echo none > "$FK/$g.lplayed"; echo none > "$FK/$g.lasked"
		done
		echo 0 > "$FK/gap"; echo 0 > "$FK/saved"
		fk_inc() { echo $(( $(cat "$FK/$1") + ${2:-1} )) > "$FK/$1"; }
		fk_set() { echo "$2" > "$FK/$1"; }
		fk_get() { cat "$FK/$1"; }
		fk_other() { [ "$1" = vm ] && echo host || echo vm; }
		fk_copy() {  # $1 game, $2 local: a copy there
			case "$1 $2" in "vm 0001B111"|"host 0001A67C"|"host 0001A6A0") return 0 ;; esac; return 1
		}
		fk_side() { [ "$2" = 00000014 ] && { [ "$1" = vm ] && echo player:2 || echo player:1; } || echo "npc:$2"; }
		fk_down() {  # $1 mover, $2 local, $3 kind: the mover's knock said and played
			local m=$1 w; w=$(fk_other "$1"); local s; s=$(fk_side "$m" "$2")
			[ "$FK_MODE" = nosay ] || { fk_inc "$m.saidDown"; fk_set "$m.lsaid" "actor=$s phase=down kind=$3 at=(100,200,0) speed=500"; }
			[ "$FK_MODE" = noevents ] || fk_inc "$m.eventsKept" 3
			[ "$FK_MODE" = saidback ] && fk_inc "$w.saidDown"
			[ "$FK_MODE" = noplay ] && return 0
			fk_inc "$w.played"; fk_set "$w.lplayed" "actor=$s phase=down kind=$3 at=(100,200,0) speed=500 on FF000AAA"
			if [ "$FK_MODE" = notake ]; then fk_inc "$w.noTake"; else fk_inc "$w.took"; fi
			fk_set "$w.heldNow" 1; fk_set "$w.held" 1
			[ "$FK_MODE" = far ] && fk_set "$w.restGapLast" 600
			[ "$s" = player:2 ] && fk_set gap 400
			return 0
		}
		fk_up() {  # $1 mover, $2 local, $3 kind: the mover's actor up
			local m=$1 w; w=$(fk_other "$1"); local s; s=$(fk_side "$m" "$2")
			[ "$FK_MODE" = noup ] && return 0
			fk_inc "$m.saidUp"; fk_set "$m.lsaid" "actor=$s phase=up kind=$3 at=(110,200,0) speed=0"
			[ "$FK_MODE" = noplay ] && return 0
			fk_inc "$w.upHeard"
			[ "$(fk_get "$w.kept")" = 1 ] && fk_inc "$w.letUp"
			fk_set "$w.kept" 0; fk_set "$w.paralysis" 0.0
			fk_set "$w.heldNow" 0; fk_set "$w.held" 0; fk_inc "$w.ended"
			[ "$FK_MODE" = gaveup ] && fk_inc "$w.gaveUp"
			[ "$FK_MODE" = unplaced ] || fk_set gap 0
			return 0
		}
		fk_ask() {  # $1 mover, $2 its copy's local: held, asked, applied, said, joined, let go
			local m=$1 w; w=$(fk_other "$1")
			fk_inc "$m.copies"
			[ "$FK_MODE" = noask ] && return 0
			fk_inc "$m.asked"; fk_set "$m.lasked" "actor=npc:$2 phase=ask kind=push at=(150,200,0) speed=500"
			[ "$FK_MODE" = noapply ] && return 0
			fk_inc "$w.applied"; fk_inc "$w.saidDown"; fk_set "$w.lsaid" "actor=npc:$2 phase=down kind=push at=(150,200,0) speed=480"
			[ "$FK_MODE" = nojoin ] || fk_inc "$m.joined"
			return 0
		}
		fk_report() {
			local g=$1
			echo "knocks ours watched=1 saidDown=$(fk_get "$g.saidDown") saidUp=$(fk_get "$g.saidUp") saidPush=0 saidParalysis=0 launchMax=500 | theirs heard=1 played=$(fk_get "$g.played") playedPush=0 playedParalysis=0 inPlace=0 refused=$([ "$FK_MODE" = noplay ] && echo far:1 || echo none) failed=0 joined=$(fk_get "$g.joined") took=$(fk_get "$g.took") keptDown=$(fk_get "$g.keptDown") byValue=0 byCall=0 letUp=$(fk_get "$g.letUp") ended=$(fk_get "$g.ended") noTake=$(fk_get "$g.noTake") gaveUp=$(fk_get "$g.gaveUp") released=0 died=0 handedOver=0 holdFull=0 upHeard=$(fk_get "$g.upHeard") upUnknown=0 restGapLast=$(fk_get "$g.restGapLast") restGapMax=600 heldNow=$(fk_get "$g.heldNow") waiting=0 late=0 dropped=0 | native copies=$(fk_get "$g.copies") standIns=0 asked=$(fk_get "$g.asked") | asks askHeard=1 applied=$(fk_get "$g.applied") askRefused=$([ "$FK_MODE" = noapply ] && echo busy:1 || echo none) | scale perMagnitude=120 learnt=0 ignored=0 speedLast=none wantedLast=none | savedBack=$(fk_get "$g.savedBack") eventsKept=$(fk_get "$g.eventsKept")"
		}
		fk_one() {  # $1 game, $2 the NPC
			local role kind=human
			fk_copy "$1" "$2" && role=copy || role=run
			[ "$2" = 0001A6A0 ] && kind=creature
			echo "npcs $2 | $2 local=$2 owner=2 role=$role kind=$kind at=(100,200,0) dead=0 child=0 weapon=0 combat=0 hp=1.000 sessionDead=0 killer=0 confidence=1 aggression=0 health=200 max=200 essential=0 protected=0"
		}
		dc() {
			local g=$1; set -- $2
			case "$g $*" in
				"vm session status") echo "role=client connection=up welcomed=1 player=2" ;;
				"host session status") echo "role=host connection=up welcomed=1 player=1" ;;
				"vm where"|"host where") echo "cell=0000A1B2 world=0000003C pos=(100,200,0) modal=0" ;;
				"vm npcs list")
					case "$FK_MODE" in
						nopick) echo "npcs=0" ;;
						nocreature) echo 'npcs=2 | 0001A67C local=0001A67C owner=2 role=run kind=human at=(100,200,0) dead=0 child=0 | 0001B111 local=0001B111 owner=1 role=copy kind=human at=(151,200,0) dead=0 child=0 runner=1 mode=mirror' ;;
						*) echo 'npcs=3 | 0001A67C local=0001A67C owner=2 role=run kind=human at=(100,200,0) dead=0 child=0 | 0001A6A0 local=0001A6A0 owner=2 role=run kind=creature at=(300,200,0) dead=0 child=0 | 0001B111 local=0001B111 owner=1 role=copy kind=human at=(151,200,0) dead=0 child=0 runner=1 mode=mirror' ;;
					esac ;;
				"host npcs list")
					case "$FK_MODE" in
						nopick) echo "npcs=0" ;;
						*) echo 'npcs=3 | 0001A67C local=0001A67C owner=2 role=copy kind=human at=(101,200,0) dead=0 child=0 runner=2 mode=mirror | 0001A6A0 local=0001A6A0 owner=2 role=copy kind=creature at=(301,200,0) dead=0 child=0 runner=2 mode=mirror | 0001B111 local=0001B111 owner=1 role=run kind=human at=(150,200,0) dead=0 child=0' ;;
					esac ;;
				"vm npcs 0001"*|"host npcs 0001"*) fk_one "$g" "$2" ;;
				"vm npcs"|"host npcs") echo "npcs run=2 copies=1" ;;
				"vm ping"|"host ping") echo pong ;;
				*" knocks push "*)
					[ "$FK_MODE" = refused ] && { echo "refused: the actor has no process"; return; }
					if fk_copy "$g" "$3"; then fk_ask "$g" "$3"; else fk_down "$g" "$3" push; fk_up "$g" "$3" push; fi
					echo "pushed id=$3 magnitude=$4.0 from=(0,0,0)" ;;
				*" knocks paralyze "*" on")
					[ "$FK_MODE" = refused ] && { echo "refused: the actor is dead or not loaded"; return; }
					fk_down "$g" "$3" paralysis; fk_set "$g.paraon" 1
					local w; w=$(fk_other "$g")
					if [ "$FK_MODE" != noplay ] && [ "$FK_MODE" != nokeep ]; then fk_inc "$w.keptDown"; fk_set "$w.kept" 1; fk_set "$w.paralysis" 1.0; fi
					echo "on id=$3 paralysis=1.0 knock=1" ;;
				*" knocks paralyze "*" off")
					if [ "$(fk_get "$g.paraon")" = 1 ]; then fk_set "$g.paraon" 0; fk_up "$g" "$3" paralysis; fi
					echo "off id=$3 raised=1 paralysis=0.0" ;;
				*" knocks state "*) echo "state id=$3 knock=0 phase=standing resting=0 paralysis=$(fk_get "$g.paralysis") dead=0 held=$(fk_get "$g.held") kept=$(fk_get "$g.kept") at=(0,0,0)" ;;
				*" knocks last") echo "said $(fk_get "$g.lsaid") | played $(fk_get "$g.lplayed") | asked $(fk_get "$g.lasked")" ;;
				*" knocks list") echo "held=$(fk_get "$g.heldNow")" ;;
				*" knocks holds") echo "holds none" ;;
				*" knocks") fk_report "$g" ;;
				"host standins") echo "standins n=1 | player 2 standin=FF000AAA placed=1 dressed=1 at=(100,200,0) heading=1.500 target=($((100 + $(fk_get gap))),200,0) speed=0" ;;
				"vm standins") echo "standins n=1 | player 1 standin=FF000BBB placed=1 dressed=1 at=(100,200,0) heading=1.500 target=($((100 + $(fk_get gap))),200,0) speed=0" ;;
				"vm console save "*) fk_set saved 1; [ "$FK_MODE" = nosaveback ] || fk_inc vm.savedBack; echo ran ;;
				*" console "*) echo ran ;;
				*) echo ok ;;
			esac
		}
		log_since_mark() { [ "$(cat "$FK/saved")" = 1 ] && echo "Save: saving \"$SAVE_NAME\""; return 0; }
		( A_OUT=$d/fk$RANDOM; mkdir -p "$A_OUT"; a_init fk >/dev/null
			eval "${FK_LEGS:-knocks_ways vm host}" >/dev/null 2>&1
			cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' | sed 's/ $//' )
	}
	# vm the mover: the person's push (said, played, took, not back, kept here, up, let go, rest: 8), the creature's
	# (8), the player's (8 and placed: 9), the paralysis (said, played, took, not back, kept here, kept down, up, let go,
	# no value left: 9), the ask (asked, applied, said, joined, let go: 5)
	t "ways: all of it" "$(fake_kn ok)" "PASS:39"
	t "ways: no down said fails, each leg but the ask" "$(fake_kn nosay)" "FAIL:4 PASS:35"
	t "ways: nothing played fails, nothing after it read" "$(fake_kn noplay)" "FAIL:4 PASS:21"
	t "ways: played and never down fails" "$(fake_kn notake)" "FAIL:4 PASS:35"
	t "ways: a knock said back fails" "$(fake_kn saidback)" "FAIL:4 PASS:35"
	t "ways: no event kept is a WARN" "$(fake_kn noevents)" "PASS:35 WARN:4"
	t "ways: never up fails the up and the let-go, and the stand-in unplaced" "$(fake_kn noup)" "FAIL:9 PASS:26"
	t "ways: resting far apart is a WARN" "$(fake_kn far)" "PASS:36 WARN:3"
	t "ways: held to the end is a WARN" "$(fake_kn gaveup)" "PASS:35 WARN:4"
	t "ways: a stand-in left where it fell fails" "$(fake_kn unplaced)" "FAIL:1 PASS:38"
	t "ways: a paralysed copy not kept down fails" "$(fake_kn nokeep)" "FAIL:1 PASS:38"
	t "ways: a copy's knock not asked fails, and not applied" "$(fake_kn noask)" "FAIL:2 PASS:35"
	t "ways: an ask not applied fails" "$(fake_kn noapply)" "FAIL:1 PASS:36"
	t "ways: the mover's copy not joined fails" "$(fake_kn nojoin)" "FAIL:1 PASS:38"
	t "ways: refused knocks are the rig's" "$(fake_kn refused)" "RIG:5"
	t "ways: no NPC: the person and the paralysis the rig's, the creature and the ask WARNs" "$(fake_kn nopick)" "PASS:9 RIG:2 WARN:2"
	t "ways: no creature is a WARN" "$(fake_kn nocreature)" "PASS:31 WARN:1"
	# the host the mover, the VM watching a paralysis: 9 and the save (lives, log, savedBack, kept again: 4)
	t "para, the VM watching: all of it, and its save" "$(FK_LEGS='knocks_para_way host vm' fake_kn ok)" "PASS:13"
	t "para, the VM watching: its value saved fails" "$(FK_LEGS='knocks_para_way host vm' fake_kn nosaveback)" "FAIL:1 PASS:12"
	t "player, the VM watching: all of it" "$(FK_LEGS='knocks_player_way host vm' fake_kn ok)" "PASS:9"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "knocks is a step" "$(case " $STEPS " in *" knocks "*) echo yes ;; esac)" yes
	t "knocks comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' knocks')" 1
	t "knocks needs a session" "$(case " $SESSION_STEPS " in *" knocks "*) echo yes ;; esac)" yes
	t "knocks runs by default" "$(case " $OPTIN_STEPS " in *" knocks "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "knocks.sh selftest: all ok" || echo "knocks.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
