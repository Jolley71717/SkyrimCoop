#!/bin/bash
# steps/shots.sh: the rig step `shots` (protocol 13, docs/DESIGN.md "Projectiles"), sourced by steps.sh. Every
# projectile an actor launches is said once as it leaves and launched in the other game from the actor that shows its
# shooter there: a friend's arrows, bolts, spells of every projectile kind, dual casts and shouts from their stand-in,
# and an NPC's (a person's, a summon's, a creature's or a dragon's) from its copy. What the stand-in's own replayed
# release would launch is ended, nothing flies twice, the launches of a game are ended once their flight is over and
# before a save (and none comes back with it), a stand-in's shots harm no one and a copy's harm once. Not run in a game
# yet.
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
#        - the shout (Unrelenting Force, RB): a cone, in both games.
#      For each: the mover's game said it (its `shots` ours byKind= of that kind went up). When it said none, the
#      mover's graph is asked whether the press did anything (its `actions` heard byKind=): it did, and nothing was
#      said, is a FAIL; it did nothing is the press's (a rig fault, which ends that way). Then the watcher launched that
#      kind from its stand-in (`shots` theirs byKind=) within SHOTS_WAIT: FAIL if not. Nothing flies twice: for each
#      one-shot press (an arrow, a bolt, a fire-and-forget spell, the dual cast) the watcher launched exactly one
#      (theirs launched= up by 1) and saw exactly one launch of its own fly (flying oursSeen= up by 1: a native of the
#      stand-in taken for ours would make two). A dual cast said as one (ours dual=) is launched as one (theirs
#      dual=); none said is a WARN (the two presses may not have been taken as one). The stand-in's replayed releases
#      and casts launched natives the watcher ended (natives ended= up over the way); none is a WARN (the open
#      question: does a replayed release launch one at all).
#   3. It harms no one, and it hits: the mover walks up to the watcher's player with its virtual pad and looses an arrow
#      at it; the watcher launched exactly one, it reached the watcher's player (`shots` hits onPlayer= up, or the
#      health it took given back: `actions` damaged=/undone=; neither is a miss, a WARN), and the watcher's player
#      keeps its health (within 1).
# An NPC's shots, each way (the runner's NPC shoots, the watcher's copy of it is read), the watcher's player given
# SHOTS_HEALTH more health for them (set-up, taken back after); each NPC casts Firebolt (SHOTS_NPC_SPELL) at the
# runner's stand-in of the watcher's player from the runner's console (`<ref>.cast`, set-up: the scene's shot, as the
# harness's injected gunfire is), so in the watcher's game its copy's launch flies at the watcher's own player:
#   4. a person the runner runs that the watcher shows as a copy;
#   5. a summon of the runner's player (a Flame Atronach, SHOTS_SUMMON_SPELL, conjured from the console): the shot is
#      said under the summon's spawn key (`shots last`: npc=<id>@<spawner>);
#   6. a creature the runner runs that the watcher shows as a copy, a dragon first when one is near: no hands, and a
#      big one's breath leaves far from its feet.
#   For each: the runner's game said an NPC's shot (ours npc= up; nothing said while a projectile was seen is a FAIL,
#   nothing at all the console's: a rig fault), the watcher launched it from the copy (`from player=<runner>`
#   npcLaunched= up within SHOTS_WAIT) and none was too far to launch (theirs far= unchanged), and the watcher's player
#   was hit once by it and lost health (hits onPlayer= up by exactly 1, againOnPlayer= unchanged; no hit is a WARN: a
#   miss).
# After both ways, in each game:
#   7. Every launch it made for a friend flew on its shooter's aim: the worst error read after the fix (`shots` aim
#      left=) is at most SHOTS_AIM degrees; none of its launches failed (launchFailed=0); every one is ended within
#      SHOTS_END s (live=0: an arrow that stuck a short while after, a rune after its life), none missed at an end
#      (endMissed=0) or untracked (untracked=0); no native it ended was still there after (natives survived=0).
#   8. A save with launches of ours live (the VM only, never the host): the host casts the rune spell and the VM saves
#      while it lies: the VM lives, every launch was ended for the save (endedSave= up, live=0), no stray stand-in.
#      Then the VM leaves the session and loads that save alone: no projectile of that rune's record whose shooter is
#      missing or a stand-in came back with it (`shots strays <record>`: strays=0), and no stray stand-in; then it
#      joins again.
# Said for the record: the natives each game ended, its turned-back aims (aim reaimed= reaimedLate=), late and far
# shots. Not on the rig yet: wards and barriers, a staff, explosions' damage.

SHOTS_WAIT=${SHOTS_WAIT:-5}        # s: for a friend's shot to be launched in the watcher's game
SHOTS_SETTLE=${SHOTS_SETTLE:-2}    # s: after a press, for the mover's game to have launched and said it
SHOTS_AIM=${SHOTS_AIM:-5}          # degrees: a launch's aim off its shooter's after the fix, at most
SHOTS_END=${SHOTS_END:-16}         # s: every launch of a game ended by then (ShotRules.h MAX_LIFE 12 s, LOBBER_LIFE 30 s
                                   # for a rune, which the save leg ends first)
