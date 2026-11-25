set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Path to Homebrew installed aarch64-unknown-linux-gnu cross toolchain
find_program(AARCH64_GCC aarch64-unknown-linux-gnu-gcc)
find_program(AARCH64_GXX aarch64-unknown-linux-gnu-g++)

if(AARCH64_GCC AND AARCH64_GXX)
    set(CMAKE_C_COMPILER ${AARCH64_GCC})
    set(CMAKE_CXX_COMPILER ${AARCH64_GXX})
else()
    message(FATAL_ERROR "aarch64-unknown-linux-gnu-gcc cross-compiler not found in PATH")
endif()

# Static link standard libraries for portable Raspberry Pi binary distribution
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
