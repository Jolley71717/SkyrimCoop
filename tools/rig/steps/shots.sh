#!/bin/bash
# steps/shots.sh: the rig step `shots` (protocol 13, docs/DESIGN.md "Projectiles"), sourced by steps.sh. Every
# projectile an actor launches is said once as it leaves and launched in the other game from the actor that shows its
# shooter there: a friend's arrows, bolts, spells of every projectile kind, dual casts and shouts from their stand-in,
# and an NPC's from its copy. What the stand-in's own replayed release would launch is ended, nothing flies twice, the
# launches of a game are ended once their flight is over and before a save, and a stand-in's shots harm no one. Not run
# in a game yet.
#
#   step_shots     the step
#   bash tools/rig/steps/shots.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover's player shoots, the watcher's stand-in of it is read), both players near each other, after the
# actions step (the mover's set-up is that step's: act_setup gives the bow, arrows and the shout again, harmless twice):
#   1. Both games hook the update of all seven projectile classes (`shots`: hooked=7/7), or nothing else is tried.
#   2. For each kind, with the mover's virtual pad (the game's own input) after its set-up from the console and the
#      developer channel (as the console sets a scene up, said in the run; each read back by actions.sh's act_equip):
#        - an arrow (the hunting bow: drawn, the right trigger held and let go);
#        - a bolt (`shots find crossbow`: a crossbow and bolts of the load order; none, without Dawnguard, is a WARN);
#        - a spell of each projectile kind the load order has (`shots find missile|flame|beam|lobber`: the lowest-id
#          aimed spell whose projectile is that kind, taught with `player.addspell`; a kind no spell launches is a
#          WARN), cast from the right hand;
#        - a dual cast (the missile spell in both hands, both triggers at once);
#        - the shout (Unrelenting Force, RB).
#      For each: the mover's game said it (its `shots` ours byKind= of that kind went up; the shout: sent=). When it
#      said none, the mover's graph is asked whether the press did anything (its `actions` heard byKind=): it did, and
#      nothing was said, is a FAIL; it did nothing is the press's (a rig fault, which ends that way). Then the
#      watcher launched that kind from its stand-in (`shots` theirs byKind=) within SHOTS_WAIT: FAIL if not. A dual cast
#      said as one (ours dual=) is launched as one (theirs dual=); none said is a WARN (the two presses may not have
#      been taken as one).
#   3. It harms no one: the mover walks up to the watcher's player with its virtual pad and looses an arrow at it; the
#      watcher launched it, and the watcher's player keeps its health (within 1).
# After both ways, in each game:
#   4. Every launch it made for a friend flew on its shooter's aim: the worst error left after the fix (`shots` aim
#      left=) is at most SHOTS_AIM degrees; none of its launches failed (launchFailed=0); every one is ended within
#      SHOTS_END s (live=0: an arrow that stuck a short while after, a rune after its life).
#   5. A save with launches of ours live (the VM only, never the host): the host casts the rune spell and the VM saves
#      while it lies: the VM lives, every launch was ended for the save (endedSave= up, live=0), no stray stand-in.
#   6. An NPC's shots, each way: a person the mover runs that the watcher shows as a copy casts the missile spell at the
#      mover's stand-in of the watcher's player (the console's `<ref>.cast <spell> <stand-in>`, set-up: the scene's
#      shot, as the harness's injected gunfire is): the mover's game said an NPC's shot (ours npc= up; nothing said
#      while a projectile was seen is a FAIL, nothing at all the console's: a rig fault), and the watcher launched it
#      from the copy (`from player=<mover>` npcLaunched= up within SHOTS_WAIT).
# Said for the record: the natives each game ended (natives ended=), its turned-back aims (aim reaimed=), late and far
# shots. Not on the rig yet: wards and barriers, a staff, a conjured creature's shots, explosions' damage.

SHOTS_WAIT=${SHOTS_WAIT:-5}        # s: for a friend's shot to be launched in the watcher's game
SHOTS_SETTLE=${SHOTS_SETTLE:-2}    # s: after a press, for the mover's game to have launched and said it
SHOTS_AIM=${SHOTS_AIM:-5}          # degrees: a launch's aim off its shooter's after the fix, at most
SHOTS_END=${SHOTS_END:-16}         # s: every launch of a game ended by then (ShotRules.h MAX_LIFE 12 s, LOBBER_LIFE 30 s
                                   # for a rune, which the save leg ends first)
