#!/bin/bash
# steps/talk.sh: the rig step `talk` (protocol 15, docs/DESIGN.md "Conversations"), sourced by steps.sh. One player talks
# with an NPC; the other, near, sees each line in its subtitle area (or as a notice) and hears the NPC's recording from the
# NPC; one player at a time talks with an NPC, and a friend's own talk with one someone else talks with is ended. Not run
# in a game yet.
#
#   step_talk                                  the step (in the default run, after drops, before final)
#   bash tools/rig/steps/talk.sh selftest      the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `talk` (one line: our talk and the counts; lastSaid=,
# lastPicked=, lastHeard= and lastHeardTalker= are hashes of a line's text, so the two games can be compared without the
# words: an NPC's lines are said=/lastSaid= on the talker and heard=/lastHeard= on the watcher, the talker's own topics
# playerLines=/lastPicked= and heardTalker=/lastHeardTalker=, never mixed), `talk pick` (the NPC near our player who can
# talk, lowest form ID first: both games pick the same) and `talk subs`):
#   1. Both players are put in TALK_CELL with coc (console placement, said), near each other, and both pick the same NPC.
#   2. Each way (the VM talks with the host watching, then the host with the VM watching): the talker walks up to the NPC
#      (walkto, real movement) and presses a (Activate; a menu that does not open is RIG, and the dev `use` stands in).
#      Its Dialogue Menu is open with that NPC, and it says the talk to the session (talks= up). The NPC greets: the
#      talker sends the line (said= up); within TALK_WAIT the watcher hears it (heard= up, lastHeard= the talker's
#      lastSaid=, lastFrom= the talker), shows it (subtitled= or noticed= up) and, where the game has sound (the host:
#      the VM runs with it off, a WARN), plays its recording (voiced= up; missing= or unplayed= up is a FAIL). The talker
#      picks the first topic (a): its own line goes (playerLines= up) and the watcher hears it (heardTalker= up,
#      lastHeardTalker= the talker's lastPicked=) and shows it; then the NPC's answer, heard, shown and played as the
#      greeting (an answer with no words is a WARN). The talker leaves (b, until no menu is open): its talk is over
#      (current= and lingering= 0 once its goodbye is said).
#   3. One at a time: the host talks with the NPC again; the NPC's last line of the host's talk before, still its
#      subtitle, is held back (primed= up) and the first line sent is a new one (each a WARN when not: the engine may
#      clear it, or greet with the same words). The VM uses the same NPC (`use`, the dev activation): the VM's talk is
#      ended (busyClosed= up, its menu shut; busyRefused= up in its place is a FAIL that names isGreetingPlayer) and none
#      of its lines reach the host. The host leaves.
#   4. Both are put back in the harness's cell (coc).

TALK_CELL=${TALK_CELL:-RiverwoodSleepingGiantInn}  # an interior with people to talk with, near the coc marker
TALK_WAIT=${TALK_WAIT:-15}                         # s: a greeting, a line's way to the other game, a topic's answer
TALK_NEAR=${TALK_NEAR:-110}                        # units: the walk up to the NPC ends this near

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# talk_ref <talk pick reply>: the NPC it names (empty: none)
talk_ref() { local r; r=$(field ref "$1"); [[ $r =~ ^[0-9A-F]{8}$ ]] && [ "$r" != 00000000 ] && echo "$r"; }
# talk_shown <talk reply>: what it showed of friends' lines (subtitled + noticed)
talk_shown() { local s n; s=$(field subtitled "$1"); n=$(field noticed "$1"); is_num "$s" && is_num "$n" && echo $((s + n)); }
# talk_menu_with <talk reply> <npc>: its Dialogue Menu is open with that NPC
talk_menu_with() { [ "$(field menu "$1")" = 1 ] && [ "$(field partner "$1")" = "$2" ]; }
# talk_over <talk reply>: no talk of ours, none lingering, no menu
talk_over() { [ "$(field menu "$1")" = 0 ] && [ "$(field current "$1")" = 00000000 ] && [ "$(field lingering "$1")" = 00000000 ]; }

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

