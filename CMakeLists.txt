cmake_minimum_required(VERSION 3.25)
project(libapostol VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Feature flags ───────────────────────────────────────────────────────────
option(WITH_POSTGRESQL "Build with PostgreSQL (libpq)" ON)
option(WITH_SSL        "Build with OpenSSL"            ON)
option(WITH_CURL       "Build with libcurl"            ON)

# ─── Dependencies (FetchContent) ─────────────────────────────────────────────

include(FetchContent)

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# {fmt} (string formatting, C++20 compatible, replaces std::format)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.1.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(fmt)

# llhttp (HTTP parser)
FetchContent_Declare(
    llhttp
    URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.2.1.tar.gz
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(llhttp)
unset(BUILD_STATIC_LIBS CACHE)

# ─── System libraries ────────────────────────────────────────────────────────

if(WITH_SSL)
    find_package(OpenSSL REQUIRED)
endif()

if(WITH_CURL)
    find_package(CURL REQUIRED)
endif()

if(WITH_POSTGRESQL)
    find_library(LIBPQ_LIB pq REQUIRED)
    find_path(LIBPQ_INCLUDE libpq-fe.h PATH_SUFFIXES postgresql)
endif()

find_library(ZLIB_LIB z REQUIRED)

# ─── Core library ────────────────────────────────────────────────────────────

add_library(apostol STATIC
    src/log/logger.cpp
    src/config/config.cpp
    src/core/event_loop.cpp
    src/core/process.cpp
    src/core/module.cpp
    src/core/apostol_module.cpp
    src/core/application.cpp
    src/core/crash_handler.cpp
    src/core/settings.cpp
    src/net/tcp.cpp
    src/net/http.cpp
    src/net/websocket.cpp
    src/net/udp.cpp
    src/net/tcp_client.cpp
    src/net/udp_client.cpp
    src/core/base64.cpp
    src/core/file_utils.cpp
    src/core/http_utils.cpp
    src/core/http_client.cpp
    src/core/fetch_client.cpp
    src/core/route_manager.cpp
    src/core/routed_module.cpp
    src/core/yaml_writer.cpp
    src/core/http_proxy.cpp
    src/core/oauth_providers.cpp
    src/core/site_config.cpp
    src/net/ws_client.cpp
)

if(WITH_POSTGRESQL)
    target_sources(apostol PRIVATE
        src/pg/pg.cpp
        src/pg/pg_utils.cpp
        src/pg/pg_exec.cpp
        src/core/bot_session.cpp
    )
endif()

if(WITH_SSL)
    target_sources(apostol PRIVATE
        src/core/jwt.cpp
        src/core/smtp_client.cpp
    )
endif()

if(WITH_CURL)
    target_sources(apostol PRIVATE src/core/curl_client.cpp)
endif()

target_include_directories(apostol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        $<$<BOOL:${LIBPQ_INCLUDE}>:${LIBPQ_INCLUDE}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lib    # jwt-cpp/jwt.h
)

# Feature flags as PUBLIC compile definitions
target_compile_definitions(apostol PUBLIC
    $<$<BOOL:${WITH_POSTGRESQL}>:WITH_POSTGRESQL>
    $<$<BOOL:${WITH_SSL}>:WITH_SSL>
    $<$<BOOL:${WITH_CURL}>:WITH_CURL>
)

target_compile_options(apostol PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

target_link_libraries(apostol
    PUBLIC
        nlohmann_json::nlohmann_json
        fmt::fmt
        $<$<BOOL:${WITH_SSL}>:OpenSSL::SSL>
        $<$<BOOL:${WITH_SSL}>:OpenSSL::Crypto>
        $<$<BOOL:${WITH_CURL}>:CURL::libcurl>
        $<$<BOOL:${WITH_POSTGRESQL}>:${LIBPQ_LIB}>
        ${ZLIB_LIB}
        llhttp_static
        dl      # crash_handler: backtrace_symbols, abi::__cxa_demangle
)
