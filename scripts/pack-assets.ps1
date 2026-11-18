<#
.SYNOPSIS
    把遊戲素材打包成 Assets-pack.zip（維護者用）。

.DESCRIPTION
    內容由 assets-common.ps1 定義：
      - 整個資料夾：Themes、NoteSkins、Characters、Courses、Announcers、BGAnimations、CDTitles、Packages、RandomMovies
      - Data：只帶白名單裡的內建資料與初始設定，不帶個人設定、遊玩紀錄、Profile
      - 空資料夾：Cache、Screenshots、Songs、Utils、Data\LocalProfiles、Data\MachineProfile
        （資料夾裡原本的 instructions.txt 會保留，例如 Songs\Instructions.txt）
    產生的 zip 交給其他開發者，由他們用 setup-assets.cmd 安裝。

.PARAMETER Source
    素材所在的資料夾。預設：repo 根目錄。

.PARAMETER Output
    輸出的 zip 路徑。預設：<repo>\Assets-pack.zip

.EXAMPLE
    scripts\pack-assets.cmd
.EXAMPLE
    scripts\pack-assets.cmd -Output D:\share\Assets-pack.zip
#>
param(
    [string]$Source,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'assets-common.ps1')

if ($Source) { $Source = Resolve-SMPath $Source } else { $Source = $SMRepoRoot }
if ($Output) { $Output = Resolve-SMPath $Output } else { $Output = $SMDefaultAssetsPackage }

Write-Host "來源：$Source"
Write-Host "輸出：$Output"

$missing = @()
$missing += @($SMAssetFolders | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Source $_) -PathType Container) })
$missing += @($SMAssetDataFiles | ForEach-Object { "Data\$_" } | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Source $_) -PathType Leaf) })
if ($missing.Count -gt 0) {
    throw "來源缺少：$($missing -join ', ')"
}

$stats = @{ Files = 0; Bytes = 0 }
New-SMZip $Output {
    param($zip)
    foreach ($folder in $SMAssetFolders) {
        $r = Add-SMZipTree $zip $Source $folder
        $stats.Files += $r.Files
        $stats.Bytes += $r.Bytes
        Write-Host ("  + {0,-14} {1,5} 個檔案  {2,7:N1} MB" -f $folder, $r.Files, ($r.Bytes / 1MB))
    }

    $dataBytes = 0
    foreach ($file in $SMAssetDataFiles) {
        $dataBytes += Add-SMZipFile $zip $Source "Data\$file"
    }
    $stats.Files += $SMAssetDataFiles.Count
    $stats.Bytes += $dataBytes
    Write-Host ("  + {0,-14} {1,5} 個檔案  {2,7:N1} MB  （{3}）" -f 'Data', $SMAssetDataFiles.Count, ($dataBytes / 1MB), ($SMAssetDataFiles -join ', '))

    # 空資料夾：只保留資料夾本身附的說明檔（instructions.txt），其他內容不打包。
    $emptyLabels = @()
    foreach ($folder in $SMAssetEmptyFolders) {
        $dir = Join-Path $Source $folder
        $readmes = @()
        if (Test-Path -LiteralPath $dir -PathType Container) {
            $readmes = @(Get-ChildItem -LiteralPath $dir -File | Where-Object { $_.Name -ieq 'instructions.txt' })
        }
        if ($readmes.Count -gt 0) {
            foreach ($readme in $readmes) {
                $stats.Bytes += Add-SMZipFile $zip $Source (Join-Path $folder $readme.Name)
                $stats.Files++
            }
            $emptyLabels += "$folder（含 $($readmes.Name -join ', ')）"
        }
        else {
            Add-SMZipEmptyDir $zip $folder
            $emptyLabels += $folder
        }
    }
    Write-Host "  + 空資料夾：$($emptyLabels -join ', ')"

    Add-SMZipText $zip 'PACK-INFO.txt' (@(
        $SMAssetPackTitle
        "建立時間：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        "建立時的 commit：$(Get-SMGitCommit)"
        "整個資料夾：$($SMAssetFolders -join ', ')"
        "Data：$($SMAssetDataFiles -join ', ')"
        "空資料夾：$($emptyLabels -join ', ')"
        "檔案數：$($stats.Files)"
        ''
        '不包含：StepMania.ini、Keymaps.ini、Coins.dat、Catalog.xml、Profile 內容、歌曲（遊戲會自己產生，或屬於個人資料）'
        '安裝方式：把這個 zip 放到 repo 根目錄，執行 scripts\setup-assets.cmd'
    ) -join "`r`n")
}

$zipMB = (Get-Item -LiteralPath $Output).Length / 1MB
Write-Host ''
Write-Host ("完成：{0} 個檔案，原始 {1:N1} MB，壓縮後 {2:N1} MB" -f $stats.Files, ($stats.Bytes / 1MB), $zipMB) -ForegroundColor Green
Write-Host "把 $Output 交給其他開發者，請他們放到 repo 根目錄後執行 scripts\setup-assets.cmd。"
