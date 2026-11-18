<#
.SYNOPSIS
    把編譯 StepMania 需要的第三方函式庫打包成 ThirdParty-pack.zip（維護者用）。

.DESCRIPTION
    只打包 thirdparty-common.ps1 列出的資料夾，不會把 DirectX SDK、WinDDK、boost 等用不到的東西包進去。
    產生的 zip 交給其他開發者，由他們用 setup-thirdparty.cmd 安裝。

.PARAMETER Source
    來源 ThirdParty 資料夾。預設是目前 VS 專案設定的位置
    （有 src\ThirdParty.user.props 就用它，否則為 <repo>\ThirdParty）。

.PARAMETER Output
    輸出的 zip 路徑。預設：<repo>\ThirdParty-pack.zip

.EXAMPLE
    scripts\pack-thirdparty.cmd
.EXAMPLE
    scripts\pack-thirdparty.cmd -Source D:\old\ThirdParty -Output D:\share\ThirdParty-pack.zip
#>
param(
    [string]$Source,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'thirdparty-common.ps1')

if ($Source) { $Source = Resolve-SMPath $Source } else { $Source = Get-SMConfiguredThirdPartyDir }
if ($Output) { $Output = Resolve-SMPath $Output } else { $Output = $SMDefaultPackage }

Write-Host "來源：$Source"
Write-Host "輸出：$Output"

if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
    throw "找不到來源資料夾：$Source"
}
$missingFolders = @($SMThirdPartyFolders | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Source $_) -PathType Container) })
if ($missingFolders.Count -gt 0) {
    throw "來源資料夾缺少：$($missingFolders -join ', ')"
}
$missingFiles = Get-SMMissingKeyFiles $Source
if ($missingFiles.Count -gt 0) {
    throw "來源資料夾不完整，缺少：$($missingFiles -join ', ')"
}

$stats = @{ Files = 0; Bytes = 0 }
New-SMZip $Output {
    param($zip)
    foreach ($folder in $SMThirdPartyFolders) {
        $r = Add-SMZipTree $zip $Source $folder
        $stats.Files += $r.Files
        $stats.Bytes += $r.Bytes
        Write-Host ("  + {0,-12} {1,4} 個檔案" -f $folder, $r.Files)
    }
    Add-SMZipText $zip 'PACK-INFO.txt' (@(
        $SMThirdPartyPackTitle
        "建立時間：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        "建立時的 commit：$(Get-SMGitCommit)"
        "內容：$($SMThirdPartyFolders -join ', ')"
        "檔案數：$($stats.Files)"
        ''
        '安裝方式：把這個 zip 放到 repo 根目錄，執行 scripts\setup-thirdparty.cmd'
        '（要放到別的位置：scripts\setup-thirdparty.cmd -ThirdPartyDir <路徑>）'
    ) -join "`r`n")
}

$zipMB = (Get-Item -LiteralPath $Output).Length / 1MB
Write-Host ''
Write-Host ("完成：{0} 個檔案，原始 {1:N1} MB，壓縮後 {2:N1} MB" -f $stats.Files, ($stats.Bytes / 1MB), $zipMB) -ForegroundColor Green
Write-Host "把 $Output 交給其他開發者，請他們放到 repo 根目錄後執行 scripts\setup-thirdparty.cmd。"
