#!/bin/bash
# steps/pickups.sh: the rig step `pickups` (protocol 22, docs/DESIGN.md "Taken from the world"), sourced by steps.sh. A loose
# item one player picks up goes out of the other's world; a plant one harvests is harvested in the other's, and shows it;
# keys and books stay each player's own; a game that loads a save, or joins again, is told the session's again, never its
# own. Not run in a game yet.
#
#   step_pickups                                 the step (in the default run, after claws, before jobsreset)
#   bash tools/rig/steps/pickups.sh selftest     the step's reading and its checks with both games faked (CI runs it)
#
# What it does (the replies are the plugin's dev commands `pickups` (counts), `pickups pick <item|book|flora|tree> [radius]`
# (the nearest one of that sort that is not a crime to take), `pickups ref <ref>` (what became of a reference, with what its
# 3D shows: loaded3d=, shown=<drawn geometry count>:<hash of their names>) and `pickups take <ref>` (our player's
# activation of it, as the activate key does, refused out of reach)):
#   1. Both players are put in each of PICKUPS_CELLS in turn (coc: console placement, said) until each finds a loose item
#      it may take that the other's world holds too.
#   2. An item each way (host to VM, then VM to host): the picker walks up to it (walkto, real movement) and takes it. Its
#      own world: out (in its inventory, or no longer in memory) and told to the session (sent= up). Within PICKUPS_WAIT
#      the other's world has it out (disabled), never deleted.
#   3. A book (a note, a journal) each way: the picker takes one; it stays in the other's world for PICKUPS_WAIT (each
#      player reads their own), and the picker counts it as its own (judged= book).
#   4. A taker's friend loads: the VM saves, the host takes an item, the VM loads its save (from before the take): the VM
#      asks the session again (asks= up) and the item goes out of its world again.
#   5. A taker's own load: the VM saves, takes an item, loads its save: the item lies in its world again (the save's: the
#      VM's to take again) and stays there for PICKUPS_WAIT (the session never sends a player its own); the host's world
#      still has it out.
#   6. Joining again: the VM leaves, the host takes an item, the VM joins again: the item goes out of the VM's world (the
#      welcome's catch-up, catchUps= up).
#   7. A plant each way, in PICKUPS_FLORA_CELL (coc): a flora each way, then a tree with something to harvest each way. The
#      harvester walks up to the nearest one not harvested in either world and harvests it (an activation, as the activate
#      key does); its world reads it harvested and told the session (harvestsSent= up); within PICKUPS_WAIT the other's
#      reads it harvested too, still in the world, and its 3D shows what the harvester's own shows after the harvest (the
#      same drawn geometry: its flowers or fruit gone). A harvest whose 3D the reading sees no change in is a WARN (that
#      check proves nothing for that plant), never a PASS.
#   8. A harvest kept in a save: once the host harvested a plant, the VM saves, leaves, loads that save alone: the plant
#      reads harvested there (the change is the save's). The VM joins again.
#   9. Both are put back in the harness's cell (coc).
# One way only, by the rig's rules: legs 4, 5, 6 and 8 save, load, leave or rejoin, which only the VM may do (no save or
# load on the host, and the host leaving ends the session); the host is the other side of each.
# Not checked here (follow-ups): ore veins (their count is a script's, each player's own), and an item under an enable
# parent that something enables after the take (the plugin watches it; the rig cannot set one up yet).

PICKUPS_CELLS=${PICKUPS_CELLS:-"RiverwoodSleepingGiantInn Riverwood WhiterunBanneredMare"}  # tried in turn for a loose item
PICKUPS_FLORA_CELL=${PICKUPS_FLORA_CELL:-Riverwood}  # an exterior with plants (flora and trees) near the coc marker
PICKUPS_WAIT=${PICKUPS_WAIT:-20}                     # s: a pickup is told, passed on and made (four batches a second)
PICKUPS_RADIUS=${PICKUPS_RADIUS:-1500}               # how far the picker looks for an item
PICKUPS_FLORA_RADIUS=${PICKUPS_FLORA_RADIUS:-4000}   # ... and for a plant

# ---- the reading, free of the rig (selftest) ----------------------------------------------------------------------

