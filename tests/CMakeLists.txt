if(WIN32)
    get_filename_component(baa_lsp_compiler_runtime_dir
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    string(REPLACE ";" "\\;" baa_lsp_host_path "$ENV{Path}")
    set(baa_lsp_test_path
        "${baa_lsp_compiler_runtime_dir}\\;${baa_lsp_host_path}")
endif()

function(add_baa_lsp_runtime_test test_name)
    if(WIN32)
        # A Windows environment may contain both "Path" and "PATH". CTest's
        # process launcher can then drop the MinGW runtime lookup after one
        # child process, producing 0xc0000135. Run every test through CMake,
        # remove both spellings, and establish one deterministic Path.
        add_test(
            NAME ${test_name}
            COMMAND ${CMAKE_COMMAND} -E env
                --unset=Path
                --unset=PATH
                "Path=${baa_lsp_test_path}"
                ${ARGN}
        )
    else()
        add_test(NAME ${test_name} COMMAND ${ARGN})
    endif()
endfunction()

function(add_baa_lsp_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${test_name} PRIVATE baa_lsp_core)
    baa_lsp_harden_windows_runtime(${test_name})
    add_baa_lsp_runtime_test(${test_name} $<TARGET_FILE:${test_name}>)
endfunction()

add_baa_lsp_test(test_message_framer TestMessageFramer.cpp)
add_baa_lsp_test(test_position_encoding TestPositionEncoding.cpp)
add_baa_lsp_test(test_document_store TestDocumentStore.cpp)
add_baa_lsp_test(test_language_server TestLanguageServer.cpp)

add_executable(baa_lsp_fake_compiler FakeBaaCompiler.cpp)
target_include_directories(baa_lsp_fake_compiler PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(baa_lsp_fake_compiler PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(baa_lsp_fake_compiler PRIVATE shell32)
endif()
baa_lsp_harden_windows_runtime(baa_lsp_fake_compiler)

add_baa_lsp_runtime_test(
    test_stdio_protocol
    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/TestStdioProtocol.py
    $<TARGET_FILE:baa-lsp> $<TARGET_FILE:baa_lsp_fake_compiler>
)

if(WIN32)
    add_baa_lsp_runtime_test(
        test_windows_runtime_independence
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/TestWindowsRuntime.py
        $<TARGET_FILE:baa-lsp>
    )
endif()

if(BAA_LSP_BAA_EXECUTABLE)
    add_baa_lsp_runtime_test(
        test_real_baa_protocol
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/TestRealBaaProtocol.py
        $<TARGET_FILE:baa-lsp> ${BAA_LSP_BAA_EXECUTABLE}
    )
endif()

if(BAA_LSP_BAA_EXECUTABLE AND BAA_LSP_TAKWEEN_EXECUTABLE)
    add_baa_lsp_runtime_test(
        test_project_navigation
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/TestProjectNavigation.py
        $<TARGET_FILE:baa-lsp>
        ${BAA_LSP_BAA_EXECUTABLE}
        ${BAA_LSP_TAKWEEN_EXECUTABLE}
    )
endif()
