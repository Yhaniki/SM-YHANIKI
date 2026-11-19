# 建置指南

本文件說明如何在 Windows 上準備 SM-YHANIKI 的建置環境、安裝素材與第三方元件，以及產生遊戲執行檔。

## 建置環境

- Windows 10 或更新版本。
- Visual Studio 2019，或包含 Visual Studio 2019 v142 工具組的較新版本。
- Visual Studio 的「使用 C++ 的桌面開發」工作負載。
- Windows 10 SDK。
- Win32 / x86 C++ 建置工具。

專案目前使用 C++14 與 Platform Toolset `v142`。主要方案組態為 `Debug | x86` 與 `Release | x86`。

使用 Visual Studio 2022 或更新版本時，請在 Visual Studio Installer 的「個別元件」中安裝：

```text
MSVC v142 - VS 2019 C++ x64/x86 建置工具
```

## 取得原始碼

```cmd
git clone https://github.com/Yhaniki/SM-YHANIKI.git
cd SM-YHANIKI
```

## 安裝素材

素材包應由專案維護者提供。取得 `Assets-pack.zip` 後，可以使用下列任一方式安裝：

- [下載 `Assets-pack.zip`](https://drive.google.com/file/d/1WMjjtO1CiMDN5yKbQ4CxyOb50TX7IsO8/view?usp=drive_link)

```cmd
scripts\setup-assets.cmd -Package D:\Downloads\Assets-pack.zip
```

或將 `Assets-pack.zip` 放在儲存庫根目錄後執行：

```cmd
scripts\setup-assets.cmd
```

素材會解壓縮到儲存庫根目錄，並與 `Program` 資料夾位於同一層。遊戲會從該位置讀取素材，請勿任意變更目錄結構。

歌曲檔案不包含在素材包內，請自行放入 `Songs\`。

## 安裝第三方元件

取得 `ThirdParty-pack.zip` 後執行：

- [下載 `ThirdParty-pack.zip`](https://drive.google.com/file/d/1mS1s-bfnuFG0_Snnzf2E15XQSKwRWeX8/view?usp=drive_link)

```cmd
scripts\setup-thirdparty.cmd -Package D:\Downloads\ThirdParty-pack.zip
```

也可以指定解壓縮目錄：

```cmd
scripts\setup-thirdparty.cmd -ThirdPartyDir D:\deps\sm-thirdparty
```

安裝腳本會建立 `src\ThirdParty.user.props`，將第三方元件路徑提供給 Visual Studio 專案。這是本機設定檔，不應提交至版本控制。

## 使用 Visual Studio 建置

1. 開啟 [`src\SM-YHANIKI.sln`](../src/SM-YHANIKI.sln)。
2. 選擇 `Release | x86` 或 `Debug | x86`。
3. 執行「建置方案」。

輸出檔案如下：

| 組態 | 輸出檔案 |
| --- | --- |
| `Release | x86` | `Program\StepMania.exe` |
| `Debug | x86` | `Program\StepMania-Debug.exe` |

獨立伺服器也包含在方案中。若只需要建置伺服器，可以執行：

```cmd
MSBuild StandAloneServer\StandAloneServer.vcxproj -p:Configuration=Release -p:Platform=Win32
```

輸出檔案為 `Program\StandAloneServer.exe`。

## 維護者打包素材

從儲存庫根目錄執行：

```cmd
scripts\pack-assets.cmd
```

素材包的內容清單定義於 `scripts\assets-common.ps1`。腳本會包含遊戲所需的主題、NoteSkins、Characters、Courses 及必要的預設資料，同時排除個人設定、快取、截圖與歌曲內容。

## 維護者打包第三方元件

先確認 `ThirdParty.user.props` 已指向正確的第三方元件目錄，再執行：

```cmd
scripts\pack-thirdparty.cmd
```

內容清單定義於 `scripts\thirdparty-common.ps1`。
