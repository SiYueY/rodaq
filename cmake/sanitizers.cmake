include_guard(GLOBAL)

function(rodaq_enable_sanitizers target)
    set(sanitizer_flags)

    if(RODAQ_ENABLE_ASAN)
        list(APPEND sanitizer_flags -fsanitize=address)
    endif()

    if(RODAQ_ENABLE_UBSAN)
        list(APPEND sanitizer_flags
            -fsanitize=undefined
            -fno-sanitize-recover=undefined
        )
    endif()

    if(sanitizer_flags)
        target_compile_options("${target}"
            PRIVATE
                ${sanitizer_flags}
                -fno-omit-frame-pointer
        )
        target_link_options("${target}" PRIVATE ${sanitizer_flags})
    endif()
endfunction()
