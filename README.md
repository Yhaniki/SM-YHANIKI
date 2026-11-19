# StepMania 3.9 永和金城武版(YHANIKI)
  + fork from [stepmania](https://github.com/stepmania/stepmania)
    - [tag v390release](https://github.com/stepmania/stepmania/releases/tag/v390release)
  + 部分功能沿用ddrtime編輯器版本StepMania
    - [STEPMANIA 3.9 編輯器我流版](https://forum.gamer.com.tw/C.php?bsn=16862&snA=1184)

<!-- TOC -->

- [StepMania 3.9 永和金城武版(YHANIKI)](#stepmania-39-永和金城武版yhaniki)
  - [VS-2019 Migration Notes](#vs-2019-migration-notes)
  - [編譯環境建置](#編譯環境建置)
  - [一般模式](#一般模式)
  - [遊戲中](#遊戲中)
  - [作譜模式](#作譜模式)
  - [連線模式](#連線模式)
  - [其他](#其他)
  - [TODO](#todo)
  - [測試與共同開發人員](#測試與共同開發人員)

<!-- /TOC -->
 
## VS-2019 Migration Notes
  ```
    + In order to support modern libraries, we've moved to VS2019 with MSVC140.
        VS2005/2008 will not work now.
    + To ease maintenance, this version of YHANIKI targets only modern windows OSs.
    + A bunch of compiler warnings have been ignored to reduce the workload of this migration.
        They should be dealt with after the ongoing refactoring is completed.
    + LibLua and SDL have been rebuilt with MSVC140 and are included in this repository (in folder ThirdParty_rebuild).
    + Using C++14 now. Currently there are some problems with C++17.
    + Old assets pack won't work with this source. Please contact devs for updated assets pack.
  ```
 
## 編譯環境建置
  1. 安裝 Visual Studio 2019，工作負載選「使用 C++ 的桌面開發」，並裝上 Windows 10 SDK。
     - 用 VS 2022 以後的版本也可以，但要在 Visual Studio Installer 的「個別元件」加裝「MSVC v142 - VS 2019 C++ x64/x86 建置工具」。
  2. clone 專案。
  3. 向開發者索取素材包 `Assets-pack.zip`，放到 repo 根目錄，雙擊 `scripts\setup-assets.cmd`。
     - 素材會解壓縮到 repo 根目錄（跟 `Program` 資料夾同一層），遊戲從這裡讀取，不能改位置。
     - zip 放在別處：`scripts\setup-assets.cmd -Package D:\Downloads\Assets-pack.zip`
     - 歌曲不在素材包裡，請自己放進 `Songs\`。
  4. 向開發者索取第三方資源包 `ThirdParty-pack.zip`，放到 repo 根目錄，雙擊 `scripts\setup-thirdparty.cmd`。
     - 想放到別的位置：`scripts\setup-thirdparty.cmd -ThirdPartyDir D:\deps\sm-thirdparty`
     - zip 放在別處：`scripts\setup-thirdparty.cmd -Package D:\Downloads\ThirdParty-pack.zip`
     - 腳本會把位置寫進 `src\ThirdParty.user.props`（只在本機、不進版控），VS 專案會從那個位置找 include / lib。
  5. 用 VS 開啟 `src\SM-YHANIKI.sln`，方案組態選 `Release | x86`（或 `Debug | x86`）後建置。
     - 產出：`Program\StepMania.exe`（Debug 為 `Program\StepMania-Debug.exe`）

  維護者更新兩種包：
  - 素材包：雙擊 `scripts\pack-assets.cmd`，從 repo 根目錄打包出 `Assets-pack.zip`。
    包含 `Themes`、`NoteSkins`、`Characters`、`Courses` 等整個資料夾，`Data` 只帶內建資料與預設 Theme 設定（`GamePrefs.ini`），
    不帶 `StepMania.ini`、`Keymaps.ini`、Profile 等個人設定；`Cache`、`Screenshots`、`Songs`、`Utils` 只建立空資料夾（保留資料夾原本的 `instructions.txt`，例如 `Songs\Instructions.txt`）。
    清單定義在 `scripts\assets-common.ps1`。
  - 第三方資源包：雙擊 `scripts\pack-thirdparty.cmd`，從目前設定的 ThirdParty 位置打包出 `ThirdParty-pack.zip`。
    只包含編譯會用到的 8 個資料夾（`BaseClasses`、`dx80`、`ffmpeg`、`libjpeg`、`libpng`、`libresample`、`mad-0.15.1b`、`vorbis`），
    清單定義在 `scripts\thirdparty-common.ps1`。
 
## 一般模式
  | 快速鍵     | 功能                 |
  | -----     | -----                |
  | F4        | 更新指定歌曲          |
  | F5        | 快速重讀歌曲          |
  | Ctrl + F5 | 指定資料夾快速重讀歌曲 |
 
  + note流速不因歌曲倍速改變
  + 結算成績畫面顯示歌曲倍速
  + 修正使用AUTOPLAY或譜面無note會崩潰的問題
  + hold結尾改為TNS_MARVELOUS(白光)
  + 影片速度根據歌曲倍速改變
  + combo數字圖層上移
  + 遊戲大廳Note數改為顯示總按鍵數(原本為Tap + Hold)
  + 使用Shift+f8顯示AutoPlayCPU、結算分數時為0分
  + 修改NoteSkin:note/note(RV)長條發光模式
  + 選歌畫面背景可換成目前選擇歌曲的背景圖（Options → Background Options → 選歌畫面 歌曲背景，預設關閉）
    - 需要主題支援：在選歌背景的 `BGAnimation.ini` 加一層 `File=currentsongbackground`，搭配 `Condition=SelectMusicSongBackground()` 跟著開關，
      並在 `[ScreenBackgroundOptions]` 加 `conf,SelectMusicSongBG`（語言檔要有 `SelectMusicSongBGTitle` / `SelectMusicSongBGHelp`）。
    - CyberiaStyle 6 的作法：三個 `ScreenSelectMusic *background` 各加一層 `Layer25`（在 `over.png` 之上、六角閃光與面板之下），
      這些主題檔案在素材包裡，要重新打包素材包才會帶給其他人。

## 遊戲中
  | 快速鍵     | 功能          |
  | ----------| ------------- |
  | F3        | 減速          |
  | F4        | 加速          |

## 作譜模式
  + ddrtime
    - BPM與STOP顯示最小可到小數點第三位 
    - 右上方BEAT與時間顯示可到小數點第六位(特殊用) 
    - 箭頭間距拉伸變更成以0.5倍為單位，範圍可到0.25x~8x 
      + 若想整數拉伸可用ctrl+pageup與ctrl+pagedown鍵控制 
    - STOP停拍可以變負數 
    - N鍵轉換負數功能
      + N鍵:建立一個BPM標籤 
      + CTRL+N:轉換BPM為負數
      + ALT+N:轉換STOP為負數 
    - 支援192nd箭頭編輯 
    - 增加直接編輯BPM與STOP功能 
    - 可追朔以前的備份  
      + 編輯的歌曲資料夾會多個FileBackup資料夾，並以時間戳記為檔案名稱 
    - 自動儲存
      + 可以選擇要儲存的時間間隔或關閉，預設自動儲存時間是5分鐘  
    - Reverse逆流編輯環境下可直覺操作  
    - Reverse逆流編輯直覺操作性可在Preferences下的Reverse Control Intuitive更改  
    - 增加DisplayBPM編輯，可在Edit Song Info下找到
  + 另外增加
    - 修正影片預覽撥放位置
    - 修正變速結尾bpm錯誤
    - 修正變速長條截斷問題
    - 修正開啟PlayerOptions, SongOptions 譜面還原問題(SONGMAN->Cleanup(); ignore)
    - 修正多段變速bpm計算錯誤問題
    - 修正多難度停拍位置
    - 測試前緩衝調整選項(Preferences->play mode beats buffer)
    - Preferences加入背景影片亮度調整功能(Screen Filter)
    - Preferences加入預設Reverse功能(Default Scroll Reverse)
    - 使用玩家預設NoteSkin(針對使用note(RV)玩家)
    - 測試譜面時顯示歌詞、判定與連擊數
    - 修正部分情況使用長條結尾當作假鍵(3號鍵)時閃退問題
    - 修正add/edit bg change檔案名稱過長顯示跑版問題

## 連線模式

  | 快速鍵     | 功能             |
  | -----     | -----            |
  | F1        | 重讀connect資料夾 |
  | F4        | 跳到當前選擇歌曲   |
  | F5        | 快速重讀歌曲      |
  | F8        | 開關玩家狀態列    |
  | 同時按左右 | 切換一般模式選歌  |
  
  | 指令                      | 功能                                                  |
  | -----                    | -----                                                 |
  | /host {玩家名稱或玩家編號} | 更換房主                                               |
  | /share [玩家編號]         | 傳送歌曲(過濾影片) 若不加玩家編號則傳送檔案給所有缺檔案玩家 |
  | /sharefull [玩家編號]     | 傳送歌曲(包含影片) 若不加玩家編號則傳送檔案給所有缺檔案玩家 |
  
  + 缺歌檔案傳送功能(/share /sharefull)
  + 修正CyberiaStyle 6連線模式無法顯示玩家名稱問題
  + 修正歌包跳回ALL GROUPS問題
  + 修正歌曲難度排序問題
  + 結算成績畫面傳送玩家百分比
  + 結算成績畫面傳送玩家血量graph
  + 遊戲強制變更為不關門模式
  + StandAloneServer(獨立執行伺服器，有視窗介面，說明見 [StandAloneServer/README.md](StandAloneServer/README.md))
  + 確認所有玩家連線譜面相同(判斷譜面按鍵數)
  + 大廳顯示玩家狀態(按F8開關)
  + 不是置中的玩家，遊戲中成績列表會顯示在上方
  + 大廳播放完整歌曲

## 其他
   + 修正options玩家變更名稱錯誤問題
   + options選預設charater(options->background options->danceing characters->static)
   + 增加使用右邊數字鍵輸入數字功能
   + options語言ini紀錄儲存
   + 修正ScreenDemonstration無歌曲崩潰問題

## TODO
  + 增加hold結尾判定模式
  + 打擊軌道背板
  + 遊戲輸入中文
  + 連線模式 大廳顯示歌曲背景
  + 連線模式 玩家聊天時顯示編號
  
## 測試與共同開發人員
  + MKLUO
  + 151
  + sky
  + Ameto
  + HM
  + Mochi
  + Zelda-YEH
  + Xiuer
  