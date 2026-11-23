#!/bin/bash
# steps/faces.sh: the rig step `faces` (protocol 10, docs/DESIGN.md "Players": "Its face"), sourced by steps.sh. A
# friend's stand-in has their face: it stands on a copy of its preset with their head parts, hair colour, complexion,
# skin, height, face sliders and tint layers put on it, and is placed again when their face changes. Not run in a game
# yet.
#
#   step_faces     the step
#   bash tools/rig/steps/faces.sh selftest     the step's reading, no game and no rig (CI runs it)
#
# Each way (the mover's face, the watcher's stand-in of it), both players in one session near each other, after the
# actions step:
#   1. The mover's game reads its player's face (its `faces` read=1; read=0 is a FAIL: no face goes) and has sent it
#      (sentHash= equals hash= within FACES_WAIT: the face is sent once it holds still).
#   2. The watcher's stand-in of it wears it within FACES_WAIT: fit=own (the face is for that stand-in's preset's race
#      and sex), face= the mover's hash (the same face, as the watcher's game read it), ownBase=1 (the stand-in stands on
#      the copy made for it), head=1 (the engine built that copy's head: a face node on its 3D), agree=1 (the copy holds
#      every part, slider, preset part and tint that came, the hair colour, height, skin tone, complexion and skin), and
#      givenUp=0. Each a FAIL when not, the fields said.
#   3. The mover's face changed (`faces nudge`: a face slider and the hair colour, the head built again; a change that
#      does not take, or leaves the hash as it was, is a rig fault): the mover sends the new face (sentHash= the new
#      hash), and the watcher's stand-in is placed again on a new copy with it (another standin=, face= the new hash,
#      fit=own, ownBase=1, head=1, agree=1). Then `faces nudge undo`: the hash back to the first, and the stand-in shows
#      that again.
#   4. Whether the tint layers are drawn on a head built at run time (skin tone, war paint) can only be seen: said in the
#      run for a person to look at; a WARN when the mover sent no tint layer at all (none read from its player).
#   After both ways: no face given up in either game (facesGivenUp=0: a stand-in that showed no head; oursGivenUp=0: our
#   own head gone while one stood there), and our own head never had to be asked back (repairs=0).
#   Put back: the mover's face (`faces nudge undo` if a nudge is still on).

FACES_WAIT=${FACES_WAIT:-12}  # s: for a face to settle (two reads a second apart), cross the server, and a stand-in to be placed again with it

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
# fc_sent <who>: that game's player's face, read and sent (its hash; empty while what was sent is not what is read)
fc_sent() { local o; o=$(fc_own "$(dc "$1" faces)"); [ "$(field read "$o")" = 1 ] && [ -n "$(field hash "$o")" ] && [ "$(field hash "$o")" = "$(field sentHash "$o")" ] && field hash "$o"; }
# fc_wears_in <friend part> <hash>: that stand-in wears that face on its own copy, head built, all of it held
fc_wears_in() {
	[ "$(field fit "$1")" = own ] && [ "$(field face "$1")" = "$2" ] && [ "$(field ownBase "$1")" = 1 ] && [ "$(field head "$1")" = 1 ] && [ "$(field agree "$1")" = 1 ]
}
# fc_wears <who> <hash>: that game's stand-in of its friend wears that face
fc_wears() { fc_wears_in "$(fc_friend "$(dc "$1" faces)")" "$2"; }
# fc_said <friend part>: what it shows, for a check's reason
fc_said() {
	local p=$1
	echo "fit=$(field fit "$p") face=$(field face "$p") standin=$(field standin "$p") base=$(field base "$p") preset=$(field preset "$p") copy=$(field copy "$p") ownBase=$(field ownBase "$p") head=$(field head "$p") watch=$(field watch "$p") givenUp=$(field givenUp "$p") agree=$(field agree "$p") $(sed -n 's/.*\(parts=[0-9]*\/[0-9]* .*skin=[01]\).*/\1/p' <<<"$p")"
}

# fc_judge <watcher> <mover> <hash> <label>: the watcher's stand-in wears that face (waits FACES_WAIT), each piece its check
fc_judge() {
	local w=$1 m=$2 h=$3 label=$4 p
	wait_until "$FACES_WAIT" fc_wears "$w" "$h"
	p=$(fc_friend "$(dc "$w" faces)")
	expect_eq "$label: the $w's stand-in of the $m's player takes its face (fit=)" "$(field fit "$p")" own
	expect_eq "$label: the $w's stand-in has the $m's face (face= the $m's hash)" "$(field face "$p")" "$h"
	expect_eq "$label: the $w's stand-in stands on the copy made for it (ownBase=)" "$(field ownBase "$p")" 1
	expect_eq "$label: the engine built the $w's stand-in's head from the copy (head=)" "$(field head "$p")" 1
	if [ "$(field agree "$p")" = 1 ]; then
		pass "$label: the copy holds all of the $m's face (agree=)" "$(fc_said "$p")"
	else
		fail "$label: the copy holds all of the $m's face (agree=)" "$(fc_said "$p")"
	fi
	expect_eq "$label: the $w's stand-in's face is not given up (givenUp=)" "$(field givenUp "$p")" 0
}

