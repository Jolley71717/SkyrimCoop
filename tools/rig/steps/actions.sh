#!/bin/bash
# steps/actions.sh: the rig step `actions` (protocol 9, docs/DESIGN.md "Players"), sourced by steps.sh. A friend's
# stand-in holds what they hold and does what they do: draws and puts away, swings, power attacks, blocks, bashes, draws
# and looses a bow, casts from each hand, shouts; and it deals nothing in the watcher's game (its friend's game deals it
# all). Not run in a game yet.
#
#   step_actions     the step
#   bash tools/rig/steps/actions.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover's player acts, the watcher's stand-in of it is read), both players in one session near each other
# at the harness's cell, after the npccombat step:
#   1. Set-up of the mover's hands from its console and developer channel (as the console sets a scene up, said in the
#      run): an iron sword, an iron shield, a hunting bow and iron arrows given (`player.additem`), Flames and Healing
#      taught (`player.addspell`), Unrelenting Force's first word taught and unlocked (`player.addshout`, `teachword`,
#      `unlockword`); then each put in a hand, the quiver or the voice (`actions equip`). The watcher's stand-in holds
#      the same within ACTIONS_WAIT (its `right=`, `left=`, `ammo=`: the id and 1 when it holds it; `voice=`).
#   2. Each action from the mover's virtual pad (the game's own input, never a graph event sent by a script): the
#      weapon drawn (X), a swing (a tap of the right trigger), a power attack (the right trigger held), a block (the
#      left trigger held, the shield up), a bash (the right trigger tapped while blocking), the weapon put away (X);
#      the bow drawn and loosed (the right trigger held, then let go); Flames from the right hand and Healing from the
#      left (each trigger held); the shout (RB). For each: the mover's own graph heard that kind of action (its
#      `actions` heard byKind=; none heard is the press's fault, a rig fault, unless the hook is not there: FAIL), and
#      the watcher's stand-in played that kind (its byKind=) within ACTIONS_WAIT: FAIL if not. The weapon drawn and put
#      away is read on the stand-in itself (drawn=), and a draw the stand-in's graph did not make (drawFixes= up: the
#      game told to draw by hand) is a WARN.
#   3. After both: neither stand-in took any health here (damaged=0: its friend's game deals its blows; FAIL), its
#      spells' effects on others refused (magicRefused=, said), the events its graph turned away (refused=, a WARN when
#      any), and both games' magic target hooks in (magicHooks: FAIL when not).
#   Put back: the weapon put away. What the console gave stays in the mover's inventory (the host never saves; the
#   VM's test save is its own).

ACTIONS_WAIT=${ACTIONS_WAIT:-5}       # s: for the stand-in to hold or play what its friend does
ACTIONS_SETTLE=${ACTIONS_SETTLE:-2}   # s: after a press, for the mover's graph to have taken it
ACTIONS_SWORD=${ACTIONS_SWORD:-00012EB7}    # Skyrim.esm's Iron Sword
ACTIONS_SHIELD=${ACTIONS_SHIELD:-00012EB6}  # Iron Shield
ACTIONS_BOW=${ACTIONS_BOW:-00013985}        # Hunting Bow
ACTIONS_ARROW=${ACTIONS_ARROW:-0001397D}    # Iron Arrow
ACTIONS_FLAMES=${ACTIONS_FLAMES:-00012FCD}  # Flames (a concentration spell)
ACTIONS_HEAL=${ACTIONS_HEAL:-00012FCC}      # Healing (on the caster itself)
ACTIONS_SHOUT=${ACTIONS_SHOUT:-00013E07}    # Unrelenting Force
ACTIONS_WORD=${ACTIONS_WORD:-00013E22}      # its first word

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# act_own <actions reply>: our player's part (before the first " | ")
act_own() { echo "${1%% | *}"; }
# act_friend <actions reply>: the friend's stand-in's part (the last "| player " section: one friend on the rig)
act_friend() { [[ $1 == *"| player "* ]] && echo "player ${1##*| player }"; }
# act_kind <byKind value> <kind>: how many of that kind ("attack:3,power:1" -> attack 3); 0 when none
act_kind() { local n; n=$(tr ',' '\n' <<<"$1" | sed -n "s/^$2://p" | head -1); echo "${n:-0}"; }
# act_heard <who> <kind>: how many of that kind that game's player's graph heard
act_heard() { act_kind "$(field byKind "$(act_own "$(dc "$1" actions)")")" "$2"; }
# act_played <who> <kind>: how many of that kind that game's stand-in of its friend played
act_played() { act_kind "$(field byKind "$(act_friend "$(dc "$1" actions)")")" "$2"; }
# act_field <who> <name>: a field of that game's stand-in's part
act_field() { field "$2" "$(act_friend "$(dc "$1" actions)")"; }
# act_holds <who> <hand> <id>: that game's stand-in holds the id in that hand (right=<id>:1)
act_holds() { [ "$(act_field "$1" "$2")" = "$3:1" ]; }
# act_voice <who> <id>: its friend's voice is that id, and the stand-in's too (voice=<id>/<id>)
act_voice() { [ "$(act_field "$1" voice)" = "$2/$2" ]; }
# act_drawn <who> <0|1>: that game's stand-in has its weapon out (1) or put away (0)
act_drawn() { [ "$(act_field "$1" drawn)" = "$2" ]; }
# act_up <who> <kind> <before>: that game's stand-in played more of that kind than before
act_up() { local n; n=$(act_played "$1" "$2"); is_num "$n" && [ "$n" -gt "$3" ]; }
# act_own_drawn <who>: that game's player has its weapon out (where's flags, kWeaponDrawn 0x04)
act_own_drawn() { local f; f=$(field flags "$(dc "$1" where)"); [[ $f =~ ^0x[0-9a-fA-F]+$ ]] && [ $(( f & 4 )) -ne 0 ]; }

