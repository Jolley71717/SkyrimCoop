# What Skyrim needs once on a machine before scripted starts, without the launcher and without clicks. Run on that
# machine (firststart.sh runs it on the host and, over ssh, on the VM). The game must be closed.
#
#   firststart.ps1 -Game <game folder> [-Preset Low|Medium|High|Ultra] [-Width 1280 -Height 720]
#   firststart.ps1 -Game <game folder> -Check      says what is set and what is missing; changes nothing
#   firststart.ps1 -Undo                            puts both ini files back as they were before the first run of this
#
# What it does, and why:
# - Skyrim.ini and SkyrimPrefs.ini (Documents\My Games\Skyrim Special Edition). On a game never started there are none.
#   SkyrimSELauncher.exe writes them on its first run: Skyrim.ini from the game's Skyrim_Default.ini and SkyrimPrefs.ini
#   from the chosen preset (Low.ini to Ultra.ini beside it), then the screen size. This does the same copy, so no
#   launcher window needs a click. An ini that is there already is backed up first (*.skyrimcoop-before) and only the
#   keys below are changed.
# - A window, not the full screen: SkyrimPrefs.ini [Display] bFull Screen=0, bBorderless=1, iSize W/H.
# - No intro video: Skyrim.ini [General] sIntroSequence= (empty).
# - Updates: Steam's own setting is not changed here (it is Steam's, and Steam rewrites its files). The check says
#   what steamapps\appmanifest_489830.acf holds: AutoUpdateBehavior 0 is "always keep updated", which can bring a game
#   version SKSE does not support yet; set "Only update this game when I launch it" in Steam (Properties, Updates).
# - The Microsoft C++ runtime SKSE and the plugin need (vcruntime140.dll, msvcp140.dll): checked, not installed. Steam
#   installs it the first time the game is started from Steam's Play button; a start through skse64_loader.exe skips that.
param(
	[string]$Game = '',
	[ValidateSet('Low', 'Medium', 'High', 'Ultra')] [string]$Preset = 'Medium',
	[int]$Width = 1280,
	[int]$Height = 720,
	[switch]$Check,
	[switch]$Undo
)
$ErrorActionPreference = 'Stop'

$myGames = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Skyrim Special Edition'
$skyrimIni = Join-Path $myGames 'Skyrim.ini'
$prefsIni = Join-Path $myGames 'SkyrimPrefs.ini'
$marker = '.skyrimcoop-before'    # the file as it was before the first run of this script
$created = '.skyrimcoop-created'  # this script made the file: undo moves it away

# The value of key in [section] of an ini's lines ($null when absent).
function Get-IniValue([string[]]$lines, [string]$section, [string]$key) {
	$in = $false
	foreach ($line in $lines) {
		if ($line -match '^\s*\[(.+)\]\s*$') { $in = ($Matches[1] -eq $section); continue }
		if ($in -and $line -match "^\s*$([regex]::Escape($key))\s*=(.*)$") { return $Matches[1].Trim() }
	}
	return $null
}

# The ini's lines with key=value set in [section]: the line replaced where it is, or added at the section's end (the
# section added at the file's end when missing).
function Set-IniValue([System.Collections.Generic.List[string]]$lines, [string]$section, [string]$key, [string]$value) {
	$in = $false; $sectionAt = -1; $lastInSection = -1
	for ($i = 0; $i -lt $lines.Count; $i++) {
		if ($lines[$i] -match '^\s*\[(.+)\]\s*$') {
			$in = ($Matches[1] -eq $section)
			if ($in) { $sectionAt = $i; $lastInSection = $i }
			continue
		}
		if ($in) {
			if ($lines[$i].Trim()) { $lastInSection = $i }
			if ($lines[$i] -match "^\s*$([regex]::Escape($key))\s*=") { $lines[$i] = "$key=$value"; return }
		}
	}
	if ($sectionAt -lt 0) { $lines.Add("[$section]"); $lines.Add("$key=$value"); return }
	$lines.Insert($lastInSection + 1, "$key=$value")
}

function Edit-Ini([string]$path, [scriptblock]$edit) {
	$lines = [System.Collections.Generic.List[string]]::new()
	foreach ($line in [IO.File]::ReadAllLines($path)) { $lines.Add($line) }
	& $edit $lines
	# the game's own files are plain ANSI text with CRLF line ends: written back the same way, no byte order mark
	[IO.File]::WriteAllLines($path, $lines, [Text.Encoding]::Default)
}

if ($Undo) {
	foreach ($ini in $skyrimIni, $prefsIni) {
		if (Test-Path -LiteralPath "$ini$marker") {
			Move-Item -LiteralPath "$ini$marker" -Destination $ini -Force
			"put back: $ini"
		} elseif (Test-Path -LiteralPath "$ini$created") {
			$away = "$ini.skyrimcoop-removed-$(Get-Date -Format yyyyMMdd-HHmmss)"
			if (Test-Path -LiteralPath $ini) { Move-Item -LiteralPath $ini -Destination $away }
			Move-Item -LiteralPath "$ini$created" -Destination "$away.marker"
			"moved away (this script had made it): $ini -> $away"
		} else {
			"nothing to undo: $ini"
		}
	}
	exit 0
}

