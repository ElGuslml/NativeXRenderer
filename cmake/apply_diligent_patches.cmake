# Applies XenosNative's DiligentCore patches (xenos_native/diligent_patches/, a git format-patch series in
# file name order) to the DiligentCore checkout (XENOS_NATIVE_DILIGENT_DIR) at configure time, as
# cmake/apply_sdk_patches.cmake does for the ReXGlue SDK (decision D22).
#
# - Pristine checkout (no changes to tracked files): every patch is applied in order, and a stamp with the
#   series hash is written to the checkout's git directory.
# - Checkout already patched with this same series (stamp matches): nothing to do.
# - Anything else (the patches changed, or files were edited): stop and explain how to start over.

find_package(Git REQUIRED)

file(GLOB _xn_diligent_patches "${CMAKE_CURRENT_LIST_DIR}/../diligent_patches/*.patch")
list(SORT _xn_diligent_patches)
list(LENGTH _xn_diligent_patches _xn_diligent_patch_count)

set(_xn_series "")
foreach(_patch IN LISTS _xn_diligent_patches)
    file(SHA256 "${_patch}" _hash)
    string(APPEND _xn_series "${_hash}\n")
endforeach()
string(SHA256 _xn_series_hash "${_xn_series}")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --git-dir
    WORKING_DIRECTORY "${XENOS_NATIVE_DILIGENT_DIR}"
    OUTPUT_VARIABLE _xn_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_xn_git_dir "${_xn_git_dir}" ABSOLUTE BASE_DIR "${XENOS_NATIVE_DILIGENT_DIR}")
set(_xn_stamp "${_xn_git_dir}/xenos-native-patches.stamp")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no --ignore-submodules=all
    WORKING_DIRECTORY "${XENOS_NATIVE_DILIGENT_DIR}"
    OUTPUT_VARIABLE _xn_status)

set(_xn_stamp_hash "")
if(EXISTS "${_xn_stamp}")
    file(READ "${_xn_stamp}" _xn_stamp_hash)
    string(STRIP "${_xn_stamp_hash}" _xn_stamp_hash)
endif()

if(_xn_status STREQUAL "")
    foreach(_patch IN LISTS _xn_diligent_patches)
        get_filename_component(_name "${_patch}" NAME)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
            WORKING_DIRECTORY "${XENOS_NATIVE_DILIGENT_DIR}"
            RESULT_VARIABLE _result
            ERROR_VARIABLE _error)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR
                "[diligent-patches] ${_name} does not apply to ${XENOS_NATIVE_DILIGENT_DIR}:\n${_error}\n"
                "Is the submodule at the pinned commit? Run: git submodule update --init --recursive")
        endif()
    endforeach()
    file(WRITE "${_xn_stamp}" "${_xn_series_hash}\n")
    message(STATUS "[diligent-patches] applied ${_xn_diligent_patch_count} patches to ${XENOS_NATIVE_DILIGENT_DIR}")
elseif(_xn_stamp_hash STREQUAL _xn_series_hash)
    message(STATUS "[diligent-patches] ${_xn_diligent_patch_count} patches already applied")
else()
    message(FATAL_ERROR
        "[diligent-patches] ${XENOS_NATIVE_DILIGENT_DIR} has changes that do not match "
        "xenos_native/diligent_patches/ (the patches changed, or files in the submodule were edited).\n"
        "To start over from the pinned DiligentCore with the current patches (this discards edits in the "
        "submodule):\n"
        "  git -C thirdparty/diligent-core checkout -- .\n"
        "then configure again.")
endif()
