# libapostol tests. Built with -DAPOSTOL_BUILD_TESTS=ON, run with ctest.

# The library takes its APP_* defaults from the application that builds it.
# Built on its own, it has none: the tests give it a set of their own.
if(PROJECT_IS_TOP_LEVEL)
    target_compile_definitions(apostol PUBLIC
        APP_NAME="apostol-test"
        APP_DESCRIPTION="libapostol test"
        APP_VERSION="${PROJECT_VERSION}"
        APP_DEFAULT_LOCALE="en_US.UTF-8"
        APP_VAR="apostol-test"
        APP_OLDPID_EXT=".oldbin"
        APP_DEFAULT_USER="nobody"
        APP_DEFAULT_GROUP="nogroup"
        APP_DEFAULT_LISTEN="127.0.0.1"
        APP_PREFIX="${CMAKE_CURRENT_BINARY_DIR}/"
        APP_CONF_PREFIX="conf/"
        APP_CACHE_PREFIX="cache/"
        APP_SBIN_PATH="sbin/apostol-test"
        APP_CONF_FILE="apostol-test.json"
        APP_PID_FILE="logs/apostol-test.pid"
        APP_LOCK_FILE="logs/apostol-test.lock"
        APP_ERROR_LOG_FILE="logs/error.log"
        APP_ACCESS_LOG_FILE="logs/access.log"
        APP_POSTGRES_LOG_FILE="logs/postgres.log"
        APP_STREAM_LOG_FILE="logs/stream.log"
        APP_DOC_ROOT="www"
        APP_DEFAULT_LOG_LEVEL="notice"
        APP_DEFAULT_LOG_MAX_SIZE=104857600
        APP_DEFAULT_LISTEN_BACKLOG=511
        APP_DEFAULT_WORKERS=1
        APP_DEFAULT_PORT=0
        APP_DEFAULT_SERVER_TIMEOUT=5000
        APP_DEFAULT_PG_CONNECT_TIMEOUT=5
    )
endif()

# ipc_test — a master, three workers, the helper and a custom process "probe"
# that drives the scenario and writes the verdict; no database, no HTTP.
add_executable(ipc_test ipc_test.cpp)
target_link_libraries(ipc_test PRIVATE apostol)
target_compile_options(ipc_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)

set(IPC_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/ipc_test.d")
add_test(NAME ipc COMMAND ipc_test -p "${IPC_TEST_DIR}/" -c "${IPC_TEST_DIR}/ipc_test.json")
set_tests_properties(ipc PROPERTIES TIMEOUT 120)
