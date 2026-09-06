#include "NPRBlueprintLibrary.h"
#include "NPRRendererModule.h"

void UNPRBlueprintLibrary::SetNPREnabled(bool bEnabled)
{
	if (FNPRRendererModule* Module = FModuleManager::GetModulePtr<FNPRRendererModule>("NPRRenderer"))
	{
		Module->SetNPREnabled(bEnabled);
	}
}

bool UNPRBlueprintLibrary::IsNPREnabled()
{
	if (FNPRRendererModule* Module = FModuleManager::GetModulePtr<FNPRRendererModule>("NPRRenderer"))
	{
		return Module->IsNPREnabled();
	}
	return false;
}

