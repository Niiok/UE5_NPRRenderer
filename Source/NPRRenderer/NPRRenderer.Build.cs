// NPR Renderer - Uses Engine's Forward Lighting System with RDG
// ★ Note: FForwardLightUniformParameters is NOT exported (no RENDERER_API)
// ★ Solution: Extract light data as individual parameters

using UnrealBuildTool;
using System.IO;

public class NPRRenderer : ModuleRules
{
	public NPRRenderer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		// ★ Access to Renderer private/internal headers for FViewInfo, FForwardLightUniformParameters
		// WARNING: These are not officially supported APIs and may change between engine versions
		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		PrivateIncludePaths.Add(Path.Combine(EngineDir, "Source/Runtime/Renderer/Private"));
		PrivateIncludePaths.Add(Path.Combine(EngineDir, "Source/Runtime/Renderer/Internal"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"Renderer",
			"RHI",
			"Projects"
		});
		
		PrivateIncludePathModuleNames.Add("Renderer");
		
		// Editor-only modules for Material Function generation
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
				"MaterialEditor"
			});
		}
	}
}
