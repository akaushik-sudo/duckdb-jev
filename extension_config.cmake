# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo. A release build (scripts/build-release.sh) stamps its version into
# the binary's metadata through SNX_JEV_VERSION; otherwise DuckDB derives one from git.
if(DEFINED ENV{SNX_JEV_VERSION} AND NOT "$ENV{SNX_JEV_VERSION}" STREQUAL "")
    duckdb_extension_load(snx_jev
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        EXTENSION_VERSION $ENV{SNX_JEV_VERSION}
    )
else()
    duckdb_extension_load(snx_jev
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    )
endif()

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)