# GCC 12 development headers + PRIVATE device runtime libraries.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KINAMP_TARGET_SYSROOT KINAMP_FFMPEG_PREFIX)
set(CMAKE_SYSROOT "${KINAMP_TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${KINAMP_TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Debian's relocated cross compiler has a non-sysroot-relative libc include
# directory: explicitly prefer the staged development headers to that fallback.
set(CMAKE_C_FLAGS_INIT "-isystem ${KINAMP_TARGET_SYSROOT}/usr/include -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-isystem ${KINAMP_TARGET_SYSROOT}/usr/include -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${KINAMP_TARGET_SYSROOT}/usr/lib -Wl,-rpath-link,${KINAMP_TARGET_SYSROOT}/usr/lib -Wl,-rpath-link,${KINAMP_TARGET_SYSROOT}/lib")
set(CMAKE_SKIP_RPATH TRUE)
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${KINAMP_FFMPEG_PREFIX}/lib/pkgconfig:${KINAMP_FFMPEG_PREFIX}/opus/lib/pkgconfig:${KINAMP_TARGET_SYSROOT}/usr/lib/pkgconfig")
# Both PC sets were generated with absolute staging prefixes.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