# act_setup <mover> <watcher>: the mover given and taught what the step uses (console set-up), each said
act_setup() {
	local m=$1 id r
	for id in "$ACTIONS_SWORD 1" "$ACTIONS_SHIELD 1" "$ACTIONS_BOW 1" "$ACTIONS_ARROW 60"; do
		r=$(dc "$m" "console player.additem $id"); [ "$r" = ran ] || { rig_fault state "$m: set-up" "the console refused player.additem $id: $r"; return 1; }
	done
	for id in "$ACTIONS_FLAMES" "$ACTIONS_HEAL"; do dc "$m" "console player.addspell $id" >/dev/null; done
	dc "$m" "console player.addshout $ACTIONS_SHOUT" >/dev/null
	dc "$m" "console player.teachword $ACTIONS_WORD" >/dev/null
	dc "$m" "console player.unlockword $ACTIONS_WORD" >/dev/null
	say "actions: $m given a sword, a shield, a bow, arrows, Flames, Healing and Unrelenting Force from its console (set-up)"
}

# act_equip <mover> <watcher> <hand> <id> <label>: the mover's hand given the id (set-up), and the watcher's stand-in holds it
act_equip() {
	local m=$1 w=$2 hand=$3 id=$4 label=$5 r
	r=$(dc "$m" "actions equip $hand $id")
	[[ $r == equipped* ]] || { rig_fault state "$label: the $m's $hand" "actions equip refused: $r"; return 1; }
	if [ "$hand" = voice ]; then
		wait_until "$ACTIONS_WAIT" act_voice "$w" "$id" && pass "$label: the $w's stand-in has it ready" "voice=$(act_field "$w" voice)" ||
			fail "$label: the $w's stand-in has it ready" "voice=$(act_field "$w" voice) after $ACTIONS_WAIT s (want $id/$id)"
	else
		wait_until "$ACTIONS_WAIT" act_holds "$w" "$hand" "$id" && pass "$label: the $w's stand-in holds it" "$hand=$(act_field "$w" "$hand")" ||
			fail "$label: the $w's stand-in holds it" "$hand=$(act_field "$w" "$hand") after $ACTIONS_WAIT s (want $id:1)"
	fi
}

# act_press <mover> <press...>: each pad command on the mover's game; "wait:<s>" waits between
act_press() {
	local m=$1 p; shift
	for p in "$@"; do
		if [[ $p == wait:* ]]; then sleep "${p#wait:}"; else dc "$m" "pad $p" >/dev/null; fi
	done
}

# act_try <mover> <watcher> <label> <kind> <press...>: the presses make the mover's graph take that kind of action, and
# the watcher's stand-in plays it
act_try() {
	local m=$1 w=$2 label=$3 kind=$4 h0 p0 h1; shift 4
	h0=$(act_heard "$m" "$kind"); p0=$(act_played "$w" "$kind")
	act_press "$m" "$@"
	sleep "$ACTIONS_SETTLE"
	h1=$(act_heard "$m" "$kind")
	if ! is_num "$h0" || ! is_num "$h1" || [ "$h1" -le "$h0" ]; then
		rig_fault input "$label: the $m's press" "the $m's graph heard no $kind ($kind $h0 -> $h1; $(act_own "$(dc "$m" actions)" | cut -c1-160))"
		return 1
	fi
	wait_until "$ACTIONS_WAIT" act_up "$w" "$kind" "$p0" && pass "$label: the $w's stand-in plays it" "$kind played $p0 -> $(act_played "$w" "$kind")" ||
		fail "$label: the $w's stand-in plays it" "the $m sent $((h1 - h0)) $kind event(s); the $w's stand-in played none in $ACTIONS_WAIT s ($(act_friend "$(dc "$w" actions)" | cut -c1-200))"
}