SHOTS_KINDS=${SHOTS_KINDS:-"missile flame beam lobber"}
SHOTS_ONCE=${SHOTS_ONCE:-"arrow missile lobber"}   # the kinds one press launches one of (not a held stream)
SHOTS_NPC_SPELL=${SHOTS_NPC_SPELL:-0001C789}       # Skyrim.esm's Firebolt: what each NPC casts at the stand-in
SHOTS_SUMMON_SPELL=${SHOTS_SUMMON_SPELL:-000204C3} # Skyrim.esm's Conjure Flame Atronach
SHOTS_HEALTH=${SHOTS_HEALTH:-2000}                 # the health the watcher's player is given for the NPC legs

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# sh_part <shots reply> <first word>: that part (shots, ours, theirs, hits, natives, flying, aim, hook)
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
# sh_once <kind>: one press launches one of that kind
sh_once() { case " $SHOTS_ONCE " in *" $1 "*) return 0 ;; esac; return 1; }
# sh_delta <before> <after>: after less before; nothing when either is not a number
sh_delta() { is_num "$1" && is_num "$2" && echo $(($2 - $1)); }
# sh_spawn_said <shots last reply>: the shot said last was an NPC's under a spawn key (npc=<id>@<spawner>)
sh_spawn_said() { [[ $1 =~ ^said\ npc=[0-9A-F]{8}@[0-9]+\  ]]; }
# sh_pick <runner rows> <watcher rows> <runner id> <x y z> <kinds, first wanted first>: "<key> <local id> <kind>" of
# an NPC (of the load order, alive, no child, in the world) the runner runs and the watcher shows as a copy: of the
# first kind that has one, the nearest to that place; empty: none. Rows as npcs.sh's npc_rows.
sh_pick() {
	awk -v me="$3" -v p="$4" -v kinds="$5" 'BEGIN { split(p, q, " "); n = split(kinds, k, " "); for (i = 1; i <= n; i++) rank[k[i]] = i }
		NF == 0 { next }
		FILENAME == ARGV[1] { if ($3 == "copy") shown[$1] = 1; next }
		$2 == me && $3 == "run" && ($4 in rank) && $1 !~ /:/ && $15 == "0" && $16 != "1" && $5 != "-" && ($1 in shown) {
			d = sqrt(($5 - q[1]) ^ 2 + ($6 - q[2]) ^ 2)
			if (best == "" || rank[$4] < br || (rank[$4] == br && d < bestd)) { best = $1 " " $14 " " $4; bestd = d; br = rank[$4] }
		}
		END { if (best != "") print best }' <(printf '%s\n' "$2") <(printf '%s\n' "$1")
}

# The last try's rig fault: the way ends there.
SH_RIG=0

# sh_exactly_one <mover> <watcher> <label> <launched before> <oursSeen before>: the watcher launched one, and one
# launch of its own flew (a native taken for ours, or one launched twice, makes two)
sh_exactly_one() {
	local m=$1 w=$2 label=$3 dl ds
	sleep "$SHOTS_SETTLE"
	dl=$(sh_delta "$4" "$(sh_get "$w" theirs launched)"); ds=$(sh_delta "$5" "$(sh_get "$w" flying oursSeen)")
	if [ "$dl" = 1 ] && [ "$ds" = 1 ]; then
		pass "$label: nothing flies twice in the $w (one launched, one flew)" "launched +$dl, oursSeen +$ds"
	else
		fail "$label: nothing flies twice in the $w (one launched, one flew)" "launched +${dl:-?}, oursSeen +${ds:-?} for one press ($(sh_part "$(dc "$w" shots)" natives | cut -c1-160))"
	fi
}

# sh_try <mover> <watcher> <label> <kind|any> <graph kind> <press...>: the presses make the mover's game say a shot of
# that kind, and the watcher launches it from its stand-in of the mover; one of a one-shot kind (sh_once) exactly once.
# False when it did not (SH_RIG=1: the press's).
sh_try() {
	local m=$1 w=$2 label=$3 kind=$4 ak=$5 s0 l0 h0 s1 h1 a0 o0; shift 5
	SH_RIG=0
	s0=$(sh_count "$m" ours "$kind"); l0=$(sh_count "$w" theirs "$kind"); h0=$(act_heard "$m" "$ak")
	a0=$(sh_get "$w" theirs launched); o0=$(sh_get "$w" flying oursSeen)
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
	sh_once "$kind" && sh_exactly_one "$m" "$w" "$label" "$a0" "$o0"
	return 0
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
	local m=$1 w=$2 spell d0 l0 d1 a0 o0
	spell=$(sh_spell_of "$m" missile) || { warn "$m dual casts" "no missile spell in the load order: not tried"; return 0; }
	act_equip "$m" "$w" right "$spell" "$m readies $spell in the right hand" || { SH_RIG=1; return 1; }
	act_equip "$m" "$w" left "$spell" "$m readies $spell in the left hand" || { SH_RIG=1; return 1; }
	act_draw "$m" "$w" "$m raises both hands" 1 || { SH_RIG=1; return 1; }
	d0=$(sh_get "$m" ours dual); l0=$(sh_get "$w" theirs dual)
	a0=$(sh_get "$w" theirs launched); o0=$(sh_get "$w" flying oursSeen)
	act_press "$m" "trigger l 1 1500" "trigger r 1 1500"
	sleep "$SHOTS_SETTLE"
	d1=$(sh_get "$m" ours dual)
	if ! is_num "$d0" || ! is_num "$d1" || [ "$d1" -le "$d0" ]; then
		warn "$m dual casts $spell: the $m's game says a dual cast" "none said (dual $d0 -> $d1): the two presses may not have been taken as one"
	else
		pass "$m dual casts $spell: the $m's game says a dual cast" "dual $d0 -> $d1"
		if wait_until "$SHOTS_WAIT" sh_field_up "$w" theirs dual "$l0"; then
			pass "$m dual casts $spell: the $w launches it as a dual cast" "dual $l0 -> $(sh_get "$w" theirs dual)"
			sh_exactly_one "$m" "$w" "$m dual casts $spell" "$a0" "$o0"
		else
			fail "$m dual casts $spell: the $w launches it as a dual cast" "$(sh_part "$(dc "$w" shots)" theirs | cut -c1-220)"
		fi
	fi
	act_draw "$m" "$w" "$m lowers its hands" 0
}

