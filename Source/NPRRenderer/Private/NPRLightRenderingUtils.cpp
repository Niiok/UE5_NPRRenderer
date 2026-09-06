// NPR Light Rendering Utilities Implementation
// Replicates engine functions that are not exported with RENDERER_API

#include "NPRLightRenderingUtils.h"  // Now in Public folder
#include "NPRRendererModule.h"
#include "RHIResourceUtils.h"
#include "ScenePrivate.h"
#include "SceneManagement.h"  // For FLightRenderParameters
#include "HAL/IConsoleManager.h"

//=============================================================================
// Shader Implementation
//=============================================================================

IMPLEMENT_GLOBAL_SHADER(FNPRDeferredLightVS, "/Engine/Private/DeferredLightVertexShaders.usf", "VertexMain", SF_Vertex);

bool FNPRDeferredLightVS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FNPRDeferredLightVS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
}

//=============================================================================
// NPR Stenciling Geometry - Sphere Vertex Buffer
//=============================================================================

void NPRStencilingGeometry::FNPRStencilSphereVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	const int32 NumVerts = (NumSides + 1) * (NumRings + 1);
	const float RadiansPerRingSegment = UE_PI / (float)NumRings;
	float Radius = 1.0f;
	
	TArray<FVector4f> ArcVerts;
	ArcVerts.Empty(NumRings + 1);
	for (int32 i = 0; i < NumRings + 1; i++)
	{
		const float Angle = i * RadiansPerRingSegment;
		ArcVerts.Add(FVector4f(0.0f, FMath::Sin(Angle), FMath::Cos(Angle), 0.0f));
	}
	
	TArray<FVector4f> Verts;
	Verts.Empty(NumVerts);
	const FVector3f Center = FVector3f(0, 0, 0);
	for (int32 s = 0; s < NumSides + 1; s++)
	{
		FRotator3f ArcRotator(0, 360.f * ((float)s / NumSides), 0);
		FRotationMatrix44f ArcRot(ArcRotator);
		
		for (int32 v = 0; v < NumRings + 1; v++)
		{
			FVector3f ArcVert = FVector3f(ArcVerts[v].X, ArcVerts[v].Y, ArcVerts[v].Z);
			Verts.Add(FVector4f(Center + Radius * ArcRot.TransformPosition(ArcVert), 1.0f));
		}
	}
	
	NumSphereVerts = Verts.Num();
	VertexBufferRHI = UE::RHIResourceUtils::CreateVertexBufferFromArray(RHICmdList, TEXT("NPRStencilSphereVB"), EBufferUsageFlags::Static, MakeConstArrayView(Verts));
}

void NPRStencilingGeometry::FNPRStencilSphereVertexBuffer::CalcTransform(FVector4f& OutPosAndScale, const FSphere& Sphere, const FVector& PreViewTranslation, bool bConservativelyBoundSphere)
{
	float Radius = Sphere.W;
	if (bConservativelyBoundSphere)
	{
		const float RadiansPerRingSegment = UE_PI / (float)NumRings;
		Radius /= FMath::Cos(RadiansPerRingSegment);
	}
	
	const FVector3f Translate(Sphere.Center + PreViewTranslation);
	OutPosAndScale = FVector4f(Translate, Radius);
}

//=============================================================================
// NPR Stenciling Geometry - Sphere Index Buffer
//=============================================================================

void NPRStencilingGeometry::FNPRStencilSphereIndexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	TArray<uint16> Indices;
	Indices.Reserve(NumSides * NumRings * 6);
	
	for (int32 s = 0; s < NumSides; s++)
	{
		const int32 a0start = (s + 0) * (NumRings + 1);
		const int32 a1start = (s + 1) * (NumRings + 1);
		
		for (int32 r = 0; r < NumRings; r++)
		{
			Indices.Add(a0start + r + 0);
			Indices.Add(a1start + r + 0);
			Indices.Add(a0start + r + 1);
			Indices.Add(a1start + r + 0);
			Indices.Add(a1start + r + 1);
			Indices.Add(a0start + r + 1);
		}
	}
	
	NumIndices = Indices.Num();
	IndexBufferRHI = UE::RHIResourceUtils::CreateIndexBufferFromArray(RHICmdList, TEXT("NPRStencilSphereIB"), EBufferUsageFlags::Static, MakeConstArrayView(Indices));
}

