// NPR Light Rendering Implementation
// Based on Engine's LightRendering.cpp RenderLight function
// Uses DeferredLightUniformWrapper to avoid linker errors

#include "NPRLightRendering.h"
#include "DeferredLightUniformWrapper.h"
#include "NPRLightRenderingUtils.h"
#include "NPRRendererModule.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "SceneRenderTargetParameters.h"
#include "PixelShaderUtils.h"
#include "RenderGraphUtils.h"
#include "GenerateMips.h"  // For FGenerateMips::Execute (SSR scene-color mip chain)
#include "PipelineStateCache.h"
#include "CommonRenderResources.h"
#include "LightSceneInfo.h"
#include "HAL/IConsoleManager.h"
#include "PostProcess/SceneFilterRendering.h"  // For DrawRectangle
#include "SystemTextures.h"  // For FRDGSystemTextures
#include "PSOPrecache.h"  // For FRegisterGlobalPSOCollectorFunction
#include "PSOPrecacheValidation.h"  // For PSO_PRECACHING_VALIDATE
#include "ReflectionEnvironment.h"  // For SetupSkyIrradianceEnvironmentMapConstantsFromSkyIrradiance + FReflectionUniformParameters
#include "ReflectionUniformWrapper.h"  // ReflectionStruct binding wrapper
#include "ReflectionCaptureWrapper.h"  // ReflectionCaptureSM5 binding wrapper
#include "SceneProxies/SkyLightSceneProxy.h"  // For FSkyLightSceneProxy
#include "Components/SkyLightComponent.h"  // For USkyLightComponent (GetLightColor/Intensity)

// Declared before render-time validation paths that look up this collector.
static const TCHAR* NPRLightPSOCollectorName = TEXT("NPRLightPSOCollector");

//=============================================================================
// Shadow Quality Helper (same logic as engine's GetShadowQuality)
//=============================================================================

static uint32 GetNPRShadowQuality()
{
	static IConsoleVariable* CVarShadowQuality = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShadowQuality"));
	
	int32 Quality = CVarShadowQuality ? CVarShadowQuality->GetInt() : 3;
	
	// Clamp to NPR's supported range: 1-4 (matching SHADOW_QUALITY permutation)
	// Engine uses 0-5, but 0 means off (no shadows) and 5 is same as 4 for PCF
	return FMath::Clamp(Quality, 1, 4);
}

//=============================================================================
// Shader Implementation
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FNPRDeferredLightPS, "/Plugin/NPRRenderer/Private/NPRLightPixelShader.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FNPRDeferredShadowPS, "/Plugin/NPRRenderer/Private/NPRShadowPixelShader.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FNPRSkyLightPS, "/Plugin/NPRRenderer/Private/NPRSkyLightPixelShader.usf", "MainPS", SF_Pixel);

//=============================================================================
// Setup PSO for Light Rendering (same as engine's SetupLightGraphicsPSOState)
//=============================================================================

static uint32 SetupNPRLightGraphicsPSOState(
	bool bIsDirectional,
	bool bCameraInsideLightGeometry,
	bool bReverseCulling,
	FGraphicsPipelineStateInitializer& GraphicsPSOInit,
	EShaderPlatform ShaderPlatform)
{
	// MRT Blend States:
	// RT0 (SceneColor): Additive blend (one + one)
	// RT1 (ShadowMask): Max blend - any light illuminating removes shadow tint
	//   If any light has shadowMask close to 1.0, the pixel is lit and tint fades
	GraphicsPSOInit.BlendState = TStaticBlendState<
		CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One,  // RT0: Scene Color (additive)
		CW_RED, BO_Max, BF_One, BF_One, BO_Max, BF_One, BF_One    // RT1: Shadow Mask (max blend - brightest wins)
	>::GetRHI();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	
	if (bIsDirectional)
	{
		// Fullscreen pass
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		return 0;
	}
	else
	{
		// Stencil geometry pass
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
		
		if (bCameraInsideLightGeometry)
		{
			// Render backfaces (same as engine: InternalSetBoundingGeometryRasterizerState + CF_Always)
			GraphicsPSOInit.RasterizerState = bReverseCulling 
				? TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI()
				: TStaticRasterizerState<FM_Solid, CM_CCW>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		}
		else
		{
			// Render frontfaces (same as engine: InternalSetBoundingGeometryRasterizerState + CF_DepthNearOrEqual)
			GraphicsPSOInit.RasterizerState = bReverseCulling 
				? TStaticRasterizerState<FM_Solid, CM_CCW>::GetRHI()
				: TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI();
		}
		return 0;
	}
}

//=============================================================================
// Internal Render Light - Same structure as engine's InternalRenderLight
// Uses our replicated vertex shader and geometry functions
//=============================================================================

template<typename TShaderType, typename TParametersType>
static void InternalRenderNPRLight(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLightSceneInfo* LightSceneInfo,
	TShaderType& PixelShader,
	TParametersType* PassParameters,
	const TCHAR* ShaderName)
{
	const FLightSceneProxy* RESTRICT LightProxy = LightSceneInfo->Proxy;
	const FSphere LightBounds = LightProxy->GetBoundingSphere();
	const ELightComponentType LightType = (ELightComponentType)LightProxy->GetLightType();
	const bool bIsDirectional = (LightType == LightType_Directional);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("%s: %s", ShaderName, *LightProxy->GetOwnerNameOrLabel()),
		PassParameters,
		ERDGPassFlags::Raster,
		[Scene, &View, PixelShader, LightSceneInfo, PassParameters, LightBounds, bIsDirectional](FRDGAsyncTask, FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

		const bool bCameraInsideLightGeometry = bIsDirectional ? false :
			((FVector(View.ViewMatrices.GetViewOrigin()) - LightBounds.Center).SizeSquared() < FMath::Square(LightBounds.W * 1.05f + View.NearClippingDistance * 2.0f)
			|| !View.IsPerspectiveProjection());

		SetupNPRLightGraphicsPSOState(bIsDirectional, bCameraInsideLightGeometry, View.bReverseCulling, GraphicsPSOInit, View.GetShaderPlatform());

		// Set viewport
		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

		if (bIsDirectional)
		{
			// Directional light - fullscreen pass using our replicated vertex shader
			FNPRDeferredLightVS::FPermutationDomain PermutationVectorVS;
			PermutationVectorVS.Set<FNPRDeferredLightVS::FRadialLight>(false);
			TShaderMapRef<FNPRDeferredLightVS> VertexShader(View.ShaderMap, PermutationVectorVS);

			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

#if PSO_PRECACHING_VALIDATE
			if (PSOCollectorStats::IsFullPrecachingValidationEnabled())
			{
				static const int32 GlobalPSOCollectorIndex = FGlobalPSOCollectorManager::GetIndex(NPRLightPSOCollectorName);
				PSOCollectorStats::CheckGlobalGraphicsPipelineStateInCache(GraphicsPSOInit, GlobalPSOCollectorIndex);
			}
#endif

			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

			// Draw fullscreen quad (DrawRectangle signature from SceneFilterRendering.h)
			DrawRectangle(
				RHICmdList,
				0, 0,
				View.ViewRect.Width(), View.ViewRect.Height(),
				View.ViewRect.Min.X, View.ViewRect.Min.Y,
				View.ViewRect.Width(), View.ViewRect.Height(),
				FIntPoint(View.ViewRect.Width(), View.ViewRect.Height()),
				View.GetSceneTexturesConfig().Extent,
				VertexShader,
				EDRF_UseTriangleOptimization);
		}
		else
		{
			// Radial light - stencil geometry pass using our replicated vertex shader
			FNPRDeferredLightVS::FPermutationDomain PermutationVectorVS;
			PermutationVectorVS.Set<FNPRDeferredLightVS::FRadialLight>(true);
			TShaderMapRef<FNPRDeferredLightVS> VertexShader(View.ShaderMap, PermutationVectorVS);

			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

#if PSO_PRECACHING_VALIDATE
			if (PSOCollectorStats::IsFullPrecachingValidationEnabled())
			{
				static const int32 GlobalPSOCollectorIndex = FGlobalPSOCollectorManager::GetIndex(NPRLightPSOCollectorName);
				PSOCollectorStats::CheckGlobalGraphicsPipelineStateInCache(GraphicsPSOInit, GlobalPSOCollectorIndex);
			}
#endif

			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

			// Draw stenciling geometry using our replicated geometry (sphere or cone)
			if (LightSceneInfo->Proxy->GetLightType() == LightType_Point ||
				LightSceneInfo->Proxy->GetLightType() == LightType_Rect)
			{
				NPRStencilingGeometry::DrawSphere(RHICmdList);
			}
			else // Spot
			{
				NPRStencilingGeometry::DrawCone(RHICmdList);
			}
		}
	});
}

