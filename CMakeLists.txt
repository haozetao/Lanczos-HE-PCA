cmake_minimum_required(VERSION 3.16)
project(HE_PCA_PoC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake 4.x 兼容: 允许旧版第三方 CMakeLists 正常工作
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

include(FetchContent)

# ── Microsoft SEAL 4.1.2 ─────────────────────────────────────────────────────
FetchContent_Declare(
    seal
    GIT_REPOSITORY https://github.com/microsoft/SEAL.git
    GIT_TAG        v4.1.2
)
set(SEAL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SEAL_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(SEAL_BUILD_SEAL_C   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(seal)

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

# ── 主可执行文件 ─────────────────────────────────────────────────────────────
add_executable(he_pca
    src/main.cpp
    src/client.cpp
    src/server.cpp
    src/newton_inv_sqrt.cpp
)

target_link_libraries(he_pca PRIVATE SEAL::seal Eigen3::Eigen)
target_include_directories(he_pca PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(plaintext_lanczos_tune
    src/plaintext_lanczos_tune.cpp
    src/client.cpp
)
target_link_libraries(plaintext_lanczos_tune PRIVATE SEAL::seal Eigen3::Eigen)
target_include_directories(plaintext_lanczos_tune PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
