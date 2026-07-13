#!/usr/bin/env bash

set -euo pipefail

compile_db=${1:-build/compile_commands.json}
if [[ ! -f "${compile_db}" ]]; then
    echo "compile database not found: ${compile_db}" >&2
    exit 2
fi

# The ESP-IDF build can run in a container whose mount path differs from
# this step's checkout path (e.g. espressif/esp-idf-ci-action), and it
# always includes one project_elf_src_*.c entry for a linker-generated
# stub that never exists as a real source file. cppcheck's --project
# loader aborts entirely on the first compile-database entry whose file
# is missing, so rewrite each entry's path to this checkout and drop any
# entry that still doesn't resolve, before cppcheck ever sees the file.
# The filtered copy is written outside build/ since that directory (and
# its contents) can be root-owned/read-only when the build itself ran in
# a container image running as a different user than this step.
filtered_dir=$(mktemp -d)
trap 'rm -rf "${filtered_dir}"' EXIT
filtered_db="${filtered_dir}/compile_commands.first_party.json"
python3 - "${compile_db}" "${filtered_db}" "${PWD}" <<'PY'
import json
import os
import sys

src_path, dst_path, repo_root = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src_path) as f:
    entries = json.load(f)

repo_name = os.path.basename(repo_root.rstrip("/"))
marker = f"/{repo_name}/"
kept = []
for entry in entries:
    file_path = entry.get("file", "")
    idx = file_path.find(marker)
    resolved = os.path.join(repo_root, file_path[idx + len(marker):]) if idx != -1 else file_path
    if not os.path.isfile(resolved):
        continue
    entry["file"] = resolved
    directory = entry.get("directory", "")
    didx = directory.find(marker)
    if didx != -1:
        entry["directory"] = os.path.join(repo_root, directory[didx + len(marker):]) or repo_root
    kept.append(entry)

with open(dst_path, "w") as f:
    json.dump(kept, f)
PY
compile_db="${filtered_db}"

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
