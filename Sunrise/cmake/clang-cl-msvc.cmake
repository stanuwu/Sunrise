set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED XWIN_DIR)
    if(DEFINED ENV{XWIN_DIR})
        set(XWIN_DIR "$ENV{XWIN_DIR}")
    else()
        set(XWIN_DIR "$ENV{HOME}/.xwin")
    endif()
endif()

find_program(CLANG_CL NAMES clang-cl clang-cl-20 clang-cl-19 clang-cl-18 PATHS /usr/lib/llvm-20/bin /usr/lib/llvm-19/bin REQUIRED)
find_program(LLD_LINK NAMES lld-link lld-link-20 PATHS /usr/lib/llvm-20/bin REQUIRED)
find_program(LLVM_RC NAMES llvm-rc llvm-rc-20 PATHS /usr/lib/llvm-20/bin REQUIRED)
set(CMAKE_C_COMPILER   "${CLANG_CL}")
set(CMAKE_CXX_COMPILER "${CLANG_CL}")
set(CMAKE_RC_COMPILER  "${LLVM_RC}")
set(CMAKE_LINKER       "${LLD_LINK}")

set(_xwin_incs
    "-imsvc${XWIN_DIR}/crt/include"
    "-imsvc${XWIN_DIR}/sdk/include/ucrt"
    "-imsvc${XWIN_DIR}/sdk/include/um"
    "-imsvc${XWIN_DIR}/sdk/include/shared"
    "-imsvc${XWIN_DIR}/sdk/include/winrt")
list(JOIN _xwin_incs " " _xwin_incs_str)

set(_common "--target=x86_64-pc-windows-msvc -Wno-unused-command-line-argument ${_xwin_incs_str}")
set(CMAKE_C_FLAGS_INIT   "${_common}")
set(CMAKE_CXX_FLAGS_INIT "${_common}")

set(_xwin_libs
    "/libpath:${XWIN_DIR}/crt/lib/x86_64"
    "/libpath:${XWIN_DIR}/sdk/lib/um/x86_64"
    "/libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
list(JOIN _xwin_libs " " _xwin_libs_str)
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_xwin_libs_str}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_libs_str}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_libs_str}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
