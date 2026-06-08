param(
  [Alias("size")]
  [int]$FontSize = 14,
  [Alias("bbp")]
  [int]$Bpp = 1,
  [string]$Font = "",
  [string]$Text = "",
  [string]$Output = "",
  [string]$Active = "",
  [string]$Python = "python"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Split-Path -Parent $ScriptDir

if ($Bpp -ne 1) {
  throw "Only -Bpp/-bbp 1 is supported by the current ESP32 lazy font reader"
}

if ([string]::IsNullOrWhiteSpace($Font)) {
  $Font = Join-Path $ProjectDir "fonts\puhui.ttf"
} elseif (-not [System.IO.Path]::IsPathRooted($Font)) {
  $Font = Join-Path $ProjectDir $Font
}

if ([string]::IsNullOrWhiteSpace($Text)) {
  $Text = Join-Path $ProjectDir "fonts\常用字.txt"
} elseif (-not [System.IO.Path]::IsPathRooted($Text)) {
  $Text = Join-Path $ProjectDir $Text
}

if ([string]::IsNullOrWhiteSpace($Output)) {
  $Output = Join-Path $ProjectDir "fonts\llm_text_${FontSize}_lazy.bin"
} elseif (-not [System.IO.Path]::IsPathRooted($Output)) {
  $Output = Join-Path $ProjectDir $Output
}

if ([string]::IsNullOrWhiteSpace($Active)) {
  $Active = Join-Path $ProjectDir ("font_active\" + [System.IO.Path]::GetFileName($Output))
} elseif (-not [System.IO.Path]::IsPathRooted($Active)) {
  $Active = Join-Path $ProjectDir $Active
}

& $Python (Join-Path $ProjectDir "tools\generate_lazy_font.py") `
  --font $Font `
  --text $Text `
  --size $FontSize `
  --bpp $Bpp `
  --output $Output

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Active) | Out-Null
Copy-Item -LiteralPath $Output -Destination $Active -Force

Write-Host "copied to $Active"
