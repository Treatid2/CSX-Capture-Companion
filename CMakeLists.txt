cmake_minimum_required(VERSION 3.28)

project(
    CSXCaptureCompanion
    VERSION 0.1.0
    LANGUAGES CXX
)

set(COMMONLIBSSE_SOURCE_DIR "" CACHE PATH "Optional CommonLibSSE-NG source tree")
if(COMMONLIBSSE_SOURCE_DIR)
    add_subdirectory(
        "${COMMONLIBSSE_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/commonlibsse-ng"
        EXCLUDE_FROM_ALL
    )
else()
    find_package(CommonLibSSE CONFIG REQUIRED)
endif()

add_commonlibsse_plugin(
    ${PROJECT_NAME}
    AUTHOR "Treatid2"
    SOURCES
        src/main.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX /permissive- /EHsc)
endif()

install(
    TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "SKSE/Plugins"
)
install(
    DIRECTORY Data/
    DESTINATION .
)
