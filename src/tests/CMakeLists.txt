find_package(Qt6 REQUIRED COMPONENTS Test Widgets)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

include_directories(${CMAKE_SOURCE_DIR})

# Legacy test: currently unbuildable (stale include path predating the gui/
# tree reorganization). Kept for reference; flip on to attempt revival.
option(QIMGV_LEGACY_TESTS "Build legacy MapOverlay test (currently broken)" OFF)
if(QIMGV_LEGACY_TESTS)
    add_executable(unit_tests test_mapoverlay.cpp)
    target_link_libraries(unit_tests PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME QUI_TEST COMMAND unit_tests)
endif()

# Rust engine contract smoke test (pure C ABI, no Qt needed)
add_executable(engine_contract_tests test_engine_contract.cpp)
target_link_libraries(engine_contract_tests PRIVATE qimgv_engine_ffi qimgv_sanitizers)
target_include_directories(engine_contract_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/qimgv-engine-ffi/include)
# The TSan tree's named noise baseline, wired where the tests are registered so
# it is machine-absorbed rather than read by a human each run: the mixed binary
# reports only the uninstrumented side (qimgv_engine / Rust std — see
# tsan-noise.supp for why that is undecidable rather than informative), and the
# "thread leak" reports are joins TSan never recorded (pthread_clockjoin_np —
# the same upstream gap behind the QThreadPool CHECK). A no-op in every other
# tree: TSAN_OPTIONS is only read by the TSan runtime.
# Defined BEFORE the first set_tests_properties that uses it: it once sat below
# ENGINE_CONTRACT's, so that suite's property expanded to an empty string and
# the whole baseline silently never applied there — 32 engine-boundary reports
# a run, looking exactly like the residue the baseline exists to absorb.
set(TSAN_ENV "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-noise.supp:report_thread_leaks=0")

add_test(NAME ENGINE_CONTRACT COMMAND engine_contract_tests)
set_tests_properties(ENGINE_CONTRACT PROPERTIES ENVIRONMENT "${TSAN_ENV}")

# Engine stack integration: EngineBridge pump + DirectoryManager async wiring.
# Links qimgv_nongui — the same objects the executable is built from — so a test
# that touches another component costs nothing to add.
# One class per domain, one binary. 49 slots in a single 1371-line TU meant every
# edit recompiled the whole suite, and the fifteen "layer" comments in it were
# file boundaries that never became files.
add_executable(engine_stack_tests
    engine_stack_main.cpp
    listing_tests.cpp
    wire_format_tests.cpp
    folder_boundary_tests.cpp
    decode_tests.cpp
    settings_tests.cpp
    filelog_tests.cpp
    machinery_tests.cpp
    thumbnail_store_tests.cpp
    thumbnailer_tests.cpp
    workerpool_tests.cpp
    fileops_tests.cpp
    image_ops_tests.cpp
    infra_tests.cpp)
# USE_EXIV2 arrives with qimgv_nongui's PUBLIC compile definitions: one answer
# to "is Exiv2 available" for every target, instead of each restating it.
target_link_libraries(engine_stack_tests PRIVATE Qt6::Test qimgv_nongui)
add_test(NAME ENGINE_STACK COMMAND engine_stack_tests)
set_tests_properties(ENGINE_STACK PROPERTIES ENVIRONMENT "${TSAN_ENV}")

# Core-level navigation: links the widget tree too, so a real Core can be built
# and driven under the offscreen platform. OBJECT libraries do not re-export each
# other's objects, so both are listed — the same pair the executable links.
add_executable(core_nav_tests core_nav_tests.cpp)
target_link_libraries(core_nav_tests PRIVATE Qt6::Test qimgv_gui qimgv_nongui)
add_test(NAME CORE_NAV COMMAND core_nav_tests)
set_tests_properties(CORE_NAV PROPERTIES ENVIRONMENT "${TSAN_ENV}")