//=============================================================================
// NPRLightRendering Namespace Implementation
//=============================================================================

namespace NPRLightRendering
{

void RenderNPRLight(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	const FLightSceneInfo* LightSceneInfo,
	FRDGTextureRef LightingChannelsTexture,
	FRDGTextureRef ShadowMaskTexture)
{
	// Null check only — ShouldRenderLight already called by CollectVisibleLights
	if (!LightSceneInfo)
	{
		return;
	}

	const FLightSceneProxy* RESTRICT LightProxy = LightSceneInfo->Proxy;
	// IES texture and Transmission not yet implemented — no permutation dimension
	// const bool bUseIESTexture = View.Family->EngineShowFlags.TexturedLightProfiles && (LightProxy->GetIESTextureResource() != 0);
	// const bool bTransmission = LightProxy->Transmission();
	const ELightComponentType LightType = (ELightComponentType)LightProxy->GetLightType();
	const bool bIsRadial = LightType != LightType_Directional;

	// Get shadow info for all lights (Point/Spot/Directional)
	const FProjectedShadowInfo* ShadowInfo = nullptr;  // First cascade (for point/spot lights or fallback)
	TArray<const FProjectedShadowInfo*, TInlineAllocator<4>> CSMCascades;  // All CSM cascades for directional lights
	{
		ISceneRenderer* SceneRendererInterface = View.Family->GetSceneRenderer();
		if (SceneRendererInterface)
		{
			FSceneRenderer* SceneRenderer = static_cast<FSceneRenderer*>(SceneRendererInterface);
			if (LightSceneInfo->Id < SceneRenderer->VisibleLightInfos.Num())
			{
				const FVisibleLightInfo& VisibleLightInfo = SceneRenderer->VisibleLightInfos[LightSceneInfo->Id];
				
				// For directional lights, get all CSM cascades
				if (!bIsRadial)
				{
					NPRLightRenderingUtils::GetAllWholeSceneDirectionalShadows(VisibleLightInfo, CSMCascades);
					if (CSMCascades.Num() > 0)
					{
						ShadowInfo = CSMCascades[0];  // First cascade for shared resources
					}
				}
				else
				{
					// For point/spot lights, get first shadow
					ShadowInfo = NPRLightRenderingUtils::GetFirstWholeSceneShadowForLight(VisibleLightInfo);
				}
			}
		}
	}

	// Allocate pass parameters (same as engine)
	FNPRRenderLightParameters* PassParameters = GraphBuilder.AllocParameters<FNPRRenderLightParameters>();

	// PS Parameters
	PassParameters->PS.View = View.ViewUniformBuffer;
	PassParameters->PS.SceneTextures = SceneTextures.UniformBuffer;
	
	// Create DeferredLightUniforms uniform buffer using DeferredLightUniformWrapper
	PassParameters->PS.DeferredLightUniforms = CreateDeferredLightUniformBuffer(GraphBuilder, View, *LightSceneInfo);
	
	// Point light cubemap shadow — only for bOnePassPointLightShadow (not Spot)
	// Spot lights use 2D shadow maps bound via CSM infrastructure below
	{
		const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
		const bool bIsPointCubemapShadow = ShadowInfo && bIsRadial && ShadowInfo->bOnePassPointLightShadow;
		
		FRDGTexture* ShadowDepthTextureValue = bIsPointCubemapShadow
			? GraphBuilder.RegisterExternalTexture(ShadowInfo->RenderTargets.DepthTarget)
			: SystemTextures.BlackDepthCube;
		
		PassParameters->PS.ShadowDepthCubeTexture = ShadowDepthTextureValue;
		PassParameters->PS.ShadowDepthCubeTextureSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp, 0, 0, 0, SCF_Less>::GetRHI();
		
		if (bIsPointCubemapShadow)
		{
			for (int32 i = 0; i < ShadowInfo->OnePassShadowViewProjectionMatrices.Num() && i < 6; i++)
			{
				PassParameters->PS.ShadowViewProjectionMatrices[i] = FMatrix44f(ShadowInfo->OnePassShadowViewProjectionMatrices[i]);
			}
			PassParameters->PS.InvShadowmapResolution = 1.0f / ShadowInfo->ResolutionX;
			// Engine ShadowRendering.h:1621 — FVector3f(DepthBias, SlopeDepthBias, MaxSlopeDepthBias)
			PassParameters->PS.PointLightDepthBias = FVector3f(
				ShadowInfo->GetShaderDepthBias(),
				ShadowInfo->GetShaderSlopeDepthBias(),
				ShadowInfo->GetShaderMaxSlopeDepthBias());
		}
		else
		{
			FMemory::Memzero(PassParameters->PS.ShadowViewProjectionMatrices);
			PassParameters->PS.InvShadowmapResolution = 0.0f;
			PassParameters->PS.PointLightDepthBias = FVector3f(0.03f, 0.0f, 0.0f);
		}
	}
	
	// 2D Shadow (CSM / Spot) — uses ScreenToShadowMatrix approach
	// Spot lights reuse CSM infrastructure with NumCSMCascades=1
	{
		const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
		const bool bIsSpotShadow = bIsRadial && ShadowInfo && !ShadowInfo->bOnePassPointLightShadow
			&& ShadowInfo->RenderTargets.DepthTarget;
		const bool bIsCSMShadow = !bIsRadial && CSMCascades.Num() > 0
			&& CSMCascades[0]->RenderTargets.DepthTarget;
		
		// Initialize cascade data to defaults
		PassParameters->PS.NumCSMCascades = 0;
		PassParameters->PS.CSMScreenToShadowMatrix0 = FMatrix44f::Identity;
		PassParameters->PS.CSMScreenToShadowMatrix1 = FMatrix44f::Identity;
		PassParameters->PS.CSMScreenToShadowMatrix2 = FMatrix44f::Identity;
		PassParameters->PS.CSMScreenToShadowMatrix3 = FMatrix44f::Identity;
		PassParameters->PS.CSMSoftTransitionScale = FVector4f(512.0f, 512.0f, 512.0f, 512.0f);
		PassParameters->PS.CSMCascadeEndDepth = FVector4f(FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX);
#if NPR_USE_RECEIVER_BIAS
		PassParameters->PS.CSMReceiverBias = 0.0f;
#endif
#if NPR_CSM_BLEND_CASCADES
		PassParameters->PS.CSMFadeRegionFraction = 0.0f;
#endif
		
		if (bIsCSMShadow)
		{
			// ── Directional light: multi-cascade CSM ──
			FRDGTexture* DirectionalShadowDepthTexture = GraphBuilder.RegisterExternalTexture(CSMCascades[0]->RenderTargets.DepthTarget);
			PassParameters->PS.ShadowDepthTexture = DirectionalShadowDepthTexture;
			PassParameters->PS.ShadowDepthTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			
			// Shadow buffer size (shared across cascades — same atlas)
			const FIntPoint ShadowBufferResolution = CSMCascades[0]->GetShadowBufferResolution();
			PassParameters->PS.ShadowBufferSize = FVector4f(
				(float)ShadowBufferResolution.X,
				(float)ShadowBufferResolution.Y,
				1.0f / (float)ShadowBufferResolution.X,
				1.0f / (float)ShadowBufferResolution.Y);
			
			// Fill per-cascade data. Clamp to 4 — GPU params hold 4 cascades (matrices 0..3,
			// CSMSoftTransitionScale/CSMCascadeEndDepth are float4); >4 would write out of bounds.
			const int32 NumCascades = FMath::Min(CSMCascades.Num(), 4);
			PassParameters->PS.NumCSMCascades = NumCascades;
			for (int32 CascadeIdx = 0; CascadeIdx < NumCascades; CascadeIdx++)
			{
				const FProjectedShadowInfo* Cascade = CSMCascades[CascadeIdx];
				
				const FMatrix ScreenToShadow = NPRLightRenderingUtils::GetScreenToShadowMatrix(Cascade, View);
				const FMatrix44f ScreenToShadow44f(ScreenToShadow);
				switch (CascadeIdx)
				{
					case 0: PassParameters->PS.CSMScreenToShadowMatrix0 = ScreenToShadow44f; break;
					case 1: PassParameters->PS.CSMScreenToShadowMatrix1 = ScreenToShadow44f; break;
					case 2: PassParameters->PS.CSMScreenToShadowMatrix2 = ScreenToShadow44f; break;
					case 3: PassParameters->PS.CSMScreenToShadowMatrix3 = ScreenToShadow44f; break;
				}
				
				const float TransitionSize = NPRLightRenderingUtils::ComputeTransitionSize(Cascade);
				PassParameters->PS.CSMSoftTransitionScale[CascadeIdx] = (TransitionSize > KINDA_SMALL_NUMBER) ? (1.0f / TransitionSize) : 1.0f;
				PassParameters->PS.CSMCascadeEndDepth[CascadeIdx] = Cascade->CascadeSettings.SplitFar;
			}
#if NPR_USE_RECEIVER_BIAS
			// CSMReceiverBias: Single float from r.Shadow.CSMReceiverBias CVar (same for all cascades)
			PassParameters->PS.CSMReceiverBias = NPRLightRenderingUtils::GetShaderReceiverDepthBias(CSMCascades[0]);
#endif
#if NPR_CSM_BLEND_CASCADES
			// CSMFadeRegionFraction: Derive from first cascade's settings
			// Same as engine's CascadeTransitionFraction: FadeLength / CascadeLength
			const FProjectedShadowInfo* FirstCascade = CSMCascades[0];
			const float CascadeLength = FirstCascade->CascadeSettings.SplitFar - FirstCascade->CascadeSettings.SplitNear;
			if (CascadeLength > KINDA_SMALL_NUMBER && FirstCascade->CascadeSettings.FadePlaneLength > 0)
			{
				PassParameters->PS.CSMFadeRegionFraction = FirstCascade->CascadeSettings.FadePlaneLength / CascadeLength;
			}
#endif
		}
		else if (bIsSpotShadow)
		{
			// ── Spot light: single 2D shadow map, reuses CSM infrastructure with 1 cascade ──
			FRDGTexture* SpotShadowDepthTexture = GraphBuilder.RegisterExternalTexture(ShadowInfo->RenderTargets.DepthTarget);
			PassParameters->PS.ShadowDepthTexture = SpotShadowDepthTexture;
			PassParameters->PS.ShadowDepthTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			
			// Shadow buffer size (spot light shadow atlas)
			const FIntPoint ShadowBufferResolution = ShadowInfo->GetShadowBufferResolution();
			PassParameters->PS.ShadowBufferSize = FVector4f(
				(float)ShadowBufferResolution.X,
				(float)ShadowBufferResolution.Y,
				1.0f / (float)ShadowBufferResolution.X,
				1.0f / (float)ShadowBufferResolution.Y);
			
			// Treat as 1-cascade CSM — cascade selection always picks index 0
			PassParameters->PS.NumCSMCascades = 1;
			
			// ScreenToShadowMatrix (same function works for spot perspective projection)
			PassParameters->PS.CSMScreenToShadowMatrix0 = FMatrix44f(
				NPRLightRenderingUtils::GetScreenToShadowMatrix(ShadowInfo, View));
			
			// SoftTransitionScale (uses r.Shadow.SpotLightTransitionScale CVar via existing helper)
			const float TransitionSize = NPRLightRenderingUtils::ComputeTransitionSize(ShadowInfo);
			PassParameters->PS.CSMSoftTransitionScale[0] = (TransitionSize > KINDA_SMALL_NUMBER) ? (1.0f / TransitionSize) : 1.0f;
#if NPR_USE_RECEIVER_BIAS
			// ReceiverBias (uses r.Shadow.SpotLightReceiverBias CVar via existing helper)
			PassParameters->PS.CSMReceiverBias = NPRLightRenderingUtils::GetShaderReceiverDepthBias(ShadowInfo);
#endif
			// CSMCascadeEndDepth already initialized to FLT_MAX — no cascade depth selection needed
		}
		else
		{
			// No 2D shadow available
			PassParameters->PS.ShadowDepthTexture = SystemTextures.Black;
			PassParameters->PS.ShadowDepthTextureSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			PassParameters->PS.ShadowBufferSize = FVector4f(1, 1, 1, 1);
		}
	}
	
	// Shadow post-processing parameters (same as engine's ShadowProjectionPixelShader.usf)
	{
		// ShadowSharpen: from LightProxy->GetShadowSharpen() * 7.0f + 1.0f
		// Engine (ShadowRendering.h line 1299): LightProxy.GetShadowSharpen() * 7.0f + 1.0f
		// Default ShadowSharpen on light is 0.0, so default shader value is 1.0
		// Shader: Shadow = saturate((Shadow - 0.5) * ShadowSharpen + 0.5)
		PassParameters->PS.ShadowSharpen = LightProxy->GetShadowSharpen() * 7.0f + 1.0f;
		
		// ShadowFadeFraction: from FProjectedShadowInfo::FadeAlphas[0] (default 1.0)
		// Engine: FadedShadow = lerp(1.0f, Shadow, ShadowFadeFraction)
		// For CSM, this is typically 1.0 since distance fade is handled separately
		float ShadowFadeValue = 1.0f;
		if (ShadowInfo && ShadowInfo->FadeAlphas.Num() > 0)
		{
			ShadowFadeValue = ShadowInfo->FadeAlphas[0];
		}
		PassParameters->PS.ShadowFadeFraction = ShadowFadeValue;
	}

	// Lighting channels texture
	if (LightingChannelsTexture)
	{
		PassParameters->PS.LightingChannelsTexture = LightingChannelsTexture;
	}

	// IES texture (currently unused - USE_IES_PROFILE=0)
	// if (bUseIESTexture)
	// {
	// 	const FTexture* IESTextureResource = LightProxy->GetIESTextureResource();
	// 	if (IESTextureResource && IESTextureResource->TextureRHI)
	// 	{
	// 		PassParameters->PS.IESTexture = IESTextureResource->TextureRHI;
	// 		PassParameters->PS.IESSampler = IESTextureResource->SamplerStateRHI;
	// 	}
	// 	else
	// 	{
	// 		PassParameters->PS.IESTexture = GWhiteTexture->TextureRHI;
	// 		PassParameters->PS.IESSampler = GWhiteTexture->SamplerStateRHI;
	// 	}
	// }
	// else
	// {
	// 	PassParameters->PS.IESTexture = GWhiteTexture->TextureRHI;
	// 	PassParameters->PS.IESSampler = GWhiteTexture->SamplerStateRHI;
	// }

	// NPR Phase 1: No CVar parameters to bind
	// All NPR parameters are derived from GBuffer values in the shader

	// Render targets (MRT)
	// RT0: Scene Color (additive blend)
	PassParameters->PS.RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);
	// RT1: Shadow Mask (min blend - darkest shadow wins)
	if (ShadowMaskTexture)
	{
		PassParameters->PS.RenderTargets[1] = FRenderTargetBinding(ShadowMaskTexture, ERenderTargetLoadAction::ELoad);
	}
	if (SceneTextures.Depth.Target)
	{
		PassParameters->PS.RenderTargets.DepthStencil = FDepthStencilBinding(
			SceneTextures.Depth.Target, 
			ERenderTargetLoadAction::ELoad, 
			ERenderTargetLoadAction::ELoad, 
			FExclusiveDepthStencil::DepthRead_StencilWrite);
	}

