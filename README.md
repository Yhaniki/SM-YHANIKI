# SM-YHANIKI

SM-YHANIKI 是以 [StepMania 3.9](https://github.com/stepmania/stepmania) 為基礎開發的 Windows 衍生專案，提供譜面編輯、網路對戰與獨立伺服器等功能。

部分譜面編輯功能源自 [ddrtime 編輯器版本的 StepMania](https://forum.gamer.com.tw/C.php?bsn=16862&snA=1184)。

## 基本功能

- 遊戲：歌曲倍速、Hold 結尾判定、打擊軌道背板、歌曲背景及中文輸入。
- 譜面編輯：BPM、STOP、192nd 箭頭、Reverse 編輯、自動儲存與備份。
- 網路對戰：玩家狀態同步、歌曲檔案傳送、聊天室指令及大廳背景。
- 伺服器：提供可獨立執行的視窗化伺服器，不需同時開啟遊戲主程式。

## 快速建置

需求：Windows 10 或更新版本、Visual Studio 2019 或含 v142 工具組的較新版本、Windows 10 SDK，以及 Win32 / x86 C++ 工具。

### 1. 下載必要資源

- [素材包 `Assets-pack.zip`](https://drive.google.com/file/d/1WMjjtO1CiMDN5yKbQ4CxyOb50TX7IsO8/view?usp=drive_link)
- [第三方資源包 `ThirdParty-pack.zip`](https://drive.google.com/file/d/1mS1s-bfnuFG0_Snnzf2E15XQSKwRWeX8/view?usp=drive_link)

歌曲檔案不包含在素材包中，請自行放入 `Songs\`。

### 2. 安裝資源

```cmd
git clone https://github.com/Yhaniki/SM-YHANIKI.git
cd SM-YHANIKI
scripts\setup-assets.cmd -Package D:\Downloads\Assets-pack.zip
scripts\setup-thirdparty.cmd -Package D:\Downloads\ThirdParty-pack.zip
```

### 3. 建置

使用 Visual Studio 開啟 [`src\SM-YHANIKI.sln`](src/SM-YHANIKI.sln)，選擇 `Release | x86` 並建置。

輸出檔案為 `Program\StepMania.exe`；Debug 組態輸出為 `Program\StepMania-Debug.exe`。

## 文件

- [完整建置指南](docs/build.md)
- [遊戲與譜面編輯器功能](docs/gameplay.md)
- [網路功能](docs/network.md)
- [獨立執行伺服器](StandAloneServer/README.md)

## 致謝

- [StepMania](https://github.com/stepmania/stepmania)
- [ddrtime 編輯器版本的 StepMania](https://forum.gamer.com.tw/C.php?bsn=16862&snA=1184)
- 感謝參與測試、回報問題及協助開發的貢獻者：MKLUO、151、sky、Ameto、HM、Mochi、Zelda-YEH、Xiuer。
