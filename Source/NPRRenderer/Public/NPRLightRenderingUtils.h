// NPR Light Rendering Utilities
// Replicates engine functions that are not exported with RENDERER_API

#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RenderResource.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "SceneRendering.h"
#include "LightSceneInfo.h"
#include "ShadowRendering.h"  // For FProjectedShadowInfo

//=============================================================================
// NPR_USE_RECEIVER_BIAS: NoL-based TransitionScale modulation
// Engine ignores this (SHADING_PATH_DEFERRED=0 in ShadowProjectionPixelShader.usf)
// Set to 1 to enable NPR-specific behavior, 0 to match engine (disabled)
//=============================================================================
#ifndef NPR_USE_RECEIVER_BIAS
#define NPR_USE_RECEIVER_BIAS 0
#endif

//=============================================================================
// NPR_CSM_BLEND_CASCADES: Cascade blending in fade regions
// Costs ~4 extra Gather ops in blend regions, but eliminates visible cascade seams
// Set to 1 to enable (default), 0 to disable for performance
//=============================================================================
#ifndef NPR_CSM_BLEND_CASCADES
#define NPR_CSM_BLEND_CASCADES 1
#endif

//=============================================================================
// NPR Stenciling Geometry - Replicates engine's StencilingGeometry
//=============================================================================

namespace NPRStencilingGeometry
{
	// Sphere vertex buffer (18 sides, 12 rings) - same as engine
	class FNPRStencilSphereVertexBuffer : public FVertexBuffer
	{
	public:
		static const int32 NumSides = 18;
		static const int32 NumRings = 12;
		
		int32 GetNumRings() const { return NumRings; }
		int32 GetVertexCount() const { return NumSphereVerts; }
		
		void InitRHI(FRHICommandListBase& RHICmdList) override;
		
		void CalcTransform(FVector4f& OutPosAndScale, const FSphere& Sphere, const FVector& PreViewTranslation, bool bConservativelyBoundSphere = true);
		
	private:
		int32 NumSphereVerts = 0;
	};
	
	// Sphere index buffer
	class FNPRStencilSphereIndexBuffer : public FIndexBuffer
	{
	public:
		static const int32 NumSides = 18;
		static const int32 NumRings = 12;
		
		int32 GetIndexCount() const { return NumIndices; }
		
		void InitRHI(FRHICommandListBase& RHICmdList) override;
		
	private:
		int32 NumIndices = 0;
	};
	
	// Cone for spot lights — matches engine's FStencilConeIndexBuffer/FStencilConeVertexBuffer
	// Vertex positions are all zero; actual geometry is generated procedurally in the vertex shader
	class FNPRStencilConeIndexBuffer : public FIndexBuffer
	{
	public:
		static const int32 NumSides = 18;
		static const int32 NumSlices = 12;
		// Cone body (NumSides * NumSlices) + Spherical cap (NumSides * NumSlices)
		static const uint32 NumVerts = NumSides * NumSlices * 2;
		
		int32 GetIndexCount() const { return NumIndices; }
		
		void InitRHI(FRHICommandListBase& RHICmdList) override;
		
	private:
		int32 NumIndices = 0;
	};
	
	class FNPRStencilConeVertexBuffer : public FVertexBuffer
	{
	public:
		static const int32 NumVerts = FNPRStencilConeIndexBuffer::NumSides * FNPRStencilConeIndexBuffer::NumSlices * 2;
		
		void InitRHI(FRHICommandListBase& RHICmdList) override;
	};
	
	// Global resource instances
	extern TGlobalResource<FNPRStencilSphereVertexBuffer> GNPRStencilSphereVertexBuffer;
	extern TGlobalResource<FNPRStencilSphereIndexBuffer> GNPRStencilSphereIndexBuffer;
	extern TGlobalResource<FNPRStencilConeVertexBuffer> GNPRStencilConeVertexBuffer;
	extern TGlobalResource<FNPRStencilConeIndexBuffer> GNPRStencilConeIndexBuffer;
	
	// Draw functions
	void DrawSphere(FRHICommandList& RHICmdList);
	void DrawCone(FRHICommandList& RHICmdList);
}

//=============================================================================
// NPR Stenciling Geometry Shader Parameters
//=============================================================================

BEGIN_SHADER_PARAMETER_STRUCT(FNPRStencilingGeometryParameters, )
	SHADER_PARAMETER(FVector4f, StencilingGeometryPosAndScale)
	SHADER_PARAMETER(FVector4f, StencilingConeParameters)
	SHADER_PARAMETER(FMatrix44f, StencilingConeTransform)
END_SHADER_PARAMETER_STRUCT()

