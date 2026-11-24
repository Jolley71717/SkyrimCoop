# questscan.ps1 [-Data <game Data folder>] [-Write [-Out <header>]] [-Audit] [-Order] [-Choices] [-Dump <editor ID>]
# Reads Skyrim Special Edition's own files (Skyrim.esm, Update.esm, Dawnguard.esm, HearthFires.esm, Dragonborn.esm and the
# scripts in Skyrim - Misc.bsa, then the loose ones in Data\Scripts; nothing is written there) and lists every stage of a
# quest a session may share (a journal type other than misc, an editor ID) whose fragment moves the player, fades the
# screen out or takes their controls away, itself or through the scripts it calls and the stages it sets or the quests it
# starts (tools/questscan.cs, MoveScan), each with the chain it followed. With -Write: writes those stages as
# src/game/StageRows.h (or -Out), QuestRules::STAGE_RULES's rows (docs/DESIGN.md, "Stages that move the player"), and
# prints what it wrote. With -Dump <editor ID>: that quest's record and its stage fragments, disassembled (or, for an image space, whether it plays out or stays on). Windows PowerShell 5.1 compiles the reader (tools/questscan.cs) on the fly; the scan runs at idle
# priority and reads only the files' quest and image space records and the scripts' archive.
# With -Audit: writes every quest of the masters, judged as the game judges it (shared, shared with limits, not shared,
# with the reason), as src/game/QuestAudit.h (QuestRules::QUEST_AUDIT, which the code reads) and docs/QUEST-AUDIT.md, and
# prints the counts. With -Order: every quest a session may share whose stage fragment sets a lower stage of its own
# quest (the rules take a higher stage for a later one), and those whose record lets a stage be set again. With -Choices:
# every quest a session may share whose script property a dialogue line's or a scene's fragment writes (those run only
# in one game) and whether the quest reads it again, and the globals such fragments set.
param([string]$Data = 'F:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data', [switch]$Write, [switch]$Audit, [switch]$Order, [switch]$Choices, [string]$Dump,
	[string]$Out = (Join-Path $PSScriptRoot '..\src\game\StageRows.h'))
$ErrorActionPreference = 'Stop'
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'Idle'
Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'questscan.cs')) -ReferencedAssemblies System.Core -Language CSharp
if ($Dump) { [MoveScan]::Dump($Data, $Dump); return }
if ($Order) { [MoveScan]::Order($Data); return }
if ($Choices) { [MoveScan]::Choices($Data); return }
if ($Audit) {
	$all = [MoveScan]::Audit($Data)
	[IO.File]::WriteAllText([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\src\game\QuestAudit.h')), [MoveScan]::AuditHeader($all))
	[IO.File]::WriteAllText([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\docs\QUEST-AUDIT.md')), [MoveScan]::AuditDoc($all))
	$all | Group-Object Verdict, Why | Sort-Object Name | ForEach-Object { '{0,-24} {1}' -f $_.Name, $_.Count }
	return
}
if ($Write) {
	$header = [MoveScan]::Header($Data)
	[IO.File]::WriteAllText([IO.Path]::GetFullPath($Out), $header)  # ASCII, CRLF
}
[MoveScan]::Run($Data)
