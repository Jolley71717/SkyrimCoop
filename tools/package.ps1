# Builds a release from a committed tree, runs the unit tests and packages it for players.
#
# Produces dist\out\SkyrimCoop-<version>[-<label>].zip, laid out like a Data folder so a mod manager can
# install it directly, plus the standalone server. Beside it go a manifest (the commit, the build label
# and the DLL's SHA-256, which tools\promote.ps1 checks) and, under symbols\, the .pdb for reading crash
# logs from exactly this build. docs\RELEASING.md has the order: package, then tag the commit the
# manifest names, then publish.
param(
	[string]$Mode = 'releasedbg',
	[string]$Label = '',  # "rc3" for a release candidate: named in the zip, and with the commit in the plugin log
	[switch]$Nexus,  # the Nexus upload: without SkyrimCoopServer.exe (only dedicated servers need it, and it is one more compiled file for their scanner); the GitHub release carries the full zip
	[string]$OutDir = '',  # where the zip goes (dist\out); a scratch folder for trying the script out
	[switch]$AllowDirty  # local experiments only: packages uncommitted changes, the build calls itself "-dirty" and promote.ps1 refuses it
)

$ErrorActionPreference = 'Stop'

# A dirty build packaged under an rc's name in dist\out would replace that rc's zip and manifest, the
# ones its soak verdict was given for.
if ($AllowDirty -and $Label -and -not $OutDir) { throw "-AllowDirty with -Label needs -OutDir (a scratch folder), so it can't replace a real rc's package" }

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
. (Join-Path $PSScriptRoot 'release-lib.ps1')

$env:Path = "C:\Program Files\xmake;C:\Program Files\Git\cmd;" + $env:Path
if (-not $OutDir) { $OutDir = Join-Path $root 'dist\out' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath

# The label goes into a C++ string literal and into file names.
if ($Label -notmatch '^[A-Za-z0-9.-]*$') { throw "Label '$Label' may hold only letters, digits, '.' and '-'" }
$version = (Select-String -Path xmake.lua -Pattern 'set_version\("([^"]+)"\)').Matches[0].Groups[1].Value
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "xmake.lua's set_version is '$version', not a version like 0.1.0" }

# What is packaged is a commit: no changes to tracked files, and an rc's notes are part of it.
$changed = @(git status --porcelain --untracked-files=no)
Assert-Native 'git status'
$untracked = @(git status --porcelain | Where-Object { $_.StartsWith('?? ') } | ForEach-Object { $_.Substring(3).Trim('"') } |
	Where-Object { -not $_.StartsWith('.claude/') -and -not $_.StartsWith('dist/out/') })