faces_way() {
	local m=$1 w=$2 h0 h1 back r s0 s1 own tints
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
		fail "faces: the $m's game sent its player's face (sentHash= its hash=)" "hash=$(fc_own_field "$m" hash) sentHash=$(fc_own_field "$m" sentHash) sent=$(fc_own_field "$m" sent)"
		return
	fi
	pass "faces: the $m's game sent its player's face" "hash=$h0 parts=$(field parts "$own") tints=$(field tints "$own") morphsSet=$(field morphsSet "$own")"
	fc_judge "$w" "$m" "$h0" "faces"

	# its face changed: a new stand-in with the new face
	s0=$(fc_field "$w" standin)
	r=$(dc "$m" "faces nudge")
	h1=$(field hash "$r")
	if [[ $r != nudged* ]] || ! [[ $h1 =~ ^[0-9A-F]{8}$ ]] || [ "$h1" = "$h0" ]; then
		rig_fault state "faces: the $m's face changed (set-up)" "faces nudge: $r (the face was $h0)"
		[[ $r == nudged* ]] && dc "$m" "faces nudge undo" >/dev/null
		return
	fi
	say "faces: the $m's face changed for the run (a face slider and the hair colour; dev faces nudge): $h0 -> $h1"
	if wait_until "$FACES_WAIT" fc_sent "$m" >/dev/null && [ "$(fc_sent "$m")" = "$h1" ]; then
		pass "nudged: the $m's game sent its changed face" "sentHash=$h1"
	else
		fail "nudged: the $m's game sent its changed face" "hash=$(fc_own_field "$m" hash) sentHash=$(fc_own_field "$m" sentHash) (want $h1)"
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
	back=$(field hash "$r")
	if [[ $r != undone* ]]; then
		rig_fault state "faces: the $m's face put back (set-up)" "faces nudge undo: $r"
		return
	fi
	expect_eq "undone: the $m's face is as it was (the same hash)" "$back" "$h0"
	fc_judge "$w" "$m" "$h0" "undone"

	# what only a person can see
	tints=$(field tints "$own")
	if [ "$tints" = 0 ]; then
		warn "faces: the $m's player has tint layers to send (skin tone, paint)" "tints=0: no tint mask read from its player (ourTintsLeft=$(field ourTintsLeft "$own")); its skin tone is the preset's on the $w's stand-in"
	else
		say "faces: the $w's stand-in carries the $m's $tints tint layers on its copy (agree= checked them); whether a head built at run time draws them (skin tone, paint) is for a person to look at"
	fi
}

step_faces() {
	local w s
	for w in host vm; do still $w; done
	faces_way vm host
	faces_way host vm
	section "faces: what the stand-ins' faces did, both games"
	for w in host vm; do
		[ "$(fc_own_field $w nudged)" = 1 ] && dc $w "faces nudge undo" >/dev/null
		s=$(dc $w faces)
		expect_eq "$w: no stand-in's face given up for no head (facesGivenUp=)" "$(field facesGivenUp "$(fc_all "$s")")" 0
		expect_eq "$w: no stand-in's face given up for our own head going (oursGivenUp=)" "$(field oursGivenUp "$(fc_all "$s")")" 0
		expect_eq "$w: our own head was never asked back (repairs=)" "$(field repairs "$(fc_own "$s")")" 0
		say "faces: $w: copies made $(field copiesMade "$(fc_own "$s")") (kept $(field copiesKept "$(fc_own "$s")"), refused $(field refused "$(fc_own "$s")")), colours made $(field colors "$(fc_own "$s")"), tint layers left out $(field tintsLeft "$(fc_own "$s")"), heads built again $(field rebuilds "$(fc_own "$s")"), presets put back for saves $(field presetsPutBack "$(fc_all "$s")")"
	done
}

