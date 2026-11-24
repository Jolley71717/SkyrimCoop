#!/bin/bash
# steps/faces.sh: the rig step `faces` (protocol 10, docs/DESIGN.md "Players": "Its face"), sourced by steps.sh. A
# friend's stand-in has their face: it stands on a copy of its preset with their head parts, hair colour, complexion,
# skin, height, face sliders and tint layers put on it, and is placed again when their face changes. Not run in a game
# yet.
#
#   step_faces     the step
#   bash tools/rig/steps/faces.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Faces are compared by their session hash: the face's fingerprint with its forms in the session's numbering, which
# every game computes alike whatever its load order (the mover's sessionHash=, the watcher's sessionFace=).
#
# Each way (the mover's face, the watcher's stand-in of it), both players in one session near each other, after the
# actions step:
#   1. The mover's game reads its player's face (its `faces` read=1; read=0 is a FAIL: no face goes) and has sent it
#      (sentHash= equals hash= within FACES_WAIT: the face is sent once it holds still).
#   2. The watcher's stand-in of it wears it within FACES_WAIT: fit=own (the face is for that stand-in's preset's race
#      and sex), sessionFace= the mover's sessionHash= (the same face), ownBase=1 (the stand-in stands on the copy made
#      for it), shown3d=1 (read from the stand-in's 3D, what the engine built: under its face node, a node named by each
#      of the copy's head parts, none of the preset's parts the copy left out, and the hair's tint the colour that came),
#      agree=1 (the copy holds every part, slider, preset part and tint that came, the hair colour, height, skin tone,
#      complexion and skin) and givenUp=0. Each a FAIL when not, the fields said.
#   3. The watcher in first person (its pad's right stick pressed: the game's own view toggle; a press that does not
#      change the view is a rig fault) for longer than the own-head watch waits: no face given up (oursGivenUp=0,
#      facesGivenUp=0), our head not asked back (repairs=0), and the stand-in still wears the face. Back to third person.
#   4. The mover's face changed (`faces nudge`: a face slider and the hair colour, the head built again; a change that
#      does not take, or leaves the hash as it was, is a rig fault): the mover sends the new face, and the watcher's
#      stand-in is placed again with it (another standin=, the new face, all of step 2). Then `faces nudge undo`: the
#      same face as at first, and the stand-in shows that again.
#   5. Whether the tint layers are drawn on a head built at run time (skin tone, war paint) can only be seen: said in the
#      run for a person to look at; a WARN when the mover sent no tint layer at all (none read from its player). A face
#      node child named by no part of the copy (others3d=) is said too.
# Then a save while a stand-in stands on a copy (the VM saves, never the host; the host's face on the VM's stand-in):
#   6. The VM's stand-in wears the host's face first (a FAIL and no save otherwise: the save path with a copy would not
#      be run). The VM saves under $SAVE_NAME: it lives through it, its stand-in had the preset back as its base
#      (presetsPutBack= up) and the co-save named the preset (presetsNamed= up), no stray; a stand-in given its preset
#      back while still loading its 3D (putBackLoading= up) is a WARN. Its stand-in wears the host's face again after.
#   7. The VM leaves the session and loads that save alone: no stray stand-in (strays=0) and no copy of a preset came
#      back from the save (savedCopies=0: runtime preset NPCs its game did not make). It joins again, and its stand-in
#      wears the host's face. (smoke.sh moves the VM's $SAVE_NAME saves aside when the run ends.)
#   After all: no face given up in either game (facesGivenUp=0: a stand-in that showed no head; oursGivenUp=0: our
#   own head gone while one stood there), and our own head never had to be asked back (repairs=0).
#   Put back: the mover's face (`faces nudge undo` if a nudge is still on), third person.

