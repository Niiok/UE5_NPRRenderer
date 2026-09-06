#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "RenderGraph.h"
#include "RenderGraphDefinitions.h"
#include "SceneTextureParameters.h"

class FNPRSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FNPRSceneViewExtension(const FAutoRegister& AutoRegister);
	
	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
	bool IsEnabled() const { return bEnabled; }
	
	//~ ISceneViewExtension interface
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	
	//=========================================================================
	// PC Deferred Hook (RDG-based)
	// Note: Mobile uses same renderer (SVE cannot disable mobile path)
	//=========================================================================
	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder, 
		FSceneView& InView, 
		const FRenderTargetBindingSlots& RenderTargets, 
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

private:
	bool bEnabled = true;
	// Incoming SkyLighting show flag, captured in SetupNPRShowFlags before NPR forces it off, so the
	// NPR sky pass can still respect the user's intent (game thread -> render thread, single-view).
	bool bSkyLightingEnabled = true;

	void SetupNPRShowFlags(FEngineShowFlags& ShowFlags);
};
