#!/bin/bash

set -euo pipefail

PROJECT_NAME="${PROJECT_NAME:-$APP_NAME}"

set -o allexport
source /opt/$PROJECT_NAME/.env
set +o allexport

PGHOST="${PGHOST:-postgres}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-daemon}"
PGDATABASE="${PGDATABASE:-mydb}"

PG_PARAMS=(-d "$PGDATABASE" -U "$PGUSER")

if [ -n "$PGHOST" ]; then
  PG_PARAMS+=(-h "$PGHOST")
fi

if [ -n "$PGPORT" ]; then
  PG_PARAMS+=(-p "$PGPORT")
fi

# Logs are NOT cleared on start. The first version of this file deleted
# /var/log/$PROJECT_NAME/*.log here, so a log directory kept on a volume lost
# its history on every restart. libapostol appends (O_APPEND) and rotates by
# size — there is nothing to protect.

# Stale pid file from a run that ended in SIGKILL or a crash (a SIGTERM stop
# removes it itself). The path is "daemon.pid" from docker/conf/default.json.
# /run survives `docker restart`, and after the exec below the master is PID 1,
# so the file always holds 1: check_running() (src/core/application.cpp) has no
# "is this my own pid" guard and would refuse to start. Safe here: this script
# is PID 1 of a fresh pid namespace, nothing from the previous run exists.
rm -f /run/$APP_NAME.pid

display_message() {
  echo "$@"
}

display_error() {
  >&2 echo "$@"
}

display_configuration() {
  display_message "--------------------------------------------------------------------"
  display_message "PROJECT_NAME    : $PROJECT_NAME"
  display_message "WORKER_PROCESSES: $WORKER_PROCESSES"
  display_message "PGHOST          : $PGHOST"
  display_message "PGPORT          : $PGPORT"
  display_message "PGDATABASE      : $PGDATABASE"
  display_message "PGUSER          : $PGUSER"
  display_message "--------------------------------------------------------------------"
}

# The binary reads /etc/$PROJECT_NAME/$PROJECT_NAME.json — APP_PREFIX +
# APP_CONF_FILE (settings.cpp; not conf/, since 30789c7). The first version of
# this file put the config under conf/, where nothing looks: every container
# ran on the compiled-in defaults ("config file ... not found, using defaults").
init_app() {
  mkdir -p /etc/"$PROJECT_NAME"
  push_directory /opt/"$PROJECT_NAME"
  cp -p conf/default.json /etc/"$PROJECT_NAME"/"$PROJECT_NAME".json
  pop_directory
}

pop_directory() {
  popd >/dev/null
}

push_directory() {
  local DIRECTORY="$1"
  pushd "$DIRECTORY" >/dev/null
}

display_configuration

if [[ ! -f /etc/$PROJECT_NAME/$PROJECT_NAME.json ]]; then
  init_app
fi

display_message "Waiting for the database to be ready..."

retries=10
until pg_isready --timeout=1 "${PG_PARAMS[@]}" >/dev/null 2>&1; do
  sleep 1
  # not ((retries=retries-1)): under set -e that exits the script when the
  # value reaches 0, before the message below.
  retries=$((retries-1))
  if [ $retries -eq 0 ]; then
    display_error "Can't connect to database $PGDATABASE on $PGHOST:$PGPORT as user $PGUSER."
    exit 1
  fi
done

# exec: the application must be PID 1, or docker's SIGTERM stops a bash that
# never forwards it and everything gets SIGKILLed after 10 s — no remove_pid_file,
# no fast shutdown of the workers.
exec /usr/sbin/"$PROJECT_NAME" -w "$WORKER_PROCESSES"
