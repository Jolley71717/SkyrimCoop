# Starts Skyrim through SKSE on this machine and waits until the plugin is up, with nobody at the keyboard. Run on that
# machine (start.sh runs it on the host, and over ssh on the VM with -ViaTask).
#
#   start.ps1 -Game <game folder> [-Cell <cell>] [-NoAudio] [-WaitSeconds 240] [-ViaTask <in-vm-run.ps1>]
#   start.ps1 -RestoreIni          puts Skyrim.ini back as it was before a start with -Cell
#
# - The game is started with skse64_loader.exe from the game folder (SKSE 2.2.3 and later need no Steam loader; Steam
#   must be running and signed in). A start without Steam gets no Steam in the game, and the first test then fails.
# - -Cell loads a cell from the main menu with no character creation: Skyrim runs Skyrim.ini [General]
#   sStartingConsoleCommand at the main menu, so "coc <cell>" there is what typing it in the console would be. Without
#   -Cell the game stays at the main menu, which is enough for the Steam test. Skyrim.ini is backed up before the key
#   is set (*.skyrimcoop-before-start) and put back by -RestoreIni, or by the next start without -Cell.
# - -NoAudio: the game's sound off (Skyrim.ini [Audio] bEnableAudio=0), for a machine with no audio device. The rig's
#   VM has none (a Hyper-V basic session), and Skyrim then sometimes dies on its audio thread seconds after the start
#   (a null call, no plugin frame). It stays set: it is the VM's setting, not a test's. The first time it is set the
#   ini is backed up as *.skyrimcoop-before-noaudio; it is set before -Cell's backup, so -RestoreIni keeps it.
# - A game that dies in its first 60 s with a new crash log whose call stack has no SkyrimCoop frame is started once
#   more, and the retry and the crash log's name are said. Two such deaths in a row end the start (the rig's circuit
#   breaker): a game that can't start is the rig's to fix, not a test's to retry.
# - -ViaTask: an ssh session cannot show a window, so on the VM the loader is started on the logged-in desktop by the
#   rig's scheduled-task helper (F4Multiplayer's in-vm-run.ps1).
# - Done when SkyrimCoop.log is new and says "Game data loaded" and the tick ran; the log's last lines are shown.
param(
	[string]$Game = '',
	[string]$Cell = '',
	[switch]$NoAudio,
	[int]$WaitSeconds = 240,
	[string]$ViaTask = '',
	[switch]$RestoreIni
)
$ErrorActionPreference = 'Stop'

$myGames = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition'
$skyrimIni = Join-Path $myGames 'Skyrim.ini'
$before = "$skyrimIni.skyrimcoop-before-start"
$beforeNoAudio = "$skyrimIni.skyrimcoop-before-noaudio"
$skse = Join-Path $myGames 'SKSE'
$log = Join-Path $skse 'SkyrimCoop.log'
$skseLog = Join-Path $skse 'skse64.log'
$EARLY_SECONDS = 60

function Restore-Ini {
	if (Test-Path -LiteralPath $before) {
		Move-Item -LiteralPath $before -Destination $skyrimIni -Force
		'Skyrim.ini put back as it was before the start with -Cell'
	}
}

# Sets a_key=a_value in section a_section of Skyrim.ini (adding the section if missing); true when the file changed.
function Set-IniKey([string]$a_section, [string]$a_key, [string]$a_value) {
	$lines = [System.Collections.Generic.List[string]]::new([IO.File]::ReadAllLines($skyrimIni))
	$at = $lines.FindIndex([Predicate[string]] { param($l) $l -match "^\s*\[$([regex]::Escape($a_section))\]\s*$" })
	if ($at -lt 0) { $lines.Add("[$a_section]"); $at = $lines.Count - 1 }
	$end = $lines.FindIndex($at + 1, [Predicate[string]] { param($l) $l -match '^\s*\[' })
	if ($end -lt 0) { $end = $lines.Count }
	for ($i = $at + 1; $i -lt $end; $i++) {
		if ($lines[$i] -match "^\s*$([regex]::Escape($a_key))\s*=\s*(.*?)\s*$") {
			if ($Matches[1] -eq $a_value) { return $false }
			$lines[$i] = "$a_key=$a_value"
			[IO.File]::WriteAllLines($skyrimIni, $lines, [Text.Encoding]::Default)
			return $true
		}
	}
	$lines.Insert($at + 1, "$a_key=$a_value")
	[IO.File]::WriteAllLines($skyrimIni, $lines, [Text.Encoding]::Default)
	return $true
}

# The call stack of a crash log, without the module list (which always names SkyrimCoop.dll).
function Get-CrashStack([string]$a_path) {
	$text = Get-Content -LiteralPath $a_path -Raw
	if ($text -match '(?s)CALL STACK[^\n]*\n(.*?)\n\s*REGISTERS:') { return $Matches[1] }
	return ($text -split 'MODULES:')[0]
}

if ($RestoreIni) {
	if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Skyrim is running: the game writes Skyrim.ini as it quits; stop it first' }
	Restore-Ini
	exit 0
}

