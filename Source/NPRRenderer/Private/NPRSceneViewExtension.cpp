#include "NPRSceneViewExtension.h"
#include "NPRRendererModule.h"
#include "NPRLightRendering.h"
#include "NPRLightRenderingUtils.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "SceneRenderTargetParameters.h"
#include "RenderGraphUtils.h"
#include "LightSceneInfo.h"

FNPRSceneViewExtension::FNPRSceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	UE_LOG(LogNPRRenderer, Log, TEXT("NPR SceneViewExtension initialized"));
}

void FNPRSceneViewExtension::SetupNPRShowFlags(FEngineShowFlags& ShowFlags)
{
	// Only disable the engine features NPR replaces; features the passes depend on (Lighting,
	// ReflectionEnvironment, GlobalIllumination) are left as-is and read back by the passes.
	ShowFlags.SetDeferredLighting(false);      // engine direct lighting replaced by NPR light passes
	ShowFlags.SetSubsurfaceScattering(false);  // CustomData.rgb repurposed as NPR shadow color
	// IndirectLightingCache stays ON: the base pass writes GBuffer.IndirectIrradiance from it, which the
	// NPR reflection pass feeds to the compositor to brighten capture reflections on metal (no sky needed).

	// Capture SkyLighting intent before forcing it off (RenderNPRSkyLight still gates on it). Forcing it off
	// disables the engine sky apply and zeroes View.SkyLightColor (keeps sky out of the NPR shadow slot).
	bSkyLightingEnabled = ShowFlags.SkyLighting;
	ShowFlags.SetSkyLighting(false);           // engine sky diffuse/reflection replaced by RenderNPRSkyLight

	// Lumen is disabled per-view in SetupView() (DynamicGlobalIlluminationMethod=None), not via a show flag.
}

void FNPRSceneViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	if (!bEnabled) return;

	SetupNPRShowFlags(InViewFamily.EngineShowFlags);
}

void FNPRSceneViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (!bEnabled) return;

	// Disable dynamic GI (Lumen/SSGI) for our view. method=None avoids Lumen's cost/side effects while
	// leaving the GlobalIllumination flag on, so IndirectLightingIntensity (the reflection-capture scale)
	// is preserved. Runs after EndFinalPostprocessSettings, so it reaches the view uniform buffer setup.
	InView.FinalPostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
}

//=============================================================================
// Helper: Collect Visible Lights from Scene
// Mirrors engine's RenderLights light collection logic
//=============================================================================

static void CollectVisibleLights(
	const FScene* Scene,
	const FViewInfo& View,
	TArray<const FLightSceneInfo*, SceneRenderingAllocator>& OutDirectionalLights,
	TArray<const FLightSceneInfo*, SceneRenderingAllocator>& OutLocalLights)
{
	OutDirectionalLights.Reset();
	OutLocalLights.Reset();
	
	if (!Scene)
	{
		return;
	}
	
	// Collect all visible lights (using engine's iterator pattern)
	for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
	{
		const FLightSceneInfoCompact& LightCompact = *LightIt;
		const FLightSceneInfo* LightSceneInfo = LightCompact.LightSceneInfo;
		
		if (!LightSceneInfo || !NPRLightRenderingUtils::ShouldRenderLight(LightSceneInfo, View))
		{
			continue;
		}
		
		const ELightComponentType LightType = (ELightComponentType)LightSceneInfo->Proxy->GetLightType();
		
		if (LightType == LightType_Directional)
		{
			OutDirectionalLights.Add(LightSceneInfo);
		}
		else
		{
			// Point, Spot, Rect lights
			OutLocalLights.Add(LightSceneInfo);
		}
	}
	
	UE_LOG(LogNPRRenderer, Verbose, TEXT("NPR: Collected %d directional lights, %d local lights"),
		OutDirectionalLights.Num(), OutLocalLights.Num());
}

//=============================================================================
// PC Deferred Implementation - Per-Light Passes (matching engine's RenderLights)
//=============================================================================

void FNPRSceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> /*InSceneTextures*/)
{
	if (!bEnabled) return;

	// Respect the incoming Lighting master flag: when it is off (e.g. unlit / lighting-only viewmodes),
	// the engine does no lighting, so NPR skips its lighting + reflection passes too.
	if (!InView.Family->EngineShowFlags.Lighting) return;

	checkSlow(InView.bIsViewInfo);
	const FViewInfo& View = static_cast<const FViewInfo&>(InView);
	
	// Get Scene
	const FScene* Scene = View.Family->Scene ? View.Family->Scene->GetRenderScene() : nullptr;
	if (!Scene)
	{
		return;
	}
	
	// Create SceneTextures with GBuffer data
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures = 
		CreateSceneTextureUniformBuffer(
			GraphBuilder, 
			InView, 
			ESceneTextureSetupMode::GBuffers | ESceneTextureSetupMode::SceneDepth);
	
	if (!SceneTextures)
	{
		return;
	}
	
	// Create MinimalSceneTextures for RenderNPRLight
	FMinimalSceneTextures MinimalSceneTextures;
	MinimalSceneTextures.UniformBuffer = SceneTextures;
	MinimalSceneTextures.Color.Target = RenderTargets[0].GetTexture();
	MinimalSceneTextures.Depth.Target = RenderTargets.DepthStencil.GetTexture();
	
	// Shadow mask: light passes write it (Max blend, any light brightens), the NPR shadow pass reads it
	// to apply the shadow color from GBuffer.CustomData.rgb.
	FRDGTextureDesc ShadowMaskDesc = FRDGTextureDesc::Create2D(
		MinimalSceneTextures.Color.Target->Desc.Extent,
		PF_R8,  // Single channel, 8-bit is sufficient for shadow mask
		FClearValueBinding::Black,  // Initialize to 0.0 (fully in shadow - no lights yet)
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	
	FRDGTextureRef ShadowMaskTexture = GraphBuilder.CreateTexture(ShadowMaskDesc, TEXT("NPRShadowMask"));
	
	// Clear shadow mask to 0.0 (fully in shadow) before light passes
	// Max blend: any light with shadowMask > 0 will brighten the pixel
	AddClearRenderTargetPass(GraphBuilder, ShadowMaskTexture, FLinearColor::Black);
	
	// Collect visible lights from scene (matching engine's RenderLights)
	TArray<const FLightSceneInfo*, SceneRenderingAllocator> DirectionalLights;
	TArray<const FLightSceneInfo*, SceneRenderingAllocator> LocalLights;
	CollectVisibleLights(Scene, View, DirectionalLights, LocalLights);
	
	// Lighting channels texture (may be null if not used)
	FRDGTextureRef LightingChannelsTexture = nullptr;
	
	RDG_EVENT_SCOPE(GraphBuilder, "NPR_Lighting");

	//=========================================================================
	// Render Directional Lights (Per-light passes like engine)
	// Each pass writes to SceneColor (additive) and ShadowMask (min blend)
	//=========================================================================
	
	for (const FLightSceneInfo* LightSceneInfo : DirectionalLights)
	{
		NPRLightRendering::RenderNPRLight(
			GraphBuilder,
			Scene,
			View,
			MinimalSceneTextures,
			LightSceneInfo,
			LightingChannelsTexture,
			ShadowMaskTexture);
	}
	
	//=========================================================================
	// Render Local Lights (per-light passes, full shadow support)
	//=========================================================================

	for (const FLightSceneInfo* LightSceneInfo : LocalLights)
	{
		NPRLightRendering::RenderNPRLight(
			GraphBuilder,
			Scene,
			View,
			MinimalSceneTextures,
			LightSceneInfo,
			LightingChannelsTexture,
			ShadowMaskTexture);
	}
	
	// NPR shadow: applies the shadow color from GBuffer.CustomData.rgb using the accumulated ShadowMask.
	NPRLightRendering::RenderNPRShadow(
		GraphBuilder,
		View,
		MinimalSceneTextures,
		ShadowMaskTexture);

	// NPR sky light + reflection. Runs last so SSR reflects the fully NPR-lit SceneColor. Sky terms are
	// gated by the incoming flags: SkyLighting -> sky cubemap, GlobalIllumination -> sky SH diffuse ambient.
	const bool bDiffuseAmbientEnabled = View.Family->EngineShowFlags.GlobalIllumination;
	NPRLightRendering::RenderNPRSkyLight(
		GraphBuilder,
		Scene,
		View,
		MinimalSceneTextures,
		bSkyLightingEnabled,
		bDiffuseAmbientEnabled);
}