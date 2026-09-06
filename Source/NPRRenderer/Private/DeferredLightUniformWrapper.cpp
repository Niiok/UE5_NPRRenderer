#include "DeferredLightUniformWrapper.h"
#include "NPRRendererModule.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "SceneManagement.h"
#include "LightSceneInfo.h"
#include "HAL/IConsoleManager.h"
#include "RenderUtils.h"  // For IsStaticLightingAllowed
#include "ShaderParameterMacros.h"  // For FindUniformBufferStructByName

//=============================================================================
// Metadata for RDG uniform buffer binding
//=============================================================================

const FShaderParametersMetadata* GetForwardDeclaredShaderParametersStructMetadata(
	const FDeferredLightUniformWrapper*)
{
	return FDeferredLightUniformWrapper::FTypeInfo::GetStructMetadata();
}

const FShaderParametersMetadata* FDeferredLightUniformWrapper::FTypeInfo::GetStructMetadata()
{
	// Simply look up the engine's already-registered metadata at runtime
	// This avoids all metadata creation complexity and nested struct resolution issues
	return FindUniformBufferStructByName(TEXT("DeferredLightUniforms"));
}

//=============================================================================
// Helper to get light max draw distance scale from CVar
//=============================================================================

static float GetLightMaxDrawDistanceScale()
{
	static IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightMaxDrawDistanceScale"));
	return CVar ? CVar->GetFloat() : 1.0f;
}

static float GetLightFadeFactorInternal(const FSceneView& View, const FLightSceneProxy* Proxy)
{
	const FSphere LightBounds = Proxy->GetBoundingSphere();
	const float DistanceSquared = (LightBounds.Center - View.ViewMatrices.GetViewOrigin()).SizeSquared();
	
	const float ProxyMaxDist = Proxy->GetMaxDrawDistance();
	const float ScaledMaxDist = ProxyMaxDist * GetLightMaxDrawDistanceScale();
	const float FadeRange = Proxy->GetFadeRange();
	
	if (ProxyMaxDist <= 0.0f)
	{
		return 1.0f;
	}
	
	float DistanceFade = (ScaledMaxDist - FMath::Sqrt(DistanceSquared)) / FadeRange;
	DistanceFade = FMath::Clamp(DistanceFade, 0.0f, 1.0f);
	
	return DistanceFade;
}

//=============================================================================
// GetDeferredLightParametersWrapper - Replicated from LightRendering.cpp
//=============================================================================

