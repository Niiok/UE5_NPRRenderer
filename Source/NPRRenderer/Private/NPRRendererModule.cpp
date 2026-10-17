#include "NPRRendererModule.h"
#include "NPRSceneViewExtension.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"

#if WITH_EDITOR
#include "NPRMaterialFunctionFactory.h"
#endif

#define LOCTEXT_NAMESPACE "FNPRRendererModule"

DEFINE_LOG_CATEGORY(LogNPRRenderer);

void FNPRRendererModule::StartupModule()
{
	// Register shader directory
	FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("NPRRenderer"))->GetBaseDir(),
		TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/NPRRenderer"), PluginShaderDir);

	// Defer until engine is fully initialized
	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FNPRRendererModule::OnPostEngineInit);

	SetupNPRConsoleVariables();
	ApplyNPRConsoleVariables(bNPREnabled);  // apply initial state (default: NPR on)

	UE_LOG(LogNPRRenderer, Log, TEXT("Module started"));
}

void FNPRRendererModule::OnPostEngineInit()
{
	// Register scene view extension
	if (!SceneViewExtension.IsValid())
	{
		SceneViewExtension = FSceneViewExtensions::NewExtension<FNPRSceneViewExtension>();
		UE_LOG(LogNPRRenderer, Log, TEXT("SceneViewExtension registered"));
	}

#if WITH_EDITOR
	// Factory handles Asset Registry wait + file existence check internally
	FNPRMaterialFunctionFactory::EnsureExists();
#endif
}

void FNPRRendererModule::ShutdownModule()
{
	ApplyNPRConsoleVariables(false);  // restore engine CVars on unload

	// Unregister our console commands — they capture `this` and must not outlive the module.
	for (IConsoleObject* Command : NPRConsoleCommands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Command);
	}
	NPRConsoleCommands.Reset();

	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
	SceneViewExtension.Reset();
	UE_LOG(LogNPRRenderer, Log, TEXT("Module shutdown"));
}

// NPR console-variable overrides, applied/stripped together by ApplyNPRConsoleVariables().
//   r.shadow.ShadowMapsRenderEarly : NPR shadow pass needs shadow maps available early
//   r.Shadow.Virtual.Enable        : VSM unsupported by the NPR shadow path
//   r.Visibility.LocalLightPrimitiveInteraction : default 2 skips local light-primitive interactions when
//                                    MegaLights is enabled for the project, leaving point/spot shadow maps
//                                    without casters. NPR disables MegaLights, so force them on.
static const TCHAR* const GNPRConsoleVarNames[] =
{
	TEXT("r.shadow.ShadowMapsRenderEarly"),
	TEXT("r.Shadow.Virtual.Enable"),
	TEXT("r.Visibility.LocalLightPrimitiveInteraction"),
};
static const TCHAR* const GNPRConsoleVarValues[] =
{
	TEXT("1"),
	TEXT("0"),
	TEXT("1"),
};
static_assert(UE_ARRAY_COUNT(GNPRConsoleVarNames) == UE_ARRAY_COUNT(GNPRConsoleVarValues), "NPR cvar name/value count mismatch");

void FNPRRendererModule::ApplyNPRConsoleVariables(bool bEnable)
{
	for (int32 i = 0; i < UE_ARRAY_COUNT(GNPRConsoleVarNames); ++i)
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(GNPRConsoleVarNames[i]);
		if (!CVar)
		{
			continue;
		}
		if (bEnable)
		{
			CVar->Set(GNPRConsoleVarValues[i], ECVF_SetByCode);
		}
		else
		{
			// Strip our code override; the CVar reverts to its prior config/default value (UE tracks
			// per-priority history on every Set, so Unset(ECVF_SetByCode) restores it).
			CVar->Unset(ECVF_SetByCode);
		}
	}
}

void FNPRRendererModule::SetupNPRConsoleVariables()
{
	if (NPRConsoleCommands.Num() > 0)
	{
		return;  // already registered (guard against double-register on reload)
	}

	NPRConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("r.NPR.Toggle"), TEXT("Toggle NPR rendering on/off"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			SetNPREnabled(!bNPREnabled);
			UE_LOG(LogNPRRenderer, Log, TEXT("NPR %s"), bNPREnabled ? TEXT("Enabled") : TEXT("Disabled"));
		}), ECVF_Default));

	NPRConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("r.NPR.On"), TEXT("Enable NPR rendering"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			SetNPREnabled(true);
			UE_LOG(LogNPRRenderer, Log, TEXT("NPR Enabled"));
		}), ECVF_Default));

	NPRConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("r.NPR.Off"), TEXT("Disable NPR rendering"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			SetNPREnabled(false);
			UE_LOG(LogNPRRenderer, Log, TEXT("NPR Disabled"));
		}), ECVF_Default));
}

void FNPRRendererModule::SetNPREnabled(bool bEnabled)
{
	bNPREnabled = bEnabled;
	ApplyNPRConsoleVariables(bEnabled);  // apply overrides on, or revert them off
	if (SceneViewExtension.IsValid())
	{
		SceneViewExtension->SetEnabled(bEnabled);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNPRRendererModule, NPRRenderer)
