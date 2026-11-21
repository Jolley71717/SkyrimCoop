# Says whether a game's own Steam library (steam_api64.dll) has the functions the plugin calls.
#
#   tools\steamcheck.ps1 -Path "<Steam library>\steamapps\common\Skyrim Special Edition"
#
# The plugin never ships a Steam library: it calls the one the game loaded (src\steam\SteamApi.cpp), so
# which functions that file exports decides whether lobbies, invites and the relay can work at all. This
# script reads the file's export table and compares it with the names SteamApi.cpp asks for. It only reads
# the file: the game does not have to run, and nothing is loaded or changed.
#
# What it cannot say: whether Steam lets this game's app id use lobbies and the relay. That takes two games
# (docs\GETTING-STARTED.md, "The first thing to test").
param(
	[string]$Path = ''  # the game's folder, or the steam_api64.dll itself
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not $Path) { $Path = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition' }
if (Test-Path -LiteralPath $Path -PathType Container) { $Path = Join-Path $Path 'steam_api64.dll' }
if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "No steam_api64.dll at $Path (pass the game's folder with -Path)" }

# The names the plugin resolves: "ok &= Resolve(...)" lines are needed, the other Resolve lines are extras
# (voice, the friends list, achievements, diagnostics) the plugin does without.
$source = Join-Path $root 'src\steam\SteamApi.cpp'
$needed = @()
$extras = @()
foreach ($line in Get-Content -LiteralPath $source) {
	$names = @([regex]::Matches($line, 'Resolve\(module, "([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
	if (-not $names) { continue }
	if ($line -match '^\s*ok &= Resolve') { $needed += $names } else { $extras += $names }
}
if ($needed.Count -lt 10) { throw "Found only $($needed.Count) needed names in ${source}: has the way it resolves them changed?" }

# The export table of a 64-bit PE file, read from its bytes.
function Get-PeExports([string]$file) {
	$bytes = [IO.File]::ReadAllBytes($file)
	if ($bytes.Length -lt 0x100 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "$file is not a Windows program file" }
	$pe = [BitConverter]::ToInt32($bytes, 0x3C)
	if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) { throw "$file has no PE header" }
	$sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
	$optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
	$optional = $pe + 24
	$magic = [BitConverter]::ToUInt16($bytes, $optional)
	if ($magic -ne 0x20B) { throw "$file is not a 64-bit library (the game's is steam_api64.dll)" }
	$exportRva = [BitConverter]::ToUInt32($bytes, $optional + 112)
	if (-not $exportRva) { return @() }

	$sections = for ($i = 0; $i -lt $sectionCount; $i++) {
		$at = $optional + $optionalSize + 40 * $i
		[pscustomobject]@{
			Size    = [Math]::Max([BitConverter]::ToUInt32($bytes, $at + 8), [BitConverter]::ToUInt32($bytes, $at + 16))
			Address = [BitConverter]::ToUInt32($bytes, $at + 12)
			Raw     = [BitConverter]::ToUInt32($bytes, $at + 20)
		}
	}
	$offsetOf = {
		param([uint32]$rva)
		foreach ($section in $sections) {
			if ($rva -ge $section.Address -and $rva -lt $section.Address + $section.Size) { return [int]($rva - $section.Address + $section.Raw) }
		}
		throw "${file}: an address in its export table points outside every section"
	}

	$table = & $offsetOf $exportRva
	$nameCount = [BitConverter]::ToUInt32($bytes, $table + 24)
	$nameList = & $offsetOf ([BitConverter]::ToUInt32($bytes, $table + 32))
	for ($i = 0; $i -lt $nameCount; $i++) {
		$start = & $offsetOf ([BitConverter]::ToUInt32($bytes, $nameList + 4 * $i))
		$end = [Array]::IndexOf($bytes, [byte]0, $start)
		[Text.Encoding]::ASCII.GetString($bytes, $start, $end - $start)
	}
}

$exports = @(Get-PeExports $Path)
$has = @{}
foreach ($name in $exports) { $has[$name] = $true }

$info = (Get-Item -LiteralPath $Path).VersionInfo
"steam_api64.dll: $($exports.Count) exported functions, file version $($info.FileVersion)"

$missing = @($needed | Where-Object { -not $has[$_] })
$missingExtras = @($extras | Where-Object { -not $has[$_] })
"Needed by the plugin: $($needed.Count - $missing.Count) of $($needed.Count) present"
foreach ($name in $missing) { "  missing: $name" }
"Extras (the plugin runs without them): $($extras.Count - $missingExtras.Count) of $($extras.Count) present"
foreach ($name in $missingExtras) { "  missing: $name" }

# What a missing accessor could be replaced with: the versions this file does have, and the older ways in.
if ($missing -or $missingExtras) {
	''
	'Interface accessors this file has (another version of a missing one may do, with its layout checked):'
	$accessors = @($exports | Where-Object { $_ -match '^SteamAPI_Steam(User|Friends|Matchmaking|NetworkingMessages|NetworkingUtils|NetworkingSockets|Networking|UserStats|Utils)(_SteamAPI)?_v\d+$' } | Sort-Object)
	if ($accessors) { $accessors | ForEach-Object { "  $_" } } else { '  none (a Steam library from before the versioned accessors)' }
	'Other ways to reach an interface:'
	foreach ($name in 'SteamInternal_FindOrCreateUserInterface', 'SteamInternal_CreateInterface', 'SteamClient', 'SteamAPI_ISteamClient_GetISteamGenericInterface') {
		"  $name" + $(if ($has[$name]) { ': present' } else { ': missing' })
	}
}

''
if ($missing) {
	"RESULT: this Steam library lacks $($missing.Count) function(s) the plugin needs. docs\GETTING-STARTED.md says what to try next."
	exit 1
}
'RESULT: every function the plugin needs is here. Whether Steam lets this game use lobbies and the relay is the next test (docs\GETTING-STARTED.md).'
