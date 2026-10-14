#include "TestHarness.h"
#include "server/DocumentStore.h"

namespace {
DocumentChange ranged(int startLine, int startCharacter, int endLine, int endCharacter,
                      std::string text)
{
    return {true, {startLine, startCharacter}, {endLine, endCharacter}, std::move(text)};
}

DocumentChange full(std::string text)
{
    return {false, {}, {}, std::move(text)};
}
}

int main()
{
    DocumentStore store;
    std::string error;
    CHECK(store.open({"file:///main.baa", "baa", "", 1}, &error));
    CHECK(error.empty());
    CHECK(not store.open({"file:///other.txt", "unknown", "", 1}, &error));
    CHECK(not error.empty());

    CHECK(store.open({"file:///versioned.baa", "baa", "أ", 3}));
    CHECK(not store.change("file:///versioned.baa", 3, "ب", &error));
    CHECK(store.document("file:///versioned.baa").text == "أ");
    CHECK(store.change("file:///versioned.baa", 4, "ب", &error));
    CHECK(store.document("file:///versioned.baa").version == 4);
    CHECK(store.close("file:///versioned.baa"));

    // Ranged edits count UTF-16 code units: each Arabic letter is one unit and
    // the emoji is two, while the stored text stays UTF-8.
    const std::string uri = "file:///incremental.baa";
    CHECK(store.open({uri, "baa", "صحيح س = ١.\n😀 ع\n", 1}));
    CHECK(store.applyChanges(uri, 2, {ranged(0, 5, 0, 6, "ص")}, &error));
    CHECK(store.document(uri).text == "صحيح ص = ١.\n😀 ع\n");
    CHECK(store.applyChanges(uri, 3, {ranged(1, 3, 1, 4, "غ")}, &error));
    CHECK(store.document(uri).text == "صحيح ص = ١.\n😀 غ\n");

    // Changes in one notification apply in order, each to the previous result.
    CHECK(store.applyChanges(uri, 4,
                             {ranged(0, 0, 0, 4, "عدد"), ranged(0, 3, 0, 3, "_صحيح")},
                             &error));
    CHECK(store.document(uri).text == "عدد_صحيح ص = ١.\n😀 غ\n");

    // Insertion at the start of the line after the last newline, deletion
    // across lines, and a character past the line end, which means the end.
    CHECK(store.applyChanges(uri, 5, {ranged(2, 0, 2, 0, "// نهاية")}, &error));
    CHECK(store.document(uri).text == "عدد_صحيح ص = ١.\n😀 غ\n// نهاية");
    CHECK(store.applyChanges(uri, 6, {ranged(0, 99, 1, 99, "")}, &error));
    CHECK(store.document(uri).text == "عدد_صحيح ص = ١.\n// نهاية");
    CHECK(store.document(uri).version == 6);

    // A full-text change inside the same notification resets the base.
    CHECK(store.applyChanges(uri, 7, {full("أ\n"), ranged(1, 0, 1, 0, "ب")}, &error));
    CHECK(store.document(uri).text == "أ\nب");

    // A range outside the document rejects the whole notification and leaves
    // the text unchanged, then only a full-text change is accepted.
    for (const DocumentChange &bad : {ranged(5, 0, 5, 0, "x"), ranged(0, 3, 0, 2, "x")}) {
        CHECK(store.open({uri, "baa", "😀", 1}));
        CHECK(not store.applyChanges(uri, 2, {ranged(0, 2, 0, 2, "ي"), bad}, &error));
        CHECK(not error.empty());
        CHECK(store.document(uri).text == "😀");
        CHECK(store.document(uri).version == 1);
        CHECK(not store.document(uri).synchronized);
        CHECK(not store.applyChanges(uri, 2, {ranged(0, 0, 0, 0, "ي")}, &error));
        CHECK(store.document(uri).text == "😀");
        CHECK(store.applyChanges(uri, 2, {full("ي")}, &error));
        CHECK(store.document(uri).synchronized);
        CHECK(store.applyChanges(uri, 3, {ranged(0, 1, 0, 1, "ي")}, &error));
        CHECK(store.document(uri).text == "يي");
    }

    // A position inside a surrogate pair names no character.
    CHECK(store.open({uri, "baa", "😀", 1}));
    CHECK(not store.applyChanges(uri, 2, {ranged(0, 1, 0, 1, "ي")}, &error));
    CHECK(store.document(uri).text == "😀");

    // A ranged edit for a version already applied arrived out of order.
    CHECK(store.open({uri, "baa", "أ", 4}));
    CHECK(not store.applyChanges(uri, 4, {ranged(0, 0, 0, 0, "ب")}, &error));
    CHECK(not store.document(uri).synchronized);
    CHECK(not store.applyChanges(uri, 5, {}, &error));
    CHECK(not store.applyChanges("file:///closed.baa", 1, {full("")}, &error));
    return 0;
}