	// VS Parameters using our replicated GetParameters functions
	// Note: bBindViewUniform must be true to set VS.View (required by shader)
	if (bIsRadial)
	{
		PassParameters->VS = FNPRDeferredLightVS::GetParameters(View, LightSceneInfo);
	}
	else
	{
		PassParameters->VS = FNPRDeferredLightVS::GetParameters(View);
	}

	// Setup permutation
	FNPRDeferredLightPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FNPRDeferredLightPS::FLightingChannelsDim>(View.bUsesLightingChannels);
	
	// Shadow quality from CVar (r.ShadowQuality)
	// 1: NoFiltering, 2: 1x1 PCF, 3: 3x3 PCF, 4: 5x5 PCF
	PermutationVector.Set<FNPRDeferredLightPS::FShadowQualityDim>(GetNPRShadowQuality());

	// Light source shape
	if (bIsRadial)
	{
		PermutationVector.Set<FNPRDeferredLightPS::FSourceShapeDim>(
			LightProxy->IsRectLight() ? ENPRLightSourceShape::Rect : ENPRLightSourceShape::Capsule);
		PermutationVector.Set<FNPRDeferredLightPS::FSourceTextureDim>(LightProxy->IsRectLight() && LightProxy->HasSourceTexture());
	}
	else
	{
		PermutationVector.Set<FNPRDeferredLightPS::FSourceShapeDim>(ENPRLightSourceShape::Directional);
		PermutationVector.Set<FNPRDeferredLightPS::FSourceTextureDim>(false);
	}
	
