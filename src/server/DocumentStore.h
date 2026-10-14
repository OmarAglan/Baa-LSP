#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct BaaDocument
{
    std::string uri;
    std::string languageId;
    std::string text;
    int version{};
    // False after an incremental change was rejected: the server no longer
    // knows the client's text, so only a full-text change may follow.
    bool synchronized{true};
};

struct DocumentPosition
{
    int line{};
    int character{};
};

// One LSP TextDocumentContentChangeEvent. Without a range, text replaces the
// whole document; with one, text replaces the UTF-16 range in the text left
// by the previous change of the same notification.
struct DocumentChange
{
    bool hasRange{};
    DocumentPosition start;
    DocumentPosition end;
    std::string text;
};

class DocumentStore
{
public:
    bool open(const BaaDocument &document, std::string *errorMessage = nullptr);
    bool change(const std::string &uri, int version, const std::string &text,
                std::string *errorMessage = nullptr);
    bool applyChanges(const std::string &uri, int version,
                      const std::vector<DocumentChange> &changes,
                      std::string *errorMessage = nullptr);
    bool close(const std::string &uri);

    bool contains(const std::string &uri) const;
    BaaDocument document(const std::string &uri) const;
    std::vector<BaaDocument> documents() const;
    void clear();

private:
    std::unordered_map<std::string, BaaDocument> m_documents;
};
