function(add_baa_lsp_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${test_name} PRIVATE baa_lsp_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_baa_lsp_test(test_message_framer TestMessageFramer.cpp)
add_baa_lsp_test(test_position_encoding TestPositionEncoding.cpp)
add_baa_lsp_test(test_document_store TestDocumentStore.cpp)
add_baa_lsp_test(test_language_server TestLanguageServer.cpp)

add_executable(baa_lsp_fake_compiler FakeBaaCompiler.cpp)
target_include_directories(baa_lsp_fake_compiler PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(baa_lsp_fake_compiler PRIVATE nlohmann_json::nlohmann_json)

add_test(
    NAME test_stdio_protocol
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/TestStdioProtocol.py
            $<TARGET_FILE:baa-lsp> $<TARGET_FILE:baa_lsp_fake_compiler>
)