SHOTS_KINDS=${SHOTS_KINDS:-"missile flame beam lobber"}

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# sh_part <shots reply> <first word>: that part (shots, ours, theirs, natives, flying, aim, hook)
sh_part() { awk -v h="$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == h { print; exit }' <<<"$1"; }
# sh_from <shots reply> <player id>: the part of the shots heard from that player's game
sh_from() { awk -v p="player=$2" 'BEGIN { RS = " [|] " } { sub(/\n$/, "") } $1 == "from" && $2 == p { print; exit }' <<<"$1"; }
# sh_get <who> <part> <field>: a field of a part of that game's `shots`
sh_get() { field "$3" "$(sh_part "$(dc "$1" shots)" "$2")"; }
# sh_count <who> <ours|theirs> <kind|any>: how many of that kind that game said (ours) or launched (theirs); any: all
sh_count() {
	if [ "$3" = any ]; then
		if [ "$2" = ours ]; then sh_get "$1" ours sent; else sh_get "$1" theirs launched; fi
	else
		act_kind "$(sh_get "$1" "$2" byKind)" "$3"
	fi
}
# sh_up <who> <ours|theirs> <kind|any> <before>: more than before
sh_up() { local n; n=$(sh_count "$1" "$2" "$3"); is_num "$n" && is_num "$4" && [ "$n" -gt "$4" ]; }
# sh_field_up <who> <part> <field> <before>
sh_field_up() { local n; n=$(sh_get "$1" "$2" "$3"); is_num "$n" && is_num "$4" && [ "$n" -gt "$4" ]; }
# sh_npc_launched <who> <runner's id>: the NPC shots that game launched for that runner's game
sh_npc_launched() { local n; n=$(field npcLaunched "$(sh_from "$(dc "$1" shots)" "$2")"); echo "${n:-0}"; }
sh_npc_up() { [ "$(sh_npc_launched "$1" "$2")" -gt "$3" ] 2>/dev/null; }
# sh_found <shots find reply> <field>: a field of what was found; nothing when none
sh_found() { [[ $1 != none* ]] && field "$2" "$1"; }
# sh_hooked <who>: all seven projectile classes hooked
sh_hooked() { [ "$(field hooked "$(sh_part "$(dc "$1" shots)" shots)")" = 7/7 ]; }
# sh_none_live <who>: no launch of that game's for a friend flies now
sh_none_live() { [ "$(sh_get "$1" flying live)" = 0 ]; }

# The last try's rig fault: the way ends there.
SH_RIG=0

# sh_try <mover> <watcher> <label> <kind|any> <graph kind> <press...>: the presses make the mover's game say a shot of that
# kind, and the watcher launches it from its stand-in of the mover. False when it did not (SH_RIG=1: the press's).
sh_try() {
	local m=$1 w=$2 label=$3 kind=$4 ak=$5 s0 l0 h0 s1 h1; shift 5
	SH_RIG=0
	s0=$(sh_count "$m" ours "$kind"); l0=$(sh_count "$w" theirs "$kind"); h0=$(act_heard "$m" "$ak")
	act_press "$m" "$@"
	sleep "$SHOTS_SETTLE"
	s1=$(sh_count "$m" ours "$kind")
	if ! is_num "$s0" || ! is_num "$s1" || [ "$s1" -le "$s0" ]; then
		h1=$(act_heard "$m" "$ak")
		if is_num "$h0" && is_num "$h1" && [ "$h1" -gt "$h0" ]; then
			fail "$label: the $m's game says the shot" "its graph heard the $ak ($h0 -> $h1), and no $kind was said ($s0 -> $s1; $(sh_part "$(dc "$m" shots)" ours | cut -c1-200))"
		else
			SH_RIG=1
			rig_fault input "$label: the $m's press" "its graph heard no $ak ($h0 -> $h1) and no $kind was said ($s0 -> $s1)"
		fi
		return 1
	fi
	pass "$label: the $m's game says the shot ($kind)" "$s0 -> $s1"
	if wait_until "$SHOTS_WAIT" sh_up "$w" theirs "$kind" "$l0"; then
		pass "$label: the $w launches it from its stand-in of the $m" "$kind $l0 -> $(sh_count "$w" theirs "$kind")"
	else
		fail "$label: the $w launches it from its stand-in of the $m" "none in $SHOTS_WAIT s ($(sh_part "$(dc "$w" shots)" theirs | cut -c1-220))"
		return 1
	fi
}

# sh_bow <mover> <watcher> <label> <weapon> <ammo> <press>: the weapon and its ammunition in hand, drawn, shot, put away
sh_bow() {
	local m=$1 w=$2 label=$3 weapon=$4 ammo=$5 press=$6
	act_equip "$m" "$w" right "$weapon" "$label: the $m holds it" || { SH_RIG=1; return 1; }
	act_equip "$m" "$w" ammo "$ammo" "$label: the $m has its ammunition" || { SH_RIG=1; return 1; }
	act_draw "$m" "$w" "$label: the $m draws it" 1 || { SH_RIG=1; return 1; }
	sh_try "$m" "$w" "$label" arrow bow "$press"
	local rc=$?
	act_draw "$m" "$w" "$label: the $m puts it away" 0
	return $rc
}

# sh_crossbow <mover> <watcher>: a crossbow and bolts of the load order (Dawnguard's), when there are any
sh_crossbow() {
	local m=$1 w=$2 r weapon ammo
	r=$(dc "$m" 'shots find crossbow'); weapon=$(sh_found "$r" weapon); ammo=$(sh_found "$r" ammo)
	if [ -z "$weapon" ] || [ -z "$ammo" ]; then
		warn "$m shoots a bolt" "no crossbow and bolt in the load order ($r): not tried"
		return 0
	fi
	dc "$m" "console player.additem $weapon 1" >/dev/null; dc "$m" "console player.additem $ammo 30" >/dev/null
	say "shots: $m given the crossbow $weapon and bolts $ammo from its console (set-up)"
	sh_bow "$m" "$w" "$m shoots a bolt" "$weapon" "$ammo" "trigger r 1 400"
}

# sh_spell_of <mover> <kind>: the spell of that kind the mover's load order has (taught to it, set-up); nothing: none
sh_spell_of() {
	local m=$1 kind=$2 r spell
	r=$(dc "$m" "shots find $kind"); spell=$(sh_found "$r" spell)
	[ -n "$spell" ] || return 1
	dc "$m" "console player.addspell $spell" >/dev/null
	echo "$spell"
}

# sh_spell <mover> <watcher> <kind>: a spell whose projectile is that kind, cast from the right hand
sh_spell() {
	local m=$1 w=$2 kind=$3 spell rc
	if ! spell=$(sh_spell_of "$m" "$kind"); then
		warn "$m casts a $kind spell" "no aimed spell of the load order launches a $kind ($(dc "$m" "shots find $kind")): not tried"
		return 0
	fi
	say "shots: $m taught $spell ($kind) from its console (set-up)"
	act_equip "$m" "$w" right "$spell" "$m readies $spell ($kind)" || { SH_RIG=1; return 1; }
	act_draw "$m" "$w" "$m raises its hands for $spell" 1 || { SH_RIG=1; return 1; }
	sh_try "$m" "$w" "$m casts $spell ($kind)" "$kind" cast "trigger r 1 1500"
	rc=$?
	act_draw "$m" "$w" "$m lowers its hands" 0
	return $rc
}

# sh_dual <mover> <watcher>: the missile spell in both hands, both triggers at once: one dual cast, said and launched as one
sh_dual() {
	local m=$1 w=$2 spell d0 l0 d1
	spell=$(sh_spell_of "$m" missile) || { warn "$m dual casts" "no missile spell in the load order: not tried"; return 0; }
	act_equip "$m" "$w" right "$spell" "$m readies $spell in the right hand" || { SH_RIG=1; return 1; }
	act_equip "$m" "$w" left "$spell" "$m readies $spell in the left hand" || { SH_RIG=1; return 1; }
	act_draw "$m" "$w" "$m raises both hands" 1 || { SH_RIG=1; return 1; }
	d0=$(sh_get "$m" ours dual); l0=$(sh_get "$w" theirs dual)
	act_press "$m" "trigger l 1 1500" "trigger r 1 1500"
	sleep "$SHOTS_SETTLE"
	d1=$(sh_get "$m" ours dual)
	if ! is_num "$d0" || ! is_num "$d1" || [ "$d1" -le "$d0" ]; then
		warn "$m dual casts $spell: the $m's game says a dual cast" "none said (dual $d0 -> $d1): the two presses may not have been taken as one"
	else
		pass "$m dual casts $spell: the $m's game says a dual cast" "dual $d0 -> $d1"
		if wait_until "$SHOTS_WAIT" sh_field_up "$w" theirs dual "$l0"; then
			pass "$m dual casts $spell: the $w launches it as a dual cast" "dual $l0 -> $(sh_get "$w" theirs dual)"
		else
			fail "$m dual casts $spell: the $w launches it as a dual cast" "$(sh_part "$(dc "$w" shots)" theirs | cut -c1-220)"
		fi
	fi
	act_draw "$m" "$w" "$m lowers its hands" 0
}

# sh_harm <mover> <watcher>: the mover looses an arrow at the watcher's player; it is launched there and harms no one
sh_harm() {
	local m=$1 w=$2 label="$1's arrow at the $2's player" hp0 d0 u0 hp d u l0
	act_walk_near "$m" "$w" || { SH_RIG=1; return 1; }
	act_equip "$m" "$w" right "$ACTIONS_BOW" "$label: the $m holds the bow" || { SH_RIG=1; return 1; }
	act_equip "$m" "$w" ammo "$ACTIONS_ARROW" "$label: the $m has arrows" || { SH_RIG=1; return 1; }
	act_draw "$m" "$w" "$label: the $m draws" 1 || { SH_RIG=1; return 1; }
	read -r hp0 _ _ _ d0 u0 <<<"$(act_harm_read "$w")"
	l0=$(sh_count "$w" theirs arrow)
	act_press "$m" "trigger r 1 1500"
	sleep "$SHOTS_SETTLE"
	if wait_until "$SHOTS_WAIT" sh_up "$w" theirs arrow "$l0"; then
		pass "$label: the $w launches it at its player" "arrow $l0 -> $(sh_count "$w" theirs arrow)"
	else
		fail "$label: the $w launches it at its player" "$(sh_part "$(dc "$w" shots)" theirs | cut -c1-220)"
	fi
	sleep 1  # its flight
	read -r hp _ _ _ d u <<<"$(act_harm_read "$w")"
	act_unharmed "$label: the $w's player keeps its health" "$hp0" "$hp" "$d0" "$d" "$u0" "$u"
	act_draw "$m" "$w" "$label: the $m puts the bow away" 0
}

shots_way() {
	local m=$1 w=$2 kind
	section "shots: the $m's player shoots, the $w launches it from its stand-in"
	if ! sh_hooked "$m" || ! sh_hooked "$w"; then
		fail "both games hook all seven projectile classes" "$m: $(sh_part "$(dc "$m" shots)" shots); $w: $(sh_part "$(dc "$w" shots)" shots)"
		return
	fi
	pass "both games hook all seven projectile classes" "hooked=7/7"
	still "$m"
	act_setup "$m" "$w" || return
	sh_bow "$m" "$w" "$m looses an arrow" "$ACTIONS_BOW" "$ACTIONS_ARROW" "trigger r 1 1500" || { [ "$SH_RIG" = 1 ] && return; }
	sh_crossbow "$m" "$w" || { [ "$SH_RIG" = 1 ] && return; }
	for kind in $SHOTS_KINDS; do
		sh_spell "$m" "$w" "$kind" || { [ "$SH_RIG" = 1 ] && return; }
	done
	sh_dual "$m" "$w" || { [ "$SH_RIG" = 1 ] && return; }
	if act_equip "$m" "$w" voice "$ACTIONS_SHOUT" "$m readies a shout"; then
		sh_try "$m" "$w" "$m shouts" any shout "button rb 300" || { [ "$SH_RIG" = 1 ] && return; }
		sleep 2  # the shout's cooldown
	fi
	sh_harm "$m" "$w"
	act_own_drawn "$m" && act_press "$m" "button x 100"
}

# shots_end <who>: its launches flew on their shooters' aim, none failed, and every one ends
shots_end() {
	local w=$1 s
	s=$(dc "$w" shots)
	expect_le "$w: its launches fly on their shooters' aim (worst error left after the fix)" "$(field left "$(sh_part "$s" aim)")" "$SHOTS_AIM" degrees
	expect_zero "$w: none of its launches failed (launchFailed=)" "$(field launchFailed "$(sh_part "$s" theirs)")"
	if wait_until "$SHOTS_END" sh_none_live "$w"; then
		pass "$w: every launch it made for a friend is ended once its flight is over (live=0)" "$(sh_part "$(dc "$w" shots)" flying | cut -c1-200)"
	else
		fail "$w: every launch it made for a friend is ended once its flight is over (live=0)" "after $SHOTS_END s: $(sh_part "$(dc "$w" shots)" flying | cut -c1-200)"
	fi
	say "shots: $w: $(sh_part "$s" natives); $(sh_part "$s" aim); $(field noShooter "$(sh_part "$s" theirs)") without a shooter, $(field far "$(sh_part "$s" theirs)") too far, $(field late "$(sh_part "$s" theirs)") late"
}

# shots_save: the host casts the rune spell (the VM launches it, and it lies), and the VM saves while it does
shots_save() {
	local spell live e0 e1 saves r
	section "shots: the VM saves while a launch of its own for the host lies (the host never saves)"
	if ! spell=$(sh_spell_of host lobber); then
		warn "shots save: a rune to lie while the VM saves" "no lobbed spell in the load order: not tried"
		return 0
	fi
	act_equip host vm right "$spell" "host readies the rune $spell" || return 0
	act_draw host vm "host raises its hands for the rune" 1 || return 0
	e0=$(sh_get vm flying endedSave)
	act_press host "trigger r 1 1500"
	sleep "$SHOTS_SETTLE"
	if ! wait_until "$SHOTS_WAIT" sh_field_up vm flying live 0; then
		rig_fault state "shots save: a launch of the VM's lies when it saves (set-up)" "the VM has none live ($(sh_part "$(dc vm shots)" flying | cut -c1-200))"
		act_draw host vm "host lowers its hands" 0
		return 0
	fi
	live=$(sh_get vm flying live)
	saves=$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { fail "shots save: the VM saves" "the console refused: $r"; return 1; }
	sleep 8
	if [ "$(dc vm ping)" != pong ]; then
		fail "shots save: the VM's game lives through its save with launches of its own live" "its dev channel went silent after the save (a crash? see the crash scan)"
		_rig_stop "the VM's game went down" "after its save with launches live"
		return 1
	fi
	pass "shots save: the VM's game lives through its save with launches of its own live" "$live live before it"
	expect_ge "shots save: the VM's log says it saved" "$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)" $((saves + 1))
	e1=$(sh_get vm flying endedSave)
	if is_num "$e0" && is_num "$e1" && [ "$e1" -ge $((e0 + live)) ]; then
		pass "shots save: every launch it had live was ended for the save (endedSave=)" "$e0 -> $e1, $live live"
	else
		fail "shots save: every launch it had live was ended for the save (endedSave=)" "${e0:-?} -> ${e1:-?} with $live live"
	fi
	expect_zero "shots save: none live after the save (live=)" "$(sh_get vm flying live)"
	expect_zero "shots save: no stray stand-in after the save (strays=)" "$(field strays "$(dc vm strays)")"
	act_draw host vm "host lowers its hands" 0
}

# shots_npc <mover> <watcher>: a person the mover runs casts at the mover's stand-in of the watcher's player (console
# set-up); the mover says an NPC's shot, the watcher launches it from its copy
shots_npc() {
	local m=$1 w=$2 label="shots npc ($1's person casts, $2 watches)" mid pick key local spell sid r n0 f0 l0
	section "$label"
	mid=$(field player "$(dc "$m" 'session status')")
	pick=$(rd_npc_pick "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")")
	if [ -z "$pick" ]; then
		rig_fault state "$label: a person the $m runs that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"
		return 0
	fi
	read -r key local <<<"$pick"
	if ! spell=$(sh_spell_of "$m" missile); then
		warn "$label: a missile spell for the person" "none in the load order: not tried"
		return 0
	fi
	sid=$(field standin "$(dc "$m" standins)")
	if ! [[ $sid =~ ^[0-9A-F]{8}$ ]] || [ "$sid" = 00000000 ]; then
		rig_fault state "$label: the $m's stand-in of the $w's player to cast at (set-up)" "$(dc "$m" standins | cut -c1-200)"
		return 0
	fi
	n0=$(sh_get "$m" ours npc); f0=$(sh_get "$m" hook firsts); l0=$(sh_npc_launched "$w" "$mid")
	r=$(dc "$m" "console $local.cast $spell $sid")
	if [ "$r" != ran ]; then
		rig_fault input "$label: the $m's person $key casts $spell (the console's cast; set-up)" "the console refused: $r"
		return 0
	fi
	say "shots npc: the $m's person $key ($local) cast $spell at the $m's stand-in $sid with the console (set-up)"
	if ! wait_until "$SHOTS_WAIT" sh_field_up "$m" ours npc "$n0"; then
		if sh_field_up "$m" hook firsts "$f0"; then
			fail "$label: the $m's game says its NPC's shot" "a projectile was seen (firsts $f0 -> $(sh_get "$m" hook firsts)), no NPC's shot said ($(sh_part "$(dc "$m" shots)" ours | cut -c1-200))"
		else
			rig_fault input "$label: the $m's person casts (set-up)" "no projectile at all in the $m's game after the console's cast"
		fi
		return 0
	fi
	pass "$label: the $m's game says its NPC's shot" "npc $n0 -> $(sh_get "$m" ours npc)"
	if wait_until "$SHOTS_WAIT" sh_npc_up "$w" "$mid" "$l0"; then
		pass "$label: the $w launches it from its copy of the person" "npcLaunched $l0 -> $(sh_npc_launched "$w" "$mid")"
	else
		fail "$label: the $w launches it from its copy of the person" "$(sh_from "$(dc "$w" shots)" "$mid"); $(sh_part "$(dc "$w" shots)" theirs | cut -c1-200)"
	fi
}

step_shots() {
	local w
	for w in host vm; do still $w; done
	shots_way vm host
	shots_way host vm
	shots_save
	shots_npc vm host
	shots_npc host vm
	section "shots: what each game launched for the other"
	for w in host vm; do shots_end $w; done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init shotstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Shots::Command)
	S='shots active=1 hooked=7/7 | ours seen=5 sent=4 byKind=arrow:2,missile:1,cone:1 npc=1 dual=1 coalesced=3 dropped=0 notOurs=2 shooterGone=0 unfit=0 waiting=0 | theirs in=6 launched=5 byKind=arrow:3,flame:2 dual=0 late=1 inboxDropped=0 queued=0 noShooter=0 far=0 unknown=0 full=0 launchFailed=0 runByUs=0 unmirrored=0 | natives ended=2 byKind=arrow:2 skipped=4 ending=0 | flying oursSeen=5 raced=0 pending=0 live=1 liveArrows=1 gone=3 endedStuck=1 endedOld=0 endedSave=0 | aim reaimed=2 reaimedLate=0 errMax=34.5 left=0.4 | hook firsts=12 sight=12 dropped=0 | from player=2 in=6 launched=5 npcIn=2 npcLaunched=2 | from player=12 in=1 launched=0 npcIn=0 npcLaunched=0'
	t "part: ours" "$(field byKind "$(sh_part "$S" ours)")" "arrow:2,missile:1,cone:1"
	t "part: theirs" "$(field byKind "$(sh_part "$S" theirs)")" "arrow:3,flame:2"
	t "part: natives" "$(field ended "$(sh_part "$S" natives)")" 2
	t "part: flying" "$(field live "$(sh_part "$S" flying)")" 1
	t "part: aim" "$(field left "$(sh_part "$S" aim)")" 0.4
	t "part: hook" "$(field firsts "$(sh_part "$S" hook)")" 12
	t "part: shots" "$(field hooked "$(sh_part "$S" shots)")" 7/7
	t "part: none" "$(sh_part "$S" nothing)" ""
	t "from: that player's" "$(field npcLaunched "$(sh_from "$S" 2)")" 2
	t "from: not player 2 in player 12" "$(field in "$(sh_from "$S" 12)")" 1
	t "from: none" "$(sh_from "$S" 3)" ""
	t "count: ours of a kind" "$(dc() { echo "$S"; }; sh_count host ours arrow)" 2
	t "count: theirs of a kind" "$(dc() { echo "$S"; }; sh_count host theirs flame)" 2
	t "count: none of a kind" "$(dc() { echo "$S"; }; sh_count host theirs beam)" 0
	t "count: ours, any" "$(dc() { echo "$S"; }; sh_count host ours any)" 4
	t "count: theirs, any" "$(dc() { echo "$S"; }; sh_count host theirs any)" 5
	t "up" "$(dc() { echo "$S"; }; sh_up host theirs arrow 2 && echo yes)" yes
	t "up: not when the same" "$(dc() { echo "$S"; }; sh_up host theirs arrow 3 || echo no)" no
	t "npc launched" "$(dc() { echo "$S"; }; sh_npc_launched host 2)" 2
	t "npc launched: none heard from that game" "$(dc() { echo "$S"; }; sh_npc_launched host 5)" 0
	t "hooked" "$(dc() { echo "$S"; }; sh_hooked host && echo yes)" yes
	t "hooked: not six" "$(dc() { echo "${S/hooked=7/hooked=6}"; }; sh_hooked host || echo no)" no
	t "none live: one is" "$(dc() { echo "$S"; }; sh_none_live host || echo no)" no
	t "found: a spell" "$(sh_found 'spell=0001C789 projectile=0001C788 kind=missile casting=ff shout=0 name="Firebolt"' spell)" 0001C789
	t "found: a crossbow" "$(sh_found 'weapon=0200F19F ammo=0200CAB9 name="Crossbow"' ammo)" 0200CAB9
	t "found: none" "$(sh_found 'none: no spell launches a beam' spell)" ""

	# The way, both games faked: the actions step's set-up, equips and draws are taken as done (its own self-test reads
	# them); the mover's pad presses launch what its right hand holds (its graph hears the bow, the cast or the shout,
	# its game says the shot) and the watcher launches what was said, as the plugin does. FS_MODE: ok, nohook (a game
	# has six classes hooked), deaf (the presses do nothing: the rig's), unsaid (the graph hears each, nothing is said:
	# the mod's), unshown (said, never launched by the watcher), nocrossbow (no crossbow in the load order: a WARN),
	# nodual (the two triggers make two casts, no dual: a WARN), lostdual (a dual said, launched as a single), harm (the
	# watcher's player loses health to the stand-in's arrow).
	fake_shots() {
		FS=$d/fs.$RANDOM; mkdir -p "$FS"; FS_MODE=$1
		for k in arrow missile flame beam lobber cone; do echo 0 > "$FS/o_$k"; echo 0 > "$FS/t_$k"; done
		for k in bow cast shout sent launched odual tdual left hp_lost; do echo 0 > "$FS/$k"; done
		echo none > "$FS/right"; echo none > "$FS/lefthand"
		fs_by() { local k out=""; for k in arrow missile flame beam lobber cone; do [ "$(cat "$FS/$1_$k")" -gt 0 ] && out+="${out:+,}$k:$(cat "$FS/$1_$k")"; done; echo "${out:-none}"; }
		fs_inc() { echo $(( $(cat "$FS/$1") + ${2:-1} )) > "$FS/$1"; }
		fs_kind_of() { case $1 in "$ACTIONS_BOW"|0200F19F) echo arrow ;; 00000A01) echo missile ;; 00000A02) echo flame ;; 00000A03) echo beam ;; 00000A04) echo lobber ;; *) echo none ;; esac; }
		fs_shot() {  # fs_shot <kind> <graph kind> [dual]: the mover's game launches it
			fs_inc "$2"
			[ "$FS_MODE" = unsaid ] && return
			fs_inc "o_$1"; fs_inc sent; [ "${3:-}" = dual ] && fs_inc odual
			[ "$FS_MODE" = unshown ] && return
			fs_inc "t_$1"; fs_inc launched
			[ "${3:-}" = dual ] && [ "$FS_MODE" != lostdual ] && fs_inc tdual
			[ "$1" = arrow ] && [ "$FS_MODE" = harm ] && fs_inc hp_lost 10
			return 0
		}
		act_setup() { :; }
		act_equip() { case $3 in right) echo "$4" > "$FS/right" ;; left) echo "$4" > "$FS/lefthand" ;; esac; return 0; }
		act_draw() { return 0; }
		act_walk_near() { return 0; }
		act_own_drawn() { return 1; }
		dc() { local k hooked=7/7; case "$1 $2" in
			"vm shots")
				[ "$FS_MODE" = nohook ] && hooked=6/7
				echo "shots active=1 hooked=$hooked | ours seen=1 sent=$(cat "$FS/sent") byKind=$(fs_by o) npc=0 dual=$(cat "$FS/odual") coalesced=0 dropped=0 | theirs in=0 launched=0 byKind=none dual=0 | flying live=0 | aim left=0.0 | hook firsts=1" ;;
			"host shots")
				echo "shots active=1 hooked=7/7 | ours seen=0 sent=0 byKind=none npc=0 dual=0 | theirs in=1 launched=$(cat "$FS/launched") byKind=$(fs_by t) dual=$(cat "$FS/tdual") launchFailed=0 | flying live=0 | aim left=0.0 | hook firsts=0" ;;
			"vm actions") echo "actions hooked=1 byKind=bow:$(cat "$FS/bow"),cast:$(cat "$FS/cast"),shout:$(cat "$FS/shout") | player 1 standin=FF000801" ;;
			"host actions") echo "actions hooked=1 byKind=none hp=$((120 - $(cat "$FS/hp_lost"))).0/120.0 | player 2 standin=FF0008AE magicRefused=0 casts=0 blows=0 damaged=0 undone=0.0" ;;
			"vm shots find crossbow") [ "$FS_MODE" = nocrossbow ] && echo "none: no crossbow or bolt in the load order" || echo 'weapon=0200F19F ammo=0200CAB9 name="Crossbow"' ;;
			"vm shots find missile") echo 'spell=00000A01 projectile=00000B01 kind=missile casting=ff shout=0 name="Bolt"' ;;
			"vm shots find flame") echo 'spell=00000A02 projectile=00000B02 kind=flame casting=held shout=0 name="Flames"' ;;
			"vm shots find beam") echo 'spell=00000A03 projectile=00000B03 kind=beam casting=ff shout=0 name="Lightning"' ;;
			"vm shots find lobber") echo 'spell=00000A04 projectile=00000B04 kind=lobber casting=ff shout=0 name="Rune"' ;;
			"vm pad trigger l 1 1500") [ "$FS_MODE" = deaf ] || echo 1 > "$FS/left"; echo ok ;;
			"vm pad trigger r "*)
				[ "$FS_MODE" = deaf ] && { echo ok; return; }
				k=$(fs_kind_of "$(cat "$FS/right")")
				if [ "$(cat "$FS/left")" = 1 ]; then
					echo 0 > "$FS/left"
					if [ "$FS_MODE" = nodual ]; then fs_shot "$k" cast; fs_shot "$k" cast; else fs_shot "$k" cast dual; fi
				elif [ "$k" = arrow ]; then fs_shot arrow bow
				elif [ "$k" != none ]; then fs_shot "$k" cast; fi
				echo ok ;;
			"vm pad button rb 300") [ "$FS_MODE" = deaf ] || fs_shot cone shout; echo ok ;;
			*) echo ran ;; esac; }
		( A_OUT=$d/fs$RANDOM; mkdir -p "$A_OUT"; a_init fs >/dev/null; shots_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: hooked (1); the arrow, the bolt, the four spells and the shout: said and launched (2 each, 14); the dual cast
	# said and launched as one (2); the arrow at the watcher launched (1), its player unharmed (1)
	t "way: all of it works" "$(fake_shots ok)" "PASS:19 "
	t "way: a game without all seven hooks fails, and nothing else is tried" "$(fake_shots nohook)" "FAIL:1 "
	t "way: presses that do nothing are the rig's, and end the way" "$(fake_shots deaf)" "PASS:1 RIG:1 "
	# unsaid: each of the arrow, the bolt, the four spells and the shout heard and not said (7); the dual: a WARN; the
	# arrow at the watcher never launched (1), its player unharmed (1)
	t "way: shots the graph heard and the game never said fail, never RIG" "$(fake_shots unsaid)" "FAIL:8 PASS:2 WARN:1 "
	# unshown: 7 said, 7 not launched; the dual said (1), not launched (1); the arrow at the watcher not launched (1)
	t "way: shots said and never launched by the watcher fail" "$(fake_shots unshown)" "FAIL:9 PASS:10 "
	t "way: no crossbow in the load order is a WARN, the rest goes on" "$(fake_shots nocrossbow)" "PASS:17 WARN:1 "
	t "way: two presses that make no dual cast are a WARN" "$(fake_shots nodual)" "PASS:17 WARN:1 "
	t "way: a dual cast launched as a single fails" "$(fake_shots lostdual)" "FAIL:1 PASS:18 "
	t "way: a stand-in's arrow that hurts the watcher's player fails" "$(fake_shots harm)" "FAIL:1 PASS:18 "

	# The end of the step in a game, faked: FE_MODE ok, aimed (a launch left 20 degrees off), failed (a launch failed),
	# lingers (one stays live)
	fake_end() {
		FE_MODE=$1
		dc() { local left=0.4 failed=0 live=0
			[ "$FE_MODE" = aimed ] && left=20.0; [ "$FE_MODE" = failed ] && failed=1; [ "$FE_MODE" = lingers ] && live=1
			echo "shots active=1 hooked=7/7 | theirs launched=4 launchFailed=$failed noShooter=0 far=0 late=0 | natives ended=1 | flying live=$live | aim reaimed=1 left=$left"; }
		( A_OUT=$d/fe$RANDOM; mkdir -p "$A_OUT"; a_init fe >/dev/null; shots_end host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	t "end: aim, no failed launch, all ended" "$(fake_end ok)" "PASS:3 "
	t "end: a launch left off its shooter's aim fails" "$(fake_end aimed)" "FAIL:1 PASS:2 "
	t "end: a failed launch fails" "$(fake_end failed)" "FAIL:1 PASS:2 "
	t "end: a launch never ended fails" "$(fake_end lingers)" "FAIL:1 PASS:2 "

	# The NPC leg, faked: FN_MODE ok, noperson (none to cast: the rig's), refused (the console refused the cast: the
	# rig's), unsaid (a projectile seen, no NPC's shot said), unshown (said, never launched by the watcher)
	fake_npc_shot() {
		FN=$d/fn.$RANDOM; mkdir -p "$FN"; FN_MODE=$1; echo 0 > "$FN/cast"
		rd_npc_pick() { [ "$FN_MODE" = noperson ] || echo "0001A001 0001A001"; }
		pos_of() { echo "0 0 0"; }
		dc() { local c; c=$(cat "$FN/cast"); case "$1 $2" in
			"vm session status") echo "role=client connection=up welcomed=1 player=2" ;;
			"vm shots find missile") echo 'spell=00000A01 projectile=00000B01 kind=missile casting=ff shout=0 name="Bolt"' ;;
			"vm standins") echo "players=1 | player 1 standin=FF000801 placed=1" ;;
			"vm console 0001A001.cast 00000A01 FF000801") [ "$FN_MODE" = refused ] && { echo "refused"; return; }; echo 1 > "$FN/cast"; echo ran ;;
			"vm shots")
				local n=$c; [ "$FN_MODE" = unsaid ] && n=0
				echo "shots active=1 hooked=7/7 | ours sent=$n byKind=missile:$n npc=$n dual=0 | hook firsts=$c" ;;
			"host shots")
				local l=$c; [ "$FN_MODE" = unsaid ] || [ "$FN_MODE" = unshown ] && l=0
				echo "shots active=1 hooked=7/7 | theirs launched=$l | from player=2 in=0 launched=0 npcIn=$c npcLaunched=$l" ;;
			*) echo ran ;; esac; }
		( A_OUT=$d/fn$RANDOM; mkdir -p "$A_OUT"; a_init fn >/dev/null; shots_npc vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	t "npc: said by its runner, launched from the copy" "$(fake_npc_shot ok)" "PASS:2 "
	t "npc: no person to cast is the rig's" "$(fake_npc_shot noperson)" "RIG:1 "
	t "npc: a console that refuses the cast is the rig's" "$(fake_npc_shot refused)" "RIG:1 "
	t "npc: a projectile seen and no NPC's shot said fails" "$(fake_npc_shot unsaid)" "FAIL:1 "
	t "npc: said and never launched from the copy fails" "$(fake_npc_shot unshown)" "FAIL:1 PASS:1 "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "shots is a step" "$(case " $STEPS " in *" shots "*) echo yes ;; esac)" yes
	t "shots comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' shots')" 1
	t "shots needs a session" "$(case " $SESSION_STEPS " in *" shots "*) echo yes ;; esac)" yes
	t "shots runs by default" "$(case " $OPTIN_STEPS " in *" shots "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "shots.sh selftest: all ok" || echo "shots.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
