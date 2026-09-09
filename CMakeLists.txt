cmake_minimum_required(VERSION 3.5...3.31)
project(DTO VERSION 1.0 LANGUAGES C)

# Default to RelWithDebInfo when no build type is specified.
# (Skip for multi-config generators, which ignore CMAKE_BUILD_TYPE.)
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
        "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No build type specified, defaulting to RelWithDebInfo")
endif()

include(GNUInstallDirs)
set(CMAKE_INSTALL_LIBDIR lib)

# Options to enable/disable library support (on by default)
option(DTO_ACCEL_CONFIG_SUPPORT "Enable accel-config for automatic DSA discovery (requires libaccel-config)" ON)
option(DTO_NUMA_SUPPORT "Enable NUMA awareness support (requires libnuma)" ON)

# Options to statically link dependencies into libdto.so
option(DTO_STATIC_ACCEL_CONFIG "Statically link accel-config into libdto.so" OFF)
option(DTO_STATIC_NUMA "Statically link numa into libdto.so (requires PIC-compiled libnuma.a)" OFF)

# Build the shared library
add_library(dto SHARED dto.c)
add_library(DTO::dto ALIAS dto)
target_include_directories(dto
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# set gnu source everywhere
add_compile_definitions(_GNU_SOURCE)

# Add the -DDTO_STATS_SUPPORT preprocessor definition
target_compile_definitions(dto PRIVATE DTO_STATS_SUPPORT)

# Link libraries
set(DTO_STATIC_LIBS "")
set(DTO_DYNAMIC_LIBS dl)

# Handle accel-config support
if(DTO_ACCEL_CONFIG_SUPPORT)
    target_compile_definitions(dto PRIVATE DTO_ACCEL_CONFIG_SUPPORT)
    message(STATUS "accel-config support: enabled")

    if(DTO_STATIC_ACCEL_CONFIG)
        find_library(ACCEL_CONFIG_STATIC NAMES libaccel-config.a PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
        if(NOT ACCEL_CONFIG_STATIC)
            message(FATAL_ERROR "libaccel-config.a not found. Install static accel-config or disable DTO_STATIC_ACCEL_CONFIG.")
        endif()
        message(STATUS "Statically linking accel-config: ${ACCEL_CONFIG_STATIC}")
        list(APPEND DTO_STATIC_LIBS ${ACCEL_CONFIG_STATIC})
    else()
        list(APPEND DTO_DYNAMIC_LIBS accel-config)
    endif()
else()
    message(STATUS "accel-config support: disabled (DTO_WQ_LIST env var required at runtime)")
endif()

# Handle numa support
if(DTO_NUMA_SUPPORT)
    target_compile_definitions(dto PRIVATE DTO_NUMA_SUPPORT)
    message(STATUS "NUMA support: enabled")

    if(DTO_STATIC_NUMA)
        find_library(NUMA_STATIC NAMES libnuma.a PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
        if(NOT NUMA_STATIC)
            message(FATAL_ERROR "libnuma.a not found. Install static numa or disable DTO_STATIC_NUMA.")
        endif()
        message(STATUS "Statically linking numa: ${NUMA_STATIC}")
        message(STATUS "  (Note: libnuma.a must be compiled with -fPIC)")
        list(APPEND DTO_STATIC_LIBS ${NUMA_STATIC})
    else()
        list(APPEND DTO_DYNAMIC_LIBS numa)
    endif()
else()
    message(STATUS "NUMA support: disabled")
endif()

# Link everything together. Use PRIVATE so accel-config/numa/dl are the shared
# library's own dependencies (resolved via its DT_NEEDED) rather than INTERFACE
# requirements that would be forced onto every downstream consumer of DTO::dto.
if(DTO_STATIC_LIBS)
    target_link_libraries(dto PRIVATE
        -Wl,--whole-archive
        ${DTO_STATIC_LIBS}
        -Wl,--no-whole-archive
        ${DTO_DYNAMIC_LIBS}
    )
else()
    target_link_libraries(dto PRIVATE ${DTO_DYNAMIC_LIBS})
endif()

include(CheckCCompilerFlag)
check_c_compiler_flag("-mwaitpkg" HAS_WAITPKG)
if (HAS_WAITPKG)
  target_compile_options(dto PRIVATE -mwaitpkg -march=native)
endif()

# Build dto-test and dto-test-wodto
add_executable(dto-test dto-test.c)
target_link_libraries(dto-test PRIVATE DTO::dto pthread)

add_executable(dto-test-wodto dto-test.c)
target_link_libraries(dto-test-wodto PRIVATE pthread)

# ---- Test Suite ----
option(DTO_BUILD_TESTS "Build the DTO test suite" ON)

if(DTO_BUILD_TESTS)
    enable_testing()

    # Functional correctness tests (safe to run in CI without DSA hardware)
    add_executable(dto-test-functional tests/test_functional.c)
    target_link_libraries(dto-test-functional PRIVATE DTO::dto pthread)

    add_test(NAME functional COMMAND dto-test-functional)
    set_tests_properties(functional PROPERTIES
        LABELS "functional"
        TIMEOUT 120
    )

    # ---- A/B performance regression test ----
    # The baseline object is compiled from unguarded upstream sources that
    # always reference accel-config and numa, so the perf test can only be
    # built when both features are enabled. The functional test above links
    # DTO::dto and follows the options, so it is always available.
    if(DTO_ACCEL_CONFIG_SUPPORT AND DTO_NUMA_SUPPORT)
        # Compile flags shared by the current and baseline perf objects so the
        # A/B comparison uses an identical configuration. -mwaitpkg is gated on
        # compiler support, matching the main library above.
        set(PERF_OBJ_CFLAGS -O3 -DNDEBUG -march=native -fPIC
            -D_GNU_SOURCE -DDTO_STATS_SUPPORT
            -DDTO_ACCEL_CONFIG_SUPPORT -DDTO_NUMA_SUPPORT)
        if(HAS_WAITPKG)
            list(APPEND PERF_OBJ_CFLAGS -mwaitpkg)
        endif()

        # Build current dto.c as object file with renamed symbols (cur_memcpy, etc.)
        set(PERF_BASELINE_DIR "${CMAKE_BINARY_DIR}/perf_baseline")

        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/dto_current.o
            COMMAND ${CMAKE_C_COMPILER} -c ${PERF_OBJ_CFLAGS}
                    ${CMAKE_SOURCE_DIR}/dto.c
                    -o ${CMAKE_BINARY_DIR}/dto_current_raw.o
            COMMAND objcopy --redefine-sym memcpy=cur_memcpy
                            --redefine-sym memset=cur_memset
                            --redefine-sym memcmp=cur_memcmp
                            --redefine-sym memmove=cur_memmove
                            ${CMAKE_BINARY_DIR}/dto_current_raw.o
                            ${CMAKE_BINARY_DIR}/dto_current.o
            DEPENDS ${CMAKE_SOURCE_DIR}/dto.c
            COMMENT "Building dto_current.o with renamed symbols"
        )

        # Build baseline dto.o from baseline branch (cached by SHA). The same
        # PERF_OBJ_CFLAGS are forwarded so the baseline matches the current object.
        # A custom target (not a custom command) so the script runs on every
        # build: its SHA check against upstream decides whether the cached
        # object is still fresh, and a custom command would never re-run once
        # the object exists.
        add_custom_target(dto_baseline_build
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/build_baseline.sh
                    ${CMAKE_SOURCE_DIR} ${PERF_BASELINE_DIR} ${CMAKE_C_COMPILER}
                    ${PERF_OBJ_CFLAGS}
            BYPRODUCTS ${PERF_BASELINE_DIR}/dto_baseline.o
            COMMENT "Building dto_baseline.o from baseline branch"
        )

        add_executable(dto-test-perf
            tests/test_perf.c
            ${CMAKE_BINARY_DIR}/dto_current.o
            ${PERF_BASELINE_DIR}/dto_baseline.o
        )
        add_dependencies(dto-test-perf dto_baseline_build)
        target_link_libraries(dto-test-perf PRIVATE m dl pthread accel-config numa)

        # Common env vars for perf tests
        # Pin core and uncore frequency to 2000 MHz by default; override with
        # -DPERF_CORE_FREQ_MHZ=... and/or -DPERF_UNCORE_FREQ_MHZ=...
        if(NOT DEFINED PERF_CORE_FREQ_MHZ)
            set(PERF_CORE_FREQ_MHZ 2000)
        endif()
        if(NOT DEFINED PERF_UNCORE_FREQ_MHZ)
            set(PERF_UNCORE_FREQ_MHZ 2000)
        endif()

        set(PERF_COMMON_ENV
            "PERF_CORE_FREQ_MHZ=${PERF_CORE_FREQ_MHZ}"
            "PERF_UNCORE_FREQ_MHZ=${PERF_UNCORE_FREQ_MHZ}"
        )

        # Both tests pin CPU 1 with SCHED_FIFO and pin core/uncore frequency,
        # so they must never run concurrently (RUN_SERIAL). Each gets its own
        # RESULTS_DIR because every run deletes and rewrites distributions.csv
        # in that directory.

        # --- A/B perf test (all configs + page sizes in one run) ---
        add_test(NAME perf COMMAND dto-test-perf)
        set_tests_properties(perf PROPERTIES
            LABELS "perf"
            TIMEOUT 2400
            RUN_SERIAL TRUE
            ENVIRONMENT "RESULTS_DIR=${CMAKE_BINARY_DIR}/perf_results;${PERF_COMMON_ENV}"
        )

        # short sanity variant: memcpy 4k/64k/1m + memset 64k,
        # few iterations. use -V to see the
        # condensed speedup table
        add_test(NAME sanity COMMAND dto-test-perf)
        set_tests_properties(sanity PROPERTIES
            LABELS "perf"
            TIMEOUT 120
            RUN_SERIAL TRUE
            ENVIRONMENT "PERF_SHORT=1;RESULTS_DIR=${CMAKE_BINARY_DIR}/perf_results_sanity;${PERF_COMMON_ENV}"
        )
    else()
        message(STATUS "perf test: disabled (requires DTO_ACCEL_CONFIG_SUPPORT and "
                       "DTO_NUMA_SUPPORT; its A/B baseline is built from unguarded "
                       "upstream sources)")
    endif()
endif()

include(GNUInstallDirs)

install(FILES dto.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install and export the library
install(TARGETS dto
        EXPORT DTOTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(EXPORT DTOTargets
        FILE DTOTargets.cmake
        NAMESPACE DTO::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DTO)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/DTOConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/DTOConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DTOConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DTO)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/DTOConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/DTOConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DTO)