# pk_found <pick reply>: the reference it names (empty: none, or an error)
pk_found() { local r; r=$(field ref "$1"); [[ $r =~ ^[0-9A-F]{8}$ ]] && echo "$r"; }
# pk_is <ref reply> <field> <value>: that reply is of a reference found there, and its field reads so
pk_is() { [[ $1 == ref=* ]] && [ "$(field found "$1")" = 1 ] && [ "$(field "$2" "$1")" = "$3" ]; }
# pk_gone_reply <ref reply>: the picker's own reference is out of its world: in its inventory (found, not in the world), or
# no longer in memory at all (found=0: the engine may free a reference picked up). An error is never so.
pk_gone_reply() { [[ $1 == ref=* ]] && { [ "$(field found "$1")" = 0 ] || [ "$(field inworld "$1")" = 0 ]; }; }
# pk_shape <ref reply>: what its 3D shows (count:hash), empty when its 3D is not loaded or it is not found
pk_shape() { pk_is "$1" loaded3d 1 && field shown "$1"; }
# pk_judged <counts reply> <why>: how many of our player's own were judged so (0 when none)
pk_judged() { local j n; j=$(field judged "$1"); n=$(tr ',' '\n' <<<"$j" | sed -n "s/^$2://p" | head -1); echo "${n:-0}"; }
# pk_why_untold <counts reply>: the counts that say why a pickup was not told (for a FAIL's reason)
pk_why_untold() { tr ' ' '\n' <<<"$1" | grep -E '^(takes|takesNoRef|takesOtherBase|sent|judged|queued|eventsDropped)=' | tr '\n' ' '; }

# ---- the checks (each reads both games through dc) ----------------------------------------------------------------

pk_count() { field "$2" "$(dc "$1" pickups)"; }
pk_ref() { dc "$1" "pickups ref $2"; }
pk_in() { pk_is "$(pk_ref "$1" "$2")" inworld 1; }
pk_gone() { pk_gone_reply "$(pk_ref "$1" "$2")"; }
pk_off() { pk_is "$(pk_ref "$1" "$2")" disabled 1; }
pk_harvested() { pk_is "$(pk_ref "$1" "$2")" harvested 1; }
pk_shows() { [ "$(pk_shape "$(pk_ref "$1" "$2")")" = "$3" ]; }
pk_ge() { local n; n=$(pk_count "$1" "$2"); [ "$n" -ge "$3" ] 2>/dev/null; }
# pk_stays <s> <check...>: the check holds at every look over that time
pk_stays() { local i t=$1; shift; for ((i = 0; i < t; i++)); do "$@" || return 1; sleep 1; done; "$@"; }

# pk_pick <who> <sort> <radius>: prints the nearest reference of that sort that game may take, which the other's world
# holds too, of the same base, in the world (and for a plant, not harvested there, its 3D loaded); 1 when there is none
pk_pick() {
	local who=$1 sort=$2 r ref base o
	o=$(other "$who")
	r=$(dc "$who" "pickups pick $sort $3")
	ref=$(pk_found "$r") || return 1
	base=$(field base "$r")
	r=$(pk_ref "$o" "$ref")
	pk_is "$r" inworld 1 && [ "$(field base "$r")" = "$base" ] || return 1
	case "$sort" in flora | tree) pk_is "$r" harvested 0 && pk_is "$r" loaded3d 1 || return 1 ;; esac
	echo "$ref"
}

# pk_take <who> <ref>: that game's player walks up to the reference (walkto: real movement) and activates it (the dev
# channel's take, refused out of reach). 1 and a RIG when it could not.
pk_take() {
	local who=$1 ref=$2 r k st
	r=$(dc "$who" "walkto $ref 0.3 70 20")
	[[ $r == walking* ]] || { rig_fault input "$who: walks up to $ref" "walkto replied '$r'"; return 1; }
	for ((k = 0; k < 50; k++)); do st=$(dc "$who" walkto); [[ $st == "not walking"* ]] && break; sleep 0.5; done
	still "$who"; sleep 1
	r=$(dc "$who" "pickups take $ref")
	[ "$r" = activated ] || { rig_fault walk "$who: takes $ref within reach" "'pickups take' replied '$r'"; return 1; }
	return 0
}

