#!/bin/bash
# selftest.sh: the rig harness's own tests, with no game, no VM and no rig (the Linux tests run it on every push):
# assert.sh (checks, the log delta, the privacy scan, the breaker, the verdicts), crashlib.sh (reading crash logs),
# steps.sh and steps/*.sh (the steps' reading of the games' replies, both games faked) and smoke.sh (its options and wiring).
# Exit code: the number of parts that failed.
export VM_SSH=selftest-no-vm SKYRIMCOOP_STAGE=${SKYRIMCOOP_STAGE:-$(mktemp -d)}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
failed=0
for part in "assert.sh selftest" "crashlib.sh selftest" "steps.sh selftest" "steps/forms.sh selftest" "steps/npcs.sh selftest" "steps/npccombat.sh selftest" "steps/actions.sh selftest" "steps/faces.sh selftest" "steps/swim.sh selftest" "steps/ride.sh selftest" "steps/shots.sh selftest" "steps/sit.sh selftest" "steps/peace.sh selftest" "smoke.sh --selftest"; do
	echo "### $part"
	# shellcheck disable=SC2086
	bash "$here"/$part || { failed=$((failed + 1)); echo "### $part: FAILED"; }
done
[ $failed -eq 0 ] && echo "rig harness selftest: all ok" || echo "rig harness selftest: $failed part(s) FAILED"
exit $failed
