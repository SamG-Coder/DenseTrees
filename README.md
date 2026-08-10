# Dense Trees

Dense Trees is a native C++ procedural tree laboratory. The first milestone grows a tree by distributing attraction points through a crown and iteratively extending branches toward locally available light. It then applies a pipe-model approximation to branch thickness and builds tapered branch tubes and leaf clusters for real-time rendering.

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
- Left/Right arrows: move the sun and regrow toward the new light environment
- Escape: exit

## Architecture

- `tree.cpp`: platform-independent growth simulation and CPU mesh generation
- `renderer.cpp`: native Direct3D real-time preview
- `main.cpp`: Win32 application, camera, and regeneration controls
- `tests/tree_tests.cpp`: determinism, hierarchy, and mesh integrity checks

The application queries the selected high-performance adapter through Direct3D 12 and reports its DXR, mesh-shader, and sampler-feedback tiers. The interactive renderer remains on a low-overhead Direct3D 11 preview path while simulation code stays API-independent. The next renderer milestone is a dedicated Direct3D 12/DXR path: acceleration structures for branch meshes, alpha-tested leaf geometry, a progressive path tracer, and optional DLSS Ray Reconstruction. Fine twigs can subsequently be represented with Blackwell linear swept spheres through OptiX or NVAPI.

## Toolchain

The checked-in project has no third-party source dependencies. The build script expects the MSYS2 UCRT64 GCC/CMake/Ninja tools installed under `C:\msys64\ucrt64\bin`.