talk_n() { field "$2" "$(dc "$1" talk)"; }
talk_ge() { local v; v=$(talk_n "$1" "$2"); is_num "$v" && [ "$v" -ge "$3" ]; }
talk_shown_ge() { local v; v=$(talk_shown "$(dc "$1" talk)"); is_num "$v" && [ "$v" -ge "$2" ]; }
talk_open_with() { talk_menu_with "$(dc "$1" talk)" "$2"; }
talk_done() { talk_over "$(dc "$1" talk)"; }
# talk_heard_same <talker> <watcher>: the NPC's line the watcher showed last is the one the talker sent last
talk_heard_same() { [ "$(talk_n "$2" lastHeard)" = "$(talk_n "$1" lastSaid)" ] && [ "$(talk_n "$2" lastFrom)" = "$(talk_n "$1" us)" ]; }
# talk_topic_same <talker> <watcher>: the talker's own line the watcher showed last is the topic the talker picked last
# (hashes of their own: an NPC's answer never stands in for it)
talk_topic_same() { local p; p=$(talk_n "$1" lastPicked); [ -n "$p" ] && [ "$p" != 00000000 ] && [ "$(talk_n "$2" lastHeardTalker)" = "$p" ]; }

# talk_open <who> <npc>: that game's player walks up to the NPC (real movement) and presses a; its Dialogue Menu is open
# with it. A menu that does not open is RIG (the input), and the dev `use` stands in. 1 when no menu opens at all.
talk_open() {
	local who=$1 npc=$2 r st k
	r=$(dc "$who" "walkto $npc 0.3 $TALK_NEAR 20")
	if [[ $r == walking* ]]; then
		for ((k = 0; k < 50; k++)); do st=$(dc "$who" walkto); [[ $st == "not walking"* ]] && break; sleep 0.5; done
		still "$who"; sleep 1
		r=$(dc "$who" 'pad button a')
		if [[ $r == button* ]] && wait_until 5 talk_open_with "$who" "$npc"; then
			pass "$who: walked up to $npc and talks with it (a)" "$(dc "$who" talk | cut -c1-120)"; return 0
		fi
		rig_fault input "$who: walked up to $npc and talks with it (a)" "the pad replied '$r'; $(dc "$who" talk | cut -c1-160)"
		talk_leave "$who" quiet
	else
		rig_fault input "$who: walks up to $npc" "walkto replied '$r'"
	fi
	r=$(dc "$who" "use $npc")
	if [ "$r" = used ] && wait_until 5 talk_open_with "$who" "$npc"; then
		pass "$who: talks with $npc (the dev use stands in)" "$(dc "$who" talk | cut -c1-120)"; return 0
	fi
	rig_unhealed input "$who: talks with $npc" "'use' replied '$r'; $(dc "$who" talk | cut -c1-160)"; return 1
}

# talk_leave <who> [quiet]: that game's player leaves the conversation (b, until no menu is open and its talk is over)
talk_leave() {
	local who=$1 i
	for ((i = 0; i < 5; i++)); do
		talk_done "$who" && in_world "$who" && break
		dc "$who" 'pad button b' >/dev/null; sleep 1
	done
	[ "${2:-}" = quiet ] && return 0
	if wait_until 5 talk_done "$who"; then pass "$who: left the conversation (b); its talk is over once its goodbye is said" "$(dc "$who" talk | cut -c1-120)"
	else fail "$who: left the conversation (b); its talk is over once its goodbye is said" "$(dc "$who" talk | cut -c1-200)"; fi
}

