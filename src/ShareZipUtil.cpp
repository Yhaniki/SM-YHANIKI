#include "global.h"
#include "ShareZipUtil.h"
#include "RageLog.h"
#include "RageUtil.h" // ssprintf

#include "minizip/zip.h"
#include "minizip/unzip.h"
#include "zlib/zlib.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include <string>

// 連結 zlib (Steam SDK + zlib 都已經在 RageFileDriverZip.cpp 連過，
// 這裡為了讓 ShareZipUtil.cpp 自成一體再 pragma 一次也沒事，linker 會去重)
#pragma comment(lib, "zlib/zdll.lib")

namespace ShareZipUtil
{

// ============================================================================
// 內部工具
// ============================================================================

// 把 Windows 路徑統一成 zip 內部用的 forward-slash 格式
static std::string NormalizeZipPath(const std::string& p)
{
    std::string out = p;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == '\\') out[i] = '/';
    return out;
}

// 計算單一檔案的 CRC32 (zlib)。ZipCrypto 加密寫入時需要事先知道 CRC，
// 用來填寫 12-byte crypt header 的最後 1 byte (用來驗證密碼正確)。
// 兩遍讀檔在所難免，第一遍算 CRC，第二遍壓縮寫入。
static bool ComputeFileCrc32(const CString& absPath, uLong& outCrc)
{
    FILE* fp = fopen(absPath.c_str(), "rb");
    if (!fp) return false;

    uLong crc = crc32(0L, Z_NULL, 0);
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        crc = crc32(crc, buf, (uInt)n);
    fclose(fp);

    outCrc = crc;
    return true;
}

