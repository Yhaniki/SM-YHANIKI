# .gn（SDO StepFile）格式與本專案的實作

本文件記錄 `GNFile.*` / `NotesLoaderGN.*` / `NotesWriterGN.*` 依據的格式，以及實作上做過的取捨。
格式知識來自對實際檔案的分析與 `stepfile_dump2.py`（外部工具）的行為比對；本專案的實作已對 873 個實際 `.gn` 做過位元組級的往返驗證。

## 一、四種容器

`.gn` 的外殼有四種，`GNFile::Decrypt` 會自動判斷：

| 容器 | 判斷方式 | 解法 |
|------|----------|------|
| `PLAIN` | 檔頭 offset 4 是 `gn\0\0` 且 offset 284 是 300 | 直接就是本文 |
| `DDRM` | 檔頭四位元組是 `ddrm`（0x6D726464） | 0x54 bytes 表頭；seed1 在 0x0C，解出 0x20 起的 32 bytes 得 seed2，再解本文 |
| `SDOM` | 檔首是資源檔名表，往後掃到內嵌的 StepFile 表頭（常見 offset 0x1C8） | 表頭 300 bytes 是明文，其後整段是 LCG 密文；用那 300 bytes 當已知明文回推 seed |
| `REWU`（熱舞 Online） | 以上皆非 | 整檔 LCG；用 offset 4 的 `gn\0\0` 當已知明文暴力搜 seed |

三者的 LCG 都是 `state = state * 0x3D09`，取 `(state >> 16) & 0xFF` 當 keystream，加密是加、解密是減。
還原 seed 的搜尋空間是 2^24，C++ 實作單檔約數十毫秒。

**SDOM 的特殊之處**：加密區的明文開頭就是同一份 300 bytes 表頭（所以才能做已知明文攻擊），
而且遊戲會檢查加密區的 CRC32，寫回時必須把 CRC 湊回原值 —— `Crc32FixLast4` 用中間相遇法改寫最後 4 bytes 達成。

## 二、StepFile 本體

固定 300 bytes 表頭（全小端序）＋ Easy／Normal／Hard 三段譜面資料。

| 偏移 | 內容 |
|------|------|
| 0 | `file_id` (int32) |
| 4 | `file_type`，固定 `gn` |
| 16 | 基準 BPM (float) |
| 20 | 三個難度的 level (int16 ×3) |
| 26 | 14 bytes，未解讀（遊戲的譜面載入不讀） |
| 40 | 三個難度的音符數 (int32 ×3) |
| 52 | `extra52` (int32 ×3)：歌曲最後一個 measurement 的編號 |
| 64 | 三個難度的 StepFrame 個數 (int32 ×3) |
| 76 | 六個 32 bytes 字串：未知0、標題、未知1、作者、製作、檔名 |
| 272 | 三個難度的歌曲長度秒數 (int32 ×3) |
| 284 | `address_easy/normal/hard/end` (uint32 ×4)，`address_easy` 必為 300 |

### 遊戲怎麼用每個難度的表頭欄位

依據 SDO 單機版 `sdo_stand_alone.exe` 的反編譯（`CNewNote::LoadNote` 與遊玩畫面），以及 65_remake 的
`docs/reverse-engineering/SDOM_STEPFILE_HEADER.md`、`GnStepFile.cs`、`GnChart.cs`：

| 欄位 | 遊戲的用法 | 寫入時的要求 |
|------|------------|--------------|
| 音符數（40） | 每顆音符狀態陣列的大小 | 官方的算法：frame type 2～8 中 `u0` 非 0 的 slot 數 |
| `extra52`（52） | 歌曲最後一個 measurement：查詢表配置 `extra52+1` 個 measurement，以 `measurement × 0x22 + type` 存 frame 指標且**不檢查範圍**；遊戲在第 `(extra52+1) × 192` row 結束 | **必須 ≥ 該難度最大的 frame measurement**，否則寫出陣列外；**必須 < 501**，否則整份譜面不載入 |
| StepFrame 個數（64） | 依序讀幾個 frame | 等於實際 frame 數 |
| 長度秒數（272） | ① 舞者動作的每拍毫秒數 ＝ `長度 × 1000 / (extra52 × DPS 每小節拍數)` ② 開局用 `rand() % 長度` 抽兩個秒數，直到 ≥ 21、≥ 41 為止 | 要跟 `extra52`、BPM 相符，舞步速度才對；**必須 > 41**，否則開局卡死 |
| 26～39 | 譜面載入（`CNewNote`）完全不讀 | 原樣保留 |