$loader = Join-Path $Game 'skse64_loader.exe'
if (-not (Test-Path -LiteralPath $loader)) { throw "No skse64_loader.exe in '$Game' (install.sh first)" }
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Skyrim is running already' }
if (Get-Process Fallout4 -ErrorAction SilentlyContinue) { throw 'Fallout 4 is running: one game at a time' }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw 'Steam is not running: start it (signed in) first' }
if (-not (Test-Path -LiteralPath $skyrimIni)) { throw "No ${skyrimIni}: run firststart.ps1 first" }

if ($NoAudio) {
	# Into the ini the game is left with, not a -Cell start's changed copy: put that back first.
	Restore-Ini | Out-Null
	if (-not (Test-Path -LiteralPath $beforeNoAudio)) { Copy-Item -LiteralPath $skyrimIni -Destination $beforeNoAudio }
	if (Set-IniKey 'Audio' 'bEnableAudio' '0') { 'Skyrim.ini: [Audio] bEnableAudio=0 (this machine has no audio device)' } else { 'Skyrim.ini: the sound is off already ([Audio] bEnableAudio=0)' }
}

if ($Cell) {
	if ($Cell -notmatch '^[A-Za-z0-9_]+$') { throw "'$Cell' is not a cell's editor id" }
	if (-not (Test-Path -LiteralPath $before)) { Copy-Item -LiteralPath $skyrimIni -Destination $before }
	$lines = [System.Collections.Generic.List[string]]::new()
	foreach ($line in [IO.File]::ReadAllLines($skyrimIni)) { if ($line -notmatch '^\s*sStartingConsoleCommand\s*=') { $lines.Add($line) } }
	$general = $lines.FindIndex([Predicate[string]] { param($l) $l -match '^\s*\[General\]\s*$' })
	if ($general -lt 0) { $lines.Insert(0, '[General]'); $general = 0 }
	$lines.Insert($general + 1, "sStartingConsoleCommand=coc $Cell")
	[IO.File]::WriteAllLines($skyrimIni, $lines, [Text.Encoding]::Default)
	"Skyrim.ini: sStartingConsoleCommand=coc $Cell"
} elseif (-not $NoAudio) {
	Restore-Ini
}

for ($attempt = 1; ; $attempt++) {
	$started = Get-Date
	if ($ViaTask) {
		& $ViaTask -Name "SkyrimCoop-Game-$(Get-Date -Format HHmmss)" -Exe $loader
	} else {
		Start-Process -FilePath $loader -WorkingDirectory $Game | Out-Null
		"started $loader"
	}

	$deadline = $started.AddSeconds($WaitSeconds)
	$state = 'no game process yet'
	$seen = $false
	while ((Get-Date) -lt $deadline) {
		Start-Sleep -Seconds 3
		$game = Get-Process SkyrimSE -ErrorAction SilentlyContinue
		if (-not $game) {
			if ($seen) { $state = 'the game quit'; break }
			$state = 'no SkyrimSE process'; continue
		}
		$seen = $true
		if (-not (Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).LastWriteTime -lt $started) { $state = 'the game runs; no new SkyrimCoop.log (SKSE did not load the plugin yet, or refused it: see skse64.log)'; continue }
		$text = Get-Content -LiteralPath $log -Raw
		if ($text -match 'Not loaded\.') { $state = 'the plugin refused to load (its log says why)'; break }
		if ($text -notmatch 'Game data loaded') { $state = 'the plugin is loaded; the game data is not yet'; continue }
		if ($text -notmatch 'Game thread: the first tick ran') { $state = 'game data loaded; the tick has not run yet'; continue }
		$state = 'up'
		if ($Cell) { Start-Sleep -Seconds 20 }  # the cell's load, after the menu; its end has no line of ours yet
		break
	}
	"state: $state ($([int]((Get-Date) - $started).TotalSeconds) s after the start)"
	if ($state -eq 'up' -or (Get-Process SkyrimSE -ErrorAction SilentlyContinue)) { break }

	# Did it die early, in a crash that isn't ours?
	$crash = Get-ChildItem -LiteralPath $skse -Filter 'crash-*.log' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $started } |
		Sort-Object LastWriteTime | Select-Object -Last 1
	if (-not $crash) { break }
	$after = [int]($crash.LastWriteTime - $started).TotalSeconds
	$ours = (Get-CrashStack $crash.FullName) -match 'SkyrimCoop'
	if ($after -gt $EARLY_SECONDS -or $ours) {
		"the game crashed $after s after the start: $($crash.Name)$(if ($ours) { ', with a SkyrimCoop frame in its call stack' })"
		break
	}
	if ($attempt -ge 2) {
		"the game died again $after s after the start ($($crash.Name), no SkyrimCoop frame): two in a row, stopping (circuit breaker; the rig needs looking at)"
		break
	}
	"the game died $after s after the start ($($crash.Name), no SkyrimCoop frame in its call stack): starting it once more"
	Start-Sleep -Seconds 5
}
if (Test-Path -LiteralPath $skseLog) { 'skse64.log, its last lines:'; Get-Content -LiteralPath $skseLog | Select-Object -Last 6 | ForEach-Object { "  $_" } }
if (Test-Path -LiteralPath $log) { 'SkyrimCoop.log, its last lines:'; Get-Content -LiteralPath $log | Select-Object -Last 15 | ForEach-Object { "  $_" } }
if ($state -ne 'up') { exit 1 }
