# 언리얼 엔진 5.6 GBuffer 분석 - NPR 렌더링 전환용

> **문서 버전**: 1.1  
> **작성일**: 2026-04-11  
> **최종 수정**: 2026-04-11  
> **목적**: NPR(Non-Photorealistic Rendering) 파이프라인 전환 시 기존 GBuffer 채널 재활용 가이드

---

## 목차

1. [개요](#1-개요)
2. [GBuffer 레이아웃](#2-gbuffer-레이아웃)
3. [채널별 상세 분석](#3-채널별-상세-분석)
4. [NPR 재활용 권장안](#4-npr-재활용-권장안)
5. [기타 버퍼 및 유니폼](#5-기타-버퍼-및-유니폼)
6. [쉐이딩 모델 ID](#6-쉐이딩-모델-id)
7. [참조 파일 목록](#7-참조-파일-목록)

---

## 1. 개요

### 1.1 문서 목적

이 문서는 언리얼 엔진 5.6의 Deferred Shading 파이프라인에서 사용되는 GBuffer 구조를 분석하여, NPR 렌더링으로 전환 시 각 채널을 새로운 용도로 재활용하기 위한 참조 자료입니다.

### 1.2 핵심 원칙

- **기존 채널 유지**: GBuffer 레이아웃은 변경하지 않고 채널의 의미만 재정의
- **점진적 전환**: BRDF만 우선 교체하고, 외부 시스템은 점진적으로 수정
- **호환성 유지**: 기존 엔진 기능(SSR, Reflection 등)과의 호환성 고려

### 1.3 분류 기준

| 분류 | 설명 |
|------|------|
| 🔴 **BRDF 외부 의존** | BRDF 외 다른 시스템에서도 사용되어 의미 변경 시 주의 필요 |
| 🟡 **조건부 재활용** | 특정 기능 비활성화 시 자유롭게 재활용 가능 |
| 🟢 **자유 재활용** | BRDF에서만 사용되어 NPR용으로 자유롭게 재정의 가능 |
| ⚪ **유지 필수** | NPR에서도 동일한 용도로 반드시 유지 |

### 1.4 GBuffer 기본값 요약 (Quick Reference)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      GBuffer 기본값 요약 테이블                              │
├─────────────────────┬──────────────────┬────────────────────────────────────┤
│       채널          │      기본값       │             비고                   │
├─────────────────────┼──────────────────┼────────────────────────────────────┤
│ WorldNormal         │ (0, 0, 1)        │ 표면 노말 (Tangent Space)          │
│ PerObjectGBufferData│ 0                │ 모든 플래그 비활성                  │
├─────────────────────┼──────────────────┼────────────────────────────────────┤
│ Metallic            │ 0.0              │ 비금속                             │
│ Specular            │ 0.5              │ F0 ≈ 0.04 (일반 비금속)            │
│ Roughness           │ 0.5              │ 중간 거칠기                        │
│ ShadingModelID      │ 1                │ DEFAULT_LIT                        │
│ SelectiveOutputMask │ 0                │ 모든 마스크 비활성                  │
├─────────────────────┼──────────────────┼────────────────────────────────────┤
│ BaseColor           │ (0.5, 0.5, 0.5)  │ 머티리얼에서 설정 (일반적 기본값)   │
│ GBufferAO           │ 1.0              │ 오클루전 없음                       │
│ IndirectIrradiance  │ 1.0              │ 최대 간접광 (정적 라이팅 시)        │
├─────────────────────┼──────────────────┼────────────────────────────────────┤
│ CustomData          │ (0, 0, 0, 0)     │ DEFAULT_LIT 기준                   │
├─────────────────────┼──────────────────┼────────────────────────────────────┤
│ PrecomputedShadow   │ (1, 1, 1, 1)     │ 그림자 없음 (정적 라이팅 시)        │
├─────────────────────┼──────────────────┼────────────────────────────────────┤
│ WorldTangent        │ (1, 0, 0)        │ X축 (Anisotropy 활성 시)           │
│ Anisotropy          │ 0.0              │ 등방성 (비등방성 효과 없음)         │
├─────────────────────┼──────────────────┼────────────────────────────────────┤
│ Velocity            │ (0, 0)           │ 움직임 없음                        │
└─────────────────────┴──────────────────┴────────────────────────────────────┘
```

---

## 2. GBuffer 레이아웃

### 2.1 Desktop Deferred Shading 레이아웃

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        GBuffer Layout (UE 5.6)                               │
├──────────┬──────────┬──────────┬──────────┬──────────────────────────────────┤
│  Buffer  │    R     │    G     │    B     │              A                   │
├──────────┼──────────┼──────────┼──────────┼──────────────────────────────────┤
│ GBufferA │     WorldNormal.x/y/z (Encoded) │   PerObjectGBufferData          │
│ (RGB10A2)│                                 │   (Contact Shadow 등)           │
├──────────┼──────────┼──────────┼──────────┼──────────────────────────────────┤
│ GBufferB │ Metallic │ Specular │ Roughness│ ShadingModelID(4b) +             │
│ (RGBA8)  │  [0,1]   │  [0,1]   │  [0,1]   │ SelectiveOutputMask(4b)          │
├──────────┼──────────┼──────────┼──────────┼──────────────────────────────────┤
│ GBufferC │        BaseColor.rgb (sRGB)     │ IndirectIrradiance /             │
│ (RGBA8)  │           [0,1]³                │ GBufferAO / DiffuseIndirect      │
├──────────┼──────────┼──────────┼──────────┼──────────────────────────────────┤
│ GBufferD │            CustomData.rgba                                        │
│ (RGBA8)  │      (쉐이딩 모델별 상이 - 하단 상세 참조)                         │
├──────────┼──────────┼──────────┼──────────┼──────────────────────────────────┤
│ GBufferE │        PrecomputedShadowFactors.rgba                              │
│ (RGBA8)  │            (정적 라이팅 전용)                                      │
├──────────┼──────────┼──────────┼──────────┼──────────────────────────────────┤
│ GBufferF │      WorldTangent.xyz (Encoded) │        Anisotropy               │
│ (RGBA8)  │       (Anisotropy 있을 때만)    │         [-1,1]                  │
├──────────┼──────────┼──────────┼──────────┼──────────────────────────────────┤
│ Velocity │         Motion Vector (RG16F 또는 RGBA16F)                        │
└──────────┴──────────────────────────────────────────────────────────────────┘
```

### 2.2 관련 텍스처

| 텍스처 | 포맷 | 용도 |
|--------|------|------|
| SceneDepthTexture | D32F / D24S8 | 장면 깊이 |
| CustomDepthTexture | D32F | 커스텀 깊이 (아웃라인 등) |
| CustomStencilTexture | R8UI | 커스텀 스텐실 마스크 |
| SceneLightingChannels | R8UI | 라이팅 채널 마스크 |
| ScreenSpaceAOTexture | R8 | SSAO 결과 |

---

## 3. 채널별 상세 분석

### 3.1 GBufferA - WorldNormal + PerObjectData

#### 3.1.1 WorldNormal (RGB)

| 항목 | 내용 |
|------|------|
| **PBR 용도** | 라이팅 계산의 기본 방향 벡터 (N·L, N·V, N·H 등) |
| **인코딩** | `EncodeNormal()`: N * 0.5 + 0.5 (또는 Octahedron) |
| **값 범위** | [-1, 1] → [0, 1] 인코딩 후 저장 |
| **기본값** | `(0, 0, 1)` (Tangent Space) → 표면 노말 방향 |
| **BRDF 사용** | 모든 라이팅 계산의 핵심 |
| **외부 사용** | SSR 반사 방향, Shadow 계산, GI 등 |
| **NPR 필요성** | ⚪ **유지 필수** - 톤 쉐이딩 N·L 계산에 필수 |

#### 3.1.2 PerObjectGBufferData (A)

| 항목 | 내용 |
|------|------|
| **PBR 용도** | 2비트 플래그 패킹 |
| **비트 구조** | Bit0: Cast Contact Shadow, Bit1: Has Dynamic Indirect Shadow |
| **값 범위** | 0, 0.33, 0.66, 1.0 (2비트 양자화) |
| **기본값** | `0` (모든 플래그 비활성) |
| **BRDF 사용** | 직접 사용 안 함 |
| **외부 사용** | Contact Shadow 패스, Indirect Shadow 판단 |
| **NPR 필요성** | ⚪ **유지 필수** - Contact Shadow 유지 시 |

---

### 3.2 GBufferB - PBR 파라미터

#### 3.2.1 Metallic (R)

| 항목 | 내용 |
|------|------|
| **PBR 용도** | 금속성 정도, F0/DiffuseColor 계산에 사용 |
| **계산식** | `SpecularColor = lerp(DielectricF0, BaseColor, Metallic)` |
|           | `DiffuseColor = BaseColor * (1 - Metallic)` |
| **값 범위** | [0, 1] - 0=비금속, 1=금속 |
| **기본값** | **`0.0`** (비금속) |
| **BRDF 사용** | `ComputeF0()`, `ComputeDiffuseAlbedo()` |
| **외부 사용** | Eye: CausticNormalDelta.x 저장용 재활용 |
|              | Hair: HAIR_SCATTER 저장용 재활용 |
|              | RayTracing: IOR 계산 |
| **NPR 필요성** | 🟡 **조건부** - Eye/Hair 미사용 시 자유 재활용 |

**NPR 재활용 제안**:
```
- 톤 쉐이딩 단계 수 (예: 2~16 단계)
- 림 라이트 강도
- 스타일라이즈드 반사 강도
- 커스텀 쉐이딩 파라미터 1
```

#### 3.2.2 Specular (G)

| 항목 | 내용 |
|------|------|
| **PBR 용도** | 비금속의 Fresnel F0 결정 |
| **계산식** | `F0 = 0.08 * Specular` (비금속일 때) |
| **값 범위** | [0, 1] |
| **기본값** | **`0.5`** (F0 ≈ 0.04, 일반적인 비금속) |
| **BRDF 사용** | `ComputeF0()`, `DielectricSpecularToF0()` |
| **외부 사용** | Eye: CausticNormalDelta.y 저장용 재활용 |
|              | SSR: `MinSpecular` 비교 조건 |
|              | Water: Volume Lighting |
|              | Hair: Sampling |
| **NPR 필요성** | 🟡 **조건부** - SSR MinSpecular 수정 시 자유 재활용 |

**NPR 재활용 제안**:
```
- 스타일라이즈드 하이라이트 크기
- 하이라이트 선명도 (Hard/Soft Edge)
- 아웃라인 두께 보조 파라미터
- 커스텀 쉐이딩 파라미터 2
```

#### 3.2.3 Roughness (B)

| 항목 | 내용 |
|------|------|
| **PBR 용도** | GGX NDF의 α 파라미터 (α = Roughness²) |
| **계산식** | `a2 = Pow4(Roughness)` for GGX |
| **값 범위** | [0, 1] - 0=거울, 1=완전 확산 |
| **기본값** | **`0.5`** (중간 정도의 거칠기) |
| **BRDF 사용** | `D_GGX()`, `Vis_SmithJointApprox()`, `Diffuse_Burley()` |
| **외부 사용** | ⚠️ **광범위하게 사용됨** |
|              | SSR: `MaxRoughness` 비교, LOD 선택 |
|              | Reflection Environment: `GetOffSpecularPeakReflectionDir()` |
|              |                         `GetSpecularOcclusion()` |
|              | Single Layer Water: `EnvBRDF()` |
|              | Sky Lighting: Energy Conservation |
|              | Bent Normal: `ApplyBentNormal()` |
|              | MegaLights: Shading Data |
| **NPR 필요성** | 🔴 **BRDF 외부 의존** - 의미 변경 시 다수 시스템 수정 필요 |

**NPR 재활용 시 주의사항**:
```
⚠️ Roughness는 SSR/Reflection에서 블러/LOD 결정에 사용됨
   - NPR에서 다른 의미로 사용 시 반사 품질에 영향
   - 권장: 값 범위는 유지하되 의미만 재해석
   
가능한 재해석:
- 0 = 선명한 스타일라이즈드 반사
- 1 = 흐린/없는 반사
→ SSR이 "Roughness가 높으면 반사 안 함"으로 동작하도록 활용
```

#### 3.2.4 ShadingModelID + SelectiveOutputMask (A)

| 항목 | 내용 |
|------|------|
| **구조** | 하위 4비트: ShadingModelID, 상위 4비트: Mask |
| **ShadingModelID** | 0~15 (현재 13개 사용) |
| **SelectiveOutputMask** | HAS_ANISOTROPY, SKIP_PRECSHADOW 등 |
| **기본값** | **`1`** (SHADINGMODELID_DEFAULT_LIT), Mask=`0` |
| **NPR 필요성** | ⚪ **유지 필수** - NPR 쉐이딩 모델 구분에 사용 |

---

### 3.3 GBufferC - BaseColor + 보조 데이터

#### 3.3.1 BaseColor (RGB)

| 항목 | 내용 |
|------|------|
| **PBR 용도** | 표면의 기본 색상 (Albedo) |
| **값 범위** | [0, 1]³ (sRGB 공간 저장) |
| **기본값** | 머티리얼 설정 필수 (일반적으로 **`(0.5, 0.5, 0.5)`** 또는 흰색) |
| **BRDF 사용** | DiffuseColor, SpecularColor 계산의 기반 |
| **외부 사용** | 모든 라이팅 결과에 곱해짐 |
| **NPR 필요성** | ⚪ **유지 필수** - 기본 색상은 항상 필요 |

#### 3.3.2 IndirectIrradiance / GBufferAO (A)

| 항목 | 내용 |
|------|------|
| **조건부 용도** | `ALLOW_STATIC_LIGHTING`: IndirectIrradiance (로그 인코딩) |
|                | 그 외: GBufferAO |
|                | `GBUFFER_HAS_DIFFUSE_SAMPLE_OCCLUSION`: 8비트 오클루전 마스크 |
| **값 범위** | [0, 1] |
| **기본값** | **`1.0`** (오클루전 없음 / 최대 간접광) |
| **BRDF 사용** | 간접광 강도 조절, AO 적용 |
| **외부 사용** | Reflection Environment, Sky Lighting |
| **NPR 필요성** | 🟡 **조건부** - 정적 라이팅/GI 사용 여부에 따라 |

---

### 3.4 GBufferD - CustomData (쉐이딩 모델별)

| 쉐이딩 모델 | R | G | B | A | 기본값 |
|------------|---|---|---|---|--------|
| **DEFAULT_LIT** | - | - | - | - | `(0, 0, 0, 0)` |
| **SUBSURFACE** | SubsurfaceColor.r | SubsurfaceColor.g | SubsurfaceColor.b | Opacity | `(1, 1, 1, 1)` |
| **PREINTEGRATED_SKIN** | SubsurfaceColor.r | SubsurfaceColor.g | SubsurfaceColor.b | Opacity | `(1, 1, 1, 1)` |
| **CLEAR_COAT** | ClearCoat | ClearCoatRoughness | (Bottom Normal Oct.x) | (Bottom Normal Oct.y) | `(0, 0.5, 0.5, 0.5)` |
| **SUBSURFACE_PROFILE** | ProfileID | - | - | Opacity | `(0, 0, 0, 1)` |
| **TWOSIDED_FOLIAGE** | SubsurfaceColor.r | SubsurfaceColor.g | SubsurfaceColor.b | - | `(1, 1, 1, 0)` |
| **HAIR** | - | - | Backlit | - | `(0, 0, 1, 0)` |
| **CLOTH** | FuzzColor.r | FuzzColor.g | FuzzColor.b | Cloth | `(0, 0, 0, 0)` |
| **EYE** | (SubsurfaceProfile) | IrisNormal.x | IrisNormal.y | IrisMask | `(0, 0.5, 0.5, 0)` |

**NPR 필요성**: 🟢 **자유 재활용** - 해당 쉐이딩 모델 미사용 시

**NPR 재활용 제안**:
```
NPR 전용 쉐이딩 모델 정의 시 CustomData 활용:
- R: 톤 쉐이딩 램프 텍스처 인덱스
- G: 아웃라인 색상 오프셋
- B: 스타일라이즈드 SSS 강도
- A: 특수 효과 마스크 (하프톤, 해칭 등)
```

---

### 3.5 GBufferE - PrecomputedShadowFactors

| 항목 | 내용 |
|------|------|
| **용도** | Lightmass에서 베이킹된 정적 그림자 (최대 4채널) |
| **조건** | `ALLOW_STATIC_LIGHTING` 활성화 시만 존재 |
| **값 범위** | [0, 1]⁴ - 각 채널이 별도 라이트의 그림자 |
| **기본값** | **`(1, 1, 1, 1)`** (그림자 없음 = 완전히 밝음) |
| **BRDF 사용** | 직접 사용 안 함 |
| **외부 사용** | Deferred Light Pass에서 그림자 팩터로 사용 |
| **NPR 필요성** | 🟡 **조건부** - 정적 라이팅 미사용 시 자유 재활용 |

**NPR 재활용 제안** (정적 라이팅 미사용 시):
```
- R: 추가 아웃라인 마스크
- G: 스타일 변형 ID
- B: 애니메이션 프레임 오프셋
- A: 특수 렌더링 플래그
```

---

### 3.6 GBufferF - WorldTangent + Anisotropy

| 항목 | 내용 |
|------|------|
| **조건** | `HAS_ANISOTROPY_MASK` 플래그 설정 시만 유효 |
| **WorldTangent (RGB)** | 비등방성 반사의 접선 방향 |
| **Anisotropy (A)** | 비등방성 강도 [-1, 1] |
| **기본값** | WorldTangent: `(1, 0, 0)` (X축), Anisotropy: **`0.0`** (등방성) |
| **BRDF 사용** | `SpecularGGX()` Anisotropic 버전, `D_GGXaniso()` |
| **외부 사용** | SSR: `ModifyGGXAnisotropicNormalRoughness()` |
|              | Reflection Environment: 동일 함수 |
| **NPR 필요성** | 🟡 **조건부** - Anisotropic 반사 미사용 시 자유 재활용 |

**NPR 재활용 제안**:
```
- RGB: 스타일라이즈드 노말 오프셋 또는 Flow Map
- A: 해칭/크로스해칭 방향 각도
```

---

### 3.7 GBufferVelocity - Motion Vector

| 항목 | 내용 |
|------|------|
| **조건** | `WRITES_VELOCITY_TO_GBUFFER` 활성화 시 |
| **용도** | 프레임 간 픽셀 이동량 (Motion Blur, TAA) |
| **포맷** | RG16F 또는 RGBA16F |
| **기본값** | **`(0, 0)`** (움직임 없음) |
| **BRDF 사용** | 사용 안 함 |
| **외부 사용** | Motion Blur, TSR/TAA |
| **NPR 필요성** | ⚠️ Motion Blur/TAA 사용 시 유지 필요 |

---

## 4. NPR 재활용 권장안

### 4.1 GBufferB 재활용 매핑

```
┌────────────────────────────────────────────────────────────────────┐
│                    GBufferB NPR 재활용 권장안                       │
├──────────┬──────────────────┬──────────────────────────────────────┤
│  채널    │    PBR 원래 용도  │           NPR 재활용 용도            │
├──────────┼──────────────────┼──────────────────────────────────────┤
│    R     │    Metallic      │  ToonSteps (톤 쉐이딩 단계 수)       │
│ (8bit)   │    [0, 1]        │  [0, 1] → 2~16 단계 매핑             │
├──────────┼──────────────────┼──────────────────────────────────────┤
│    G     │    Specular      │  HighlightSize (하이라이트 크기)     │
│ (8bit)   │    [0, 1]        │  [0, 1] → 작음~큼                    │
├──────────┼──────────────────┼──────────────────────────────────────┤
│    B     │    Roughness     │  ⚠️ 유지 권장 (SSR/Reflection 호환)  │
│ (8bit)   │    [0, 1]        │  또는 NPR 반사 블러 정도로 재해석    │
├──────────┼──────────────────┼──────────────────────────────────────┤
│    A     │ ShadingModelID   │  유지 (NPR 모델 ID 할당)             │
│ (8bit)   │ + Masks          │  + SelectiveOutputMask 유지          │
└──────────┴──────────────────┴──────────────────────────────────────┘
```

### 4.2 GBufferD NPR 전용 레이아웃 제안

```hlsl
// NPR_SHADING_MODEL 일 때의 CustomData 레이아웃
struct FNPRCustomData
{
    half RampTextureIndex;    // R: 램프 텍스처 배열 인덱스
    half OutlineColorOffset;  // G: 아웃라인 색상 Hue 오프셋
    half StylizedSSS;         // B: 스타일라이즈드 SSS 강도
    half SpecialEffectMask;   // A: 하프톤, 해칭 등 특수 효과 마스크
};
```

### 4.3 완전 자유 재활용 가능 채널

| 채널 | 조건 | NPR 용도 제안 |
|------|------|--------------|
| GBufferB.R (Metallic) | Eye/Hair 미사용 | 톤 단계, 림 강도 |
| GBufferB.G (Specular) | SSR 수정 | 하이라이트 파라미터 |
| GBufferD (CustomData) | 해당 모델 미사용 | NPR 전용 파라미터 |
| GBufferE | 정적 라이팅 미사용 | 추가 마스크/데이터 |
| GBufferF | Anisotropy 미사용 | Flow Map, 방향 데이터 |

### 4.4 주의 필요 채널

| 채널 | 외부 의존성 | 권장 대응 |
|------|------------|----------|
| GBufferB.B (Roughness) | SSR, Reflection, Water | 값 범위 유지, 의미만 재해석 |
| GBufferC.A | GI/AO 시스템 | 사용 여부에 따라 판단 |
| GBufferVelocity | Motion Blur/TAA | 기능 사용 시 유지 필수 |

---

## 5. 기타 버퍼 및 유니폼

### 5.1 Scene Textures

| 텍스처 | NPR 필요성 | 용도 |
|--------|-----------|------|
| SceneDepthTexture | ✅ 필수 | 아웃라인, 깊이 기반 효과 |
| CustomDepthTexture | ✅ 유용 | 선택적 아웃라인, 마스킹 |
| CustomStencilTexture | ✅ 유용 | 오브젝트별 스타일 분리 |
| ScreenSpaceAOTexture | ⚠️ 선택 | NPR AO 또는 제거 |
| SceneLightingChannels | ⚠️ 선택 | 선택적 라이팅 |

### 5.2 View Uniform Buffer 주요 멤버

```hlsl
// NPR에서 필수로 사용되는 View 멤버
View.TranslatedWorldToClip      // 변환 행렬
View.TranslatedWorldToView
View.TranslatedWorldCameraOrigin // 카메라 위치
View.ViewForward                // 뷰 방향
View.BufferSizeAndInvSize       // 해상도
View.ViewSizeAndInvSize
View.ScreenPositionScaleBias
View.StateFrameIndex            // 프레임 인덱스 (노이즈/지터)
View.PreExposure                // HDR 노출 (선택적)
```

### 5.3 Primitive Scene Data 주요 플래그

```cpp
// NPR에서 유용한 플래그들
PRIMITIVE_SCENE_DATA_FLAG_CAST_SHADOWS           // 그림자 캐스팅
PRIMITIVE_SCENE_DATA_FLAG_HAS_CAST_CONTACT_SHADOW // 컨택트 쉐도우
PRIMITIVE_SCENE_DATA_FLAG_LIGHTING_CHANNEL_0~2   // 라이팅 채널
PRIMITIVE_SCENE_DATA_FLAG_WRITES_CUSTOM_DEPTH_STENCIL // 커스텀 깊이/스텐실
PRIMITIVE_SCENE_DATA_FLAG_HOLDOUT                // 홀드아웃 (합성용)

// NPR에서 재활용 고려 가능
CustomStencilValueAndMask  // 오브젝트별 스텐실 값
CustomPrimitiveData[9]     // 커스텀 데이터 (float4 x 9)
```

---

## 6. 쉐이딩 모델 ID

### 6.1 기존 ID 할당

```cpp
#define SHADINGMODELID_UNLIT                0
#define SHADINGMODELID_DEFAULT_LIT          1
#define SHADINGMODELID_SUBSURFACE           2
#define SHADINGMODELID_PREINTEGRATED_SKIN   3
#define SHADINGMODELID_CLEAR_COAT           4
#define SHADINGMODELID_SUBSURFACE_PROFILE   5
#define SHADINGMODELID_TWOSIDED_FOLIAGE     6
#define SHADINGMODELID_HAIR                 7
#define SHADINGMODELID_CLOTH                8
#define SHADINGMODELID_EYE                  9
#define SHADINGMODELID_SINGLELAYERWATER     10
#define SHADINGMODELID_THIN_TRANSLUCENT     11
#define SHADINGMODELID_SUBSTRATE            12
#define SHADINGMODELID_NUM                  13
#define SHADINGMODELID_MASK                 0xF  // 4비트 (최대 16개)
```

### 6.2 NPR 쉐이딩 모델 ID 할당 제안

```cpp
// 미사용 ID를 NPR 전용으로 활용
#define SHADINGMODELID_NPR_TOON             13  // 톤 쉐이딩
#define SHADINGMODELID_NPR_HATCHING         14  // 해칭
#define SHADINGMODELID_NPR_WATERCOLOR       15  // 수채화 스타일

// 또는 기존 ID 재활용 (해당 기능 미사용 시)
// SUBSURFACE, PREINTEGRATED_SKIN, CLEAR_COAT 등을 NPR용으로 재정의
```

---

## 7. 참조 파일 목록

### 7.1 핵심 쉐이더 파일

| 파일 | 경로 | 내용 |
|------|------|------|
| DeferredShadingCommon.ush | Private/ | GBuffer 구조체, 인코딩/디코딩 |
| ShadingCommon.ush | Private/ | ShadingModelID 정의, F0 계산 |
| ShadingModels.ush | Private/ | BRDF 구현 (DefaultLitBxDF 등) |
| ShadingModelsMaterial.ush | Private/ | 머티리얼→GBuffer 매핑 |
| BRDF.ush | Private/ | GGX, Lambert 등 BRDF 함수 |
| SceneTexturesCommon.ush | Private/ | Scene Texture 접근 함수 |
| SceneData.ush | Private/ | Primitive/Instance 데이터 |

### 7.2 외부 의존 시스템 파일

| 파일 | 경로 | Roughness 의존 |
|------|------|---------------|
| SSRTReflections.usf | Private/SSRT/ | ✅ MaxRoughness, Anisotropic |
| ReflectionEnvironmentPixelShader.usf | Private/ | ✅ LOD, SpecularOcclusion |
| SingleLayerWaterComposite.usf | Private/ | ✅ EnvBRDF |
| SkyLightingDiffuseShared.ush | Private/ | ✅ Energy Conservation |
| MegaLightsShading.ush | Private/MegaLights/ | ✅ Roughness 복사 |

### 7.3 공유 헤더 파일

| 파일 | 경로 | 내용 |
|------|------|------|
| SceneDefinitions.h | Shared/ | Primitive 플래그 정의 |
| LightDefinitions.h | Shared/ | 라이트 타입 정의 |
| SubstrateDefinitions.h | Shared/ | Substrate 시스템 정의 |

---

## 부록 A: 파생값 계산 참조

```hlsl
// ComputeF0 - Specular Color 계산
half3 ComputeF0(half Specular, half3 BaseColor, half Metallic)
{
    return lerp(DielectricSpecularToF0(Specular).xxx, BaseColor, Metallic.xxx);
}

// DielectricSpecularToF0 - 비금속 F0
half DielectricSpecularToF0(half Specular)
{
    return half(0.08f * Specular);
}

// ComputeDiffuseAlbedo - Diffuse Color 계산
float3 ComputeDiffuseAlbedo(float3 BaseColor, float Metallic)
{
    return BaseColor - BaseColor * Metallic;
}

// NPR에서는 이 계산들을 단순화하거나 다른 방식으로 대체
// 예: DiffuseColor = BaseColor (Metallic 무시)
//     SpecularColor = 고정값 또는 BaseColor 기반
```

---

## 부록 B: SelectiveOutputMask 비트 정의

```cpp
#define HAS_ANISOTROPY_MASK      (1 << 4)  // GBufferF 유효
#define SKIP_PRECSHADOW_MASK     (1 << 5)  // PrecomputedShadow 스킵
#define ZERO_PRECSHADOW_MASK     (1 << 6)  // PrecomputedShadow = 0
#define IS_FIRST_PERSON_MASK     (1 << 6)  // 1인칭 플래그 (정적 라이팅 비활성 시)
#define SKIP_VELOCITY_MASK       (1 << 7)  // Velocity 출력 스킵
```

---

## 변경 이력

| 버전 | 날짜 | 변경 내용 |
|------|------|----------|
| 1.0 | 2026-04-11 | 초기 문서 작성 |
| 1.1 | 2026-04-11 | 각 채널별 기본값 추가, Quick Reference 테이블 추가 |

---

## 다음 단계

1. **NPR BRDF 구현**: `ShadingModels.ush`에 NPR BxDF 함수 추가
2. **GBuffer 재해석 코드 작성**: NPR 쉐이딩 모델용 Encode/Decode 함수
3. **외부 시스템 호환성 검토**: SSR, Reflection 등에서 Roughness 재해석 영향 테스트
4. **머티리얼 파라미터 매핑**: 엔진 머티리얼 에디터에서 NPR 파라미터 노출