	// Shadow type permutation (replaces runtime bUsePointLightShadow / NumCSMCascades check)
	if (ShadowInfo && bIsRadial && ShadowInfo->bOnePassPointLightShadow)
	{
		PermutationVector.Set<FNPRDeferredLightPS::FShadowTypeDim>(ENPRShadowType::PointCubemap);
	}
	else if ((ShadowInfo && bIsRadial) || (!bIsRadial && CSMCascades.Num() > 0))
	{
		PermutationVector.Set<FNPRDeferredLightPS::FShadowTypeDim>(ENPRShadowType::Shadow2D);
	}
	else
	{
		PermutationVector.Set<FNPRDeferredLightPS::FShadowTypeDim>(ENPRShadowType::None);
	}

	PermutationVector = FNPRDeferredLightPS::RemapPermutation(PermutationVector);

	// Get shader and render
	TShaderMapRef<FNPRDeferredLightPS> PixelShader(View.ShaderMap, PermutationVector);
	InternalRenderNPRLight(GraphBuilder, Scene, View, LightSceneInfo, PixelShader, PassParameters, TEXT("Light::NPRDeferred"));
}

//=============================================================================
// NPR Shadow Rendering
// Applies NPR shadow color from GBuffer.CustomData.rgb based on ShadowMask.
// Engine's SkyLight pass handles actual sky lighting.
//=============================================================================

