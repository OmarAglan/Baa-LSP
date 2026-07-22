#pragma once

#include <string>
#include <unordered_map>

struct BaaDocument
{
    std::string uri;
    std::string languageId;
    std::string text;
    int version{};
};

class DocumentStore
{
public:
    bool open(const BaaDocument &document, std::string *errorMessage = nullptr);
    bool change(const std::string &uri, int version, const std::string &text,
                std::string *errorMessage = nullptr);
    bool close(const std::string &uri);

    bool contains(const std::string &uri) const;
    BaaDocument document(const std::string &uri) const;
    void clear();

private:
    std::unordered_map<std::string, BaaDocument> m_documents;
};
