# Offline tests for the mods folder's archive overrides (mods_psarc.cpp + r2_mods.cpp / r3_mods.cpp).
# Standalone: does not need the lifted game code.
#   cmake -S project/tests/mods_psarc -B build_modstest -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl
#   cmake --build build_modstest && ctest --test-dir build_modstest --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(mods_psarc_test C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(PORT_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(PORT_ROOT "${PORT_PROJECT_DIR}/.." ABSOLUTE)
set(MINIZ_DIR "${PORT_ROOT}/ps3recomp/third_party/fossilize/miniz")
file(GLOB PORT_MODS_CPP "${PORT_PROJECT_DIR}/r[23]_mods.cpp")
# a real level archive for the round-trip test (skipped when absent)
file(GLOB _lobby "${PORT_ROOT}/gamefiles/game/*/USRDIR/packed/levels/lobby/level_cached.psarc")
list(FILTER _lobby EXCLUDE REGEX "\\.mine/")
set(_lobby0 "none")
if(_lobby)
    list(GET _lobby 0 _lobby0)
endif()
set(MODS_TEST_ARCHIVE "${_lobby0}" CACHE FILEPATH "level_cached.psarc for the real-data test")

add_executable(mods_psarc_test
    mods_psarc_test.cpp
    "${PORT_PROJECT_DIR}/mods_psarc.cpp"
    ${PORT_MODS_CPP}
    "${MINIZ_DIR}/miniz.c")
target_include_directories(mods_psarc_test PRIVATE "${PORT_PROJECT_DIR}" "${MINIZ_DIR}")
target_compile_definitions(mods_psarc_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
set_source_files_properties("${MINIZ_DIR}/miniz.c" PROPERTIES COMPILE_OPTIONS "-w")

set(_dir "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME mods_unit COMMAND mods_psarc_test unit WORKING_DIRECTORY "${_dir}")
add_test(NAME mods_setup COMMAND mods_psarc_test setup "${_dir}/hook" WORKING_DIRECTORY "${_dir}")
add_test(NAME mods_hook COMMAND mods_psarc_test hook "${_dir}/hook" WORKING_DIRECTORY "${_dir}")
add_test(NAME mods_rebuild COMMAND mods_psarc_test rebuild "${_dir}/hook" WORKING_DIRECTORY "${_dir}")
add_test(NAME mods_nohook COMMAND mods_psarc_test nohook WORKING_DIRECTORY "${_dir}")
add_test(NAME mods_real COMMAND mods_psarc_test real "${MODS_TEST_ARCHIVE}" WORKING_DIRECTORY "${_dir}")
set_tests_properties(mods_setup PROPERTIES FIXTURES_SETUP modsdir)
set_tests_properties(mods_hook PROPERTIES FIXTURES_REQUIRED modsdir ENVIRONMENT "R2_MODS=${_dir}/hook/mods")
set_tests_properties(mods_rebuild PROPERTIES FIXTURES_REQUIRED modsdir DEPENDS mods_hook ENVIRONMENT "R2_MODS=${_dir}/hook/mods")
set_tests_properties(mods_nohook PROPERTIES ENVIRONMENT "R2_MODS=")
set_tests_properties(mods_real PROPERTIES SKIP_RETURN_CODE 77)
