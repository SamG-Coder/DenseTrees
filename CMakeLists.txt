cmake_minimum_required(VERSION 3.24)
project(DenseTrees VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(densetrees_core
    src/environment.cpp
    src/ground_texture.cpp
    src/tree.cpp
)
target_include_directories(densetrees_core PUBLIC src)
target_compile_options(densetrees_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(DenseTrees WIN32
    src/main.cpp
    src/renderer.cpp
    src/rtx_caps.cpp
    src/dxr_renderer.cpp
)
target_link_libraries(DenseTrees PRIVATE
    densetrees_core d3d11 d3d12 dxgi d3dcompiler user32 gdi32 shell32 comctl32
)
target_compile_definitions(DenseTrees PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(DenseTrees PRIVATE -Wall -Wextra -Wpedantic)
if(MINGW)
    target_link_options(DenseTrees PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

add_executable(densetrees_tests tests/tree_tests.cpp)
target_link_libraries(densetrees_tests PRIVATE densetrees_core)
enable_testing()
add_test(NAME procedural_tree_tests COMMAND densetrees_tests)

set_target_properties(DenseTrees densetrees_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
