// Wrapper for FDeferredLightUniformStruct to enable RDG usage from plugins.
// GetStructMetadata() looks up engine's registered metadata at runtime via FindUniformBufferStructByName.
// Wrapper pattern for using engine's uniform buffer types from plugins without linker errors

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ShaderParameterMacros.h"
#include "ShaderParameterStruct.h"
#include "SceneRendering.h"
#include "SceneManagement.h"  // For FLightShaderParameters
#include "LightRendering.h"   // For FDeferredLightUniformStruct

/**
 * Inherits from FDeferredLightUniformStruct with identical memory layout.
 * GetStructMetadata() delegates to engine's pre-registered metadata via runtime lookup.
 * 
 * This avoids:
 * - Linker errors for FDeferredLightUniformStruct::GetStructMetadata() (not exported)
 * - Nested struct (FLightShaderParameters) resolution issues with DataDrivenUniformBuffer
 */
class FDeferredLightUniformWrapper : public FDeferredLightUniformStruct
{
public:
	struct FTypeInfo
	{
		static constexpr int32 NumRows = 1;
		static constexpr int32 NumColumns = 1;
		static constexpr int32 NumElements = 0;
		static constexpr int32 Alignment = SHADER_PARAMETER_STRUCT_ALIGNMENT;
		static constexpr bool bIsStoredInConstantBuffer = true;
		static constexpr const ANSICHAR* const FileName = __FILE__;
		static constexpr int32 FileLine = __LINE__;
		using TAlignedType = FDeferredLightUniformWrapper;
		
		static const FShaderParametersMetadata* GetStructMetadata();
	};
	
	static FUniformBufferRHIRef CreateUniformBuffer(
		const FDeferredLightUniformWrapper& InContents, 
		EUniformBufferUsage InUsage)
	{
		return RHICreateUniformBuffer(
			&InContents, 
			FTypeInfo::GetStructMetadata()->GetLayoutPtr(), 
			InUsage);
	}
};

// Forward declaration for RDG template specialization
const FShaderParametersMetadata* GetForwardDeclaredShaderParametersStructMetadata(
	const FDeferredLightUniformWrapper*);

// Populate wrapper with light parameters (replicated from engine's GetDeferredLightParameters)
FDeferredLightUniformWrapper GetDeferredLightParametersWrapper(
	const FSceneView& View,
	const FLightSceneInfo& LightSceneInfo,
	bool bUseLightFunctionAtlas = false,
	uint32 LightFlags = 0);

// Create RDG uniform buffer for deferred light
TRDGUniformBufferRef<FDeferredLightUniformWrapper> CreateDeferredLightUniformBuffer(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FLightSceneInfo& LightSceneInfo);
