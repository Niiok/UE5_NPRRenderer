#include "ReflectionCaptureWrapper.h"
#include "ShaderParameterMacros.h"  // FindUniformBufferStructByName

const FShaderParametersMetadata* GetForwardDeclaredShaderParametersStructMetadata(const FReflectionCaptureWrapper*)
{
	return FReflectionCaptureWrapper::FTypeInfo::GetStructMetadata();
}

const FShaderParametersMetadata* FReflectionCaptureWrapper::FTypeInfo::GetStructMetadata()
{
	return FindUniformBufferStructByName(TEXT("ReflectionCaptureSM5"));
}