# talk_heard <talker> <watcher> <what> <npc|topic> <heard before> <shown before> [voiced before]: the watcher hears and
# shows the talker's line, an NPC's (heard=, lastHeard= against the talker's lastSaid=) or the talker's own topic
# (heardTalker=, lastHeardTalker= against the talker's lastPicked=: an NPC's answer never counts for it); with voiced
# before, its recording plays where the watcher's game has sound. 1 when it is not heard or not shown.
talk_heard() {
	local t=$1 w=$2 what=$3 kind=$4 h0=$5 s0=$6 v0=${7:-} m0 u0 n=heard same=talk_heard_same last=lastHeard ok=0
	if [ "$kind" = topic ]; then n=heardTalker; same=talk_topic_same; last=lastHeardTalker; fi
	if wait_until "$TALK_WAIT" talk_ge "$w" $n $((h0 + 1)); then pass "$w: hears $t's $what" "$n=$(talk_n "$w" $n)"
	else fail "$w: hears $t's $what" "$w: $(dc "$w" talk | cut -c1-200) | $t: $(dc "$t" talk | cut -c1-200)"; return 1; fi
	if wait_until 5 $same "$t" "$w"; then pass "$w: the line it heard is the one $t said ($what)" "$last=$(talk_n "$w" $last)"
	else fail "$w: the line it heard is the one $t said ($what)" "$w $last=$(talk_n "$w" $last) lastFrom=$(talk_n "$w" lastFrom); $t lastSaid=$(talk_n "$t" lastSaid) lastPicked=$(talk_n "$t" lastPicked) us=$(talk_n "$t" us)"; fi
	if wait_until 5 talk_shown_ge "$w" $((s0 + 1)); then pass "$w: shows $t's $what (subtitle area or notice)" "subtitled=$(talk_n "$w" subtitled) noticed=$(talk_n "$w" noticed) dropped=$(talk_n "$w" dropped)"
	else fail "$w: shows $t's $what (subtitle area or notice)" "$(dc "$w" talk | cut -c1-200); $(dc "$w" 'talk subs')"; ok=1; fi
	[ -n "$v0" ] || return $ok
	if [ "$(talk_n "$w" audio)" != 1 ]; then
		warn "$w: plays $t's $what from the NPC" "its game's sound is off (the rig's VM): not checked here"; return $ok
	fi
	m0=$(talk_n "$w" missing); u0=$(talk_n "$w" unplayed)
	if wait_until 5 talk_ge "$w" voiced $((v0 + 1)); then pass "$w: plays $t's $what from the NPC" "voiced=$(talk_n "$w" voiced)"
	else fail "$w: plays $t's $what from the NPC" "voiced=$(talk_n "$w" voiced) missing=$m0 unplayed=$u0 (a line with no recording here, or one the audio manager would not play)"; fi
	return $ok
}

# talk_way <talker> <npc>: the talker talks with the NPC, the other watches: the greeting, the first topic and its answer
# heard, shown and played (each kind on counters of its own); the talker leaves
talk_way() {
	local t=$1 npc=$2 w s0 p0 k0 h0 ht0 sh0 v0 tu0 tb0 topicShown=0
	w=$(other "$t")
	s0=$(talk_n "$t" said); p0=$(talk_n "$t" playerLines); k0=$(talk_n "$t" talks)
	h0=$(talk_n "$w" heard); sh0=$(talk_shown "$(dc "$w" talk)"); v0=$(talk_n "$w" voiced)
	is_num "$s0" && is_num "$h0" && is_num "$sh0" || { rig_broken game "both: read their talk" "$t: $(dc "$t" talk | cut -c1-120) | $w: $(dc "$w" talk | cut -c1-120)"; return; }
	talk_open "$t" "$npc" || return
	if wait_until 5 talk_ge "$t" talks $((k0 + 1)); then pass "$t: tells the session it talks with $npc" "talks=$(talk_n "$t" talks)"
	else fail "$t: tells the session it talks with $npc" "$(dc "$t" talk | cut -c1-200)"; fi
	if wait_until "$TALK_WAIT" talk_ge "$t" said $((s0 + 1)); then pass "$t: sends $npc's greeting" "said=$(talk_n "$t" said)"
		talk_heard "$t" "$w" "NPC's greeting" npc "$h0" "$sh0" "$v0"
	else fail "$t: sends $npc's greeting" "$(dc "$t" talk | cut -c1-200)"; fi

	# The topic and its answer: the baselines before the pick (the answer may arrive before the topic's checks end).
	s0=$(talk_n "$t" said); h0=$(talk_n "$w" heard); ht0=$(talk_n "$w" heardTalker); sh0=$(talk_shown "$(dc "$w" talk)"); v0=$(talk_n "$w" voiced)
	tu0=$(talk_n "$t" topicUnlisted); tb0=$(talk_n "$t" topicBusy)
	dc "$t" 'pad button a' >/dev/null
	if wait_until 5 talk_ge "$t" playerLines $((p0 + 1)); then pass "$t: sends the topic its player picked (a)" "playerLines=$(talk_n "$t" playerLines) lastPicked=$(talk_n "$t" lastPicked)"
		talk_heard "$t" "$w" "topic" topic "$ht0" "$sh0" && topicShown=1
	elif ! talk_ge "$t" topicUnlisted $((tu0 + 1)) && ! talk_ge "$t" topicBusy $((tb0 + 30)); then rig_fault input "$t: sends the topic its player picked (a)" "$(dc "$t" talk | cut -c1-200)"
	else fail "$t: sends the topic its player picked (a)" "the topic was not read: topicUnlisted=$(talk_n "$t" topicUnlisted) topicBusy=$(talk_n "$t" topicBusy) (the menu's lists or its lock are not as the plugin reads them)"; fi
	if wait_until "$TALK_WAIT" talk_ge "$t" said $((s0 + 1)); then pass "$t: sends $npc's answer" "said=$(talk_n "$t" said)"
		talk_heard "$t" "$w" "NPC's answer" npc "$h0" $((sh0 + topicShown)) "$v0"
	else warn "$t: sends $npc's answer" "no new line after the topic (one that opens a menu, or an answer with no words): $(dc "$t" talk | cut -c1-160)"; fi
	talk_leave "$t"
}

