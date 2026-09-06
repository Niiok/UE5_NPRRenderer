// Wrapper for FReflectionUniformParameters ("ReflectionStruct") to enable RDG binding from plugins.
// GetStructMetadata() looks up the engine's registered metadata at runtime via FindUniformBufferStructByName,
// avoiding linker errors for the non-exported FReflectionUniformParameters::GetStructMetadata().

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ShaderParameterMacros.h"
#include "ShaderParameterStruct.h"
#include "ReflectionEnvironment.h"  // FReflectionUniformParameters

class FReflectionUniformWrapper : public FReflectionUniformParameters
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
		using TAlignedType = FReflectionUniformWrapper;

		static const FShaderParametersMetadata* GetStructMetadata();
	};
};

// Forward declaration for RDG template specialization
const FShaderParametersMetadata* GetForwardDeclaredShaderParametersStructMetadata(
	const FReflectionUniformWrapper*);
