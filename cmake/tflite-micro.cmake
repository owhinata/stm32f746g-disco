# tflite-micro (TFLM) C++ backend for CONFIG_NN_BACKEND=tflm (Epic #80 P3, issue #86).
#
# Included ONLY from the tflm branch of CMakeLists.txt, so C++ is enabled for this
# build alone; the default (null/stedgeai) firmware never sees enable_language(CXX)
# and stays byte-identical.  All C++ is confined to this `tflm` STATIC lib so the
# final threadx link keeps the C driver (gcc) and does not auto-pull full libstdc++.
#
# The tflite-micro sources are NOT vendored.  At CONFIGURE time we fetch the pinned
# upstream commit and run its own `create_tflm_tree.py` (which drives the tflite-micro
# Makefile to list library sources, download the header-only third-party deps
# flatbuffers/gemmlowp/ruy, and generate the model-data C array) into a self-contained,
# glob-able tree under the build dir.  We then compile that tree with OUR exact flags
# (so the ABI -- cortex-m7 / fpv5-sp-d16 / hard / -fno-exceptions -- is fully ours).
# This mirrors the repo's "download the toolchain at configure" convention; only the
# tflm build pays the fetch cost, and it is cached (SHA-keyed stamp) after the first run.

enable_language(CXX)

# --- Pinned upstream ---------------------------------------------------------
set(TFLM_GIT_URL "https://github.com/tensorflow/tflite-micro.git")
set(TFLM_GIT_SHA "e142972d4f4382f77faa8212b7c70b37d28b9cb9")   # tflite-micro main, 2026-07
set(TFLM_SRC  "${CMAKE_BINARY_DIR}/tflm-src")     # shallow clone at the pinned SHA
set(TFLM_ROOT "${CMAKE_BINARY_DIR}/tflm-tree")    # generated self-contained source tree
set(TFLM_STAMP "${TFLM_ROOT}/.tflm-generated-${TFLM_GIT_SHA}")
set(TFLM_GEN   "${TFLM_SRC}/tensorflow/lite/micro/tools/project_generation/create_tflm_tree.py")

# --- Python venv (numpy/Pillow are required by TFLM's Makefile evaluation) ----
# Create the venv if missing here; the dependency INSTALL happens in the generation
# block below (not gated on venv creation) so a pre-existing venv that predates the
# numpy/Pillow requirement -- e.g. a docs-only .venv, or one left half-installed by an
# interrupted run -- gets healed instead of silently skipped.
set(TFLM_VENV "${CMAKE_SOURCE_DIR}/.venv")
set(TFLM_PY   "${TFLM_VENV}/bin/python3")
if(NOT EXISTS "${TFLM_PY}")
    find_program(TFLM_HOST_PY NAMES python3 python REQUIRED)
    message(STATUS "tflm: creating ${TFLM_VENV}")
    execute_process(COMMAND "${TFLM_HOST_PY}" -m venv "${TFLM_VENV}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: python venv creation failed (need python3 + venv)")
    endif()
endif()

