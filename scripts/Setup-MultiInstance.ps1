<#
.SYNOPSIS
    Prepares and launches N SimCity 4 instances that share one Regions folder.

.DESCRIPTION
    SimCity 4 keeps everything user-specific under one "user dir"
    (Documents\SimCity 4 by default): Regions, Plugins, Albums, preferences.
    Two instances pointed at the same user dir will fight over preferences
    and the region thumbnail cache, so this script gives each instance its
    own user dir and makes the Regions folder a directory junction back to
    the real one. Each instance then sees the same region and the same city
    save files, while everything else stays separate.

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
$sharedStore = Join-Path $instanceRoot 'RegionSync'

function Get-InstanceDir([int]$i) {
    return Join-Path $instanceRoot ("Instance{0}" -f $i)
}

if ($Setup) {
    New-Item -ItemType Directory -Force -Path $instanceRoot | Out-Null

    for ($i = 1; $i -le $Instances; $i++) {
        $dir = Get-InstanceDir $i
        New-Item -ItemType Directory -Force -Path $dir | Out-Null

        # Everything except Regions is a private copy (or empty).
        foreach ($sub in @('Albums', 'Plugins', 'Downloads')) {
            $src = Join-Path $SourceUserDir $sub
            $dst = Join-Path $dir $sub
            if ((Test-Path $src) -and -not (Test-Path $dst)) {
                Write-Host "Copying $sub into $dst"
                Copy-Item -Recurse -Force $src $dst
            } elseif (-not (Test-Path $dst)) {
                New-Item -ItemType Directory -Force -Path $dst | Out-Null
            }
        }

        # Regions is shared through a junction.
        $regionsLink = Join-Path $dir 'Regions'
        if (-not (Test-Path $regionsLink)) {
            Write-Host "Linking $regionsLink -> $sharedRegions"
            cmd /c mklink /J "`"$regionsLink`"" "`"$sharedRegions`"" | Out-Null
        }

        # Copy preferences so each instance starts from your settings.
        $prefs = Join-Path $SourceUserDir 'SimCity 4.cfg'
        if (Test-Path $prefs) {
            Copy-Item -Force $prefs (Join-Path $dir 'SimCity 4.cfg')
        }

        # If SC4RegionSync.dll lives in the (now private) Plugins copy, its
        # default store would be private too and the instances would never
        # see each other. Point every instance at one shared store.
        $ini = Join-Path $dir 'Plugins\SC4RegionSync.ini'
        if (Test-Path (Join-Path $dir 'Plugins\SC4RegionSync.dll')) {
            if (Test-Path $ini) {
                $text = Get-Content -Raw $ini
                $text = $text -replace '(?m)^StoreDirectory=.*$', "StoreDirectory=$sharedStore"
            } else {
                $text = "[Sync]`r`nStoreDirectory=$sharedStore`r`n"
            }
            Set-Content -Path $ini -Value $text -NoNewline
            Write-Host "Set StoreDirectory=$sharedStore in $ini"
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

        # -UserDir must end with a trailing backslash.
        $userDirArg = '-UserDir:"' + $dir.TrimEnd('\') + '\"'
        $argList = "$userDirArg $ExtraArgs".Trim()

        Write-Host "Launching instance $i : `"$GameExe`" $argList"
        Start-Process -FilePath $GameExe -ArgumentList $argList -WorkingDirectory (Split-Path $GameExe -Parent)

        # Give the first instance a head start so both do not hammer the disk
        # on the same startup files at once.
        Start-Sleep -Seconds 8
    }
}

if (-not $Setup -and -not $Launch) {
    Write-Host "Nothing to do. Pass -Setup and/or -Launch."
}