# xmake.lua compiles every source file under these folders by pattern, so an untracked one would be
# built into the package although the commit doesn't have it.
$compiled = @($untracked | Where-Object { $_ -match '^(src|common|server|bot|tests)/' })
$untracked = @($untracked | Where-Object { $compiled -notcontains $_ })
if ($untracked) { Write-Warning "Untracked files (not packaged, but check they belong nowhere): $($untracked -join ', ')" }
$problems = @()
if ($changed) { $problems += "uncommitted changes:`n  $($changed -join "`n  ")" }
if ($compiled) { $problems += "untracked files that would be compiled in:`n  $($compiled -join "`n  ")" }
if ($Label) {
	$notes = "dist/RELEASE-$version-$Label.md"
	if (-not (git ls-files -- $notes)) { $problems += "$notes is not committed (the rc's commit carries its notes)" }
}
# set_version is bumped by hand, so notes for a newer version mean it was missed. A release's own notes
# are written alongside its rcs, so they don't count against one.
$packaged = $version
if ($Label -match '^rc\d+$') { $packaged = "$version-$Label" }
$newer = @(Get-NewerReleaseNotes (Join-Path $root 'dist') $packaged | Where-Object { $_ -ne "RELEASE-$version.md" })
if ($newer) { $problems += "dist has notes for a newer version than $packaged ($($newer -join ', ')): bump set_version in xmake.lua, or check -Label" }
if ($problems) {
	if (-not $AllowDirty) { throw "Not packaging: $($problems -join '; ')" }
	Write-Warning "Packaging anyway (-AllowDirty): $($problems -join '; ')"
}

# The plugin's first log lines name the build: "SkyrimCoop build rc3 1a2b3c4".
$commit = (git rev-parse HEAD).Trim()
Assert-Native 'git rev-parse'
$sha = $commit.Substring(0, 7)
if ($problems) { $sha += '-dirty' }
$env:SKYRIMCOOP_LABEL = "$Label $sha".Trim()

# -P .: in a worktree nested inside another checkout, xmake would otherwise build the outer one.
# -p windows -a x64: the folder the build is read from below, whatever xmake would pick by itself (a
# shell from Git for Windows makes it guess another platform) or the worktree was configured with.
# --vs=2022: the Visual Studio CONTRIBUTING.md names, not whichever newer one is also installed. xmake
# doesn't refuse one that is missing (it configures with another), so it is looked for first.
# --skyrim_vr=n --skse_patch_safety=n: the library's options this project builds without (xmake.lua).
# Its output is kept for a failure; Windows PowerShell 5.1 would stop at xmake's first line on stderr.
if (-not (Find-VisualStudio '[17.0,18.0)')) {
	throw ("Visual Studio 2022 with its C++ tools is not installed. Install Visual Studio 2022 (or its Build Tools) " +
		"with the 'Desktop development with C++' workload; releases are built with that one.")
}
$preference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
	$configOutput = @(xmake config -P . -p windows -a x64 --vs=2022 -m $Mode --skyrim_vr=n --skse_patch_safety=n -y 2>&1 | ForEach-Object { "$_" })
	$configFailed = $LASTEXITCODE
} finally {
	$ErrorActionPreference = $preference
}
if ($configFailed) {
	$configOutput
	if ($configOutput -match 'cannot get program for') {
		throw ("xmake config found no C++ compiler although Visual Studio 2022 is installed. If it was just updated, " +
			"clear xmake's remembered toolchain: xmake config -c -P . -p windows -a x64 --vs=2022 -m $Mode")
	}
	throw "xmake config failed (exit code $configFailed)"
}
xmake build -P . -y  # every default target, the unit tests included
Assert-Native 'xmake build'

$build = Join-Path $root "build\windows\x64\$Mode"
# With a time limit: a server test that deadlocks would otherwise hang packaging for good.
$testLimitMinutes = 25
$testLog = [IO.Path]::GetTempFileName()
$tests = Start-Process -FilePath (Join-Path $build 'SkyrimCoopTests.exe') -WorkingDirectory $root -NoNewWindow -PassThru -RedirectStandardOutput $testLog
$null = $tests.Handle  # Windows PowerShell 5.1 loses the exit code of a process whose handle was never read
if (-not $tests.WaitForExit($testLimitMinutes * 60 * 1000)) {
	$tests.Kill()
	$tests.WaitForExit()
	# The last lines say how far it got: the test after the last one listed is the one that was running.
	$done = @(Get-Content -LiteralPath $testLog)
	"SkyrimCoopTests: $($done.Count) lines of output before the limit; the last of them:"
	$done | Select-Object -Last 15
	throw "SkyrimCoopTests did not finish within $testLimitMinutes minutes (a test is stuck or the machine is slow; its output so far is in $testLog)"
}
$testsFailed = $tests.ExitCode
Get-Content -LiteralPath $testLog | Where-Object { -not $_.StartsWith('ok ') }
Remove-Item -LiteralPath $testLog
if ($testsFailed) { throw "SkyrimCoopTests: $testsFailed test(s) failed" }

$suffix = ''
if ($Label) { $suffix += "-$Label" }
if ($Nexus) { $suffix += '-nexus' }
$name = "SkyrimCoop-$version$suffix"
$zip = Join-Path $OutDir "$name.zip"
$stage = Join-Path $OutDir 'stage'