# talk_fresh <who> <last line before> <primed before> <said before>: a talk with an NPC whose last line (of a talk before)
# is still its subtitle: that line is held back (primed= up) and the talk's first line sent is a new one. A WARN, not a
# FAIL, when the engine had cleared the subtitle (nothing to hold back), or the greeting has that line's very words.
talk_fresh() {
	local who=$1 l0=$2 q0=$3 s0=$4
	if wait_until 5 talk_ge "$who" primed $((q0 + 1)); then pass "$who: the line left up from its talk before is held back at the start (primed)" "primed=$(talk_n "$who" primed)"
	else warn "$who: the line left up from its talk before is held back at the start (primed)" "no subtitle was up when the talk began (the engine cleared it): nothing to hold back"; fi
	wait_until "$TALK_WAIT" talk_ge "$who" said $((s0 + 1)) || return 0  # no greeting: each way checks that
	if [ "$(talk_n "$who" lastSaid)" != "$l0" ]; then pass "$who: the talk's first line is a new one, not the one left up" "lastSaid=$(talk_n "$who" lastSaid) before=$l0"
	else warn "$who: the talk's first line is a new one, not the one left up" "it has the words of the last line of the talk before (a stale subtitle sent, or a greeting with the same words): lastSaid=$l0"; fi
}

# talk_busy <npc>: the host talks with the NPC again (its last line from before held back), the VM uses it: the VM's
# talk is ended (busyClosed up, its menu shut) and none of its lines reach the host
talk_busy() {
	local npc=$1 b0 r0 h0 ht0 l0 q0 s0 r
	l0=$(talk_n host lastSaid); q0=$(talk_n host primed); s0=$(talk_n host said)
	talk_open host "$npc" || return
	wait_until 5 talk_ge host granted 1 || fail "host: the session says the talk with $npc is its own" "$(dc host talk | cut -c1-200)"
	if [ -n "$l0" ] && [ "$l0" != 00000000 ] && is_num "$q0"; then talk_fresh host "$l0" "$q0" "$s0"; fi
	b0=$(talk_n vm busyClosed); r0=$(talk_n vm busyRefused); h0=$(talk_n host heard); ht0=$(talk_n host heardTalker)
	r=$(dc vm "use $npc")
	[ "$r" = used ] || { rig_unhealed input "vm: uses $npc while the host talks with it" "'use' replied '$r'"; talk_leave host; return; }
	if wait_until 10 talk_ge vm busyClosed $((b0 + 1)); then pass "vm: its talk with $npc is ended: the host talks with it" "busyClosed=$(talk_n vm busyClosed)"
	elif talk_ge vm busyRefused $((r0 + 1)); then
		fail "vm: its talk with $npc is ended: the host talks with it" "taken for the NPC's own greeting (busyRefused=$(talk_n vm busyRefused)): the game said the NPC greeted in a talk its player began (isGreetingPlayer is not for forcegreets only)"
	else fail "vm: its talk with $npc is ended: the host talks with it" "$(dc vm talk | cut -c1-200)"; fi
	if wait_until 5 talk_done vm; then pass "vm: its Dialogue Menu is shut" "$(dc vm talk | cut -c1-120)"
	else fail "vm: its Dialogue Menu is shut" "$(dc vm talk | cut -c1-200)"; talk_leave vm quiet; fi
	expect_eq "host: heard no NPC's line of the VM's talk with $npc" "$(talk_n host heard)" "$h0"
	expect_eq "host: heard no topic of the VM's talk with $npc" "$(talk_n host heardTalker)" "$ht0"
	talk_leave host
}

