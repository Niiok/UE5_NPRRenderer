// NPR Light Rendering
// Based on Engine's LightRendering.cpp - provides identical per-light rendering with NPR BRDF
// Uses DeferredLightUniformWrapper to avoid linker errors

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderParameterMacros.h"
#include "RenderGraph.h"
#include "SceneRendering.h"
#include "ShadowRendering.h"  // For FProjectedShadowInfo

#include "DeferredLightUniformWrapper.h"
#include "ForwardLightUniformWrapper.h"
#include "ReflectionUniformWrapper.h"
#include "ReflectionCaptureWrapper.h"
#include "NPRLightRenderingUtils.h"

class FLightSceneInfo;

//=============================================================================
// Light Source Shape Enum (matching engine's LightRendering.cpp)
//=============================================================================

enum class ENPRLightSourceShape : uint8
{
	Directional = 0,
	Capsule = 1,  // Point/Spot lights
	Rect = 2,
	MAX
};

//=============================================================================
// Shadow Type Enum — compile-time permutation replaces runtime bUsePointLightShadow
// Mutually exclusive: Point params and CSM params never coexist in GPU cbuffer
//=============================================================================

enum class ENPRShadowType : uint8
{
	None = 0,         // No shadow
	PointCubemap = 1, // Point light cubemap PCF (ShadowDepthCubeTexture)
	Shadow2D = 2,     // 2D ManualPCF — Spot (single matrix) or Directional (CSM cascades)
	MAX
};

//=============================================================================
// FNPRDeferredLightPS - NPR version of FDeferredLightPS
// Same parameter structure, different BRDF
// Uses FDeferredLightUniformWrapper (identical layout to engine's FDeferredLightUniformStruct)
//=============================================================================

class FNPRDeferredLightPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FNPRDeferredLightPS);
	SHADER_USE_PARAMETER_STRUCT(FNPRDeferredLightPS, FGlobalShader);

	// Permutation dimensions
	class FSourceShapeDim : SHADER_PERMUTATION_ENUM_CLASS("LIGHT_SOURCE_SHAPE", ENPRLightSourceShape);
	class FSourceTextureDim : SHADER_PERMUTATION_BOOL("USE_SOURCE_TEXTURE");
	class FLightingChannelsDim : SHADER_PERMUTATION_BOOL("USE_LIGHTING_CHANNELS");
	// Shadow quality permutation: 1=NoFiltering, 2=1x1 PCF, 3=3x3 PCF, 4=5x5 PCF
	class FShadowQualityDim : SHADER_PERMUTATION_RANGE_INT("SHADOW_QUALITY", 1, 4);
	// Shadow type permutation: None/PointCubemap/Shadow2D — replaces runtime bUsePointLightShadow
	class FShadowTypeDim : SHADER_PERMUTATION_ENUM_CLASS("NPR_SHADOW_TYPE", ENPRShadowType);

	using FPermutationDomain = TShaderPermutationDomain<
		FSourceShapeDim,
		FSourceTextureDim,
		FLightingChannelsDim,
		FShadowQualityDim,
		FShadowTypeDim
	>;

	// Shader parameters - uses FDeferredLightUniformWrapper (binds as "DeferredLightUniforms" in shader)
	// Order matches engine expectations - additional parameters at the end
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FDeferredLightUniformWrapper, DeferredLightUniforms)
		
		// Lighting channels texture
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, LightingChannelsTexture)
		
		// IES Profile (currently unused - USE_IES_PROFILE=0)
		// SHADER_PARAMETER_TEXTURE(Texture2D, IESTexture)
		// SHADER_PARAMETER_SAMPLER(SamplerState, IESSampler)
		
		// === Additional parameters below (shadow + NPR) ===
		// Active parameters depend on NPR_SHADOW_TYPE permutation:
		//   NPR_SHADOW_TYPE=0 (None):         no shadow params bound
		//   NPR_SHADOW_TYPE=1 (PointCubemap):  ShadowDepthCubeTexture, ShadowViewProjectionMatrices, PointLightDepthBias
		//   NPR_SHADOW_TYPE=2 (Shadow2D):      ShadowDepthTexture, CSM* params
		// Unused params are stripped by shader compiler → no GPU cbuffer waste
		
		// Point light cubemap shadow (NPR_SHADOW_TYPE == 1 only)
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, ShadowDepthCubeTexture)
		SHADER_PARAMETER_SAMPLER(SamplerComparisonState, ShadowDepthCubeTextureSampler)
		SHADER_PARAMETER_ARRAY(FMatrix44f, ShadowViewProjectionMatrices, [6])
		SHADER_PARAMETER(float, InvShadowmapResolution)
		// .x:DepthBias, .y:SlopeDepthBias, .z:MaxSlopeDepthBias
		SHADER_PARAMETER(FVector3f, PointLightDepthBias)
		
		// Directional light CSM shadow - Multi-cascade support
		// Uses ScreenToShadowMatrix approach (same as engine's shadow projection shader)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ShadowDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ShadowDepthTextureSampler)
		
		// CSM Multi-Cascade Parameters (up to 4 cascades)
		// NumCSMCascades: Number of active CSM cascades (0 = no CSM shadow)
		SHADER_PARAMETER(uint32, NumCSMCascades)
		// CSMScreenToShadowMatrix: transforms (ScreenX, ScreenY, ViewZ, 1) to shadow space for each cascade
		// Note: Using individual matrices instead of array for reliable shader binding
		SHADER_PARAMETER(FMatrix44f, CSMScreenToShadowMatrix0)
		SHADER_PARAMETER(FMatrix44f, CSMScreenToShadowMatrix1)
		SHADER_PARAMETER(FMatrix44f, CSMScreenToShadowMatrix2)
		SHADER_PARAMETER(FMatrix44f, CSMScreenToShadowMatrix3)
		// CSMSoftTransitionScale: 1.0f / TransitionSize for each cascade (PCF softness), packed in float4
		SHADER_PARAMETER(FVector4f, CSMSoftTransitionScale)
		// CSMCascadeEndDepth: View-space depth at which each cascade ends (SplitFar), packed in float4
		SHADER_PARAMETER(FVector4f, CSMCascadeEndDepth)
#if NPR_USE_RECEIVER_BIAS
		// CSMReceiverBias: ReceiverDepthBias from r.Shadow.CSMReceiverBias CVar (same for all cascades)
		SHADER_PARAMETER(float, CSMReceiverBias)
#endif
#if NPR_CSM_BLEND_CASCADES
		// CSMFadeRegionFraction: Fraction of cascade length for fade region (same for all cascades)
		// FadeLength = CascadeLength × Fraction, same as engine's CascadeTransitionFraction
		SHADER_PARAMETER(float, CSMFadeRegionFraction)
