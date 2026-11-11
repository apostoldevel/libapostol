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

# ip_allow_list_test — IpAllowList in this process; no node, no files.
add_executable(ip_allow_list_test ip_allow_list_test.cpp)
target_link_libraries(ip_allow_list_test PRIVATE apostol)
target_compile_options(ip_allow_list_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME ip_allow_list COMMAND ip_allow_list_test)

# fetch_policy_test — FetchAddressPolicy in this process; no network, no files.
add_executable(fetch_policy_test fetch_policy_test.cpp)
target_link_libraries(fetch_policy_test PRIVATE apostol)
target_compile_options(fetch_policy_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME fetch_policy COMMAND fetch_policy_test)

# metrics_test — the metrics text, then a node of two workers, the helper and
# "probe": with enable_metrics() (on), and with no channels at all (off).
add_executable(metrics_test metrics_test.cpp)
target_link_libraries(metrics_test PRIVATE apostol)
target_compile_options(metrics_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)

foreach(mode on off)
    set(METRICS_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/metrics_test_${mode}.d")
    add_test(NAME metrics_${mode}
             COMMAND metrics_test --mode ${mode} -p "${METRICS_TEST_DIR}/" -c "${METRICS_TEST_DIR}/metrics_test.json")
    set_tests_properties(metrics_${mode} PROPERTIES TIMEOUT 60)
endforeach()

# ws_client_test — the port of a WsClient URL against a listener on 127.0.0.1.
add_executable(ws_client_test ws_client_test.cpp)
target_link_libraries(ws_client_test PRIVATE apostol)
target_compile_options(ws_client_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME ws_client COMMAND ws_client_test)
set_tests_properties(ws_client PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# url_test — how HttpClient and WsClient read a URL, against listeners on
# 127.0.0.1; url_ipv6 — on [::1], skipped where [::1] is not reachable.
add_executable(url_test url_test.cpp)
target_link_libraries(url_test PRIVATE apostol)
target_compile_options(url_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME url COMMAND url_test)
add_test(NAME url_ipv6 COMMAND url_test --ipv6)
set_tests_properties(url url_ipv6 PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# accept_balance_test — the slot set aside for a worker before its fork: the
# table in this process, then (edge-triggered build) a master with three
# workers and a custom process "probe"; a level-triggered build runs the table only.
add_executable(accept_balance_test accept_balance_test.cpp)
target_link_libraries(accept_balance_test PRIVATE apostol)
target_compile_options(accept_balance_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)

set(ACCEPT_BALANCE_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/accept_balance_test.d")
add_test(NAME accept_balance
         COMMAND accept_balance_test -p "${ACCEPT_BALANCE_TEST_DIR}/" -c "${ACCEPT_BALANCE_TEST_DIR}/accept_balance_test.json")
set_tests_properties(accept_balance PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# path_under_root_test — path_under_root() and ApostolModule::try_files() on a
# temporary tree (mkdtemp): paths under the root are served as
# before, paths that do not stay under it are refused.
add_executable(path_under_root_test path_under_root_test.cpp)
target_link_libraries(path_under_root_test PRIVATE apostol)
target_compile_options(path_under_root_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME path_under_root COMMAND path_under_root_test)
