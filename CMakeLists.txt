cmake_minimum_required(VERSION 3.20)

project(femtoFS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

if (APPLE)
    list(PREPEND CMAKE_PREFIX_PATH
        "/opt/homebrew/opt/libarchive"
        "/usr/local/opt/libarchive"
    )
endif ()

find_package(LibArchive REQUIRED)

add_executable(femtofsSim
    programs/femtofsSim.cpp
)

target_include_directories(femtofsSim PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_options(femtofsSim PRIVATE
    -Wall -Wextra -Wpedantic
)

add_executable(makefemtofs
    programs/makefemtofs.cpp
)

target_include_directories(makefemtofs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(makefemtofs PRIVATE
    LibArchive::LibArchive
)

target_compile_options(makefemtofs PRIVATE
    -Wall -Wextra -Wpedantic
)

add_custom_target(run-femtofsSim
    COMMAND $<TARGET_FILE:femtofsSim> ${CMAKE_SOURCE_DIR}/misc/list
    DEPENDS femtofsSim
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

if (BUILD_TESTING)
    add_subdirectory(units)
    add_test(
        NAME femtofsSim-format-accounting
        COMMAND /bin/sh
            ${CMAKE_SOURCE_DIR}/units/test-femtofsSim.sh
            $<TARGET_FILE:femtofsSim>
            ${CMAKE_BINARY_DIR}
    )
    add_test(
        NAME makefemtofs-directory-archive-equivalence
        COMMAND /bin/sh
            ${CMAKE_SOURCE_DIR}/units/test-makefemtofs.sh
            $<TARGET_FILE:makefemtofs>
            ${CMAKE_BINARY_DIR}
    )
endif ()
