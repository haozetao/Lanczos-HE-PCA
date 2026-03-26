# Install script for directory: /Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/SEAL-4.1/seal/util" TYPE FILE FILES
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/blake2.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/blake2-impl.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/clang.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/clipnormal.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/common.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/croots.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/defines.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/dwthandler.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/fips202.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/galois.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/gcc.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/globals.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/hash.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/hestdparms.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/iterator.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/locks.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/mempool.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/msvc.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/numth.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/pointer.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/polyarithsmallmod.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/polycore.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/rlwe.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/rns.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/scalingvariant.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/ntt.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/streambuf.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/uintarith.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/uintarithmod.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/uintarithsmallmod.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/uintcore.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/util/ztools.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/bytedance/PCA/build/_deps/seal-build/native/src/seal/util/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
