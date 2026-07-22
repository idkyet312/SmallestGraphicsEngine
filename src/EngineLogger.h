#ifndef ENGINE_LOGGER_H
#define ENGINE_LOGGER_H

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>

namespace EngineLog {

enum class Level { Verbose, Display, Warning, Error, Fatal };

inline const char* LevelName(Level level) {
    switch (level) {
    case Level::Verbose: return "Verbose";
    case Level::Display: return "Display";
    case Level::Warning: return "Warning";
    case Level::Error:   return "Error";
    case Level::Fatal:   return "Fatal";
    }
    return "Display";
}

class Logger;

class LineStreamBuffer final : public std::streambuf {
public:
    LineStreamBuffer(Logger& logger, std::streambuf* console,
                     std::string category, Level level)
        : logger_(logger), console_(console), category_(std::move(category)),
          level_(level) {}

    void FlushPending();

protected:
    int_type overflow(int_type ch) override;
    std::streamsize xsputn(const char* data, std::streamsize count) override;
    int sync() override;

private:
    void WriteUnlocked(const char* data, std::streamsize count);

    Logger& logger_;
    std::streambuf* console_ = nullptr;
    std::string category_;
    Level level_ = Level::Display;
    std::string pending_;
};

class Logger final {
public:
    static Logger& Instance() {
        static Logger logger;
        return logger;
    }

    bool Initialize(const std::string& application = "GraphicEngine") {
        if (active_) return true;

        std::error_code error;
        const std::filesystem::path directory = "logs";
        std::filesystem::create_directories(directory, error);
        if (error) return false;

        application_ = application;
        currentPath_ = directory / (application_ + ".log");
        if (std::filesystem::exists(currentPath_, error) && !error) {
            std::filesystem::path backup = directory /
                (application_ + "-backup-" + FileTimestamp() + ".log");
            for (unsigned suffix = 1; std::filesystem::exists(backup, error) &&
                 !error; ++suffix) {
                backup = directory / (application_ + "-backup-" +
                    FileTimestamp() + "-" + std::to_string(suffix) + ".log");
            }
            error.clear();
            std::filesystem::rename(currentPath_, backup, error);
        }

        file_.open(currentPath_, std::ios::out | std::ios::trunc);
        if (!file_) return false;

        originalOut_ = std::cout.rdbuf();
        originalErr_ = std::cerr.rdbuf();
        outBuffer_ = std::make_unique<LineStreamBuffer>(
            *this, originalOut_, "LogConsole", Level::Display);
        errBuffer_ = std::make_unique<LineStreamBuffer>(
            *this, originalErr_, "LogConsole", Level::Error);
        active_ = true;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            WriteLineUnlocked("LogInit", Level::Display,
                              "Log file open: " + currentPath_.string());
            WriteLineUnlocked("LogInit", Level::Display,
                              "Process ID: " + std::to_string(GetCurrentProcessId()));
#ifdef _DEBUG
            WriteLineUnlocked("LogInit", Level::Display, "Build: Debug");
#else
            WriteLineUnlocked("LogInit", Level::Display, "Build: Release");
#endif
            WriteLineUnlocked("LogInit", Level::Display,
                              "Working directory: " +
                              std::filesystem::current_path(error).string());
        }

        std::cout.rdbuf(outBuffer_.get());
        std::cerr.rdbuf(errBuffer_.get());
        return true;
    }

    void Shutdown() {
        if (!active_) return;

        outBuffer_->FlushPending();
        errBuffer_->FlushPending();
        std::cout.rdbuf(originalOut_);
        std::cerr.rdbuf(originalErr_);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            WriteLineUnlocked("LogExit", Level::Display, "Log closed cleanly");
            file_.flush();
            file_.close();
            active_ = false;
        }
        outBuffer_.reset();
        errBuffer_.reset();
    }

    void Write(const std::string& category, Level level,
               const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (originalOut_) {
            std::ostringstream consoleLine;
            consoleLine << category << ": " << LevelName(level) << ": "
                        << message << '\n';
            const std::string text = consoleLine.str();
            originalOut_->sputn(text.data(), static_cast<std::streamsize>(text.size()));
            originalOut_->pubsync();
        }
        WriteLineUnlocked(category, level, message);
    }

    bool IsActive() const { return active_; }
    const std::filesystem::path& CurrentPath() const { return currentPath_; }