# The zip is a Data folder: SKSE\Plugins for the plugin and its settings, and a folder of ours for what
# a player reads (and the dedicated server, which a game never loads).
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'SKSE\Plugins') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'SkyrimCoop') | Out-Null

Copy-Item (Join-Path $build 'SkyrimCoop.dll') (Join-Path $stage 'SKSE\Plugins')
Copy-Item (Join-Path $root 'dist\SkyrimCoop.ini') (Join-Path $stage 'SKSE\Plugins')
if (-not $Nexus) { Copy-Item (Join-Path $build 'SkyrimCoopServer.exe') (Join-Path $stage 'SkyrimCoop') }
Copy-Item (Join-Path $root 'dist\INSTALL.md') (Join-Path $stage 'SkyrimCoop')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'SkyrimCoop')
Copy-Item (Join-Path $root 'EXCEPTIONS') (Join-Path $stage 'SkyrimCoop')

# The mod's texts in every language (Text.cpp reads Data\SKSE\Plugins\SkyrimCoop\lang\<code>.txt).
$lang = Join-Path $stage 'SKSE\Plugins\SkyrimCoop\lang'
New-Item -ItemType Directory -Force $lang | Out-Null
$langFiles = @(Get-ChildItem (Join-Path $root 'dist\lang') -Filter '*.txt' -File)
if (-not ($langFiles | Where-Object { $_.Name -eq 'en.txt' })) { throw 'dist\lang has no en.txt' }
Copy-Item $langFiles.FullName $lang

# Nothing in the zip may name this PC or its user.
Assert-LeakScanWorks
$patterns = @(Get-LeakPatterns)
$leaks = foreach ($file in Get-ChildItem $stage -Recurse -File) {
	$found = @(Find-Leaks ([IO.File]::ReadAllBytes($file.FullName)) $patterns)
	if ($found) { "$($file.Name) ($($found.Count) pattern(s))" }
}
if ($leaks) {
	Remove-Item -Recurse -Force $stage
	throw "Build paths or this PC's names leaked into: $($leaks -join ', ')"
}

New-Zip $stage $zip
$dllHash = Get-Sha256 (Join-Path $stage 'SKSE\Plugins\SkyrimCoop.dll')
Remove-Item -Recurse -Force $stage

# The .pdb stays out of the zip (it is full of build paths, so it is never uploaded anywhere); it is
# kept here, named for this exact build, for reading crash logs from it.
$symbols = Join-Path $OutDir 'symbols'
New-Item -ItemType Directory -Force $symbols | Out-Null
$pdb = "$name-$sha.pdb"
Copy-Item (Join-Path $build 'SkyrimCoop.pdb') (Join-Path $symbols $pdb)

$enet = (Select-String -Path xmake.lua -Pattern 'add_requires\("enet ([^"]+)"\)').Matches[0].Groups[1].Value
$library = (git -C (Join-Path $root 'lib\commonlibsse-ng') rev-parse HEAD).Trim()
Assert-Native 'git rev-parse (the library)'
$manifest = Join-Path $OutDir "$name.manifest.txt"
@(
	"version=$version"
	"label=$env:SKYRIMCOOP_LABEL"
	"commit=$commit"
	"dirty=$([bool]$problems)".ToLowerInvariant()
	"zip=$name.zip"
	"zip_sha256=$(Get-Sha256 $zip)"
	"dll_sha256=$dllHash"
	"pdb=symbols/$pdb"
	"enet=$enet"
	"commonlibsse_ng=$library"
) | Set-Content -LiteralPath $manifest -Encoding ASCII

"Packaged $zip"
"  build $env:SKYRIMCOOP_LABEL, DLL sha256 $dllHash"
"  manifest $manifest"
if ($Label -and -not $problems) { "Next: git tag v$version-$Label $commit, then the release (docs\RELEASING.md)." }
