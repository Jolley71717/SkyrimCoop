# Starts Skyrim through SKSE on this machine and waits until the plugin is up, with nobody at the keyboard. Run on that
# machine (start.sh runs it on the host, and over ssh on the VM with -ViaTask).
#
#   start.ps1 -Game <game folder> [-Cell <cell>] [-WaitSeconds 240] [-ViaTask <in-vm-run.ps1>]
#   start.ps1 -RestoreIni          puts Skyrim.ini back as it was before a start with -Cell
#
# - The game is started with skse64_loader.exe from the game folder (SKSE 2.2.3 and later need no Steam loader; Steam
#   must be running and signed in). A start without Steam gets no Steam in the game, and the first test then fails.
# - -Cell loads a cell from the main menu with no character creation: Skyrim runs Skyrim.ini [General]
#   sStartingConsoleCommand at the main menu, so "coc <cell>" there is what typing it in the console would be. Without
#   -Cell the game stays at the main menu, which is enough for the Steam test. Skyrim.ini is backed up before the key
#   is set (*.skyrimcoop-before-start) and put back by -RestoreIni, or by the next start without -Cell.
# - -ViaTask: an ssh session cannot show a window, so on the VM the loader is started on the logged-in desktop by the
#   rig's scheduled-task helper (F4Multiplayer's in-vm-run.ps1).
# - Done when SkyrimCoop.log is new and says "Game data loaded" and the tick ran; the log's last lines are shown.
param(
	[string]$Game = '',
	[string]$Cell = '',
	[int]$WaitSeconds = 240,
	[string]$ViaTask = '',
	[switch]$RestoreIni
)
$ErrorActionPreference = 'Stop'

$myGames = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition'
$skyrimIni = Join-Path $myGames 'Skyrim.ini'
$before = "$skyrimIni.skyrimcoop-before-start"
$log = Join-Path $myGames 'SKSE\SkyrimCoop.log'
$skseLog = Join-Path $myGames 'SKSE\skse64.log'

function Restore-Ini {
	if (Test-Path -LiteralPath $before) {
		Move-Item -LiteralPath $before -Destination $skyrimIni -Force
		'Skyrim.ini put back as it was before the start with -Cell'
	}
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
} else {
	Restore-Ini
}

$started = Get-Date
if ($ViaTask) {
	& $ViaTask -Name "SkyrimCoop-Game-$(Get-Date -Format HHmmss)" -Exe $loader
} else {
	Start-Process -FilePath $loader -WorkingDirectory $Game | Out-Null
	"started $loader"
}

$deadline = $started.AddSeconds($WaitSeconds)
$state = 'no game process yet'
while ((Get-Date) -lt $deadline) {
	Start-Sleep -Seconds 3
	$game = Get-Process SkyrimSE -ErrorAction SilentlyContinue
	if (-not $game) { $state = 'no SkyrimSE process'; continue }
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
if (Test-Path -LiteralPath $skseLog) { 'skse64.log, its last lines:'; Get-Content -LiteralPath $skseLog | Select-Object -Last 6 | ForEach-Object { "  $_" } }
if (Test-Path -LiteralPath $log) { 'SkyrimCoop.log, its last lines:'; Get-Content -LiteralPath $log | Select-Object -Last 15 | ForEach-Object { "  $_" } }
if ($state -ne 'up') { exit 1 }
