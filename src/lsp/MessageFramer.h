#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class MessageFramer
{
public:
    static constexpr std::size_t MaximumHeaderBytes = 8 * 1024;
    static constexpr std::size_t MaximumContentBytes = 16 * 1024 * 1024;

    std::vector<std::string> appendData(std::string_view data,
                                        std::string *errorMessage = nullptr);
    void clear();

    static std::string frame(std::string_view jsonBody);

private:
    std::string m_buffer;
};
