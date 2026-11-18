<#
.SYNOPSIS
    安裝素材包（Assets-pack.zip）到 repo 根目錄。

.DESCRIPTION
    StepMania.exe 啟動時會切換到 Program 的上一層（也就是 repo 根目錄）讀取 Themes、Data 等資料夾，
    所以素材一定要放在 repo 根目錄，這支腳本不提供改位置的選項。

    同名檔案會被覆蓋（例如更新後的 Theme）。StepMania.ini、Keymaps.ini、Profile 等個人設定不在素材包裡，
    不會被動到；但 Data\GamePrefs.ini 在包裡，重新安裝會把 Theme 選擇還原成素材包的預設值。

.PARAMETER Package
    素材包 zip 的路徑。預設：<repo>\Assets-pack.zip

.EXAMPLE
    scripts\setup-assets.cmd
.EXAMPLE
    scripts\setup-assets.cmd -Package D:\Downloads\Assets-pack.zip
#>
param(
    [string]$Package
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'assets-common.ps1')
. (Join-Path $PSScriptRoot 'thirdparty-common.ps1')

if ($Package) { $Package = Resolve-SMPath $Package } else { $Package = $SMDefaultAssetsPackage }
if (-not (Test-Path -LiteralPath $Package -PathType Leaf)) {
    throw "找不到素材包：$Package。請把 Assets-pack.zip 放到 $SMRepoRoot，或用 -Package 指定 zip 的位置。"
}

Write-Host "repo 位置：$SMRepoRoot"
Write-Host "素材包   ：$Package"

# 確認是素材包，而且裡面只有素材資料夾（避免解壓縮到 repo 根目錄時蓋到原始碼）。
$info = Read-SMPackInfo $Package
if (-not $info -or -not $info.StartsWith($SMAssetPackTitle)) {
    throw "$Package 不是素材包（找不到「$SMAssetPackTitle」的 PACK-INFO.txt）。ThirdParty 資源包請改用 scripts\setup-thirdparty.cmd。"
}
$unexpected = @(Get-SMZipTopLevelNames $Package | Where-Object { $SMAssetTopLevelNames -notcontains $_ })
if ($unexpected.Count -gt 0) {
    throw "素材包裡有不該出現在 repo 根目錄的項目：$($unexpected -join ', ')。為了避免蓋到原始碼，已停止安裝。"
}

$count = Expand-SMZip $Package $SMRepoRoot -Skip @('PACK-INFO.txt')
Write-Host "已解壓縮 $count 個檔案到 repo 根目錄（同名檔案已覆蓋；個人設定與 Profile 不在素材包裡，沒有被動到）。"

# 提醒還沒裝 ThirdParty 資源包的情況。
$thirdPartyReady = (Get-SMMissingKeyFiles (Get-SMConfiguredThirdPartyDir)).Count -eq 0
Write-Host ''
if (-not $thirdPartyReady) {
    Write-Host '提醒：還沒安裝 ThirdParty 資源包，編譯前請先執行 scripts\setup-thirdparty.cmd。' -ForegroundColor Yellow
    Write-Host ''
}
Write-Host '完成！編譯後執行 Program\StepMania.exe 即可（Debug 為 Program\StepMania-Debug.exe）。' -ForegroundColor Green
