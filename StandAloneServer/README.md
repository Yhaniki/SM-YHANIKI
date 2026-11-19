# StandAloneServer（獨立連線伺服器）

不用開遊戲也能架的連線房間。執行 `Program/StandAloneServer.exe`，按「啟動伺服器」，
玩家在遊戲的連線選項輸入這台電腦的 IP 就能進來。房主可以離開，房間不會因此解散。

## 使用方式

1. 執行 `Program/StandAloneServer.exe`，視情況改「伺服器名稱」，按「啟動伺服器」。
2. 「連線位址」列出這台電腦的每個網路介面（區網、Hamachi、ZeroTier…），把對應的 IP 給玩家；
   在同一台電腦上玩的人連 `127.0.0.1`。
3. 第一個進來的玩家是房主，跟遊戲內開伺服器時一樣，用 `/host`、`/start`、`/kick`、`/ban` 等指令。
   房主離線時由下一位玩家接手。

外網玩家要連進來，需要在路由器把 **TCP 8765** 轉到這台電腦（或大家用同一個 VPN）。
第一次執行時 Windows 防火牆可能會詢問是否允許連線，請允許。

## 視窗

| 項目 | 說明 |
| --- | --- |
| 玩家列表 | 跟遊戲裡的玩家列表一樣，房主顯示 `HOST`，其他人顯示編號（`/share N`、`/host N` 用的就是它）；狀態、是否有房主選的歌、傳歌進度 |
| 設為房主 / 踢出 / 封鎖 | 對列表中選取的玩家。封鎖以 IP 為單位，停止伺服器或按「解除封鎖」後失效 |
| 強制開始 | 等同房主的 `/start` |
| 取消傳歌 | 取消進行中與排隊中的傳歌 |
| 下方輸入框 | 以伺服器名義對所有玩家說話 |
| 訊息紀錄 | 連線、離線、聊天、開始遊戲等，同時寫入 `Program/StandAloneServer.log`（每次開程式重寫） |

伺服器名稱和「開啟程式時自動啟動」存在 `Program/StandAloneServer.ini`。

傳歌（`/share`、缺歌自動傳送）是**玩家之間直接連線**，伺服器只負責協調。
如果傳送的一方在外網又沒有開對外連線，傳歌會失敗，但不影響一起玩。

## 建置

```
MSBuild StandAloneServer/StandAloneServer.vcxproj -p:Configuration=Release -p:Platform=Win32
```

或在 Visual Studio 開 `src/SM-YHANIKI.sln` 建置 StandAloneServer 專案（v142 工具組）。
輸出 `Program/StandAloneServer.exe`，靜態連結 C runtime，可以單獨複製到別台電腦執行。

## 程式架構

房間邏輯**直接共用遊戲的程式碼**，不另外維護一份，兩邊功能不會分歧：

| 檔案 | 內容 |
| --- | --- |
| `src/NetworkSyncServer.cpp` | 伺服器本體（遊戲內「Start Server」用的也是它） |
| `src/ezsockets.cpp`、`src/NetworkPacket.cpp` | 連線與封包 |
| `DedicatedServer.*` | 繼承 `StepManiaLanServer`：在背景執行緒跑伺服器、提供狀態給視窗、執行視窗的指令 |
| `ServerWindow.cpp` | 視窗 |
| `ServerStubs.cpp`、`shim/` | 伺服器程式碼用到的少數遊戲元件（`LOG`、`PREFSMAN`、`ssprintf`）的替身 |

**要改房間規則、指令、傳歌流程，請改 `src/` 裡的程式碼**，遊戲和獨立伺服器會一起更新。
如果改完後 StandAloneServer 編不過，代表伺服器程式碼開始用到遊戲的其他部分，
請在 `shim/` 或 `ServerStubs.cpp` 補上需要的東西，不要複製伺服器程式碼過來。
