#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogNPRRenderer, Log, All);

class FNPRSceneViewExtension;
class IConsoleObject;

class FNPRRendererModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void SetNPREnabled(bool bEnabled);
	bool IsNPREnabled() const { return bNPREnabled; }

	static FNPRRendererModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FNPRRendererModule>("NPRRenderer");
	}

private:
	void OnPostEngineInit();
	void SetupNPRConsoleVariables();
	// Apply NPR console-variable overrides (Set with ECVF_SetByCode) when bEnable, or strip them
	// (Unset ECVF_SetByCode) when !bEnable so each CVar reverts to its prior config/default value.
	void ApplyNPRConsoleVariables(bool bEnable);

	TSharedPtr<FNPRSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
	bool bNPREnabled = true;
	// Console commands registered in SetupNPRConsoleVariables, unregistered in ShutdownModule
	// (they capture `this`, so they must not outlive the module).
	TArray<IConsoleObject*> NPRConsoleCommands;
};
