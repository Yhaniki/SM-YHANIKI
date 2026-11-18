# pack-thirdparty.ps1 與 setup-thirdparty.ps1 共用的定義。不要直接執行這個檔案。

. (Join-Path $PSScriptRoot 'sm-common.ps1')

# 資源包裡的資料夾（相對於 ThirdParty 根目錄）。
# 這份清單是從實際建置的 MSBuild 檔案存取記錄（*.tlog）整理出來的：
# 編譯 / 連結 StepMania 時只會讀到這些資料夾。
$SMThirdPartyFolders = @(
    'BaseClasses'
    'dx80'
    'ffmpeg'
    'libjpeg'
    'libpng'
    'libresample'
    'mad-0.15.1b'
    'vorbis'
)

# 每個資料夾挑一個關鍵檔，用來判斷資源包是否完整。
# 要跟 src\ThirdParty.props 裡的 _SMThirdPartyKeyFile 保持一致。
$SMThirdPartyKeyFiles = @(
    'BaseClasses\streams.h'
    'dx80\include\d3d8.h'
    'ffmpeg\include\ffmpeg\avformat.h'
    'libjpeg\jpeglib.h'
    'libpng\include\png.h'
    'libresample\include\libresample.h'
    'mad-0.15.1b\mad.h'
    'vorbis\vorbis\vorbisfile.h'
)

# PACK-INFO.txt 的第一行，用來確認拿到的是 ThirdParty 資源包。
$SMThirdPartyPackTitle = 'SM-YHANIKI ThirdParty 資源包'

$SMUserPropsPath        = Join-Path $SMRepoRoot 'src\ThirdParty.user.props'
$SMDefaultThirdPartyDir = Join-Path $SMRepoRoot 'ThirdParty'
$SMDefaultPackage       = Join-Path $SMRepoRoot 'ThirdParty-pack.zip'

# 讀出目前 VS 專案實際使用的 ThirdParty 位置（有 ThirdParty.user.props 就用它，否則用預設值）。
function Get-SMConfiguredThirdPartyDir {
    if (Test-Path -LiteralPath $SMUserPropsPath) {
        [xml]$xml = Get-Content -LiteralPath $SMUserPropsPath -Raw -Encoding UTF8
        $value = ($xml.Project.PropertyGroup | ForEach-Object { $_.SMThirdPartyDir } | Where-Object { $_ } | Select-Object -First 1)
        if ($value) { return [System.Uri]::UnescapeDataString($value).TrimEnd('\') }
    }
    return $SMDefaultThirdPartyDir
}

# 回傳缺少的關鍵檔清單（空陣列代表完整）。
function Get-SMMissingKeyFiles([string]$ThirdPartyDir) {
    return @($SMThirdPartyKeyFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $ThirdPartyDir $_)) })
}
