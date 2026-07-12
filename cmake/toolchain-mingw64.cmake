# CMake toolchain for cross-compiling DearTT to Windows x86_64 from Linux
# using MinGW-w64 (Ubuntu: `apt install mingw-w64`).
#
# The *-posix compiler variants are required: they use winpthreads, which
# provides the std::thread / std::mutex support the codebase relies on.
#
# Usage:
#   ./scripts/build-win64-deps.sh          # once: fetch/cross-build deps
#   cmake -B build-win64 -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
#   cmake --build build-win64 -j

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

find_program(_gcc_posix ${TOOLCHAIN_PREFIX}-gcc-posix)
if(_gcc_posix)
  set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc-posix)
  set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-posix)
else()
  set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
  set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
endif()
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Sysroot + cross-built dependencies (see scripts/build-win64-deps.sh).
get_filename_component(_deartt_deps
  "${CMAKE_CURRENT_LIST_DIR}/../win64-deps" ABSOLUTE)
set(CMAKE_FIND_ROOT_PATH
  /usr/${TOOLCHAIN_PREFIX}
  ${_deartt_deps}/openssl
  ${_deartt_deps}/protobuf
  ${_deartt_deps}/curl-impersonate
  ${_deartt_deps}/ffmpeg
)

# Host programs (protoc, python, ...), target libs/headers only from the
# root paths above.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Keep the mingw runtime out of the DLL list; only libwinpthread-1.dll
# remains to be shipped next to the exe.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
