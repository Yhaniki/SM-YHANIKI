# pack-assets.ps1 與 setup-assets.ps1 共用的定義。不要直接執行這個檔案。
# 素材一定要放在 repo 根目錄：StepMania.exe 啟動時會切換到 Program 的上一層，從那裡讀 Themes、Data 等資料夾。

. (Join-Path $PSScriptRoot 'sm-common.ps1')

# 整個資料夾打包。
$SMAssetFolders = @(
    'Announcers'
    'BGAnimations'
    'CDTitles'
    'Characters'
    'Courses'
    'NoteSkins'
    'Packages'
    'RandomMovies'
    'Themes'
)

# Data 只帶遊戲內建的資料與初始設定（白名單）。
# 不打包的（遊戲執行時會自己產生，或是個人設定 / 遊玩紀錄）：
#   StepMania.ini、Keymaps.ini、Coins.dat、Catalog.xml、LocalProfiles\、MachineProfile\ 的內容
$SMAssetDataFiles = @(
    'AI.ini'               # 電腦玩家的設定
    'GamePrefs.ini'        # 預設遊戲模式與 Theme（CyberiaStyle 6）；少了它會退回 default Theme
    'NamesBlacklist.dat'
    'Translation.dat'
    'Unlocks.dat'
    'splash.png'           # 啟動畫面
)

# 只建立資料夾，不帶內容；資料夾本身附的說明檔（instructions.txt，例如 Songs\Instructions.txt）會保留。
$SMAssetEmptyFolders = @(
    'Cache'
    'Screenshots'
    'Songs'
    'Utils'
    'Data\LocalProfiles'
    'Data\MachineProfile'
)

# PACK-INFO.txt 的第一行，用來確認拿到的是素材包。
$SMAssetPackTitle = 'SM-YHANIKI 素材包'

$SMDefaultAssetsPackage = Join-Path $SMRepoRoot 'Assets-pack.zip'

# 素材包裡允許出現的最上層名稱。安裝時用來擋掉不是素材包的 zip，避免解壓縮到 repo 根目錄時蓋到原始碼。
$SMAssetTopLevelNames = @(
    $SMAssetFolders
    'Data'
    $SMAssetEmptyFolders | ForEach-Object { ($_ -split '\\')[0] }
    'PACK-INFO.txt'
) | Sort-Object -Unique
