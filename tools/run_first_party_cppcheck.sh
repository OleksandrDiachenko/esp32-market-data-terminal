#!/usr/bin/env bash

set -euo pipefail

compile_db=${1:-build/compile_commands.json}
if [[ ! -f "${compile_db}" ]]; then
    echo "compile database not found: ${compile_db}" >&2
    exit 2
fi

common_args=(
    "--project=${compile_db}"
    "--enable=warning,performance,portability"
    "--std=c11"
    "--inline-suppr"
    "--suppress=missingIncludeSystem"
    "--suppress=unusedFunction"
    "--suppress=*:*esp-idf*/components/*"
    "--suppress=*:*/managed_components/*"
    "--error-exitcode=1"
)

cppcheck "${common_args[@]}" "--file-filter=${PWD}/main/*"
cppcheck "${common_args[@]}" "--file-filter=${PWD}/components/*"
