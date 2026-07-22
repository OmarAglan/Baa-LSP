#include "TestHarness.h"
#include "server/DocumentStore.h"

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
    return 0;
}
