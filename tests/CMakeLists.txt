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

# ipc_frames_test — datagrams no sender of the library writes, and a request
# with no descriptor left; a master and two workers, one case per test.
add_executable(ipc_frames_test ipc_frames_test.cpp)
target_link_libraries(ipc_frames_test PRIVATE apostol)
target_compile_options(ipc_frames_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
foreach(case empty_datagram part_flood no_timer)
    add_test(NAME ipc_frames_${case}
             COMMAND ipc_frames_test ${case} "${CMAKE_CURRENT_BINARY_DIR}/ipc_frames_${case}.d")
    set_tests_properties(ipc_frames_${case} PROPERTIES TIMEOUT 90)
endforeach()

# config_list_test — config_list(): the forms of a list in the configuration.
add_executable(config_list_test config_list_test.cpp)
target_link_libraries(config_list_test PRIVATE apostol)
target_compile_options(config_list_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME config_list COMMAND config_list_test)

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

# http_split_test — HttpParser and HttpResponseParser give the same headers,
# body and handler call however the bytes of a message are split.
add_executable(http_split_test http_split_test.cpp)
target_link_libraries(http_split_test PRIVATE apostol)
target_compile_options(http_split_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME http_split COMMAND http_split_test)

# http_send_file_test — send_file(): the content type cannot add a header line;
# a file cut while it is sent closes the connection after the short body.
add_executable(http_send_file_test http_send_file_test.cpp)
target_link_libraries(http_send_file_test PRIVATE apostol)
target_compile_options(http_send_file_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME http_send_file COMMAND http_send_file_test)

# http_serve_file_test — try_files() answers with the file as the body and the
# connection sends it from the file: memory, the bytes on the wire, HEAD.
add_executable(http_serve_file_test http_serve_file_test.cpp)
target_link_libraries(http_serve_file_test PRIVATE apostol)
target_compile_options(http_serve_file_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME http_serve_file COMMAND http_serve_file_test)

# debug_mask_test — masking of the _DEBUG request/response dump, in this
# process: what is not recognised is not written; credentials masked by name.
add_executable(debug_mask_test debug_mask_test.cpp)
target_link_libraries(debug_mask_test PRIVATE apostol)
target_compile_options(debug_mask_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME debug_mask COMMAND debug_mask_test)

# content_json_test — content_to_json() on JSON bodies nested at, over and far
# over the depth limit, brackets in strings, broken JSON, form and query; in
# this process, no files.
add_executable(content_json_test content_json_test.cpp)
target_link_libraries(content_json_test PRIVATE apostol)
target_compile_options(content_json_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME content_json COMMAND content_json_test)

# pg_nul_test — a statement with a NUL byte is refused by PgPool::execute() and
# listener_execute() from the loop, never sent; no database (a pool whose server
# does not exist), the same calls without the byte as the control.
add_executable(pg_nul_test pg_nul_test.cpp)
target_link_libraries(pg_nul_test PRIVATE apostol)
target_compile_options(pg_nul_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME pg_nul COMMAND pg_nul_test)
set_tests_properties(pg_nul PROPERTIES TIMEOUT 30)

# pg_queue_test — the bound and the age of PgPool's queue: refused past the
# limit, failed unsent past the timeout; a child process plays a server that
# finishes the handshake late, to show nothing stale reaches it.
add_executable(pg_queue_test pg_queue_test.cpp)
target_link_libraries(pg_queue_test PRIVATE apostol)
target_compile_options(pg_queue_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME pg_queue COMMAND pg_queue_test)
set_tests_properties(pg_queue PROPERTIES TIMEOUT 60)

# pg_resolve_test — PgPool and PgLeaderLock look host names up off the loop:
# getaddrinfo() is replaced in the test binary (exported, so libpq reaches it).
add_executable(pg_resolve_test pg_resolve_test.cpp)
target_link_libraries(pg_resolve_test PRIVATE apostol dl)
set_target_properties(pg_resolve_test PROPERTIES ENABLE_EXPORTS ON)
target_compile_options(pg_resolve_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME pg_resolve COMMAND pg_resolve_test)
set_tests_properties(pg_resolve PROPERTIES TIMEOUT 60)

# service_token_test — ServiceToken's retry schedule against a clock the test
# moves (steady_clock::now() is defined in the test binary), and how
# refresh_service_token() reads daemon.token's answer from a child process
# that plays the server.
add_executable(service_token_test service_token_test.cpp)
target_link_libraries(service_token_test PRIVATE apostol)
target_compile_options(service_token_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME service_token COMMAND service_token_test)
set_tests_properties(service_token PROPERTIES TIMEOUT 60)

# bot_session_test — BotSession's outcome of a sign-in that finds no session,
# and the pool's handlers of BotSession and refresh_service_token() once their
# owner is destroyed with a request in flight; a child process plays the server.
add_executable(bot_session_test bot_session_test.cpp)
target_link_libraries(bot_session_test PRIVATE apostol)
target_compile_options(bot_session_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME bot_session COMMAND bot_session_test)
set_tests_properties(bot_session PROPERTIES TIMEOUT 60)

# pg_server_test — PgPool against a scripted server in a child process:
# cancel() of a finished query, a listener handler that throws, a listener
# statement larger than the socket takes at once.
add_executable(pg_server_test pg_server_test.cpp)
target_link_libraries(pg_server_test PRIVATE apostol)
target_compile_options(pg_server_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME pg_server COMMAND pg_server_test)
set_tests_properties(pg_server PROPERTIES TIMEOUT 60)

# http_framing_test — one Content-Length per response; content_length() of a
# value that does not fit; ".." segments in is_safe_path(); query decoding;
# "Expect: 100-continue" answered before the body, after the answers due.
add_executable(http_framing_test http_framing_test.cpp)
target_link_libraries(http_framing_test PRIVATE apostol)
target_compile_options(http_framing_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME http_framing COMMAND http_framing_test)

# The tests below talk to a scripted peer on 127.0.0.1 (net_test_util.hpp);
# those that need a TLS server or a WebSocket handshake use OpenSSL.
# tls_client_test — host name check of a verified TcpClient, STARTTLS for one
# connection, HttpProxy to a TLS upstream; certificates made at run time.
add_executable(tls_client_test tls_client_test.cpp)
target_link_libraries(tls_client_test PRIVATE apostol)
target_compile_options(tls_client_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME tls_client COMMAND tls_client_test)
set_tests_properties(tls_client PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# http_proxy_test — a forward() that fails at once is reported from the loop.
add_executable(http_proxy_test http_proxy_test.cpp)
target_link_libraries(http_proxy_test PRIVATE apostol)
target_compile_options(http_proxy_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME http_proxy COMMAND http_proxy_test)
set_tests_properties(http_proxy PROPERTIES TIMEOUT 30)

# smtp_client_test — SmtpClient against a relay that does STARTTLS: envelope
# addresses, data after the 220, an endless reply line, two sends in a row.
add_executable(smtp_client_test smtp_client_test.cpp)
target_link_libraries(smtp_client_test PRIVATE apostol)
target_compile_options(smtp_client_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME smtp_client COMMAND smtp_client_test)
set_tests_properties(smtp_client PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# base64_test — base64_decode(): padding optional, data after it or a third
# '=' refused; round trip of every length to 255.
add_executable(base64_test base64_test.cpp)
target_link_libraries(base64_test PRIVATE apostol)
target_compile_options(base64_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME base64 COMMAND base64_test)

# ws_close_test — WsClient after a refused upgrade, and the close handshake
# with a silent, a dropping and an answering server.
add_executable(ws_close_test ws_close_test.cpp)
target_link_libraries(ws_close_test PRIVATE apostol)
target_compile_options(ws_close_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME ws_close COMMAND ws_close_test)
set_tests_properties(ws_close PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# ws_message_client_test — WsMessageClient links with WsMessage (one
# definition of to_json/from_json); an unanswered request is reported.
add_executable(ws_message_client_test ws_message_client_test.cpp)
target_link_libraries(ws_message_client_test PRIVATE apostol)
target_compile_options(ws_message_client_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME ws_message_client COMMAND ws_message_client_test)
set_tests_properties(ws_message_client PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# fetch_limit_test — the response size limit of HttpClient, CurlClient and
# FetchClient against servers that never stop sending.
add_executable(fetch_limit_test fetch_limit_test.cpp)
target_link_libraries(fetch_limit_test PRIVATE apostol)
target_compile_options(fetch_limit_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME fetch_limit COMMAND fetch_limit_test)
set_tests_properties(fetch_limit PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# process_model_test — the master and a failed fork, the pid file, a daemon's
# umask, a failed privilege switch, the crash report; one case per test.
add_executable(process_model_test process_model_test.cpp)
target_link_libraries(process_model_test PRIVATE apostol)
target_compile_options(process_model_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
foreach(case fork_retry own_pid pid_mode privilege crash_fd crash_reload
             worker_sigint child_fds environ args daemon_refused
             fork_retry_nofd reload_spawn privilege_stop)
    add_test(NAME process_model_${case}
             COMMAND process_model_test ${case} "${CMAKE_CURRENT_BINARY_DIR}/process_model_${case}.d")
    set_tests_properties(process_model_${case} PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
endforeach()

# logger_test — FileTarget when a reopen or a compression is refused; no node.
add_executable(logger_test logger_test.cpp)
target_link_libraries(logger_test PRIVATE apostol ${CMAKE_DL_LIBS})
target_compile_options(logger_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME logger COMMAND logger_test "${CMAKE_CURRENT_BINARY_DIR}/logger_test.d")
set_tests_properties(logger PROPERTIES SKIP_RETURN_CODE 77)

# settings_range_test — numbers out of their setting's range; no node, no files.
add_executable(settings_range_test settings_range_test.cpp)
target_link_libraries(settings_range_test PRIVATE apostol)
target_compile_options(settings_range_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME settings_range COMMAND settings_range_test)

# yaml_writer_test — json_to_yaml read back by a YAML parser (python3 yaml).
add_executable(yaml_writer_test yaml_writer_test.cpp)
target_link_libraries(yaml_writer_test PRIVATE apostol)
target_compile_options(yaml_writer_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME yaml_writer COMMAND yaml_writer_test "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(yaml_writer PROPERTIES SKIP_RETURN_CODE 77)

# file_logger_test — stream_logger() and file_logger() in a master, a worker,
# the helper and a custom process: the configured path, reopened on SIGUSR1.
add_executable(file_logger_test file_logger_test.cpp)
target_link_libraries(file_logger_test PRIVATE apostol)
target_compile_options(file_logger_test PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_test(NAME file_logger COMMAND file_logger_test "${CMAKE_CURRENT_BINARY_DIR}/file_logger_test.d")
set_tests_properties(file_logger PROPERTIES TIMEOUT 60)
