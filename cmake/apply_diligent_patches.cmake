# Applies NativeXRenderer's DiligentCore patches (native_x_renderer/diligent_patches/, a git format-patch series in
# file name order) to the DiligentCore checkout (NATIVE_X_RENDERER_DILIGENT_DIR) at configure time, as
# the games' cmake/apply_sdk_patches.cmake did for the ReXGlue SDK until D89 (decision D22).
#
# - Pristine checkout (no changes to tracked files): every patch is applied in order, and a stamp with the
#   series hash is written to the checkout's git directory.
# - Checkout already patched with this same series (stamp matches): nothing to do.
# - Anything else (the patches changed, or files were edited): stop and explain how to start over.

find_package(Git REQUIRED)

file(GLOB _nxr_diligent_patches "${CMAKE_CURRENT_LIST_DIR}/../diligent_patches/*.patch")
list(SORT _nxr_diligent_patches)
list(LENGTH _nxr_diligent_patches _nxr_diligent_patch_count)

set(_nxr_series "")
foreach(_patch IN LISTS _nxr_diligent_patches)
    file(SHA256 "${_patch}" _hash)
    string(APPEND _nxr_series "${_hash}\n")
endforeach()
string(SHA256 _nxr_series_hash "${_nxr_series}")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --git-dir
    WORKING_DIRECTORY "${NATIVE_X_RENDERER_DILIGENT_DIR}"
    OUTPUT_VARIABLE _nxr_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_nxr_git_dir "${_nxr_git_dir}" ABSOLUTE BASE_DIR "${NATIVE_X_RENDERER_DILIGENT_DIR}")
set(_nxr_stamp "${_nxr_git_dir}/native-x-renderer-patches.stamp")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no --ignore-submodules=all
    WORKING_DIRECTORY "${NATIVE_X_RENDERER_DILIGENT_DIR}"
    OUTPUT_VARIABLE _nxr_status)

set(_nxr_stamp_hash "")
if(EXISTS "${_nxr_stamp}")
    file(READ "${_nxr_stamp}" _nxr_stamp_hash)
    string(STRIP "${_nxr_stamp_hash}" _nxr_stamp_hash)
endif()

if(_nxr_status STREQUAL "")
    foreach(_patch IN LISTS _nxr_diligent_patches)
        get_filename_component(_name "${_patch}" NAME)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
            WORKING_DIRECTORY "${NATIVE_X_RENDERER_DILIGENT_DIR}"
            RESULT_VARIABLE _result
            ERROR_VARIABLE _error)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR
                "[diligent-patches] ${_name} does not apply to ${NATIVE_X_RENDERER_DILIGENT_DIR}:\n${_error}\n"
                "Is the submodule at the pinned commit? Run: git submodule update --init --recursive")
        endif()
    endforeach()
    file(WRITE "${_nxr_stamp}" "${_nxr_series_hash}\n")
    message(STATUS "[diligent-patches] applied ${_nxr_diligent_patch_count} patches to ${NATIVE_X_RENDERER_DILIGENT_DIR}")
elseif(_nxr_stamp_hash STREQUAL _nxr_series_hash)
    message(STATUS "[diligent-patches] ${_nxr_diligent_patch_count} patches already applied")
else()
    message(FATAL_ERROR
        "[diligent-patches] ${NATIVE_X_RENDERER_DILIGENT_DIR} has changes that do not match "
        "diligent_patches/ (the patches changed, or files in the submodule were edited).\n"
        "To start over from the pinned DiligentCore with the current patches (this discards edits in the "
        "submodule):\n"
        "  git -C \"${NATIVE_X_RENDERER_DILIGENT_DIR}\" checkout -- .\n"
        "then configure again.")
endif()
