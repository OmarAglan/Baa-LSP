#pragma once

#include <iostream>
#include <string_view>

inline int testFailure(std::string_view expression, std::string_view file, int line)
{
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    return 1;
}

#define CHECK(expression) \
    do { if (not (expression)) return testFailure(#expression, __FILE__, __LINE__); } while (false)
