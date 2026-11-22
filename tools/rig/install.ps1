# Puts a prepared payload into one machine's Skyrim folder, or takes it out again. Run on that machine; install.sh
# prepares the payload (SKSE64, the Address Library file, Crash Logger, the plugin and its ini) and runs this on the
# host and, over ssh, on the VM.
#
#   install.ps1 -Game <game folder> -Payload <folder laid out like the game folder> -Record <file> [-Expect <hash list>]
#   install.ps1 -Game <game folder> -Undo -Record <file>
#
# Install: every file of the payload is copied to the same place under the game folder and its hash checked after. A
# file that was there already is first copied to a backup folder beside the record; the record lists every file
# written, its hash and its backup, one line each, and every folder made for them. Nothing is written while the game
# runs.
# Undo: every file in the record that still has the hash we wrote is moved out of the game folder (into the record's
# "removed" folder, never deleted) and its backup, if any, put back; a file that changed since is left alone and named.
# The folders the install made are taken out once empty. Nothing outside the record is touched.
param(
	[Parameter(Mandatory)] [string]$Game,
	[string]$Payload = '',
	[Parameter(Mandatory)] [string]$Record,
	[string]$Expect = '',  # install.sh's hash list of the payload: checked before anything is written
	[switch]$Undo
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath (Join-Path $Game 'SkyrimSE.exe'))) { throw "No SkyrimSE.exe in $Game" }
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Skyrim is running: stop it first' }
if (Get-Process Fallout4 -ErrorAction SilentlyContinue) { throw 'Fallout 4 is running: the rig runs one game at a time' }
$Game = (Resolve-Path -LiteralPath $Game).Path
$recordDir = Split-Path -Parent $Record
function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

if ($Undo) {
	if (-not (Test-Path -LiteralPath $Record)) { throw "No record at $Record" }
	$removed = Join-Path $recordDir ("removed-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
	$kept = 0; $back = 0; $gone = 0; $dirs = @()
	foreach ($line in Get-Content -LiteralPath $Record) {
		if ($line.StartsWith("#dir`t")) { $dirs += $line.Substring(5); continue }
		if (-not $line -or $line.StartsWith('#')) { continue }
		$rel, $hash, $backup = $line -split "`t"
		$target = Join-Path $Game $rel
		if (-not (Test-Path -LiteralPath $target)) { "gone already: $rel"; $gone++ }
		elseif ((Hash $target) -ne $hash) { "CHANGED since install, left alone: $rel"; $kept++; continue }
		else {
			$to = Join-Path $removed $rel
			New-Item -ItemType Directory -Force (Split-Path -Parent $to) | Out-Null
			Move-Item -LiteralPath $target -Destination $to
		}
		if ($backup -and $backup -ne '-') {
			Copy-Item -LiteralPath $backup -Destination $target -Force
			if ((Hash $target) -ne (Hash $backup)) { throw "restoring $rel from its backup: the hash differs after the copy" }
			$back++
		}
	}
	# The folders the install made, deepest first (each was recorded after its parent), taken out only when empty
	$dirsGone = 0
	[array]::Reverse($dirs)
	foreach ($rel in $dirs) {
		$dir = Join-Path $Game $rel
		if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
		if (@(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) { Remove-Item -LiteralPath $dir; $dirsGone++ }
		else { "folder not empty, left: $rel" }
	}
	"undone: our files moved to $removed; $back earlier files put back; $kept changed files left alone; $gone were gone already; $dirsGone empty folders the install made taken out"
	if ($kept -gt 0) { exit 1 }
	exit 0
}

if (-not $Payload) { throw '-Payload is needed to install' }
if (Test-Path -LiteralPath $Record) { throw "A record is already at ${Record}: undo that install first (-Undo), or name a new record" }
$Payload = (Resolve-Path -LiteralPath $Payload).Path
# The payload as install.sh made it (a copy over ssh must not be trusted blind): "<sha256>  <relative path>" lines
if ($Expect) {
	$listed = 0
	foreach ($line in Get-Content -LiteralPath $Expect) {
		if (-not $line.Trim()) { continue }
		$hash = $line.Substring(0, 64).ToLowerInvariant(); $rel = $line.Substring(64).TrimStart(' ', '*').Replace('/', '\')
		$file = Join-Path $Payload $rel
		if (-not (Test-Path -LiteralPath $file)) { throw "the payload lacks $rel" }
		if ((Hash $file) -ne $hash) { throw "the payload's $rel is not the file install.sh made (hash differs)" }
		$listed++
	}
	$present = @(Get-ChildItem -LiteralPath $Payload -Recurse -File).Count
	if ($present -ne $listed) { throw "the payload holds $present files, its list $listed" }
	"payload checked: $listed files, every hash as listed"
}
$backupDir = Join-Path $recordDir ("backup-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $recordDir | Out-Null
$lines = @("# SkyrimCoop rig install $(Get-Date -Format s) into $Game", "# relative path`tsha256 written`tbackup of what was there before (- for none)")
$written = 0; $replaced = 0
foreach ($file in Get-ChildItem -LiteralPath $Payload -Recurse -File) {
	$rel = $file.FullName.Substring($Payload.Length).TrimStart('\')
	$target = Join-Path $Game $rel
	$want = Hash $file.FullName
	$backup = '-'
	if (Test-Path -LiteralPath $target) {
		# Backed up even when it is the same file: undo then puts back exactly what was there, whoever put it there.
		$backup = Join-Path $backupDir $rel
		New-Item -ItemType Directory -Force (Split-Path -Parent $backup) | Out-Null
		Copy-Item -LiteralPath $target -Destination $backup
		if ((Hash $backup) -ne (Hash $target)) { throw "backing up ${rel}: the hash differs after the copy" }
		$replaced++
	}
	# Folders made for it are recorded ("#dir", parents first), so undo can take them out again when they are empty
	$made = @(); $dir = Split-Path -Parent $target
	while (-not (Test-Path -LiteralPath $dir)) { $made = @($dir) + $made; $dir = Split-Path -Parent $dir }
	New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
	foreach ($d in $made) { $lines += "#dir`t" + $d.Substring($Game.Length).TrimStart('\') }
	Copy-Item -LiteralPath $file.FullName -Destination $target -Force
	if ((Hash $target) -ne $want) { throw "copying ${rel}: the hash differs after the copy" }
	$lines += "$rel`t$want`t$backup"
	# written as it goes: an install stopped half way can still be undone
	Set-Content -LiteralPath $Record -Value $lines -Encoding utf8
	$written++
}
"installed $written files into $Game ($replaced replaced, their old copies in $backupDir); record: $Record"
