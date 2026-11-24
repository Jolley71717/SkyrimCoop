# questscan.ps1 [-Data <game Data folder>] [-Write [-Out <header>]]
# Reads Skyrim Special Edition's own files (Skyrim.esm, Update.esm, Dawnguard.esm, HearthFires.esm, Dragonborn.esm and the
# scripts in Skyrim - Misc.bsa, then the loose ones in Data\Scripts; nothing is written there) and lists every stage of a
# quest a session may share (a journal type other than misc, an editor ID) whose fragment moves the player, fades the
# screen out or takes their controls away, itself or through the scripts it calls and the stages it sets or the quests it
# starts (tools/questscan.cs, MoveScan), each with the chain it followed. With -Write: writes those stages as
# src/game/StageRows.h (or -Out), QuestRules::STAGE_RULES's rows (docs/DESIGN.md, "Stages that move the player"), and
# prints what it wrote. With -Dump <editor ID>: that quest's record and its stage fragments, disassembled. Windows PowerShell 5.1 compiles the reader (tools/questscan.cs) on the fly; the scan runs at idle
# priority and reads only the files' quest records and the scripts' archive.
param([string]$Data = 'F:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data', [switch]$Write, [string]$Dump,
	[string]$Out = (Join-Path $PSScriptRoot '..\src\game\StageRows.h'))
$ErrorActionPreference = 'Stop'
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'Idle'
Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'questscan.cs')) -ReferencedAssemblies System.Core -Language CSharp
if ($Dump) { [MoveScan]::Dump($Data, $Dump); return }
if ($Write) {
	$header = [MoveScan]::Header($Data)
	[IO.File]::WriteAllText([IO.Path]::GetFullPath($Out), $header)  # ASCII, CRLF
}
[MoveScan]::Run($Data)
