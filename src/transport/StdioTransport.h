#pragma once

#include "lsp/MessageFramer.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

class StdioTransport
{
public:
    using MessageCallback = std::function<void(std::string)>;
    using ErrorCallback = std::function<void(std::string)>;

    void setMessageCallback(MessageCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void run();
    void requestStop();
    void writeMessage(std::string_view jsonBody);

private:
    MessageFramer m_framer;
    MessageCallback m_messageCallback;
    ErrorCallback m_errorCallback;
    std::atomic_bool m_stopRequested{};
    std::mutex m_outputMutex;
};