//=============================================================================
// NPR Stenciling Geometry - Cone Vertex Buffer
// Matches engine's FStencilConeVertexBuffer (LightRendering.cpp)
// All vertices are zero — actual geometry is generated procedurally in the vertex shader
//=============================================================================

void NPRStencilingGeometry::FNPRStencilConeVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	TArray<FVector4f> Verts;
	Verts.Empty(NumVerts);
	for (int32 s = 0; s < NumVerts; s++)
	{
		Verts.Emplace(0, 0, 0, 0);
	}
	VertexBufferRHI = UE::RHIResourceUtils::CreateVertexBufferFromArray(RHICmdList, TEXT("NPRStencilConeVB"), EBufferUsageFlags::Static, MakeConstArrayView(Verts));
}

//=============================================================================
// NPR Stenciling Geometry - Cone Index Buffer
// Matches engine's FStencilConeIndexBuffer (LightRendering.cpp)
// Two grids: cone body (0..NumSides*NumSlices-1) + spherical cap (NumSides*NumSlices..NumVerts-1)
//=============================================================================

void NPRStencilingGeometry::FNPRStencilConeIndexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	TArray<uint16> Indices;
	Indices.Empty((NumSlices - 1) * NumSides * 12);

	// Generate triangles for the cone body vertices
	for (int32 SliceIndex = 0; SliceIndex < NumSlices - 1; SliceIndex++)
	{
		for (int32 SideIndex = 0; SideIndex < NumSides; SideIndex++)
		{
			const int32 CurrentIndex = SliceIndex * NumSides + SideIndex % NumSides;
			const int32 NextSideIndex = SliceIndex * NumSides + (SideIndex + 1) % NumSides;
			const int32 NextSliceIndex = (SliceIndex + 1) * NumSides + SideIndex % NumSides;
			const int32 NextSliceAndSideIndex = (SliceIndex + 1) * NumSides + (SideIndex + 1) % NumSides;

			Indices.Add(CurrentIndex);
			Indices.Add(NextSideIndex);
			Indices.Add(NextSliceIndex);
			Indices.Add(NextSliceIndex);
			Indices.Add(NextSideIndex);
			Indices.Add(NextSliceAndSideIndex);
		}
	}

	// Generate triangles for the spherical cap vertices (offset by NumSides * NumSlices)
	// NOTE: Winding order is intentionally reversed vs cone body (engine LightRendering.cpp:359-374)
	// Body uses (Current, NextSide, NextSlice) but cap uses (Current, NextSlice, NextSide)
	// This ensures outward-facing normals for the cap's different orientation
	const int32 CapIndexStart = NumSides * NumSlices;
	for (int32 SliceIndex = 0; SliceIndex < NumSlices - 1; SliceIndex++)
	{
		for (int32 SideIndex = 0; SideIndex < NumSides; SideIndex++)
		{
			const int32 CurrentIndex = SliceIndex * NumSides + SideIndex % NumSides + CapIndexStart;
			const int32 NextSideIndex = SliceIndex * NumSides + (SideIndex + 1) % NumSides + CapIndexStart;
			const int32 NextSliceIndex = (SliceIndex + 1) * NumSides + SideIndex % NumSides + CapIndexStart;
			const int32 NextSliceAndSideIndex = (SliceIndex + 1) * NumSides + (SideIndex + 1) % NumSides + CapIndexStart;

			Indices.Add(CurrentIndex);
			Indices.Add(NextSliceIndex);
			Indices.Add(NextSideIndex);
			Indices.Add(NextSideIndex);
			Indices.Add(NextSliceIndex);
			Indices.Add(NextSliceAndSideIndex);
		}
	}

	NumIndices = Indices.Num();
	IndexBufferRHI = UE::RHIResourceUtils::CreateIndexBufferFromArray(RHICmdList, TEXT("NPRStencilConeIB"), EBufferUsageFlags::Static, MakeConstArrayView(Indices));
}

