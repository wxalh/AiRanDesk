cmake_minimum_required(VERSION 3.16)

project(AiRanDesk VERSION 3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable automoc, autouic, and autorcc for Qt project
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${QT5_DIR}")

# Find Qt modules
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets WebSockets Network Concurrent Multimedia)

# ---------------------------
# OpenSSL (必须在 third_party 依赖之前配置，避免 vcpkg 干扰)
# ---------------------------
# Windows/MSVC：通常是 AMD64
if(WIN32)
    set(OPENSSL_USE_STATIC_LIBS OFF CACHE BOOL "" FORCE)
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include" CACHE PATH "Path to OpenSSL include directory" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto.lib" CACHE FILEPATH "Path to OpenSSL crypto library" FORCE)
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl.lib" CACHE FILEPATH "Path to OpenSSL SSL library" FORCE)
    # 立即查找一次 OpenSSL，创建 OpenSSL::SSL 和 OpenSSL::Crypto 目标
    # 这样后续 add_subdirectory(libdatachannel) 及其子依赖（libsrtp）就会复用这些目标，而不是重新查找
    find_package(OpenSSL REQUIRED)
    message(STATUS "[OpenSSL] Found version: ${OPENSSL_VERSION}")
    message(STATUS "[OpenSSL] Include: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "[OpenSSL] SSL Library: ${OPENSSL_SSL_LIBRARY}")
    message(STATUS "[OpenSSL] Crypto Library: ${OPENSSL_CRYPTO_LIBRARY}")
endif()

# ---------------------------
# third_party dependencies (source build)
# ---------------------------
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ===== spdlog =====
# 只编译依赖库：关闭安装/示例/测试/benchmark
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
# 使用spdlog自带fmt（避免额外引入fmt依赖）
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO OFF CACHE BOOL "" FORCE)
# 强制使用header-only fmt（spdlog内置 third_party/spdlog/include/spdlog/fmt/bundled）
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/spdlog)

# ===== libdatachannel =====
# 只编译依赖库：关闭示例/测试
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

# libdatachannel 自身也提供 WARNINGS_AS_ERRORS 选项
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

# 优先使用子模块（避免系统依赖探测），但加密库希望走 Qt 自带 OpenSSL
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(${THIRD_PARTY_DIR}/libdatachannel)
set(LIBDATACHANNEL_INCLUDE_DIR "${THIRD_PARTY_DIR}/libdatachannel/include" CACHE PATH "libdatachannel 头文件目录(包含 libdatachannel/*.h)")

# 这里在找到 OpenSSL 后为 datachannel 目标补齐 Crypto 依赖。
if(TARGET OpenSSL::Crypto)
    if(TARGET datachannel)
        target_link_libraries(datachannel PRIVATE OpenSSL::Crypto OpenSSL::SSL)
    endif()
    if(TARGET datachannel-static)
        target_link_libraries(datachannel-static PRIVATE OpenSSL::Crypto OpenSSL::SSL)
    endif()
endif()
# ===== FFmpeg =====
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT_DIR}/include" CACHE PATH "FFmpeg 头文件目录(包含 libavcodec 等子目录)")
set(FFMPEG_LIBRARY_DIR "${FFMPEG_ROOT_DIR}/lib" CACHE PATH "FFmpeg 库目录(包含 avcodec.lib/avformat.lib 等导入库)")
set(FFMPEG_BINARY_DIR "${FFMPEG_ROOT_DIR}/bin" CACHE PATH "FFmpeg 动态库目录(Windows下用于拷贝 DLL)")

# 典型最小链接集合（按需增减）
set(FFMPEG_LIBRARIES
    avcodec
    avformat
    avutil
    swscale
    swresample
    avdevice
)

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
        ws2_32.lib
        Iphlpapi.lib
        Secur32.lib
        Crypt32.lib
        Bcrypt.lib
    )

    add_definitions(-DUNICODE -D_UNICODE)
    add_compile_options(/utf-8)
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/wd4819>")
    
    # Disable SAFESEH linker requirement for MSVC x86 builds because some third-party
    # libraries (e.g., prebuilt FFmpeg) may not be SAFESEH-aware. This avoids LNK2026.
    if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(STATUS "MSVC x86 detected: disabling /SAFESEH requirement (adding /SAFESEH:NO)")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
    endif()