官方檔案的長度秒數大多等於「從第 0 拍到最後一個 measurement 結束的秒數，無條件捨去」，
這只用在完全沒有其他難度可以參考的時候。

字串沒有標示編碼，實測多為 GBK，少數台版是 Big5；loader 依序嘗試 `utf-8, chinese(936), big5(950), japanese, english`。

### StepFrame

每個 frame 是 `measurement(int32) + type(int16) + interval(uint16) + interval × 4 bytes 的 slot`。
frame 在檔案裡照 **(measurement, type) 由小到大** 排列，writer 也維持這個順序。

一個 measurement 是 4 拍，slot 把它切成 `interval` 等分，所以

```
全域拍數 = measurement * 4 + 4 * slot / interval
```

`interval` 最大 192，而 SM 的 `ROWS_PER_MEASURE` 剛好也是 192（48 × 4），
所以 GN 的格點可以無損對應到 SM 的 row：`row = measurement * 192 + 192 * slot / interval`。

### type

| type | 意義 |
|------|------|
| 1 | BPM 變化（slot 的 4 bytes 直接是一個 little-endian float） |
| 2 / 3 / 4 / 5 | 左 / 下 / 上 / 右 |
| 9 | 小節線，slot 的 `u0` 是遞增的小節編號 |
| 10 | 音樂起止標記 |

上下兩軌要注意：外部的 `SM_GN_NOTE_FORMAT.md` 原本把 3 寫成 Up、4 寫成 Down，
`stepfile_dump2.py` 的 `_TYPE_NAMES` 也是同一個錯。實際寫回遊戲會上下顛倒，
正確的是 `COL_TO_FRAME_TYPE = (2, 3, 4, 5)` 依序對到 Left / Down / Up / Right，
也就是 **3 = Down、4 = Up**。

slot 是 `u0(int16) + u1(uint8) + step_note_type(uint8)`；四個位元組全 0 代表空格。
`step_note_type`：0 = 一般音符，2 = hold 起點，3 = hold 終點。

## 三、對應到這個引擎

### 每個難度各自的 BPM 表

這是 `.gn` 跟 `.sm` 最大的結構差異：**BPM 事件（type 1）存在各難度自己的資料段裡**，
所以同一首歌的 Easy 與 Hard 可以有完全不同的變速。實測 40 首抽樣中有 1 首真的如此
（`sdom1502t.gn`：Hard 有 17 個變速事件，Easy/Normal 只有 7 個且位置不同）。

這個引擎的 `TimingData` 掛在 `Song` 上，一首歌只有一份。做法是：

- `Steps` 多一個 `m_pTiming`（`Steps::GetOwnTiming()` / `SetOwnTiming()`），NULL 表示沿用 Song 的。
- 切到某個難度時呼叫 `Song::UseTimingOf( pSteps )`，把那一份套到 `Song::m_Timing`。
  呼叫點：`ScreenSelectMusic::AfterStepsChange`、`ScreenEditMenu`、`ScreenGameplay::SetupSong`、
  `ScreenEdit` 的 F5/F6 換難度與載入。
- 所有既有讀 `Song::m_Timing` 的程式碼因此都不用動。
- 編輯器裡改過的 BPM 會在存檔與換難度前收回該難度的 `m_pTiming`。

為了讓這份資訊在走 `.sm`／cache 的路徑時不遺失，`NotesWriterSM` 會在每個 `#NOTES` 前面多寫一行

```
#STEPSTIMING:<stepstype>:<difficulty>:<offset>:<beat=bpm,...>:<beat=stop,...>;
```

`SMLoader` 讀到就套用到後面那個 `#NOTES`。其他格式的譜面不會有這個 tag，行為完全不變。

### 音樂對齊

`m_fBeat0OffsetInSeconds` 設成「從第 0 拍走到第一個 type-10 frame 所在拍數」需要的秒數，
也就是音樂的第 0 秒對齊 type-10 的位置。譜面的 measurement **不做平移**，
GN 的 `measurement * 4` 直接就是 SM 的拍數。

### level 與 meter

