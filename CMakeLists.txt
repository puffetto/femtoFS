cmake_minimum_required(VERSION 3.20)

project(femtoFS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_executable(femtofsSim
    programs/femtofsSim.cpp
)

target_include_directories(femtofsSim PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

add_custom_target(run-femtofsSim
    COMMAND $<TARGET_FILE:femtofsSim> ${CMAKE_SOURCE_DIR}/misc/list
    DEPENDS femtofsSim
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

if (BUILD_TESTING)
    add_subdirectory(units)
endif ()
