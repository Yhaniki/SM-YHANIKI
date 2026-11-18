<#
.SYNOPSIS
    安裝第三方資源包（ThirdParty-pack.zip），並把 VS 專案指向安裝的位置。

.DESCRIPTION
    1. 把資源包解壓縮到 -ThirdPartyDir（預設 <repo>\ThirdParty）。
    2. 產生 src\ThirdParty.user.props，讓 StepMania.vcxproj 從同一個位置找 include / lib。
    3. 檢查 Visual Studio（v142 工具組）、Windows 10 SDK、遊戲素材，缺少時提出警告。

    如果沒有資源包、但 -ThirdPartyDir 已經有完整的第三方函式庫，就只更新 VS 專案設定。

.PARAMETER Package
    資源包 zip 的路徑。預設：<repo>\ThirdParty-pack.zip

.PARAMETER ThirdPartyDir
    要把第三方函式庫放在哪裡。預設：<repo>\ThirdParty

.EXAMPLE
    scripts\setup-thirdparty.cmd
.EXAMPLE
    scripts\setup-thirdparty.cmd -ThirdPartyDir D:\deps\sm-thirdparty
.EXAMPLE
    scripts\setup-thirdparty.cmd -Package D:\Downloads\ThirdParty-pack.zip -ThirdPartyDir D:\deps\sm-thirdparty
#>
param(
    [string]$Package,
    [string]$ThirdPartyDir
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'thirdparty-common.ps1')

if ($ThirdPartyDir) { $ThirdPartyDir = Resolve-SMPath $ThirdPartyDir } else { $ThirdPartyDir = $SMDefaultThirdPartyDir }
if ($ThirdPartyDir.Contains(';')) {
    throw "ThirdParty 路徑不能包含分號（;）：$ThirdPartyDir"
}

if ($Package) {
    $Package = Resolve-SMPath $Package
    if (-not (Test-Path -LiteralPath $Package -PathType Leaf)) { throw "找不到資源包：$Package" }
}
elseif (Test-Path -LiteralPath $SMDefaultPackage -PathType Leaf) {
    $Package = $SMDefaultPackage
}

Write-Host "repo 位置      ：$SMRepoRoot"
Write-Host "ThirdParty 位置：$ThirdPartyDir"

# ---- 1. 解壓縮 ----------------------------------------------------------------
if ($Package) {
    $info = Read-SMPackInfo $Package
    if (-not $info -or -not $info.StartsWith($SMThirdPartyPackTitle)) {
        throw "$Package 不是 ThirdParty 資源包（找不到「$SMThirdPartyPackTitle」的 PACK-INFO.txt）。素材包請改用 scripts\setup-assets.cmd。"
    }
    Write-Host "解壓縮資源包   ：$Package"
    $count = Expand-SMZip $Package $ThirdPartyDir
    Write-Host "  已解壓縮 $count 個檔案"
}
elseif ((Get-SMMissingKeyFiles $ThirdPartyDir).Count -eq 0) {
    Write-Host '沒有資源包，但這個位置已經有完整的第三方函式庫，只更新 VS 專案設定。'
}
else {
    throw "找不到資源包。請把 ThirdParty-pack.zip 放到 $SMRepoRoot，或用 -Package 指定 zip 的位置。"
}

$missing = Get-SMMissingKeyFiles $ThirdPartyDir
if ($missing.Count -gt 0) {
    throw "第三方函式庫不完整，缺少：$($missing -join ', ')。資源包可能是舊版或損壞，請向維護者重新索取。"
}

# ---- 2. 寫入 VS 專案設定 --------------------------------------------------------
# MSBuild 會把 % $ @ ' 當成特殊字元，先用 %XX 跳脫，再做 XML 跳脫。
$msbuildValue = ($ThirdPartyDir + '\') -replace '%', '%25' -replace '\$', '%24' -replace '@', '%40' -replace "'", '%27'
$xmlValue = [System.Security.SecurityElement]::Escape($msbuildValue)
$props = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- 由 scripts\setup-thirdparty.ps1 產生（$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')）。只在本機使用，不要 commit。 -->
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <SMThirdPartyDir>$xmlValue</SMThirdPartyDir>
  </PropertyGroup>
</Project>
"@
[System.IO.File]::WriteAllText($SMUserPropsPath, $props, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "已寫入 VS 專案設定：$SMUserPropsPath"

# ---- 3. 環境檢查（只警告，不中斷） ----------------------------------------------
$warnings = @()

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$hasV142 = $false
if (Test-Path -LiteralPath $vswhere) {
    foreach ($vsPath in @(& $vswhere -all -products * -property installationPath)) {
        if (Test-Path (Join-Path $vsPath 'MSBuild\Microsoft\VC\*\Platforms\Win32\PlatformToolsets\v142')) { $hasV142 = $true }
    }
}
if (-not $hasV142) {
    $warnings += '找不到 MSVC v142 建置工具。請安裝 Visual Studio 2019（「使用 C++ 的桌面開發」）；' +
                 '用 VS 2022 以後的版本則要在 Visual Studio Installer 的「個別元件」加裝「MSVC v142 - VS 2019 C++ x64/x86 建置工具」。'
}

if (-not (Test-Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include\10.*'))) {
    $warnings += '找不到 Windows 10 SDK，請用 Visual Studio Installer 安裝。'
}

$assetDirs = @('Data', 'NoteSkins', 'Themes')
$missingAssets = @($assetDirs | Where-Object { -not (Test-Path -LiteralPath (Join-Path $SMRepoRoot $_) -PathType Container) })
if ($missingAssets.Count -gt 0) {
    $warnings += "repo 根目錄還沒有遊戲素材（缺少 $($missingAssets -join '、')）。不影響編譯，" +
                 '但執行前要先安裝素材包：把 Assets-pack.zip 放到 repo 根目錄，執行 scripts\setup-assets.cmd。'
}

Write-Host ''
foreach ($w in $warnings) { Write-Host "警告：$w" -ForegroundColor Yellow }
if ($warnings.Count -gt 0) { Write-Host '' }

Write-Host '完成！接下來：' -ForegroundColor Green
Write-Host '  1. 用 Visual Studio 開啟 src\SM-YHANIKI.sln（已經開著的話，請關掉重開或重新載入方案）'
Write-Host '  2. 方案組態選 Release | x86（或 Debug | x86），建置'
Write-Host '  3. 產出：Program\StepMania.exe（Debug 為 Program\StepMania-Debug.exe）'