step_talk() {
	local npc w
	section "talk: one player talks with an NPC, the other sees and hears it"
	doors_coc "$TALK_CELL" || return
	sleep 3
	npc=$(talk_ref "$(dc host 'talk pick')")
	if [ -z "$npc" ] || [ "$npc" != "$(talk_ref "$(dc vm 'talk pick')")" ]; then
		rig_unhealed state "both: pick the same NPC to talk with" "host '$(dc host 'talk pick')', vm '$(dc vm 'talk pick')'"; return
	fi
	pass "both: pick the same NPC to talk with" "$npc"
	for w in host vm; do echo "     $w: $(dc $w 'talk subs')"; done

	section "talk: the VM talks with $npc, the host watches"
	talk_way vm "$npc"

	section "talk: the host talks with $npc, the VM watches"
	talk_way host "$npc"

	section "talk: one player at a time talks with $npc"
	talk_busy "$npc"

	for w in host vm; do echo "     $w: $(dc $w talk | tr ' ' '\n' | grep -E '^(dropped|missing|unplayed|silent|far|noSpeaker|repeats|tooMany|cutLong|cutCrowded|cutEnded|offmain|events|eventsDropped)=' | tr '\n' ' ')"; done

	section "talk: both back in $SMOKE_CELL"
	doors_coc "$SMOKE_CELL"
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init talktest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	TALK_WAIT=2

	S='installed=1 us=2 menu=1 partner=0001A6A4 current=0001A6A4 lingering=00000000 granted=1 busy=0 holder=0 started=1 said=3 playerLines=1 ends=1 talks=2 lastSaid=9E1C2A44 heard=4 subtitled=2 noticed=1 dropped=0 voiced=2 missing=0 unplayed=0 silent=0 far=0 noSpeaker=0 repeats=0 tooMany=0 busyRefused=0 busyClosed=0 sounds=1 cutLong=0 cutCrowded=0 cutEnded=0 subUp=1 subsOff=0 lastHeard=00000000 lastFrom=0 audio=1 offmain=0 events=6 eventsDropped=0 lastPicked=00000000 heardTalker=0 lastHeardTalker=00000000 primed=0 endsOther=0 topicUnlisted=0 topicBusy=0'
	t "ref: from a pick" "$(talk_ref 'ref=0001A6A4 dist=212 of=3')" 0001A6A4
	t "ref: none from an error" "$(talk_ref 'error: 0 NPCs to talk with near, none at 0')" ""
	t "ref: none for 0" "$(talk_ref 'ref=00000000')" ""
	t "shown: subtitled and noticed" "$(talk_shown "$S")" 3
	t "shown: nothing for an unreadable reply" "$(talk_shown 'error: x')" ""
	t "menu with: open with that NPC" "$(talk_menu_with "$S" 0001A6A4 && echo yes)" yes
	t "menu with: not another" "$(talk_menu_with "$S" 0001A6A5 || echo no)" no
	t "over: not while talking" "$(talk_over "$S" || echo no)" no
	t "over: no menu, nothing current or lingering" "$(talk_over "${S/menu=1/menu=0}" && echo no || echo still)" still
	O=$(sed -e 's/menu=1/menu=0/; s/current=0001A6A4/current=00000000/' <<<"$S")
	t "over: ... once current is 0 too" "$(talk_over "$O" && echo yes)" yes
	t "over: a goodbye still lingering is not over" "$(talk_over "${O/lingering=00000000/lingering=0001A6A4}" || echo no)" no

	# Both games faked: each one's talk (a file of key=value), the NPC both pick, and a session that passes the talker's
	# lines to the other (SHARE=1, the mod at work; 0 a mod that passes none) and plays them where there is sound (VOICE=1;
	# 0 a recording that is not found). OPENS=1: the pad's a opens the menu after the walk; 0: only the dev use does.
	# BUSY=1: the VM's use of an NPC the host talks with ends the VM's talk; 2: the VM takes it for the NPC's own greeting
	# (busyRefused, its menu left open). As the plugin counts them: an NPC's line is said=/lastSaid= on the talker and
	# heard=/lastHeard= on the other; the talker's own topic is playerLines=/lastPicked= and heardTalker=/lastHeardTalker=.
	# TOPIC=0: the topic is sent and never reaches the other. ANSWER=0: the topic's answer has no words (no line).
	# STALE=1: a talk's start sends the NPC's last line of the talk before again (the bug primed= is for). UNREAD=1: the
	# topic picked is not found in the menu's lists (topicUnlisted=), so never sent.
	fake() {
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; VOICE=${2:-1}; OPENS=${3:-1}; BUSY=${4:-1}; TOPIC=${5:-1}; ANSWER=${6:-1}; STALE=${7:-0}; UNREAD=${8:-0}
		for w in host vm; do
			sed -e "s/us=2/us=$([ $w = host ] && echo 1 || echo 2)/; s/menu=1/menu=0/; s/partner=0001A6A4/partner=00000000/; s/current=0001A6A4/current=00000000/" \
				-e 's/said=3/said=0/; s/playerLines=1/playerLines=0/; s/talks=2/talks=0/; s/heard=4/heard=0/; s/subtitled=2/subtitled=0/; s/noticed=1/noticed=0/; s/voiced=2/voiced=0/' \
				-e 's/lastSaid=9E1C2A44/lastSaid=00000000/' \
				-e "s/audio=1/audio=$([ $w = host ] && echo 1 || echo 0)/; s/granted=1/granted=0/" <<<"$S" > "$FK/$w"
			echo 0 > "$FK/$w.walked"
		done
	}
	get() { field "$2" "$(cat "$FK/$1")"; }
	put() { sed -i "s/\b$2=[^ ]*/$2=$3/" "$FK/$1"; }
	inc() { put "$1" "$2" $(( $(get "$1" "$2") + ${3:-1} )); }
	say() {  # say <talker> <hash>: the talker sends its NPC's line; the other hears it when the session passes it on
		local o; o=$(other "$1"); inc "$1" said; put "$1" lastSaid "$2"
		[ "$SHARE" = 1 ] || return 0
		inc "$o" heard; inc "$o" noticed; put "$o" lastHeard "$2"; put "$o" lastFrom "$(get "$1" us)"
		if [ "$(get "$o" audio)" = 1 ]; then if [ "$VOICE" = 1 ]; then inc "$o" voiced; else inc "$o" missing; fi; fi
	}
	pick() {  # pick <talker> <hash>: the talker sends the topic its player picked (no recording); the other hears it
		local o; o=$(other "$1"); inc "$1" playerLines; put "$1" lastPicked "$2"
		[ "$SHARE" = 1 ] && [ "$TOPIC" = 1 ] || return 0
		inc "$o" heardTalker; inc "$o" noticed; put "$o" lastHeardTalker "$2"; put "$o" lastFrom "$(get "$1" us)"
	}
	open() {  # open <who>: the menu opens with the NPC, it tells the session, the NPC greets
		put "$1" menu 1; put "$1" partner 0001A6A4; put "$1" current 0001A6A4; inc "$1" talks; put "$1" granted 1
		if [ "$1" = vm ] && [ "$(get host current)" = 0001A6A4 ]; then
			if [ "$BUSY" = 1 ]; then inc vm busyClosed; put vm menu 0; put vm partner 00000000; put vm current 00000000; return; fi
			if [ "$BUSY" = 2 ]; then inc vm busyRefused; return; fi
		fi
		local before; before=$(get "$1" lastSaid)
		if [ "$before" != 00000000 ]; then
			if [ "$STALE" = 1 ]; then say "$1" "$before"; return; fi
			inc "$1" primed
		fi
		say "$1" 1111AAAA
	}
	dc() {
		local w=$1 c=$2 a; read -ra a <<<"$c"
		case "$c" in
			talk) cat "$FK/$w" ;;
			"talk pick") echo "ref=0001A6A4 dist=212 of=3" ;;
			"talk subs") echo "general=1 dialogue=1 manager=1 listed=0 ours=0 off=0 drops=0" ;;
			"walkto stop") echo stopped ;;
			"walkto "*) echo 1 > "$FK/$w.walked"; echo "walking to ${a[1]}" ;;
			walkto) echo "not walking" ;;
			"pad off") echo off ;;
			"pad button a")
				if [ "$(get "$w" menu)" = 1 ]; then if [ "$UNREAD" = 1 ]; then inc "$w" topicUnlisted; else pick "$w" 2222BBBB; fi; if [ "$ANSWER" = 1 ]; then say "$w" 3333CCCC; fi
				elif [ "$(cat "$FK/$w.walked")" = 1 ] && [ "$OPENS" = 1 ]; then open "$w"; fi
				echo "button a" ;;
			"pad button b") put "$w" menu 0; put "$w" partner 00000000; put "$w" current 00000000; echo "button b" ;;
			"use "*) open "$w"; echo used ;;
			where) echo "cell=0001605E world=00000000 modal=0" ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init tt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; o=$(run 1 talk_way vm 0001A6A4)
	t "way: the VM talks, the host hears, shows and plays every line: passes" "$(verdicts "$o")" "PASS=17 "
	t "way: ... the counts (NPC lines and the topic apart)" "$(get vm said),$(get vm playerLines),$(get vm lastPicked),$(get host heard),$(get host heardTalker),$(get host lastHeardTalker),$(get host voiced)" \
		2,1,2222BBBB,2,1,2222BBBB,2
	fake 1; o=$(run 2 talk_way host 0001A6A4)
	t "way: the host talks, the VM (no sound) hears and shows: passes, the recordings WARNs" "$(verdicts "$o")" "PASS=15 WARN=2 "
	fake 0; o=$(run 3 talk_way vm 0001A6A4)
	t "way: lines the other game never hears fail (greeting, topic, answer)" "$(grep -c '^FAIL host: hears vm' <<<"$o")" 3
	fake 1 0; o=$(run 4 talk_way vm 0001A6A4)
	t "way: a recording not found where there is sound fails" "$(grep -c "^FAIL host: plays vm's NPC's greeting" <<<"$o")" 1
	fake 1 1 0; o=$(run 5 talk_way vm 0001A6A4)
	t "way: a menu the pad does not open is RIG (the dev use stands in), never FAIL" "$(verdicts "$o")" "PASS=17 RIG=1 "
	fake 1 1 1 1 0; o=$(run 8 talk_way vm 0001A6A4)
	t "way: a topic that never reaches the other fails, though the answer does" "$(verdicts "$o")" "FAIL=1 PASS=14 "
	t "way: ... on the topic's own check" "$(grep -c "^FAIL host: hears vm's topic" <<<"$o")" 1
	fake 1 1 1 1 1 0; o=$(run 9 talk_way vm 0001A6A4)
	t "way: an answer with no words is a WARN, never a FAIL" "$(verdicts "$o")" "PASS=12 WARN=1 "
	fake 1 1 1 1 1 1 0 1; o=$(run 13 talk_way vm 0001A6A4)
	t "way: a topic the plugin could not read fails (never an input fault)" "$(grep -c "^FAIL vm: sends the topic its player picked (a): the topic was not read" <<<"$o"),$(grep -c "^RIG " <<<"$o")" 1,0
	fake 1; o=$(run 6 talk_busy 0001A6A4)
	t "busy: the VM's talk with the host's NPC is ended: passes" "$(verdicts "$o")" "PASS=6 "
	fake 1 1 1 0; o=$(run 7 talk_busy 0001A6A4)
	t "busy: a VM still talking with it fails" "$(grep -c '^FAIL vm: its talk with 0001A6A4 is ended' <<<"$o"),$(grep -c '^FAIL vm: its Dialogue Menu is shut' <<<"$o")" 1,1
	fake 1 1 1 2; o=$(run 12 talk_busy 0001A6A4)
	t "busy: a talk taken for the NPC's own greeting fails, saying so" "$(grep -c '^FAIL vm: its talk with 0001A6A4 is ended.*isGreetingPlayer' <<<"$o")" 1
	again() { talk_way host 0001A6A4; talk_busy 0001A6A4; }
	fake 1; o=$(run 10 again)
	t "busy: the host's second talk holds back the line left up and sends a new one: passes" "$(verdicts "$o")" "PASS=23 WARN=2 "
	fake 1 1 1 1 1 1 1; o=$(run 11 again)
	t "busy: a second talk that sends the line left up again is WARNed (held back, first line new)" "$(verdicts "$o")" "PASS=21 WARN=4 "

	# the step is wired into the harness: in STEPS before final, a session step, in the default run
	t "talk is a step, before final" "$(case " $STEPS " in *" talk "*"final "*) echo yes;; esac)" yes
	t "talk needs a session" "$(case " $SESSION_STEPS " in *" talk "*) echo yes;; esac)" yes
	t "talk runs by default" "$(case " $OPTIN_STEPS " in *" talk "*) echo no;; *) echo yes;; esac)" yes
	t "talk has its line" "$([ -n "${STEP_INFO[talk]:-}" ] && echo yes)" yes
	[ $bad -eq 0 ] && echo "talk.sh selftest: all ok" || echo "talk.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