# sh_harm <mover> <watcher>: the mover looses an arrow at the watcher's player; it is launched there once, reaches the
# watcher's player and harms it not
sh_harm() {
	local m=$1 w=$2 label="$1's arrow at the $2's player" hp0 d0 u0 hp d u l0 a0 o0 p0 p
	act_walk_near "$m" "$w" || { SH_RIG=1; return 1; }
	act_equip "$m" "$w" right "$ACTIONS_BOW" "$label: the $m holds the bow" || { SH_RIG=1; return 1; }
	act_equip "$m" "$w" ammo "$ACTIONS_ARROW" "$label: the $m has arrows" || { SH_RIG=1; return 1; }
	act_draw "$m" "$w" "$label: the $m draws" 1 || { SH_RIG=1; return 1; }
	read -r hp0 _ _ _ d0 u0 <<<"$(act_harm_read "$w")"
	l0=$(sh_count "$w" theirs arrow); a0=$(sh_get "$w" theirs launched); o0=$(sh_get "$w" flying oursSeen); p0=$(sh_get "$w" hits onPlayer)
	act_press "$m" "trigger r 1 1500"
	sleep "$SHOTS_SETTLE"
	if wait_until "$SHOTS_WAIT" sh_up "$w" theirs arrow "$l0"; then
		pass "$label: the $w launches it at its player" "arrow $l0 -> $(sh_count "$w" theirs arrow)"
		sh_exactly_one "$m" "$w" "$label" "$a0" "$o0"
	else
		fail "$label: the $w launches it at its player" "$(sh_part "$(dc "$w" shots)" theirs | cut -c1-220)"
	fi
	sleep 1  # its flight
	read -r hp _ _ _ d u <<<"$(act_harm_read "$w")"
	p=$(sh_get "$w" hits onPlayer)
	if act_more "$p0" "$p" || act_more "$d0" "$d" || _sh_more_f "$u0" "$u"; then
		pass "$label: it reaches the $w's player" "hits onPlayer $p0 -> $p, health taken $d0 -> $d, given back $u0 -> $u"
		act_unharmed "$label: the $w's player keeps its health" "$hp0" "$hp" "$d0" "$d" "$u0" "$u"
	else
		warn "$label: it reaches the $w's player" "no hit on it (hits onPlayer $p0 -> $p, health taken $d0 -> $d): a miss, so its harm was not tried; hp $hp0 -> $hp"
	fi
	act_draw "$m" "$w" "$label: the $m puts the bow away" 0
}
_sh_more_f() { is_num "$1" && is_num "$2" && _cmp "$2" '>' "$1"; }

shots_way() {
	local m=$1 w=$2 kind n0 n1
	section "shots: the $m's player shoots, the $w launches it from its stand-in"
	if ! sh_hooked "$m" || ! sh_hooked "$w"; then
		fail "both games hook all seven projectile classes" "$m: $(sh_part "$(dc "$m" shots)" shots); $w: $(sh_part "$(dc "$w" shots)" shots)"
		return
	fi
	pass "both games hook all seven projectile classes" "hooked=7/7"
	still "$m"
	act_setup "$m" "$w" || return
	n0=$(sh_get "$w" natives ended)
	sh_bow "$m" "$w" "$m looses an arrow" "$ACTIONS_BOW" "$ACTIONS_ARROW" "trigger r 1 1500" || { [ "$SH_RIG" = 1 ] && return; }
	sh_crossbow "$m" "$w" || { [ "$SH_RIG" = 1 ] && return; }
	for kind in $SHOTS_KINDS; do
		sh_spell "$m" "$w" "$kind" || { [ "$SH_RIG" = 1 ] && return; }
	done
	sh_dual "$m" "$w" || { [ "$SH_RIG" = 1 ] && return; }
	n1=$(sh_get "$w" natives ended)
	if act_more "$n0" "$n1"; then
		pass "$m's releases and casts: the $w's stand-in's own projectiles are ended (natives ended=)" "$n0 -> $n1"
	else
		warn "$m's releases and casts: the $w's stand-in's own projectiles are ended (natives ended=)" "none ended ($n0 -> $n1): its replayed releases and casts launched no projectile of their own (the open question)"
	fi
	if act_equip "$m" "$w" voice "$ACTIONS_SHOUT" "$m readies a shout"; then
		sh_try "$m" "$w" "$m shouts" cone shout "button rb 300" || { [ "$SH_RIG" = 1 ] && return; }
		sleep 2  # the shout's cooldown
	fi
	sh_harm "$m" "$w"
	act_own_drawn "$m" && act_press "$m" "button x 100"
}

# shots_end <who>: its launches flew on their shooters' aim, none failed, every one ends, no native survived
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
	s=$(dc "$w" shots)
	expect_zero "$w: no launch of its own was left unended for an update that would not finish (endMissed=)" "$(field endMissed "$(sh_part "$s" flying)")"
	expect_zero "$w: no launch of its own was lost from its book (untracked=)" "$(field untracked "$(sh_part "$s" flying)")"
	expect_zero "$w: no stand-in's or copy's own projectile was still there after it was ended (natives survived=)" "$(field survived "$(sh_part "$s" natives)")"
	say "shots: $w: $(sh_part "$s" natives); $(sh_part "$s" aim); $(sh_part "$s" hits); $(field noShooter "$(sh_part "$s" theirs)") without a shooter, $(field far "$(sh_part "$s" theirs)") too far, $(field late "$(sh_part "$s" theirs)") late"
}

# shots_save: the host casts the rune spell (the VM launches it, and it lies), and the VM saves while it does; then the
# VM loads that save alone and no projectile of that rune came back without its shooter or with a stand-in's
shots_save() {
	local spell live e0 e1 saves r found record
	section "shots: the VM saves while a launch of its own for the host lies (the host never saves)"
	found=$(dc host 'shots find lobber'); record=$(sh_found "$found" projectile)
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
	expect_zero "shots save: none was being updated through the save's wait (endMissed=)" "$(sh_get vm flying endMissed)"
	expect_zero "shots save: no stray stand-in after the save (strays=)" "$(field strays "$(dc vm strays)")"
	act_draw host vm "host lowers its hands" 0

	section "shots: the VM loads that save alone (out of the session)"
	if [ -z "$record" ]; then
		rig_fault state "shots load: the rune's record to look for (set-up)" "host's shots find lobber: $found"
		return 0
	fi
	r=$(dc vm 'session leave'); [[ $r == left* ]] || fail "shots load: the VM leaves" "$r"
	gone_from host || fail "shots load: the host's stand-in of the VM goes when it leaves" "$(dc host standins | cut -c1-200)"
	r=$(dc vm "console load $SAVE_NAME"); [ "$r" = ran ] || { fail "shots load: the VM loads its save" "the console refused: $r"; return 1; }
	sleep 5
	if ! wait_until 90 in_world vm; then fail "shots load: the VM is back in the world after its load" "$(dc vm where | cut -c1-160)"; return 1; fi
	sleep 5
	r=$(dc vm "shots strays $record")
	expect_zero "shots load: no rune of the host's ($record) came back from the save without its shooter or with a stand-in's (shots strays=)" "$(field strays "$r")"
	say "shots load: projectiles in the VM's world after the load: $(field projectiles "$r") ($(dc vm 'shots strays' | cut -c1-200))"
	expect_zero "shots load: no stray stand-in came back from the save (strays=)" "$(field strays "$(dc vm strays)")"
	if r=$(vm_join) && wait_until 90 joined_both; then pass "shots load: the VM joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "shots load: the VM joins again" "$r"; return 1; fi
	wait_until 30 placed vm || fail "shots load: the VM's stand-in of the host is placed again after the rejoin" "$(dc vm standins | cut -c1-200)"
}

