#include "ForwardLightUniformWrapper.h"
#include "ShaderParameterMacros.h"  // FindUniformBufferStructByName

const FShaderParametersMetadata* GetForwardDeclaredShaderParametersStructMetadata(const FForwardLightUniformWrapper*)
{
	return FForwardLightUniformWrapper::FTypeInfo::GetStructMetadata();
}

const FShaderParametersMetadata* FForwardLightUniformWrapper::FTypeInfo::GetStructMetadata()
{
	return FindUniformBufferStructByName(TEXT("ForwardLightStruct"));
}
