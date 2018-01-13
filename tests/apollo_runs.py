#!/usr/bin/env python3
import json
import os
import re
import subprocess

# paths
SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
WORKSPACE_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", ".."))
TESTS_DIR_REL = os.path.join("..", "build", "tests")
TESTS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, TESTS_DIR_REL))

# generation mode
mode = "release"
if os.environ.get("PROJECT", "") == "mg-master-diff":
    mode = "diff"

# ctest tests
ctest_output = subprocess.run(["ctest", "-N"], cwd=TESTS_DIR, check=True,
        stdout=subprocess.PIPE).stdout.decode("utf-8")
tests = []

# test ordering: first unit, then concurrent, then everything else
CTEST_ORDER = {"unit": 0, "concurrent": 1}
CTEST_DELIMITER = "__"
for row in ctest_output.split("\n"):
    # Filter rows only containing tests.
    if not re.match("^\s*Test\s+#", row): continue
    test_name = row.split(":")[1].strip()
    name = test_name.replace("memgraph" + CTEST_DELIMITER, "")
    path = os.path.join(TESTS_DIR_REL, name.replace(CTEST_DELIMITER, "/", 1))
    order = CTEST_ORDER.get(name.split(CTEST_DELIMITER)[0], len(CTEST_ORDER))
    tests.append((order, name, path))

tests.sort()

runs = []
for test in tests:
    order, name, path = test
    dirname, basename = os.path.split(path)
    cmakedir = os.path.join("CMakeFiles",
            "memgraph" + CTEST_DELIMITER + name + ".dir")
    files = [basename, cmakedir]

    # extra files for specific tests
    if name == "unit__fswatcher":
        files.append(os.path.join("..", "data"))

    # skip benchmark tests on diffs
    if name.startswith("benchmark") and mode == "diff":
        continue

    # larger timeout for benchmark tests
    prefix = ""
    if name.startswith("benchmark"):
        prefix = "TIMEOUT=600 "

    outfile_paths = []
    if name.startswith("unit"):
        cmakedir_abs = os.path.join(TESTS_DIR, "unit", cmakedir)
        cmakedir_rel = os.path.relpath(cmakedir_abs, WORKSPACE_DIR)
        outfile_paths.append("\./" + cmakedir_rel.replace(".", "\\.") + ".+")

    runs.append({
        "name": name,
        "cd": dirname,
        "commands": prefix + "./" + basename,
        "infiles": files,
        "outfile_paths": outfile_paths,
    })

print(json.dumps(runs, indent=4, sort_keys=True))