# sh_npc_cast <runner> <watcher> <label> <key> <local id> [spawn]: that NPC of the runner's casts SHOTS_NPC_SPELL at the
# runner's stand-in of the watcher's player (console set-up); the runner says it, the watcher launches it from its copy
# (not too far), and it hits the watcher's player once
sh_npc_cast() {
	local m=$1 w=$2 label=$3 key=$4 local=$5 spawn=${6:-} mid sid r n0 f0 l0 far0 p0 a0 hp0 hp p a far
	mid=$(field player "$(dc "$m" 'session status')")
	sid=$(field standin "$(dc "$m" standins)")
	if ! [[ $sid =~ ^[0-9A-F]{8}$ ]] || [ "$sid" = 00000000 ]; then
		rig_fault state "$label: the $m's stand-in of the $w's player to cast at (set-up)" "$(dc "$m" standins | cut -c1-200)"
		return 0
	fi
	n0=$(sh_get "$m" ours npc); f0=$(sh_get "$m" hook firsts); l0=$(sh_npc_launched "$w" "$mid"); far0=$(sh_get "$w" theirs far)
	p0=$(sh_get "$w" hits onPlayer); a0=$(sh_get "$w" hits againOnPlayer); read -r hp0 _ <<<"$(act_harm_read "$w")"
	r=$(dc "$m" "console $local.cast $SHOTS_NPC_SPELL $sid")
	if [ "$r" != ran ]; then
		rig_fault input "$label: the $m's $key casts $SHOTS_NPC_SPELL (the console's cast; set-up)" "the console refused: $r"
		return 0
	fi
	say "$label: the $m's $key ($local) cast $SHOTS_NPC_SPELL at the $m's stand-in $sid with the console (set-up)"
	if ! wait_until "$SHOTS_WAIT" sh_field_up "$m" ours npc "$n0"; then
		if sh_field_up "$m" hook firsts "$f0"; then
			fail "$label: the $m's game says its NPC's shot" "a projectile was seen (firsts $f0 -> $(sh_get "$m" hook firsts)), no NPC's shot said ($(sh_part "$(dc "$m" shots)" ours | cut -c1-200))"
		else
			rig_fault input "$label: the $m's NPC casts (set-up)" "no projectile at all in the $m's game after the console's cast"
		fi
		return 0
	fi
	pass "$label: the $m's game says its NPC's shot" "npc $n0 -> $(sh_get "$m" ours npc)"
	if [ -n "$spawn" ]; then
		r=$(dc "$m" 'shots last')
		if sh_spawn_said "$r"; then pass "$label: said under the summon's spawn key" "$(cut -c1-80 <<<"$r")"; else fail "$label: said under the summon's spawn key" "$(cut -c1-160 <<<"$r")"; fi
	fi
	if wait_until "$SHOTS_WAIT" sh_npc_up "$w" "$mid" "$l0"; then
		pass "$label: the $w launches it from its copy" "npcLaunched $l0 -> $(sh_npc_launched "$w" "$mid")"
	else
		fail "$label: the $w launches it from its copy" "$(sh_from "$(dc "$w" shots)" "$mid"); $(sh_part "$(dc "$w" shots)" theirs | cut -c1-200)"
		return 0
	fi
	far=$(sh_get "$w" theirs far)
	if [ "$far" = "$far0" ]; then pass "$label: none too far from its copy to launch (far=)" "$far"; else fail "$label: none too far from its copy to launch (far=)" "$far0 -> $far: the copy stood too far from where the shot left"; fi
	sleep 2  # its flight
	p=$(sh_get "$w" hits onPlayer); a=$(sh_get "$w" hits againOnPlayer); read -r hp _ <<<"$(act_harm_read "$w")"
	case "$(sh_delta "$p0" "$p")" in
		0) warn "$label: the copy's shot hits the $w's player once" "no hit on it (onPlayer $p0 -> $p, hp $hp0 -> $hp): a miss" ;;
		1)
			if [ "$a" != "$a0" ]; then
				fail "$label: the copy's shot hits the $w's player once" "hit again (againOnPlayer $a0 -> $a): counted twice"
			elif is_num "$hp0" && is_num "$hp" && _cmp "$hp" '<' "$hp0"; then
				pass "$label: the copy's shot hits the $w's player once, and harms it" "onPlayer $p0 -> $p, hp $hp0 -> $hp"
			else
				fail "$label: the copy's shot hits the $w's player once, and harms it" "hit (onPlayer $p0 -> $p) and no health lost (hp $hp0 -> $hp): an NPC's spell harms no friend"
			fi ;;
		*) fail "$label: the copy's shot hits the $w's player once" "onPlayer $p0 -> $p for one cast: more than one launch hit it" ;;
	esac
}

