set(PN ${PARENT_PROJECT_NAME}_Tests)
project(${PN} LANGUAGES C CXX)

# ---------------------- doctest -------------------------
CPMAddPackage("gh:onqtam/doctest@2.4.12")

add_executable(${PN}
        basic.cpp
)
target_link_libraries(${PN}
        PRIVATE
        mythril
        doctest
)
enable_testing()