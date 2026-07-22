#pragma once

#include "compiler/BaaCompilerBridge.h"
#include "server/DocumentStore.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

class BaaLanguageServer
{
public:
    using MessageCallback = std::function<void(std::string)>;
    using ExitCallback = std::function<void(int)>;

    BaaLanguageServer();
    ~BaaLanguageServer();

    void setCompilerProgram(std::string program);
    void setApplicationDirectory(std::filesystem::path directory);
    void setAnalysisDebounceInterval(int milliseconds);
    void setMessageCallback(MessageCallback callback);
    void setExitCallback(ExitCallback callback);
    void receiveMessage(std::string_view jsonBody);

private:
    void handleRequest(const Json &message);
    void handleNotification(const Json &message);
    void handleDidOpen(const Json &params);
    void handleDidChange(const Json &params);
    void handleDidSave(const Json &params);
    void handleDidClose(const Json &params);

    void analyze(const BaaDocument &document);
    void onAnalysisFinished(BaaAnalysisResult result);
    void publishDiagnostics(const std::string &uri, int version, const Json &diagnostics);
    void sendLogMessage(const std::string &message, int type = 1);
    void sendResult(const Json &id, const Json &result);
    void sendError(const Json &id, int code, const std::string &message);
    void sendJson(Json message);

    std::mutex m_documentsMutex;
    DocumentStore m_documents;
    BaaCompilerBridge m_compiler;
    std::mutex m_callbackMutex;
    MessageCallback m_messageCallback;
    ExitCallback m_exitCallback;
    bool m_initializeResponded{};
    bool m_initialized{};
    bool m_shutdownRequested{};
};
