// Wrapper for FReflectionCaptureShaderData ("ReflectionCaptureSM5") to enable binding from plugins.
// GetStructMetadata() looks up the engine's registered metadata at runtime via FindUniformBufferStructByName.
// This is a plain constant uniform buffer (no RDG resources) -> bind via SHADER_PARAMETER_STRUCT_REF and
// reinterpret_cast its value from the engine's already-built View.ReflectionCaptureUniformBuffer.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "ShaderParameterMacros.h"
#include "ShaderParameterStruct.h"
#include "SceneRendering.h"  // FReflectionCaptureShaderData

// FReflectionCaptureShaderData arrays use the deprecated GMaxNumReflectionCaptures; inheriting it pulls
// that in, so silence the deprecation around the declaration like the engine does.
PRAGMA_DISABLE_DEPRECATION_WARNINGS
class FReflectionCaptureWrapper : public FReflectionCaptureShaderData
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
		using TAlignedType = FReflectionCaptureWrapper;

		static const FShaderParametersMetadata* GetStructMetadata();
	};
};
PRAGMA_ENABLE_DEPRECATION_WARNINGS

// Forward declaration for RDG template specialization
const FShaderParametersMetadata* GetForwardDeclaredShaderParametersStructMetadata(
	const FReflectionCaptureWrapper*);
