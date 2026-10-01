#!/usr/bin/env python3
"""Loads a release binary into a stock duckdb and checks it is what we ship.

    python3 scripts/verify-release.py <path/to/snx_jev.duckdb_extension> <version> <platform>

Run against the file exactly as it is shipped, name included: DuckDB derives the extension's
entry point from the file name (up to the first '.'), so a renamed file does not load at all.
Needs `pip install duckdb==1.5.4` (the DuckDB the binary is built for).
"""
import os
import sys

import duckdb

path, version, platform = sys.argv[1], sys.argv[2], sys.argv[3]
assert os.path.basename(path) == "snx_jev.duckdb_extension", (
    "the shipped file must be named snx_jev.duckdb_extension, not " + os.path.basename(path))

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute(f"LOAD '{path}'")
actual_platform = con.execute("PRAGMA platform").fetchone()[0]
extension_version = con.execute(
    "SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'snx_jev'").fetchone()[0]
jev_version = con.execute("SELECT jev_version()").fetchone()[0]
functions = sorted(r[0] for r in con.execute(
    "SELECT DISTINCT function_name FROM duckdb_functions() "
    "WHERE function_name LIKE 'snx_%' OR function_name LIKE 'jev%'").fetchall())
print(f"{path}: loads into duckdb {duckdb.__version__} on {actual_platform}; extension_version={extension_version} "
      f"jev_version()={jev_version} functions={functions}")
assert actual_platform == platform, actual_platform
assert extension_version == "v" + version, extension_version
assert jev_version == version, jev_version
assert functions == ["jev_version", "snx_jev_last_query_stats", "snx_prompt_intent"], functions