# shots_npc <runner> <watcher>: a person, a summon and a creature (a dragon first) of the runner's shoot at the watcher's
# player's stand-in; the watcher's copies launch it
shots_npc() {
	local m=$1 w=$2 mid pick key local kind label
	section "shots npc: the $m's NPCs shoot, the $w launches it from their copies"
	mid=$(field player "$(dc "$m" 'session status')")
	dc "$w" "console player.modav health $SHOTS_HEALTH" >/dev/null
	say "shots npc: the $w's player given $SHOTS_HEALTH more health for the NPCs' shots (set-up, taken back after)"
	# A person.
	label="shots npc ($m's person casts, $w watches)"
	pick=$(sh_pick "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")" human)
	if [ -z "$pick" ]; then
		rig_fault state "$label: a person the $m runs that the $w shows (set-up)" "$(dc "$m" npcs | cut -c1-200)"
	else
		read -r key local _ <<<"$pick"
		sh_npc_cast "$m" "$w" "$label" "$key" "$local"
	fi
	# A summon: a spawn, said under its spawn key.
	label="shots npc ($m's summon casts, $w watches)"
	dc "$m" "console player.addspell $SHOTS_SUMMON_SPELL" >/dev/null
	dc "$m" "console player.cast $SHOTS_SUMMON_SPELL player" >/dev/null
	say "$label: set-up (console): the $m's player conjures $SHOTS_SUMMON_SPELL"
	if ! wait_until 8 _npcs_spawned "$m" "$mid"; then
		rig_fault state "$label: the summon (set-up)" "no summon of the $m's player in its npcs list: $(dc "$m" npcs | cut -c1-160)"
	else
		read -r key local <<<"$(npcs_spawn_of "$(_npcs_rows "$m")" "$mid")"
		if ! wait_until "$NPC_WAIT" _npcs_copy_shown "$w" "$key"; then
			fail "$label: the $w shows a copy of the summon" "no mirrored copy of $key in $NPC_WAIT s: $(dc "$w" npcs | cut -c1-200)"
		else
			sh_npc_cast "$m" "$w" "$label" "$key" "$local" spawn
		fi
		dc "$m" "console $local.kill" >/dev/null
		say "$label: set-up (console): the summon $local killed"
	fi
	dc "$m" "console player.removespell $SHOTS_SUMMON_SPELL" >/dev/null
	# A creature, a dragon first.
	label="shots npc ($m's creature casts, $w watches)"
	pick=$(sh_pick "$(npc_rows "$(dc "$m" 'npcs list')")" "$(npc_rows "$(dc "$w" 'npcs list')")" "$mid" "$(pos_of "$m")" "dragon creature")
	if [ -z "$pick" ]; then
		warn "$label: a creature or dragon the $m runs that the $w shows" "none near ($(dc "$m" npcs | cut -c1-160)): not tried"
	else
		read -r key local kind <<<"$pick"
		sh_npc_cast "$m" "$w" "shots npc ($m's $kind casts, $w watches)" "$key" "$local"
	fi
	dc "$w" "console player.modav health -$SHOTS_HEALTH" >/dev/null
}

