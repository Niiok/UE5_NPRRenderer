# 언리얼 엔진 5.6 버퍼 및 유니폼 분석 - NPR 렌더링용

> **문서 버전**: 1.1  
> **작성일**: 2026-04-11  
> **최종 수정**: 2026-04-11  
> **관련 문서**: GBuffer_Analysis_for_NPR.md

---

## 목차

1. [Scene Textures](#1-scene-textures)
2. [Depth Buffer](#2-depth-buffer)
3. [Stencil Buffer](#3-stencil-buffer)
4. [View Uniform Buffer](#4-view-uniform-buffer)
5. [Primitive Scene Data](#5-primitive-scene-data)
6. [Instance Scene Data](#6-instance-scene-data)
7. [Light Data](#7-light-data)
8. [NPR 활용 전략](#8-npr-활용-전략)

---

## 1. Scene Textures

### 1.1 개요

Scene Textures는 렌더링 파이프라인 전반에서 공유되는 텍스처들의 집합입니다.

**참조 파일**: `Engine/Shaders/Private/SceneTexturesCommon.ush`

### 1.2 주요 텍스처 목록

```hlsl
// Desktop Deferred Path
struct SceneTexturesStruct
{
    Texture2D SceneColorTexture;           // 최종 색상 (누적)
    Texture2D SceneDepthTexture;           // 장면 깊이
    Texture2D<uint2> SceneStencilTexture;  // 장면 스텐실
    
    // GBuffer Textures
    Texture2D GBufferATexture;             // WorldNormal
    Texture2D GBufferBTexture;             // Metallic/Specular/Roughness
    Texture2D GBufferCTexture;             // BaseColor
    Texture2D GBufferDTexture;             // CustomData
    Texture2D GBufferETexture;             // PrecomputedShadow
    Texture2D GBufferFTexture;             // Tangent/Anisotropy
    Texture2D GBufferVelocityTexture;      // Motion Vector
    
    // Additional
    Texture2D<uint> SceneLightingChannels; // 라이팅 채널 마스크
    Texture2D CustomDepthTexture;          // 커스텀 깊이
    Texture2D<uint> CustomStencilTexture;  // 커스텀 스텐실
    
    // AO
    Texture2D ScreenSpaceAOTexture;        // SSAO 결과
};
```

### 1.3 NPR 필요성 분류

| 텍스처 | NPR 필요성 | 용도 |
|--------|-----------|------|
| SceneColorTexture | ✅ 필수 | 누적 결과, 포스트 프로세스 입력 |
| SceneDepthTexture | ✅ 필수 | 아웃라인, 깊이 효과 |
| SceneStencilTexture | ⚠️ 선택 | 영역 마스킹 |
| GBufferA~F | ✅/⚠️ | GBuffer 문서 참조 |
| SceneLightingChannels | ⚠️ 선택 | 선택적 라이팅 |
| CustomDepthTexture | ✅ 유용 | 선택적 아웃라인 |
| CustomStencilTexture | ✅ 유용 | 오브젝트별 스타일 |
| ScreenSpaceAOTexture | ⚠️ 선택 | NPR AO 유지 시 |

### 1.4 주요 액세스 함수

```hlsl
// 깊이 샘플링
float CalcSceneDepth(float2 ScreenUV)
{
    return ConvertFromDeviceZ(SceneDepthTexture.SampleLevel(...));
}

// 커스텀 스텐실 샘플링
uint CalcSceneCustomStencil(uint2 PixelPos)
{
    return CustomStencilTexture.Load(int3(PixelPos, 0)) STENCIL_COMPONENT_SWIZZLE;
}

// 라이팅 채널 샘플링
uint GetSceneLightingChannel(uint2 PixelPos)
{
    return SceneLightingChannels.Load(int3(PixelPos, 0));
}
```

---

## 2. Depth Buffer

### 2.1 개요

깊이 버퍼는 NPR 렌더링에서 아웃라인 검출, 깊이 기반 효과 등에 핵심적으로 사용됩니다.

### 2.2 깊이 텍스처 종류

| 텍스처 | 포맷 | 용도 | NPR 필요성 |
|--------|------|------|-----------|
| SceneDepthTexture | D32F or D24S8 | 메인 장면 깊이 | ✅ 필수 |
| CustomDepthTexture | D32F | 선택적 깊이 | ✅ 유용 |

### 2.3 깊이 인코딩/디코딩

```hlsl
// Device Z → Linear Depth 변환
float ConvertFromDeviceZ(float DeviceZ)
{
    // Reversed-Z 사용 (UE5 기본)
    // DeviceZ: 0=far, 1=near
    return View.ViewToClip[3][2] / (DeviceZ - View.ViewToClip[2][2]);
}

// Linear Depth → Device Z 변환
float ConvertToDeviceZ(float LinearDepth)
{
    return View.ViewToClip[2][2] + View.ViewToClip[3][2] / LinearDepth;
}
```

### 2.4 HTile (Hierarchical Tile)

**참조 파일**: `Engine/Shaders/Private/HTileEncoding.ush`

HTile은 깊이/스텐실 버퍼의 압축된 계층 구조입니다.

```hlsl
// 타일 Min/Max 깊이 인코딩
uint EncodeTileMinMaxDepth(float MinDepth, float MaxDepth, bool bCompactVal)
{
    uint MinZ = asuint(clamp(MinDepth, HTILE_DEFAULT_XX, 1.0f));
    uint MaxZ = asuint(clamp(MaxDepth, HTILE_DEFAULT_XX, 1.0f));
    // ... 패킹 로직
    return MinZ | (Delta << 14);
}

// 타일 Min/Max 깊이 디코딩
void DecodeTileMinMax(uint EncodedTileMinMax, out float OutMin, out float OutMax)
{
    uint MinZ = EncodedTileMinMax & ((1u << 14) - 1);
    uint Delta = EncodedTileMinMax >> 14;
    OutMin = asfloat(MinZ << 18);
    OutMax = asfloat((MinZ + Delta) << 18);
}
```

### 2.5 NPR 활용

```hlsl
// 아웃라인 검출을 위한 깊이 차이 계산
float GetDepthDiscontinuity(float2 UV, float2 TexelSize)
{
    float Center = CalcSceneDepth(UV);
    float Left   = CalcSceneDepth(UV - float2(TexelSize.x, 0));
    float Right  = CalcSceneDepth(UV + float2(TexelSize.x, 0));
    float Top    = CalcSceneDepth(UV - float2(0, TexelSize.y));
    float Bottom = CalcSceneDepth(UV + float2(0, TexelSize.y));
    
    // Sobel 또는 Roberts Cross 필터 적용
    float MaxDelta = max(max(abs(Left - Right), abs(Top - Bottom)),
                        max(abs(Center - Left), abs(Center - Top)));
    return MaxDelta;
}
```

---

## 3. Stencil Buffer

### 3.1 스텐실 종류

| 종류 | 소스 | 용도 |
|------|------|------|
| Scene Stencil | SceneStencilTexture | Hi-Stencil, Lighting Channel |
| Custom Stencil | CustomStencilTexture | 사용자 정의 마스킹 |

### 3.2 Scene Stencil

**참조 파일**: `DeferredShadingCommon.ush`

```hlsl
// 플랫폼별 스텐실 스위즐
#define STENCIL_COMPONENT_SWIZZLE .g  // 대부분 플랫폼
// 또는
#define STENCIL_COMPONENT_SWIZZLE .r  // 일부 플랫폼
```

```hlsl
// Scene Stencil 샘플링
uint GetSceneStencil(uint2 PixelPos)
{
    return SceneStencilTexture.Load(int3(PixelPos, 0)) STENCIL_COMPONENT_SWIZZLE;
}
```

### 3.3 Custom Stencil

```hlsl
// Custom Stencil은 별도 텍스처로 관리
// 머티리얼에서 "Render CustomDepth Pass" 활성화 필요

uint GetCustomStencil(uint2 PixelPos)
{
    return CustomStencilTexture.Load(int3(PixelPos, 0)) STENCIL_COMPONENT_SWIZZLE;
}
```

### 3.4 스텐실 비트 활용

```cpp
// UE5에서 정의된 스텐실 비트 (예시)
// 실제 값은 프로젝트 설정에 따라 다름

// Scene Stencil Bits:
// Bit 0-2: Lighting Channel (0-7)
// Bit 3: Custom Depth 기록 여부
// Bit 4-7: Reserved / Custom

// Custom Stencil: 8비트 전체 사용자 정의
```

### 3.5 HTile 스텐실 압축/해제

```hlsl
// HTile에서 스텐실 값 압축 해제
uint DecompressStencilValue(uint HiStencil, float Depth)
{
    // HTile은 스텐실의 min/max 또는 압축 값을 저장
    // 실제 값 복원 로직
    return ExtractedStencil;
}
```

### 3.6 기본값

| 데이터 | 기본값 | 비고 |
|--------|--------|------|
| SceneStencil | `0` | 모든 비트 클리어 |
| CustomStencil | `0` | 사용자 미설정 시 |
| CustomDepth | `MAX_DEPTH` | 기록되지 않으면 최대 깊이 |

### 3.7 NPR 스텐실 활용 전략

```
┌──────────────────────────────────────────────────────────────────┐
│                  NPR Custom Stencil 비트 할당 제안                 │
├──────────┬───────────────────────────────────────────────────────┤
│  비트    │                      용도                              │
├──────────┼───────────────────────────────────────────────────────┤
│  0-2     │  NPR 스타일 ID (0-7)                                  │
│          │  0: Default, 1: Toon, 2: Hatching, 3: Watercolor...  │
├──────────┼───────────────────────────────────────────────────────┤
│  3       │  아웃라인 활성화 (0: Off, 1: On)                       │
├──────────┼───────────────────────────────────────────────────────┤
│  4       │  그림자 스타일 (0: Hard, 1: Soft)                      │
├──────────┼───────────────────────────────────────────────────────┤
│  5       │  하이라이트 스타일 (0: Circle, 1: Square)              │
├──────────┼───────────────────────────────────────────────────────┤
│  6-7     │  특수 효과 (0: None, 1: Halftone, 2: CrossHatch...)   │
└──────────┴───────────────────────────────────────────────────────┘
```

---

## 4. View Uniform Buffer

### 4.1 개요

View Uniform Buffer는 뷰/카메라 관련 데이터를 담고 있으며, `View.xxx` 형태로 접근합니다.

**참조 파일**: 
- `Engine/Shaders/Private/CommonViewUniformBuffer.ush`
- `Engine/Shaders/Shared/ViewDefinitions.h`

### 4.2 주요 변환 행렬

```hlsl
// 월드 → 클립 변환
float4x4 View.TranslatedWorldToClip;      // 카메라 원점 기준
float4x4 View.WorldToClip;                // 절대 월드 좌표

// 월드 → 뷰 변환
float4x4 View.TranslatedWorldToView;
float4x4 View.ViewToTranslatedWorld;

// 클립 → 스크린
float4x4 View.ClipToView;
float4x4 View.SVPositionToTranslatedWorld;

// 역변환
float4x4 View.ViewToClip;
float4x4 View.ClipToTranslatedWorld;
```

### 4.3 카메라 정보

```hlsl
// 카메라 위치 (Translated = 카메라 중심 기준)
float3 View.TranslatedWorldCameraOrigin;   // = (0,0,0) 상대 좌표계
float3 View.WorldCameraOrigin;             // 절대 좌표 (LWC)

// 뷰 방향
float3 View.ViewForward;                   // 카메라 전방 벡터
float3 View.ViewUp;                        // 카메라 상향 벡터
float3 View.ViewRight;                     // 카메라 우측 벡터

// 뷰 평면
float4 View.ViewFrustumPlanes[6];          // 프러스텀 평면
float View.NearPlane;                      // 근평면 거리
float View.FarPlane;                       // 원평면 거리 (없을 수 있음)
```

### 4.4 렌더 타겟 정보

```hlsl
// 버퍼 크기
float4 View.BufferSizeAndInvSize;          // xy: 크기, zw: 1/크기
float4 View.ViewSizeAndInvSize;            // xy: 뷰포트 크기, zw: 1/크기

// UV 변환
float4 View.ScreenPositionScaleBias;       // SvPosition → ScreenUV 변환
float4 View.BufferBilinearUVMinMax;        // UV 클램프 범위
```

### 4.5 시간/프레임 정보

```hlsl
float View.GameTime;                       // 게임 시간 (초)
float View.RealTime;                       // 실제 시간 (초)
float View.DeltaTime;                      // 프레임 델타 타임
uint  View.StateFrameIndex;                // 프레임 인덱스 (0,1,2...)
uint  View.StateFrameIndexMod8;            // 프레임 인덱스 % 8
float View.Random;                         // 프레임별 랜덤 값
```

### 4.6 노출/톤매핑

```hlsl
float View.PreExposure;                    // 사전 노출 스케일
float View.OneOverPreExposure;             // 1 / PreExposure
float View.EyeAdaptationValue;             // 자동 노출 값
```

### 4.7 NPR 필수 View 멤버

| 멤버 | NPR 용도 | 필수 여부 |
|------|----------|----------|
| TranslatedWorldToClip | 변환 | ✅ |
| TranslatedWorldCameraOrigin | 뷰 방향 계산 | ✅ |
| ViewForward/Up/Right | 림 라이트, 프레넬 | ✅ |
| BufferSizeAndInvSize | UV 계산 | ✅ |
| StateFrameIndex | 노이즈, 지터 | ⚠️ TAA 사용 시 |
| PreExposure | HDR 호환 | ⚠️ 선택 |
| GameTime | 애니메이션 | ⚠️ 선택 |

### 4.8 주요 기본값

| 멤버 | 기본값 | 비고 |
|------|--------|------|
| TranslatedWorldCameraOrigin | `(0, 0, 0)` | 상대 좌표계 기준 |
| PreExposure | `1.0` | 노출 보정 없음 |
| OneOverPreExposure | `1.0` | 1/PreExposure |
| GameTime | `0.0` | 게임 시작 시 |
| DeltaTime | `~0.016` | 60fps 기준 약 16ms |
| StateFrameIndex | `0` | 매 프레임 증가 |
| Random | `[0, 1]` | 프레임별 랜덤 |
| NearPlane | `10.0` | 언리얼 유닛 (cm) |

---

## 5. Primitive Scene Data

### 5.1 개요

프리미티브(메시, 액터 등) 단위의 데이터를 저장하는 구조체입니다.

**참조 파일**: `Engine/Shaders/Private/SceneData.ush`

### 5.2 구조체 정의

```hlsl
struct FPrimitiveSceneData
{
    // 변환 행렬 (Large World Coordinates)
    float4x4 LocalToWorld;
    float4x4 WorldToLocal;
    float4x4 PreviousLocalToWorld;
    
    // 바운딩
    float3 BoundsOrigin;                    // 로컬 바운딩 원점
    float  BoundsExtent;                    // 바운딩 반경
    float3 ActorWorldPosition;              // 액터 월드 위치
    
    // ID
    uint PrimitiveId;                       // 프리미티브 고유 ID
    uint InstanceSceneDataOffset;           // 인스턴스 데이터 오프셋
    uint NumInstanceSceneDataEntries;       // 인스턴스 수
    
    // 플래그
    uint Flags;                             // 아래 상세
    
    // 커스텀 데이터
    uint CustomStencilValueAndMask;         // 8비트 값 + 8비트 마스크
    float4 CustomPrimitiveData[9];          // 커스텀 float4 x 9 = 36 floats
    
    // Lightmap
    float4 LightmapUVScaleBias;
    float4 ShadowmapUVScaleBias;
    
    // 기타
    float LpvBiasMultiplier;
    float ReceiveDecalHalfDistance;
    float ObjectOrientation;
    float3 ObjectBoundsExtent;
};
```

### 5.3 주요 플래그

```cpp
// 렌더링 관련
#define PRIMITIVE_SCENE_DATA_FLAG_CAST_SHADOWS                    0x1
#define PRIMITIVE_SCENE_DATA_FLAG_CAST_SHADOWS_ON_SELF_ONLY       0x4
#define PRIMITIVE_SCENE_DATA_FLAG_DECAL_RECEIVER                  0x8
#define PRIMITIVE_SCENE_DATA_FLAG_OUTPUT_VELOCITY                 0x20
#define PRIMITIVE_SCENE_DATA_FLAG_HAS_DISTANCE_FIELD              0x40
#define PRIMITIVE_SCENE_DATA_FLAG_HAS_CAST_CONTACT_SHADOW         0x200

// 라이팅 채널
#define PRIMITIVE_SCENE_DATA_FLAG_LIGHTING_CHANNEL_0              0x800
#define PRIMITIVE_SCENE_DATA_FLAG_LIGHTING_CHANNEL_1              0x1000
#define PRIMITIVE_SCENE_DATA_FLAG_LIGHTING_CHANNEL_2              0x2000

// 특수 기능
#define PRIMITIVE_SCENE_DATA_FLAG_WRITES_CUSTOM_DEPTH_STENCIL     0x100000
#define PRIMITIVE_SCENE_DATA_FLAG_HOLDOUT                         0x400000
#define PRIMITIVE_SCENE_DATA_FLAG_SPLINE_MESH                     0x800000
#define PRIMITIVE_SCENE_DATA_FLAG_INSTANCE_DRAW                   0x1000000
#define PRIMITIVE_SCENE_DATA_FLAG_EVALUATE_WPO                    0x2000000

// 뷰 모드
#define PRIMITIVE_SCENE_DATA_FLAG_IS_FIRST_PERSON                 0x40000000
```

### 5.4 접근 방법

```hlsl
// 인덱스로 프리미티브 데이터 접근
FPrimitiveSceneData GetPrimitiveData(uint PrimitiveId)
{
    return Scene.PrimitiveSceneData[PrimitiveId];
}

// 현재 프리미티브 데이터 (BasePass 등)
FPrimitiveSceneData PrimitiveData = GetPrimitiveData(PrimitiveIndex);

// 플래그 확인
bool bCastShadows = (PrimitiveData.Flags & PRIMITIVE_SCENE_DATA_FLAG_CAST_SHADOWS) != 0;
uint LightingChannelMask = (PrimitiveData.Flags >> 11) & 0x7;  // Channel 0,1,2
```

### 5.5 주요 기본값

| 데이터 | 기본값 | 비고 |
|--------|--------|------|
| LocalToWorld | Identity | 단위 행렬 |
| Flags | `0` | 모든 플래그 비활성 |
| CustomStencilValueAndMask | `0` | 스텐실 값 0, 마스크 0 |
| CustomPrimitiveData[0-8] | `(0,0,0,0)` x 9 | 모두 0으로 초기화 |
| LightingChannelMask | `0x1` | Channel 0만 활성 |

### 5.6 NPR 활용

| 데이터 | NPR 용도 |
|--------|----------|
| CustomStencilValueAndMask | 오브젝트별 스타일 ID |
| CustomPrimitiveData[0-8] | NPR 파라미터 (아웃라인 두께, 색상 오프셋 등) |
| Flags.LightingChannel | 선택적 NPR 라이팅 |
| Flags.Holdout | 합성용 마스크 |
| ActorWorldPosition | 오브젝트 중심 기반 효과 |
| ObjectBoundsExtent | 크기 기반 아웃라인 스케일 |

---

## 6. Instance Scene Data

### 6.1 개요

인스턴스(Instanced Static Mesh 등) 단위의 데이터입니다.

**참조 파일**: `Engine/Shaders/Private/SceneData.ush`

### 6.2 구조체 정의

```hlsl
struct FInstanceSceneData
{
    // 변환
    float4x4 LocalToWorld;
    float4x4 PreviousLocalToWorld;
    
    // ID
    uint PrimitiveId;                       // 부모 프리미티브 ID
    uint RelativeId;                        // 프리미티브 내 상대 ID
    
    // 커스텀 데이터
    uint CustomDataOffset;                  // 커스텀 데이터 시작 오프셋
    uint CustomDataCount;                   // 커스텀 데이터 개수
    
    // 기타
    float RandomID;                         // 인스턴스별 랜덤 값 [0,1]
    uint Flags;
    
    // LOD
    float3 LocalBoundsCenter;
    float3 LocalBoundsExtent;
};
```

### 6.3 주요 플래그

```cpp
#define INSTANCE_SCENE_DATA_FLAG_CAST_SHADOWS                     0x1
#define INSTANCE_SCENE_DATA_FLAG_DETERMINANT_SIGN                 0x2
#define INSTANCE_SCENE_DATA_FLAG_CUSTOM_DATA_START               16  // 비트 16부터 커스텀
```

### 6.4 주요 기본값

| 데이터 | 기본값 | 비고 |
|--------|--------|------|
| LocalToWorld | Identity | 단위 행렬 |
| RandomID | `[0, 1]` | 인스턴스 생성 시 랜덤 할당 |
| Flags | `0` | 모든 플래그 비활성 |
| CustomDataOffset | `0` | 오프셋 없음 |
| CustomDataCount | `0` | 커스텀 데이터 없음 |

### 6.5 NPR 활용

| 데이터 | NPR 용도 |
|--------|----------|
| RandomID | 인스턴스별 색상 변형, 노이즈 시드 |
| CustomDataOffset/Count | 인스턴스별 NPR 파라미터 |
| LocalBoundsExtent | 크기 기반 아웃라인/LOD |

---

## 7. Light Data

### 7.1 개요

라이트 정보를 담는 구조체들입니다.

**참조 파일**: `Engine/Shaders/Private/LightData.ush`

### 7.2 Directional Light Data

```hlsl
struct FDirectionalLightData
{
    // 기본 속성
    float3 DirectionalLightColor;           // 라이트 색상 (강도 포함)
    float  DirectionalLightShadowMaskSelect;
    float3 DirectionalLightDirection;       // 라이트 방향 (정규화)
    
    // 그림자
    uint   DirectionalLightShadowMapChannelMask;
    float  DirectionalLightDistanceFadeMAD;
    float  DirectionalLightSpotAngles;      // (Spot일 때)
    
    // Atmosphere/Sky
    bool   DirectionalLightUseStaticShadowing;
    bool   DirectionalLightUseSkyAtmosphere;
    
    // 추가
    float  DirectionalLightSourceRadius;
    float  DirectionalLightSoftSourceRadius;
    float  DirectionalLightVolumetricScatteringIntensity;
};
```

### 7.3 Light Shader Parameters

```hlsl
struct FLightShaderParameters
{
    // 위치/방향
    float3 TranslatedWorldPosition;         // 카메라 기준 위치
    float  InvRadius;                       // 1 / 감쇠 반경
    float3 Color;                           // 색상
    float  FalloffExponent;                 // 감쇠 지수
    float3 Direction;                       // 방향 (정규화)
    float  SpecularScale;                   // 스페큘러 스케일
    
    // Tangent (Area Light)
    float3 Tangent;
    float  SourceRadius;
    
    // Spotlight
    float2 SpotAngles;                      // cos(Inner), cos(Outer)
    float  SoftSourceRadius;
    float  SourceLength;                    // 튜브 라이트 길이
    
    // Rect Light
    float  RectLightBarnCosAngle;
    float  RectLightBarnLength;
    float2 RectLightAtlasUVOffset;
    float2 RectLightAtlasUVScale;
    float  RectLightAtlasMaxLevel;
    
    // IES
    float  IESAtlasIndex;
};
```

### 7.4 Lighting Channel

```cpp
#define LIGHTING_CHANNEL_MASK 0x7           // 3비트 (0-7)

// 라이트와 프리미티브의 채널 비교
bool IsLightInChannel(uint LightChannelMask, uint PrimitiveChannelMask)
{
    return (LightChannelMask & PrimitiveChannelMask) != 0;
}
```

### 7.5 주요 기본값

| 데이터 | 기본값 | 비고 |
|--------|--------|------|
| DirectionalLightDirection | `(0, 0, -1)` | 아래 방향 (에디터 기본) |
| DirectionalLightColor | `(1, 1, 1)` | 흰색, 강도 1 |
| InvRadius | `0.001` | 반경 1000 유닛 기준 |
| FalloffExponent | `2.0` | 역제곱 감쇠 |
| SpecularScale | `1.0` | 스페큘러 100% |
| SpotAngles | `(-1, -2)` | 비활성 (Point Light) |
| LightingChannelMask | `0x1` | Channel 0만 활성 |
| SourceRadius | `0.0` | 점광원 |

### 7.6 NPR 활용

| 데이터 | NPR 용도 |
|--------|----------|
| DirectionalLightDirection | N·L 톤 쉐이딩 계산 |
| DirectionalLightColor | 라이트 색상 (강도 분리 가능) |
| LightingChannelMask | 선택적 라이팅 |
| InvRadius | 거리 기반 효과 (선택적) |
| SpotAngles | 스팟라이트 마스크 |

---

## 8. NPR 활용 전략

### 8.1 필수 유지 데이터

```
┌──────────────────────────────────────────────────────────────────┐
│                       NPR 필수 유지 데이터                        │
├────────────────────┬─────────────────────────────────────────────┤
│ 분류               │ 데이터                                       │
├────────────────────┼─────────────────────────────────────────────┤
│ Depth              │ SceneDepthTexture (아웃라인, 깊이 효과)      │
│ Color              │ BaseColor (기본 색상)                        │
│ Normal             │ WorldNormal (N·L 계산)                       │
│ Transform          │ View.TranslatedWorldToClip 등                │
│ Camera             │ View.TranslatedWorldCameraOrigin             │
│ Light Direction    │ DirectionalLightDirection                    │
│ Light Color        │ DirectionalLightColor                        │
└────────────────────┴─────────────────────────────────────────────┘
```

### 8.2 재활용 가능 데이터

```
┌──────────────────────────────────────────────────────────────────┐
│                     NPR 재활용 가능 데이터                        │
├────────────────────┬─────────────────────────────────────────────┤
│ 원래 용도          │ NPR 재활용 제안                              │
├────────────────────┼─────────────────────────────────────────────┤
│ Metallic           │ 톤 단계 수, 림 강도                          │
│ Specular           │ 하이라이트 크기/선명도                       │
│ CustomData         │ NPR 전용 파라미터                            │
│ PrecomputedShadow  │ 추가 마스크 (정적 라이팅 미사용 시)          │
│ WorldTangent       │ Flow Map (Anisotropy 미사용 시)              │
│ CustomStencil      │ 오브젝트별 스타일 ID                         │
│ CustomPrimitiveData│ 아웃라인 두께, 색상 오프셋 등                │
│ Instance.RandomID  │ 인스턴스별 색상 변형                         │
└────────────────────┴─────────────────────────────────────────────┘
```

### 8.3 제거 가능 데이터

```
┌──────────────────────────────────────────────────────────────────┐
│                 NPR에서 불필요할 수 있는 데이터                   │
├────────────────────┬─────────────────────────────────────────────┤
│ 데이터             │ 조건                                         │
├────────────────────┼─────────────────────────────────────────────┤
│ ScreenSpaceAOTexture│ SSAO 미사용 시                              │
│ GBufferVelocity    │ Motion Blur/TAA 미사용 시                    │
│ IndirectIrradiance │ GI 미사용 시                                 │
│ PreExposure        │ 고정 노출 사용 시                            │
│ 복잡한 감쇠 계산   │ 단순 감쇠로 대체                            │
└────────────────────┴─────────────────────────────────────────────┘
```

### 8.4 코드 참조 예시

```hlsl
// NPR 톤 쉐이딩 예시
half3 NPRToonShading(FGBufferData GBuffer, FDirectionalLightData Light)
{
    // 필수 데이터 사용
    half3 N = GBuffer.WorldNormal;
    half3 L = Light.DirectionalLightDirection;
    half3 BaseColor = GBuffer.BaseColor;
    half3 LightColor = Light.DirectionalLightColor;
    
    // 재활용 데이터 사용
    half ToonSteps = GBuffer.Metallic * 14 + 2;  // 2~16 단계
    half HighlightSize = GBuffer.Specular;
    
    // N·L 계산
    half NdotL = dot(N, L);
    
    // 톤 양자화
    half ToonNdotL = floor(saturate(NdotL) * ToonSteps) / ToonSteps;
    
    // 최종 색상
    return BaseColor * LightColor * ToonNdotL;
}
```

---

## 부록: 데이터 흐름 다이어그램

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Material  │────▶│   GBuffer   │────▶│   Lighting  │
│    Graph    │     │   Pass      │     │    Pass     │
└─────────────┘     └─────────────┘     └─────────────┘
      │                    │                   │
      │                    │                   │
      ▼                    ▼                   ▼
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  Primitive  │     │  GBufferA   │     │   View UB   │
│  SceneData  │     │  GBufferB   │     │  Light UB   │
│             │     │  GBufferC   │     │             │
│  Instance   │     │  GBufferD   │     │  Shadow     │
│  SceneData  │     │  Depth      │     │  Maps       │
└─────────────┘     └─────────────┘     └─────────────┘
                           │
                           │
                           ▼
                    ┌─────────────┐
                    │    NPR      │
                    │   Shading   │
                    │   (BRDF)    │
                    └─────────────┘
                           │
                           ▼
                    ┌─────────────┐
                    │    Post     │
                    │   Process   │
                    │  (Outline)  │
                    └─────────────┘
```

---

## 변경 이력

| 버전 | 날짜 | 변경 내용 |
|------|------|----------|
| 1.0 | 2026-04-11 | 초기 문서 작성 |
| 1.1 | 2026-04-11 | 각 섹션별 주요 기본값 추가 (View, Primitive, Instance, Light, Stencil) |
