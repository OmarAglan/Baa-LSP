# Keep Baa-LSP executables independent of whichever compiler runtime happens
# to appear first on the caller's Windows PATH.
function(baa_lsp_harden_windows_runtime target_name)
    if(NOT WIN32)
        return()
    endif()

    if(MINGW)
        # Baa-LSP has no shared third-party runtime dependency. Linking the
        # MinGW runtime statically removes libgcc/libstdc++/winpthread DLL
        # lookup from the production server and all native test helpers.
        target_link_options(${target_name} PRIVATE -static)
    elseif(MSVC)
        set_property(TARGET ${target_name} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