void RenderNPRShadow(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	FRDGTextureRef ShadowMaskTexture)
{
	// Allocate pass parameters
	FNPRDeferredShadowPassParameters* PassParameters = GraphBuilder.AllocParameters<FNPRDeferredShadowPassParameters>();
	
	// PS parameters
	PassParameters->PS.View = View.ViewUniformBuffer;
	PassParameters->PS.SceneTextures = SceneTextures.UniformBuffer;
	
	// Shadow mask texture (accumulated from light passes with Max blend)
	PassParameters->PS.ShadowMaskTexture = ShadowMaskTexture;
	PassParameters->PS.ShadowMaskSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	
	// Render target: additive to SceneColor
	PassParameters->PS.RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);
	
	// VS parameters (fullscreen mode)
	PassParameters->VS = FNPRDeferredLightVS::GetParameters(View);
	
	// Get pixel shader
	TShaderMapRef<FNPRDeferredShadowPS> PixelShader(View.ShaderMap);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Shadow::NPRDeferred"),
		PassParameters,
		ERDGPassFlags::Raster,
		[&View, PixelShader, PassParameters](FRDGAsyncTask, FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		
		// Additive blend (adds tint color to SceneColor)
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		
		// Vertex shader (fullscreen mode)
		FNPRDeferredLightVS::FPermutationDomain PermutationVectorVS;
		PermutationVectorVS.Set<FNPRDeferredLightVS::FRadialLight>(false);
		TShaderMapRef<FNPRDeferredLightVS> VertexShader(View.ShaderMap, PermutationVectorVS);
		
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

#if PSO_PRECACHING_VALIDATE
		if (PSOCollectorStats::IsFullPrecachingValidationEnabled())
		{
			static const int32 GlobalPSOCollectorIndex = FGlobalPSOCollectorManager::GetIndex(NPRLightPSOCollectorName);
			PSOCollectorStats::CheckGlobalGraphicsPipelineStateInCache(GraphicsPSOInit, GlobalPSOCollectorIndex);
		}
#endif
		
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);
		
		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);
		
		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);
		
		// Draw fullscreen quad
		DrawRectangle(
			RHICmdList,
			0, 0,
			View.ViewRect.Width(), View.ViewRect.Height(),
			View.ViewRect.Min.X, View.ViewRect.Min.Y,
			View.ViewRect.Width(), View.ViewRect.Height(),
			FIntPoint(View.ViewRect.Width(), View.ViewRect.Height()),
			View.GetSceneTexturesConfig().Extent,
			VertexShader,
			EDRF_UseTriangleOptimization);
	});
}

//=============================================================================
// NPR Sky Light + Reflection
// NPR-dedicated replacement for the engine's PBR sky pass. Sky data is self-sourced
// from the SkyLight proxy (show-flag independent) since ShowFlags.SkyLighting is off.
//=============================================================================

//=============================================================================
// Replicates the engine's (non-exported) SetupReflectionUniformParameters, but binds the sky cubemap +
// reflection-capture array by data availability rather than the SkyLighting/ReflectionEnvironment show
// flags. Feeds CompositeReflectionCapturesAndSkylightTWS in the sky/reflection shader.
//=============================================================================

static void NPRSetupReflectionUniformParameters(FRDGBuilder& GraphBuilder, const FViewInfo& View, FReflectionUniformParameters& OutParameters)
{
	const FScene* Scene = (const FScene*)View.Family->Scene;

	FRHITexture* SkyLightTextureResource = nullptr;
	FSamplerStateRHIRef SkyLightCubemapSampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
	FTexture* SkyLightBlendDestinationTextureResource = GBlackTextureCube;
	float ApplySkyLightMask = 0;
	float BlendFraction = 0;
	bool bSkyLightIsDynamic = false;
	ERDGTextureFlags SkyLightTextureFlags = ERDGTextureFlags::None;

	// Replicates FScene::CanSampleSkyLightRealTimeCaptureData() (not exported) — same as RenderNPRSkyLight.
	const FSkyLightSceneProxy* Sky = Scene ? Scene->SkyLight : nullptr;
	const bool bRealTimeCaptureReady = Sky && Sky->bRealTimeCaptureEnabled && Scene->ConvolvedSkyRenderTargetReadyIndex >= 0;

	// NPR: gate on data availability, NOT View.Family->EngineShowFlags.SkyLighting (forced false by NPR).
	if (Sky && (Sky->ProcessedTexture || bRealTimeCaptureReady))
	{
		const FSkyLightSceneProxy& SkyLight = *Sky;

		if (bRealTimeCaptureReady)
		{
			SkyLightTextureResource = Scene->ConvolvedSkyRenderTarget[Scene->ConvolvedSkyRenderTargetReadyIndex]->GetRHI();
		}
		else if (SkyLight.ProcessedTexture)
		{
			SkyLightTextureResource = SkyLight.ProcessedTexture->TextureRHI;
			SkyLightCubemapSampler = SkyLight.ProcessedTexture->SamplerStateRHI;
			BlendFraction = SkyLight.BlendFraction;

			if (SkyLight.BlendFraction > 0.0f && SkyLight.BlendDestinationProcessedTexture)
			{
				if (SkyLight.BlendFraction < 1.0f)
				{
					SkyLightBlendDestinationTextureResource = SkyLight.BlendDestinationProcessedTexture;
				}
				else
				{
					SkyLightTextureResource = SkyLight.BlendDestinationProcessedTexture->TextureRHI;
					SkyLightCubemapSampler = SkyLight.ProcessedTexture->SamplerStateRHI;
					BlendFraction = 0;
				}
			}

			SkyLightTextureFlags = ERDGTextureFlags::SkipTracking;
		}

		ApplySkyLightMask = 1;
		bSkyLightIsDynamic = !SkyLight.bHasStaticLighting && !SkyLight.bWantsStaticShadowing;
	}

	FRDGTextureRef SkyLightTexture = nullptr;
	if (SkyLightTextureResource)
	{
		SkyLightTexture = RegisterExternalTexture(GraphBuilder, SkyLightTextureResource, TEXT("NPRReflectionSkyLightTexture"), SkyLightTextureFlags);
	}
	else
	{
		SkyLightTexture = GSystemTextures.GetCubeBlackDummy(GraphBuilder);
	}

	const int32 CubemapWidth = SkyLightTexture->Desc.Extent.X;
	const float SkyMipCount = FMath::Log2(static_cast<float>(FMath::Max(CubemapWidth, 1))) + 1.0f;

	OutParameters.SkyLightCubemap = SkyLightTexture;
	OutParameters.SkyLightCubemapSampler = SkyLightCubemapSampler;
	OutParameters.SkyLightBlendDestinationCubemap = SkyLightBlendDestinationTextureResource->TextureRHI;
	OutParameters.SkyLightBlendDestinationCubemapSampler = SkyLightBlendDestinationTextureResource->SamplerStateRHI;
	OutParameters.SkyLightParameters = FVector4f(SkyMipCount - 1.0f, ApplySkyLightMask, bSkyLightIsDynamic ? 1.0f : 0.0f, BlendFraction);

	// Reflection-capture cubemap array — gate on data availability (NOT the ReflectionEnvironment show flag).
	FRHITexture* CubeArrayTexture = SupportsTextureCubeArray(View.FeatureLevel) ? GBlackCubeArrayTexture->TextureRHI : GBlackTextureCube->TextureRHI;
	if (SupportsTextureCubeArray(View.FeatureLevel)
		&& Scene
		&& Scene->ReflectionSceneData.CubemapArray.IsValid()
		&& Scene->ReflectionSceneData.RegisteredReflectionCaptures.Num())
	{
		CubeArrayTexture = Scene->ReflectionSceneData.CubemapArray.GetRenderTarget()->GetRHI();
	}

	OutParameters.ReflectionCubemap = CubeArrayTexture;
	OutParameters.ReflectionCubemapSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	OutParameters.PreIntegratedGF = GSystemTextures.PreintegratedGF->GetRHI();
	OutParameters.PreIntegratedGFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
}

