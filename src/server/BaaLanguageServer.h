#pragma once

#include "compiler/BaaCompilerBridge.h"
#include "server/DocumentStore.h"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class BaaLanguageServer
{
public:
    using MessageCallback = std::function<void(std::string)>;
    using ExitCallback = std::function<void(int)>;

    BaaLanguageServer();
    ~BaaLanguageServer();

    void setCompilerProgram(std::string program);
    void setTakweenProgram(std::string program);
    void setApplicationDirectory(std::filesystem::path directory);
    void setAnalysisDebounceInterval(int milliseconds);
    void setMessageCallback(MessageCallback callback);
    void setExitCallback(ExitCallback callback);
    void receiveMessage(std::string_view jsonBody);

private:
    enum class SemanticReplyKind
    {
        Completion,
        Hover,
        SignatureHelp,
        Definition,
        References,
        PrepareRename,
        Rename
    };

    enum class StructureReplyKind
    {
        Folding,
        Selection
    };

    void handleRequest(const Json &message);
    void handleNotification(const Json &message);
    void handleDidOpen(const Json &params);
    void handleDidChange(const Json &params);
    void handleDidSave(const Json &params);
    void handleDidClose(const Json &params);
    void handleDocumentSymbol(const Json &id, const Json &params);
    void handleSemanticTokensFull(const Json &id, const Json &params);
    void handleFoldingRange(const Json &id, const Json &params);
    void handleSelectionRange(const Json &id, const Json &params);
    void handleStructureRequest(const Json &id, const Json &params,
                                StructureReplyKind kind);
    void handleWorkspaceSymbol(const Json &id, const Json &params);
    void handleCompletion(const Json &id, const Json &params);
    void handleCompletionResolve(const Json &id, const Json &item);
    void handleCodeAction(const Json &id, const Json &params);
    void handleDocumentFormatting(const Json &id, const Json &params);
    void handleSemanticRequest(const Json &id,
                               const Json &params,
                               SemanticReplyKind kind);
    void handleCancelRequest(const Json &params);
    void loadProjectPlan(const Json &initializeParams);

    void analyze(const BaaDocument &document);
    void onAnalysisFinished(BaaAnalysisResult result);
    void onSymbolsFinished(BaaSymbolResult result);
    void onTokensFinished(BaaTokenResult result);
    void onStructureFinished(BaaStructureResult result);
    void onCompletionDataFinished(BaaCompletionDataResult result);
    void onFormatFinished(BaaFormatResult result);
    void onSemanticFinished(BaaSemanticResult result);
    void requestSymbolsForDocument(const BaaDocument &document, const Json *requestId);
    void requestSymbols(std::string uri,
                        std::string filePath,
                        std::string text,
                        int version,
                        bool requireOpenDocument,
                        bool workspaceIndex,
                        const Json *requestId = nullptr);
    void resolveWorkspaceSymbolRequests();
    Json workspaceSymbolResult(const std::string &query);
    void completeRequest(const Json &id, const std::string &uri, int version,
                         int line, int character);
    void invalidateSymbolRequests(const std::string &uri, int code,
                                  const std::string &message);
    void invalidateTokenRequests(const std::string &uri, int code,
                                 const std::string &message);
    void invalidateStructureRequests(const std::string &uri, int code,
                                     const std::string &message);
    void invalidateWorkspaceSymbolRequests(int code,
                                           const std::string &message);
    void invalidateCompletionRequests(const std::string &uri, int code,
                                      const std::string &message);
    void invalidateFormatRequests(const std::string &uri, int code,
                                  const std::string &message);
    void invalidateSemanticRequests(const std::string &uri, int code,
                                    const std::string &message);
    void publishDiagnostics(const std::string &uri, int version, const Json &diagnostics);
    void sendLogMessage(const std::string &message, int type = 1);
    void sendResult(const Json &id, const Json &result);
    void sendError(const Json &id, int code, const std::string &message);
    void sendJson(Json message);

    std::mutex m_documentsMutex;
    DocumentStore m_documents;
    BaaCompilerBridge m_compiler;
    struct PublishedDiagnostics
    {
        int version{};
        Json diagnostics = Json::array();
    };
    std::mutex m_diagnosticsMutex;
    std::unordered_map<std::string, PublishedDiagnostics> m_publishedDiagnostics;
    struct PendingSymbolRequest
    {
        std::vector<Json> ids;
        std::string uri;
        int version{};
        bool requireOpenDocument{true};
        bool workspaceIndex{};
    };
    struct CachedSymbols
    {
        int version{};
        std::string text;
        Json symbols = Json::array();
    };
    std::mutex m_symbolRequestsMutex;
    std::unordered_map<std::uint64_t, PendingSymbolRequest> m_symbolRequests;
    std::unordered_map<std::string, CachedSymbols> m_symbolCache;
    std::uint64_t m_nextSymbolToken{1};
    struct PendingTokenRequest
    {
        std::vector<Json> ids;
        std::string uri;
        int version{};
    };
    struct CachedTokens
    {
        int version{};
        std::string text;
        Json tokens = Json::array();
    };
    std::mutex m_tokenRequestsMutex;
    std::unordered_map<std::uint64_t, PendingTokenRequest> m_tokenRequests;
    std::unordered_map<std::string, CachedTokens> m_tokenCache;
    std::uint64_t m_nextTokenToken{1};
    struct PendingStructureReply
    {
        Json id;
        StructureReplyKind kind{StructureReplyKind::Folding};
        Json positions = Json::array();
    };
    struct PendingStructureRequest
    {
        std::vector<PendingStructureReply> replies;
        std::string uri;
        int version{};
    };
    struct CachedStructure
    {
        int version{};
        std::string text;
        bool complete{};
        Json foldingRanges = Json::array();
        Json selectionRanges = Json::array();
    };
    std::mutex m_structureMutex;
    std::unordered_map<std::uint64_t, PendingStructureRequest>
        m_structureRequests;
    std::unordered_map<std::string, CachedStructure> m_structureCache;
    std::uint64_t m_nextStructureToken{1};
    struct WorkspaceSymbolIndexEntry
    {
        int version{};
        bool openDocument{};
        Json symbols = Json::array();
        std::string errorMessage;
    };
    struct WorkspaceSymbolSourceVersion
    {
        std::string uri;
        int version{};
        bool openDocument{};
    };
    struct PendingWorkspaceSymbolRequest
    {
        Json id;
        std::string query;
        std::vector<WorkspaceSymbolSourceVersion> sources;
    };
    std::unordered_map<std::string, WorkspaceSymbolIndexEntry>
        m_workspaceSymbolIndex;
    std::vector<PendingWorkspaceSymbolRequest>
        m_pendingWorkspaceSymbolRequests;
    enum class CompletionDataState { NotRequested, Loading, Ready, Failed };
    struct PendingCompletionRequest
    {
        Json id;
        std::string uri;
        int version{};
        int line{};
        int character{};
    };
    std::mutex m_completionMutex;
    CompletionDataState m_completionDataState{CompletionDataState::NotRequested};
    Json m_completionItems = Json::array();
    std::string m_completionDataError;
    std::vector<PendingCompletionRequest> m_pendingCompletionRequests;
    struct PendingFormatRequest
    {
        Json id;
        std::string uri;
        int version{};
    };
    std::mutex m_formatMutex;
    std::unordered_map<std::uint64_t, PendingFormatRequest> m_formatRequests;
    std::uint64_t m_nextFormatToken{1};
    struct PendingSemanticReply
    {
        Json id;
        SemanticReplyKind kind{SemanticReplyKind::Hover};
        bool includeDeclaration{};
        std::string newName;
    };
    struct PendingSemanticRequest
    {
        std::vector<PendingSemanticReply> replies;
        std::string uri;
        int version{};
        std::size_t positionByte{};
        bool projectIndexRequired{};
        std::unordered_map<std::string, std::string> projectTexts;
        std::unordered_map<std::string, int> projectVersions;
    };
    struct CachedSemanticQuery
    {
        int version{};
        std::size_t positionByte{};
        std::string text;
        Json hover = nullptr;
        Json signatureHelp = nullptr;
        Json definition = nullptr;
        Json references = Json::array();
        Json completionItems = Json::array();
        bool completionComplete{};
        Json symbol = nullptr;
        Json projectOccurrences = Json::array();
        Json projectIndexOccurrences = Json::array();
        bool projectIndexIncluded{};
        bool projectIndexComplete{true};
        std::unordered_map<std::string, std::string> projectTexts;
        std::unordered_map<std::string, int> projectVersions;
    };
    Json convertSemanticReply(const PendingSemanticReply &reply,
                              const std::string &uri,
                              const CachedSemanticQuery &query);
    Json renameWorkspaceEdit(const PendingSemanticReply &reply,
                             const std::string &uri,
                             const CachedSemanticQuery &query,
                             std::string *error);
    std::mutex m_semanticMutex;
    std::unordered_map<std::uint64_t, PendingSemanticRequest> m_semanticRequests;
    std::unordered_map<std::string, CachedSemanticQuery> m_semanticCache;
    std::uint64_t m_nextSemanticToken{1};
    struct ProjectPlan
    {
        std::filesystem::path root;
        std::filesystem::path workingDirectory;
        std::vector<std::filesystem::path> sourceFiles;
        std::vector<std::string> includePaths;
        bool loaded{};
    };
    std::string m_takweenProgram;
    std::filesystem::path m_applicationDirectory;
    ProjectPlan m_projectPlan;
    ProcessRunner m_projectRunner;
    std::mutex m_callbackMutex;
    MessageCallback m_messageCallback;
    ExitCallback m_exitCallback;
    bool m_initializeResponded{};
    bool m_initialized{};
    bool m_shutdownRequested{};
};