//=============================================================================
// NPR Stenciling Geometry - Global Resources
//=============================================================================

TGlobalResource<NPRStencilingGeometry::FNPRStencilSphereVertexBuffer> NPRStencilingGeometry::GNPRStencilSphereVertexBuffer;
TGlobalResource<NPRStencilingGeometry::FNPRStencilSphereIndexBuffer> NPRStencilingGeometry::GNPRStencilSphereIndexBuffer;
TGlobalResource<NPRStencilingGeometry::FNPRStencilConeVertexBuffer> NPRStencilingGeometry::GNPRStencilConeVertexBuffer;
TGlobalResource<NPRStencilingGeometry::FNPRStencilConeIndexBuffer> NPRStencilingGeometry::GNPRStencilConeIndexBuffer;

//=============================================================================
// NPR Stenciling Geometry - Draw Functions
//=============================================================================

void NPRStencilingGeometry::DrawSphere(FRHICommandList& RHICmdList)
{
	RHICmdList.SetStreamSource(0, GNPRStencilSphereVertexBuffer.VertexBufferRHI, 0);
	RHICmdList.DrawIndexedPrimitive(
		GNPRStencilSphereIndexBuffer.IndexBufferRHI,
		0, 0,
		GNPRStencilSphereVertexBuffer.GetVertexCount(),
		0,
		GNPRStencilSphereIndexBuffer.GetIndexCount() / 3,
		1);
}

void NPRStencilingGeometry::DrawCone(FRHICommandList& RHICmdList)
{
	RHICmdList.SetStreamSource(0, GNPRStencilConeVertexBuffer.VertexBufferRHI, 0);
	RHICmdList.DrawIndexedPrimitive(
		GNPRStencilConeIndexBuffer.IndexBufferRHI,
		0, 0,
		FNPRStencilConeIndexBuffer::NumVerts,
		0,
		GNPRStencilConeIndexBuffer.GetIndexCount() / 3,
		1);
}

//=============================================================================
// NPR Stenciling Geometry Shader Parameters
//=============================================================================

FNPRStencilingGeometryParameters NPRLightRenderingUtils::GetStencilingGeometryParameters(const FVector4f& InStencilingGeometryPosAndScale)
{
	FNPRStencilingGeometryParameters Out;
	Out.StencilingGeometryPosAndScale = InStencilingGeometryPosAndScale;
	Out.StencilingConeParameters = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	Out.StencilingConeTransform = FMatrix44f::Identity;
	return Out;
}

FNPRStencilingGeometryParameters NPRLightRenderingUtils::GetStencilingGeometryParameters(const FSceneView& View, const FLightSceneInfo* LightSceneInfo)
{
	FNPRStencilingGeometryParameters Out;
	
	if (LightSceneInfo->Proxy->GetLightType() == LightType_Point ||
		LightSceneInfo->Proxy->GetLightType() == LightType_Rect)
	{
		NPRStencilingGeometry::GNPRStencilSphereVertexBuffer.CalcTransform(
			Out.StencilingGeometryPosAndScale,
			LightSceneInfo->Proxy->GetBoundingSphere(),
			View.ViewMatrices.GetPreViewTranslation());
		Out.StencilingConeParameters = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		Out.StencilingConeTransform = FMatrix44f::Identity;
	}
	else if (LightSceneInfo->Proxy->GetLightType() == LightType_Spot)
	{
		const FMatrix WorldToTranslatedWorld = FTranslationMatrix(View.ViewMatrices.GetPreViewTranslation());
		Out.StencilingGeometryPosAndScale = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		Out.StencilingConeTransform = FMatrix44f(LightSceneInfo->Proxy->GetLightToWorld() * WorldToTranslatedWorld);
		Out.StencilingConeParameters = FVector4f(
			NPRStencilingGeometry::FNPRStencilConeIndexBuffer::NumSides,
			NPRStencilingGeometry::FNPRStencilConeIndexBuffer::NumSlices,
			LightSceneInfo->Proxy->GetOuterConeAngle(),
			LightSceneInfo->Proxy->GetRadius());
	}
	
	return Out;
}