# ---- self-test: the reading, both games faked ---------------------------------------------------------------------
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init facestest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }

	# a reply in the format the plugin writes (Face::Describe | RemotePlayers::Faces)
	F='read=1 hash=1A2B3C4D race=00013746 female=0 parts=7 types=0x5e hair=000A0435/FF141E28 details=0001A28B skin=00000000 height=1.000 morphsSet=19 tints=12 body=FF8CA0C8 ourTintsLeft=0 sent=2 sentHash=1A2B3C4D nudged=0 copiesMade=3 copiesKept=1 full=0 refused=0 colors=1 tintsLeft=0 rebuilds=0 repairs=0 | facesGivenUp=0 oursGivenUp=0 presetsPutBack=2 oursRepaired=0 | player 2 face=99887766 in=2 fit=own standin=FF0008AE base=FF000901 preset=00013A2E copy=FF000901 shown=99887766 ownBase=1 head=1 watch=built givenUp=0 remakes=1 agree=1 parts=7/7 morphs=19 presets=4 tints=12/12 hair=1 height=1 body=1 details=1 skin=1'
	t "own: our part only" "$(field hash "$(fc_own "$F")")" 1A2B3C4D
	t "own: no stand-in's fields" "$(field fit "$(fc_own "$F")")" ""
	t "all: the common part" "$(field presetsPutBack "$(fc_all "$F")")" 2
	t "all: not the friend's" "$(field fit "$(fc_all "$F")")" ""
	t "friend: the stand-in's part" "$(field face "$(fc_friend "$F")")" 99887766
	t "friend: its tints, not ours" "$(field tints "$(fc_friend "$F")")" 12/12
	t "friend: none without a friend" "$(fc_friend 'read=1 hash=1 | facesGivenUp=0 oursGivenUp=0')" ""
	t "sent: what was read went" "$(dc() { echo "$F"; }; fc_sent host)" 1A2B3C4D
	t "sent: not while another is read" "$(dc() { echo "${F/sentHash=1A2B3C4D/sentHash=00000000}"; }; fc_sent host || echo no)" no
	t "sent: not when nothing is read" "$(dc() { echo 'read=0 sent=0 sentHash=00000000 | facesGivenUp=0'; }; fc_sent host || echo no)" no
	t "wears: all of it" "$(fc_wears_in "$(fc_friend "$F")" 99887766 && echo yes)" yes
	t "wears: another face" "$(fc_wears_in "$(fc_friend "$F")" 11111111 || echo no)" no
	t "wears: no head" "$(fc_wears_in "$(fc_friend "${F/head=1/head=0}")" 99887766 || echo no)" no
	t "wears: the preset's" "$(fc_wears_in "$(fc_friend "${F/ownBase=1/ownBase=0}")" 99887766 || echo no)" no
	t "wears: not all held" "$(fc_wears_in "$(fc_friend "${F/agree=1/agree=0}")" 99887766 || echo no)" no
	t "said: the pieces" "$(fc_said "$(fc_friend "$F")" | grep -o 'parts=7/7 .* skin=1')" "parts=7/7 morphs=19 presets=4 tints=12/12 hair=1 height=1 body=1 details=1 skin=1"

	# the step's way, both games faked: the mover's face is read and sent, the watcher's stand-in placed again on a copy
	# with each face it gets, as the plugin does. FF_MODE: ok, noread (the mover reads no face), nosend (its face never
	# goes), nohead (the copy's head is never built), partial (the copy holds less than came), stale (the watcher never
	# takes a changed face), stuck (the nudge does nothing: the rig's), notints (the mover reads no tint layer).
	fake_faces() {
		FF=$d/ff.$RANDOM; mkdir -p "$FF"; FF_MODE=$1
		echo 1A2B3C4D > "$FF/hash"; echo 0 > "$FF/nudged"; echo 1A2B3C4D > "$FF/shown"; echo 100 > "$FF/standin"; echo 0 > "$FF/remakes"
		ff_hex() { printf 'FF000%03X' "$(cat "$FF/standin")"; }
		dc() { local h n; case "$2" in
			faces) if [ "$1" = vm ]; then
					h=$(cat "$FF/hash")
					[ "$FF_MODE" = noread ] && { echo "read=0 ourTintsLeft=0 sent=0 sentHash=00000000 nudged=0 repairs=0 | facesGivenUp=0 oursGivenUp=0 presetsPutBack=0 | player 1 face=none fit=noface"; return; }
					echo "read=1 hash=$h race=00013746 female=0 parts=7 tints=$([ "$FF_MODE" = notints ] && echo 0 || echo 12) morphsSet=19 ourTintsLeft=0 sent=1 sentHash=$([ "$FF_MODE" = nosend ] && echo 00000000 || echo "$h") nudged=$(cat "$FF/nudged") copiesMade=1 rebuilds=0 repairs=0 | facesGivenUp=0 oursGivenUp=0 presetsPutBack=0 oursRepaired=0 | player 1 face=CAFEF00D fit=own standin=FF000700 ownBase=1 head=1 agree=1 givenUp=0"
				else
					h=$(cat "$FF/shown")
					[ "$FF_MODE" = nosend ] && h=none
					echo "read=1 hash=CAFEF00D sentHash=CAFEF00D tints=12 repairs=0 copiesMade=2 | facesGivenUp=0 oursGivenUp=0 presetsPutBack=1 oursRepaired=0 | player 2 face=$h in=2 fit=$([ "$h" = none ] && echo noface || echo own) standin=$(ff_hex) base=FF000901 preset=00013A2E copy=FF000901 shown=$h ownBase=1 head=$([ "$FF_MODE" = nohead ] && echo 0 || echo 1) watch=built givenUp=0 remakes=$(cat "$FF/remakes") agree=$([ "$FF_MODE" = partial ] && echo 0 || echo 1) parts=7/7 morphs=19 presets=4 tints=$([ "$FF_MODE" = partial ] && echo 9/12 || echo 12/12) hair=1 height=1 body=1 details=1 skin=1"
				fi ;;
			'faces nudge')
				[ "$FF_MODE" = stuck ] && { echo "nudged hash=$(cat "$FF/hash")"; echo 1 > "$FF/nudged"; return; }
				echo 5E5E5E5E > "$FF/hash"; echo 1 > "$FF/nudged"
				[ "$FF_MODE" = stale ] || { echo 5E5E5E5E > "$FF/shown"; n=$(cat "$FF/standin"); echo $((n + 1)) > "$FF/standin"; echo $(( $(cat "$FF/remakes") + 1 )) > "$FF/remakes"; }
				echo "nudged hash=5E5E5E5E" ;;
			'faces nudge undo')
				[ "$(cat "$FF/nudged")" = 1 ] || { echo "error: nothing nudged"; return; }
				echo 1A2B3C4D > "$FF/hash"; echo 0 > "$FF/nudged"
				[ "$FF_MODE" = stale ] || { echo 1A2B3C4D > "$FF/shown"; n=$(cat "$FF/standin"); echo $((n + 1)) > "$FF/standin"; }
				echo "undone hash=1A2B3C4D" ;;
			standins) echo "players=1 | player 2 standin=$(ff_hex) placed=1 dressed=1 at=(100,200,0) heading=0.000" ;;
			*) echo ok ;; esac; }
		( A_OUT=$d/ff$RANDOM; mkdir -p "$A_OUT"; a_init ff >/dev/null; faces_way vm host >/dev/null; cut -f1 "$A_RESULTS" | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}' )
	}
	# ok: the face sent (1), its six checks (6), the changed face sent (1), its six (6), the stand-in placed again (1), the
	# same hash back (1), its six (6): 22
	t "way: all of it works" "$(fake_faces ok)" "PASS:22 "
	t "way: a mover that reads no face fails, and nothing else is tried" "$(fake_faces noread)" "FAIL:1 "
	t "way: a face never sent fails, and nothing else is tried" "$(fake_faces nosend)" "FAIL:1 "
	t "way: a copy whose head is never built fails each time it is judged" "$(fake_faces nohead | grep -o 'FAIL:[0-9]*')" "FAIL:3"
	t "way: a copy that holds less than came fails each time it is judged" "$(fake_faces partial | grep -o 'FAIL:[0-9]*')" "FAIL:3"
	# stale: the changed face never reaches the stand-in: its face= (fit, own base, head and agree still hold) and not
	# placed again; then undone is the first face again, which it still shows
	t "way: a changed face the stand-in never takes fails" "$(fake_faces stale | grep -o 'FAIL:[0-9]*')" "FAIL:2"
	t "way: a nudge that changes nothing is the rig's, and ends the way" "$(fake_faces stuck | grep -o 'FAIL:[0-9]*\|RIG[A-Z]*:[0-9]*' | tr '\n' ' ')" "RIG:1 "
	t "way: no tint layer read is a WARN" "$(fake_faces notints | grep -o 'WARN:[0-9]*')" "WARN:1"

	# the step is wired into the harness: in STEPS before final, a session step, run by default
	t "faces is a step" "$(case " $STEPS " in *" faces "*) echo yes ;; esac)" yes
	t "faces comes before final" "$(sed 's/ final.*//' <<<" $STEPS " | grep -c ' faces')" 1
	t "faces needs a session" "$(case " $SESSION_STEPS " in *" faces "*) echo yes ;; esac)" yes
	t "faces runs by default" "$(case " $OPTIN_STEPS " in *" faces "*) echo no ;; *) echo yes ;; esac)" yes
	[ $bad -eq 0 ] && echo "faces.sh selftest: all ok" || echo "faces.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
