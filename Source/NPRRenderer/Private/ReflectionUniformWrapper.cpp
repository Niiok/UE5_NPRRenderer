#include "ReflectionUniformWrapper.h"
#include "ShaderParameterMacros.h"  // FindUniformBufferStructByName

const FShaderParametersMetadata* GetForwardDeclaredShaderParametersStructMetadata(const FReflectionUniformWrapper*)
{
	return FReflectionUniformWrapper::FTypeInfo::GetStructMetadata();
}

const FShaderParametersMetadata* FReflectionUniformWrapper::FTypeInfo::GetStructMetadata()
{
	return FindUniformBufferStructByName(TEXT("ReflectionStruct"));
}