//=============================================================================
// Fullscreen Rectangle Parameters
//=============================================================================

FNPRDrawFullScreenRectangleParameters NPRLightRenderingUtils::GetFullScreenRectParameters(
	float X, float Y,
	float SizeX, float SizeY,
	float U, float V,
	float SizeU, float SizeV,
	FIntPoint TargetSize,
	FIntPoint TextureSize)
{
	FNPRDrawFullScreenRectangleParameters Out;
	
	Out.PosScaleBias = FVector4f(SizeX, SizeY, X, Y);
	Out.UVScaleBias = FVector4f(SizeU, SizeV, U, V);
	Out.InvTargetSizeAndTextureSize = FVector4f(
		1.0f / TargetSize.X, 1.0f / TargetSize.Y,
		1.0f / TextureSize.X, 1.0f / TextureSize.Y);
	
	return Out;
}

//=============================================================================
// FNPRDeferredLightVS::GetParameters
//=============================================================================

FNPRDeferredLightVS::FParameters FNPRDeferredLightVS::GetParameters(const FViewInfo& View, bool bBindViewUniform)
{
	FParameters Out;
	if (bBindViewUniform)
	{
		Out.View = View.ViewUniformBuffer;
	}
	Out.Geometry = NPRLightRenderingUtils::GetStencilingGeometryParameters(FVector4f(0, 0, 0, 0));
	Out.FullScreenRect = NPRLightRenderingUtils::GetFullScreenRectParameters(
		0, 0,
		View.ViewRect.Width(), View.ViewRect.Height(),
		View.ViewRect.Min.X, View.ViewRect.Min.Y,
		View.ViewRect.Width(), View.ViewRect.Height(),
		View.ViewRect.Size(),
		View.GetSceneTexturesConfig().Extent);
	
	return Out;
}

FNPRDeferredLightVS::FParameters FNPRDeferredLightVS::GetParameters(const FViewInfo& View, const FLightSceneInfo* LightSceneInfo, bool bBindViewUniform)
{
	FParameters Out;
	if (bBindViewUniform)
	{
		Out.View = View.ViewUniformBuffer;
	}
	Out.Geometry = NPRLightRenderingUtils::GetStencilingGeometryParameters(View, LightSceneInfo);
	Out.FullScreenRect = NPRLightRenderingUtils::GetFullScreenRectParameters(0, 0, 0, 0, 0, 0, 0, 0, FIntPoint(1, 1), FIntPoint(1, 1));
	return Out;
}

FNPRDeferredLightVS::FParameters FNPRDeferredLightVS::GetParameters(const FViewInfo& View,
	float X, float Y,
	float SizeX, float SizeY,
	float U, float V,
	float SizeU, float SizeV,
	FIntPoint TargetSize,
	FIntPoint TextureSize,
	bool bBindViewUniform)
{
	FParameters Out;
	if (bBindViewUniform)
	{
		Out.View = View.ViewUniformBuffer;
	}
	Out.Geometry = NPRLightRenderingUtils::GetStencilingGeometryParameters(FVector4f(0, 0, 0, 0));
	Out.FullScreenRect = NPRLightRenderingUtils::GetFullScreenRectParameters(X, Y, SizeX, SizeY, U, V, SizeU, SizeV, TargetSize, TextureSize);
	return Out;
}

//=============================================================================
// ShouldRenderLight - Replicated from LightSceneInfo.cpp
//=============================================================================

// Helper: Check if light is in desired scene subset (same as engine's IsInDesiredSceneSubset)
static bool IsInDesiredSceneSubset(const FViewInfo& View, FLightSceneProxy* Proxy)
{
	if (!View.bStaticSceneOnly)
	{
		return true;
	}
	// For static scene only views (e.g., reflection captures), only render non-movable lights
	return !Proxy->IsMovable();
}