FDeferredLightUniformWrapper GetDeferredLightParametersWrapper(
	const FSceneView& View,
	const FLightSceneInfo& LightSceneInfo,
	bool bUseLightFunctionAtlas,
	uint32 LightFlags)
{
	FDeferredLightUniformWrapper Out;
	
	// Get light shader parameters from proxy
	FLightRenderParameters LightParameters;
	LightSceneInfo.Proxy->GetLightShaderParameters(LightParameters, LightFlags);
	LightParameters.MakeShaderParameters(View.ViewMatrices, View.GetLastEyeAdaptationExposure(), Out.LightParameters);
	
	// Distance fade
	// Note: IsPrecomputedLightingValid() is not exported, using simplified logic
	const bool bPrecomputedLightingValid = !LightSceneInfo.Proxy->HasStaticShadowing();
	const FVector2D FadeParams = LightSceneInfo.Proxy->GetDirectionalLightDistanceFadeParameters(
		View.GetFeatureLevel(),
		bPrecomputedLightingValid,
		View.MaxShadowCascades);
	Out.DistanceFadeMAD = FVector2f(FadeParams.Y, -FadeParams.X * FadeParams.Y);
	
	// Shadow map channel mask
	int32 ShadowMapChannel = LightSceneInfo.Proxy->GetShadowMapChannel();
	const bool bAllowStaticLighting = IsStaticLightingAllowed();
	
	if (!bAllowStaticLighting)
	{
		ShadowMapChannel = INDEX_NONE;
		Out.ShadowMapChannelMask = FVector4f(0);
	}
	else
	{
		Out.ShadowMapChannelMask = FVector4f(
			ShadowMapChannel == 0 ? 1 : 0,
			ShadowMapChannel == 1 ? 1 : 0,
			ShadowMapChannel == 2 ? 1 : 0,
			ShadowMapChannel == 3 ? 1 : 0);
	}
	
	// Shadowed bits
	const bool bDynamicShadows = View.Family->EngineShowFlags.DynamicShadows;
	const bool bHasLightFunction = LightSceneInfo.Proxy->GetLightFunctionMaterial() != nullptr;
	Out.ShadowedBits = LightSceneInfo.Proxy->CastsStaticShadow() || bHasLightFunction ? 1 : 0;
	Out.ShadowedBits |= LightSceneInfo.Proxy->CastsDynamicShadow() && bDynamicShadows ? 3 : 0;
	
	// Volumetric scattering
	Out.VolumetricScatteringIntensity = LightSceneInfo.Proxy->GetVolumetricScatteringIntensity();
	
	// Contact shadows
	// Replicated from GetDeferredLightParameters + GetLightContactShadowParameters
	// (LightRendering.cpp:638-657, 262-290)
	static auto* ContactShadowsCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ContactShadows"));
	Out.ContactShadowLength = 0;
	Out.ContactShadowCastingIntensity = 1.0f;
	Out.ContactShadowNonCastingIntensity = 0.0f;
	if (ContactShadowsCVar && ContactShadowsCVar->GetValueOnRenderThread() != 0)
	{
		float CSLength = LightSceneInfo.Proxy->GetContactShadowLength();
		bool bLengthInWS = LightSceneInfo.Proxy->IsContactShadowLengthInWS();
		Out.ContactShadowCastingIntensity = LightSceneInfo.Proxy->GetContactShadowCastingIntensity();
		Out.ContactShadowNonCastingIntensity = LightSceneInfo.Proxy->GetContactShadowNonCastingIntensity();

		// CVar overrides (same as engine)
		static IConsoleVariable* OverrideLengthCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ContactShadows.OverrideLength"));
		static IConsoleVariable* OverrideLengthInWSCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ContactShadows.OverrideLengthInWS"));
		if (OverrideLengthCVar && OverrideLengthCVar->GetFloat() >= 0.0f)
		{
			CSLength = OverrideLengthCVar->GetFloat();
			if (OverrideLengthInWSCVar) { bLengthInWS = OverrideLengthInWSCVar->GetBool(); }
		}
		if (!bLengthInWS)
		{
			CSLength *= 2.0f;  // Screen space convention (engine multiplies by 2)
		}
		Out.ContactShadowLength = CSLength * (bLengthInWS ? -1.0f : 1.0f);  // Sign encodes WS/SS
	}
	
	// Apply indirect lighting scale for reflection captures
	if (View.bIsReflectionCapture)
	{
		Out.LightParameters.Color *= LightSceneInfo.Proxy->GetIndirectLightingScale();
	}
	
	// Apply light fade factor
	const ELightComponentType LightType = (ELightComponentType)LightSceneInfo.Proxy->GetLightType();
	if ((LightType == LightType_Point || LightType == LightType_Spot || LightType == LightType_Rect) && View.IsPerspectiveProjection())
	{
		Out.LightParameters.Color *= GetLightFadeFactorInternal(View, LightSceneInfo.Proxy);
	}
	
	// Lighting channel mask
	Out.LightingChannelMask = LightSceneInfo.Proxy->GetLightingChannelMask();
	
	// Force falloff exponent to 0 for inverse squared lights
	if (LightSceneInfo.Proxy->IsInverseSquared())
	{
		Out.LightParameters.FalloffExponent = 0.0f;
	}
	
	return Out;
}

//=============================================================================
// CreateDeferredLightUniformBuffer
//=============================================================================

TRDGUniformBufferRef<FDeferredLightUniformWrapper> CreateDeferredLightUniformBuffer(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FLightSceneInfo& LightSceneInfo)
{
	FDeferredLightUniformWrapper* DeferredLightStruct = GraphBuilder.AllocParameters<FDeferredLightUniformWrapper>();
	*DeferredLightStruct = GetDeferredLightParametersWrapper(View, LightSceneInfo, false, 0);
	return GraphBuilder.CreateUniformBuffer(DeferredLightStruct);
}
