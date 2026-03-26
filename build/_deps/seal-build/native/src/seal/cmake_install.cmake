# Install script for directory: /Users/bytedance/PCA/build/_deps/seal-src/native/src/seal

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/SEAL-4.1/seal" TYPE FILE FILES
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/batchencoder.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/ciphertext.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/ckks.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/modulus.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/context.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/decryptor.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/dynarray.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/encryptionparams.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/encryptor.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/evaluator.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/galoiskeys.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/keygenerator.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/kswitchkeys.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/memorymanager.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/plaintext.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/publickey.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/randomgen.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/randomtostd.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/relinkeys.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/seal.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/secretkey.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/serializable.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/serialization.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/valcheck.h"
    "/Users/bytedance/PCA/build/_deps/seal-src/native/src/seal/version.h"
    )
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/Users/bytedance/PCA/build/_deps/seal-build/native/src/seal/util/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/bytedance/PCA/build/_deps/seal-build/native/src/seal/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