if (-not $Game -or -not (Test-Path -LiteralPath (Join-Path $Game 'SkyrimSE.exe'))) { throw "No SkyrimSE.exe in '$Game' (pass -Game)" }

$problems = @()
$facts = @()
foreach ($dll in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') {
	if (-not (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\$dll"))) { $problems += "the Microsoft C++ runtime lacks $dll (install the x64 Visual C++ 2015-2022 redistributable, or start the game once from Steam's Play button)" }
}
$manifest = Join-Path (Split-Path -Parent (Split-Path -Parent $Game)) 'appmanifest_489830.acf'
if (Test-Path -LiteralPath $manifest) {
	$auto = Select-String -LiteralPath $manifest -Pattern '"AutoUpdateBehavior"\s+"(\d+)"' | Select-Object -First 1
	$behaviour = if ($auto) { $auto.Matches[0].Groups[1].Value } else { '?' }
	$facts += "Steam's update setting for the game: AutoUpdateBehavior $behaviour (0 = always keep updated, 1 = only when launched)"
	if ($behaviour -eq '0') { $problems += 'Steam keeps the game always updated: an update can arrive before SKSE supports it (Steam: Properties, Updates, "Only update this game when I launch it")' }
} else {
	$problems += "no Steam manifest at $manifest (is the game installed through Steam?)"
}
foreach ($f in 'skse64_loader.exe', 'skse64_1_7_104.dll', 'Data\SKSE\Plugins\versionlib-1-7-104-0.bin', 'Data\SKSE\Plugins\SkyrimCoop.dll') {
	$facts += "$f $(if (Test-Path -LiteralPath (Join-Path $Game $f)) { 'there' } else { 'not there (install.sh)' })"
}
if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { $problems += 'Skyrim is running' }

if ($Check) {
	foreach ($ini in $skyrimIni, $prefsIni) {
		if (-not (Test-Path -LiteralPath $ini)) { $problems += "no $ini yet"; continue }
		$lines = [IO.File]::ReadAllLines($ini)
		if ($ini -eq $prefsIni) {
			foreach ($pair in @('bFull Screen', '0'), @('bBorderless', '1')) {
				$have = Get-IniValue $lines 'Display' $pair[0]
				if ($have -ne $pair[1]) { $problems += "SkyrimPrefs.ini [Display] $($pair[0]) is '$have', not $($pair[1])" }
			}
			$facts += "window $(Get-IniValue $lines 'Display' 'iSize W')x$(Get-IniValue $lines 'Display' 'iSize H')"
		} else {
			$intro = Get-IniValue $lines 'General' 'sIntroSequence'
			if ($null -eq $intro -or $intro -ne '') { $problems += "Skyrim.ini [General] sIntroSequence is '$intro', not empty (the intro video plays)" }
			$facts += "sStartingConsoleCommand '$(Get-IniValue $lines 'General' 'sStartingConsoleCommand')'"
		}
	}
	$facts | ForEach-Object { "  $_" }
	if ($problems) { $problems | ForEach-Object { "PROBLEM: $_" }; exit 1 }
	'first start: everything is set'
	exit 0
}

if (Get-Process SkyrimSE -ErrorAction SilentlyContinue) { throw 'Skyrim is running: close it first' }
New-Item -ItemType Directory -Force $myGames | Out-Null
$sources = @{ $skyrimIni = (Join-Path $Game 'Skyrim_Default.ini'); $prefsIni = (Join-Path $Game "$Preset.ini") }
foreach ($ini in $skyrimIni, $prefsIni) {
	if (Test-Path -LiteralPath $ini) {
		if (-not (Test-Path -LiteralPath "$ini$marker") -and -not (Test-Path -LiteralPath "$ini$created")) { Copy-Item -LiteralPath $ini -Destination "$ini$marker" }
		"kept, keys changed: $ini"
	} else {
		if (-not (Test-Path -LiteralPath $sources[$ini])) { throw "the game folder lacks $($sources[$ini])" }
		Copy-Item -LiteralPath $sources[$ini] -Destination $ini
		Set-Content -LiteralPath "$ini$created" -Value "made by tools/rig/firststart.ps1 $(Get-Date -Format s) from $($sources[$ini])"
		"made from $(Split-Path -Leaf $sources[$ini]): $ini"
	}
}
Edit-Ini $prefsIni {
	param($lines)
	Set-IniValue $lines 'Display' 'bFull Screen' '0'
	Set-IniValue $lines 'Display' 'bBorderless' '1'
	Set-IniValue $lines 'Display' 'iSize W' "$Width"
	Set-IniValue $lines 'Display' 'iSize H' "$Height"
}
Edit-Ini $skyrimIni {
	param($lines)
	Set-IniValue $lines 'General' 'sIntroSequence' ''
}
"set: a ${Width}x$Height window, no intro video"
$facts | ForEach-Object { "  $_" }
$problems | ForEach-Object { "PROBLEM: $_" }
