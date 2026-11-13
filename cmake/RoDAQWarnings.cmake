function(rodaq_enable_project_warnings target)
    target_compile_options(
        ${target}
        PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wall>
            $<$<COMPILE_LANGUAGE:C>:-Wextra>
            $<$<COMPILE_LANGUAGE:C>:-Wpedantic>
            $<$<COMPILE_LANGUAGE:CXX>:-Wall>
            $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)

    if(RODAQ_WARNINGS_AS_ERRORS)
        target_compile_options(
            ${target}
            PRIVATE
                $<$<COMPILE_LANGUAGE:C>:-Werror>
                $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
endfunction()
