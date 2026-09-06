# NPR Renderer Plugin

**A Non-Photorealistic Rendering plugin for Unreal Engine**

## Overview

NPR Renderer processes direct lighting with NPR-specific deferred per-light passes. It uses the GBuffer and the scene's visible lights to composite toon shading, shadow color, sky ambient lighting, reflection captures, and screen-space reflections into `SceneColor`.

The engine's `RenderLights()` pass does not run; the plugin renders each visible light directly. No engine-source modifications are required.

## Rendering Pipeline

1. `SetupViewFamily()` disables the engine's deferred direct lighting.
2. The `PostRenderBasePassDeferred` hook receives the GBuffer and scene textures.
3. Renderable directional, point, spot, and rect lights are collected from the scene.
4. An NPR deferred light pass runs for each light, additively accumulates direct lighting into `SceneColor`, and writes its lighting result to `ShadowMask`.
5. `NPRShadowPixelShader` additively composites the NPR shadow color into `SceneColor` using the accumulated `ShadowMask`.
6. An NPR sky/reflection pass handles the Sky Light, reflection captures, and SSR.

## Direct Lighting

Direct lighting uses a deferred light shader with `FDeferredLightData` and `DeferredLightUniforms`. Shader permutations account for light shape, shadow type, rect-light source textures, lighting channels, and shadow quality.

| Light type | Rendering |
| --- | --- |
| Directional Light | Fullscreen deferred pass with up to four CSM cascades |
| Point Light | Light-volume deferred pass with one-pass cubemap shadow support |
| Spot Light | Light-volume deferred pass with 2D-shadow support |
| Rect Light | Light-volume deferred pass with source-texture and 2D-shadow support |

## Shadow Compositing

Each NPR light pass evaluates its per-light shadow data and writes a lighting mask to `ShadowMask`. The mask is cleared to `0` at the beginning of the frame and uses max blending across all light passes: `0` represents fully shadowed, while a value near `1` means that at least one light illuminates the pixel.

After all light passes complete, `NPRShadowPixelShader` runs as a fullscreen pass. It point-loads the mask and the NPR shadow color from `GBufferD.rgb`, then outputs:

```text
ShadowColor * (1 - ShadowMask)
```

The fullscreen shadow pass uses additive blending, so this output is added to `SceneColor`. Fully lit pixels do not add a shadow color.

## Light Grid and Forward Light Data

Direct NPR lighting does not use the Light Grid. Visible lights are collected from the scene and rendered through individual deferred passes.

`ForwardLightStruct` is used only by the NPR sky/reflection pass to provide the reflection-capture culling grid. It is not used as the direct-light list or as deferred light data.

## Available Resources

| Resource | Access |
| --- | --- |
| GBuffer | Scene texture binding |
| Scene depth | Scene texture binding |
| Deferred light data | Per-light `FDeferredLightData` / `DeferredLightUniforms` |
| Visible lights | Scene light collection |
| Shadow data | Per-light shadow information from `FVisibleLightInfo` |
| NPR shadow color | `GBufferD.rgb` |
| Shadow mask | `ShadowMask`, accumulated with max blending across light passes |
| Reflection-capture culling grid | `ForwardLightStruct` in the sky/reflection pass |

## Installation

1. Copy the `NPRRenderer` folder to the project's `Plugins` directory.
2. Generate project files.
3. Enable **NPR Renderer** in **Edit > Plugins** in the Editor.

## Usage

### Material Setup

The plugin generates the `MF_NPR_Encode` material function at `/Game/NPR/MaterialFunctions/MF_NPR_Encode`. It accepts the material's PBR values and NPR shadow controls, then provides encoded outputs for the material's standard inputs.

Create a duplicate of this generated function in a project-owned folder, for example `/Game/Materials/NPR/MF_NPR_Encode_Project`, and use the duplicate in materials. Treat the generated function as disposable: plugin setup can recreate it, which can break existing material-function connections.

In each material, add a **Material Function Call** to the duplicate and connect its outputs to the corresponding material inputs:

| Material-function output | Material input |
| --- | --- |
| `Metallic` | `Metallic` |
| `Specular` | `Specular` |
| `Roughness` | `Roughness` |

The function inputs are `Metallic`, `Specular`, `Roughness`, `NPR_ShadowThreshold`, `NPR_ShadowBrightness`, and `NPR_ShadowSoftness`.

### Blueprint API

```blueprint
Set NPR Enabled (true/false)
Is NPR Enabled -> bool

Set Toon Steps (3)
Set Toon Smoothness (0.1)
Set Shadow Intensity (0.5)

Set Specular Size (0.05)
Set Specular Intensity (0.8)

Set Rim Color (White)
Set Rim Power (3.0)
```

### Console Commands

```text
r.NPR.Toggle
r.NPR.On
r.NPR.Off
```

## Source Layout

```text
NPRRenderer/
├── Shaders/
│   ├── Private/
│   │   ├── NPRLightPixelShader.usf
│   │   ├── NPRShadowPixelShader.usf
│   │   ├── NPRSkyLightPixelShader.usf
│   │   ├── NPRBxDF.ush
│   │   └── NPRShadowUtils.ush
│   └── Public/
│       └── NPREncoding.ush
└── Source/NPRRenderer/
    ├── Public/
    │   ├── NPRSceneViewExtension.h
    │   ├── NPRLightRendering.h
    │   └── DeferredLightUniformWrapper.h
    └── Private/
        ├── NPRSceneViewExtension.cpp
        ├── NPRLightRendering.cpp
        ├── NPRLightRenderingUtils.cpp
        └── DeferredLightUniformWrapper.cpp
```

## License

Copyright (c) 2024. All rights reserved.
