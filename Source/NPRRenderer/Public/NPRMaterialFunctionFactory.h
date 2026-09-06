#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

class UMaterialFunction;
class UMaterialExpressionFunctionInput;
class UMaterialExpressionCustom;

class NPRRENDERER_API FNPRMaterialFunctionFactory
{
public:
    /**
     * Asset Registry가 준비된 후 호출하면 됨.
     * - 파일이 없으면 생성·저장
     * - 파일이 있으면 아무것도 하지 않음 (Material 참조 보존)
     */
    static void EnsureExists();

    /** bForceRecreate=true 이면 기존 파일을 덮어쓰고 재생성 */
    static UMaterialFunction* CreateNPREncodeFunction(bool bForceRecreate = false);

private:
    static const FString& AssetName();
    static const FString& PackagePath();
    static FString PackageFileName();

    static UMaterialFunction* BuildAndSave(const FString& InPackageFileName);

    static void CreateInputNodes(UMaterialFunction* MF,
        TArray<UMaterialExpressionFunctionInput*>& OutInputs);
    static UMaterialExpressionCustom* CreateCustomNode(UMaterialFunction* MF,
        const TArray<UMaterialExpressionFunctionInput*>& Inputs);
    static void CreateOutputNodes(UMaterialFunction* MF,
        UMaterialExpressionCustom* CustomNode);
};

#endif // WITH_EDITOR
