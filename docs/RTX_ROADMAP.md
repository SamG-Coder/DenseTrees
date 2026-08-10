# RTX implementation status

Dense Trees keeps biological simulation independent from the rendering backend. This permits a fast interactive preview today while the DirectX 12 renderer is built without duplicating growth logic.

## Active now

- High-performance adapter selection and DirectX 12 feature interrogation
- Hardware Direct3D rasterization
- GPU vertex animation for foliage wind
- Procedural GPU bark, leaf-transmission, and ground shading
- Compact indexed branch and leaf meshes
- CPU light/carbon growth model with deterministic species presets

## Next rendering backend

1. DirectX 12 device, flip-model swap chain, descriptor heaps, upload ring, and timestamp queries.
2. GPU-driven mesh submission with ExecuteIndirect and mesh shaders where supported.
3. DXR bottom-level acceleration structures for branch tubes and leaf clusters, then a refittable top-level structure.
4. Progressive path tracing with physically based bark, thin-sheet leaf transmission, multiple importance sampling, and an HDR environment.
5. Ray-query canopy irradiance fed back into the growth simulation.
6. DLSS Super Resolution and Ray Reconstruction through Streamline after motion vectors, depth, exposure, and jitter are correct.
7. OptiX linear swept spheres for fine twigs and needles on Blackwell, with triangle fallback.
8. Neural texture compression/material experiments only after conventional reference materials are validated.

Tensor Cores cannot be consumed merely by compiling a shader, and RT Cores are not activated by a marketing flag. Each feature above requires its specific data, synchronization, quality validation, and fallback path.