bool NPRLightRenderingUtils::ShouldRenderLight(const FLightSceneInfo* LightSceneInfo, const FViewInfo& View, bool bOffscreen)
{
	if (!LightSceneInfo || !LightSceneInfo->Proxy)
	{
		return false;
	}
	
	FLightSceneProxy* Proxy = LightSceneInfo->Proxy;
	
	// Check if light is in view frustum
	bool bLocalVisible = LightSceneInfo->bVisible;
	
	// Check visibility based on Id bounds
	if (LightSceneInfo->Id < View.VisibleLightInfos.Num())
	{
		bLocalVisible = bLocalVisible && (bOffscreen 
			? View.VisibleLightInfos[LightSceneInfo->Id].bInDrawRange 
			: View.VisibleLightInfos[LightSceneInfo->Id].bInViewFrustum);
	}
	
#if !UE_BUILD_SHIPPING
	// Check show flags
	switch (Proxy->GetLightType())
	{
		case LightType_Directional:
			if (!View.Family->EngineShowFlags.DirectionalLights)
			{
				bLocalVisible = false;
			}
			break;
		case LightType_Point:
			if (!View.Family->EngineShowFlags.PointLights)
			{
				bLocalVisible = false;
			}
			break;
		case LightType_Spot:
			if (!View.Family->EngineShowFlags.SpotLights)
			{
				bLocalVisible = false;
			}
			break;
		case LightType_Rect:
			if (!View.Family->EngineShowFlags.RectLights)
			{
				bLocalVisible = false;
			}
			break;
	}
#endif
	
	// Skip lights with zero color (same as engine's ShouldRenderLightViewIndependent)
	if (Proxy->GetColor().IsAlmostBlack())
	{
		bLocalVisible = false;
	}
	
	// Static (fully baked) lights are handled by lightmaps — render only Stationary/Movable. The engine
	// also renders unbuilt static lights (IsPrecomputedLightingValid()), but that isn't exported, so an
	// unbuilt static light won't preview under NPR.
	if (Proxy->HasStaticLighting())
	{
		bLocalVisible = false;
	}
	
	// === Additional engine checks (same as LightSceneInfo.cpp ShouldRenderLight) ===
	
	// Static scene subset check - for reflection captures, only render non-movable lights
	if (!IsInDesiredSceneSubset(View, Proxy))
	{
		bLocalVisible = false;
	}
	
	// View lighting channel mask check - lights can specify which views they affect
	if (!(Proxy->GetViewLightingChannelMask() & View.ViewLightingChannelMask))
	{
		bLocalVisible = false;
	}
	
	// Lighting channel check - only render lights in default channel, or if there are primitives outside default channel
	if (!(Proxy->GetLightingChannelMask() & GetDefaultLightingChannelMask() || View.bUsesLightingChannels || bOffscreen))
	{
		bLocalVisible = false;
	}
	
	return bLocalVisible;
}

//=============================================================================
// GetFirstWholeSceneShadowForLight - Helper to find shadow info for a light
// For CSM, prioritizes cascade 0 (nearest cascade) which has the highest resolution
// for close objects. This matches how engine renders directional light shadows.
//=============================================================================

const FProjectedShadowInfo* NPRLightRenderingUtils::GetFirstWholeSceneShadowForLight(
	const FVisibleLightInfo& VisibleLightInfo)
{
	const FProjectedShadowInfo* BestShadow = nullptr;
	int32 BestCascadeIndex = INT_MAX;
	
	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];

		if (ProjectedShadowInfo->bAllocated
			&& ProjectedShadowInfo->bWholeSceneShadow
			&& !ProjectedShadowInfo->bRayTracedDistanceField)
		{
			// For CSM, prefer cascade 0 (nearest, highest resolution)
			const int32 CascadeIndex = ProjectedShadowInfo->CascadeSettings.ShadowSplitIndex;
			if (CascadeIndex < BestCascadeIndex)
			{
				BestCascadeIndex = CascadeIndex;
				BestShadow = ProjectedShadowInfo;
			}
		}
	}
	return BestShadow;
}