# act_draw <mover> <watcher> <label> <0|1>: X draws (1) or puts away (0) the mover's weapon; the stand-in follows
act_draw() {
	local m=$1 w=$2 label=$3 want=$4 f0
	f0=$(act_field "$w" drawFixes)
	act_press "$m" "button x 100"
	sleep "$ACTIONS_SETTLE"
	if [ "$want" = 1 ] && ! act_own_drawn "$m"; then rig_fault input "$label: the $m's press" "X left the $m's weapon away ($(dc "$m" where | grep -o 'flags=[^ ]*'))"; return 1; fi
	if [ "$want" = 0 ] && act_own_drawn "$m"; then rig_fault input "$label: the $m's press" "X left the $m's weapon out"; return 1; fi
	wait_until "$ACTIONS_WAIT" act_drawn "$w" "$want" && pass "$label: the $w's stand-in follows" "drawn=$want" ||
		fail "$label: the $w's stand-in follows" "drawn=$(act_field "$w" drawn) wanted=$(act_field "$w" wanted) after $ACTIONS_WAIT s"
	local f1; f1=$(act_field "$w" drawFixes)
	is_num "$f0" && is_num "$f1" && [ "$f1" -gt "$f0" ] && warn "$label: the $w's stand-in's graph followed by itself" "drawn by hand (drawFixes $f0 -> $f1): its graph took no draw event"
	return 0
}

actions_way() {
	local m=$1 w=$2 own
	section "actions: the $m's player acts, the $w's stand-in of it shows it"
	still "$m"
	own=$(act_own "$(dc "$m" actions)")
	if [ "$(field hooked "$own")" != 1 ]; then fail "$m: its player's graph events are heard" "$own"; return; fi
	pass "$m: its player's graph events are heard" "$(cut -c1-120 <<<"$own")"
	act_setup "$m" "$w" || return
	# melee: sword and shield
	act_equip "$m" "$w" right "$ACTIONS_SWORD" "$m holds a sword"
	act_equip "$m" "$w" left "$ACTIONS_SHIELD" "$m holds a shield"
	act_draw "$m" "$w" "$m draws" 1
	act_try "$m" "$w" "$m swings" attack "trigger r 1 150"
	act_try "$m" "$w" "$m power attacks" power "trigger r 1 1200"
	act_try "$m" "$w" "$m blocks" block "trigger l 1 1500"
	act_try "$m" "$w" "$m bashes" bash "trigger l 1 2000" wait:0.6 "trigger r 1 150"
	act_draw "$m" "$w" "$m puts it away" 0
	# the bow
	act_equip "$m" "$w" right "$ACTIONS_BOW" "$m holds a bow"
	act_equip "$m" "$w" ammo "$ACTIONS_ARROW" "$m has arrows"
	act_draw "$m" "$w" "$m draws the bow" 1
	act_try "$m" "$w" "$m draws and looses an arrow" bow "trigger r 1 1500"
	say "actions: the $w's stand-in aims at $(act_field "$w" aim) (its angle x $(act_field "$w" angleX)); its blows $(act_field "$w" blows)"
	act_draw "$m" "$w" "$m puts the bow away" 0
	# magic, each hand
	act_equip "$m" "$w" right "$ACTIONS_FLAMES" "$m readies Flames in the right hand"
	act_equip "$m" "$w" left "$ACTIONS_HEAL" "$m readies Healing in the left hand"
	act_draw "$m" "$w" "$m raises its hands" 1
	act_try "$m" "$w" "$m casts Flames (right, held)" cast "trigger r 1 1500"
	act_try "$m" "$w" "$m casts Healing (left, held)" cast "trigger l 1 1500"
	act_draw "$m" "$w" "$m lowers its hands" 0
	# the voice
	act_equip "$m" "$w" voice "$ACTIONS_SHOUT" "$m readies a shout"
	act_try "$m" "$w" "$m shouts" shout "button rb 300"
	sleep 2  # the shout's cooldown and its push, before the next way
	act_own_drawn "$m" && act_press "$m" "button x 100"
}

