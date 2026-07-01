param(
  [string]$BuildDir = "build-release",
  [string]$Sdkconfig = "sdkconfig.release",
  [string]$DistDir = "dist",
  [int]$Baud = 921600,
  [switch]$SkipBuild,
  [switch]$NoMerge
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectDir = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $ProjectDir $BuildDir
$SdkconfigPath = Join-Path $ProjectDir $Sdkconfig
$DistPath = Join-Path $ProjectDir $DistDir
$ImagesPath = Join-Path $DistPath "images"

function Invoke-Checked {
  param(
    [string]$Command,
    [string[]]$Arguments
  )

  if ([string]::IsNullOrWhiteSpace($Command)) {
    throw "Command is empty. Check that ESP-IDF tools are available in PATH."
  }

  Write-Host "> $Command $($Arguments -join ' ')"
  & $Command @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code ${LASTEXITCODE}: $Command"
  }
}

function Invoke-Idf {
  param([string[]]$Arguments)

  $idf = Get-Command "idf.py" -ErrorAction SilentlyContinue
  if ($idf -ne $null) {
    Invoke-Checked "idf.py" $Arguments
    return
  }

  if (-not [string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
    $idfScript = Join-Path $env:IDF_PATH "tools\idf.py"
    if (Test-Path $idfScript) {
      $pythonArgs = @($idfScript) + $Arguments
      Invoke-Checked "python" $pythonArgs
      return
    }
  }

  throw "idf.py not found. Open an ESP-IDF PowerShell/CMD, or run export.ps1 before this script."
}

function Invoke-Esptool {
  param([string[]]$Arguments)

  $esptool = Get-Command "esptool.py" -ErrorAction SilentlyContinue
  if ($esptool -ne $null) {
    Invoke-Checked "esptool.py" $Arguments
    return
  }

  $pythonArgs = @("-m", "esptool") + $Arguments
  Invoke-Checked "python" $pythonArgs
}

function Get-FileSha256 {
  param([string]$Path)
  return (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

function Convert-OffsetToInt {
  param([string]$Offset)
  if ($Offset.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
    return [Convert]::ToInt64($Offset.Substring(2), 16)
  }
  return [Convert]::ToInt64($Offset, 10)
}

Push-Location $ProjectDir
try {
  if (-not (Test-Path $SdkconfigPath)) {
    throw "Release sdkconfig not found: $SdkconfigPath"
  }

  if (-not $SkipBuild) {
    Invoke-Idf @("-B", $BuildDir, "-D", "SDKCONFIG=$Sdkconfig", "build")
  }

  $FlasherArgsPath = Join-Path $BuildPath "flasher_args.json"
  $FlashArgsPath = Join-Path $BuildPath "flash_args"
  if (-not (Test-Path $FlasherArgsPath)) {
    throw "Build output not found: $FlasherArgsPath"
  }

  $flasher = Get-Content $FlasherArgsPath -Raw | ConvertFrom-Json
  $chip = if ($flasher.extra_esptool_args.chip) { $flasher.extra_esptool_args.chip } else { "esp32s3" }
  $before = if ($flasher.extra_esptool_args.before) { $flasher.extra_esptool_args.before } else { "default_reset" }
  $after = if ($flasher.extra_esptool_args.after) { $flasher.extra_esptool_args.after } else { "hard_reset" }
  $flashMode = $flasher.flash_settings.flash_mode
  $flashFreq = $flasher.flash_settings.flash_freq
  $flashSize = $flasher.flash_settings.flash_size

  if (Test-Path $DistPath) {
    Remove-Item -LiteralPath $DistPath -Recurse -Force
  }
  New-Item -ItemType Directory -Path $ImagesPath -Force | Out-Null

  Copy-Item -LiteralPath $FlasherArgsPath -Destination (Join-Path $DistPath "flasher_args.json")
  if (Test-Path $FlashArgsPath) {
    Copy-Item -LiteralPath $FlashArgsPath -Destination (Join-Path $DistPath "flash_args")
  }

  $flashFiles = @()
  foreach ($property in $flasher.flash_files.PSObject.Properties) {
    $offset = $property.Name
    $relativePath = [string]$property.Value
    $sourcePath = Join-Path $BuildPath $relativePath
    if (-not (Test-Path $sourcePath)) {
      throw "Flash image missing: $sourcePath"
    }

    $destRelativePath = Join-Path "images" $relativePath
    $destPath = Join-Path $DistPath $destRelativePath
    $destParent = Split-Path $destPath -Parent
    New-Item -ItemType Directory -Path $destParent -Force | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destPath
    $manifestFilePath = $destRelativePath -replace "\\", "/"

    $flashFiles += [pscustomobject]@{
      offset = $offset
      offset_value = Convert-OffsetToInt $offset
      source = $relativePath
      file = $manifestFilePath
      size = (Get-Item $destPath).Length
      sha256 = Get-FileSha256 $destPath
    }
  }
  $flashFiles = $flashFiles | Sort-Object offset_value

  $factoryPath = Join-Path $DistPath "factory.bin"
  if (-not $NoMerge) {
    $mergeArgs = @(
      "--chip", $chip,
      "merge_bin",
      "-o", $factoryPath,
      "--flash_mode", $flashMode,
      "--flash_freq", $flashFreq,
      "--flash_size", $flashSize
    )

    foreach ($file in $flashFiles) {
      $mergeArgs += @($file.offset, (Join-Path $DistPath $file.file))
    }

    Invoke-Esptool $mergeArgs
  }

  $manifest = [pscustomobject]@{
    project = Split-Path $ProjectDir -Leaf
    generated_at = (Get-Date).ToString("yyyy-MM-ddTHH:mm:sszzz")
    build_dir = $BuildDir
    sdkconfig = $Sdkconfig
    chip = $chip
    flash_mode = $flashMode
    flash_freq = $flashFreq
    flash_size = $flashSize
    factory_bin = if (Test-Path $factoryPath) {
      [pscustomobject]@{
        file = "factory.bin"
        size = (Get-Item $factoryPath).Length
        sha256 = Get-FileSha256 $factoryPath
      }
    } else {
      $null
    }
    flash_files = $flashFiles | ForEach-Object {
      [pscustomobject]@{
        offset = $_.offset
        file = $_.file
        source = $_.source
        size = $_.size
        sha256 = $_.sha256
      }
    }
  }
  $manifest | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $DistPath "manifest.json")

  $flashBat = @"
@echo off
setlocal
set BAUD=%1
if "%BAUD%"=="" set BAUD=$Baud
esptool.py --chip $chip --baud %BAUD% --before $before --after $after write_flash 0x0 factory.bin
endlocal
"@
  Set-Content -Encoding ASCII -Path (Join-Path $DistPath "flash_factory.bat") -Value $flashBat

  $flashPs1 = @"
param(
  [int]`$Baud = $Baud
)

`$ErrorActionPreference = "Stop"
`$Here = Split-Path -Parent `$MyInvocation.MyCommand.Path
Push-Location `$Here
try {
  esptool.py --chip $chip --baud `$Baud --before $before --after $after write_flash 0x0 factory.bin
  if (`$LASTEXITCODE -ne 0) {
    throw "esptool.py failed with exit code `$LASTEXITCODE"
  }
} finally {
  Pop-Location
}
"@
  Set-Content -Encoding ASCII -Path (Join-Path $DistPath "flash_factory.ps1") -Value $flashPs1

  Write-Host ""
  Write-Host "Release package ready: $DistPath"
  Write-Host "Factory image: $factoryPath"
  Write-Host "Flash command: dist\flash_factory.bat"
} finally {
  Pop-Location
}