//=============================================================================
// GetAllWholeSceneDirectionalShadows - Get all CSM cascades sorted by index
// Returns cascades in order from nearest (cascade 0) to farthest
//=============================================================================

int32 NPRLightRenderingUtils::GetAllWholeSceneDirectionalShadows(
	const FVisibleLightInfo& VisibleLightInfo,
	TArray<const FProjectedShadowInfo*, TInlineAllocator<4>>& OutCascades)
{
	OutCascades.Reset();
	
	// Collect all whole-scene directional shadows (CSM cascades)
	TArray<const FProjectedShadowInfo*, TInlineAllocator<4>> TempCascades;
	
	for (int32 ShadowIndex = 0; ShadowIndex < VisibleLightInfo.ShadowsToProject.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = VisibleLightInfo.ShadowsToProject[ShadowIndex];

		if (ProjectedShadowInfo->bAllocated
			&& ProjectedShadowInfo->bWholeSceneShadow
			&& ProjectedShadowInfo->bDirectionalLight
			&& !ProjectedShadowInfo->bRayTracedDistanceField
			&& ProjectedShadowInfo->CascadeSettings.ShadowSplitIndex >= 0)  // Valid CSM cascade
		{
			TempCascades.Add(ProjectedShadowInfo);
		}
	}
	
	// Sort by cascade index (nearest first)
	// Note: TArray::Sort with pointers dereferences them, so lambda receives references
	TempCascades.Sort([](const FProjectedShadowInfo& A, const FProjectedShadowInfo& B)
	{
		return A.CascadeSettings.ShadowSplitIndex < B.CascadeSettings.ShadowSplitIndex;
	});
	
	// Copy to output (up to 4 cascades)
	const int32 MaxCascades = 4;
	for (int32 i = 0; i < TempCascades.Num() && i < MaxCascades; i++)
	{
		OutCascades.Add(TempCascades[i]);
	}
	
	return OutCascades.Num();
}

//=============================================================================
// GetScreenToShadowMatrix - ScreenPosition to Shadow UV
// Same as engine's FProjectedShadowInfo::GetScreenToShadowMatrix
// Shader must use ScreenPosition for transformation
//=============================================================================

FMatrix NPRLightRenderingUtils::GetScreenToShadowMatrix(const FProjectedShadowInfo* ShadowInfo, const FViewInfo& View)
{
	if (!ShadowInfo)
	{
		return FMatrix::Identity;
	}
	
	const FIntPoint ShadowBufferResolution = ShadowInfo->GetShadowBufferResolution();
	const float InvBufferResolutionX = 1.0f / (float)ShadowBufferResolution.X;
	const float ShadowResolutionFractionX = 0.5f * (float)ShadowInfo->ResolutionX * InvBufferResolutionX;
	const float InvBufferResolutionY = 1.0f / (float)ShadowBufferResolution.Y;
	const float ShadowResolutionFractionY = 0.5f * (float)ShadowInfo->ResolutionY * InvBufferResolutionY;
	
	// ViewDependentTransform: ScreenPosition -> WorldPosition
	// Same as engine's ShadowRendering.cpp line 1727-1736
	FMatrix ViewDependentTransform =
		// Z of the position being transformed is actually view space Z, 
		// Transform it into post projection space by applying the projection matrix,
		// Which is the required space before applying View.InvTranslatedViewProjectionMatrix
		View.ViewMatrices.GetScreenToClipMatrix() *
		// Transform the post projection space position into translated world space
		// Translated world space is normal world space translated to the view's origin, 
		// Which prevents floating point imprecision far from the world origin.
		View.ViewMatrices.GetInvTranslatedViewProjectionMatrix() *
		FTranslationMatrix(-View.ViewMatrices.GetPreViewTranslation());
	
	// ShadowMapDependentTransform: WorldPosition -> Shadow UV
	// Same as engine's ShadowRendering.cpp line 1738-1756
	FMatrix ShadowMapDependentTransform =
		// Translate to the origin of the shadow's translated world space
		FTranslationMatrix(ShadowInfo->PreShadowTranslation) *
		// Transform into the shadow's post projection space
		// This has to be the same transform used to render the shadow depths
		FMatrix(ShadowInfo->TranslatedWorldToClipInnerMatrix) *
		// Scale and translate x and y to be texture coordinates into the ShadowInfo's rectangle in the shadow depth buffer
		// Normalize z by MaxSubjectDepth, as was done when writing shadow depths
		FMatrix(
			FPlane(ShadowResolutionFractionX, 0, 0, 0),
			FPlane(0, -ShadowResolutionFractionY, 0, 0),
			FPlane(0, 0, ShadowInfo->InvMaxSubjectDepth, 0),
			FPlane(
				(ShadowInfo->X + ShadowInfo->BorderSize) * InvBufferResolutionX + ShadowResolutionFractionX,
				(ShadowInfo->Y + ShadowInfo->BorderSize) * InvBufferResolutionY + ShadowResolutionFractionY,
				0,
				1
			)
		);
	
	return ViewDependentTransform * ShadowMapDependentTransform;
}