step_actions() {
	local w s
	for w in host vm; do still $w; done
	actions_way vm host
	actions_way host vm
	section "actions: what the stand-ins did here, both games"
	for w in host vm; do
		s=$(dc $w actions)
		expect_eq "$w: the magic target hooks are in (a stand-in's spells take hold nowhere)" "$(field character "$s") $(field player "${s#*magicHooks }")" "1 1"
		expect_zero "$w: its stand-in took no health here (its friend's game deals its blows)" "$(act_field $w damaged)"
		warn_gt "$w: events its stand-in's graph turned away" "$(act_field $w refused)" 0
		say "actions: $w: heard $(field heard "$(act_own "$s")") ($(field byKind "$(act_own "$s")")), sent $(field sent "$(act_own "$s")"), dropped $(field dropped "$(act_own "$s")"); its stand-in played $(act_field $w played) ($(act_field $w byKind)), refused $(act_field $w refusedKinds); spells' effects refused $(act_field $w magicRefused), kept on itself $(act_field $w magicKept); blows $(act_field $w blows), casts $(act_field $w casts), health given back $(act_field $w undone)"
	done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init actionstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (PlayerActions::Describe | RemotePlayers::Actions)
	A='actions hooked=1 recording=1 heard=7 byKind=attack:3,power:1,draw:3 sent=7 dropped=0 waiting=0 notOurs=0 last=attackStop | magicHooks character=1 player=1 | player 2 standin=FF0008AE in=9 unplaced=0 waiting=0 lost=0 played=8 byKind=attack:2,block:1,draw:5 refused=1 refusedKinds=power:1 lastRefused=attackPowerStartForward drawn=1 wanted=1 drawFixes=0 aim=-0.20 angleX=-0.20 right=00012EB7:1 left=00012EB6:0 ammo=00000000:1 voice=00013E07/00013E07 gearOk=0 missing=0 refusedForms=0 redressed=1 ammoGiven=60 magicRefused=3 magicKept=1 blows=2 casts=1 damaged=0 dealt=0.0 undone=0.0'
	t "own: our part only" "$(field byKind "$(act_own "$A")")" "attack:3,power:1,draw:3"
	t "friend: the stand-in's part only" "$(field byKind "$(act_friend "$A")")" "attack:2,block:1,draw:5"
	t "friend: none without a friend" "$(act_friend 'actions hooked=1 byKind=none | magicHooks character=0 player=1')" ""
	t "kind: a count" "$(act_kind attack:3,power:1 power)" 1
	t "kind: none of it" "$(act_kind attack:3,power:1 bash)" 0
	t "kind: none at all" "$(act_kind none attack)" 0
	t "kind: attack is not attackpower" "$(act_kind power:4,attack:2 attack)" 2
	t "holds: right" "$(dc() { echo "$A"; }; act_holds host right 00012EB7 && echo yes)" yes
	t "holds: left not on" "$(dc() { echo "$A"; }; act_holds host left 00012EB6 || echo no)" no
	t "voice: both" "$(dc() { echo "$A"; }; act_voice host 00013E07 && echo yes)" yes
	t "drawn" "$(dc() { echo "$A"; }; act_drawn host 1 && echo yes)" yes
	t "own drawn: flags 0x04" "$(dc() { echo 'cell=0 world=3C pos=(1,2,3) flags=0x05 modal=0'; }; act_own_drawn host && echo yes)" yes
	t "own drawn: flags 0x01 is not" "$(dc() { echo 'cell=0 world=3C pos=(1,2,3) flags=0x01 modal=0'; }; act_own_drawn host || echo no)" no

	# the step's way, both games faked: the mover's pad presses make its graph hear that kind, and the watcher's stand-in
	# plays it, as the plugin does. FA_MODE: ok, nohook (the mover's hook is not in), deaf (the presses do nothing: the
	# rig's input), mute (heard and sent, the stand-in plays none), nohold (the stand-in never holds what its friend does),
	# handdraw (each draw the stand-in makes by hand).
	fake_actions() {
		FA=$d/fa.$RANDOM; mkdir -p "$FA"; FA_MODE=$1
		for k in attack power block bash bow cast shout draw; do echo 0 > "$FA/h_$k"; echo 0 > "$FA/p_$k"; done
		echo 0 > "$FA/drawn"; echo 0 > "$FA/fixes"; echo 00000000 > "$FA/right"; echo 00000000 > "$FA/left"; echo 00000000 > "$FA/ammo"; echo 00000000 > "$FA/voice"
		fa_by() { local k out=""; for k in attack power block bash bow cast shout draw; do [ "$(cat "$FA/$1_$k")" -gt 0 ] && out+="${out:+,}$k:$(cat "$FA/$1_$k")"; done; echo "${out:-none}"; }
		fa_add() { echo $(( $(cat "$FA/h_$1") + 1 )) > "$FA/h_$1"; [ "$FA_MODE" = mute ] || echo $(( $(cat "$FA/p_$1") + 1 )) > "$FA/p_$1"; }
		dc() { local v; case "$2" in
			actions) if [ "$1" = vm ]; then
					echo "actions hooked=$([ "$FA_MODE" = nohook ] && echo 0 || echo 1) recording=1 heard=1 byKind=$(fa_by h) sent=1 dropped=0 | magicHooks character=1 player=1 | player 1 standin=FF000801 played=0 byKind=none refused=0 drawn=0 drawFixes=0 damaged=0"
				else
					v=$(cat "$FA/voice")
					echo "actions hooked=1 recording=1 heard=0 byKind=none | magicHooks character=1 player=1 | player 2 standin=FF0008AE played=1 byKind=$(fa_by p) refused=0 refusedKinds=none drawn=$(cat "$FA/drawn") wanted=$(cat "$FA/drawn") drawFixes=$(cat "$FA/fixes") aim=0.00 angleX=0.00 right=$(cat "$FA/right"):1 left=$(cat "$FA/left"):1 ammo=$(cat "$FA/ammo"):1 voice=$v/$v magicRefused=0 magicKept=0 blows=0 casts=0 damaged=0 undone=0.0"
				fi ;;
			'actions equip '*) [ "$FA_MODE" = nohold ] || { set -- $2; echo "$4" > "$FA/$3"; }; echo "equipped" ;;
			'pad button x 100') [ "$FA_MODE" = deaf ] && return; v=$(( 1 - $(cat "$FA/drawn") )); echo "$v" > "$FA/drawn"; fa_add draw
				[ "$FA_MODE" = handdraw ] && echo $(( $(cat "$FA/fixes") + 1 )) > "$FA/fixes"; echo ok ;;
			'pad trigger r 1 150') [ "$FA_MODE" = deaf ] && return
				if [ "$(cat "$FA/h_block")" -gt "$(cat "$FA/h_bash")" ] && [ -f "$FA/blocking" ]; then fa_add bash; rm -f "$FA/blocking"; else fa_add attack; fi; echo ok ;;
			'pad trigger r 1 1200') [ "$FA_MODE" = deaf ] || fa_add power; echo ok ;;
			'pad trigger l 1 1500') [ "$FA_MODE" = deaf ] && return; if [ "$(cat "$FA/left")" = 00012FCC ]; then fa_add cast; else fa_add block; fi; echo ok ;;
			'pad trigger l 1 2000') [ "$FA_MODE" = deaf ] || { touch "$FA/blocking"; fa_add block; }; echo ok ;;
			'pad trigger r 1 1500') [ "$FA_MODE" = deaf ] && return; if [ "$(cat "$FA/right")" = 00013985 ]; then fa_add bow; else fa_add cast; fi; echo ok ;;
			'pad button rb 300') [ "$FA_MODE" = deaf ] || fa_add shout; echo ok ;;
			where) echo "cell=00000000 world=0000003C pos=(100,200,0) flags=0x0$(( $(cat "$FA/drawn") * 4 )) modal=0" ;;
			console*) echo ran ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fa$RANDOM; mkdir -p "$A_OUT"; a_init fa >/dev/null; actions_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: the hook, 7 things held, 6 draws and put-aways, 8 actions played
	t "way: all of it works" "$(fake_actions ok)" "PASS:22 "
	t "way: no hook on the mover's graph fails, and nothing else is tried" "$(fake_actions nohook)" "FAIL:1 "
	# deaf: the hook and the 7 held pass, the 3 put-aways pass (nothing was out); the 3 draws and 8 actions are the rig's
	t "way: presses that do nothing are the rig's input (RIG), not the mod's" "$(fake_actions deaf)" "PASS:11 RIG:11 "
	t "way: events heard and sent that the stand-in never plays fail" "$(fake_actions mute)" "FAIL:8 PASS:14 "
	t "way: a stand-in that never holds its friend's gear fails" "$(fake_actions nohold | grep -o 'FAIL:[0-9]*')" "FAIL:7"
	t "way: a draw the stand-in made by hand is a WARN" "$(fake_actions handdraw | grep -o 'WARN:[0-9]*')" "WARN:6"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "actions is a step" "$(case " $STEPS " in *" actions "*) echo yes ;; esac)" yes
	t "actions comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' actions')" 1
	t "actions needs a session" "$(case " $SESSION_STEPS " in *" actions "*) echo yes ;; esac)" yes
	t "actions runs by default" "$(case " $OPTIN_STEPS " in *" actions "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "actions.sh selftest: all ok" || echo "actions.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