elseif(APPLE)
    list(APPEND EXTRA_LIBS "-framework Carbon" "-framework CoreGraphics")
elseif(UNIX)
    list(APPEND EXTRA_LIBS Xtst X11)
endif()

# Add executable
if(WIN32)
    add_executable(${PROJECT_NAME} WIN32
        ${SRC_FILES}
        ${HDR_FILES}
        ${UI_FILES}
        ${RC_FILE}
    )
else()
    add_executable(${PROJECT_NAME}
        ${SRC_FILES}
        ${HDR_FILES}
        ${UI_FILES}
        ${RC_FILE}
    )
endif()

# 立即为主可执行文件设置输出目录，确保不会落到 third_party/bin
if(UNIX AND NOT APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
    )
else()
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release"
    )
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
    ${FFMPEG_INCLUDE_DIR}
    ${LIBDATACHANNEL_INCLUDE_DIR}
)

target_link_directories(${PROJECT_NAME} PRIVATE "${FFMPEG_LIBRARY_DIR}" "${FFMPEG_BINARY_DIR}")

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
    datachannel-static
    ${FFMPEG_LIBRARIES}
    ${EXTRA_LIBS}
)

# 立即启用 origin 用于构建时 rpath（使 $ORIGIN 在 build_rpath 中起作用）
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_SKIP_RPATH OFF)

# 设置 RPATH，让程序优先从相对路径查找动态库
if(UNIX AND NOT APPLE)
    # Linux: 设置 RPATH 为 $ORIGIN（可执行文件所在目录）
    # 因为库文件会被拷贝到可执行文件目录，所以只需要 $ORIGIN
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# 设置可执行文件输出目录（兼容单/多配置）
if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/release"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/release"
    )
else()
    # 单配置生成器（如 Ninja）：根据 CMAKE_BUILD_TYPE 决定输出目录
    if(NOT CMAKE_BUILD_TYPE)
        # 默认到 release
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/debug")
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release")
    endif()
    # 也为 multi-config 生成器提供通用占位（不会影响单配置）
    set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<CONFIG>")
endif()

# Windows: 拷贝 FFmpeg DLL 到输出目录（如果存在）
if(WIN32)
    file(GLOB _FFMPEG_DLLS "${FFMPEG_BINARY_DIR}/*.dll")
    if(_FFMPEG_DLLS)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_FFMPEG_DLLS}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
    endif()

    # 拷贝 OpenSSL DLL（如果存在）
    if(OPENSSL_ROOT_DIR)
        file(GLOB _OPENSSL_DLLS
            "${OPENSSL_ROOT_DIR}/bin/*.dll"
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll"
            "${OPENSSL_ROOT_DIR}/bin/libssl-*.dll"
        )
        if(_OPENSSL_DLLS)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_OPENSSL_DLLS}
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            )
        endif()
    endif()

    # 拷贝 Qt DLL（使用 windeployqt 工具自动部署）
    get_target_property(_qt5_qmake_location Qt5::qmake IMPORTED_LOCATION)
    get_filename_component(_qt5_install_prefix "${_qt5_qmake_location}" DIRECTORY)
    get_filename_component(_qt5_install_prefix "${_qt5_install_prefix}" DIRECTORY)
    set(_qt5_windeployqt "${_qt5_install_prefix}/bin/windeployqt.exe")

    if(EXISTS "${_qt5_windeployqt}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${_qt5_windeployqt}"
            "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "正在使用 windeployqt 部署 Qt 依赖..."
        )
    else()
        message(WARNING "未找到 windeployqt 工具，Qt DLL 需要手动拷贝")
    endif()
endif()

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
