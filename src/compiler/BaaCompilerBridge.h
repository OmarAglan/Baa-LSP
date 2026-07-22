#pragma once

#include "lsp/Json.h"
#include "process/ProcessRunner.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

struct BaaAnalysisRequest
{
    std::string uri;
    std::string filePath;
    std::string text;
    int version{};

    bool isValid() const { return not uri.empty() and not filePath.empty(); }
};

struct BaaAnalysisResult
{
    std::string uri;
    std::string filePath;
    std::string text;
    int version{};
    int exitCode{-1};
    Json diagnostics = Json::array();
    std::string errorMessage;
};

class BaaCompilerBridge
{
public:
    using AnalysisCallback = std::function<void(BaaAnalysisResult)>;

    BaaCompilerBridge();
    ~BaaCompilerBridge();

    BaaCompilerBridge(const BaaCompilerBridge &) = delete;
    BaaCompilerBridge &operator=(const BaaCompilerBridge &) = delete;

    void setCompilerProgram(std::string program);
    void setApplicationDirectory(std::filesystem::path directory);
    void setDebounceInterval(int milliseconds);
    void setAnalysisCallback(AnalysisCallback callback);
    void schedule(BaaAnalysisRequest request);
    void cancel(const std::string &uri);
    void cancelAll();

private:
    std::string resolveCompilerProgram() const;
    void workerLoop();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::string m_compilerProgram;
    std::filesystem::path m_applicationDirectory;
    std::chrono::milliseconds m_debounce{250};
    std::unordered_map<std::string, BaaAnalysisRequest> m_pending;
    std::unordered_map<std::string, int> m_latestVersions;
    std::string m_activeUri;
    std::uint64_t m_scheduleSerial{};
    AnalysisCallback m_callback;
    bool m_stopping{};
    ProcessRunner m_runner;
    std::thread m_worker;
};
