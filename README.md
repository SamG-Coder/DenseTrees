# Dense Trees

Dense Trees is a native C++ and DirectX Raytracing procedural ecosystem laboratory. It grows biologically structured trees, derives branch thickness from supported foliage, and renders the result inside a ray-traced outdoor environment with rolling hills, a distant mountain range, runtime-generated bark, atmospheric sky, clustered wind-animated grass, shrubs, stones, and a proxy forest.

## Run

From PowerShell:

```text
build.cmd
run.cmd
```

Controls:

- Left-drag: orbit
- Mouse wheel: zoom
- `R` or Space: grow the next deterministic tree
- `1`: English oak
- `2`: Norway spruce
- `3`: Silver birch
- `4`: Weeping willow
- `5`: Umbrella acacia
- Left/Right arrows: move the environment sun without regrowing the tree
- `W`: toggle grass wind simulation
- Escape: exit

## Architecture

- `tree.cpp`: platform-independent growth simulation and CPU tree mesh generation
- `environment.cpp`: deterministic terrain, mountain, meadow, shrub, rock, and background-forest generation
- `dxr_renderer.cpp`: DirectX 12 acceleration structures, runtime bark resources, and path tracing
- `raytracing.hlsl`: atmosphere, terrain, bark, leaf, and procedural grass shaders
- `main.cpp`: Win32 application, camera, lighting, wind, and regeneration controls
- `tests/tree_tests.cpp`: tree morphology plus terrain and grass invariants

The application queries the selected high-performance adapter through Direct3D 12 and reports its DXR, mesh-shader, and sampler-feedback tiers. Tree, leaf, terrain, rocks, shrubs, and background proxies occupy a static triangle BLAS. Grass occupies a second procedural-AABB BLAS: its intersection shader reconstructs mixed short and tall blades, clusters the long blades into coherent meadow islands, applies spatially coherent gusts, and scales blade density and curve resolution with distance without rebuilding either acceleration structure. Primary, sun-shadow, AO, and bounce rays use separate instance masks to control cost.

## Toolchain

The checked-in project has no third-party source dependencies. The build script expects the MSYS2 UCRT64 GCC/CMake/Ninja tools installed under `C:\msys64\ucrt64\bin`.
