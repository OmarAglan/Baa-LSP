cmake_minimum_required(VERSION 3.21)

project(BaaLSP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/WindowsRuntime.cmake)

include(FetchContent)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

option(BAA_LSP_BUILD_TESTS "Build Baa-LSP tests" ON)
set(BAA_LSP_BAA_EXECUTABLE "" CACHE FILEPATH
    "Optional Baa compiler used by the real compiler/LSP integration test")
set(BAA_LSP_TAKWEEN_EXECUTABLE "" CACHE FILEPATH
    "Optional Takween executable used by the project navigation integration test")

add_library(baa_lsp_core STATIC
    src/compiler/BaaCompilerBridge.cpp
    src/compiler/BaaCompilerBridge.h
    src/lsp/MessageFramer.cpp
    src/lsp/MessageFramer.h
    src/lsp/Json.h
    src/lsp/PositionEncoding.cpp
    src/lsp/PositionEncoding.h
    src/process/ProcessRunner.cpp
    src/process/ProcessRunner.h
    src/server/BaaLanguageServer.cpp
    src/server/BaaLanguageServer.h
    src/server/DocumentStore.cpp
    src/server/DocumentStore.h
)

target_include_directories(baa_lsp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(baa_lsp_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
baa_lsp_harden_windows_runtime(baa_lsp_core)

add_executable(baa-lsp
    src/main.cpp
    src/transport/StdioTransport.cpp
    src/transport/StdioTransport.h
)
target_link_libraries(baa-lsp PRIVATE baa_lsp_core)
baa_lsp_harden_windows_runtime(baa-lsp)

install(TARGETS baa-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md ROADMAP.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/baa-lsp)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/baa-lsp)
install(FILES assets/branding/icons/baa-lsp-64.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/baa-lsp/branding)

if(WIN32)
    set(BAA_LSP_PACKAGE_SYSTEM "windows-x86_64")
elseif(APPLE)
    set(BAA_LSP_PACKAGE_SYSTEM "macos-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(BAA_LSP_PACKAGE_SYSTEM "linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_PACKAGE_NAME "Baa-LSP")
set(CPACK_PACKAGE_VENDOR "Baa Ecosystem")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "Baa-LSP-${PROJECT_VERSION}-${BAA_LSP_PACKAGE_SYSTEM}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

if(BAA_LSP_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_subdirectory(tests)
endif()