#endif
		// Common CSM parameters (shared across cascades)
		// ShadowBufferSize: xy = resolution, zw = 1/resolution (CSM atlas size)
		SHADER_PARAMETER(FVector4f, ShadowBufferSize)
		
		// Shadow post-processing parameters (same as engine's ShadowProjectionPixelShader.usf)
		// ShadowSharpen: Controls shadow edge sharpness (r.Shadow.Sharpen CVar, default 1.0)
		SHADER_PARAMETER(float, ShadowSharpen)
		// ShadowFadeFraction: Shadow fade multiplier (default 1.0, from FProjectedShadowInfo)
		SHADER_PARAMETER(float, ShadowFadeFraction)
		
		// NPR params are derived from GBuffer in the shader (no CVars): Metallic->shadow threshold,
		// Specular->highlight threshold, Roughness->shadow softness/highlight size, BaseColor->diffuse.

		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		
		// Source texture only valid for Rect lights (engine identical)
		if (PermutationVector.Get<FSourceShapeDim>() != ENPRLightSourceShape::Rect 
			&& PermutationVector.Get<FSourceTextureDim>())
		{
			return false;
		}
		
		// Directional lights don't use cubemap shadows (engine identical)
		if (PermutationVector.Get<FSourceShapeDim>() == ENPRLightSourceShape::Directional
			&& PermutationVector.Get<FShadowTypeDim>() == ENPRShadowType::PointCubemap)
		{
			return false;
		}
		
		// Rect lights don't use cubemap shadows (use 2D shadow maps like spot lights)
		if (PermutationVector.Get<FSourceShapeDim>() == ENPRLightSourceShape::Rect
			&& PermutationVector.Get<FShadowTypeDim>() == ENPRShadowType::PointCubemap)
		{
			return false;
		}
		
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	
	// Don't precache rarely used permutations to reduce initial shader compilation
	// (engine uses this pattern for FVisualizeCullingDim and other debug features)
	static EShaderPermutationPrecacheRequest ShouldPrecachePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		
		// Don't precache lighting channels variants - most scenes don't use them
		// They will be compiled on-demand when actually needed
		if (PermutationVector.Get<FLightingChannelsDim>())
		{
			return EShaderPermutationPrecacheRequest::NotPrecached;
		}
		
		// Don't precache high shadow quality variants (3, 4) - quality 2 is usually sufficient
		if (PermutationVector.Get<FShadowQualityDim>() > 2)
		{
			return EShaderPermutationPrecacheRequest::NotPrecached;
		}
		
		return FGlobalShader::ShouldPrecachePermutation(Parameters);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		
		// Required defines (matching engine's DeferredLightPixelShaders.usf)
		OutEnvironment.SetDefine(TEXT("SUPPORT_CONTACT_SHADOWS"), 0);
		OutEnvironment.SetDefine(TEXT("NON_DIRECTIONAL_DIRECT_LIGHTING"), 0);
		OutEnvironment.SetDefine(TEXT("USE_HAIR_LIGHTING"), 0);
		OutEnvironment.SetDefine(TEXT("USE_ATMOSPHERE_TRANSMITTANCE"), 0);
		OutEnvironment.SetDefine(TEXT("USE_CLOUD_TRANSMITTANCE"), 0);
		OutEnvironment.SetDefine(TEXT("USE_VIRTUAL_SHADOW_MAP_MASK"), 0);
		OutEnvironment.SetDefine(TEXT("USE_ADAPTIVE_VOLUMETRIC_SHADOW_MAP"), 0);
		OutEnvironment.SetDefine(TEXT("SHADING_PATH_DEFERRED"), 1);
		OutEnvironment.SetDefine(TEXT("ALLOW_STATIC_LIGHTING"), 1);
		
		// Features not yet implemented in NPR — hardcoded off (no permutation dimension)
		OutEnvironment.SetDefine(TEXT("USE_IES_PROFILE"), 0);
		OutEnvironment.SetDefine(TEXT("USE_TRANSMISSION"), 0);
		
		// Note: LIGHT_SOURCE_SHAPE, USE_SOURCE_TEXTURE, USE_LIGHTING_CHANNELS,
		//       SHADOW_QUALITY, NPR_SHADOW_TYPE are set by permutation dimensions
		
		// CSM cascade blending - costs ~4 extra Gather ops in blend regions
		// Set to 0 to disable for performance (visible cascade seams)
		OutEnvironment.SetDefine(TEXT("NPR_CSM_BLEND_CASCADES"), 1);
		
		// ReceiverBias modulation: NoL-based TransitionScale adjustment
		// Engine ignores this (SHADING_PATH_DEFERRED=0 in ShadowProjectionPixelShader)
		// Set to 1 to enable NPR-specific behavior, 0 to match engine (disabled)
		OutEnvironment.SetDefine(TEXT("NPR_USE_RECEIVER_BIAS"), 0);
	}

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		// Source texture only valid for Rect lights
		if (PermutationVector.Get<FSourceShapeDim>() != ENPRLightSourceShape::Rect)
		{
			PermutationVector.Set<FSourceTextureDim>(false);
		}
		
		// Directional + PointCubemap is invalid (directional lights don't use cubemaps)
		if (PermutationVector.Get<FSourceShapeDim>() == ENPRLightSourceShape::Directional
			&& PermutationVector.Get<FShadowTypeDim>() == ENPRShadowType::PointCubemap)
		{
			PermutationVector.Set<FShadowTypeDim>(ENPRShadowType::None);
		}
		
		// No shadow → SHADOW_QUALITY is meaningless, force to 1 to avoid duplicate variants
		if (PermutationVector.Get<FShadowTypeDim>() == ENPRShadowType::None)
		{
			PermutationVector.Set<FShadowQualityDim>(1);
		}
		
		return PermutationVector;
	}
	
	// Override job priority to match engine's FDeferredLightPS behavior
	// Deferred light shaders can be slow to compile, so prioritize them
	static EShaderCompileJobPriority GetOverrideJobPriority()
	{
		return EShaderCompileJobPriority::ExtraHigh;
	}
};