step_shots() {
	local w
	for w in host vm; do still $w; done
	shots_way vm host
	shots_way host vm
	shots_npc vm host
	shots_npc host vm
	section "shots: what each game launched for the other"
	for w in host vm; do shots_end $w; done
	shots_save
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init shotstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Shots::Command)
	S='shots active=1 hooked=7/7 | ours seen=5 sent=4 byKind=arrow:2,missile:1,cone:1 npc=1 dual=1 coalesced=3 dropped=0 notOurs=2 shooterGone=0 unfit=0 waiting=0 | theirs in=6 launched=5 byKind=arrow:3,flame:2 dual=0 late=1 inboxDropped=0 queued=0 noShooter=0 far=0 unknown=0 full=0 launchFailed=0 runByUs=0 unmirrored=0 | hits hit=2 onPlayer=1 again=0 againOnPlayer=0 | natives ended=2 byKind=arrow:2 skipped=4 ending=0 survived=0 lingering=0 | flying oursSeen=5 raced=0 pending=0 live=1 liveArrows=1 gone=3 endedStuck=1 endedOld=0 endedSave=0 endWaits=0 endMissed=0 untracked=0 tracked=1 | aim reaimed=2 reaimedLate=0 errMax=34.5 turned=0.0 left=0.4 | hook firsts=12 sight=12 dropped=0 | from player=2 in=6 launched=5 npcIn=2 npcLaunched=2 | from player=12 in=1 launched=0 npcIn=0 npcLaunched=0'
	t "part: ours" "$(field byKind "$(sh_part "$S" ours)")" "arrow:2,missile:1,cone:1"
	t "part: theirs" "$(field byKind "$(sh_part "$S" theirs)")" "arrow:3,flame:2"
	t "part: hits" "$(field onPlayer "$(sh_part "$S" hits)")" 1
	t "part: natives" "$(field ended "$(sh_part "$S" natives)")" 2
	t "part: natives survived" "$(field survived "$(sh_part "$S" natives)")" 0
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
	t "once: an arrow, a missile, a rune" "$(for k in arrow missile lobber flame beam cone; do sh_once $k && echo -n "$k "; done)" "arrow missile lobber "
	t "delta" "$(sh_delta 3 5)" 2
	t "delta: not a number" "$(sh_delta x 5)" ""
	t "spawn said: a summon's" "$(sh_spawn_said 'said npc=FF000801@2 missile projectile=0001C788 weapon=00000000 | launched none' && echo yes)" yes
	t "spawn said: a load-order NPC's" "$(sh_spawn_said 'said npc=0001A67C missile projectile=0001C788 | launched none' || echo no)" no
	t "spawn said: a player's" "$(sh_spawn_said 'said player=2 arrow projectile=0001C788 | launched none' || echo no)" no
	PR='npcs=5 | 0001A67C local=0001A67C owner=1 role=run kind=human at=(100,200,0) dead=0 child=0 | 0001A690 local=0001A690 owner=1 role=run kind=creature at=(900,200,0) dead=0 child=0 | 0001A691 local=0001A691 owner=1 role=run kind=creature at=(150,200,0) dead=0 child=0 | 0001A699 local=0001A699 owner=1 role=run kind=dragon at=(3000,200,0) dead=0 child=0 | 0001A6A0 local=0001A6A0 owner=1 role=run kind=human at=(101,200,0) dead=0 child=1'
	PW='npcs=5 | 0001A67C local=0001A67C owner=1 role=copy kind=human | 0001A690 local=0001A690 owner=1 role=copy kind=creature | 0001A691 local=0001A691 owner=1 role=copy kind=creature | 0001A699 local=0001A699 owner=1 role=copy kind=dragon | 0001A6A0 local=0001A6A0 owner=1 role=copy kind=human'
	t "pick: a person" "$(sh_pick "$(npc_rows "$PR")" "$(npc_rows "$PW")" 1 "100 200 0" human)" "0001A67C 0001A67C human"
	t "pick: a dragon before a creature, however far" "$(sh_pick "$(npc_rows "$PR")" "$(npc_rows "$PW")" 1 "100 200 0" "dragon creature")" "0001A699 0001A699 dragon"
	t "pick: the nearest creature" "$(sh_pick "$(npc_rows "$PR")" "$(npc_rows "$PW")" 1 "100 200 0" creature)" "0001A691 0001A691 creature"
	t "pick: none the watcher shows" "$(sh_pick "$(npc_rows "$PR")" "npcs=0" 1 "100 200 0" creature)" ""
	t "pick: none the runner runs" "$(sh_pick "$(npc_rows "$PR")" "$(npc_rows "$PW")" 2 "100 200 0" human)" ""

	# The way, both games faked: the actions step's set-up, equips and draws are taken as done (its own self-test reads
	# them); the mover's pad presses launch what its right hand holds (its graph hears the bow, the cast or the shout,
	# its game says the shot) and the watcher launches what was said, as the plugin does, ending the stand-in's native
	# and hitting its player with the arrow. FS_MODE: ok, nohook (a game has six classes hooked), deaf (the presses do
	# nothing: the rig's), unsaid (the graph hears each, nothing is said: the mod's), unshown (said, never launched by
	# the watcher), nocrossbow (no crossbow in the load order: a WARN), nodual (the two triggers make two casts, no dual:
	# a WARN), lostdual (a dual said, launched as a single), harm (the watcher's player loses health to the stand-in's
	# arrow), miss (the arrow hits nothing: a WARN), twice (a native taken for ours flies too: two launches seen for
	# each one-shot press), nonative (the stand-in's replays launch nothing of their own: a WARN).
	fake_shots() {
		FS=$d/fs.$RANDOM; mkdir -p "$FS"; FS_MODE=$1
		for k in arrow missile flame beam lobber cone; do echo 0 > "$FS/o_$k"; echo 0 > "$FS/t_$k"; done
		for k in bow cast shout sent launched seen natives odual tdual left hp_lost onplayer; do echo 0 > "$FS/$k"; done
		echo none > "$FS/right"; echo none > "$FS/lefthand"
		fs_by() { local k out=""; for k in arrow missile flame beam lobber cone; do [ "$(cat "$FS/$1_$k")" -gt 0 ] && out+="${out:+,}$k:$(cat "$FS/$1_$k")"; done; echo "${out:-none}"; }
		fs_inc() { echo $(( $(cat "$FS/$1") + ${2:-1} )) > "$FS/$1"; }
		fs_kind_of() { case $1 in "$ACTIONS_BOW"|0200F19F) echo arrow ;; 00000A01) echo missile ;; 00000A02) echo flame ;; 00000A03) echo beam ;; 00000A04) echo lobber ;; *) echo none ;; esac; }
		fs_shot() {  # fs_shot <kind> <graph kind> [dual]: the mover's game launches it
			fs_inc "$2"
			[ "$FS_MODE" = unsaid ] && return
			fs_inc "o_$1"; fs_inc sent; [ "${3:-}" = dual ] && fs_inc odual
			[ "$FS_MODE" = unshown ] && return
			fs_inc "t_$1"; fs_inc launched; fs_inc seen
			[ "$FS_MODE" = twice ] && fs_inc seen
			[ "$FS_MODE" = nonative ] || fs_inc natives
			[ "${3:-}" = dual ] && [ "$FS_MODE" != lostdual ] && fs_inc tdual
			if [ "$1" = arrow ] && [ "$FS_MODE" != miss ]; then fs_inc onplayer; [ "$FS_MODE" = harm ] && fs_inc hp_lost 10; fi
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
				echo "shots active=1 hooked=$hooked | ours seen=1 sent=$(cat "$FS/sent") byKind=$(fs_by o) npc=0 dual=$(cat "$FS/odual") coalesced=0 dropped=0 | theirs in=0 launched=0 byKind=none dual=0 | hits hit=0 onPlayer=0 again=0 againOnPlayer=0 | natives ended=0 survived=0 | flying oursSeen=0 live=0 | aim left=0.0 | hook firsts=1" ;;
			"host shots")
				echo "shots active=1 hooked=7/7 | ours seen=0 sent=0 byKind=none npc=0 dual=0 | theirs in=1 launched=$(cat "$FS/launched") byKind=$(fs_by t) dual=$(cat "$FS/tdual") launchFailed=0 | hits hit=$(cat "$FS/onplayer") onPlayer=$(cat "$FS/onplayer") again=0 againOnPlayer=0 | natives ended=$(cat "$FS/natives") survived=0 | flying oursSeen=$(cat "$FS/seen") live=0 | aim left=0.0 | hook firsts=0" ;;
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
	# ok: hooked (1); the arrow, the bolt, the four spells and the shout: said and launched (2 each, 14); one each for
	# the arrow, the bolt, the missile and the rune (4); the dual cast said, launched as one, once (3); natives ended
	# (1); the arrow at the watcher launched, once, reaching it, unharmed (4)
	t "way: all of it works" "$(fake_shots ok)" "PASS:27 "
	t "way: a game without all seven hooks fails, and nothing else is tried" "$(fake_shots nohook)" "FAIL:1 "
	t "way: presses that do nothing are the rig's, and end the way" "$(fake_shots deaf)" "PASS:1 RIG:1 "
	# unsaid: each of the arrow, the bolt, the four spells and the shout heard and not said (7); the dual: a WARN;
	# natives: none (a WARN); the arrow at the watcher never launched (1), and never reached it (a WARN)
	t "way: shots the graph heard and the game never said fail, never RIG" "$(fake_shots unsaid)" "FAIL:8 PASS:1 WARN:3 "
	# unshown: 7 said, 7 not launched; the dual said (1), not launched (1); natives none (WARN); the arrow at the
	# watcher not launched (1), never reached it (WARN)
	t "way: shots said and never launched by the watcher fail" "$(fake_shots unshown)" "FAIL:9 PASS:9 WARN:2 "
	t "way: no crossbow in the load order is a WARN, the rest goes on" "$(fake_shots nocrossbow)" "PASS:24 WARN:1 "
	t "way: two presses that make no dual cast are a WARN" "$(fake_shots nodual)" "PASS:24 WARN:1 "
	t "way: a dual cast launched as a single fails" "$(fake_shots lostdual)" "FAIL:1 PASS:25 "
	t "way: a stand-in's arrow that hurts the watcher's player fails" "$(fake_shots harm)" "FAIL:1 PASS:26 "
	t "way: a stand-in's arrow that hits nothing is a WARN, not a PASS" "$(fake_shots miss)" "PASS:25 WARN:1 "
	# twice: the arrow, the bolt, the missile, the rune, the dual and the arrow at the watcher each seen twice (6)
	t "way: a native taken for ours (two fly for one press) fails" "$(fake_shots twice)" "FAIL:6 PASS:21 "
	t "way: replays that launch no native of their own are a WARN" "$(fake_shots nonative)" "PASS:26 WARN:1 "

	# The end of the step in a game, faked: FE_MODE ok, aimed (a launch left 20 degrees off), failed (a launch failed),
	# lingers (one stays live), survived (a native still there after its end), missed (an end missed through the wait)
	fake_end() {
		FE_MODE=$1
		dc() { local left=0.4 failed=0 live=0 survived=0 missed=0
			[ "$FE_MODE" = aimed ] && left=20.0; [ "$FE_MODE" = failed ] && failed=1; [ "$FE_MODE" = lingers ] && live=1
			[ "$FE_MODE" = survived ] && survived=1; [ "$FE_MODE" = missed ] && missed=1
			echo "shots active=1 hooked=7/7 | theirs launched=4 launchFailed=$failed noShooter=0 far=0 late=0 | hits hit=1 onPlayer=1 | natives ended=1 survived=$survived lingering=0 | flying live=$live endMissed=$missed untracked=0 | aim reaimed=1 left=$left"; }
		( A_OUT=$d/fe$RANDOM; mkdir -p "$A_OUT"; a_init fe >/dev/null; shots_end host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	t "end: aim, no failed launch, all ended, nothing survived" "$(fake_end ok)" "PASS:6 "
	t "end: a launch left off its shooter's aim fails" "$(fake_end aimed)" "FAIL:1 PASS:5 "
	t "end: a failed launch fails" "$(fake_end failed)" "FAIL:1 PASS:5 "
	t "end: a launch never ended fails" "$(fake_end lingers)" "FAIL:1 PASS:5 "
	t "end: a native still there after its end fails" "$(fake_end survived)" "FAIL:1 PASS:5 "
	t "end: a launch missed at an end fails" "$(fake_end missed)" "FAIL:1 PASS:5 "

	# One NPC's cast, faked: FN_MODE ok, refused (the console refused the cast: the rig's), unsaid (a projectile seen,
	# no NPC's shot said), unshown (said, never launched by the watcher), far (launched, one too far), miss (no hit on
	# the watcher's player: a WARN), twice (two launches hit it), again (one launch hit it twice), harmless (hit, no
	# health lost), nospawn (a summon's said as a load-order NPC's)
	fake_npc_cast() {
		FN=$d/fn.$RANDOM; mkdir -p "$FN"; FN_MODE=$1; echo 0 > "$FN/cast"
		dc() { local c; c=$(cat "$FN/cast"); case "$1 $2" in
			"vm session status") echo "role=client connection=up welcomed=1 player=2" ;;
			"vm standins") echo "players=1 | player 1 standin=FF000801 placed=1" ;;
			"vm console 0001A001.cast $SHOTS_NPC_SPELL FF000801") [ "$FN_MODE" = refused ] && { echo "refused"; return; }; echo 1 > "$FN/cast"; echo ran ;;
			"vm shots last") [ "$FN_MODE" = nospawn ] && echo "said npc=0001A001 missile projectile=0001C788 | launched none" || echo "said npc=0001A001@2 missile projectile=0001C788 | launched none" ;;
			"vm shots")
				local n=$c; [ "$FN_MODE" = unsaid ] && n=0
				echo "shots active=1 hooked=7/7 | ours sent=$n byKind=missile:$n npc=$n dual=0 | hook firsts=$c" ;;
			"host shots")
				local l=$c far=0 p=$c a=0; { [ "$FN_MODE" = unsaid ] || [ "$FN_MODE" = unshown ]; } && l=0
				[ "$FN_MODE" = far ] && far=$c; [ "$FN_MODE" = miss ] && p=0; [ "$FN_MODE" = twice ] && p=$((c * 2)); [ "$FN_MODE" = again ] && a=$c
				echo "shots active=1 hooked=7/7 | theirs launched=$l far=$far | hits hit=$p onPlayer=$p again=$a againOnPlayer=$a | from player=2 in=0 launched=0 npcIn=$c npcLaunched=$l" ;;
			"host actions")
				local hp=5120; [ "$c" = 1 ] && [ "$FN_MODE" != harmless ] && [ "$FN_MODE" != miss ] && hp=5095
				echo "actions hooked=1 byKind=none hp=$hp.0/5120.0 | player 2 standin=FF0008AE damaged=0 undone=0.0" ;;
			*) echo ran ;; esac; }
		( A_OUT=$d/fn$RANDOM; mkdir -p "$A_OUT"; a_init fn >/dev/null; sh_npc_cast vm host "npc" 0001A001 0001A001 ${2:-} >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: said (1), launched (1), none too far (1), hit once and harmed (1)
	t "npc: said by its runner, launched from the copy, not too far, hits the watcher's player once" "$(fake_npc_cast ok)" "PASS:4 "
	t "npc: a summon's said under its spawn key" "$(fake_npc_cast ok spawn)" "PASS:5 "
	t "npc: a summon's said as a load-order NPC's fails" "$(fake_npc_cast nospawn spawn)" "FAIL:1 PASS:4 "
	t "npc: a console that refuses the cast is the rig's" "$(fake_npc_cast refused)" "RIG:1 "
	t "npc: a projectile seen and no NPC's shot said fails" "$(fake_npc_cast unsaid)" "FAIL:1 "
	t "npc: said and never launched from the copy fails" "$(fake_npc_cast unshown)" "FAIL:1 PASS:1 "
	t "npc: a copy too far from where its shot left fails" "$(fake_npc_cast far)" "FAIL:1 PASS:3 "
	t "npc: a copy's shot that hits no one is a WARN" "$(fake_npc_cast miss)" "PASS:3 WARN:1 "
	t "npc: two launches that hit the watcher's player fail" "$(fake_npc_cast twice)" "FAIL:1 PASS:3 "
	t "npc: one launch that hits it twice fails" "$(fake_npc_cast again)" "FAIL:1 PASS:3 "
	t "npc: a hit that takes no health fails" "$(fake_npc_cast harmless)" "FAIL:1 PASS:3 "

	# The NPC legs of one way, faked: a person, a summon and a creature, each cast once; FL_MODE ok, noperson (none to
	# cast: the rig's), nosummon (the console made none: the rig's), unshown (no copy of the summon), nocreature (none
	# near: a WARN)
	fake_npc_legs() {
		FL_MODE=$1; FLC=$d/flc.$RANDOM; echo "" > "$FLC"
		sh_npc_cast() { echo "$3 $4 $5 ${6:-}" >> "$FLC"; }
		sh_pick() { case "$5" in human) [ "$FL_MODE" = noperson ] || echo "0001A001 0001A001 human" ;; *) [ "$FL_MODE" = nocreature ] || echo "0001A690 0001A690 creature" ;; esac; }
		_npcs_spawned() { [ "$FL_MODE" != nosummon ]; }
		npcs_spawn_of() { echo "FF000900:2 FF000900"; }
		_npcs_copy_shown() { [ "$FL_MODE" != unshown ]; }
		npc_rows() { :; }; pos_of() { echo "0 0 0"; }
		dc() { case "$1 $2" in "vm session status") echo "role=client connection=up welcomed=1 player=2" ;; *) echo ran ;; esac; }
		( A_OUT=$d/fl$RANDOM; mkdir -p "$A_OUT"; a_init fl >/dev/null; shots_npc vm host >/dev/null
		  echo "$(cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}')casts=$(grep -c . "$FLC")$(grep -q 'FF000900:2 FF000900 spawn' "$FLC" && echo ' spawn')" )
	}
	t "npc legs: a person, a summon (by its spawn key) and a creature cast" "$(fake_npc_legs ok)" "casts=3 spawn"
	t "npc legs: no person is the rig's, the rest goes on" "$(fake_npc_legs noperson)" "RIG:1 casts=2 spawn"
	t "npc legs: no summon is the rig's, the rest goes on" "$(fake_npc_legs nosummon)" "RIG:1 casts=2"
	t "npc legs: a summon the watcher never shows fails" "$(fake_npc_legs unshown)" "FAIL:1 casts=2"
	t "npc legs: no creature near is a WARN" "$(fake_npc_legs nocreature)" "WARN:1 casts=2 spawn"

	# The save and the load alone, faked: FV_MODE ok, kept (the rune came back from the save without its shooter),
	# norecord (no lobbed spell's record: the load not tried, the rig's)
	fake_save() {
		FV=$d/fv.$RANDOM; mkdir -p "$FV"; FV_MODE=$1; echo 0 > "$FV/saved"; echo 0 > "$FV/live"; echo 0 > "$FV/ended"
		act_equip() { return 0; }; act_draw() { return 0; }
		log_since_mark() { [ "$(cat "$FV/saved")" = 1 ] && echo "Save: saving \"$SAVE_NAME\""; }
		_rig_stop() { :; }; gone_from() { return 0; }; in_world() { return 0; }; vm_join() { echo joined; }; joined_both() { return 0; }; placed() { return 0; }
		dc() { case "$1 $2" in
			"host shots find lobber") [ "$FV_MODE" = norecord ] && echo 'spell=00000A04 kind=lobber' || echo 'spell=00000A04 projectile=00000B04 kind=lobber casting=ff shout=0 name="Rune"' ;;
			"host pad trigger r 1 1500") echo 1 > "$FV/live"; echo ok ;;
			"vm shots") echo "shots active=1 | flying live=$(cat "$FV/live") endedSave=$(cat "$FV/ended") endMissed=0" ;;
			"vm console save $SAVE_NAME") echo 1 > "$FV/saved"; echo 0 > "$FV/live"; echo 1 > "$FV/ended"; echo ran ;;
			"vm ping") echo pong ;;
			"vm strays") echo "strays=0 standins=1 presetActors=1" ;;
			"vm session leave") echo "left role=none" ;;
			"vm shots strays 00000B04") [ "$FV_MODE" = kept ] && echo "strays=1 killed=1 projectiles=3 FF000A10:record=00000B04:shooter=none:killed" || echo "strays=0 killed=0 projectiles=2" ;;
			"vm shots strays") echo "strays=0 killed=0 projectiles=2" ;;
			*) echo ran ;; esac; }
		( A_OUT=$d/fv$RANDOM; mkdir -p "$A_OUT"; a_init fv >/dev/null; shots_save >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: lives (1), its log (1), ended for the save (1), none live (1), none missed (1), no stray stand-in (1); the load:
	# no rune came back (1), no stray stand-in (1), joins again (1)
	t "save: the launches ended, none came back with the load" "$(fake_save ok)" "PASS:9 "
	t "save: a rune that came back with the load fails" "$(fake_save kept)" "FAIL:1 PASS:8 "
	t "save: no record to look for after the load is the rig's" "$(fake_save norecord)" "PASS:6 RIG:1 "

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "shots is a step" "$(case " $STEPS " in *" shots "*) echo yes ;; esac)" yes
	t "shots comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' shots')" 1
	t "shots needs a session" "$(case " $SESSION_STEPS " in *" shots "*) echo yes ;; esac)" yes
	t "shots runs by default" "$(case " $OPTIN_STEPS " in *" shots "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "shots.sh selftest: all ok" || echo "shots.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
