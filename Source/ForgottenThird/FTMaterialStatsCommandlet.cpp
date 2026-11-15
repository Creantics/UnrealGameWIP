#include "FTMaterialStatsCommandlet.h"

#include "ForgottenThird.h"
#include "FTWorldWave.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/PackageName.h"
#include "RHIShaderPlatform.h"
#include "RHIStrings.h"
#include "UObject/Package.h"
#endif

#if WITH_EDITOR
namespace
{
const TCHAR* const kTag = TEXT("FTMaterialStats");

const TCHAR* const kDefaultMasters[] = {
	TEXT("/Game/Rendering/M_WorldSurface"),
	TEXT("/Game/Rendering/M_WorldSurfaceTex"),
	TEXT("/Game/Rendering/M_WorldGround"),
	TEXT("/Game/Rendering/Foliage/M_FTFoliage"),
	TEXT("/Game/Rendering/Foliage/M_FTBark"),
};

// A transient copy of Master with the world wave's state call bypassed; null when it has none.
UMaterial* WithoutState(UMaterial* Master)
{
	UMaterial* Copy = DuplicateObject<UMaterial>(Master, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(),
		FName(Master->GetName() + TEXT("_NoState"))));
	if (!Copy)
	{
		return nullptr;
	}
	const FString StatePath = FString(FTWorldWave::StateFunctionPath);
	UMaterialExpressionMaterialFunctionCall* Call = nullptr;
	for (UMaterialExpression* Expression : Copy->GetExpressionCollection().Expressions)
	{
		UMaterialExpressionMaterialFunctionCall* Candidate = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		if (Candidate && Candidate->MaterialFunction && Candidate->MaterialFunction->GetOutermost()->GetName() == StatePath)
		{
			Call = Candidate;
			break;
		}
	}
	if (!Call)
	{
		return nullptr;
	}
	UMaterialEditorOnlyData* Inputs = Copy->GetEditorOnlyData();
	FExpressionInput* const MasterInputs[] = {&Inputs->BaseColor, &Inputs->Roughness, &Inputs->EmissiveColor};
	for (FExpressionInput* In : MasterInputs)
	{
		if (In->Expression != Call)
		{
			continue;
		}
		const int32 Index = In->OutputIndex;
		const FExpressionInput* Source = Call->FunctionInputs.IsValidIndex(Index) ? &Call->FunctionInputs[Index].Input : nullptr;
		if (Source && Source->Expression)
		{
			In->Connect(Source->OutputIndex, Source->Expression);
		}
		else
		{
			In->Expression = nullptr;
		}
	}
	Copy->GetExpressionCollection().RemoveExpression(Call);
	Copy->PreEditChange(nullptr);
	Copy->PostEditChange();
	return Copy;
}

FString Line(const FMaterialStatistics& S)
{
	return FString::Printf(TEXT("PS %d, VS %d instructions, %d samplers, %d PS / %d VS texture samples"), S.NumPixelShaderInstructions,
		S.NumVertexShaderInstructions, S.NumSamplers, S.NumPixelTextureSamples, S.NumVertexTextureSamples);
}
} // namespace
#endif

UFTMaterialStatsCommandlet::UFTMaterialStatsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTMaterialStatsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	TArray<FString> Masters;
	FString List;
	if (FParse::Value(*Params, TEXT("Masters="), List, false))
	{
		List.ParseIntoArray(Masters, TEXT(";"));
	}
	else
	{
		for (const TCHAR* Path : kDefaultMasters)
		{
			Masters.Add(Path);
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("%s: shader platform %s."), kTag, *LexToString(GMaxRHIShaderPlatform));
	int32 Missing = 0;
	for (const FString& Path : Masters)
	{
		UMaterial* Master = LoadObject<UMaterial>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
		if (!Master)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("%s: no material %s."), kTag, *Path);
			++Missing;
			continue;
		}
		const FMaterialStatistics With = UMaterialEditingLibrary::GetStatistics(Master);
		UMaterial* Bare = WithoutState(Master);
		if (!Bare)
		{
			UE_LOG(LogForgottenThird, Display, TEXT("%s: %s wears no world-wave state: %s."), kTag, *Master->GetName(), *Line(With));
			continue;
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		const FMaterialStatistics Without = UMaterialEditingLibrary::GetStatistics(Bare);
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s without the state: %s."), kTag, *Master->GetName(), *Line(Without));
		UE_LOG(LogForgottenThird, Display, TEXT("%s: %s with the state:    %s (PS %+d, VS %+d)."), kTag, *Master->GetName(), *Line(With),
			With.NumPixelShaderInstructions - Without.NumPixelShaderInstructions,
			With.NumVertexShaderInstructions - Without.NumVertexShaderInstructions);
	}
	return Missing == 0 ? 0 : 1;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTMaterialStats needs an editor build."));
	return 1;
#endif
}