//=============================================================================
// Render Parameters Structure - Uses our replicated vertex shader parameters
//=============================================================================

BEGIN_SHADER_PARAMETER_STRUCT(FNPRRenderLightParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FNPRDeferredLightVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FNPRDeferredLightPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()

//=============================================================================
// FNPRDeferredShadowPS - Applies NPR shadow color from GBuffer.CustomData.rgb
// Engine's SkyLight pass handles actual sky lighting.
// This pass only adds shadow color based on accumulated ShadowMask.
//=============================================================================

class FNPRDeferredShadowPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FNPRDeferredShadowPS);
	SHADER_USE_PARAMETER_STRUCT(FNPRDeferredShadowPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		
		// Shadow mask texture (accumulated from light passes with Max blend)
		// 0 = fully in shadow (no lights), 1 = fully lit (any light illuminates)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ShadowMaskTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ShadowMaskSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// Pass parameters for NPR Deferred Shadow (VS + PS combined)
BEGIN_SHADER_PARAMETER_STRUCT(FNPRDeferredShadowPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FNPRDeferredLightVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FNPRDeferredShadowPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()

//=============================================================================
// FNPRSkyLightPS - NPR-dedicated sky light + reflection
// Self-sources sky data (SH irradiance + convolved cubemap) from the SkyLight proxy
// since the engine's SkyLighting show flag is off (engine GPU sky data is zeroed).
// Diffuse: NPR toon ambient (NPRSkyDiffuse). Specular: sky cubemap IBL (metallic reflection).
//=============================================================================

class FNPRSkyLightPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FNPRSkyLightPS);
	SHADER_USE_PARAMETER_STRUCT(FNPRSkyLightPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)

		// Sky data: real-time capture (Scene GPU targets) or baked (SkyLight proxy).
		// SH coefficients packed identically to View.SkyIrradianceEnvironmentMap (8 x float4).
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, NPRSkyIrradiance)
		SHADER_PARAMETER(FVector3f, NPRSkyColor)
		SHADER_PARAMETER(float, NPRSkyMaxMip)
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, NPRSkyCubemap)
		SHADER_PARAMETER_SAMPLER(SamplerState, NPRSkyCubemapSampler)

		// SSR (screen-space scene reflection via engine RayCast). HZB + lit SceneColor copy.
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NPRHZBTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, NPRHZBSampler)
		SHADER_PARAMETER(FVector4f, NPRHZBUvFactorAndInvFactor)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NPRSceneColorCopy)
		SHADER_PARAMETER_SAMPLER(SamplerState, NPRSceneColorSampler)
		SHADER_PARAMETER(int32, NPRSSREnabled)

		// Reflection environment (captures + sky). Bound via wrappers since the engine UB metadata
		// isn't RENDERER_API-exported. ForwardLightStruct carries the per-tile reflection-capture
		// culling grid; ReflectionStruct the cubemap array + sky cubemap; ReflectionCaptureSM5 the
		// per-capture transforms. Consumed by CompositeReflectionCapturesAndSkylightTWS in the shader.
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FForwardLightUniformWrapper, ForwardLightStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FReflectionUniformWrapper, ReflectionStruct)
		SHADER_PARAMETER_STRUCT_REF(FReflectionCaptureWrapper, ReflectionCaptureSM5)

		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		// CompositeReflectionCapturesAndSkylightTWS: blended per-tile capture grid, box + sphere shapes.
		OutEnvironment.SetDefine(TEXT("REFLECTION_COMPOSITE_USE_BLENDED_REFLECTION_CAPTURES"), 1);
		OutEnvironment.SetDefine(TEXT("REFLECTION_COMPOSITE_HAS_BOX_CAPTURES"), 1);
		OutEnvironment.SetDefine(TEXT("REFLECTION_COMPOSITE_HAS_SPHERE_CAPTURES"), 1);
		// Forward light grid layout (engine values from FForwardLightingParameters). Required: without it
		// NUM_CULLED_LIGHTS_GRID_STRIDE defaults to 0 and the capture grid offset math collapses to 0 counts.
		OutEnvironment.SetDefine(TEXT("NUM_CULLED_LIGHTS_GRID_STRIDE"), 2);
		OutEnvironment.SetDefine(TEXT("NUM_CULLED_GRID_PRIMITIVE_TYPES"), 2);
		// Sky reflection block is gated by ENABLE_SKY_LIGHT; without it the sky fallback compiles out.
		OutEnvironment.SetDefine(TEXT("ENABLE_SKY_LIGHT"), 1);
		// Static-lighting path on (engine default): enables ComputeMixingWeight, which normalizes capture
		// reflections to GBuffer.IndirectIrradiance (the precomputed indirect from the lightmap/ILC). This is
		// what keeps metal lit without a sky light. The shader passes the real IndirectIrradiance (not 0).
		OutEnvironment.SetDefine(TEXT("ALLOW_STATIC_LIGHTING"), 1);
	}
};

