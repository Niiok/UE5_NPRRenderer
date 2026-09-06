// NPR Material Function Factory Implementation

#include "NPRMaterialFunctionFactory.h"

#if WITH_EDITOR

#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"

// ─────────────────────────────────────────────────────────────────────────────
// Path helpers (single source of truth)
// ─────────────────────────────────────────────────────────────────────────────

const FString& FNPRMaterialFunctionFactory::AssetName()
{
    static const FString Name = TEXT("MF_NPR_Encode");
    return Name;
}

const FString& FNPRMaterialFunctionFactory::PackagePath()
{
    static const FString Path = TEXT("/Game/NPR/MaterialFunctions");
    return Path;
}

FString FNPRMaterialFunctionFactory::PackageFileName()
{
    const FString FullPath = PackagePath() / AssetName();
    const FString PkgName  = FPackageName::ObjectPathToPackageName(FullPath);
    return FPackageName::LongPackageNameToFilename(PkgName, FPackageName::GetAssetPackageExtension());
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void FNPRMaterialFunctionFactory::EnsureExists()
{
    check(IsInGameThread());

    // Asset Registry가 아직 로딩 중이면 완료 후 재시도
    FAssetRegistryModule& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    if (AR.Get().IsLoadingAssets())
    {
        AR.Get().OnFilesLoaded().AddLambda([]() { EnsureExists(); });
        return;
    }

    CreateNPREncodeFunction(false);
}

UMaterialFunction* FNPRMaterialFunctionFactory::CreateNPREncodeFunction(bool bForceRecreate)
{
    const FString FileName = PackageFileName();

    if (FPaths::FileExists(FileName) && !bForceRecreate)
    {
        // 파일이 있으면 건드리지 않는다. Material이 직접 참조하면 됨.
        return nullptr;
    }

    return BuildAndSave(FileName);
}

// ─────────────────────────────────────────────────────────────────────────────
// Build & Save
// ─────────────────────────────────────────────────────────────────────────────

UMaterialFunction* FNPRMaterialFunctionFactory::BuildAndSave(const FString& FileName)
{
    const FString FullPath   = PackagePath() / AssetName();
    const FString PkgName    = FPackageName::ObjectPathToPackageName(FullPath);

    UE_LOG(LogTemp, Log, TEXT("NPR: Creating %s"), *FullPath);

    UPackage* Package = CreatePackage(*PkgName);
    if (!Package) { UE_LOG(LogTemp, Error, TEXT("NPR: CreatePackage failed")); return nullptr; }

    UMaterialFunction* MF = NewObject<UMaterialFunction>(Package, *AssetName(), RF_Public | RF_Standalone | RF_Transactional);
    if (!MF)      { UE_LOG(LogTemp, Error, TEXT("NPR: NewObject failed"));     return nullptr; }

    MF->Description = TEXT("PBR (M/S/R) + NPR 파라미터를 4+4 bit으로 인코딩합니다.");
    MF->bExposeToLibrary = true;
    MF->LibraryCategoriesText.Add(FText::FromString(TEXT("NPR")));

    TArray<UMaterialExpressionFunctionInput*> Inputs;
    CreateInputNodes(MF, Inputs);
    UMaterialExpressionCustom* CustomNode = CreateCustomNode(MF, Inputs);
    CreateOutputNodes(MF, CustomNode);

    // 디렉토리 생성
    const FString Dir = FPaths::GetPath(FileName);
    if (!IFileManager::Get().DirectoryExists(*Dir))
    {
        IFileManager::Get().MakeDirectory(*Dir, true);
    }

    Package->MarkPackageDirty();

    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags     = SAVE_NoError;

    if (UPackage::SavePackage(Package, MF, *FileName, Args))
    {
        FAssetRegistryModule::AssetCreated(MF);
        UE_LOG(LogTemp, Log, TEXT("NPR: Saved to %s"), *FileName);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("NPR: Save failed: %s"), *FileName);
    }

    return MF;
}

// ─────────────────────────────────────────────────────────────────────────────
// Node builders
// ─────────────────────────────────────────────────────────────────────────────

