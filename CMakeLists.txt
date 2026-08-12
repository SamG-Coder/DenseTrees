cmake_minimum_required(VERSION 3.24)
project(DenseTrees VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(densetrees_core
    src/aoe_world.cpp
    src/environment.cpp
    src/environment_simulation.cpp
    src/first_person_camera.cpp
    src/ground_texture.cpp
    src/launch_options.cpp
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
add_executable(environment_simulation_tests tests/environment_simulation_tests.cpp)
target_link_libraries(environment_simulation_tests PRIVATE densetrees_core)
add_executable(first_person_camera_tests tests/first_person_camera_tests.cpp)
target_link_libraries(first_person_camera_tests PRIVATE densetrees_core)
add_executable(ground_texture_tests tests/ground_texture_tests.cpp)
target_link_libraries(ground_texture_tests PRIVATE densetrees_core)
add_executable(aoe_world_tests tests/aoe_world_tests.cpp)
target_link_libraries(aoe_world_tests PRIVATE densetrees_core)
add_executable(launch_options_tests tests/launch_options_tests.cpp)
target_link_libraries(launch_options_tests PRIVATE densetrees_core)
enable_testing()
add_test(NAME procedural_tree_tests COMMAND densetrees_tests)
add_test(NAME environment_simulation_tests COMMAND environment_simulation_tests)
add_test(NAME first_person_camera_tests COMMAND first_person_camera_tests)
add_test(NAME ground_texture_tests COMMAND ground_texture_tests)
add_test(NAME aoe_world_tests COMMAND aoe_world_tests)
add_test(NAME launch_options_tests COMMAND launch_options_tests)

set_target_properties(DenseTrees densetrees_tests environment_simulation_tests
    first_person_camera_tests ground_texture_tests aoe_world_tests launch_options_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