private:
    friend class LineStreamBuffer;

    Logger() = default;
    ~Logger() { Shutdown(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static std::tm LocalTime(std::time_t value) {
        std::tm result = {};
        localtime_s(&result, &value);
        return result;
    }

    static std::string FileTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t value = std::chrono::system_clock::to_time_t(now);
        const std::tm local = LocalTime(value);
        std::ostringstream stream;
        stream << std::put_time(&local, "%Y.%m.%d-%H.%M.%S");
        return stream.str();
    }

    static std::string LineTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t value = std::chrono::system_clock::to_time_t(now);
        const std::tm local = LocalTime(value);
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::ostringstream stream;
        stream << std::put_time(&local, "%Y.%m.%d-%H.%M.%S") << ':'
               << std::setfill('0') << std::setw(3) << milliseconds.count();
        return stream.str();
    }

    void WriteLineUnlocked(const std::string& category, Level level,
                           const std::string& message) {
        if (!file_) return;
        file_ << '[' << LineTimestamp() << "]["
              << std::setfill(' ') << std::setw(6) << GetCurrentThreadId() << ']'
              << category << ": " << LevelName(level) << ": " << message << '\n';
        file_.flush();
    }

    std::mutex mutex_;
    std::ofstream file_;
    std::filesystem::path currentPath_;
    std::string application_;
    std::streambuf* originalOut_ = nullptr;
    std::streambuf* originalErr_ = nullptr;
    std::unique_ptr<LineStreamBuffer> outBuffer_;
    std::unique_ptr<LineStreamBuffer> errBuffer_;
    bool active_ = false;
};

inline void LineStreamBuffer::WriteUnlocked(const char* data,
                                             std::streamsize count) {
    if (console_ && count > 0) console_->sputn(data, count);
    for (std::streamsize i = 0; i < count; ++i) {
        const char ch = data[i];
        if (ch == '\n') {
            if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
            logger_.WriteLineUnlocked(category_, level_, pending_);
            pending_.clear();
        } else {
            pending_.push_back(ch);
        }
    }
}

inline LineStreamBuffer::int_type LineStreamBuffer::overflow(int_type ch) {
    if (traits_type::eq_int_type(ch, traits_type::eof()))
        return traits_type::not_eof(ch);
    const char value = traits_type::to_char_type(ch);
    std::lock_guard<std::mutex> lock(logger_.mutex_);
    WriteUnlocked(&value, 1);
    return ch;
}

inline std::streamsize LineStreamBuffer::xsputn(const char* data,
                                                 std::streamsize count) {
    std::lock_guard<std::mutex> lock(logger_.mutex_);
    WriteUnlocked(data, count);
    return count;
}

inline int LineStreamBuffer::sync() {
    std::lock_guard<std::mutex> lock(logger_.mutex_);
    return console_ ? console_->pubsync() : 0;
}

inline void LineStreamBuffer::FlushPending() {
    std::lock_guard<std::mutex> lock(logger_.mutex_);
    if (!pending_.empty()) {
        logger_.WriteLineUnlocked(category_, level_, pending_);
        pending_.clear();
    }
    if (console_) console_->pubsync();
}

class ScopedSession final {
public:
    explicit ScopedSession(const std::string& application = "GraphicEngine") {
        Logger::Instance().Initialize(application);
    }
    ~ScopedSession() { Logger::Instance().Shutdown(); }
    ScopedSession(const ScopedSession&) = delete;
    ScopedSession& operator=(const ScopedSession&) = delete;
};

inline void Write(const std::string& category, Level level,
                  const std::string& message) {
    Logger::Instance().Write(category, level, message);
}

} // namespace EngineLog

#define SGE_LOG(Category, LevelValue, Message) \
    ::EngineLog::Write((Category), (LevelValue), (Message))

#endif // ENGINE_LOGGER_H
