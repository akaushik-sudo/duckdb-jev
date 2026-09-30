PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=snx_jev
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# The regression tests read answers back with the json functions
CORE_EXTENSIONS='json'

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
# Regression tests with the mock API running (see scripts/run_tests_with_mock.sh)
.PHONY: test_mock
test_mock:
	./scripts/run_tests_with_mock.sh
