cmake_minimum_required(VERSION 3.28)

project(
    CSXCaptureCompanion
    VERSION 0.1.1
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
        src/CaptureSession.cpp
        src/VideoComposer.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        mfplat
        mfreadwrite
        mfuuid
        windowscodecs
        ole32
)

if(MSVC)
    target_compile_options(
        ${PROJECT_NAME}
        PRIVATE
            /W4
            /WX
            /permissive-
            /EHsc
            /utf-8
            "$<$<CONFIG:Release>:/Zi>"
    )
    target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG:FULL>")
endif()

install(
    TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "SKSE/Plugins"
)
install(
    FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
    DESTINATION "SKSE/Plugins"
    OPTIONAL
)
install(
    DIRECTORY Data/
    DESTINATION .
)
install(
    FILES
        LICENSE
        README.md
    DESTINATION "Docs/CSX Capture Companion"
)
install(
    DIRECTORY docs/legal/
    DESTINATION "Docs/CSX Capture Companion/Legal"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(
        CSXCaptureComposerSmoke
        tests/ComposerSmoke.cpp
        src/VideoComposer.cpp
    )
    target_compile_features(CSXCaptureComposerSmoke PRIVATE cxx_std_23)
    target_include_directories(CSXCaptureComposerSmoke PRIVATE include)
    target_compile_definitions(CSXCaptureComposerSmoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_precompile_headers(CSXCaptureComposerSmoke PRIVATE include/PCH.h)
    target_link_libraries(
        CSXCaptureComposerSmoke
        PRIVATE
            CommonLibSSE::CommonLibSSE
            mfplat
            mfreadwrite
            mfuuid
            windowscodecs
            ole32
    )
    if(MSVC)
        target_compile_options(CSXCaptureComposerSmoke PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    endif()
    add_test(
        NAME composer-smoke
        COMMAND
            pwsh -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/Run-ComposerSmoke.ps1"
            -Executable "$<TARGET_FILE:CSXCaptureComposerSmoke>"
            -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/composer-smoke"
            -AllowedRoot "${CMAKE_CURRENT_BINARY_DIR}"
    )

    add_executable(
        CSXCaptureSessionTests
        tests/CaptureSessionTests.cpp
        src/CaptureSession.cpp
    )
    target_compile_features(CSXCaptureSessionTests PRIVATE cxx_std_23)
    target_include_directories(CSXCaptureSessionTests PRIVATE include)
    target_compile_definitions(CSXCaptureSessionTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_precompile_headers(CSXCaptureSessionTests PRIVATE include/PCH.h)
    target_link_libraries(CSXCaptureSessionTests PRIVATE CommonLibSSE::CommonLibSSE)
    if(MSVC)
        target_compile_options(CSXCaptureSessionTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME capture-session COMMAND CSXCaptureSessionTests)
endif()
