cmake_minimum_required(VERSION 3.16)
project(HE_PCA_PoC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake 4.x 兼容: 允许旧版第三方 CMakeLists 正常工作
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# macOS Apple Silicon: libomp 由 Homebrew 提供
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/libomp")
endif()

include(FetchContent)

if(APPLE)
    set(OPENMP_LIB_PATH "/opt/homebrew/opt/libomp/lib/libomp.dylib")
    set(OPENMP_INCLUDE_PATH "/opt/homebrew/opt/libomp/include")
    link_directories("/opt/homebrew/opt/libomp/lib")
endif()

# ── OpenFHE 1.5.1 ─────────────────────────────────────────────────────────────
# 使用 FetchContent 拉取；OpenFHE 顶层 CMakeLists 在 .git 存在时会自动拉取 submodule
FetchContent_Declare(
    openfhe
    GIT_REPOSITORY https://github.com/openfheorg/openfhe-development.git
    GIT_TAG        v1.5.1
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
# 关闭 OpenFHE 自带的测试/例子/基准，只保留核心库
set(BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTESTS  OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(openfhe)

# ── Eigen 3.4.0 (header-only, 仅拉取源码不执行其 CMake) ─────────────────────
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(eigen)
if(NOT eigen_POPULATED)
    FetchContent_Populate(eigen)
endif()
add_library(Eigen3::Eigen INTERFACE IMPORTED)
set_target_properties(Eigen3::Eigen PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${eigen_SOURCE_DIR}"
)

# ── OpenFHE include 目录收集 ─────────────────────────────────────────────────
# OpenFHE 的 public include 分散在 core/pke/binfhe 下
set(OPENFHE_INCLUDE_DIRS
    ${openfhe_SOURCE_DIR}/src/core/include
    ${openfhe_SOURCE_DIR}/src/pke/include
    ${openfhe_SOURCE_DIR}/src/binfhe/include
    ${openfhe_SOURCE_DIR}/third-party/include
    ${openfhe_SOURCE_DIR}/third-party/cereal/include
    ${openfhe_BINARY_DIR}/src/core
    ${openfhe_BINARY_DIR}/src/pke
    ${openfhe_BINARY_DIR}/src/binfhe
)

# ── 主可执行文件 ─────────────────────────────────────────────────────────────
add_executable(he_pca
    src/main.cpp
    src/client.cpp
    src/server.cpp
    src/newton_inv_sqrt.cpp
)

target_link_libraries(he_pca PRIVATE
    OPENFHEcore
    OPENFHEpke
    OPENFHEbinfhe
    Eigen3::Eigen
    ${OPENMP_LIB_PATH}
)
target_include_directories(he_pca PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENFHE_INCLUDE_DIRS}
)

add_executable(plaintext_lanczos_tune
    src/plaintext_lanczos_tune.cpp
    src/client.cpp
)
target_link_libraries(plaintext_lanczos_tune PRIVATE
    OPENFHEcore
    OPENFHEpke
    OPENFHEbinfhe
    Eigen3::Eigen
    ${OPENMP_LIB_PATH}
)
target_include_directories(plaintext_lanczos_tune PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENFHE_INCLUDE_DIRS}
)

# ── 层数验证小工具 ───────────────────────────────────────────────────────────
add_executable(depth_probe src/depth_probe.cpp)
target_link_libraries(depth_probe PRIVATE
    OPENFHEcore
    OPENFHEpke
    OPENFHEbinfhe
    ${OPENMP_LIB_PATH}
)
target_include_directories(depth_probe PRIVATE
    ${OPENFHE_INCLUDE_DIRS}
)
