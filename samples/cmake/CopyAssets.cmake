file(GLOB_RECURSE ALL_FILES "${SRC_DIR}/*")

foreach(file IN LISTS ALL_FILES)
    # skip source files
    # dont skip header in case we want to use header files shared in shaders
    if(file MATCHES "\\.(cpp|cxx|cc|c)$")
        continue()
    endif()

    file(RELATIVE_PATH rel_path "${SRC_DIR}" "${file}")
    get_filename_component(rel_dir "${rel_path}" DIRECTORY)

    if(rel_dir)
        file(MAKE_DIRECTORY "${DST_DIR}/${rel_dir}")
    else()
        file(MAKE_DIRECTORY "${DST_DIR}")
    endif()

    file(COPY_FILE "${file}" "${DST_DIR}/${rel_path}" ONLY_IF_DIFFERENT)
endforeach()