# NPR Decimal Encoding - Material Setup Guide

## 개요

NPR 렌더러는 **소수점 기반 인코딩**을 사용합니다.

```
┌──────────────────────────────────────────────────────────────────┐
│  ★ 아티스트 친화적: "소수점 둘째자리부터 NPR"                    │
│                                                                  │
│     0.53 = PBR 0.5 + NPR 0.3                                    │
│     ───────────────   ───────                                   │
│     소수점 첫째자리   둘째자리 이하                              │
│                                                                  │
│  "Metallic 0.5에 NPR 0.3 넣고 싶으면 → 0.53 입력!"              │
└──────────────────────────────────────────────────────────────────┘
```

## 인코딩 규칙

| 입력 예시 | PBR | NPR | 결과 |
|----------|-----|-----|------|
| 0.53 | 0.5 | 0.3 | Metallic=0.5, Highlight=0.3 |
| 0.72 | 0.7 | 0.2 | Specular=0.7, Shadow=0.2 |
| 0.35 | 0.3 | 0.5 | Roughness=0.3, Softness=0.5 |
| 0.00 | 0.0 | 0.0 | 최솟값 |
| 0.99 | 0.9 | 0.9 | 최댓값 |

## 정밀도

| 채널 | 레벨 수 | 설명 |
|------|--------|------|
| **PBR** | 10 | 0.0, 0.1, 0.2, ... 0.9 |
| **NPR** | ~25 (8비트) | GBuffer 정밀도에 자동 적응 |
| **NPR** | ~6553 (16비트) | 고정밀도 GBuffer 시 |

---

## 채널별 NPR 파라미터

| GBuffer 채널 | NPR 파라미터 | 효과 (0~1) |
|-------------|-------------|------------|
| **Metallic** | Highlight Intensity | 0=하이라이트 없음, 1=전체 표면 |
| **Specular** | Shadow Threshold | 0=그림자 없음, 0.5=터미네이터, 1=전체 그림자 |
| **Roughness** | Edge Softness | 0=선명, 1=부드러움 |

---

## Material Function 사용법

### 자동 생성된 MF_NPR_Encode

에디터 시작 시 `/NPRRenderer/MaterialFunctions/MF_NPR_Encode` 자동 생성됩니다.

### 입력 (6개, 모두 0~1)

| Input | 설명 | 기본값 |
|-------|------|--------|
| `Metallic` | PBR Metallic | 0.0 |
| `Specular` | PBR Specular | 0.5 |
| `Roughness` | PBR Roughness | 0.5 |
| `NPR_HighlightIntensity` | 하이라이트 영역 | 0.5 |
| `NPR_ShadowThreshold` | 그림자 임계값 | 0.5 |
| `NPR_EdgeSoftness` | 경계 부드러움 | 0.3 |

---

## 수동 입력 (Material Function 없이)

Material Function 없이 직접 값을 입력할 수 있습니다:

```
Metallic에 직접 입력:
  - 0.53 → PBR Metallic=0.5, NPR Highlight=0.3

Specular에 직접 입력:
  - 0.72 → PBR Specular=0.7, NPR Shadow=0.2

Roughness에 직접 입력:
  - 0.35 → PBR Roughness=0.3, NPR Softness=0.5
```

---

## Custom Node (수동 생성 시)

```hlsl
// Settings: Output Type = CMOT Float 3
// Inputs: Metallic, Specular, Roughness, 
//         NPR_HighlightIntensity, NPR_ShadowThreshold, NPR_EdgeSoftness

#define NPR_PBR_MAX_LEVEL 9.0
#define NPR_PBR_DIVISOR 10.0

float EncodeChannel(float PBR, float NPR)
{
    float pbrPart = floor(PBR * NPR_PBR_MAX_LEVEL + 0.5) / NPR_PBR_DIVISOR;
    float nprPart = saturate(NPR) / NPR_PBR_DIVISOR;
    return pbrPart + nprPart;
}

return float3(
    EncodeChannel(Metallic, NPR_HighlightIntensity),
    EncodeChannel(Specular, NPR_ShadowThreshold),
    EncodeChannel(Roughness, NPR_EdgeSoftness)
);
```

---

## 추천 값

```
애니메이션 스타일:
  Metallic  = 0.05  (PBR=0.0, NPR Highlight=0.5)
  Specular  = 0.55  (PBR=0.5, NPR Shadow=0.5)
  Roughness = 0.51  (PBR=0.5, NPR Softness=0.1)

부드러운 일러스트:
  Metallic  = 0.03  (PBR=0.0, NPR Highlight=0.3)
  Specular  = 0.54  (PBR=0.5, NPR Shadow=0.4)
  Roughness = 0.54  (PBR=0.5, NPR Softness=0.4)
```

---

## 관련 파일

| 파일 | 설명 |
|------|------|
| `Shaders/Public/NPREncoding.ush` | 인코딩/디코딩 함수 |
| `Shaders/Private/NPRDeferredLightPixelShaders.usf` | NPR 라이팅 |
| `Source/.../NPRMaterialFunctionFactory.cpp` | MF 자동 생성 |