FACES_WAIT=${FACES_WAIT:-12}  # s: for a face to settle (two reads a second apart), cross the server, and a stand-in to be placed again with it
FACES_POV_HOLD=${FACES_POV_HOLD:-5}  # s: in first person, longer than the own-head watch's 2 s

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# fc_own <faces reply>: our player's part (before the first " | ")
fc_own() { echo "${1%% | *}"; }
# fc_all <faces reply>: the stand-ins' common part (facesGivenUp= ...: between the first " | " and the first "| player ")
fc_all() { local r=${1#* | }; echo "${r%%| player *}"; }
# fc_friend <faces reply>: the friend's stand-in's part (the last "| player " section: one friend on the rig)
fc_friend() { [[ $1 == *"| player "* ]] && echo "player ${1##*| player }"; }
# fc_field <who> <name>: a field of that game's stand-in's part; fc_own_field: of its own; fc_all_field: of the common
fc_field() { field "$2" "$(fc_friend "$(dc "$1" faces)")"; }
fc_own_field() { field "$2" "$(fc_own "$(dc "$1" faces)")"; }
fc_all_field() { field "$2" "$(fc_all "$(dc "$1" faces)")"; }
# fc_sent <who> [local hash]: that game's player's face, read and sent (and, when given, the face of that local hash):
# its session hash; nothing while what was sent is not what is read
fc_sent() {
	local o h s; o=$(fc_own "$(dc "$1" faces)"); h=$(field hash "$o"); s=$(field sessionHash "$o")
	[ "$(field read "$o")" = 1 ] && [ -n "$h" ] && [ "$h" = "$(field sentHash "$o")" ] && { [ -z "${2:-}" ] || [ "$h" = "$2" ]; } &&
		[[ $s =~ ^[0-9A-F]{8}$ ]] && [ "$s" != 00000000 ] && echo "$s"
}
# fc_wears_in <friend part> <session hash>: that stand-in wears that face on its own copy, its 3D showing it, all of it held
fc_wears_in() {
	[ "$(field fit "$1")" = own ] && [ "$(field sessionFace "$1")" = "$2" ] && [ "$(field ownBase "$1")" = 1 ] && [ "$(field shown3d "$1")" = 1 ] &&
		[ "$(field agree "$1")" = 1 ]
}
# fc_wears <who> <session hash>: that game's stand-in of its friend wears that face
fc_wears() { fc_wears_in "$(fc_friend "$(dc "$1" faces)")" "$2"; }
# fc_said <friend part>: what it shows, for a check's reason
fc_said() {
	local p=$1
	echo "fit=$(field fit "$p") sessionFace=$(field sessionFace "$p") standin=$(field standin "$p") base=$(field base "$p") preset=$(field preset "$p") copy=$(field copy "$p") ownBase=$(field ownBase "$p") head=$(field head "$p") watch=$(field watch "$p") givenUp=$(field givenUp "$p") shown3d=$(field shown3d "$p") parts3d=$(field parts3d "$p") presetShown=$(field presetShown "$p") hairTint=$(field hairTint "$p") others3d=$(field others3d "$p") agree=$(field agree "$p") $(sed -n 's/.*\(parts=[0-9]*\/[0-9]* .*skin=[01]\).*/\1/p' <<<"$p")"
}
# fc_first <who> [0|1]: that game's player is in first person (or, given 0, not)
fc_first() { [ "$(fc_own_field "$1" firstPerson)" = "${2:-1}" ]; }

# fc_judge <watcher> <mover> <session hash> <label>: the watcher's stand-in wears that face (waits FACES_WAIT), each
# piece its check
fc_judge() {
	local w=$1 m=$2 h=$3 label=$4 p
	wait_until "$FACES_WAIT" fc_wears "$w" "$h"
	p=$(fc_friend "$(dc "$w" faces)")
	expect_eq "$label: the $w's stand-in of the $m's player takes its face (fit=)" "$(field fit "$p")" own
	expect_eq "$label: the $w's stand-in has the $m's face (sessionFace= the $m's sessionHash=)" "$(field sessionFace "$p")" "$h"
	expect_eq "$label: the $w's stand-in stands on the copy made for it (ownBase=)" "$(field ownBase "$p")" 1
	if [ "$(field shown3d "$p")" = 1 ]; then
		pass "$label: the $w's stand-in's built head shows the copy's parts, none of the preset's, its hair's colour (shown3d=)" "$(fc_said "$p")"
	else
		fail "$label: the $w's stand-in's built head shows the copy's parts, none of the preset's, its hair's colour (shown3d=)" "$(fc_said "$p")"
	fi
	if [ "$(field agree "$p")" = 1 ]; then
		pass "$label: the copy holds all of the $m's face (agree=)" "$(fc_said "$p")"
	else
		fail "$label: the copy holds all of the $m's face (agree=)" "$(fc_said "$p")"
	fi
	expect_eq "$label: the $w's stand-in's face is not given up (givenUp=)" "$(field givenUp "$p")" 0
	if [ -n "$(field others3d "$p")" ] && [ "$(field others3d "$p")" != 0 ]; then
		say "$label: the $w's stand-in's face node also has $(field others3d "$p") node(s) named by no part of its copy (a face of before? an overlay?): for a person to look at"
	fi
}

# faces_pov <watcher> <mover> <session hash>: the watcher in first person (its pad's view toggle) for FACES_POV_HOLD;
# nothing given up, our head not asked back, the stand-in keeps the face; then third person again
faces_pov() {
	local w=$1 m=$2 h=$3 r s was
	was=$(fc_own_field "$w" firstPerson)
	if [ "$was" != 1 ]; then
		r=$(dc "$w" 'pad button rs 100')
		if [[ $r != button* ]] || ! wait_until 5 fc_first "$w"; then
			rig_fault input "first person: the $w's view toggle (the right stick pressed)" "pad: $r; firstPerson=$(fc_own_field "$w" firstPerson)"
			return
		fi
	fi
	say "first person: the $w's player in first person (its pad's right stick: the game's own view toggle) for ${FACES_POV_HOLD} s, watching the $m's stand-in"
	sleep "$FACES_POV_HOLD"
	s=$(dc "$w" faces)
	expect_eq "first person: the $w gives no stand-in's face up for its own head (oursGivenUp=)" "$(field oursGivenUp "$(fc_all "$s")")" 0
	expect_eq "first person: the $w gives no stand-in's face up for no head (facesGivenUp=)" "$(field facesGivenUp "$(fc_all "$s")")" 0
	expect_eq "first person: the $w's own head is not asked back (repairs=)" "$(field repairs "$(fc_own "$s")")" 0
	if fc_wears_in "$(fc_friend "$s")" "$h"; then
		pass "first person: the $w's stand-in of the $m's player keeps its face" "$(fc_said "$(fc_friend "$s")")"
	else
		fail "first person: the $w's stand-in of the $m's player keeps its face" "$(fc_said "$(fc_friend "$s")")"
	fi
	if [ "$was" != 1 ]; then
		r=$(dc "$w" 'pad button rs 100')
		if [[ $r != button* ]] || ! wait_until 5 fc_first "$w" 0; then
			rig_fault input "first person: the $w back in third person (the right stick pressed)" "pad: $r; firstPerson=$(fc_own_field "$w" firstPerson)"
		fi
	fi
}

faces_way() {
	local m=$1 w=$2 l0 l1 h0 h1 back r s0 s1 own tints
	section "faces: the $m's face on the $w's stand-in"
	if ! wait_until "$FACES_WAIT" placed "$w"; then
		fail "faces: the $w has a stand-in of the $m's player" "$(dc "$w" standins | cut -c1-200)"
		return
	fi
	own=$(fc_own "$(dc "$m" faces)")
	if [ "$(field read "$own")" != 1 ]; then
		fail "faces: the $m's game reads its player's face (read=)" "${own:0:200}"
		return
	fi
	if ! h0=$(wait_until "$FACES_WAIT" fc_sent "$m"); then
		fail "faces: the $m's game sent its player's face (sentHash= its hash=)" "hash=$(fc_own_field "$m" hash) sentHash=$(fc_own_field "$m" sentHash) sessionHash=$(fc_own_field "$m" sessionHash) sent=$(fc_own_field "$m" sent)"
		return
	fi
	l0=$(fc_own_field "$m" hash)
	pass "faces: the $m's game sent its player's face" "hash=$l0 sessionHash=$h0 parts=$(field parts "$own") tints=$(field tints "$own") morphsSet=$(field morphsSet "$own")"
	fc_judge "$w" "$m" "$h0" "faces"
	faces_pov "$w" "$m" "$h0"

	# its face changed: a new stand-in with the new face
	s0=$(fc_field "$w" standin)
	r=$(dc "$m" "faces nudge")
	l1=$(field hash "$r")
	if [[ $r != nudged* ]] || ! [[ $l1 =~ ^[0-9A-F]{8}$ ]] || [ "$l1" = "$l0" ]; then
		rig_fault state "faces: the $m's face changed (set-up)" "faces nudge: $r (the face was $l0)"
		[[ $r == nudged* ]] && dc "$m" "faces nudge undo" >/dev/null
		return
	fi
	say "faces: the $m's face changed for the run (a face slider and the hair colour; dev faces nudge): $l0 -> $l1"
	if h1=$(wait_until "$FACES_WAIT" fc_sent "$m" "$l1") && [ "$h1" != "$h0" ]; then
		pass "nudged: the $m's game sent its changed face" "hash=$l1 sessionHash=$h1"
	else
		fail "nudged: the $m's game sent its changed face" "hash=$(fc_own_field "$m" hash) sentHash=$(fc_own_field "$m" sentHash) sessionHash=$(fc_own_field "$m" sessionHash) (want $l1)"
		h1=$(fc_own_field "$m" sessionHash)
	fi
	fc_judge "$w" "$m" "$h1" "nudged"
	s1=$(fc_field "$w" standin)
	if [ -n "$s1" ] && [ "$s1" != "$s0" ] && [ "$s1" != 00000000 ]; then
		pass "nudged: the $w's stand-in was placed again with it" "standin $s0 -> $s1, remakes=$(fc_field "$w" remakes)"
	else
		fail "nudged: the $w's stand-in was placed again with it" "standin $s0 -> ${s1:-none}, remakes=$(fc_field "$w" remakes)"
	fi

	# and back
	r=$(dc "$m" "faces nudge undo")
	if [[ $r != undone* ]]; then
		rig_fault state "faces: the $m's face put back (set-up)" "faces nudge undo: $r"
		return
	fi
	expect_eq "undone: the $m's face is as it was (the same hash)" "$(field hash "$r")" "$l0"
	back=$(wait_until "$FACES_WAIT" fc_sent "$m" "$l0")
	expect_eq "undone: the $m's game sent its first face again (the same session hash)" "$back" "$h0"
	fc_judge "$w" "$m" "$h0" "undone"

	# what only a person can see
	tints=$(field tints "$own")
	if [ "$tints" = 0 ]; then
		warn "faces: the $m's player has tint layers to send (skin tone, paint)" "tints=0: no tint mask read from its player (ourTintsLeft=$(field ourTintsLeft "$own")); its skin tone is the preset's on the $w's stand-in"
	else
		say "faces: the $w's stand-in carries the $m's $tints tint layers on its copy (agree= checked them); whether a head built at run time draws them (skin tone, paint) is for a person to look at"
	fi
}

# faces_save: the VM saves while its stand-in of the host's player stands on a copy, then loads that save alone, then
# joins again (the host never saves or loads)
faces_save() {
	local h s r saves pp0 pn0 pl0 pl
	section "faces: the VM saves while its stand-in of the host's player stands on a copy (the host never saves)"
	if ! h=$(wait_until "$FACES_WAIT" fc_sent host) || ! wait_until "$FACES_WAIT" fc_wears vm "$h"; then
		fail "faces save: the VM's stand-in wears the host's face before the save (else the save with a copy is not tried)" "$(fc_said "$(fc_friend "$(dc vm faces)")") (the host's sessionHash=${h:-none})"
		return
	fi
	s=$(dc vm faces)
	pp0=$(field presetsPutBack "$(fc_all "$s")"); pn0=$(field presetsNamed "$(fc_all "$s")"); pl0=$(field putBackLoading "$(fc_all "$s")")
	saves=$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)
	r=$(dc vm "console save $SAVE_NAME"); [ "$r" = ran ] || { fail "faces save: the VM saves" "the console refused: $r"; return; }
	sleep 8
	if [ "$(dc vm ping)" != pong ]; then
		fail "faces save: the VM's game lives through its save with a stand-in on a copy" "its dev channel went silent after the save (a crash? see the crash scan)"
		_rig_stop "the VM's game went down" "after its save with a stand-in on a copy"
		return
	fi
	pass "faces save: the VM's game lives through its save with a stand-in on a copy" "its dev channel answers"
	expect_ge "faces save: the VM's log says it saved" "$(log_since_mark vm "Save: saving \"$SAVE_NAME" | wc -l)" $((saves + 1))
	s=$(dc vm faces)
	if is_num "$pp0" && is_num "$pn0"; then
		expect_ge "faces save: the stand-in had its preset back as its base before the save (presetsPutBack=)" "$(field presetsPutBack "$(fc_all "$s")")" $((pp0 + 1))
		expect_ge "faces save: the co-save named the preset, not the copy (presetsNamed=)" "$(field presetsNamed "$(fc_all "$s")")" $((pn0 + 1))
	else
		fail "faces save: the VM's counts before the save" "presetsPutBack=${pp0:-none} presetsNamed=${pn0:-none}"
	fi
	pl=$(field putBackLoading "$(fc_all "$s")")
	if is_num "$pl" && is_num "$pl0" && [ "$pl" -gt "$pl0" ]; then
		warn "faces save: no stand-in given its preset back while its 3D was still loading (putBackLoading=)" "$pl0 -> $pl: a head may have been built half from the copy, half from the preset; see the VM's log"
	fi
	expect_zero "faces save: no stray stand-in after the save (strays=)" "$(field strays "$(dc vm strays)")"
	if wait_until "$FACES_WAIT" fc_wears vm "$h"; then
		pass "faces save: the VM's stand-in wears the host's face again after the save" "$(fc_said "$(fc_friend "$(dc vm faces)")")"
	else
		fail "faces save: the VM's stand-in wears the host's face again after the save" "$(fc_said "$(fc_friend "$(dc vm faces)")")"
	fi

	section "faces: the VM loads that save alone (out of the session)"
	r=$(dc vm 'session leave'); [[ $r == left* ]] || fail "faces load: the VM leaves" "$r"
	gone_from host || fail "faces load: the host's stand-in of the VM goes when it leaves" "$(dc host standins | cut -c1-200)"
	r=$(dc vm "console load $SAVE_NAME"); [ "$r" = ran ] || { fail "faces load: the VM loads its save" "the console refused: $r"; return; }
	sleep 5
	if ! wait_until 90 in_world vm; then fail "faces load: the VM is back in the world after its load" "$(dc vm where | cut -c1-160)"; return; fi
	sleep 5
	s=$(dc vm faces)
	expect_zero "faces load: no stray stand-in came back from the save (strays=)" "$(field strays "$(dc vm strays)")"
	expect_zero "faces load: no copy of a preset came back from the save (savedCopies=: runtime preset NPCs this game did not make)" "$(field savedCopies "$(fc_own "$s")")"
	say "faces load: runtime preset NPCs in the VM's game after the load: $(field runtimePresets "$(fc_own "$s")") (its own copies of before the load, if a load keeps them)"

	if r=$(vm_join) && wait_until 90 joined_both; then pass "faces load: the VM joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "faces load: the VM joins again" "$r"; return; fi
	if wait_until 30 placed vm && wait_until "$FACES_WAIT" fc_wears vm "$h"; then
		pass "faces load: the VM's stand-in wears the host's face after the load and the rejoin" "$(fc_said "$(fc_friend "$(dc vm faces)")")"
	else
		fail "faces load: the VM's stand-in wears the host's face after the load and the rejoin" "$(fc_said "$(fc_friend "$(dc vm faces)")")"
	fi
}