# ---- the LLVM idiom: the tests are build targets, and the check targets fuse
# the build in front of the run. A green suite on a stale binary is not
# evidence — three encounters in one session were the prompt — so "run the
# tests" is a build-system target whose dependencies the build system itself
# guarantees. No wrapper script can say that from outside the graph.
#   cmake --build build --target check              # everything ctest knows
#   cmake --build build --target check-crop_tests   # one suite, incremental
# The cp/mv mtime trap keeps its documented ritual: no target can see a lie
# the file system tells the build system.

set(CPP_TEST_TARGETS
    engine_contract_tests engine_stack_tests
    core_nav_tests core_fileops_tests gui_widget_tests
    crop_tests grid_tests shortcut_scope_tests overlay_keys_tests
    presenter_tests edit_save_tests slideshow_tests clipboard_tests)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -j4
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${CPP_TEST_TARGETS}
    USES_TERMINAL)

foreach(t IN LISTS CPP_TEST_TARGETS)
    add_custom_target(check-${t}
        COMMAND ${t}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS ${t}
        USES_TERMINAL)
endforeach()

# The core_nav family, split by subject (the same widget-tree link as CORE_NAV):
# crop, the grid widget, the shortcut scopes, the floating-layer keyboard, the
# presenter over its scripted view, the edit/save contract, the slideshow, and
# the clipboard/drag-drop legs. One subject per suite, so a crop change runs
# crop tests and not the world.
foreach(suite crop_tests grid_tests shortcut_scope_tests overlay_keys_tests presenter_tests edit_save_tests slideshow_tests clipboard_tests)
    add_executable(${suite} ${suite}.cpp)
    target_link_libraries(${suite} PRIVATE Qt6::Test qimgv_gui qimgv_nongui)
    add_test(NAME ${suite} COMMAND ${suite})
    set_tests_properties(${suite} PROPERTIES ENVIRONMENT "${TSAN_ENV}")
endforeach()

# Core's file operations: copy, move, rename, delete and trash driven through
# scripted dialogs. Same object libraries as CORE_NAV, but no engine listing is
# read, so it runs as its own binary with its own fixture domain.
add_executable(core_fileops_tests core_fileops_tests.cpp)
target_link_libraries(core_fileops_tests PRIVATE Qt6::Test qimgv_gui qimgv_nongui)
add_test(NAME CORE_FILEOPS COMMAND core_fileops_tests)
set_tests_properties(CORE_FILEOPS PROPERTIES ENVIRONMENT "${TSAN_ENV}")

# The widget surfaces: overlays, panels, the grid's keyboard and drag, the
# settings dialog. Same object libraries, but nothing here builds a Core — this
# suite exists so the checks that only need a widget do not pay for an engine,
# a watcher and an async listing, and so a failure names the surface.
add_executable(gui_widget_tests gui_widget_tests.cpp)
target_link_libraries(gui_widget_tests PRIVATE Qt6::Test qimgv_gui qimgv_nongui)
add_test(NAME GUI_WIDGETS COMMAND gui_widget_tests)
set_tests_properties(GUI_WIDGETS PROPERTIES ENVIRONMENT "${TSAN_ENV}")

# executable architecture rules (ArchUnit-style): gui isolation, FFI caller
# allowlist, rust internal seal
add_test(NAME ARCH_RULES
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/qimgv/tests/architecture_rules.py)
set_tests_properties(ARCH_RULES PROPERTIES ENVIRONMENT "PYTHONPATH=")

# lint cache contract: cold==warm, header fan-out, include shadowing. Skips
# (77) where the lint tools are not installed — the gate itself needs them,
# but a bare configure-and-test should not fail on their absence.
add_test(NAME LINT_CACHE
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/qimgv/tests/lint_cache_test.py)
set_tests_properties(LINT_CACHE PROPERTIES ENVIRONMENT "PYTHONPATH=" SKIP_RETURN_CODE 77)
