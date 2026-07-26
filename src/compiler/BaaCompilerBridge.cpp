#include "compiler/BaaCompilerBridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace {
std::string trimmed(std::string value)
{
    const auto isSpace = [](unsigned char character) { return std::isspace(character); };
    value.erase(value.begin(), std::ranges::find_if_not(value, isSpace));
    value.erase(std::ranges::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
                value.end());
    return value;
}

std::filesystem::path pathFromUtf8(std::string_view value)
{
    const auto *first = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path(std::u8string(first, first + value.size()));
}

std::string pathToUtf8(const std::filesystem::path &path)
{
    const std::u8string encoded = path.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}
}

BaaCompilerBridge::BaaCompilerBridge()
    : m_worker(&BaaCompilerBridge::workerLoop, this)
{
}

BaaCompilerBridge::~BaaCompilerBridge()
{
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
        m_pending.clear();
        m_pendingSymbols.clear();
        m_pendingFormats.clear();
        m_pendingSemantic.clear();
        m_completionDataPending = false;
        m_latestVersions.clear();
        m_callback = {};
        m_symbolCallback = {};
        m_completionDataCallback = {};
        m_formatCallback = {};
        m_semanticCallback = {};
    }
    m_runner.cancel();
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void BaaCompilerBridge::setCompilerProgram(std::string program)
{
    std::scoped_lock lock(m_mutex);
    m_compilerProgram = trimmed(std::move(program));
}

void BaaCompilerBridge::setApplicationDirectory(std::filesystem::path directory)
{
    std::scoped_lock lock(m_mutex);
    m_applicationDirectory = std::move(directory);
}

void BaaCompilerBridge::setDebounceInterval(int milliseconds)
{
    std::scoped_lock lock(m_mutex);
    m_debounce = std::chrono::milliseconds(std::max(0, milliseconds));
}

void BaaCompilerBridge::setAnalysisCallback(AnalysisCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_callback = std::move(callback);
}

void BaaCompilerBridge::setSymbolCallback(SymbolCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_symbolCallback = std::move(callback);
}

void BaaCompilerBridge::setCompletionDataCallback(CompletionDataCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_completionDataCallback = std::move(callback);
}

void BaaCompilerBridge::setFormatCallback(FormatCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_formatCallback = std::move(callback);
}

void BaaCompilerBridge::setSemanticCallback(SemanticCallback callback)
{
    std::scoped_lock lock(m_mutex);
    m_semanticCallback = std::move(callback);
}

