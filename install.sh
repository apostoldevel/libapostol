#!/bin/bash
#
# install.sh — Create a new project from the libapostol template.
#
# Usage:
#   bash <(curl -sL https://raw.githubusercontent.com/apostoldevel/libapostol/master/install.sh)
#   # or locally:
#   bash install.sh

set -euo pipefail

REPO_URL="https://github.com/apostoldevel/libapostol"

# --- 1. Clone libapostol to temp dir -------------------------------------

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "Cloning libapostol..."
git clone --depth 1 "$REPO_URL" "$TMPDIR/libapostol" 2>/dev/null

# --- 2. Ask user ----------------------------------------------------------

read -p "Project name [myapp]: " APP_NAME
APP_NAME=${APP_NAME:-myapp}

read -p "Project description [A project based on the Apostol framework.]: " APP_DESCRIPTION
APP_DESCRIPTION=${APP_DESCRIPTION:-A project based on the Apostol framework.}

# --- 3. Create project directory ------------------------------------------

TARGET="$(pwd)/$APP_NAME"
if [ -d "$TARGET" ]; then
    echo "Error: directory '$APP_NAME' already exists"
    exit 1
fi

echo "Creating project '$APP_NAME'..."

# --- 4. Copy template -----------------------------------------------------

cp -r "$TMPDIR/libapostol/template" "$TARGET"

# --- 5. Copy libapostol into src/lib/ -------------------------------------

mkdir -p "$TARGET/src/lib/libapostol"
cp -r "$TMPDIR/libapostol"/{CMakeLists.txt,README.md,README.ru-RU.md,include,src} \
      "$TARGET/src/lib/libapostol/"

# --- 6. Replace placeholders ---------------------------------------------

# Convert APP_NAME to PascalCase for class name
PASCAL_NAME=$(echo "$APP_NAME" | sed -r 's/(^|[-_ ])([a-z])/\U\2/g')
APP_CLASS="${PASCAL_NAME}App"

find "$TARGET" -type f \( \
    -name '*.cpp' -o -name '*.hpp' -o -name '*.txt' \
    -o -name '*.json' -o -name '*.yaml' -o -name '*.sh' -o -name '*.service' \
    -o -name '*.conf' -o -name '*.env*' -o -name 'Dockerfile' -o -name 'configure' \
    -o -name '.gitignore' -o -name '.dockerignore' -o -name 'deploy' \
    \) -exec sed -i \
    -e "s/\\\$APP_NAME/$APP_NAME/g" \
    -e "s/\\\$APP_DESCRIPTION/$APP_DESCRIPTION/g" \
    -e "s/\\\$APP_CLASS/$APP_CLASS/g" \
    {} +

# Make scripts executable
chmod +x "$TARGET/configure"
chmod +x "$TARGET/deploy"
chmod +x "$TARGET/docker/entrypoint.sh"

# --- 7. Init git ----------------------------------------------------------

cd "$TARGET"
git init -q
git add .
git commit -q -m "init: $APP_NAME project from libapostol template"

# --- 8. Success message ---------------------------------------------------

echo ""
echo "Project '$APP_NAME' created in $TARGET"
echo ""
echo "To configure and build:"
echo "  cd $APP_NAME"
echo "  ./configure --debug"
echo "  cmake --build cmake-build-debug --parallel \$(nproc)"
echo ""
echo "With PostgreSQL:"
echo "  ./configure --debug --with-postgresql"
echo "  cmake --build cmake-build-debug --parallel \$(nproc)"
echo ""
echo "Run (after changing \"prefix\" to \".\" in conf/default.json):"
echo "  mkdir -p logs"
echo "  ./cmake-build-debug/$APP_NAME -p . -c conf/default.json"
echo "  curl http://localhost:4977/api/v1/ping"
echo "  curl http://localhost:4977/docs"
echo ""
echo "NOTE: First configure downloads dependencies via CMake FetchContent"
echo "      and may take 1-2 minutes depending on your network speed."
