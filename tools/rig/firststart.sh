#!/bin/bash
# firststart.sh [check | undo]: firststart.ps1 on both machines: the ini files Skyrim's launcher would write on its first
# run, a window instead of the full screen, no intro video; and a check of Steam's update setting and the C++ runtime.
# The host gets the Medium preset at 1280x720, the VM Low at 1280x720 (as Fallout 4's rig). Both games must be closed.
#   firststart.sh          set it up (backs up any ini that is there first)
#   firststart.sh check    what is set and what is missing, on both; changes nothing
#   firststart.sh undo     both ini files back as they were
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
set -u
what=${1:-setup}
rig_guard || exit 1
no_fallout || exit 1
case "$what" in
setup) args_host=(-Game "$(cygpath -w "$HOST_GAME")" -Preset Medium); args_vm="-Game '$VM_GAME' -Preset Low" ;;
check) args_host=(-Game "$(cygpath -w "$HOST_GAME")" -Check); args_vm="-Game '$VM_GAME' -Check" ;;
undo) args_host=(-Undo); args_vm="-Undo" ;;
*) sed -n '2,7p' "$0"; exit 2 ;;
esac
rc=0
say "host:"
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$RIG/firststart.ps1")" "${args_host[@]}" || rc=1
say "vm:"
vmssh "New-Item -ItemType Directory -Force '$VM_DIR' | Out-Null" >/dev/null
vmscp "$RIG/firststart.ps1" "$VM_SSH:$VM_DIR/" || { say "vm: could not copy the script"; exit 1; }
out=$(vmssh "powershell -NoProfile -ExecutionPolicy Bypass -File $VM_DIR/firststart.ps1 $args_vm; 'exit ' + \$LASTEXITCODE")
printf '%s\n' "$out"
grep -q '^exit 0' <<<"$out" || rc=1
exit $rc
