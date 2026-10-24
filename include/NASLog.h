#pragma once
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <chrono>
#include <ctime>

namespace Pulsar
{
    class NASLog
    {
    public:
        static NASLog& Get()
        {
            static NASLog instance;
            return instance;
        }

        void Init(const char* a_role)
        {
#ifndef PULSAR_HARDENED
            std::lock_guard lock(m_mutex);
            try {
                char path[512];
                snprintf(path, sizeof(path), "<NAS>\\%s.log", a_role);
                errno_t err = fopen_s(&m_file, path, "a");
                if (err != 0 || !m_file) {
                    m_file = nullptr; // NAS not reachable, just skip
                    return;
                }
                Write("=== PULSAR %s started ===", a_role);
            } catch (...) {
                m_file = nullptr; // Silently fail
            }
#else
            (void)a_role;   // hardened release: no NAS logging, no path/started literal
#endif
        }

        void Write(const char* a_fmt, ...)
        {
            std::lock_guard lock(m_mutex);
            if (!m_file) return;

            // Timestamp
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            struct tm tm_buf;
            localtime_s(&tm_buf, &time);
            fprintf(m_file, "[%02d:%02d:%02d.%03d] ",
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms.count());

            va_list args;
            va_start(args, a_fmt);
            vfprintf(m_file, a_fmt, args);
            va_end(args);

            fprintf(m_file, "\n");
            fflush(m_file);
        }

        ~NASLog()
        {
            if (m_file) fclose(m_file);
        }

    private:
        NASLog() = default;
        FILE* m_file = nullptr;
        std::mutex m_mutex;
    };
}

// Convenience macro. Compiled OUT in the hardened release: NAS logging is a dev-only
// diagnostic channel (logs land on the dev <NAS>), and the format literals would
// otherwise leak into the shipped binary. See include/Log.h.
#ifdef PULSAR_HARDENED
#  define NASLOG(fmt, ...) ((void)0)
#else
#  define NASLOG(fmt, ...) Pulsar::NASLog::Get().Write(fmt, ##__VA_ARGS__)
#endif