// Pass parameters for NPR Sky Light (VS + PS combined)
BEGIN_SHADER_PARAMETER_STRUCT(FNPRSkyLightPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FNPRDeferredLightVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FNPRSkyLightPS::FParameters, PS)
END_SHADER_PARAMETER_STRUCT()

//=============================================================================
// Helper Functions for NPR Light Rendering
//=============================================================================

namespace NPRLightRendering
{
	// Render a single light using NPR BRDF
	// Outputs to SceneColor (additive) and ShadowMaskTexture (Min blend)
	void RenderNPRLight(
		FRDGBuilder& GraphBuilder,
		const FScene* Scene,
		const FViewInfo& View,
		const FMinimalSceneTextures& SceneTextures,
		const FLightSceneInfo* LightSceneInfo,
		FRDGTextureRef LightingChannelsTexture,
		FRDGTextureRef ShadowMaskTexture);
	
	// Apply shadow tint from GBuffer.CustomData.rgb
	// Reads ShadowMaskTexture and adds tint color to shadowed areas
	void RenderNPRShadow(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FMinimalSceneTextures& SceneTextures,
		FRDGTextureRef ShadowMaskTexture);

	// NPR sky light + reflection (ambient diffuse + metallic capture/sky/SSR reflection), additive to
	// SceneColor. Always runs (captures/SSR are sky-light independent); the SKY terms are gated by:
	//   bSkyLightingEnabled    - incoming SkyLighting flag -> sky cubemap (specular + diffuse source)
	//   bDiffuseAmbientEnabled - incoming GlobalIllumination flag -> sky SH diffuse ambient (indirect)
	void RenderNPRSkyLight(
		FRDGBuilder& GraphBuilder,
		const FScene* Scene,
		const FViewInfo& View,
		const FMinimalSceneTextures& SceneTextures,
		bool bSkyLightingEnabled,
		bool bDiffuseAmbientEnabled);
}
