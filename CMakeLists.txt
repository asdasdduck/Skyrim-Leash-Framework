cmake_minimum_required(VERSION 3.21)

project(LeashFramework VERSION 1.1.0 LANGUAGES CXX RC)

if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME} ${CMAKE_BUILD_TYPE}")
endif()

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)

file(GLOB_RECURSE SOURCE_FILES
    CONFIGURE_DEPENDS
    "src/*.h"
    "src/*.cpp"
)

configure_file("src/Version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/Version.rc" @ONLY)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${SOURCE_FILES} "${CMAKE_CURRENT_BINARY_DIR}/Version.rc")

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_link_libraries(
    "${PROJECT_NAME}"
    PRIVATE
    glaze::glaze
)

if(DEFINED OUTPUT_FOLDER)
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE "/Zi")
        target_link_options(${PROJECT_NAME} PRIVATE "/DEBUG" "/OPT:REF" "/OPT:ICF")
    endif()

    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )
endif()
