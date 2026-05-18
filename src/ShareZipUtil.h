/*
 * ShareZipUtil — share-song 用的 zip 打包/解壓 + HTTP 上傳/下載小工具
 *
 * 為什麼存在：原本 NetworkSyncManager 的 share-song 是把整個歌曲資料夾用 NSSData
 * 一塊一塊 (60KB / 塊) 透過 Steam reliable 傳給 server 再轉發給每個 receiver。
 * 缺點：
 *   - sender 上傳量 = 檔案大小 × N (每個 receiver 都要重送一次)
 *   - 沒有壓縮，.sm / .dwi 譜面其實能省 70%
 *   - 沒有加密，封包內容用 Wireshark 直接看光光
 *
 * 新流程：
 *   sender → minizip 打包+加密成 songs/connect/temp.zip
 *          → HTTP PUT 到 temp.sh (一次)
 *          → 把 URL+密碼透過 server 廣播給所有缺檔者 (NSSShareLink)
 *          → 每個 receiver 各自 HTTP GET temp.sh URL，解壓到 songs/connect/<歌>
 *
 * 本檔只提供 4 個純粹的工具函式，不碰 NSM 的狀態機。
 */

#ifndef SHARE_ZIP_UTIL_H
#define SHARE_ZIP_UTIL_H

#include "global.h"
#include <string>
#include <vector>

namespace ShareZipUtil
{
    // 把整個 srcDir (含子資料夾) 用 password 壓縮成 outZipPath。
    // 內部走 minizip 的 zipOpen + zipOpenNewFileInZip3 (帶 password 與 crc)。
    // 若 password 為空字串就不加密。
    // 回傳 true=成功。失敗時 errMsg 會帶錯誤描述、outZipPath 通常會被刪掉。
    //
    // 注意：minizip 用的是 ZipCrypto (傳統 zip 密碼)，不是 AES。
    // 抗鍵盤小偷夠用，抗專業破解不要指望。要強加密請整合 minizip-ng / 7z。
    bool ZipFolderWithPassword(const CString& srcDir,
                                const CString& outZipPath,
                                const CString& password,
                                CString& errMsg);

    // 把 zipPath 用 password 解壓到 outDir。會自動建立 outDir 與子資料夾。
    // 回傳實際解出來的檔案數量；失敗回 -1 並寫 errMsg。
    int ExtractZipWithPassword(const CString& zipPath,
                                const CString& outDir,
                                const CString& password,
                                CString& errMsg);

    // 用本機 curl.exe 把 localFile 上傳到 temp.sh。
    // 為什麼用 curl.exe 不用 WinHTTP：Windows 10 1803+ 內建 curl.exe，
    // 一行 process spawn 就解決 HTTPS / chunked / 大檔上傳，比自己寫 WinHTTP
    // (要處理 cert pinning / proxy / 100MB+ chunked) 簡單一個量級。
    //
    // 回傳 true=成功，並把 temp.sh 回傳的 URL (字串) 塞進 outUrl。
    // 失敗時 errMsg 會帶 curl 的 stderr 摘要。
    bool UploadToTempSh(const CString& localFile,
                        const CString& remoteFileName,
                        CString& outUrl,
                        CString& errMsg);

    // 用 curl.exe 從 url 下載到 localFile。
    // 為了避免 receiver 同時下載時撞名，建議 localFile 帶 random suffix。
    bool DownloadFile(const CString& url,
                        const CString& localFile,
                        CString& errMsg);

    // 產生一個 random 密碼 (length 個字元，A-Za-z0-9)。給 zip 加密用。
    CString GenerateRandomPassword(int length = 16);
}

#endif
