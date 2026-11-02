include_guard(GLOBAL)

function(rodaq_enable_warnings target)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "RoDAQ's warning baseline currently supports GCC and Clang only")
    endif()

    target_compile_options("${target}"
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Woverloaded-virtual
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
    )

    if(RODAQ_WARNINGS_AS_ERRORS)
        target_compile_options("${target}" PRIVATE -Werror)
    endif()
endfunction()

function(rodaq_enable_c_warnings target)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "RoDAQ's C warning baseline currently supports GCC and Clang only")
    endif()

    set_target_properties("${target}" PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )

    target_compile_options("${target}"
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wformat=2
            -Wstrict-prototypes
    )

    if(RODAQ_WARNINGS_AS_ERRORS)
        target_compile_options("${target}" PRIVATE -Werror)
    endif()
endfunction()
