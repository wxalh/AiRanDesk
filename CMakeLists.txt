cmake_minimum_required(VERSION 3.16)

project(AiRanDesk VERSION 3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable automoc, autouic, and autorcc for Qt project
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Find Qt modules
find_package(Qt5 COMPONENTS Core Gui Widgets WebSockets Network Concurrent Multimedia REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

# Source files
file(GLOB SRC_FILES
    "src/*.cpp"
    "src/common/*.cpp"
    "src/media/*.cpp"
    "src/util/*.cpp"
    "src/webrtc/*.cpp"
    "src/websocket/*.cpp"
)

file(GLOB HDR_FILES
    "src/*.h"
    "src/common/*.h"
    "src/media/*.h"
    "src/util/*.h"
    "src/webrtc/*.h"
    "src/websocket/*.h"
)

file(GLOB UI_FILES
    "src/*.ui"
)

# Platform-specific settings
if(WIN32)
    if(MSVC)
        message(STATUS "Windows + MSVC")
        # Add manifest for UAC
        set_source_files_properties(conf/uac.manifest PROPERTIES
            OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/conf/uac.manifest"
        )
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
    elseif(MINGW)
        message(STATUS "Windows + MinGW")
        set(RC_FILE conf/main.rc)
    endif()

    list(APPEND EXTRA_LIBS
        User32.lib
        Advapi32.lib
        Kernel32.lib
    )

    add_definitions(-DUNICODE -D_UNICODE)
    add_compile_options(/utf-8)
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/wd4819>")
elseif(APPLE)
    list(APPEND EXTRA_LIBS "-framework Carbon" "-framework CoreGraphics")
elseif(UNIX)
    list(APPEND EXTRA_LIBS Xtst X11)
endif()

# libdatachannel
if(WIN32)
    # Add executable
    add_executable(${PROJECT_NAME} WIN32
        ${SRC_FILES}
        ${HDR_FILES}
        ${UI_FILES}
        ${RC_FILE}
    )
else()
    # Add executable
    add_executable(${PROJECT_NAME}
        ${SRC_FILES}
        ${HDR_FILES}
        ${UI_FILES}
        ${RC_FILE}
    )
    # Add platform-specific libdatachannel here if needed
endif()

# Include directories
target_include_directories(${PROJECT_NAME}
    PRIVATE
    src
    src/common
    src/media
    src/util
    src/webrtc
    src/websocket
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
# target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
# Link libraries
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::WebSockets
    Qt5::Network
    Qt5::Concurrent
    Qt5::Multimedia
    spdlog::spdlog
    LibDataChannel::LibDataChannel
    ${FFMPEG_LIBRARIES}
    ${EXTRA_LIBS}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release"
)

# Copy config files after build

file(GLOB CONFIG_INIS
    "${CMAKE_CURRENT_SOURCE_DIR}/conf/*.ini"
)
foreach(f ${CONFIG_INIS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${f}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endforeach()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/locale"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/locale"
)
