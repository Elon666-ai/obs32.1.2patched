#include "utils.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <cstdarg>
#include <cstring>
#include <ctime>

// ── Plugin file logger state ──────────────────────────────────────

namespace {
    std::mutex   g_logMutex;
    FILE*        g_logFile = nullptr;
    char         g_logDir[MAX_PATH] = {0};
    char         g_logDate[16] = {0};   // "YYYYMMDD" of current log file

    /// Get executable directory path (portable-safe).
    /// Returns the directory containing the running obs64.exe.
    std::string GetExeDir() {
#ifdef _WIN32
        wchar_t exePathW[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return ".";
        // Convert to UTF-8
        char exePath[MAX_PATH * 4];
        WideCharToMultiByte(CP_UTF8, 0, exePathW, -1, exePath, sizeof(exePath), nullptr, nullptr);
        // Strip filename, keep directory
        char* lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) lastSlash = strrchr(exePath, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(exePath);
#else
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0) return ".";
        buf[len] = '\0';
        char* lastSlash = strrchr(buf, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(buf);
#endif
    }

    /// Create directory recursively (Windows-safe).
    bool MkdirP(const std::string& path) {
#ifdef _WIN32
        // Convert to wide string for Windows API
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return false;
        std::wstring wpath(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
        // SHCreateDirectoryEx or recursive CreateDirectory
        // Build path incrementally
        std::wstring current;
        for (size_t i = 0; i < wpath.size(); ++i) {
            if (wpath[i] == L'\\' || wpath[i] == L'/') {
                if (!current.empty()) {
                    CreateDirectoryW(current.c_str(), nullptr);
                }
            }
            current += wpath[i];
        }
        if (!current.empty())
            CreateDirectoryW(current.c_str(), nullptr);
        return true;
#else
        std::string current;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '/') {
                if (!current.empty())
                    mkdir(current.c_str(), 0755);
            }
            current += path[i];
        }
        if (!current.empty())
            mkdir(current.c_str(), 0755);
        return true;
#endif
    }

    /// Open the log file for today's date, closing previous if date changed.
    void EnsureLogFileOpen() {
        // Get today's date string
        time_t now = time(nullptr);
        struct tm local;
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char todayDate[16];
        strftime(todayDate, sizeof(todayDate), "%Y%m%d", &local);

        // If date hasn't changed and file is open, reuse
        if (g_logFile && strcmp(g_logDate, todayDate) == 0)
            return;

        // Close old file if open
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }

        // Update date
        strncpy(g_logDate, todayDate, sizeof(g_logDate) - 1);
        g_logDate[sizeof(g_logDate) - 1] = '\0';

        // Build file path: <exe_dir>/logs/obs-multipusher-YYYYMMDD.log
        char filePath[MAX_PATH * 2];
        snprintf(filePath, sizeof(filePath), "%s\\obs-multipusher-%s.log",
                 g_logDir, todayDate);

        g_logFile = fopen(filePath, "a");
    }

} // anonymous namespace

// ── Public API ─────────────────────────────────────────────────────

namespace Utils {

void InitPluginLogger() {
    std::lock_guard<std::mutex> lock(g_logMutex);

    std::string exeDir = GetExeDir();
    std::string logDir = exeDir + "\\logs";

    // Store log directory
    strncpy(g_logDir, logDir.c_str(), sizeof(g_logDir) - 1);
    g_logDir[sizeof(g_logDir) - 1] = '\0';

    // Create logs/ directory
    MkdirP(logDir);

    // Open initial log file
    EnsureLogFileOpen();

    if (g_logFile) {
        // Write a startup marker
        time_t now = time(nullptr);
        struct tm local;
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &local);
        fprintf(g_logFile, "=== obs-multipusher v1.1.5 log started at %s ===\n", timeBuf);
        fflush(g_logFile);
    }
}

void PluginFileLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) return;

    // Ensure correct date file
    EnsureLogFileOpen();
    if (!g_logFile) return;

    // Timestamp: HH:MM:SS
    time_t now = time(nullptr);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char timeBuf[16];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &local);

    // Write timestamp
    fprintf(g_logFile, "%s ", timeBuf);

    // Write formatted message
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);

    // Ensure newline
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

void ShutdownPluginLogger() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        time_t now = time(nullptr);
        struct tm local;
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &local);
        fprintf(g_logFile, "=== obs-multipusher log ended at %s ===\n\n", timeBuf);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

std::string GetPluginLogDir() {
    return std::string(g_logDir);
}

} // namespace Utils
