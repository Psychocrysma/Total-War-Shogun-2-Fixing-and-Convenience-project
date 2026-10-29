#include "common.h"
#include <cstdarg>
#include <mutex>

static FILE* g_log = nullptr;
static std::mutex g_logMutex;

void LogOpen(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_log) return;
    g_log = _wfopen(path.c_str(), L"w");
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void LogClose() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_log) { fclose(g_log); g_log = nullptr; }
}