//=============================================================================
// Helper to get engine CVar values
// Used by ComputeTransitionSize and GetShaderReceiverDepthBias
//=============================================================================
static float GetEngineCVarFloat(const TCHAR* CVarName, float DefaultValue)
{
	// No caching: a shared static cache would be a render-thread data race, and FindConsoleVariable is a
	// cheap hash lookup called at most per-shadow.
	const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(CVarName);
	return CVar ? CVar->GetFloat() : DefaultValue;
}

//=============================================================================
// GetShaderReceiverDepthBias - Replicated from ShadowRendering.cpp
// FProjectedShadowInfo::GetShaderReceiverDepthBias is not exported with RENDERER_API
// Uses engine's CVars directly (r.Shadow.CSMReceiverBias, etc.)
// Note: Engine ignores this (SHADING_PATH_DEFERRED=0 in ShadowProjectionPixelShader.usf)
//=============================================================================
#if NPR_USE_RECEIVER_BIAS
float NPRLightRenderingUtils::GetShaderReceiverDepthBias(const FProjectedShadowInfo* ShadowInfo)
{
	if (!ShadowInfo)
	{
		return 0.0f;
	}
	
	float ShadowReceiverBias = 0.0f;
	
	// Access the light type through GetLightSceneInfo() which is public
	// Explicit cast required as GetLightType() returns uint8 (same as engine's VolumeLighting.cpp)
	const ELightComponentType LightType = (ELightComponentType)ShadowInfo->GetLightSceneInfo().Proxy->GetLightType();
	
	// Use engine's CVars directly (same as ShadowRendering.cpp line 1970-1977)
	switch (LightType)
	{
		case LightType_Directional:
			// Engine default: 0.9f (ShadowRendering.cpp line 74-77)
			ShadowReceiverBias = GetEngineCVarFloat(TEXT("r.Shadow.CSMReceiverBias"), 0.9f);
			break;
		case LightType_Rect:
			// Engine default: 0.0f
			ShadowReceiverBias = GetEngineCVarFloat(TEXT("r.Shadow.RectLightReceiverBias"), 0.0f);
			break;
		case LightType_Spot:
			// Engine default: 0.0f
			ShadowReceiverBias = GetEngineCVarFloat(TEXT("r.Shadow.SpotLightReceiverBias"), 0.0f);
			break;
		case LightType_Point:
			// Point lights use slope depth bias as receiver bias (same as engine)
			ShadowReceiverBias = ShadowInfo->GetShaderSlopeDepthBias();
			break;
		default:
			ShadowReceiverBias = 0.0f;
			break;
	}
	
	// Return the min lerp value for depth biasing
	// 0 : max bias when NoL == 0
	// 1 : no bias
	// Same as engine's ShadowRendering.cpp line 1980-1983
	return 1.0f - FMath::Clamp(ShadowReceiverBias, 0.0f, 1.0f);
}
#endif // NPR_USE_RECEIVER_BIAS

//=============================================================================
// ComputeTransitionSize - Replicated from ShadowRendering.cpp
// FProjectedShadowInfo::ComputeTransitionSize is not exported with RENDERER_API
// Uses engine CVars directly: r.Shadow.CSMDepthBias, r.Shadow.TransitionScale
//=============================================================================

