param(
    [Parameter(Mandatory=$true)][string]$WorkspaceDir,
    [Parameter(Mandatory=$true)][string]$InstallDir,
    [Parameter(Mandatory=$true)][string]$ZipPath,
    [Parameter(Mandatory=$true)][string]$ExePath,
    [int]$ParentPid = 0,
    [switch]$RestartService,
    [switch]$RelaunchNonElevated,
    [switch]$SkipRelaunch,
    [string]$LogDir = ""
)

$ErrorActionPreference = 'Stop'
$extractDir = Join-Path $WorkspaceDir 'extract'
$backupDir = Join-Path $WorkspaceDir 'backup'
$installFullPath = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\', '/')
$workspaceFullPath = [System.IO.Path]::GetFullPath($WorkspaceDir).TrimEnd('\', '/')
$effectiveLogDir = if ($LogDir) { $LogDir } else { $InstallDir }
$logPath = Join-Path $effectiveLogDir 'SunAndMon-hotupdate.log'
$errorLogPath = Join-Path $effectiveLogDir 'SunAndMon-update-error.log'
$serviceName = 'SunAndMonHostService'
$serviceWasRunning = [bool]$RestartService
$addedRelPaths = New-Object System.Collections.Generic.List[string]
$backedUpRelPaths = New-Object System.Collections.Generic.List[string]
$updateMode = 'full'
$updateMutex = $null
$ownsMutex = $false
$rollbackFailed = $false

function Write-UpdateLog {
    param([string]$Message)
    try {
        if ($effectiveLogDir -and !(Test-Path -LiteralPath $effectiveLogDir)) {
            New-Item -ItemType Directory -Path $effectiveLogDir -Force | Out-Null
        }
        $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
        "[$ts] $Message" | Out-File -LiteralPath $logPath -Append -Encoding utf8
    }
    catch {}
}

function Get-RequiredFreeBytes {
    param([string]$ArchivePath)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $uncompressedBytes = ($archive.Entries | Measure-Object -Property Length -Sum).Sum
        if ($null -eq $uncompressedBytes) { $uncompressedBytes = 0 }
        return [int64]$uncompressedBytes + 64MB
    }
    finally { $archive.Dispose() }
}

function Assert-FreeSpace {
    param([string]$Path, [int64]$RequiredBytes)
    if ($RequiredBytes -le 0) { return }
    $drive = New-Object System.IO.DriveInfo(([System.IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $Path).Path)))
    if ($drive.AvailableFreeSpace -lt $RequiredBytes) {
        throw "Not enough free disk space for hot update. Need about $([math]::Ceiling($RequiredBytes / 1MB)) MB free."
    }
}

function Assert-SafeZipEntries {
    param([string]$ArchivePath)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $norm = $entry.FullName -replace '\\', '/'
            if ([System.IO.Path]::IsPathRooted($norm) -or $norm.StartsWith('/') -or $norm.Contains(':') -or ($norm -split '/' | Where-Object { $_ -eq '..' })) {
                throw "Unsafe archive entry path detected: $($entry.FullName)"
            }
        }
    }
    finally { $archive.Dispose() }
}

function Wait-ForParentExit {
    param([int]$PidToWait)
    if ($PidToWait -le 0) { return }
    for ($i = 0; $i -lt 60; $i++) {
        if ($null -eq (Get-Process -Id $PidToWait -ErrorAction SilentlyContinue)) { return }
        Start-Sleep -Milliseconds 500
    }
}

