# mythril_reflect_shaders(<target> [SOURCES|UNTYPED_SOURCES|MODULES|SEARCH_PATHS ...] [FILE_NAMESPACES] [MATRIX_LAYOUT ROW|COLUMN] [TYPES_HEADER|TYPES_NAMESPACE|NAMESPACE|SPIRV_DIR <value>])
# writes <shader>.slang.spv to SPIRV_DIR and <shader>.shadergen.h with kSpirvFile in <ns>::<shader>, plus the push structs for SOURCES, structs from an imported module go in <module>.shadergen.h and the module must be in MODULES, FILE_NAMESPACES generates each file's structs in <ns>::<file stem>, build <target>_shaders to run only this step
function(mythril_reflect_shaders target)
    cmake_parse_arguments(ARG "FILE_NAMESPACES" "MATRIX_LAYOUT;TYPES_HEADER;TYPES_NAMESPACE;NAMESPACE;SPIRV_DIR" "SOURCES;UNTYPED_SOURCES;MODULES;SEARCH_PATHS" ${ARGN})
    if(NOT ARG_SOURCES AND NOT ARG_UNTYPED_SOURCES)
        message(FATAL_ERROR "mythril_reflect_shaders(${target}) needs SOURCES or UNTYPED_SOURCES")
    endif()
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "mythril_reflect_shaders(${target}) got unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/mythril_reflect/${target}")
    set(_depfile "${_out_dir}/reflect.d")
    set(_stamp "${_out_dir}/reflect.stamp")
    set(_args --out-dir "${_out_dir}" --depfile "${_depfile}" --stamp "${_stamp}" $<$<CONFIG:Debug>:--debug>)
    set(_spirv_dir "${_out_dir}")
    if(ARG_SPIRV_DIR)
        list(APPEND _args --spirv-dir "${ARG_SPIRV_DIR}")
        set(_spirv_dir "${ARG_SPIRV_DIR}")
    endif()
    foreach(_path IN LISTS ARG_SEARCH_PATHS)
        list(APPEND _args --search-path "${_path}")
    endforeach()
    if(ARG_MATRIX_LAYOUT STREQUAL "COLUMN")
        list(APPEND _args --matrix-layout column)
    elseif(ARG_MATRIX_LAYOUT AND NOT ARG_MATRIX_LAYOUT STREQUAL "ROW")
        message(FATAL_ERROR "mythril_reflect_shaders(${target}) MATRIX_LAYOUT is ROW or COLUMN, not ${ARG_MATRIX_LAYOUT}")
    endif()
    if(ARG_FILE_NAMESPACES)
        list(APPEND _args --file-namespaces)
    endif()
    if(ARG_TYPES_HEADER)
        list(APPEND _args --types-header "${ARG_TYPES_HEADER}")
    endif()
    if(ARG_TYPES_NAMESPACE)
        list(APPEND _args --types-namespace "${ARG_TYPES_NAMESPACE}")
    endif()
    if(ARG_NAMESPACE)
        list(APPEND _args --namespace "${ARG_NAMESPACE}")
    endif()

    set(_headers "")
    set(_spirv_files "")
    set(_sources "")
    set(_source_args "")
    foreach(_source IN LISTS ARG_SOURCES ARG_UNTYPED_SOURCES)
        cmake_path(ABSOLUTE_PATH _source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _abs)
        cmake_path(GET _abs STEM _stem)
        cmake_path(GET _abs FILENAME _name)
        list(APPEND _sources "${_abs}")
        list(APPEND _headers "${_out_dir}/${_stem}.shadergen.h")
        # BYPRODUCTS take no target generator expressions, so a .spv in such a SPIRV_DIR is not tracked
        if(NOT _spirv_dir MATCHES "\\$<")
            list(APPEND _spirv_files "${_spirv_dir}/${_name}.spv")
        endif()
        if(_source IN_LIST ARG_SOURCES)
            list(APPEND _source_args "${_abs}")
        else()
            list(APPEND _source_args --untyped "${_abs}")
        endif()
    endforeach()

    foreach(_module IN LISTS ARG_MODULES)
        cmake_path(ABSOLUTE_PATH _module BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _abs)
        cmake_path(GET _abs STEM _stem)
        list(APPEND _headers "${_out_dir}/${_stem}.shadergen.h")
        list(APPEND _source_args --module "${_abs}")
    endforeach()

    # the tool rewrites only changed headers, so they are byproducts and the stamp tells Make the step ran
    add_custom_command(
            OUTPUT "${_stamp}"
            BYPRODUCTS ${_headers} ${_spirv_files}
            COMMAND "$<TARGET_FILE:mythril-reflect>" ${_args} ${_source_args}
            DEPENDS mythril-reflect ${_sources}
            DEPFILE "${_depfile}"
            COMMENT "Compiling and reflecting Slang shaders for ${target}"
            COMMAND_EXPAND_LISTS
            VERBATIM
    )
    add_custom_target(${target}_shaders DEPENDS "${_stamp}")
    target_sources(${target} PRIVATE "${_stamp}" ${_headers})
    target_include_directories(${target} PRIVATE "${_out_dir}")
endfunction()