# pk_item <who>: that game's player takes a loose item; it is out of its world and told, and out of the other's (disabled)
# within PICKUPS_WAIT. PK_REF: the reference (empty when none was taken).
PK_REF=""
pk_item() {
	local who=$1 o ref s0 a0
	o=$(other "$who"); PK_REF=""
	ref=$(pk_pick "$who" item "$PICKUPS_RADIUS") || { rig_fault state "$who: finds a loose item to take that $o's world holds too" "$(dc "$who" "pickups pick item $PICKUPS_RADIUS" | cut -c1-200)"; return 1; }
	s0=$(pk_count "$who" sent); a0=$(pk_count "$o" applied)
	pk_take "$who" "$ref" || return 1
	if wait_until 10 pk_gone "$who" "$ref"; then pass "$who: took $ref out of its world (into its inventory)" "$(pk_ref "$who" "$ref" | cut -c1-160)"
	else rig_fault input "$who: took $ref out of its world (into its inventory)" "$(pk_ref "$who" "$ref" | cut -c1-160)"; return 1; fi
	PK_REF=$ref
	if is_num "$s0" && wait_until 5 pk_ge "$who" sent $((s0 + 1)); then pass "$who: told the session it took $ref" "sent=$(pk_count "$who" sent)"
	else fail "$who: told the session it took $ref" "sent was '$s0': $(pk_why_untold "$(dc "$who" pickups)")"; fi
	if wait_until "$PICKUPS_WAIT" pk_off "$o" "$ref"; then pass "$o: $ref the other took is out of its world (disabled)" "$(pk_ref "$o" "$ref" | cut -c1-160)"
	else fail "$o: $ref the other took is out of its world (disabled)" "$(pk_ref "$o" "$ref" | cut -c1-160) | $(dc "$o" pickups | cut -c1-300)"; fi
	if is_num "$a0"; then expect_ge "$o: counts what it took out for the session" "$(pk_count "$o" applied)" $((a0 + 1)); fi
}

# pk_book <who>: that game's player takes a book; it stays in the other's world, and the picker judges it its own
pk_book() {
	local who=$1 o ref b0
	o=$(other "$who")
	ref=$(pk_pick "$who" book "$PICKUPS_RADIUS") || { rig_fault state "$who: finds a book to take that $o's world holds too" "$(dc "$who" "pickups pick book $PICKUPS_RADIUS" | cut -c1-200)"; return; }
	b0=$(pk_judged "$(dc "$who" pickups)" book)
	pk_take "$who" "$ref" || return
	wait_until 10 pk_gone "$who" "$ref" || { rig_fault input "$who: took the book $ref" "$(pk_ref "$who" "$ref" | cut -c1-160)"; return; }
	if pk_stays "$PICKUPS_WAIT" pk_in "$o" "$ref"; then pass "$o: the book $ref the other took stays in its world (${PICKUPS_WAIT} s): each player reads their own" ""
	else fail "$o: the book $ref the other took stays in its world (${PICKUPS_WAIT} s): each player reads their own" "$(pk_ref "$o" "$ref" | cut -c1-160)"; fi
	expect_ge "$who: judged the book its own (judged= book)" "$(pk_judged "$(dc "$who" pickups)" book)" $((b0 + 1))
}

