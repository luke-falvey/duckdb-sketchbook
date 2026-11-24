PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=sketchbook
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# extension-ci-tools v2.0-cyanoptera builds wasm_threads extensions that DuckDB-WASM's threads bundle
# cannot load (dlopen fails with an empty error; DuckDB's own icu extension built the same way fails
# too). It now passes -DUSE_WASM_THREADS=ON (DuckDB's switch that links a side module with shared
# memory), but still leaves out -fwasm-exceptions: its WASM_CXX_THREADS_FLAGS names
# WASM_COMPILE_TIME_EH_FLAGS, which is not defined. Remove this line when the release branch has the
# fix. See docs/DESIGN.md section 21.
WASM_CXX_THREADS_FLAGS=$(WASM_CXX_EH_FLAGS) -DWITH_WASM_THREADS=1 -DWITH_WASM_SIMD=1 -DWITH_WASM_BULK_MEMORY=1 -pthread

# `make core_test`: fast tests that need no DuckDB build.
include core.Makefile