// 用 FindFirstFile 遞迴列出 srcDir 下所有「檔案」(不含 . / .. / 子資料夾本身)，
// 結果填到 outRelPaths 與 outAbsPaths，relPath 用 forward-slash。
static void EnumerateFilesRecursive(const CString& srcDir,
                                    const CString& subPath,
                                    std::vector<CString>& outRelPaths,
                                    std::vector<CString>& outAbsPaths)
{
    CString searchPath = srcDir;
    if (!subPath.empty())
        searchPath += "\\" + subPath;
    CString pattern = searchPath + "\\*";

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        CString itemRel = subPath.empty()
                          ? CString(fd.cFileName)
                          : (subPath + "\\" + fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            EnumerateFilesRecursive(srcDir, itemRel, outRelPaths, outAbsPaths);
        else
        {
            outRelPaths.push_back(itemRel);
            outAbsPaths.push_back(srcDir + "\\" + itemRel);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ============================================================================
// ZipFolderWithPassword
// ============================================================================
bool ZipFolderWithPassword(const CString& srcDir,
                           const CString& outZipPath,
                           const CString& password,
                           CString& errMsg)
{
    errMsg = "";

    std::vector<CString> rels, abss;
    EnumerateFilesRecursive(srcDir, "", rels, abss);
    if (rels.empty())
    {
        errMsg = "no files in srcDir: " + srcDir;
        return false;
    }

    // 用 zip64 (zipOpen64) 保險，避免 song dir > 4GB 時整包 zip 超過 32-bit offset
    zipFile zf = zipOpen64(outZipPath.c_str(), APPEND_STATUS_CREATE);
    if (zf == NULL)
    {
        errMsg = "zipOpen64 failed: " + outZipPath;
        return false;
    }

    const char* pwd = password.empty() ? NULL : password.c_str();
    bool ok = true;
    int filesAdded = 0;

    for (size_t i = 0; i < rels.size() && ok; ++i)
    {
        const CString& rel = rels[i];
        const CString& abs = abss[i];
        std::string zipName = NormalizeZipPath(rel.c_str());

        // 1) 算 CRC32 — ZipCrypto 加密寫 entry 時需要先知道 CRC
        //    (crypt header 最後 1 byte 等於 CRC 的最高 byte)
        uLong crc = 0;
        if (pwd != NULL)
        {
            if (!ComputeFileCrc32(abs, crc))
            {
                errMsg = "ComputeFileCrc32 failed: " + abs;
                ok = false;
                break;
            }
        }

        // zip_fileinfo 全 0 OK，minizip 會自己處理時間戳
        zip_fileinfo zi;
        memset(&zi, 0, sizeof(zi));

        // zipOpenNewFileInZip3_64 帶 password+crc
        //   method = Z_DEFLATED (壓縮)
        //   level = Z_DEFAULT_COMPRESSION (-1)
        //   raw = 0 (我們給原始資料，讓 minizip 壓)
        //   windowBits = -MAX_WBITS (raw deflate, zip 標準)
        //   memLevel / strategy 用 zlib 預設
        //   zip64 = 1 (大檔保險)
        int err = zipOpenNewFileInZip3_64(zf, zipName.c_str(), &zi,
                                          NULL, 0, NULL, 0, NULL,
                                          Z_DEFLATED, Z_DEFAULT_COMPRESSION,
                                          0,
                                          -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY,
                                          pwd, (uLong)crc,
                                          1);
        if (err != ZIP_OK)
        {
            errMsg = ssprintf("zipOpenNewFileInZip3_64 failed err=%d file=%s",
                              err, zipName.c_str());
            ok = false;
            break;
        }

        // 2) 第二遍讀檔，把內容餵給 minizip
        FILE* fp = fopen(abs.c_str(), "rb");
        if (!fp)
        {
            errMsg = "fopen failed: " + abs;
            zipCloseFileInZip(zf);
            ok = false;
            break;
        }
        unsigned char buf[64 * 1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        {
            err = zipWriteInFileInZip(zf, buf, (unsigned int)n);
            if (err != ZIP_OK)
            {
                errMsg = ssprintf("zipWriteInFileInZip err=%d file=%s",
                                  err, zipName.c_str());
                ok = false;
                break;
            }
        }
        fclose(fp);
        zipCloseFileInZip(zf);

        if (ok) ++filesAdded;

        // 每 32 個檔案 log 一次進度 (避免 log 灌爆)
        if ((filesAdded & 0x1F) == 0)
            LOG->Info("[ZIP] packing... %d/%u files", filesAdded, (unsigned)rels.size());
    }

    zipClose(zf, NULL);

    if (!ok)
    {
        // 部分失敗就直接刪掉殘檔
        DeleteFileA(outZipPath.c_str());
        return false;
    }

    LOG->Info("[ZIP] pack done: %d files -> '%s'", filesAdded, outZipPath.c_str());
    return true;
}

// ============================================================================
// ExtractZipWithPassword
// ============================================================================

// 在 outDir 下確保 zipFilename 所代表的「父資料夾」存在 (遞迴 mkdir)
static void EnsureParentDirs(const CString& outDir, const std::string& zipFilename)
{
    // zipFilename 用 '/'，這裡轉成 backslash 配合 Windows API
    std::string p = zipFilename;
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i] == '/') p[i] = '\\';

    // 找到最後一個 backslash，那之前就是父資料夾
    size_t lastBs = p.find_last_of('\\');
    if (lastBs == std::string::npos) return;

    CString sub = p.substr(0, lastBs).c_str();
    CString full = outDir + "\\" + sub;

    // 逐層 mkdir (CreateDirectoryA 多次也是無害)
    for (size_t i = 0; i < full.size(); ++i)
    {
        if (full[i] == '\\' && i > 2)  // i>2 跳過 "C:\"
        {
            CString partial = full.substr(0, i);
            CreateDirectoryA(partial.c_str(), NULL);
        }
    }
    CreateDirectoryA(full.c_str(), NULL);
}

int ExtractZipWithPassword(const CString& zipPath,
                           const CString& outDir,
                           const CString& password,
                           CString& errMsg)
{
    errMsg = "";

    unzFile uf = unzOpen64(zipPath.c_str());
    if (uf == NULL)
    {
        errMsg = "unzOpen64 failed: " + zipPath;
        return -1;
    }

    CreateDirectoryA(outDir.c_str(), NULL);

    const char* pwd = password.empty() ? NULL : password.c_str();
    int err = unzGoToFirstFile(uf);
    int count = 0;
    bool ok = true;
    char fname[1024];

    while (err == UNZ_OK && ok)
    {
        unz_file_info64 fi;
        memset(&fi, 0, sizeof(fi));
        memset(fname, 0, sizeof(fname));
        err = unzGetCurrentFileInfo64(uf, &fi, fname, sizeof(fname) - 1, NULL, 0, NULL, 0);
        if (err != UNZ_OK)
        {
            errMsg = ssprintf("unzGetCurrentFileInfo64 err=%d", err);
            ok = false;
            break;
        }

        // 跳過資料夾 entry (檔名以 / 結尾)
        size_t flen = strlen(fname);
        if (flen == 0)
        {
            err = unzGoToNextFile(uf);
            continue;
        }
        if (fname[flen - 1] == '/' || fname[flen - 1] == '\\')
        {
            err = unzGoToNextFile(uf);
            continue;
        }

        EnsureParentDirs(outDir, fname);

        err = unzOpenCurrentFilePassword(uf, pwd);
        if (err != UNZ_OK)
        {
            // 密碼錯 / 壞檔都會到這裡 (err 通常 = UNZ_CRCERROR 或 UNZ_BADZIPFILE)
            errMsg = ssprintf("unzOpenCurrentFilePassword err=%d file=%s (password 不對？)",
                              err, fname);
            ok = false;
            break;
        }

        // 把 zip 內的 forward-slash 轉回 backslash 來組 Windows 路徑
        std::string fnWin = fname;
        for (size_t i = 0; i < fnWin.size(); ++i)
            if (fnWin[i] == '/') fnWin[i] = '\\';
        CString outPath = outDir + "\\" + fnWin.c_str();

        FILE* fp = fopen(outPath.c_str(), "wb");
        if (!fp)
        {
            errMsg = "fopen for write failed: " + outPath;
            unzCloseCurrentFile(uf);
            ok = false;
            break;
        }

        unsigned char buf[64 * 1024];
        int n;
        while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0)
        {
            if (fwrite(buf, 1, n, fp) != (size_t)n)
            {
                errMsg = "fwrite failed: " + outPath;
                ok = false;
                break;
            }
        }
        fclose(fp);
        if (n < 0)
        {
            errMsg = ssprintf("unzReadCurrentFile err=%d (file=%s) - 可能是 CRC 不對 / 密碼錯",
                              n, fname);
            ok = false;
        }

        unzCloseCurrentFile(uf);
        if (!ok) break;
        ++count;

        if ((count & 0x1F) == 0)
            LOG->Info("[UNZIP] extracting... %d files", count);

        err = unzGoToNextFile(uf);
    }

    unzClose(uf);

    if (!ok) return -1;
    LOG->Info("[UNZIP] extract done: %d files -> '%s'", count, outDir.c_str());
    return count;
}

// ============================================================================
// HTTP 透過 curl.exe shell-out
// ============================================================================
//
// 為什麼 shell-out 不直接 WinHTTP：
//   * Windows 10 1803+ 內建 curl.exe (C:\Windows\System32\curl.exe)
//   * 一行 CreateProcess + 等 stdout 就解決 HTTPS / chunked / 大檔上傳/下載
//   * WinHTTP 寫得對要處理 cert / proxy / 100MB+ chunk 自製 buffer 還 callback
//   * 出問題 user 可以直接複製命令列在 cmd 跑一次重現
// ============================================================================

// 把命令 cmd 用 CreateProcess 啟動，stdout/stderr 都接到一個 pipe；
// 等 process 結束後把整段 stdout 字串塞進 outStdout，回傳 process exit code。
// 失敗回 -1 並 outStdout 留空。
static int RunCommandCaptureStdout(const CString& cmd, CString& outStdout)
{
    outStdout = "";

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;

    // CreateProcess 會修改 cmdline buffer，所以要 strdup 一份
    std::vector<char> cmdBuf(cmd.size() + 1);
    memcpy(cmdBuf.data(), cmd.c_str(), cmd.size() + 1);

    BOOL ok = CreateProcessA(NULL, cmdBuf.data(),
                             NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL,
                             &si, &pi);
    // 父行程不再寫 pipe，要先關掉 write 端，不然 ReadFile 永遠等不到 EOF
    CloseHandle(hWrite);

    if (!ok)
    {
        CloseHandle(hRead);
        return -1;
    }

    // 從 pipe 持續讀，直到 child 寫完關閉 (這時 ReadFile 會回 FALSE)
    char buf[4096];
    DWORD got = 0;
    std::string acc;
    while (ReadFile(hRead, buf, sizeof(buf), &got, NULL) && got > 0)
        acc.append(buf, got);
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    outStdout = acc.c_str();
    return (int)exitCode;
}

bool UploadToTempSh(const CString& localFile,
                    const CString& remoteFileName,
                    CString& outUrl,
                    CString& errMsg)
{
    outUrl = "";
    errMsg = "";

    // curl -k -s -T "<localFile>" https://temp.sh/<remoteFileName>
    //   -k : 不驗 cert (temp.sh 走 LetsEncrypt 一般不會出事，加 -k 保險)
    //   -s : silent，不要 progress bar 塞滿 stdout
    //   -T : upload (PUT)
    //   stdout 會印出 temp.sh 回傳的 URL，如：https://temp.sh/abcd1234/foo.zip
    CString cmd = "curl.exe -k -s -T \"" + localFile + "\" "
                 "https://temp.sh/" + remoteFileName;

    LOG->Info("[HTTP] upload: %s", cmd.c_str());

    CString stdoutStr;
    int rc = RunCommandCaptureStdout(cmd, stdoutStr);
    if (rc != 0)
    {
        errMsg = ssprintf("curl upload failed rc=%d output=%s", rc, stdoutStr.c_str());
        return false;
    }

    // temp.sh 正常回應就是一行 URL，可能尾巴有 \n / \r
    std::string s = stdoutStr.c_str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    if (s.find("https://") != 0 && s.find("http://") != 0)
    {
        errMsg = ssprintf("temp.sh response not a URL: '%s'", s.c_str());
        return false;
    }
    outUrl = s.c_str();
    LOG->Info("[HTTP] upload OK: url='%s'", outUrl.c_str());
    return true;
}

bool DownloadFile(const CString& url,
                  const CString& localFile,
                  CString& errMsg)
{
    errMsg = "";

    // curl -k -s -L -o "<localFile>" "<url>"
    //   -L : follow redirect (temp.sh 有時會 302 到 CDN)
    //   -o : output file
    CString cmd = "curl.exe -k -s -L -o \"" + localFile + "\" \"" + url + "\"";

    LOG->Info("[HTTP] download: %s", cmd.c_str());

    CString stdoutStr;
    int rc = RunCommandCaptureStdout(cmd, stdoutStr);
    if (rc != 0)
    {
        errMsg = ssprintf("curl download failed rc=%d output=%s", rc, stdoutStr.c_str());
        return false;
    }
    LOG->Info("[HTTP] download OK: '%s'", localFile.c_str());
    return true;
}

CString GenerateRandomPassword(int length)
{
    static const char kAlphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    static const int kAlphaSize = sizeof(kAlphabet) - 1;

    // 每次呼叫前用「現在時間 + 上次累積」當 seed，盡量讓不同次 share 拿到不同密碼。
    // ZipCrypto 本來就弱，password entropy 不用做到 crypto-grade。
    static unsigned int seed_acc = 0;
    seed_acc += (unsigned int)time(NULL);
    seed_acc ^= (unsigned int)GetTickCount();
    srand(seed_acc);

    std::string s;
    s.resize(length);
    for (int i = 0; i < length; ++i)
        s[i] = kAlphabet[rand() % kAlphaSize];
    return s.c_str();
}

} // namespace ShareZipUtil