GN 表頭的 level 就是譜面等級本身，跟 SM 的 meter **一對一，不做換算** ——
工具端從 `.sm` 注入 `.gn` 時也是直接把 meter 寫進 level。SDO 的等級可以到 30、40，
遠超過 SM 的 `MAX_METER`(13)，硬壓過去只會讓畫面顯示的難度跟原譜對不起來。

寫回時，原本 level 是 0 的難度不動（避免把 `Steps::TidyUpData` 猜出來的數字寫進檔案），
其餘就照編輯器裡的 meter 寫回去。原本是空的、在編輯器裡新增的難度，直接寫入編輯器裡的 meter。

## 四、寫回 `.gn`

`Song::Save()` 在這首歌來自 `.gn` 時會一併呼叫 `Song::SaveToGNFile()`：

1. 先把原檔備份到 `<songdir>/FileBackup/<時間戳>_<檔名>.gn`，備份失敗就不寫。
2. 重新讀一次原檔當骨架（表頭沒解讀的欄位、type 9/10 等控制 frame 都留著）。
3. 每個難度用 `NoteData` 重建 type 2～5、用該難度的 `TimingData` 重建 type 1。
4. 更新該難度的表頭（遊戲的用法見上面的表）：
   - 音符數、StepFrame 個數直接重算。
   - `extra52`、長度秒數描述的是歌曲本身（在哪結束、舞者多快），所以從原值出發：
     `extra52` 加上最大 measurement 的變化，且不得小於最大 frame measurement
     （有些其他工具做的檔案小於它，一併修正）；長度秒數跟著 `extra52` 等比例調整，
     舞者每拍的時間（長度 ÷ `extra52`）完全不變。譜面沒變時兩者都不動。
   - 長度秒數至少 42（遊戲的硬性要求）。
   - 最大 measurement 超過 500 時遊戲不會載入，這時不寫 `.gn`、回報錯誤（`.sm` 照常存）。
5. 用原本的鑰匙包回原容器。

**原本是空的難度**（原檔那一格沒有音符，例如只放了 Hard 的檔案，在編輯器裡新增了 Easy）也會寫進去：

- 控制 frame（小節線、音樂起止標記等）從原檔裡有譜面、最難的那個難度複製一份，
  譜面比它長時一樣照間隔往後補小節線。空格裡有時也有控制 frame，但那是佔位用的
  （例如音樂結束標記就放在開頭），只有在沒有任何難度有譜面時才沿用。
- `extra52`、長度秒數從那個難度沿用，舞步速度、歌曲結束位置都跟它一樣；新譜面比它長才照上面的方式調整。
  完全沒有其他難度可參考時，才用「最大 measurement」與「到最後一個 measurement 結束的秒數」。
- `level` 寫入編輯器裡的 meter。
- 新增了但還沒有放任何音符的難度不寫入，避免在原版遊戲裡變成一個選得到但空白的難度。
- 歌曲裡沒有的難度（從來沒有，或在編輯器裡刪掉了）則原樣保留。

`interval` 會挑「放得下所有音符的最小值」（4/8/12/16/24/32/48/64/96/192），因此重寫過的檔案通常比原檔小。

小節線（type 9）在譜面被改長時，會照原檔的間隔與編號往後補。

### 保持原檔大小

`PrefsManager` 的 `GNKeepFileSize`（`Options` 區段，預設 `0`）：

- 預設 **關閉**：譜面塞不下就讓檔案變大。
- 打開時 SDOM／熱舞／DDRM 都會補零補回原本的大小，塞不下就存檔失敗（不會動到原檔）。

SDOM 不論這個開關如何，都會把加密區的 CRC32 修回原值。

## 五、已知限制

- 一個資料夾裡有多個 `.gn` 時只讀主檔（優先檔名以 `K` 結尾者）。
- 地雷（SM 的 `M`）在 GN 沒有對應，寫回時會被略過。
- 少數原始譜面有「沒有起點的 hold 結尾」，載入時會退化成單顆普通音符（873 個檔案裡共 11 顆）。
- `type 10` 的第二個標記（音樂結束）位置不會隨譜面變長而移動。
- loader 排在 SM/DWI/BMS/KSF 之後：資料夾裡若已有 `.sm` 就以 `.sm` 為準，
  而那份 `.sm` 裡的 `#GNFILE` 會記著它來自哪個 `.gn`，所以存檔時仍寫得回去。