# pk_save <who> / pk_load <who>: that game saves ($SAVE_NAME) / loads it and is back in the world (and in the session when
# a_session is 1)
pk_save() {
	local r
	r=$(dc "$1" "console save $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "$1: saves" "'console save' replied '$r'"; return 1; }
	sleep 8
	[ "$(dc "$1" ping)" = pong ] || { fail "$1: the game lives through its save" "its dev channel went silent"; return 1; }
}
pk_load() {
	local r
	r=$(dc "$1" "console load $SAVE_NAME"); [ "$r" = ran ] || { rig_broken input "$1: loads its save" "'console load' replied '$r'"; return 1; }
	sleep 5
	wait_until 90 in_world "$1" || { fail "$1: back in the world after its load" "$(dc "$1" where | cut -c1-160)"; return 1; }
	[ "${2:-1}" = 0 ] || wait_until 30 welcomed "$1" || { fail "$1: still in the session after its load" "$(dc "$1" 'session status' | cut -c1-160)"; return 1; }
}

# pk_friend_load: the VM saves, the host takes an item, the VM loads: the item goes out of its world again
pk_friend_load() {
	local ref q0
	pk_save vm || return
	pk_item host || return
	ref=$PK_REF
	q0=$(pk_count vm asks)
	pk_load vm || return
	if is_num "$q0" && wait_until 10 pk_ge vm asks $((q0 + 1)); then pass "vm: asked the session again after its load" "asks=$(pk_count vm asks)"
	else fail "vm: asked the session again after its load" "asks was '$q0': $(dc vm pickups | cut -c1-300)"; fi
	if wait_until "$PICKUPS_WAIT" pk_off vm "$ref"; then pass "vm: $ref the host took is out of its world again after its load" "$(pk_ref vm "$ref" | cut -c1-160)"
	else fail "vm: $ref the host took is out of its world again after its load" "$(pk_ref vm "$ref" | cut -c1-160) | $(dc vm pickups | cut -c1-300)"; fi
}

# pk_own_load: the VM saves, takes an item, loads: the item is in its world again, and stays (never sent its own); the
# host's world still has it out
pk_own_load() {
	local ref
	pk_save vm || return
	pk_item vm || return
	ref=$PK_REF
	pk_load vm || return
	if wait_until 10 pk_in vm "$ref"; then pass "vm: $ref lies in its world again after loading a save from before it took it" "$(pk_ref vm "$ref" | cut -c1-160)"
	else fail "vm: $ref lies in its world again after loading a save from before it took it" "$(pk_ref vm "$ref" | cut -c1-160)"; return; fi
	if pk_stays "$PICKUPS_WAIT" pk_in vm "$ref"; then pass "vm: $ref stays in its world (${PICKUPS_WAIT} s): the session never sends a player its own" ""
	else fail "vm: $ref stays in its world (${PICKUPS_WAIT} s): the session never sends a player its own" "$(pk_ref vm "$ref" | cut -c1-160)"; fi
	if pk_off host "$ref"; then pass "host: $ref is still out of its world" ""; else fail "host: $ref is still out of its world" "$(pk_ref host "$ref" | cut -c1-160)"; fi
}

# pk_rejoin: the VM leaves, the host takes an item, the VM joins again: the item goes out of its world (the catch-up)
pk_rejoin() {
	local ref r c0
	ref=$(pk_pick host item "$PICKUPS_RADIUS") || { rig_fault state "host: finds a loose item to take that the VM's world holds too" "$(dc host "pickups pick item $PICKUPS_RADIUS" | cut -c1-200)"; return; }
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	pk_take host "$ref" || { r=$(vm_join); wait_until 90 joined_both; return; }
	wait_until 10 pk_gone host "$ref" || rig_fault input "host: took $ref" "$(pk_ref host "$ref" | cut -c1-160)"
	if pk_in vm "$ref"; then pass "vm: $ref is in its world while it is out of the session" ""; else fail "vm: $ref is in its world while it is out of the session" "$(pk_ref vm "$ref" | cut -c1-160)"; fi
	c0=$(pk_count vm catchUps)
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; return; fi
	if wait_until "$PICKUPS_WAIT" pk_off vm "$ref"; then pass "vm: $ref the host took while it was away is out of its world once it is back" "$(pk_ref vm "$ref" | cut -c1-160)"
	else fail "vm: $ref the host took while it was away is out of its world once it is back" "$(pk_ref vm "$ref" | cut -c1-160) | $(dc vm pickups | cut -c1-300)"; fi
	if is_num "$c0"; then expect_ge "vm: heard the session's catch-up with its welcome" "$(pk_count vm catchUps)" $((c0 + 1)); fi
}

# pk_plant <who> <flora|tree>: that game's player harvests a plant of that sort; it reads harvested there and told, and in
# the other's within PICKUPS_WAIT, still in the world, showing what the harvester's own 3D shows after its harvest. PK_REF:
# the plant.
pk_plant() {
	local who=$1 sort=$2 o ref s0 b0 b1 r
	o=$(other "$who"); PK_REF=""
	ref=$(pk_pick "$who" "$sort" "$PICKUPS_FLORA_RADIUS") || { rig_fault state "$who: finds a $sort to harvest, not harvested in $o's world either, its 3D loaded in both" "$(dc "$who" "pickups pick $sort $PICKUPS_FLORA_RADIUS" | cut -c1-200)"; return 1; }
	s0=$(pk_count "$who" harvestsSent)
	b0=$(pk_shape "$(pk_ref "$who" "$ref")")
	pk_take "$who" "$ref" || return 1
	if wait_until 10 pk_harvested "$who" "$ref"; then pass "$who: harvested the $sort $ref" "$(pk_ref "$who" "$ref" | cut -c1-200)"
	else rig_fault input "$who: harvested the $sort $ref" "$(pk_ref "$who" "$ref" | cut -c1-200)"; return 1; fi
	PK_REF=$ref
	if is_num "$s0" && wait_until 5 pk_ge "$who" harvestsSent $((s0 + 1)); then pass "$who: told the session it harvested $ref" "$(dc "$who" pickups | tr ' ' '\n' | grep -E '^(harvestsSent|paired|scans)=' | tr '\n' ' ')"
	else fail "$who: told the session it harvested $ref" "harvestsSent was '$s0': $(dc "$who" pickups | cut -c1-300)"; fi
	if wait_until "$PICKUPS_WAIT" pk_harvested "$o" "$ref"; then pass "$o: the $sort $ref the other harvested reads harvested" "$(pk_ref "$o" "$ref" | cut -c1-200)"
	else fail "$o: the $sort $ref the other harvested reads harvested" "$(pk_ref "$o" "$ref" | cut -c1-200) | $(dc "$o" pickups | cut -c1-300)"; fi
	if pk_in "$o" "$ref"; then pass "$o: the $sort $ref is still in its world (harvested, not taken)" ""; else fail "$o: the $sort $ref is still in its world (harvested, not taken)" "$(pk_ref "$o" "$ref" | cut -c1-160)"; fi
	# What the player sees: the harvester's own 3D after its harvest (read once it settles), and the other's the same.
	sleep 2
	b1=$(pk_shape "$(pk_ref "$who" "$ref")")
	if [ -z "$b1" ]; then rig_fault state "$who: the $sort $ref's 3D loaded after its harvest" "$(pk_ref "$who" "$ref" | cut -c1-200)"; return 1; fi
	if wait_until "$PICKUPS_WAIT" pk_shows "$o" "$ref" "$b1"; then
		if [ "$b0" = "$b1" ]; then warn "$o: the $sort $ref shows what the harvester's shows after the harvest" "the reading sees no change in the harvester's own 3D ($b0 before and after): this check proves nothing for this plant"
		else pass "$o: the $sort $ref shows what the harvester's shows after the harvest (its 3D made again)" "shown=$b1 (was $b0)"; fi
	else
		r=$(pk_ref "$o" "$ref")
		fail "$o: the $sort $ref shows what the harvester's shows after the harvest (its 3D made again)" "harvester's shown=$b1 (was $b0); $o's: loaded3d=$(field loaded3d "$r") shown=$(field shown "$r")"
	fi
}

# pk_flora_save: the host harvests, the VM saves, leaves and loads that save alone: the plant reads harvested there
pk_flora_save() {
	local ref r
	pk_plant host flora || return
	ref=$PK_REF
	pk_save vm || return
	r=$(dc vm 'session leave'); [[ $r == left* ]] || { fail "vm: leaves" "$r"; return; }
	if pk_load vm 0; then
		if pk_harvested vm "$ref"; then pass "vm: the plant $ref the host harvested reads harvested in its save, loaded out of the session" "$(pk_ref vm "$ref" | cut -c1-160)"
		else fail "vm: the plant $ref the host harvested reads harvested in its save, loaded out of the session" "$(pk_ref vm "$ref" | cut -c1-160)"; fi
	fi
	if r=$(vm_join) && wait_until 90 joined_both; then pass "vm: joins again" "$(dc vm 'session status' | cut -c1-80)"; else fail "vm: joins again" "$r"; fi
}

step_pickups() {
	local cell found="" w
	section "pickups: a loose item one player takes goes out of the other's world"
	for cell in $PICKUPS_CELLS; do
		doors_coc "$cell" || continue
		sleep 3
		if pk_pick host item "$PICKUPS_RADIUS" >/dev/null && pk_pick vm item "$PICKUPS_RADIUS" >/dev/null; then found=$cell; break; fi
		warn "both: a loose item each may take in $cell" "host: $(dc host "pickups pick item $PICKUPS_RADIUS" | cut -c1-120)"
	done
	if [ -z "$found" ]; then rig_unhealed state "both: a cell of PICKUPS_CELLS with a loose item each may take" "$PICKUPS_CELLS"
	else
		pk_item host
		pk_item vm

		section "pickups: a book stays each player's own"
		pk_book host
		pk_book vm

		section "pickups: a save from before the other's take, loaded, has it out again (the VM loads: no load on the host)"
		pk_friend_load

		section "pickups: a save from before our own take, loaded, has it back, ours to take again (the VM loads)"
		pk_own_load

		section "pickups: a player who joins again hears what was taken while it was away (the VM rejoins)"
		pk_rejoin
	fi

	section "pickups: a plant one player harvests is harvested in the other's world, and shows it"
	if doors_coc "$PICKUPS_FLORA_CELL"; then
		sleep 3
		pk_plant host flora
		pk_plant vm flora
		pk_plant host tree
		pk_plant vm tree

		section "pickups: a harvest made for the session is kept in a save (the VM saves and loads)"
		pk_flora_save
	fi

	# How the plugin made its game writes (until the frame hook, off the game's main thread: counted, for the record), and
	# the take events that named no reference or another base (a FAIL to tell the session reads them)
	for w in host vm; do echo "     $w: $(dc $w pickups | tr ' ' '\n' | grep -E '^(offmain|takesNoRef|takesOtherBase|bothTook|parked|reapplied|held|waited|attached|ownTakesWatched|overflow|refused|eventsDropped|heardDropped|plants|censuses|judged|skipped|respawnHours)=' | tr '\n' ' ')"; done

	section "pickups: both back in $SMOKE_CELL"
	doors_coc "$SMOKE_CELL"
}

if [ "${BASH_SOURCE[0]}" = "$0" ] && [ "${1:-}" = selftest ]; then
	: "${VM_SSH:=selftest-no-vm}"; export SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
	. "$(dirname "${BASH_SOURCE[0]}")/../steps.sh"
	bad=0; d=$(mktemp -d); CHECKS=$d; A_OUT=$d/run; a_init pickupstest >/dev/null
	t() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got [$2] want [$3]"; bad=1; fi; }
	sleep() { :; }; lock_keepalive() { :; }
	PICKUPS_WAIT=2

	P='ref=0001A2B3 base=00064B3D sort=potion harvested=0 quest=0 runtime=0 crime=0 aimed=0 dist=212 x=1 y=2 z=3'
	t "found: the reference a pick names" "$(pk_found "$P")" 0001A2B3
	t "found: none" "$(pk_found none || echo none)" none
	t "found: an error names none" "$(pk_found 'error: not in the world' || echo none)" none
	R='ref=0001A2B3 found=1 base=00064B3D inworld=0 disabled=1 deleted=0 harvested=0 held=0 waiting=0 watch=none known=1 ours=0 seenGrowing=0 loaded3d=1 shown=3:1A2B3C4D dist=200'
	t "is: a field of a reference found" "$(pk_is "$R" disabled 1 && echo yes)" yes
	t "is: another value" "$(pk_is "$R" inworld 1 || echo no)" no
	t "is: not found is never so" "$(pk_is 'ref=0001A2B3 found=0 base=00000000 inworld=0 disabled=0' inworld 0 || echo no)" no
	t "is: an error is never so" "$(pk_is 'error: usage' inworld 0 || echo no)" no
	t "gone: out of the world" "$(pk_gone_reply "$R" && echo yes)" yes
	t "gone: no longer in memory (freed after the pickup)" "$(pk_gone_reply 'ref=0001A2B3 found=0 base=00000000 inworld=0 disabled=0' && echo yes)" yes
	t "gone: still in the world is not" "$(pk_gone_reply 'ref=0001A2B3 found=1 base=00064B3D inworld=1 disabled=0' || echo no)" no
	t "gone: an error is not" "$(pk_gone_reply 'error: usage' || echo no)" no
	t "shape: what its 3D shows" "$(pk_shape "$R")" 3:1A2B3C4D
	t "shape: none when its 3D is not loaded" "$(pk_shape 'ref=0001A2B3 found=1 loaded3d=0 shown=0:811C9DC5')" ""
	t "judged: book" "$(pk_judged 'installed=1 judged=shared:4,book:2,key:1 skipped=-' book)" 2
	t "judged: none of that" "$(pk_judged 'installed=1 judged=shared:4 skipped=-' quest)" 0
	t "judged: none at all" "$(pk_judged 'installed=1 judged=- skipped=-' book)" 0
	t "untold: the counts that say why" "$(pk_why_untold 'installed=1 takes=0 takesNoRef=3 takesOtherBase=0 sent=0 judged=- x=1')" "takes=0 takesNoRef=3 takesOtherBase=0 sent=0 judged=- "

	# Both games faked: references (item I1, book B1, flora F1, tree T1) each with a state in each world (in, held, gone,
	# off, harv, flag); SHARE=1 is the mod at work (a take or harvest is made in the other world), 0 a mod that shares
	# nothing; LEAK=1 a mod that shares a book too; FLAT=1 a mod that sets a harvest's flag but never makes its 3D again
	# (flag: reads harvested, shows the growing plant); FREE=1 the engine frees a picked-up reference (gone: found=0);
	# SAME=1 a plant whose harvest the shape reading sees no change in.
	fake() {
		FK=$d/fake; rm -rf "$FK"; mkdir -p "$FK"; SHARE=$1; LEAK=${2:-0}; FLAT=${3:-0}; FREE=${4:-0}; SAME=${5:-0}
		for w in host vm; do for r in 000A0001 000B0001 000F0001 000E0001; do echo in > "$FK/$w.$r"; done; for c in sent harvestsSent applied asks catchUps book; do echo 0 > "$FK/$w.$c"; done; done
	}
	get() { cat "$FK/$1.$2" 2>/dev/null || echo 0; }
	put() { echo "$3" > "$FK/$1.$2"; }
	up() { put "$1" "$2" $(( $(get "$1" "$2") + 1 )); }
	base() { case "$1" in 000A*) echo 00064B3D ;; 000B*) echo 0001AFC4 ;; 000E*) echo 000BC1E2 ;; *) echo 000B6FBE ;; esac; }
	shown() { case "$1" in harv) [ "$SAME" = 1 ] && echo 4:0000AAAA || echo 2:0000BBBB ;; *) echo 4:0000AAAA ;; esac; }
	refline() {
		local s; s=$(get "$1" "$2")
		if [ "$s" = gone ]; then echo "ref=$2 found=0 base=00000000 inworld=0 disabled=0 deleted=0 harvested=0 held=0 waiting=0 watch=none known=0 ours=0 seenGrowing=0 loaded3d=0 shown=0:811C9DC5 dist=-1"; return; fi
		echo "ref=$2 found=1 base=$(base "$2") inworld=$([ "$s" = in ] || [ "$s" = harv ] || [ "$s" = flag ] && echo 1 || echo 0) disabled=$([ "$s" = off ] && echo 1 || echo 0) deleted=0 harvested=$([ "$s" = harv ] || [ "$s" = flag ] && echo 1 || echo 0) held=$([ "$s" = held ] && echo 1 || echo 0) waiting=0 watch=none known=0 ours=0 seenGrowing=0 loaded3d=1 shown=$(shown "$s") dist=100"
	}
	pick() {  # pick <who> <sort>: the fake's nearest of that sort still there
		local r; case "$2" in item) r=000A0001 ;; book) r=000B0001 ;; flora) r=000F0001 ;; tree) r=000E0001 ;; esac
		if [ "$(get "$1" "$r")" = in ]; then echo "ref=$r base=$(base "$r") sort=$2 harvested=0 quest=0 runtime=0 crime=0 aimed=0 dist=300"; else echo none; fi
	}
	ftake() {  # ftake <who> <ref>: an activation
		local o; o=$(other "$1")
		case "$2" in
			000F* | 000E*) put "$1" "$2" harv; up "$1" harvestsSent; [ "$SHARE" = 1 ] && put "$o" "$2" "$([ "$FLAT" = 1 ] && echo flag || echo harv)" ;;
			000B*) put "$1" "$2" held; up "$1" book; [ "$LEAK" = 1 ] && put "$o" "$2" off ;;
			*) put "$1" "$2" "$([ "$FREE" = 1 ] && echo gone || echo held)"; up "$1" sent; [ "$SHARE" = 1 ] && { put "$o" "$2" off; up "$o" applied; } ;;
		esac
		echo activated
	}
	dc() {
		local w=$1 c=$2 a; read -ra a <<<"$c"
		case "$c" in
			"pickups pick "*) pick "$w" "${a[2]}" ;;
			"pickups ref "*) refline "$w" "${a[2]}" ;;
			"pickups take "*) ftake "$w" "${a[2]}" ;;
			pickups) echo "installed=1 takes=$(get "$w" sent) takesNoRef=0 takesOtherBase=0 sent=$(get "$w" sent) harvestsSent=$(get "$w" harvestsSent) applied=$(get "$w" applied) asks=$(get "$w" asks) catchUps=$(get "$w" catchUps) judged=book:$(get "$w" book) offmain=0" ;;
			"walkto "*) [ "${a[1]}" = stop ] && echo stopped || echo "walking to ${a[1]}" ;;
			walkto) echo "not walking" ;;
			"pad off") echo off ;;
			*) echo "error: the fake has no '$c'" ;;
		esac
	}
	run() { local sd=$d/r$1; mkdir -p "$sd"; ( A_OUT=$sd; a_init pt >/dev/null; "${@:2}"; a_summary ) 2>&1; }
	verdicts() { grep -oE '^(PASS|FAIL|WARN|RIG|RIGFAULT) ' <<<"$1" | tr -d ' ' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}'; }

	fake 1; o=$(run 1 eval 'pk_item host; echo "ref=$PK_REF"')
	t "item: taken, told, out of the other's world, counted there: passes" "$(verdicts "$o")" "PASS=4 "
	t "item: ... its reference" "$(grep -o '^ref=.*' <<<"$o")" ref=000A0001
	t "item: ... the states" "$(get host 000A0001),$(get vm 000A0001)" held,off
	fake 1 0 0 1; o=$(run 9 pk_item vm)
	t "item: a picked-up reference the engine freed (found=0) is out for the picker: passes" "$(verdicts "$o")" "PASS=4 "
	fake 0; o=$(run 2 pk_item vm)
	t "item: a mod that shares nothing fails in the other's world" "$(grep -c '^FAIL host: 000A0001 the other took is out of its world' <<<"$o")" 1
	fake 1; put vm 000A0001 held; o=$(run 3 pk_item host)
	t "item: one the other's world lacks is no test (RIG), never FAIL" "$(verdicts "$o" | grep -c FAIL),$(grep -c '^RIG' <<<"$o")" 0,1
	fake 1; o=$(run 4 pk_book host)
	t "book host to VM: stays in the other's world, judged its own: passes" "$(verdicts "$o")" "PASS=2 "
	fake 1; o=$(run 10 pk_book vm)
	t "book VM to host: stays in the other's world, judged its own: passes" "$(verdicts "$o")" "PASS=2 "
	fake 1 1; o=$(run 5 pk_book host)
	t "book: one the other's world loses fails" "$(grep -c '^FAIL vm: the book 000B0001 the other took stays' <<<"$o")" 1
	fake 1 1; o=$(run 11 pk_book vm)
	t "book: ... the other way too" "$(grep -c '^FAIL host: the book 000B0001 the other took stays' <<<"$o")" 1
	fake 1; o=$(run 6 pk_plant host flora)
	t "flora: harvested, told, harvested in the other's, still there, shows the same: passes" "$(verdicts "$o")" "PASS=5 "
	t "flora: ... the states" "$(get host 000F0001),$(get vm 000F0001)" harv,harv
	fake 1; o=$(run 12 pk_plant vm tree)
	t "tree: the same checks pass" "$(verdicts "$o")" "PASS=5 "
	fake 0; o=$(run 7 pk_plant vm flora)
	t "flora: a mod that shares no harvest fails" "$(grep -c '^FAIL host: the flora 000F0001 the other harvested reads harvested' <<<"$o")" 1
	fake 1 0 1; o=$(run 13 pk_plant host flora)
	t "flora: a harvest flag set without its 3D made again fails the shape check (only that)" "$(verdicts "$o")" "FAIL=1 PASS=4 "
	t "flora: ... named" "$(grep -c '^FAIL vm: the flora 000F0001 shows what the harvester' <<<"$o")" 1
	fake 1 0 0 0 1; o=$(run 14 pk_plant host tree)
	t "tree: a harvest the shape reading sees no change in is a WARN, never a PASS" "$(verdicts "$o")" "PASS=4 WARN=1 "
	fake 1; put vm 000F0001 harv; o=$(run 8 pk_plant host flora)
	t "flora: a plant harvested in the other's world already is no test (RIG)" "$(verdicts "$o" | grep -c FAIL),$(grep -c '^RIG' <<<"$o")" 0,1

	# the step is wired into the harness: in STEPS before final, a session step, in the default run
	t "pickups is a step, before final" "$(case " $STEPS " in *" pickups "*"final "*) echo yes;; esac)" yes
	t "pickups needs a session" "$(case " $SESSION_STEPS " in *" pickups "*) echo yes;; esac)" yes
	t "pickups runs by default" "$(case " $OPTIN_STEPS " in *" pickups "*) echo no;; *) echo yes;; esac)" yes
	t "pickups has its line" "$([ -n "${STEP_INFO[pickups]:-}" ] && echo yes)" yes
	[ $bad -eq 0 ] && echo "pickups.sh selftest: all ok" || echo "pickups.sh selftest: FAILED"
	rm -rf "$d"; exit $bad
fi
