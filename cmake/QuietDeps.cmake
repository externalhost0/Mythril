# silences third party "CMake Warning (deprecated)" noise like glm's "Compatibility with CMake < 3.10 will be removed" on cmake_minimum_required
# function bodies keep the diagnostic state from definition time, so wrap the include that defines CPMAddPackage
macro(QUIET_DEPS_START)
    if(COMMAND cmake_diagnostic)
        cmake_diagnostic(GET CMD_DEPRECATED _QUIET_DEPS_PREV)
        cmake_diagnostic(SET CMD_DEPRECATED IGNORE)
    endif()
endmacro()

macro(QUIET_DEPS_END)
    if(COMMAND cmake_diagnostic)
        cmake_diagnostic(SET CMD_DEPRECATED ${_QUIET_DEPS_PREV})
        unset(_QUIET_DEPS_PREV)
    endif()
endmacro()