void FNPRMaterialFunctionFactory::CreateInputNodes(UMaterialFunction* MF,
    TArray<UMaterialExpressionFunctionInput*>& OutInputs)
{
    struct FDef { FName Name; FString Desc; float Default; };
    const TArray<FDef> Defs = {
        { TEXT("Metallic"),             TEXT("PBR Metallic (0~1)"),          0.0f },
        { TEXT("Specular"),             TEXT("PBR Specular (0~1)"),          0.5f },
        { TEXT("Roughness"),            TEXT("PBR Roughness (0~1)"),         0.5f },
        { TEXT("NPR_ShadowThreshold"),  TEXT("NPR Shadow Threshold (0~1)"),  0.5f },
        { TEXT("NPR_ShadowBrightness"), TEXT("NPR Shadow Brightness (0~1)"), 0.5f },
        { TEXT("NPR_ShadowSoftness"),   TEXT("NPR Shadow Softness (0~1)"),   0.5f },
    };

    for (int32 i = 0; i < Defs.Num(); ++i)
    {
        UMaterialExpressionFunctionInput* Node = NewObject<UMaterialExpressionFunctionInput>(MF);
        Node->InputName                 = Defs[i].Name;
        Node->Description               = Defs[i].Desc;
        Node->InputType                 = EFunctionInputType::FunctionInput_Scalar;
        Node->PreviewValue              = FVector4f(Defs[i].Default, 0, 0, 0);
        Node->bUsePreviewValueAsDefault = true;
        Node->SortPriority              = i;
        Node->MaterialExpressionEditorX = -400;
        Node->MaterialExpressionEditorY = i * 100;
        MF->GetExpressionCollection().AddExpression(Node);
        OutInputs.Add(Node);
    }
}

UMaterialExpressionCustom* FNPRMaterialFunctionFactory::CreateCustomNode(
    UMaterialFunction* MF,
    const TArray<UMaterialExpressionFunctionInput*>& Inputs)
{
    UMaterialExpressionCustom* Node = NewObject<UMaterialExpressionCustom>(MF);
    Node->Code        = TEXT("return EncodeNPR_MSR(Metallic, Specular, Roughness, NPR_ShadowThreshold, NPR_ShadowBrightness, NPR_ShadowSoftness);");
    Node->OutputType  = CMOT_Float3;
    Node->Description = TEXT("NPR 4+4 Bit Encoder");
    Node->IncludeFilePaths.Add(TEXT("/Plugin/NPRRenderer/Public/NPREncoding.ush"));
    Node->MaterialExpressionEditorX = 0;
    Node->MaterialExpressionEditorY = 200;

    const TArray<FString> Names = {
        TEXT("Metallic"), TEXT("Specular"), TEXT("Roughness"),
        TEXT("NPR_ShadowThreshold"), TEXT("NPR_ShadowBrightness"), TEXT("NPR_ShadowSoftness")
    };

    Node->Inputs.Empty();
    for (int32 i = 0; i < Inputs.Num() && i < Names.Num(); ++i)
    {
        FCustomInput CI;
        CI.InputName = FName(*Names[i]);
        Node->Inputs.Add(CI);
        Node->Inputs[i].Input.Connect(0, Inputs[i]);
    }

    MF->GetExpressionCollection().AddExpression(Node);
    return Node;
}

void FNPRMaterialFunctionFactory::CreateOutputNodes(UMaterialFunction* MF,
    UMaterialExpressionCustom* CustomNode)
{
    struct FDef { FName Name; FString Desc; bool R, G, B; float Y; };
    const TArray<FDef> Defs = {
        { TEXT("Metallic"),  TEXT("Encoded Metallic"),  true,  false, false, 100.f },
        { TEXT("Specular"),  TEXT("Encoded Specular"),  false, true,  false, 200.f },
        { TEXT("Roughness"), TEXT("Encoded Roughness"), false, false, true,  300.f },
    };

    for (int32 i = 0; i < Defs.Num(); ++i)
    {
        UMaterialExpressionComponentMask* Mask = NewObject<UMaterialExpressionComponentMask>(MF);
        Mask->R = Defs[i].R; Mask->G = Defs[i].G; Mask->B = Defs[i].B; Mask->A = false;
        Mask->MaterialExpressionEditorX = 200;
        Mask->MaterialExpressionEditorY = Defs[i].Y;
        Mask->Input.Connect(0, CustomNode);
        MF->GetExpressionCollection().AddExpression(Mask);

        UMaterialExpressionFunctionOutput* Out = NewObject<UMaterialExpressionFunctionOutput>(MF);
        Out->OutputName  = Defs[i].Name;
        Out->Description = Defs[i].Desc;
        Out->SortPriority = i;
        Out->MaterialExpressionEditorX = 400;
        Out->MaterialExpressionEditorY = Defs[i].Y;
        Out->A.Connect(0, Mask);
        MF->GetExpressionCollection().AddExpression(Out);
    }
}

#endif // WITH_EDITOR
