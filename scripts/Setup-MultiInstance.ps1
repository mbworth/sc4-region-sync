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

.PARAMETER LoopbackGuest
    Test-only, one Windows box. Turns InstanceN into a stand-in for a second
    PC: its Regions junction is removed (non-recursively) and replaced by an
    empty real folder, and InstanceN\SC4RegionSync.local.ini gets a HostAlias
    (<PC>-GUEST, or LOOPBACK-GUEST when that is not 1 to 15 of A-Z a-z 0-9 -)
    and its own StoreDirectory. The plugin reads that file only when the instance runs
    with -UserDir (the scheduled tasks and -Launch do). Prints the lines the
    guest launcher's launcher.ini needs ([Launcher] StatusDirectory, [Lan]
    HostAlias, RegionsDirectory, AllowLoopback=1; the host launcher needs
    AllowLoopback=1 too). Does not need -GameExe.

.PARAMETER RestoreLoopback
    Undo -LoopbackGuest N. The real InstanceN\Regions folder is moved (never
    deleted) to <SourceUserDir>\RegionSyncLAN\archive\loopback-guest-<stamp>,
    the junction is recreated and the local ini is moved there too.

.PARAMETER ExtraArgs
    Additional command-line arguments passed to every instance, for example
    "-CustomResolution:enabled -r1920x1080x32 -w".

.EXAMPLE
    .\Setup-MultiInstance.ps1 -LoopbackGuest 2
    .\Setup-MultiInstance.ps1 -RestoreLoopback -LoopbackGuest 2

.EXAMPLE
    .\Setup-MultiInstance.ps1 -GameExe "C:\GOG Games\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe" -Setup
    .\Setup-MultiInstance.ps1 -GameExe "C:\GOG Games\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe" -Launch -ExtraArgs "-w -CustomResolution:enabled -r1600x900x32"
#>
[CmdletBinding()]
param(
    [string]$GameExe,

    [string]$SourceUserDir = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'SimCity 4'),

    [int]$Instances = 2,

    [switch]$Setup,
    [switch]$Launch,

    [int]$LoopbackGuest = 0,
    [switch]$RestoreLoopback,

    [string]$ExtraArgs = ''
)

$ErrorActionPreference = 'Stop'

if (($Setup -or $Launch) -and -not (Test-Path $GameExe)) {
    throw "Game executable not found (pass -GameExe): $GameExe"
}
if ($RestoreLoopback -and $LoopbackGuest -lt 1) {
    throw "-RestoreLoopback needs -LoopbackGuest N."
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

    # Steam build: the exe relaunches itself through Steam, and Steam refuses
    # a second copy of a running app. steam_appid.txt next to the exe stops
    # the relaunch (Steam must still be running). 24780 = SimCity 4 Deluxe.
    if ($GameExe -match 'steamapps') {
        $appId = Join-Path (Split-Path $GameExe -Parent) 'steam_appid.txt'
        if (-not (Test-Path $appId)) {
            try {
                Set-Content -Path $appId -Value '24780' -NoNewline
                Write-Host "Wrote $appId"
            } catch {
                Write-Warning "Could not write $appId (run as admin once, or create it by hand with the content 24780)."
            }
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

if ($LoopbackGuest -ge 1 -and -not $RestoreLoopback) {
    $dir = Get-InstanceDir $LoopbackGuest
    $regions = Join-Path $dir 'Regions'
    if (-not (Test-Path $regions)) { throw "Missing $regions, run -Setup first." }
    if ((Get-Item $regions -Force).LinkType -ne 'Junction') {
        throw "$regions is not a junction (already a loopback guest?)."
    }
    # Site names are 1 to 15 of A-Z a-z 0-9 -; a long computer name falls back to a fixed alias.
    $alias = "$env:COMPUTERNAME-GUEST"
    if ($alias -notmatch '^[A-Za-z0-9-]{1,15}$') { $alias = 'LOOPBACK-GUEST' }
    # Non-recursive: removes the junction only, never the real Regions behind it.
    [IO.Directory]::Delete($regions)
    New-Item -ItemType Directory -Force -Path $regions | Out-Null
    $store = Join-Path $dir 'RegionSyncStore'
    $ini = Join-Path $dir 'SC4RegionSync.local.ini'
    Set-Content -Path $ini -Encoding ASCII -Value @("HostAlias=$alias", "StoreDirectory=$store")
    Write-Host "Wrote $ini"
    Write-Host ""
    Write-Host "Put this in SC4RegionSyncLauncher-Guest\launcher.ini:"
    Write-Host "[Launcher]"
    Write-Host "StatusDirectory=$store\_instances"
    Write-Host "[Lan]"
    Write-Host "HostAlias=$alias"
    Write-Host "RegionsDirectory=$regions"
    Write-Host "AllowLoopback=1"
}

if ($RestoreLoopback) {
    $dir = Get-InstanceDir $LoopbackGuest
    $regions = Join-Path $dir 'Regions'
    $ini = Join-Path $dir 'SC4RegionSync.local.ini'
    if (Test-Path $regions) {
        if ((Get-Item $regions -Force).LinkType -eq 'Junction') { throw "$regions is already a junction." }
    }
    $archive = Join-Path $SourceUserDir ('RegionSyncLAN\archive\loopback-guest-' + (Get-Date -Format 'yyyy-MM-dd HHmmss'))
    New-Item -ItemType Directory -Force -Path $archive | Out-Null
    if (Test-Path $regions) { Move-Item -LiteralPath $regions -Destination (Join-Path $archive 'Regions') }
    if (Test-Path $ini) { Move-Item -LiteralPath $ini -Destination $archive }
    cmd /c mklink /J "`"$regions`"" "`"$sharedRegions`"" | Out-Null
    Write-Host "Restored $regions -> $sharedRegions. Guest files kept in $archive"
}

if (-not $Setup -and -not $Launch -and $LoopbackGuest -lt 1) {
    Write-Host "Nothing to do. Pass -Setup and/or -Launch, or -LoopbackGuest N."
}
