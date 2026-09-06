#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NPRBlueprintLibrary.generated.h"

/**
 * NPR Renderer Blueprint Library
 * 
 * Provides Blueprint access to NPR rendering controls
 */
UCLASS()
class NPRRENDERER_API UNPRBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Enable or disable NPR rendering
	 * When disabled, standard PBR rendering is used
	 */
	UFUNCTION(BlueprintCallable, Category = "NPR Renderer")
	static void SetNPREnabled(bool bEnabled);

	/**
	 * Check if NPR rendering is enabled
	 */
	UFUNCTION(BlueprintPure, Category = "NPR Renderer")
	static bool IsNPREnabled();
};