function Stop-SunAndMonHostServiceIfRunning {
    try {
        $svc = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
        if ($null -eq $svc) { return }
        $cimSvc = Get-CimInstance Win32_Service -Filter "Name='$serviceName'" -ErrorAction SilentlyContinue
        if ($null -ne $cimSvc -and $cimSvc.PathName) {
            $svcPath = $cimSvc.PathName.Trim()
            if ($svcPath.StartsWith('"')) {
                $endQuote = $svcPath.IndexOf('"', 1)
                if ($endQuote -gt 1) { $svcPath = $svcPath.Substring(1, $endQuote - 1) }
            } else {
                $svcPath = ($svcPath -split '\s+')[0]
            }
            $prefix = $installFullPath.TrimEnd('\') + '\'
            if (!$svcPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                Write-UpdateLog "Skipping $serviceName (belongs to a different install dir: $svcPath)."
                $script:serviceWasRunning = $false
                return
            }
        }
        if ($svc.Status -eq 'Running' -or $svc.Status -eq 'StartPending') {
            $script:serviceWasRunning = $true
            Write-UpdateLog "Stopping $serviceName..."
            Stop-Service -Name $serviceName -Force -ErrorAction Stop
            for ($i = 0; $i -lt 40; $i++) {
                $svc.Refresh()
                if ($svc.Status -eq 'Stopped') {
                    Write-UpdateLog "$serviceName stopped."
                    return
                }
                Start-Sleep -Milliseconds 500
            }
        }
    }
    catch {
        Write-UpdateLog "Service stop check note: $($_.Exception.Message)"
    }
}

function Stop-InstallDirProcesses {
    $targetNames = @(
        'SunAndMon.exe', 'sunshine.exe', 'sunshinesvc.exe',
        'sunandmon-tunnel.exe', 'easytier-core.exe', 'moonlight-clipboard-helper.exe'
    )
    $prefix = $installFullPath.TrimEnd('\') + '\'
    try {
        $procs = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
            $_.ExecutablePath -and
            $_.ExecutablePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -and
            ($targetNames -contains $_.Name)
        }
        foreach ($p in $procs) {
            Write-UpdateLog "Terminating lingering process $($p.Name) (PID $($p.ProcessId))..."
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
        if ($procs) { Start-Sleep -Milliseconds 800 }
    }
    catch {
        Write-UpdateLog "Process cleanup note: $($_.Exception.Message)"
    }
}

function Wait-ForUnlockedFile {
    param([string]$Path)
    if (!(Test-Path -LiteralPath $Path)) { return }
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
            if ($stream) { $stream.Dispose() }
            return
        }
        catch { Start-Sleep -Milliseconds 500 }
    }
    throw "Timed out waiting for locked file to close: $Path"
}

function Get-PayloadRoot {
    param([string]$Root)
    $entries = @(Get-ChildItem -LiteralPath $Root -Force)
    if ($entries.Count -eq 1 -and $entries[0].PSIsContainer -and $entries[0].Name -ieq 'SunAndMon') {
        return $entries[0].FullName
    }
    return $Root
}

function Test-IsFullBundlePayload {
    param([string]$PayloadRoot)
    $hasExe = Test-Path -LiteralPath (Join-Path $PayloadRoot 'SunAndMon.exe')
    $hasHostOrQt = (Test-Path -LiteralPath (Join-Path $PayloadRoot 'host\sunshine.exe')) -or
                   (Test-Path -LiteralPath (Join-Path $PayloadRoot 'Qt6Core.dll'))
    return ($hasExe -and $hasHostOrQt)
}