# --- Fetch + generate the tree (SHA-keyed stamp: only runs once per pin) ------
# The stamp is written ONLY after a fully successful generation, and each attempt starts
# from a clean clone + a fresh dependency install, so an interrupted run self-heals on
# the next configure (no stale partial third-party downloads, no half-populated venv).
if(NOT EXISTS "${TFLM_STAMP}")
    message(STATUS "tflm: fetching tflite-micro @ ${TFLM_GIT_SHA} and generating the source tree "
                   "(one-time, a few minutes -- clones + downloads flatbuffers/gemmlowp/ruy)")

    # Ensure numpy/Pillow are present (idempotent when already satisfied); heals a venv
    # created before these were added to requirements.txt.
    execute_process(
        COMMAND "${TFLM_VENV}/bin/pip" install -q --disable-pip-version-check
                -r "${CMAKE_SOURCE_DIR}/requirements.txt"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: pip install -r requirements.txt failed")
    endif()

    # Fresh clone each attempt: upstream's third-party extractor skips a download dir
    # that merely exists, so a previously interrupted download would never self-repair
    # in place.  Wiping the clone also avoids any stale remote/HEAD idempotency issues.
    file(REMOVE_RECURSE "${TFLM_SRC}")
    file(MAKE_DIRECTORY "${TFLM_SRC}")
    execute_process(COMMAND git init -q WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git init failed")
    endif()
    execute_process(COMMAND git remote add origin "${TFLM_GIT_URL}"
                    WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git remote add failed")
    endif()
    execute_process(COMMAND git fetch --depth 1 -q origin "${TFLM_GIT_SHA}"
                    WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git fetch ${TFLM_GIT_SHA} failed (network + reachable SHA needed)")
    endif()
    execute_process(COMMAND git checkout -q -f FETCH_HEAD
                    WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "tflm: git checkout FETCH_HEAD failed")
    endif()

    file(REMOVE_RECURSE "${TFLM_ROOT}")
    execute_process(
        # reference kernels only (no OPTIMIZED_KERNEL_DIR); CMSIS-NN is a separate M2 task.
        # Prepend the venv to PATH so the Makefile's own `python3` subprocess (which runs
        # generate_cc_arrays.py) resolves numpy/Pillow -- running create_tflm_tree.py with
        # the venv python alone is not enough, as make spawns a fresh `python3`.
        COMMAND "${CMAKE_COMMAND}" -E env "PATH=${TFLM_VENV}/bin:$ENV{PATH}"
                "${TFLM_PY}" "${TFLM_GEN}" -e hello_world "${TFLM_ROOT}"
        WORKING_DIRECTORY "${TFLM_SRC}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${TFLM_ROOT}/tensorflow/lite/micro/micro_interpreter.cc")
        message(FATAL_ERROR "tflm: create_tflm_tree.py failed (need numpy/Pillow in ${TFLM_VENV})")
    endif()
    file(WRITE "${TFLM_STAMP}" "${TFLM_GIT_SHA}\n")
endif()

# --- Compile the generated tree with our flags (ABI = ours) ------------------
# create_tflm_tree emits only library sources under tensorflow/, but exclude the
# test-support cluster (helpers + mock/fake context) + any *_test.cc belt-and-suspenders.
# signal/*.cc are NOT compiled (no signal ops registered); their headers stay on the
# include path (micro_mutable_op_resolver.h #includes signal/micro/kernels/{irfft,rfft}.h).
file(GLOB_RECURSE TFLM_LIB_SOURCES CONFIGURE_DEPENDS
     "${TFLM_ROOT}/tensorflow/*.cc"
     "${TFLM_ROOT}/tensorflow/*.c")
list(FILTER TFLM_LIB_SOURCES EXCLUDE REGEX
     "(_test\\.cc|test_helpers\\.cc|test_helper_custom_ops\\.cc|mock_micro_graph\\.cc|fake_micro_context\\.cc)$")

add_library(tflm STATIC
    ${TFLM_LIB_SOURCES}
    "${TFLM_ROOT}/examples/hello_world/models/hello_world_int8_model_data.cc"  # spike model
    "${CMAKE_SOURCE_DIR}/port/nn/tflm/cxx_runtime.cc"   # noexcept operator new/delete + traps
    "${CMAKE_SOURCE_DIR}/port/nn/tflm/nn_tflm.cc"       # extern "C" nn_backend_vt_selected
    "${CMAKE_SOURCE_DIR}/port/nn/tflm/tflm_spike.cc")   # extern "C" tflm_spike_selftest

target_link_libraries(tflm PRIVATE bsp_iface)   # MCU_OPTS (fpv5-sp-d16) + CMSIS/HAL includes

target_include_directories(tflm PRIVATE
    "${TFLM_ROOT}"                                   # "tensorflow/..." and "signal/..."
    "${TFLM_ROOT}/third_party/flatbuffers/include"   # "flatbuffers/..."
    "${TFLM_ROOT}/third_party/gemmlowp"              # "fixedpoint/..."
    "${TFLM_ROOT}/third_party/ruy"                   # "ruy/..."
    "${TFLM_ROOT}/examples/hello_world"              # model .cc: "models/hello_world_int8_..."
    "${CMAKE_SOURCE_DIR}/port/nn")                   # nn.h / nn_backend.h for the bridge

# TF_LITE_STATIC_MEMORY changes TFLM's context/tensor structs across the API boundary,
# so it must be visible to every TU that includes TFLM headers -> PUBLIC.  Stripping
# error strings + NDEBUG remove MicroPrintf/assert -> no stdio/_write/abort.
target_compile_definitions(tflm
    PUBLIC  TF_LITE_STATIC_MEMORY
    PRIVATE TF_LITE_STRIP_ERROR_STRINGS NDEBUG)

set_target_properties(tflm PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF)

# Bare-metal C++: no exceptions/RTTI, single-threaded static-init (no __cxa_guard_*),
# no cxa_atexit registration, no unwind tables.  -O2 to match the rest of the build.
target_compile_options(tflm PRIVATE
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
    -fno-unwind-tables -fno-asynchronous-unwind-tables -O2
    -Wno-unused-parameter -Wno-sign-compare -Wno-maybe-uninitialized)

# Provide the nano C++ runtime archives + libm to whatever links `tflm`, ordered
# AFTER tflm's own objects.  cxx_runtime.cc defines our own operator new/delete, so
# the linker resolves operator new from this archive and never extracts
# libstdc++_nano's throwing operator new (which would drag in __cxa_throw /
# _Unwind_* / bad_alloc).  libm (`m`) is needed because the final link is forced onto
# the C driver (gcc), which -- unlike g++ -- does not auto-link it, and TFLM's
# QuantizeMultiplier uses `round` (double, soft-float on this fpv5-sp-d16 core).
target_link_libraries(tflm PUBLIC stdc++_nano supc++_nano m)