void RenderNPRSkyLight(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FMinimalSceneTextures& SceneTextures,
	bool bSkyLightingEnabled,
	bool bDiffuseAmbientEnabled)
{
	const FSkyLightSceneProxy* Sky = Scene ? Scene->SkyLight : nullptr;

	// The pass always runs (SSR + captures are sky-light independent); the sky terms are gated by binding a
	// black dummy cube / zero SH when disabled. bApplySky -> sky cubemap; bApplyDiffuse -> sky SH ambient.
	const bool bRealTime = Sky && Sky->bRealTimeCaptureEnabled && Scene->ConvolvedSkyRenderTargetReadyIndex >= 0;
	const bool bBaked    = Sky && Sky->ProcessedTexture && Sky->ProcessedTexture->TextureRHI;
	const bool bApplySky     = bSkyLightingEnabled && (bRealTime || bBaked);  // sky cubemap (specular + diffuse source)
	const bool bApplyDiffuse = bApplySky && bDiffuseAmbientEnabled;           // sky SH ambient (indirect)

	FRDGTextureRef SkyCubemap = nullptr;
	FRDGBufferRef SkyIrradianceBuffer = nullptr;
	float SkyMaxMip = 0.0f;

	// Sky cubemap: real (real-time capture or baked proxy) when sky lighting is on, else a black dummy.
	if (bApplySky && bRealTime)
	{
		SkyCubemap = GraphBuilder.RegisterExternalTexture(Scene->ConvolvedSkyRenderTarget[Scene->ConvolvedSkyRenderTargetReadyIndex]);
	}
	else if (bApplySky)
	{
		SkyCubemap = RegisterExternalTexture(GraphBuilder, Sky->ProcessedTexture->TextureRHI, TEXT("NPRSkyCubemap"));
	}
	else
	{
		SkyCubemap = GSystemTextures.GetCubeBlackDummy(GraphBuilder);
	}

	// Sky SH diffuse ambient: bound only when both sky lighting AND indirect lighting are wanted; else zeroed.
	if (bApplyDiffuse && bRealTime)
	{
		SkyIrradianceBuffer = GraphBuilder.RegisterExternalBuffer(Scene->SkyIrradianceEnvironmentMap);
	}
	else if (bApplyDiffuse)
	{
		// Baked: pack CPU SH from the proxy the same way the engine does (helper fills [0..6]; [7] = brightness).
		FVector4f PackedSH[SKY_IRRADIANCE_ENVIRONMENT_MAP_VEC4_COUNT];
		SetupSkyIrradianceEnvironmentMapConstantsFromSkyIrradiance(PackedSH, Sky->IrradianceEnvironmentMap);
		PackedSH[7] = FVector4f(Sky->AverageBrightness, Sky->AverageBrightness, Sky->AverageBrightness, Sky->AverageBrightness);
		SkyIrradianceBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NPRSkyIrradiance"),
			sizeof(FVector4f), SKY_IRRADIANCE_ENVIRONMENT_MAP_VEC4_COUNT, PackedSH, sizeof(PackedSH));
	}
	else
	{
		FVector4f ZeroSH[SKY_IRRADIANCE_ENVIRONMENT_MAP_VEC4_COUNT];
		FMemory::Memzero(ZeroSH, sizeof(ZeroSH));
		SkyIrradianceBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NPRSkyIrradianceZero"),
			sizeof(FVector4f), SKY_IRRADIANCE_ENVIRONMENT_MAP_VEC4_COUNT, ZeroSH, sizeof(ZeroSH));
	}

	// Max mip index = Log2(Width) (engine ReflectionStruct.SkyLightParameters.x)
	const int32 CubemapWidth = SkyCubemap->Desc.Extent.X;
	SkyMaxMip = FMath::Log2((float)FMath::Max(CubemapWidth, 1));

	FNPRSkyLightPassParameters* PassParameters = GraphBuilder.AllocParameters<FNPRSkyLightPassParameters>();

	PassParameters->PS.View = View.ViewUniformBuffer;
	PassParameters->PS.SceneTextures = SceneTextures.UniformBuffer;
	PassParameters->PS.NPRSkyIrradiance = GraphBuilder.CreateSRV(SkyIrradianceBuffer);
	PassParameters->PS.NPRSkyCubemap = SkyCubemap;
	PassParameters->PS.NPRSkyCubemapSampler = TStaticSamplerState<SF_Trilinear>::GetRHI();
	PassParameters->PS.NPRSkyMaxMip = SkyMaxMip;

	// Sky tint reconstructed from the SkyLight component (LightColor * Intensity * SpecifiedCubemapColorScale).
	// 0 when sky lighting is off (cube/SH are already zero, this just avoids tinting). Scales both the sky
	// SH diffuse and the sky cubemap specular reflection.
	FLinearColor SkyColor = FLinearColor::Black;
	if (bApplySky && Sky->LightComponent)
	{
		SkyColor = Sky->LightComponent->GetLightColor() * Sky->LightComponent->Intensity * Sky->SpecifiedCubemapColorScale;
	}
	PassParameters->PS.NPRSkyColor = FVector3f(SkyColor.R, SkyColor.G, SkyColor.B);

	// ---- SSR (screen-space scene reflection) inputs ----
	// HZB is built unconditionally in RenderOcclusion (before the base pass), so it is valid here
	// even with DeferredLighting=false. GetHZBParameters() isn't RENDERER_API-exported, so replicate
	// the single value the ray-cast needs: HZBUvFactorAndInvFactor = ViewRect.Size / (2 * HZB.Extent).
	const FRDGSystemTextures& SystemTex = FRDGSystemTextures::Get(GraphBuilder);
	if (View.HZB)
	{
		const FIntPoint HZBExtent = View.HZB->Desc.Extent;
		const float HZBFactorX = (float)View.ViewRect.Width()  / (float)(2 * FMath::Max(HZBExtent.X, 1));
		const float HZBFactorY = (float)View.ViewRect.Height() / (float)(2 * FMath::Max(HZBExtent.Y, 1));
		PassParameters->PS.NPRHZBTexture = View.HZB;
		PassParameters->PS.NPRHZBUvFactorAndInvFactor = FVector4f(HZBFactorX, HZBFactorY, 1.0f / HZBFactorX, 1.0f / HZBFactorY);
		PassParameters->PS.NPRSSREnabled = 1;

		// Copy SceneColor so SSR can read the pre-reflection lit scene while this pass writes SceneColor
		// (a shader can't sample the render target it writes). Copy the full extent: a view-rect-only copy
		// leaves the rest uninitialized and the bilinear SSR fetch can read garbage at the edges.
		// Build a mip chain on the copy so SSR can read a roughness-blurred mip: RayCast accumulates a
		// Level (8/NumSteps * Roughness per step) we sample at, matching the engine's roughness blur
		// (SSRTDiffuseIndirect.usf:488 samples the reduced scene color at that Level). 5 mips like the engine.
		FRDGTextureDesc SceneColorCopyDesc = SceneTextures.Color.Target->Desc;
		SceneColorCopyDesc.NumMips = 5;
		SceneColorCopyDesc.Flags |= TexCreate_UAV | TexCreate_ShaderResource;
		FRDGTextureRef SceneColorCopy = GraphBuilder.CreateTexture(SceneColorCopyDesc, TEXT("NPRSceneColorCopy"));
		AddCopyTexturePass(GraphBuilder, SceneTextures.Color.Target, SceneColorCopy);
		FGenerateMips::Execute(GraphBuilder, View.GetFeatureLevel(), SceneColorCopy);
		PassParameters->PS.NPRSceneColorCopy = SceneColorCopy;
	}
	else
	{
		PassParameters->PS.NPRHZBTexture = SystemTex.Black;
		PassParameters->PS.NPRHZBUvFactorAndInvFactor = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		PassParameters->PS.NPRSSREnabled = 0;
		PassParameters->PS.NPRSceneColorCopy = SystemTex.Black;
	}
	PassParameters->PS.NPRHZBSampler = TStaticSamplerState<SF_Point>::GetRHI();
	PassParameters->PS.NPRSceneColorSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// ---- Reflection environment (captures + sky) ----
	// ReflectionStruct: build from scene reflection/sky data (engine's reflection pass is skipped here).
	FReflectionUniformWrapper* ReflectionWrapper = GraphBuilder.AllocParameters<FReflectionUniformWrapper>();
	NPRSetupReflectionUniformParameters(GraphBuilder, View, *ReflectionWrapper);
	PassParameters->PS.ReflectionStruct = GraphBuilder.CreateUniformBuffer(ReflectionWrapper);

	// The reinterpret_casts below rebind the engine's UB refs as our wrapper types (same underlying UB,
	// metadata resolved by name). They are only valid while each wrapper stays layout-identical to the
	// engine struct it inherits — assert that so an engine layout change fails to compile instead of
	// silently mis-binding.
	static_assert(sizeof(FForwardLightUniformWrapper) == sizeof(FForwardLightUniformParameters),
		"FForwardLightUniformWrapper must stay layout-identical to FForwardLightUniformParameters");
	static_assert(sizeof(FReflectionCaptureWrapper) == sizeof(FReflectionCaptureShaderData),
		"FReflectionCaptureWrapper must stay layout-identical to FReflectionCaptureShaderData");

	// ForwardLightStruct: reuse the engine's already-built forward grid UB (holds the per-tile
	// reflection-capture culling grid). Reinterpret to our wrapper type (same UB, metadata by name).
	const TRDGUniformBufferRef<FForwardLightUniformParameters>& EngineFwdUB = View.ForwardLightingResources.ForwardLightUniformBuffer;
	PassParameters->PS.ForwardLightStruct = reinterpret_cast<const TRDGUniformBufferRef<FForwardLightUniformWrapper>&>(EngineFwdUB);

	// ReflectionCaptureSM5: reuse the engine's per-view capture-data UB (built in InitViews,
	// independent of DeferredLighting). Reinterpret to our wrapper type.
	const TUniformBufferRef<FReflectionCaptureShaderData>& EngineCaptureUB = View.ReflectionCaptureUniformBuffer;
	PassParameters->PS.ReflectionCaptureSM5 = reinterpret_cast<const TUniformBufferRef<FReflectionCaptureWrapper>&>(EngineCaptureUB);

	// Additive to SceneColor
	PassParameters->PS.RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);

	// VS parameters (fullscreen mode) — reuse directional light VS
	PassParameters->VS = FNPRDeferredLightVS::GetParameters(View);

	TShaderMapRef<FNPRSkyLightPS> PixelShader(View.ShaderMap);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("NPRSkyLight"),
		PassParameters,
		ERDGPassFlags::Raster,
		[&View, PixelShader, PassParameters](FRDGAsyncTask, FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

		// Additive blend (adds ambient + reflection to SceneColor)
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;

		FNPRDeferredLightVS::FPermutationDomain PermutationVectorVS;
		PermutationVectorVS.Set<FNPRDeferredLightVS::FRadialLight>(false);
		TShaderMapRef<FNPRDeferredLightVS> VertexShader(View.ShaderMap, PermutationVectorVS);

		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

		DrawRectangle(
			RHICmdList,
			0, 0,
			View.ViewRect.Width(), View.ViewRect.Height(),
			View.ViewRect.Min.X, View.ViewRect.Min.Y,
			View.ViewRect.Width(), View.ViewRect.Height(),
			FIntPoint(View.ViewRect.Width(), View.ViewRect.Height()),
			View.GetSceneTexturesConfig().Extent,
			VertexShader,
			EDRF_UseTriangleOptimization);
	});
}

} // namespace NPRLightRendering