function Get-ReplacedItems {
    param([string]$Path, [bool]$PreserveTunnel = $false)
    Get-ChildItem -LiteralPath $Path -Force | Where-Object {
        $itemFullPath = [System.IO.Path]::GetFullPath($_.FullName).TrimEnd('\', '/')
        $_.Name -ne 'Moonlight Game Streaming Project' -and
        $_.Name -ne 'portable.dat' -and
        $_.Name -ne 'portable.dat.inactive' -and
        $_.Name -notlike 'unins*.exe' -and
        $_.Name -notlike 'unins*.dat' -and
        $_.Name -notlike 'unins*.msg' -and
        (!$PreserveTunnel -or $_.Name -ine 'tunnel') -and
        $_.Name -notlike 'Moonlight-*.log' -and
        $_.Name -notlike 'SunAndMon-*.log' -and
        $_.Name -notlike '.backup-*' -and
        $_.Name -notlike '.repair-*' -and
        $_.Name -notlike 'update-backup-*' -and
        ![string]::Equals($itemFullPath, $workspaceFullPath, [System.StringComparison]::OrdinalIgnoreCase)
    }
}

function Acquire-HotUpdateMutex {
    foreach ($mutexName in @('Global\SunAndMonHotUpdateLock', 'Local\SunAndMonHotUpdateLock')) {
        try {
            $m = New-Object System.Threading.Mutex($false, $mutexName)
            try {
                if ($m.WaitOne(0)) {
                    $script:updateMutex = $m
                    $script:ownsMutex = $true
                    return $true
                }
                else {
                    $m.Dispose()
                    return $false
                }
            }
            catch [System.Threading.AbandonedMutexException] {
                $script:updateMutex = $m
                $script:ownsMutex = $true
                return $true
            }
        }
        catch [System.UnauthorizedAccessException] {
            continue
        }
    }
    return $true
}

function Expand-ZipFast {
    param([string]$ArchivePath, [string]$DestinationDir)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($ArchivePath, $DestinationDir)
}

function Start-RelaunchApp {
    param([string]$TargetExe, [string]$WorkDir, [bool]$DropElevation)
    if ($DropElevation) {
        try {
            Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$TargetExe`"" | Out-Null
            Write-UpdateLog "Relaunched $TargetExe via explorer.exe (non-elevated)."
            return
        }
        catch {
            Write-UpdateLog "explorer.exe relaunch fallback: $($_.Exception.Message)"
        }
    }
    Start-Process -FilePath $TargetExe -WorkingDirectory $WorkDir | Out-Null
    Write-UpdateLog "Relaunched $TargetExe directly."
}

try {
    if (!(Acquire-HotUpdateMutex)) {
        Write-UpdateLog "Another SunAndMon hot update is already in progress; exiting duplicate updater (ParentPid=$ParentPid)."
        return
    }

    Write-UpdateLog "Hot update started. InstallDir=$installFullPath Zip=$ZipPath ParentPid=$ParentPid RestartService=$serviceWasRunning"
    Wait-ForParentExit -PidToWait $ParentPid
    Stop-SunAndMonHostServiceIfRunning
    Stop-InstallDirProcesses
    Wait-ForUnlockedFile -Path $ExePath

    if (!(Test-Path -LiteralPath $WorkspaceDir)) { New-Item -ItemType Directory -Path $WorkspaceDir | Out-Null }
    if (Test-Path -LiteralPath $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force }
    if (Test-Path -LiteralPath $backupDir) { Remove-Item -LiteralPath $backupDir -Recurse -Force }
    New-Item -ItemType Directory -Path $extractDir | Out-Null
    New-Item -ItemType Directory -Path $backupDir | Out-Null

    Assert-SafeZipEntries -ArchivePath $ZipPath
    $requiredFreeBytes = Get-RequiredFreeBytes -ArchivePath $ZipPath
    Assert-FreeSpace -Path $InstallDir -RequiredBytes $requiredFreeBytes

    Expand-ZipFast -ArchivePath $ZipPath -DestinationDir $extractDir
    $payloadRoot = Get-PayloadRoot -Root $extractDir
    $isFullBundle = Test-IsFullBundlePayload -PayloadRoot $payloadRoot
    $payloadHasTunnel = Test-Path -LiteralPath (Join-Path $payloadRoot 'tunnel\sunandmon-tunnel.exe')
    $payloadHasHost = Test-Path -LiteralPath (Join-Path $payloadRoot 'host\sunshine.exe')
    $updateMode = if ($isFullBundle) { 'full' } else { 'overlay' }
    Write-UpdateLog "Extracted payload root=$payloadRoot mode=$updateMode hasTunnel=$payloadHasTunnel hasHost=$payloadHasHost"

    if ($isFullBundle) {
        if ($payloadHasHost) {
            Wait-ForUnlockedFile -Path (Join-Path $InstallDir 'host\sunshine.exe')
        }
        $itemsToReplace = @(Get-ReplacedItems -Path $InstallDir -PreserveTunnel (!$payloadHasTunnel))
        foreach ($item in $itemsToReplace) {
            Move-Item -LiteralPath $item.FullName -Destination $backupDir -Force
        }
        Get-ChildItem -LiteralPath $payloadRoot -Force | Where-Object {
            $_.Name -ne 'portable.dat' -and $_.Name -ne 'portable.dat.inactive'
        } | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $InstallDir -Recurse -Force
        }
    }
    else {
        $payloadFiles = @(Get-ChildItem -LiteralPath $payloadRoot -Recurse -File -Force)
        if ($payloadFiles.Count -eq 0) { throw 'The update archive contains no files.' }
        $payloadPrefix = [System.IO.Path]::GetFullPath($payloadRoot).TrimEnd('\') + '\'
        foreach ($file in $payloadFiles) {
            $rel = [System.IO.Path]::GetFullPath($file.FullName).Substring($payloadPrefix.Length)
            if ($rel -ieq 'portable.dat' -or $rel -ieq 'portable.dat.inactive') { continue }
            $dst = Join-Path $InstallDir $rel
            $dstDir = Split-Path -Path $dst -Parent
            if ($dstDir -and !(Test-Path -LiteralPath $dstDir)) {
                New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
            }
            if (Test-Path -LiteralPath $dst) {
                Wait-ForUnlockedFile -Path $dst
                $bak = Join-Path $backupDir $rel
                $bakDir = Split-Path -Path $bak -Parent
                if ($bakDir -and !(Test-Path -LiteralPath $bakDir)) {
                    New-Item -ItemType Directory -Path $bakDir -Force | Out-Null
                }
                Copy-Item -LiteralPath $dst -Destination $bak -Force
                $backedUpRelPaths.Add($rel) | Out-Null
            }
            else {
                $addedRelPaths.Add($rel) | Out-Null
            }
            Copy-Item -LiteralPath $file.FullName -Destination $dst -Force
        }
    }

    Remove-Item -LiteralPath $backupDir -Recurse -Force
    $backupDir = $null
    Write-UpdateLog "File swap completed successfully (mode=$updateMode)."

    if ($serviceWasRunning) {
        try {
            Write-UpdateLog "Restarting $serviceName..."
            Start-Service -Name $serviceName -ErrorAction Stop
            Write-UpdateLog "$serviceName restarted."
        }
        catch {
            Write-UpdateLog "Warning: failed to restart ${serviceName}: $($_.Exception.Message)"
        }
    }

    if (!$SkipRelaunch) {
        Start-RelaunchApp -TargetExe $ExePath -WorkDir $InstallDir -DropElevation ([bool]$RelaunchNonElevated)
    }
}
catch {
    Write-UpdateLog "ERROR: $($_.Exception.Message)"
    if ($backupDir -and (Test-Path -LiteralPath $backupDir)) {
        try {
            if ($updateMode -eq 'full') {
                Get-ChildItem -LiteralPath $backupDir -Force | ForEach-Object {
                    try {
                        Copy-Item -LiteralPath $_.FullName -Destination $InstallDir -Recurse -Force -ErrorAction Stop
                    }
                    catch {
                        $script:rollbackFailed = $true
                        Write-UpdateLog "Rollback item error ($($_.Name)): $($_.Exception.Message)"
                    }
                }
            }
            else {
                foreach ($rel in $addedRelPaths) {
                    $p = Join-Path $InstallDir $rel
                    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
                }
                foreach ($rel in $backedUpRelPaths) {
                    $bak = Join-Path $backupDir $rel
                    $dst = Join-Path $InstallDir $rel
                    if (Test-Path -LiteralPath $bak) {
                        try {
                            Copy-Item -LiteralPath $bak -Destination $dst -Force -ErrorAction Stop
                        }
                        catch {
                            $script:rollbackFailed = $true
                            Write-UpdateLog "Rollback item error ($rel): $($_.Exception.Message)"
                        }
                    }
                }
            }
            if (!$rollbackFailed) {
                Write-UpdateLog "Rollback completed."
            }
            else {
                Write-UpdateLog "Rollback completed with partial errors; backup preserved at $backupDir."
            }
        }
        catch {
            $script:rollbackFailed = $true
            Write-UpdateLog "Rollback error: $($_.Exception.Message)"
        }
    }
    if ($serviceWasRunning) {
        try { Start-Service -Name $serviceName -ErrorAction SilentlyContinue } catch {}
    }
    try { $_ | Out-File -LiteralPath $errorLogPath -Append -Encoding utf8 } catch {}
}
finally {
    try { Set-Location -LiteralPath $env:TEMP } catch {}
    if ($ownsMutex -and $updateMutex) {
        try { $updateMutex.ReleaseMutex() } catch {}
    }
    if ($updateMutex) {
        try { $updateMutex.Dispose() } catch {}
    }
    if (Test-Path -LiteralPath $extractDir) { Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue }
    if (!$rollbackFailed) {
        if ($backupDir -and (Test-Path -LiteralPath $backupDir)) { Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path -LiteralPath $WorkspaceDir) { Remove-Item -LiteralPath $WorkspaceDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}