namespace NPRLightRenderingUtils
{
	// Get stenciling parameters for a light
	FNPRStencilingGeometryParameters GetStencilingGeometryParameters(const FVector4f& InStencilingGeometryPosAndScale);
	FNPRStencilingGeometryParameters GetStencilingGeometryParameters(const FSceneView& View, const FLightSceneInfo* LightSceneInfo);
}

//=============================================================================
// NPR Fullscreen Rectangle Parameters - Replicates FDrawFullScreenRectangleParameters
//=============================================================================

BEGIN_SHADER_PARAMETER_STRUCT(FNPRDrawFullScreenRectangleParameters, )
	SHADER_PARAMETER(FVector4f, PosScaleBias)
	SHADER_PARAMETER(FVector4f, UVScaleBias)
	SHADER_PARAMETER(FVector4f, InvTargetSizeAndTextureSize)
END_SHADER_PARAMETER_STRUCT()

namespace NPRLightRenderingUtils
{
	FNPRDrawFullScreenRectangleParameters GetFullScreenRectParameters(
		float X, float Y,
		float SizeX, float SizeY,
		float U, float V,
		float SizeU, float SizeV,
		FIntPoint TargetSize,
		FIntPoint TextureSize);
}

//=============================================================================
// NPR Deferred Light Vertex Shader - Replicates FDeferredLightVS
//=============================================================================

class FNPRDeferredLightVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FNPRDeferredLightVS);
	SHADER_USE_PARAMETER_STRUCT(FNPRDeferredLightVS, FGlobalShader);
	
	class FRadialLight : SHADER_PERMUTATION_BOOL("SHADER_RADIAL_LIGHT");
	using FPermutationDomain = TShaderPermutationDomain<FRadialLight>;
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FNPRDrawFullScreenRectangleParameters, FullScreenRect)
		SHADER_PARAMETER_STRUCT_INCLUDE(FNPRStencilingGeometryParameters, Geometry)
	END_SHADER_PARAMETER_STRUCT()
	
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
	
	// Replicated GetParameters functions
	static FParameters GetParameters(const FViewInfo& View, bool bBindViewUniform = true);
	static FParameters GetParameters(const FViewInfo& View, const FLightSceneInfo* LightSceneInfo, bool bBindViewUniform = true);
	static FParameters GetParameters(const FViewInfo& View,
		float X, float Y,
		float SizeX, float SizeY,
		float U, float V,
		float SizeU, float SizeV,
		FIntPoint TargetSize,
		FIntPoint TextureSize,
		bool bBindViewUniform = true);
};

//=============================================================================
// NPR Light Visibility Helper - Replicates FLightSceneInfo::ShouldRenderLight
//=============================================================================

namespace NPRLightRenderingUtils
{
	bool ShouldRenderLight(const FLightSceneInfo* LightSceneInfo, const FViewInfo& View, bool bOffscreen = false);
}

//=============================================================================
// Shadow Helper Functions
//=============================================================================

namespace NPRLightRenderingUtils
{
	// Helper to get shadow info for a light from VisibleLightInfos
	const FProjectedShadowInfo* GetFirstWholeSceneShadowForLight(
		const FVisibleLightInfo& VisibleLightInfo);
	
	// Get all CSM cascades for directional light, sorted by cascade index
	// Returns number of cascades found (up to MaxCascades)
	int32 GetAllWholeSceneDirectionalShadows(
		const FVisibleLightInfo& VisibleLightInfo,
		TArray<const FProjectedShadowInfo*, TInlineAllocator<4>>& OutCascades);
	
	//=========================================================================
	// Replicated engine functions that are not exported with RENDERER_API
	//=========================================================================
	
	// Replicate FProjectedShadowInfo::GetScreenToShadowMatrix (not exported)
	// Returns the matrix that transforms screen position (with ViewZ as Z) to shadow space
	// Same as engine's ShadowProjectionPixelShader.usf approach
	FMatrix GetScreenToShadowMatrix(const FProjectedShadowInfo* ShadowInfo, const FViewInfo& View);
	
#if NPR_USE_RECEIVER_BIAS
	// Replicate FProjectedShadowInfo::GetShaderReceiverDepthBias (not exported)
	// Returns the receiver depth bias based on light type
	// Note: Engine ignores this (SHADING_PATH_DEFERRED=0 in shadow projection shader)
	float GetShaderReceiverDepthBias(const FProjectedShadowInfo* ShadowInfo);
#endif
	
	// Replicate FProjectedShadowInfo::ComputeTransitionSize (not exported)
	// Returns the soft PCF transition size for shadow depth comparison
	// SoftTransitionScale = 1.0f / ComputeTransitionSize()
	float ComputeTransitionSize(const FProjectedShadowInfo* ShadowInfo);
}