//=============================================================================
// PSO Precaching for NPR Deferred Light Shaders
// Collects PSO initializers at level load time to avoid runtime hitches
// Uses FRegisterGlobalPSOCollectorFunction (same pattern as VolumetricFog.cpp)
//=============================================================================

void NPRLightGlobalPSOCollector(
	const FSceneTexturesConfig& SceneTexturesConfig, 
	int32 GlobalPSOCollectorIndex, 
	TArray<FPSOPrecacheData>& PSOInitializers)
{
	EShaderPlatform ShaderPlatform = SceneTexturesConfig.ShaderPlatform;
	if (!IsFeatureLevelSupported(ShaderPlatform, ERHIFeatureLevel::SM5))
	{
		return;
	}
	
	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(ShaderPlatform);
	if (!GlobalShaderMap)
	{
		return;
	}
	
	// Helper lambda to add PSO for a given permutation
	auto AddPSOInitializer = [&](
		ENPRLightSourceShape Shape,
		ENPRShadowType ShadowType,
		bool bSourceTexture,
		bool bLightingChannels,
		int32 ShadowQuality)
	{
		// Build permutation
		FNPRDeferredLightPS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FNPRDeferredLightPS::FSourceShapeDim>(Shape);
		PermutationVector.Set<FNPRDeferredLightPS::FShadowTypeDim>(ShadowType);
		PermutationVector.Set<FNPRDeferredLightPS::FSourceTextureDim>(bSourceTexture);
		PermutationVector.Set<FNPRDeferredLightPS::FLightingChannelsDim>(bLightingChannels);
		PermutationVector.Set<FNPRDeferredLightPS::FShadowQualityDim>(ShadowQuality);
		
		PermutationVector = FNPRDeferredLightPS::RemapPermutation(PermutationVector);
		
		// Get shaders
		TShaderMapRef<FNPRDeferredLightPS> PixelShader(GlobalShaderMap, PermutationVector);
		
		FNPRDeferredLightVS::FPermutationDomain PermutationVectorVS;
		PermutationVectorVS.Set<FNPRDeferredLightVS::FRadialLight>(Shape != ENPRLightSourceShape::Directional);
		TShaderMapRef<FNPRDeferredLightVS> VertexShader(GlobalShaderMap, PermutationVectorVS);
		
		if (!PixelShader.IsValid() || !VertexShader.IsValid())
		{
			return;
		}
		
		const bool bIsDirectional = (Shape == ENPRLightSourceShape::Directional);
		
		// Build PSO initializer (same setup as SetupNPRLightGraphicsPSOState)
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		
		// MRT blend matching the runtime light pass (SetupNPRLightGraphicsPSOState): RT0 SceneColor
		// additive, RT1 ShadowMask max. Must match or the precached PSO never matches the runtime one.
		GraphicsPSOInit.BlendState = TStaticBlendState<
			CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One,  // RT0: SceneColor (additive)
			CW_RED,  BO_Max, BF_One, BF_One, BO_Max, BF_One, BF_One   // RT1: ShadowMask (max)
		>::GetRHI();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		
		if (bIsDirectional)
		{
			// Fullscreen pass
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		}
		else
		{
			// Radial light geometry pass - cache both inside/outside camera variants
			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GetVertexDeclarationFVector4();
			
			// Camera inside light geometry (backface culling)
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_CCW>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		}
		
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		
		// Render target info — must match the runtime MRT: RT0 SceneColor (HDR FloatRGBA), RT1 ShadowMask (R8).
		FGraphicsPipelineRenderTargetsInfo RenderTargetsInfo;
		RenderTargetsInfo.NumSamples = 1;
		AddRenderTargetInfo(PF_FloatRGBA, TexCreate_RenderTargetable | TexCreate_ShaderResource, RenderTargetsInfo);  // RT0 SceneColor
		AddRenderTargetInfo(PF_R8, TexCreate_RenderTargetable | TexCreate_ShaderResource, RenderTargetsInfo);          // RT1 ShadowMask
		
		GraphicsPSOInit.StatePrecachePSOHash = RHIComputeStatePrecachePSOHash(GraphicsPSOInit);
		ApplyTargetsInfo(GraphicsPSOInit, RenderTargetsInfo);
		
		FPSOPrecacheData PSOPrecacheData;
		PSOPrecacheData.bRequired = false;  // Background precache, not blocking
		PSOPrecacheData.Type = FPSOPrecacheData::EType::Graphics;
		PSOPrecacheData.GraphicsPSOInitializer = GraphicsPSOInit;
#if PSO_PRECACHING_VALIDATE
		PSOPrecacheData.PSOCollectorIndex = GlobalPSOCollectorIndex;
		PSOPrecacheData.VertexFactoryType = nullptr;
#endif
		
		PSOInitializers.Add(MoveTemp(PSOPrecacheData));
		
		// For radial lights, also cache camera-outside variant (frontface culling + depth test)
		if (!bIsDirectional)
		{
			FGraphicsPipelineStateInitializer GraphicsPSOInitOutside = GraphicsPSOInit;
			GraphicsPSOInitOutside.RasterizerState = TStaticRasterizerState<FM_Solid, CM_CW>::GetRHI();
			GraphicsPSOInitOutside.DepthStencilState = TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI();
			GraphicsPSOInitOutside.StatePrecachePSOHash = RHIComputeStatePrecachePSOHash(GraphicsPSOInitOutside);
			ApplyTargetsInfo(GraphicsPSOInitOutside, RenderTargetsInfo);
			
			FPSOPrecacheData PSOPrecacheDataOutside;
			PSOPrecacheDataOutside.bRequired = false;
			PSOPrecacheDataOutside.Type = FPSOPrecacheData::EType::Graphics;
			PSOPrecacheDataOutside.GraphicsPSOInitializer = GraphicsPSOInitOutside;
#if PSO_PRECACHING_VALIDATE
			PSOPrecacheDataOutside.PSOCollectorIndex = GlobalPSOCollectorIndex;
			PSOPrecacheDataOutside.VertexFactoryType = nullptr;
#endif
			PSOInitializers.Add(MoveTemp(PSOPrecacheDataOutside));
		}
	};
	
	// Collect PSOs for common permutations (skip LightingChannels=true and ShadowQuality>2 for faster startup)
	// These are handled by ShouldPrecachePermutation and will be compiled on-demand if needed
	
	// Directional Light permutations
	{
		const ENPRLightSourceShape Shape = ENPRLightSourceShape::Directional;
		
		// No shadow
		AddPSOInitializer(Shape, ENPRShadowType::None, false, false, 1);
		
		// CSM shadow (quality 1-2)
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, false, false, 1);
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, false, false, 2);
	}
	
	// Point/Spot Light permutations (Capsule shape)
	{
		const ENPRLightSourceShape Shape = ENPRLightSourceShape::Capsule;
		
		// No shadow
		AddPSOInitializer(Shape, ENPRShadowType::None, false, false, 1);
		
		// Point cubemap shadow (quality 1-2)
		AddPSOInitializer(Shape, ENPRShadowType::PointCubemap, false, false, 1);
		AddPSOInitializer(Shape, ENPRShadowType::PointCubemap, false, false, 2);
		
		// 2D shadow (spot light, quality 1-2)
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, false, false, 1);
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, false, false, 2);
	}
	
	// Rect Light permutations
	{
		const ENPRLightSourceShape Shape = ENPRLightSourceShape::Rect;
		
		// No shadow, with/without source texture
		AddPSOInitializer(Shape, ENPRShadowType::None, false, false, 1);
		AddPSOInitializer(Shape, ENPRShadowType::None, true, false, 1);
		
		// 2D shadow (quality 1-2), with/without source texture
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, false, false, 1);
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, false, false, 2);
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, true, false, 1);
		AddPSOInitializer(Shape, ENPRShadowType::Shadow2D, true, false, 2);
	}
}

// Register PSO collector - called automatically at module startup
FRegisterGlobalPSOCollectorFunction RegisterNPRLightPSOCollector(
	&NPRLightGlobalPSOCollector, 
	NPRLightPSOCollectorName);
