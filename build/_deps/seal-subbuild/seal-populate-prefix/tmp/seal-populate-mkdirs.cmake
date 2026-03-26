# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/bytedance/PCA/build/_deps/seal-src")
  file(MAKE_DIRECTORY "/Users/bytedance/PCA/build/_deps/seal-src")
endif()
file(MAKE_DIRECTORY
  "/Users/bytedance/PCA/build/_deps/seal-build"
  "/Users/bytedance/PCA/build/_deps/seal-subbuild/seal-populate-prefix"
  "/Users/bytedance/PCA/build/_deps/seal-subbuild/seal-populate-prefix/tmp"
  "/Users/bytedance/PCA/build/_deps/seal-subbuild/seal-populate-prefix/src/seal-populate-stamp"
  "/Users/bytedance/PCA/build/_deps/seal-subbuild/seal-populate-prefix/src"
  "/Users/bytedance/PCA/build/_deps/seal-subbuild/seal-populate-prefix/src/seal-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/bytedance/PCA/build/_deps/seal-subbuild/seal-populate-prefix/src/seal-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/bytedance/PCA/build/_deps/seal-subbuild/seal-populate-prefix/src/seal-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
