#pragma once

#include "lsp/Json.h"
#include "process/ProcessRunner.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

struct BaaSymbolRequest
{
    std::uint64_t token{};
    std::string uri;
    std::string filePath;
    std::string text;
    int version{};

    bool isValid() const { return token != 0 and not uri.empty() and not filePath.empty(); }
};

struct BaaSymbolResult
{
    std::uint64_t token{};
    std::string uri;
    std::string text;
    int version{};
    int exitCode{-1};
    Json symbols = Json::array();
    std::string errorMessage;
};

struct BaaCompletionDataResult
{
    int exitCode{-1};
    Json items = Json::array();
    std::string errorMessage;
};

struct BaaFormatRequest
{
    std::uint64_t token{};
    std::string uri;
    std::string filePath;
    std::string text;
    int version{};

    bool isValid() const
    {
        return token != 0 and not uri.empty() and not filePath.empty();
    }
};

struct BaaFormatResult
{
    std::uint64_t token{};
    std::string uri;
    std::string text;
    std::string formattedText;
    int version{};
    int exitCode{-1};
    bool changed{};
    std::string errorMessage;
};

struct BaaSemanticRequest
{
    struct ProjectSource
    {
        std::string uri;
        std::string filePath;
        std::string text;
        int version{};
        bool useStandardInput{};
    };

    std::uint64_t token{};
    std::string uri;
    std::string filePath;
    std::string text;
    int version{};
    std::size_t positionByte{};
    std::filesystem::path projectWorkingDirectory;
    std::vector<std::string> includePaths;
    std::vector<ProjectSource> projectSources;

    bool isValid() const { return token != 0 and not uri.empty() and not filePath.empty(); }
};

struct BaaSemanticResult
{
    std::uint64_t token{};
    std::string uri;
    std::string text;
    int version{};
    std::size_t positionByte{};
    int exitCode{-1};
    Json hover = nullptr;
    Json signatureHelp = nullptr;
    Json definition = nullptr;
    Json references = Json::array();
    Json symbol = nullptr;
    Json projectOccurrences = Json::array();
    Json projectIndexOccurrences = Json::array();
    bool projectIndexComplete{true};
    std::vector<std::string> warnings;
    std::string errorMessage;
};

class BaaCompilerBridge
{
public:
    using AnalysisCallback = std::function<void(BaaAnalysisResult)>;
    using SymbolCallback = std::function<void(BaaSymbolResult)>;
    using CompletionDataCallback = std::function<void(BaaCompletionDataResult)>;
    using FormatCallback = std::function<void(BaaFormatResult)>;
    using SemanticCallback = std::function<void(BaaSemanticResult)>;

    BaaCompilerBridge();
    ~BaaCompilerBridge();

    BaaCompilerBridge(const BaaCompilerBridge &) = delete;
    BaaCompilerBridge &operator=(const BaaCompilerBridge &) = delete;

    void setCompilerProgram(std::string program);
    void setApplicationDirectory(std::filesystem::path directory);
    void setDebounceInterval(int milliseconds);
    void setAnalysisCallback(AnalysisCallback callback);
    void setSymbolCallback(SymbolCallback callback);
    void setCompletionDataCallback(CompletionDataCallback callback);
    void setFormatCallback(FormatCallback callback);
    void setSemanticCallback(SemanticCallback callback);
    void schedule(BaaAnalysisRequest request);
    void requestSymbols(BaaSymbolRequest request);
    void requestCompletionData();
    void requestFormat(BaaFormatRequest request);
    void requestSemantic(BaaSemanticRequest request);
    void cancelSymbols(std::uint64_t token);
    void cancelFormat(std::uint64_t token);
    void cancelSemantic(std::uint64_t token);
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
    std::deque<BaaSymbolRequest> m_pendingSymbols;
    std::deque<BaaFormatRequest> m_pendingFormats;
    std::deque<BaaSemanticRequest> m_pendingSemantic;
    bool m_completionDataPending{};
    std::unordered_map<std::string, int> m_latestVersions;
    std::string m_activeUri;
    int m_activeVersion{};
    std::uint64_t m_activeSymbolToken{};
    std::uint64_t m_activeFormatToken{};
    std::uint64_t m_activeSemanticToken{};
    bool m_completionDataActive{};
    std::uint64_t m_scheduleSerial{};
    AnalysisCallback m_callback;
    SymbolCallback m_symbolCallback;
    CompletionDataCallback m_completionDataCallback;
    FormatCallback m_formatCallback;
    SemanticCallback m_semanticCallback;
    bool m_stopping{};
    ProcessRunner m_runner;
    std::thread m_worker;
};
