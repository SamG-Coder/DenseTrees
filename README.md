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
- `W`: toggle the shared vegetation wind field
- `F1`: show/hide live grass, ground, weather, atmosphere, day/night, and lightning controls
- Escape: exit

## Architecture

- `tree.cpp`: platform-independent growth simulation and CPU tree mesh generation
- `environment.cpp`: deterministic terrain, mountain, meadow, shrub, rock, and background-forest generation
- `environment_simulation.cpp`: CPU day/night, sun/moon, wind, rain, wetness, puddle, fog, storm, and lightning state
- `dxr_renderer.cpp`: DirectX 12 acceleration structures, runtime bark resources, and path tracing
- `environment_cb.hlsli`: shared, layout-matched `EnvironmentCB` consumed at `b1`
- `raytracing.hlsl`: dynamic atmosphere, terrain, wet materials, puddle reflections, bark, foliage, and procedural grass shading
- `main.cpp`: Win32 application, camera, regeneration, and live environment controls
- `tests/tree_tests.cpp`: tree morphology plus terrain and grass invariants
- `tests/environment_simulation_tests.cpp`: environment ABI, solar cycle, weather integration, storm, and determinism checks

The application queries the selected high-performance adapter through Direct3D 12 and reports its DXR, mesh-shader, and sampler-feedback tiers. Tree, leaf, terrain, rocks, shrubs, and background proxies occupy a GPU-local static triangle BLAS. Grass patches are frustum/range compacted on the CPU, then expanded into curved, wind-animated ribbons by an instanced raster overlay after DXR. Both pipelines consume the same environment buffer, sky/fog model, wet-material state, key-light visibility, and exposure without rebuilding an acceleration structure.

## Toolchain

The checked-in project has no third-party source dependencies. The build script expects the MSYS2 UCRT64 GCC/CMake/Ninja tools installed under `C:\msys64\ucrt64\bin`.