void BaaCompilerBridge::schedule(BaaAnalysisRequest request)
{
    if (not request.isValid()) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.insert_or_assign(request.uri, request.version);
        m_pending.insert_or_assign(request.uri, request);
        std::erase_if(m_pendingSymbols, [&request](const BaaSymbolRequest &symbolRequest) {
            return symbolRequest.uri == request.uri and symbolRequest.version != request.version;
        });
        std::erase_if(m_pendingFormats, [&request](const BaaFormatRequest &formatRequest) {
            return formatRequest.uri == request.uri and
                   formatRequest.version != request.version;
        });
        std::erase_if(m_pendingSemantic, [&request](const BaaSemanticRequest &semanticRequest) {
            return semanticRequest.uri == request.uri and
                   semanticRequest.version != request.version;
        });
        cancelActive = m_activeUri == request.uri and m_activeVersion != request.version;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::requestSymbols(BaaSymbolRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingSymbols.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestCompletionData()
{
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_completionDataPending = true;
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestFormat(BaaFormatRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingFormats.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::requestSemantic(BaaSemanticRequest request)
{
    if (not request.isValid()) return;
    {
        std::scoped_lock lock(m_mutex);
        if (m_stopping) return;
        m_latestVersions.try_emplace(request.uri, request.version);
        m_pendingSemantic.push_back(std::move(request));
        ++m_scheduleSerial;
    }
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelSymbols(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_pendingSymbols, [token](const BaaSymbolRequest &request) {
            return request.token == token;
        });
        cancelActive = m_activeSymbolToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelFormat(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_pendingFormats, [token](const BaaFormatRequest &request) {
            return request.token == token;
        });
        cancelActive = m_activeFormatToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelSemantic(std::uint64_t token)
{
    if (token == 0) return;
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_pendingSemantic, [token](const BaaSemanticRequest &request) {
            return request.token == token;
        });
        cancelActive = m_activeSemanticToken == token;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancel(const std::string &uri)
{
    bool cancelActive = false;
    {
        std::scoped_lock lock(m_mutex);
        m_pending.erase(uri);
        std::erase_if(m_pendingSymbols, [&uri](const BaaSymbolRequest &request) {
            return request.uri == uri;
        });
        std::erase_if(m_pendingFormats, [&uri](const BaaFormatRequest &request) {
            return request.uri == uri;
        });
        std::erase_if(m_pendingSemantic, [&uri](const BaaSemanticRequest &request) {
            return request.uri == uri;
        });
        m_latestVersions.erase(uri);
        cancelActive = m_activeUri == uri;
        ++m_scheduleSerial;
    }
    if (cancelActive) m_runner.cancel();
    m_wake.notify_one();
}

void BaaCompilerBridge::cancelAll()
{
    bool hasActive = false;
    {
        std::scoped_lock lock(m_mutex);
        m_pending.clear();
        m_pendingSymbols.clear();
        m_pendingFormats.clear();
        m_pendingSemantic.clear();
        m_completionDataPending = false;
        m_latestVersions.clear();
        hasActive = not m_activeUri.empty() or m_completionDataActive;
        ++m_scheduleSerial;
    }
    if (hasActive) m_runner.cancel();
    m_wake.notify_one();
}

std::string BaaCompilerBridge::resolveCompilerProgram() const
{
    std::string configured;
    std::filesystem::path applicationDirectory;
    {
        std::scoped_lock lock(m_mutex);
        configured = m_compilerProgram;
        applicationDirectory = m_applicationDirectory;
    }
    if (configured.empty()) {
        if (const char *environment = std::getenv("BAA")) configured = trimmed(environment);
    }
    if (not configured.empty()) return configured;

#if defined(_WIN32)
    constexpr std::string_view executable = "baa.exe";
#else
    constexpr std::string_view executable = "baa";
#endif
    if (not applicationDirectory.empty()) {
        const std::filesystem::path nested = applicationDirectory / "baa" / executable;
        if (std::filesystem::is_regular_file(nested)) return pathToUtf8(nested);
        const std::filesystem::path adjacent = applicationDirectory / executable;
        if (std::filesystem::is_regular_file(adjacent)) return pathToUtf8(adjacent);
    }
    return std::string(executable);
}

void BaaCompilerBridge::workerLoop()
{
    while (true) {
        BaaAnalysisRequest request;
        BaaSymbolRequest symbolRequest;
        BaaFormatRequest formatRequest;
        BaaSemanticRequest semanticRequest;
        bool isSymbolRequest = false;
        bool isFormatRequest = false;
        bool isSemanticRequest = false;
        bool isCompletionDataRequest = false;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_stopping or m_completionDataPending or
                       not m_pendingFormats.empty() or
                       not m_pendingSemantic.empty() or
                       not m_pendingSymbols.empty() or not m_pending.empty();
            });
            if (m_stopping) return;

            if (not m_completionDataPending and m_pendingFormats.empty() and
                m_pendingSemantic.empty() and
                m_pendingSymbols.empty()) {
                const std::uint64_t observedSerial = m_scheduleSerial;
                const std::chrono::milliseconds debounce = m_debounce;
                if (m_wake.wait_for(lock, debounce, [this, observedSerial] {
                        return m_stopping or m_scheduleSerial != observedSerial;
                    })) {
                    if (m_stopping) return;
                    continue;
                }
            }

            if (m_completionDataPending) {
                isCompletionDataRequest = true;
                m_completionDataPending = false;
                m_completionDataActive = true;
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeSymbolToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            } else if (not m_pendingFormats.empty()) {
                isFormatRequest = true;
                formatRequest = std::move(m_pendingFormats.front());
                m_pendingFormats.pop_front();
                m_activeUri = formatRequest.uri;
                m_activeVersion = formatRequest.version;
                m_activeSymbolToken = 0;
                m_activeFormatToken = formatRequest.token;
                m_activeSemanticToken = 0;
            } else if (not m_pendingSemantic.empty()) {
                isSemanticRequest = true;
                semanticRequest = std::move(m_pendingSemantic.front());
                m_pendingSemantic.pop_front();
                m_activeUri = semanticRequest.uri;
                m_activeVersion = semanticRequest.version;
                m_activeSymbolToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = semanticRequest.token;
            } else if (not m_pendingSymbols.empty()) {
                isSymbolRequest = true;
                symbolRequest = std::move(m_pendingSymbols.front());
                m_pendingSymbols.pop_front();
                m_activeUri = symbolRequest.uri;
                m_activeVersion = symbolRequest.version;
                m_activeSymbolToken = symbolRequest.token;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            } else {
                if (m_pending.empty()) continue;
                auto next = m_pending.begin();
                request = std::move(next->second);
                m_pending.erase(next);
                m_activeUri = request.uri;
                m_activeVersion = request.version;
                m_activeSymbolToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
            }
            m_runner.prepare();
        }

        const std::string compiler = resolveCompilerProgram();
        if (isCompletionDataRequest) {
            ProcessResult process = m_runner.run(
                compiler, {"--completion-data=json"}, {}, {});
            BaaCompletionDataResult result;
            result.exitCode = process.exitCode;
            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(process.standardOutput, nullptr, false);
                if (not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "completion-data-json-v1" and
                    parsed.value("language", "") == "baa" and
                    parsed.value("items", Json::array()).is_array()) {
                    result.items = parsed["items"];
                } else {
                    result.errorMessage =
                        "Baa returned completion data that does not satisfy completion-data-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " + trimmed(process.standardError);
                    }
                }
            }

            CompletionDataCallback callback;
            {
                std::scoped_lock lock(m_mutex);
                m_completionDataActive = false;
                callback = m_completionDataCallback;
            }
            if (not process.cancelled and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        const std::string &filePath = isFormatRequest
            ? formatRequest.filePath
            : isSemanticRequest
            ? semanticRequest.filePath
            : (isSymbolRequest ? symbolRequest.filePath : request.filePath);
        const std::string &text = isFormatRequest
            ? formatRequest.text
            : isSemanticRequest
            ? semanticRequest.text
            : (isSymbolRequest ? symbolRequest.text : request.text);
        const std::filesystem::path sourcePath = pathFromUtf8(filePath);
        std::vector<std::string> arguments;
        if (isFormatRequest) {
            arguments = {"--format=json", "--source-stdin=" + filePath};
        } else if (isSemanticRequest) {
            arguments = {
                "--semantic-query=json",
                "--position-byte=" + std::to_string(semanticRequest.positionByte)
            };
            for (const std::string &includePath : semanticRequest.includePaths) {
                arguments.push_back("-I");
                arguments.push_back(includePath);
            }
            arguments.push_back("--source-stdin=" + filePath);
        } else if (isSymbolRequest) {
            arguments = {"--dump-symbols=json", "--source-stdin=" + filePath};
        } else {
            arguments = {"--check", "--diagnostics=json",
                         "--source-stdin=" + filePath};
        }
        const std::filesystem::path workingDirectory =
            isSemanticRequest and
            not semanticRequest.projectWorkingDirectory.empty()
                ? semanticRequest.projectWorkingDirectory
                : sourcePath.parent_path();
        ProcessResult process = m_runner.run(
            compiler, arguments, workingDirectory, text);

        if (isFormatRequest) {
            BaaFormatResult result;
            result.token = formatRequest.token;
            result.uri = formatRequest.uri;
            result.text = formatRequest.text;
            result.version = formatRequest.version;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(
                    process.standardOutput, nullptr, false);
                if (process.exitCode == 0 and
                    not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "format-json-v1" and
                    parsed.value("compiler_version", Json(nullptr)).is_string() and
                    parsed.value("language", "") == "baa" and
                    parsed.value("file", Json(nullptr)).is_string() and
                    parsed.value("position_encoding", "") == "utf-8-bytes" and
                    parsed.value("line_ending", "") == "lf" and
                    parsed.value("indent_width", 0) == 4 and
                    parsed.value("insert_spaces", false) and
                    parsed.value("source_bytes", std::size_t{}) == text.size() and
                    parsed.value("formatted_bytes", Json(nullptr))
                        .is_number_unsigned() and
                    parsed.value("changed", Json(nullptr)).is_boolean() and
                    parsed.value("formatted_text", Json(nullptr)).is_string()) {
                    result.formattedText =
                        parsed["formatted_text"].get<std::string>();
                    result.changed = parsed["changed"].get<bool>();
                    const bool exactMetadata =
                        parsed["formatted_bytes"].get<std::size_t>() ==
                            result.formattedText.size() and
                        result.changed == (result.formattedText != text);
                    if (not exactMetadata) {
                        result.errorMessage =
                            "Baa returned formatting byte/change metadata that "
                            "does not match format-json-v1.";
                    }
                } else {
                    result.errorMessage =
                        "Baa returned formatting data that does not satisfy "
                        "format-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " +
                            trimmed(process.standardError);
                    }
                }
            }

            FormatCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeFormatToken = 0;
                const auto latest = m_latestVersions.find(formatRequest.uri);
                publish = not process.cancelled and
                          latest != m_latestVersions.end() and
                          latest->second == formatRequest.version;
                callback = m_formatCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        if (isSemanticRequest) {
            BaaSemanticResult result;
            result.token = semanticRequest.token;
            result.uri = semanticRequest.uri;
            result.text = semanticRequest.text;
            result.version = semanticRequest.version;
            result.positionByte = semanticRequest.positionByte;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled and process.exitCode == 1) {
                // An incomplete/invalid edit has no current semantic answer.
                // Return null LSP results instead of turning a source error into
                // a language-server transport failure.
                result.hover = nullptr;
                result.signatureHelp = nullptr;
                result.definition = nullptr;
                result.references = Json::array();
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(process.standardOutput, nullptr, false);
                if (not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "semantic-query-json-v1" and
                    parsed.value("position_encoding", "") == "utf-8-bytes" and
                    parsed.value("position_byte", std::size_t{}) ==
                        semanticRequest.positionByte and
                    parsed.contains("symbol") and
                    parsed.contains("hover") and
                    parsed.contains("signature_help") and
                    parsed.contains("definition") and
                    parsed.contains("references") and
                    parsed["references"].is_array()) {
                    result.hover = parsed["hover"];
                    result.signatureHelp = parsed["signature_help"];
                    result.definition = parsed["definition"];
                    result.references = parsed["references"];
                    result.symbol = parsed["symbol"];
                } else {
                    result.errorMessage =
                        "Baa returned semantic data that does not satisfy "
                        "semantic-query-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " + trimmed(process.standardError);
                    }
                }
            }

            if (result.errorMessage.empty() and
                not process.cancelled and result.symbol.is_object() and
                not semanticRequest.projectSources.empty()) {
                const std::string domain = result.symbol.value("domain", "");
                const bool projectIdentity =
                    domain == "external" or domain == "declaration";
                // All project translation units participate in an
                // external/declaration identity. Local and file identities
                // only need the origin unit, but still require its complete
                // compiler-owned index for collision-checked rename.
                for (const BaaSemanticRequest::ProjectSource &projectSource :
                     semanticRequest.projectSources) {
                    if (not projectIdentity and
                        projectSource.filePath != semanticRequest.filePath)
                        continue;
                    std::vector<std::string> indexArguments{
                        "--semantic-index=json"
                    };
                    for (const std::string &includePath :
                         semanticRequest.includePaths) {
                        indexArguments.push_back("-I");
                        indexArguments.push_back(includePath);
                    }
                    if (projectSource.useStandardInput) {
                        indexArguments.push_back(
                            "--source-stdin=" + projectSource.filePath);
                    } else {
                        indexArguments.push_back(projectSource.filePath);
                    }

                    const std::filesystem::path projectPath =
                        pathFromUtf8(projectSource.filePath);
                    const std::filesystem::path indexWorkingDirectory =
                        not semanticRequest.projectWorkingDirectory.empty()
                            ? semanticRequest.projectWorkingDirectory
                            : projectPath.parent_path();
                    ProcessResult indexProcess = m_runner.run(
                        compiler,
                        indexArguments,
                        indexWorkingDirectory,
                        projectSource.useStandardInput
                            ? std::string_view(projectSource.text)
                            : std::string_view{});
                    if (indexProcess.cancelled) {
                        process.cancelled = true;
                        break;
                    }
                    if (not indexProcess.started) {
                        result.errorMessage =
                            indexProcess.errorMessage.empty()
                                ? "Baa compiler executable was not found."
                                : indexProcess.errorMessage;
                        break;
                    }
                    if (indexProcess.exitCode == 1) {
                        result.projectIndexComplete = false;
                        result.warnings.push_back(
                            "Project semantic index skipped a source with "
                            "compiler errors: " + projectSource.filePath);
                        continue;
                    }
                    if (indexProcess.exitCode != 0) {
                        result.errorMessage =
                            "Baa semantic index failed for " +
                            projectSource.filePath + " with exit code " +
                            std::to_string(indexProcess.exitCode) + ".";
                        if (not indexProcess.standardError.empty()) {
                            result.errorMessage += " " +
                                trimmed(indexProcess.standardError);
                        }
                        break;
                    }

                    const Json index = Json::parse(
                        indexProcess.standardOutput, nullptr, false);
                    if (index.is_discarded() or not index.is_object() or
                        index.value("schema_version", "") !=
                            "semantic-index-json-v1" or
                        index.value("position_encoding", "") !=
                            "utf-8-bytes" or
                        not index.value(
                            "occurrences", Json::array()).is_array()) {
                        result.errorMessage =
                            "Baa returned semantic index data that does not "
                            "satisfy semantic-index-json-v1 for " +
                            projectSource.filePath + ".";
                        break;
                    }
                    for (const Json &occurrence : index["occurrences"]) {
                        if (occurrence.is_object())
                            result.projectIndexOccurrences.push_back(
                                occurrence);
                        if (occurrence.is_object() and
                            occurrence.value("symbol", Json(nullptr)) ==
                                result.symbol and
                            occurrence.contains("location") and
                            occurrence["location"].is_object()) {
                            result.projectOccurrences.push_back(occurrence);
                        }
                    }
                }
            }

            SemanticCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeSemanticToken = 0;
                m_activeFormatToken = 0;
                const auto latest = m_latestVersions.find(semanticRequest.uri);
                publish = not process.cancelled and latest != m_latestVersions.end() and
                          latest->second == semanticRequest.version;
                callback = m_semanticCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        if (isSymbolRequest) {
            BaaSymbolResult result;
            result.token = symbolRequest.token;
            result.uri = symbolRequest.uri;
            result.text = symbolRequest.text;
            result.version = symbolRequest.version;
            result.exitCode = process.exitCode;

            if (not process.started) {
                result.errorMessage = process.errorMessage.empty()
                    ? "Baa compiler executable was not found."
                    : process.errorMessage;
            } else if (not process.cancelled) {
                const Json parsed = Json::parse(process.standardOutput, nullptr, false);
                if (not parsed.is_discarded() and parsed.is_object() and
                    parsed.value("schema_version", "") == "symbols-json-v1" and
                    parsed.value("position_encoding", "") == "utf-8-bytes" and
                    parsed.value("symbols", Json::array()).is_array()) {
                    result.symbols = parsed["symbols"];
                } else {
                    result.errorMessage =
                        "Baa returned symbols that do not satisfy symbols-json-v1.";
                    if (not process.standardError.empty()) {
                        result.errorMessage += " " + trimmed(process.standardError);
                    }
                }
            }

            SymbolCallback callback;
            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                m_activeUri.clear();
                m_activeVersion = 0;
                m_activeSymbolToken = 0;
                m_activeFormatToken = 0;
                m_activeSemanticToken = 0;
                const auto latest = m_latestVersions.find(symbolRequest.uri);
                publish = not process.cancelled and latest != m_latestVersions.end() and
                          latest->second == symbolRequest.version;
                callback = m_symbolCallback;
            }
            if (publish and callback) callback(std::move(result));
            m_wake.notify_one();
            continue;
        }

        BaaAnalysisResult result;
        result.uri = request.uri;
        result.filePath = request.filePath;
        result.text = request.text;
        result.version = request.version;
        result.exitCode = process.exitCode;

        if (not process.started) {
            result.errorMessage = process.errorMessage.empty()
                ? "Baa compiler executable was not found."
                : process.errorMessage;
        } else if (not process.cancelled) {
            const Json parsed = Json::parse(process.standardOutput, nullptr, false);
            if (not parsed.is_discarded() and parsed.is_object() and
                parsed.value("schema_version", "") == "diagnostics-json-v1" and
                parsed.value("diagnostics", Json::array()).is_array()) {
                result.diagnostics = parsed["diagnostics"];
            } else {
                result.errorMessage = "Baa returned diagnostics that do not satisfy diagnostics-json-v1.";
                if (not process.standardError.empty()) {
                    result.errorMessage += " " + trimmed(process.standardError);
                }
            }
        }

        AnalysisCallback callback;
        bool publish = false;
        {
            std::scoped_lock lock(m_mutex);
            m_activeUri.clear();
            m_activeVersion = 0;
            m_activeSymbolToken = 0;
            m_activeFormatToken = 0;
            m_activeSemanticToken = 0;
            const auto latest = m_latestVersions.find(request.uri);
            publish = not process.cancelled and latest != m_latestVersions.end() and
                      latest->second == request.version;
            callback = m_callback;
        }
        if (publish and callback) callback(std::move(result));
        m_wake.notify_one();
    }
}