step_faces() {
	local w s
	for w in host vm; do still $w; done
	faces_way vm host
	faces_way host vm
	faces_save
	section "faces: what the stand-ins' faces did, both games"
	for w in host vm; do
		[ "$(fc_own_field $w nudged)" = 1 ] && dc $w "faces nudge undo" >/dev/null
		fc_first $w && dc $w 'pad button rs 100' >/dev/null
		s=$(dc $w faces)
		expect_eq "$w: no stand-in's face given up for no head (facesGivenUp=)" "$(field facesGivenUp "$(fc_all "$s")")" 0
		expect_eq "$w: no stand-in's face given up for our own head going (oursGivenUp=)" "$(field oursGivenUp "$(fc_all "$s")")" 0
		expect_eq "$w: our own head was never asked back (repairs=)" "$(field repairs "$(fc_own "$s")")" 0
		say "faces: $w: copies made $(field copiesMade "$(fc_own "$s")") (kept $(field copiesKept "$(fc_own "$s")"), written again $(field copiesRewritten "$(fc_own "$s")"), refused $(field refused "$(fc_own "$s")")), colours made $(field colors "$(fc_own "$s")"), tint layers left out $(field tintsLeft "$(fc_own "$s")"), heads built again $(field rebuilds "$(fc_own "$s")"), presets put back for saves $(field presetsPutBack "$(fc_all "$s")")"
	done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init facestest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	FACES_POV_HOLD=0

	# a reply in the format the plugin writes (Face::Describe | RemotePlayers::Faces)
	F='read=1 hash=1A2B3C4D sessionHash=5A5B5C5D race=00013746 female=0 parts=7 types=0x5e hair=000A0435/FF141E28 details=0001A28B skin=00000000 height=1.000 morphsSet=19 tints=12 body=FF8CA0C8 ourTintsLeft=0 sent=2 sentHash=1A2B3C4D nudged=0 copiesMade=3 copiesKept=1 copiesRewritten=2 full=0 refused=0 colors=1 tintsLeft=0 rebuilds=0 repairs=0 repairsRefused=0 firstPerson=0 editing=0 ourHead=1 ourHeadJudged=1 ourHeadRace=00013746 runtimePresets=1 savedCopies=0 | facesGivenUp=0 oursGivenUp=0 presetsPutBack=2 presetsNamed=2 putBackLoading=0 oursRepaired=0 | player 2 face=99887766 sessionFace=9A9B9C9D in=2 fit=own standin=FF0008AE base=FF000901 preset=00013A2E copy=FF000901 shown=99887766 ownBase=1 head=1 watch=built givenUp=0 remakes=1 shown3d=1 face3d=1 parts3d=7/7 presetShown=0/2 hairTint=1 others3d=0 agree=1 parts=7/7 morphs=19 presets=4 tints=12/12 hair=1 height=1 body=1 details=1 skin=1'
	t "own: our part only" "$(field hash "$(fc_own "$F")")" 1A2B3C4D
	t "own: no stand-in's fields" "$(field fit "$(fc_own "$F")")" ""
	t "all: the common part" "$(field presetsNamed "$(fc_all "$F")")" 2
	t "all: not the friend's" "$(field fit "$(fc_all "$F")")" ""
	t "friend: the stand-in's part" "$(field sessionFace "$(fc_friend "$F")")" 9A9B9C9D
	t "friend: its tints, not ours" "$(field tints "$(fc_friend "$F")")" 12/12
	t "friend: its face=, not its face3d=" "$(field face "$(fc_friend "$F")")" 99887766
	t "friend: none without a friend" "$(fc_friend 'read=1 hash=1 | facesGivenUp=0 oursGivenUp=0')" ""
	t "sent: what was read went, by its session hash" "$(dc() { echo "$F"; }; fc_sent host)" 5A5B5C5D
	t "sent: that face" "$(dc() { echo "$F"; }; fc_sent host 1A2B3C4D)" 5A5B5C5D
	t "sent: not another face" "$(dc() { echo "$F"; }; fc_sent host 11111111 || echo no)" no
	t "sent: not while another is read" "$(dc() { echo "${F/sentHash=1A2B3C4D/sentHash=00000000}"; }; fc_sent host || echo no)" no
	t "sent: not when nothing is read" "$(dc() { echo 'read=0 sent=0 sentHash=00000000 | facesGivenUp=0'; }; fc_sent host || echo no)" no
	t "sent: not with no session hash" "$(dc() { echo "${F/sessionHash=5A5B5C5D/sessionHash=00000000}"; }; fc_sent host || echo no)" no
	t "wears: all of it" "$(fc_wears_in "$(fc_friend "$F")" 9A9B9C9D && echo yes)" yes
	t "wears: by the session hash, not this game's" "$(fc_wears_in "$(fc_friend "$F")" 99887766 || echo no)" no
	t "wears: the preset's face built (shown3d=0)" "$(fc_wears_in "$(fc_friend "${F/shown3d=1/shown3d=0}")" 9A9B9C9D || echo no)" no
	t "wears: the preset's base" "$(fc_wears_in "$(fc_friend "${F/ownBase=1/ownBase=0}")" 9A9B9C9D || echo no)" no
	t "wears: not all held" "$(fc_wears_in "$(fc_friend "${F/agree=1/agree=0}")" 9A9B9C9D || echo no)" no
	t "said: the pieces" "$(fc_said "$(fc_friend "$F")" | grep -o 'parts=7/7 .* skin=1')" "parts=7/7 morphs=19 presets=4 tints=12/12 hair=1 height=1 body=1 details=1 skin=1"
	t "said: the 3D's" "$(fc_said "$(fc_friend "$F")" | grep -o 'shown3d=1 parts3d=7/7 presetShown=0/2 hairTint=1 others3d=0')" "shown3d=1 parts3d=7/7 presetShown=0/2 hairTint=1 others3d=0"
	t "first: not in first person" "$(dc() { echo "$F"; }; fc_first host || echo no)" no
	t "first: in third person" "$(dc() { echo "$F"; }; fc_first host 0 && echo yes)" yes

	# The step's way, both games faked: the mover's face is read and sent, the watcher's stand-in placed again on a copy
	# with each face it gets, as the plugin does. FF_MODE: ok, noread (the mover reads no face), nosend (its face never
	# goes), preset3d (the engine builds the preset's head: the copy holds the face, its 3D does not show it), partial
	# (the copy holds less than came), stale (the watcher never takes a changed face), stuck (the nudge does nothing: the
	# rig's), notints (the mover reads no tint layer), nopov (the view toggle does nothing: the rig's), povgiveup (in first
	# person the watcher gives the face up: the bug of before).
	fake_faces() {
		FF=$d/ff.$RANDOM; mkdir -p "$FF"; FF_MODE=$1
		echo 1A2B3C4D > "$FF/hash"; echo 0 > "$FF/nudged"; echo 1A2B3C4D > "$FF/shown"; echo 100 > "$FF/standin"; echo 0 > "$FF/remakes"; echo 0 > "$FF/pov"
		ff_hex() { printf 'FF000%03X' "$(cat "$FF/standin")"; }
		ff_session() { echo "5${1:1}"; }  # the session hash of a local one (another load order would number it otherwise)
		dc() { local h n pov gu; case "$2" in
			faces) if [ "$1" = vm ]; then
					h=$(cat "$FF/hash")
					[ "$FF_MODE" = noread ] && { echo "read=0 ourTintsLeft=0 sent=0 sentHash=00000000 nudged=0 repairs=0 | facesGivenUp=0 oursGivenUp=0 presetsPutBack=0 | player 1 face=none fit=noface"; return; }
					echo "read=1 hash=$h sessionHash=$(ff_session "$h") race=00013746 female=0 parts=7 tints=$([ "$FF_MODE" = notints ] && echo 0 || echo 12) morphsSet=19 ourTintsLeft=0 sent=1 sentHash=$([ "$FF_MODE" = nosend ] && echo 00000000 || echo "$h") nudged=$(cat "$FF/nudged") copiesMade=1 rebuilds=0 repairs=0 firstPerson=0 savedCopies=0 | facesGivenUp=0 oursGivenUp=0 presetsPutBack=0 presetsNamed=0 putBackLoading=0 oursRepaired=0 | player 1 sessionFace=CAFEF00D fit=own standin=FF000700 ownBase=1 shown3d=1 agree=1 givenUp=0"
				else
					h=$(cat "$FF/shown"); pov=$(cat "$FF/pov"); gu=0
					[ "$FF_MODE" = povgiveup ] && [ "$pov" = 1 ] && { gu=1; echo 1 > "$FF/gaveup"; }
					[ -f "$FF/gaveup" ] && gu=1
					[ "$FF_MODE" = nosend ] && h=none
					echo "read=1 hash=CAFEF00D sessionHash=CAFEF00D sentHash=CAFEF00D tints=12 repairs=0 copiesMade=2 firstPerson=$pov | facesGivenUp=0 oursGivenUp=$gu presetsPutBack=1 presetsNamed=1 putBackLoading=0 oursRepaired=0 | player 2 face=$h sessionFace=$([ "$h" = none ] && echo 00000000 || ff_session "$h") in=2 fit=$([ "$h" = none ] && echo noface || ([ "$gu" = 1 ] && echo givenup) || echo own) standin=$(ff_hex) base=FF000901 preset=00013A2E copy=FF000901 shown=$h ownBase=$([ "$gu" = 1 ] && echo 0 || echo 1) head=1 watch=built givenUp=$gu remakes=$(cat "$FF/remakes") shown3d=$([ "$FF_MODE" = preset3d ] && echo 0 || echo 1) face3d=1 parts3d=$([ "$FF_MODE" = preset3d ] && echo 3/7 || echo 7/7) presetShown=$([ "$FF_MODE" = preset3d ] && echo 2/2 || echo 0/2) hairTint=1 others3d=0 agree=$([ "$FF_MODE" = partial ] && echo 0 || echo 1) parts=7/7 morphs=19 presets=4 tints=$([ "$FF_MODE" = partial ] && echo 9/12 || echo 12/12) hair=1 height=1 body=1 details=1 skin=1"
				fi ;;
			'pad button rs 100')
				[ "$FF_MODE" = nopov ] || { [ "$(cat "$FF/pov")" = 1 ] && echo 0 > "$FF/pov" || echo 1 > "$FF/pov"; }
				echo "button rs 100" ;;
			'faces nudge')
				[ "$FF_MODE" = stuck ] && { echo "nudged hash=$(cat "$FF/hash")"; echo 1 > "$FF/nudged"; return; }
				echo 1E5E5E5E > "$FF/hash"; echo 1 > "$FF/nudged"
				[ "$FF_MODE" = stale ] || { echo 1E5E5E5E > "$FF/shown"; n=$(cat "$FF/standin"); echo $((n + 1)) > "$FF/standin"; echo $(( $(cat "$FF/remakes") + 1 )) > "$FF/remakes"; }
				echo "nudged hash=1E5E5E5E" ;;
			'faces nudge undo')
				[ "$(cat "$FF/nudged")" = 1 ] || { echo "error: nothing nudged"; return; }
				echo 1A2B3C4D > "$FF/hash"; echo 0 > "$FF/nudged"
				[ "$FF_MODE" = stale ] || { echo 1A2B3C4D > "$FF/shown"; n=$(cat "$FF/standin"); echo $((n + 1)) > "$FF/standin"; }
				echo "undone hash=1A2B3C4D" ;;
			standins) echo "players=1 | player 2 standin=$(ff_hex) placed=1 dressed=1 at=(100,200,0) heading=0.000" ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/ff$RANDOM; mkdir -p "$A_OUT"; a_init ff >/dev/null; faces_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: the face sent (1), its six checks (6), first person's four (4), the changed face sent (1), its six (6), the
	# stand-in placed again (1), the same hash back and sent again (2), its six (6): 27
	t "way: all of it works" "$(fake_faces ok)" "PASS:27 "
	t "way: a mover that reads no face fails, and nothing else is tried" "$(fake_faces noread)" "FAIL:1 "
	t "way: a face never sent fails, and nothing else is tried" "$(fake_faces nosend)" "FAIL:1 "
	# preset3d: shown3d fails each time it is judged (3), and in first person the stand-in does not "keep its face" (1)
	t "way: a stand-in whose built head is the preset's fails, though its copy holds the face" "$(fake_faces preset3d | grep -o 'FAIL:[0-9]*')" "FAIL:4"
	t "way: a copy that holds less than came fails each time it is judged" "$(fake_faces partial | grep -o 'FAIL:[0-9]*')" "FAIL:4"
	# stale: the changed face never reaches the stand-in: its sessionFace= (the rest still holds) and not placed again;
	# then undone is the first face again, which it still shows
	t "way: a changed face the stand-in never takes fails" "$(fake_faces stale | grep -o 'FAIL:[0-9]*')" "FAIL:2"
	t "way: a nudge that changes nothing is the rig's, and ends the way" "$(fake_faces stuck | grep -o 'FAIL:[0-9]*\|RIG[A-Z]*:[0-9]*' | tr '\n' ' ')" "RIG:1 "
	t "way: no tint layer read is a WARN" "$(fake_faces notints | grep -o 'WARN:[0-9]*')" "WARN:1"
	t "way: a view toggle that does nothing is the rig's, and the way goes on" "$(fake_faces nopov | grep -o 'FAIL:[0-9]*\|RIG[A-Z]*:[0-9]*\|PASS:[0-9]*' | tr '\n' ' ')" "PASS:23 RIG:1 "
	t "way: a face given up in first person fails its first-person checks" "$( (fake_faces povgiveup | grep -qE 'FAIL:([2-9]|[1-9][0-9])') && echo yes)" yes

	# The save: the VM saves with its stand-in on a copy, loads alone, joins again. FS_MODE: ok, nowear (the VM's
	# stand-in never wears the host's face: no save tried), noputback (the save left the stand-in on its copy), named0
	# (the co-save named the copy), loading (put back while loading: a WARN), saved (a copy came back from the save),
	# crash (the VM's game goes down at the save).
	fake_save() {
		FS=$d/fs.$RANDOM; mkdir -p "$FS"; FS_MODE=$1; echo 0 > "$FS/saved"; echo 1 > "$FS/in"
		_rig_stop() { record FAIL "stopped" "$1"; }
		log_since_mark() { [ "$(cat "$FS/saved")" = 1 ] && echo "Save: saving \"$SAVE_NAME\""; }
		vm_join() { echo 1 > "$FS/in"; }
		joined_both() { true; }
		gone_from() { true; }
		in_world() { true; }
		dc() { local s pp pn pl sc; case "$1 $2" in
			"host faces") echo "read=1 hash=1A2B3C4D sessionHash=5A2B3C4D sentHash=1A2B3C4D | facesGivenUp=0 oursGivenUp=0 | player 2 sessionFace=CAFEF00D fit=own" ;;
			"vm faces")
				s=$(cat "$FS/saved"); pp=$s; pn=$s; pl=0; sc=0
				[ "$FS_MODE" = noputback ] && pp=0
				[ "$FS_MODE" = named0 ] && pn=0
				[ "$FS_MODE" = loading ] && pl=$s
				[ "$FS_MODE" = saved ] && sc=$s
				echo "read=1 hash=CAFEF00D sessionHash=CAFEF00D sentHash=CAFEF00D repairs=0 firstPerson=0 runtimePresets=$((1 + sc)) savedCopies=$sc | facesGivenUp=0 oursGivenUp=0 presetsPutBack=$pp presetsNamed=$pn putBackLoading=$pl oursRepaired=0 | player 1 sessionFace=$([ "$FS_MODE" = nowear ] && echo 00000000 || echo 5A2B3C4D) fit=own standin=FF000801 ownBase=1 shown3d=1 agree=1 givenUp=0" ;;
			"vm console save"*) echo 1 > "$FS/saved"; echo ran ;;
			"vm console load"*) echo ran ;;
			"vm ping") [ "$FS_MODE" = crash ] && [ "$(cat "$FS/saved")" = 1 ] && echo "" || echo pong ;;
			"vm session leave") echo 0 > "$FS/in"; echo "left role=none" ;;
			"vm session status") echo "role=friend connection=up welcomed=1" ;;
			"vm strays") echo "strays=0 standins=1 presetActors=1" ;;
			*" standins") echo "players=1 | player 1 standin=FF000801 placed=1 dressed=1 at=(1,2,3)" ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/fs$RANDOM; mkdir -p "$A_OUT"; a_init fs >/dev/null; faces_save >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: lives (1), its log (1), put back (1), named (1), no stray (1), wears again (1); load: no stray (1), no copy (1);
	# joins again (1), wears after (1): 10
	t "save: all of it works" "$(fake_save ok)" "PASS:10 "
	t "save: no save tried when the stand-in never wore the face" "$(fake_save nowear)" "FAIL:1 "
	t "save: a stand-in left on its copy for the save fails" "$(fake_save noputback | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	t "save: a co-save that named the copy fails" "$(fake_save named0 | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	t "save: put back while loading is a WARN" "$(fake_save loading | grep -o 'WARN:[0-9]*\|FAIL:[0-9]*' | tr '\n' ' ')" "WARN:1 "
	t "save: a copy that came back from the save fails" "$(fake_save saved | grep -o 'FAIL:[0-9]*')" "FAIL:1"
	t "save: a game that goes down at the save stops the run" "$(fake_save crash | grep -o 'FAIL:[0-9]*')" "FAIL:2"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "faces is a step" "$(case " $STEPS " in *" faces "*) echo yes ;; esac)" yes
	t "faces comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' faces')" 1
	t "faces needs a session" "$(case " $SESSION_STEPS " in *" faces "*) echo yes ;; esac)" yes
	t "faces runs by default" "$(case " $OPTIN_STEPS " in *" faces "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "faces.sh selftest: all ok" || echo "faces.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
