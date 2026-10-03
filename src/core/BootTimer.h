#pragma once

// Boot progress and timing. Every line is "[boot +12.34s] ..." where the time
// is measured from process creation, so it includes everything before main.
// Printed to stdout (the console window) and appended to boot.log next to the
// working directory, truncated once per run.
//
//   BootTimer::Log("Loading fonts");
//   { BootTimer::Scope step("Visibility buffer init"); ... }  // logs its ms
//
// Shader compiles report through ShaderCacheDX12 (HIT / COMPILE lines).

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace BootTimer {

inline double SecondsSinceProcessStart() {
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user);
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = creation.dwLowDateTime; a.HighPart = creation.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;      b.HighPart = now.dwHighDateTime;
    return static_cast<double>(b.QuadPart - a.QuadPart) / 1.0e7;
}

inline double MillisecondsNow() {
    static const double frequency = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / frequency;
}

inline std::mutex& LogMutex() { static std::mutex m; return m; }

inline void Log(const std::string& line) {
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "[boot +%6.2fs] ",
                  SecondsSinceProcessStart());
    std::lock_guard<std::mutex> lock(LogMutex());
    std::cout << prefix << line << std::endl;
    static std::ofstream file("boot.log", std::ios::trunc);
    if (file) file << prefix << line << std::endl;
}

// Logs "name..." on entry and "name: N ms" on exit.
struct Scope {
    std::string name;
    double start;
    explicit Scope(std::string stepName)
        : name(std::move(stepName)), start(MillisecondsNow()) {
        Log(name + "...");
    }
    ~Scope() {
        char text[64];
        std::snprintf(text, sizeof(text), ": %.0f ms",
                      MillisecondsNow() - start);
        Log(name + text);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace BootTimer