float NPRLightRenderingUtils::ComputeTransitionSize(const FProjectedShadowInfo* ShadowInfo)
{
	if (!ShadowInfo)
	{
		return 1.0f;
	}
	
	float TransitionSize = 1.0f;
	
	const FLightSceneInfo* LightSceneInfo = &ShadowInfo->GetLightSceneInfo();
	const FLightSceneProxy* Proxy = LightSceneInfo ? LightSceneInfo->Proxy : nullptr;
	const float UserShadowBias = Proxy ? Proxy->GetUserShadowBias() : 1.0f;
	
	// Check if this is a whole-scene directional shadow (CSM)
	// Same logic as engine's IsWholeSceneDirectionalShadow() - includes bDirectionalLight check
	const bool bIsWholeSceneDirectionalShadow = 
		ShadowInfo->bWholeSceneShadow && 
		ShadowInfo->CascadeSettings.ShadowSplitIndex >= 0 &&
		ShadowInfo->bDirectionalLight;
	
	// Check if this is a whole-scene point light shadow
	// Same logic as engine's IsWholeScenePointLightShadow()
	const bool bIsWholeScenePointLightShadow = 
		ShadowInfo->bWholeSceneShadow && 
		(ShadowInfo->CascadeSettings.ShadowSplitIndex == INDEX_NONE) && 
		ShadowInfo->bOnePassPointLightShadow;
	
	if (bIsWholeScenePointLightShadow)
	{
		// Point/Spot lights use fixed transition scale
		// TransitionSize = 1/CVarShadowTransitionScale * 2 * UserShadowBias
		// Use engine's r.Shadow.TransitionScale (default 60.0f)
		const float ShadowTransitionScale = GetEngineCVarFloat(TEXT("r.Shadow.TransitionScale"), 60.0f);
		TransitionSize = (1.0f / ShadowTransitionScale) * 2.0f * UserShadowBias;
	}
	else if (bIsWholeSceneDirectionalShadow)
	{
		// CSM uses depth-range-based transition (same as engine ShadowRendering.cpp line 1932-1937)
		const float DepthRange = ShadowInfo->MaxSubjectZ - ShadowInfo->MinSubjectZ;
		if (DepthRange > KINDA_SMALL_NUMBER)
		{
			// Use engine's r.Shadow.CSMDepthBias (default 10.0f)
			const float CSMShadowDepthBias = GetEngineCVarFloat(TEXT("r.Shadow.CSMDepthBias"), 10.0f);
			TransitionSize = CSMShadowDepthBias / DepthRange;
			
			// WorldSpaceTexelScale = ShadowBounds.W / ResolutionX
			const float WorldSpaceTexelScale = ShadowInfo->ShadowBounds.W / (float)ShadowInfo->ResolutionX;
			
			// Engine line 1936: TransitionSize *= WorldSpaceTexelScale;
			// Note: CascadeBiasDistribution is used in UpdateShaderDepthBias (DepthBias), NOT here
			TransitionSize *= WorldSpaceTexelScale;
			
			TransitionSize *= UserShadowBias;
		}
	}
	else if (ShadowInfo->bPreShadow)
	{
		// Preshadows don't have self shadowing
		TransitionSize = 0.0f;
	}
	else
	{
		// Per-object shadows - same logic as engine line 1946-1949
		const float ShadowTransitionScale = ShadowInfo->bDirectionalLight
			? GetEngineCVarFloat(TEXT("r.Shadow.TransitionScale"), 60.0f)
			: GetEngineCVarFloat(TEXT("r.Shadow.SpotLightTransitionScale"), 60.0f);
		TransitionSize = (1.0f / ShadowTransitionScale) * 2.0f * UserShadowBias;
	}
	
	// Ensure TransitionSize is non-zero to prevent shader divide by zero
	// Same as engine line 1952-1954
	const float MinTransitionSize = 0.00001f;
	return FMath::Max(TransitionSize, MinTransitionSize);
}
