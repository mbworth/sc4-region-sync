<#
.SYNOPSIS
    Prepares and launches N SimCity 4 instances that share one Regions folder.

.DESCRIPTION
    SimCity 4 keeps everything user-specific under one "user dir"
    (Documents\SimCity 4 by default): Regions, Plugins, Albums, preferences.
    Two instances pointed at the same user dir will fight over preferences
    and the region thumbnail cache, so this script gives each instance its
    own user dir with Regions and Plugins as directory junctions back to
    the real ones. Each instance then sees the same region, the same city
    save files and the same plugins (including SC4RegionSync.dll and its
    store), while preferences and Albums stay separate. The plugin writes
    its log into the instance's user dir when -UserDir is set.

    Run once with -Setup to create the per-instance user dirs, then run
    with -Launch to start the instances.

.PARAMETER GameExe
    Full path to SimCity 4.exe (the 641 digital release).

.PARAMETER SourceUserDir
    Your real SimCity 4 user dir. Defaults to Documents\SimCity 4.

.PARAMETER Instances
    How many instances to prepare or launch. Default 2.

.PARAMETER Setup
    Create the per-instance user dirs and junctions.

.PARAMETER Launch
    Launch the instances.

.PARAMETER ExtraArgs
    Additional command-line arguments passed to every instance, for example
    "-CustomResolution:enabled -r1920x1080x32 -w".

.EXAMPLE
    .\Setup-MultiInstance.ps1 -GameExe "C:\GOG Games\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe" -Setup
    .\Setup-MultiInstance.ps1 -GameExe "C:\GOG Games\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe" -Launch -ExtraArgs "-w -CustomResolution:enabled -r1600x900x32"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameExe,

    [string]$SourceUserDir = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'SimCity 4'),

    [int]$Instances = 2,

    [switch]$Setup,
    [switch]$Launch,

    [string]$ExtraArgs = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $GameExe)) {
    throw "Game executable not found: $GameExe"
}
if (-not (Test-Path $SourceUserDir)) {
    throw "Source user dir not found: $SourceUserDir"
}

$instanceRoot = Join-Path (Split-Path $SourceUserDir -Parent) 'SimCity 4 Instances'
$sharedRegions = Join-Path $SourceUserDir 'Regions'

function Get-InstanceDir([int]$i) {
    return Join-Path $instanceRoot ("Instance{0}" -f $i)
}

if ($Setup) {
    New-Item -ItemType Directory -Force -Path $instanceRoot | Out-Null

    for ($i = 1; $i -le $Instances; $i++) {
        $dir = Get-InstanceDir $i
        New-Item -ItemType Directory -Force -Path $dir | Out-Null

        # Albums is private and starts empty.
        New-Item -ItemType Directory -Force -Path (Join-Path $dir 'Albums') | Out-Null

        # Regions and Plugins are shared through junctions. Plugins is
        # read-only to the game and often NAM-sized, so copying it is slow
        # and trips the 260-char path limit. Sharing it also means both
        # instances load the same SC4RegionSync.dll and share its store.
        foreach ($sub in @('Regions', 'Plugins')) {
            $src = Join-Path $SourceUserDir $sub
            $dst = Join-Path $dir $sub
            if (-not (Test-Path $src)) { continue }
            if (Test-Path $dst) {
                $item = Get-Item $dst -Force
                if (-not $item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
                    throw "$dst exists and is a real folder (leftover from an earlier run?). Delete it and rerun."
                }
            } else {
                Write-Host "Linking $dst -> $src"
                cmd /c mklink /J "`"$dst`"" "`"$src`"" | Out-Null
            }
        }

        # Copy preferences so each instance starts from your settings.
        $prefs = Join-Path $SourceUserDir 'SimCity 4.cfg'
        if (Test-Path $prefs) {
            Copy-Item -Force $prefs (Join-Path $dir 'SimCity 4.cfg')
        }
    }

    Write-Host "Setup complete. Instance dirs under: $instanceRoot"
}

if ($Launch) {
    for ($i = 1; $i -le $Instances; $i++) {
        $dir = Get-InstanceDir $i
        if (-not (Test-Path $dir)) {
            throw "Instance dir missing, run with -Setup first: $dir"
        }

        # -UserDir must end with a trailing backslash. Start-Process re-quotes
        # its argument string (it wrapped the whole thing in one pair of
        # quotes and dropped ours), so hand the raw string to .NET instead.
        $userDirArg = '-UserDir:"' + $dir.TrimEnd('\') + '\"'
        $argList = "$userDirArg $ExtraArgs".Trim()

        Write-Host "Launching instance $i : `"$GameExe`" $argList"
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $GameExe
        $psi.Arguments = $argList
        $psi.WorkingDirectory = Split-Path $GameExe -Parent
        $psi.UseShellExecute = $false
        [System.Diagnostics.Process]::Start($psi) | Out-Null

        # Give the first instance a head start so both do not hammer the disk
        # on the same startup files at once.
        Start-Sleep -Seconds 8
    }
}

if (-not $Setup -and -not $Launch) {
    Write-Host "Nothing to do. Pass -Setup and/or -Launch."
}
