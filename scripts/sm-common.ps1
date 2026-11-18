# scripts\*.ps1 共用的工具函式。不要直接執行這個檔案。
# 處理路徑一律用 -LiteralPath 或 .NET API：素材裡有 Characters\SINGLES[L] 這種含 [ ] 的路徑，
# 一般的 -Path 會把 [ ] 當成萬用字元。

$SMRepoRoot = Split-Path -Parent $PSScriptRoot

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# 把使用者給的路徑（可以是相對路徑、不一定已存在）轉成完整路徑，結尾不帶反斜線。
function Resolve-SMPath([string]$Path) {
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    return [System.IO.Path]::GetFullPath($full).TrimEnd('\')
}

function Get-SMGitCommit {
    try { return [string](& git -C $SMRepoRoot rev-parse --short HEAD 2>$null) } catch { return '' }
}

# 把 $Root\$RelativeDir 整個資料夾加進 zip：zip 內用正斜線、保留大小寫，空資料夾也會保留。
# 回傳 @{ Files = 檔案數; Bytes = 原始大小 }。
function Add-SMZipTree($Zip, [string]$Root, [string]$RelativeDir) {
    $Root = $Root.TrimEnd('\')
    $top = Get-Item -LiteralPath (Join-Path $Root $RelativeDir)
    $files = 0
    $bytes = 0
    foreach ($item in @($top) + @(Get-ChildItem -LiteralPath $top.FullName -Recurse)) {
        $entryName = $item.FullName.Substring($Root.Length + 1) -replace '\\', '/'
        if ($item.PSIsContainer) {
            if (-not [System.IO.Directory]::EnumerateFileSystemEntries($item.FullName).GetEnumerator().MoveNext()) {
                [void]$Zip.CreateEntry($entryName + '/')
            }
        }
        else {
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $Zip, $item.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
            $files++
            $bytes += $item.Length
        }
    }
    return @{ Files = $files; Bytes = $bytes }
}

# 把單一檔案加進 zip。回傳檔案大小。
function Add-SMZipFile($Zip, [string]$Root, [string]$RelativePath) {
    $item = Get-Item -LiteralPath (Join-Path $Root $RelativePath)
    $entryName = $RelativePath -replace '\\', '/'
    [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $Zip, $item.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
    return $item.Length
}

function Add-SMZipEmptyDir($Zip, [string]$RelativeDir) {
    [void]$Zip.CreateEntry(($RelativeDir -replace '\\', '/').TrimEnd('/') + '/')
}

function Add-SMZipText($Zip, [string]$EntryName, [string]$Text) {
    $entry = $Zip.CreateEntry($EntryName)
    $writer = New-Object System.IO.StreamWriter($entry.Open(), (New-Object System.Text.UTF8Encoding($true)))
    try { $writer.Write($Text) } finally { $writer.Dispose() }
}

# 讀出 zip 裡 PACK-INFO.txt 的內容，沒有就回傳 $null。
function Read-SMPackInfo([string]$Package) {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Package)
    try {
        $entry = $zip.GetEntry('PACK-INFO.txt')
        if (-not $entry) { return $null }
        $reader = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
        try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    finally {
        $zip.Dispose()
    }
}

# 列出 zip 內所有項目的最上層名稱（例如 Themes/default/... → Themes）。
function Get-SMZipTopLevelNames([string]$Package) {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Package)
    try {
        return @($zip.Entries | ForEach-Object { ($_.FullName -split '/')[0] } | Sort-Object -Unique)
    }
    finally {
        $zip.Dispose()
    }
}

# 把 zip 解壓縮到 $Destination，同名檔案直接覆蓋；$Skip 列出的項目不解壓縮。
# 指向 $Destination 外面的路徑（zip slip）會直接報錯。回傳解出的檔案數。
function Expand-SMZip([string]$Package, [string]$Destination, [string[]]$Skip = @()) {
    [void][System.IO.Directory]::CreateDirectory($Destination)
    $destRoot = $Destination.TrimEnd('\') + '\'
    $count = 0
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Package)
    try {
        foreach ($entry in $zip.Entries) {
            if ($Skip -contains $entry.FullName) { continue }
            $dest = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($destRoot, $entry.FullName))
            if (-not $dest.StartsWith($destRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "壓縮檔內有不合法的路徑：$($entry.FullName)"
            }
            if ($entry.FullName.EndsWith('/')) {
                [void][System.IO.Directory]::CreateDirectory($dest)
                continue
            }
            [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($dest))
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
            $count++
        }
    }
    finally {
        $zip.Dispose()
    }
    return $count
}

# 建立 zip：先寫到暫存檔，$Fill 執行成功後才取代舊檔。$Fill 會拿到 ZipArchive 物件；
# 要從 $Fill 裡帶出統計數字，請改寫呼叫端的 hashtable（scriptblock 裡改不到呼叫端的一般變數）。
function New-SMZip([string]$Output, [scriptblock]$Fill) {
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Output))
    $tmp = "$Output.tmp"
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    $zip = [System.IO.Compression.ZipFile]::Open($tmp, [System.IO.Compression.ZipArchiveMode]::Create)
    $ok = $false
    try {
        & $Fill $zip
        $ok = $true
    }
    finally {
        $zip.Dispose()
        if (-not $ok) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    }
    # Windows PowerShell 5.1 的 Move-Item -Force 不會覆蓋已存在的檔案，要先刪掉舊檔。
    if (Test-Path -LiteralPath $Output) {
        try {
            Remove-Item -LiteralPath $Output -Force
        }
        catch {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            throw "無法覆蓋 $Output：檔案正被其他程式使用（例如用壓縮軟體開著），請關閉後再執行一次。"
        }
    }
    Move-Item -LiteralPath $tmp -Destination $Output
}
