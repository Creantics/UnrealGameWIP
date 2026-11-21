#include "FTBuildFleshCommandlet.h"

#include "FTFleshContact.h"
#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBreakMaterialAttributes.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPreSkinnedPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "RHIGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

#if WITH_EDITOR
// Named, not anonymous: in a unity build the other material commandlets' helpers share this translation unit.
namespace FTFleshBuild
{
namespace fl = ftcore::flesh;

// Vector parameter outputs: 0 RGB .. 5 RGBA.
constexpr int32 kRgba = 5;

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

FString PackageOf(const FString& ObjectPath)
{
	return FPackageName::ObjectPathToPackageName(ObjectPath);
}

template <typename T>
T* AddExpression(UMaterial* Material, int32 X, int32 Y)
{
	T* Expression = NewObject<T>(Material, NAME_None, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	Expression->Material = Material;
	Expression->MaterialExpressionEditorX = X;
	Expression->MaterialExpressionEditorY = Y;
	Expression->UpdateMaterialExpressionGuid(true, false);
	return Expression;
}

UMaterialExpressionVectorParameter* Vector(UMaterial* Material, const FName& Name, const FLinearColor& Default, int32 Y)
{
	UMaterialExpressionVectorParameter* P = AddExpression<UMaterialExpressionVectorParameter>(Material, -2600, Y);
	P->ParameterName = Name;
	P->DefaultValue = Default;
	P->Group = TEXT("FleshContact");
	P->UpdateParameterGuid(true, false);
	return P;
}

UMaterialExpressionScalarParameter* Scalar(UMaterial* Material, const TCHAR* Name, float Default, int32 Y)
{
	UMaterialExpressionScalarParameter* P = AddExpression<UMaterialExpressionScalarParameter>(Material, -2600, Y);
	P->ParameterName = Name;
	P->DefaultValue = Default;
	P->Group = TEXT("FleshContact");
	P->UpdateParameterGuid(true, false);
	return P;
}

// The defaults: no contact (depth 0), identity rotation.
const FLinearColor kPosDefault(0.f, 0.f, 0.f, 0.f);
const FLinearColor kNrmDefault(0.f, 0.f, 1.f, 0.f);
const FLinearColor kRotDefault(0.f, 0.f, 0.f, 1.f);

// One contact's shared opening, in HLSL: c, n, q, R, d; v, h, t, s from the ref-pose point Pt; win the window that keeps
// the dent to the skin facing the contact (a point the far side of the limb, or on another limb, is out of it).
FString ContactOpen(int32 I, const TCHAR* Pt)
{
	return FString::Printf(TEXT("{\n\tfloat4 c = C%dP; float4 nd = C%dN; float4 q = C%dR;\n\tfloat R = c.w; float d = nd.w;\n")
						   TEXT("\tif (d > 0.0 && R > 0.0)\n\t{\n")
						   TEXT("\t\tfloat3 n = nd.xyz; float3 v = %s - c.xyz; float h = dot(v, n); float3 t = v - n * h; float s = length(t);\n")
						   TEXT("\t\tfloat win = 1.0 - saturate((abs(h) - (R + W)) / max(R, 0.5));\n"),
		I, I, I, Pt);
}

// ftcore::flesh::profile, term for term, along the contact's ref-pose normal turned to world.
FString WpoCode()
{
	const float Peak = fl::kBulgePeak;
	FString Code = TEXT("float3 o = 0;\n");
	for (int32 I = 0; I < fl::kMaxContacts; ++I)
	{
		Code += ContactOpen(I, TEXT("P"));
		Code += TEXT("\t\tfloat y = 0.0;\n");
		Code += TEXT("\t\tif (s < R) { float u = s / R; float w = 1.0 - u * u; y = -d * w * w; }\n");
		Code += FString::Printf(TEXT("\t\telse if (W > 0.0 && B > 0.0 && s < R + W) { float k = (s - R) / W; float m = k * (1.0 - k); y = B * %.6f * d * 16.0 * m * m; }\n"), Peak);
		Code += TEXT("\t\tfloat3 nw = n + 2.0 * cross(q.xyz, cross(q.xyz, n) + q.w * n);\n");
		Code += TEXT("\t\to += nw * (y * win);\n\t}\n}\n");
	}
	Code += TEXT("return o;");
	return Code;
}

// ftcore::flesh::slope, the height's gradient along the skin, turned to world and then to tangent space; the normal tilts
// against it. With no contact the normal is handed back untouched.
FString NormalCode()
{
	const float Peak = fl::kBulgePeak;
	FString Code = TEXT("float3 g = 0;\n");
	for (int32 I = 0; I < fl::kMaxContacts; ++I)
	{
		Code += ContactOpen(I, TEXT("RP"));
		Code += TEXT("\t\tif (s > 1e-4)\n\t\t{\n\t\t\tfloat sl = 0.0;\n");
		Code += TEXT("\t\t\tif (s < R) { float u = s / R; float w = 1.0 - u * u; sl = 4.0 * d * w * u / R; }\n");
		Code += FString::Printf(TEXT("\t\t\telse if (W > 0.0 && B > 0.0 && s < R + W) { float k = (s - R) / W; float m = k * (1.0 - k); sl = B * %.6f * d * 32.0 * m * (1.0 - 2.0 * k) / W; }\n"), Peak);
		Code += TEXT("\t\t\tfloat3 gr = t * (sl * win / s);\n");
		Code += TEXT("\t\t\tg += gr + 2.0 * cross(q.xyz, cross(q.xyz, gr) + q.w * gr);\n\t\t}\n\t}\n}\n");
	}
	Code += TEXT("if (all(g == 0.0)) return N;\n");
	Code += TEXT("float3 gt = mul(Parameters.TangentToWorld, g);\n");
	Code += TEXT("return normalize(float3(N.xy - gt.xy, N.z));");
	return Code;
}

struct FParams
{
	UMaterialExpressionVectorParameter* Pos[fl::kMaxContacts];
	UMaterialExpressionVectorParameter* Nrm[fl::kMaxContacts];
	UMaterialExpressionVectorParameter* Rot[fl::kMaxContacts];
	UMaterialExpressionScalarParameter* Bulge;
	UMaterialExpressionScalarParameter* BulgeWidth;
};

UMaterialExpressionCustom* ContactCustom(UMaterial* Material, const TCHAR* Description, const FString& Code, const TCHAR* PointName,
	UMaterialExpression* Point, int32 PointOutput, const FParams& P, const TCHAR* NormalName, UMaterialExpression* Normal,
	int32 NormalOutput, int32 Y)
{
	UMaterialExpressionCustom* C = AddExpression<UMaterialExpressionCustom>(Material, -1800, Y);
	C->Description = Description;
	C->OutputType = CMOT_Float3;
	C->Code = Code;
	C->Inputs.Reset();
	const auto In = [C](const FString& Name, UMaterialExpression* From, int32 Output)
	{
		FCustomInput& Slot = C->Inputs.AddDefaulted_GetRef();
		Slot.InputName = FName(*Name);
		Slot.Input.Connect(Output, From);
	};
	In(PointName, Point, PointOutput);
	if (Normal)
	{
		In(NormalName, Normal, NormalOutput);
	}
	for (int32 I = 0; I < fl::kMaxContacts; ++I)
	{
		In(FString::Printf(TEXT("C%dP"), I), P.Pos[I], kRgba);
		In(FString::Printf(TEXT("C%dN"), I), P.Nrm[I], kRgba);
		In(FString::Printf(TEXT("C%dR"), I), P.Rot[I], kRgba);
	}
	In(TEXT("B"), P.Bulge, 0);
	In(TEXT("W"), P.BulgeWidth, 0);
	return C;
}

int32 OutputByName(const UMaterialExpression* Expression, const TCHAR* Name)
{
	const TArray<FExpressionOutput>& Outputs = const_cast<UMaterialExpression*>(Expression)->GetOutputs();
	for (int32 I = 0; I < Outputs.Num(); ++I)
	{
		if (Outputs[I].OutputName == FName(Name))
		{
			return I;
		}
	}
	return INDEX_NONE;
}

// Adds the dent and the normal fix to a copied master. False (with an error) for a graph shape it doesn't know.
bool Insert(UMaterial* Material)
{
	UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
	FParams P;
	for (int32 I = 0; I < fl::kMaxContacts; ++I)
	{
		P.Pos[I] = Vector(Material, FTFlesh::PosName(I), kPosDefault, 4000 + I * 300);
		P.Nrm[I] = Vector(Material, FTFlesh::NrmName(I), kNrmDefault, 4100 + I * 300);
		P.Rot[I] = Vector(Material, FTFlesh::RotName(I), kRotDefault, 4200 + I * 300);
	}
	P.Bulge = Scalar(Material, FTFlesh::kBulgeName, 1.f, 5300);
	P.BulgeWidth = Scalar(Material, FTFlesh::kBulgeWidthName, 1.f, 5400);

	// The ref-pose position: vertex only, so the pixel side gets it through an interpolator.
	UMaterialExpressionPreSkinnedPosition* Ref = AddExpression<UMaterialExpressionPreSkinnedPosition>(Material, -2600, 3700);
	UMaterialExpressionVertexInterpolator* RefPixel = AddExpression<UMaterialExpressionVertexInterpolator>(Material, -2300, 3800);
	RefPixel->Input.Connect(0, Ref);

	if (Material->bUseMaterialAttributes)
	{
		FMaterialAttributesInput& Attributes = Data->MaterialAttributes;
		UMaterialExpression* From = Attributes.Expression;
		const int32 FromOutput = Attributes.OutputIndex;
		if (!From)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: %s uses material attributes but nothing feeds them."), *Material->GetName());
			return false;
		}
		UMaterialExpressionBreakMaterialAttributes* Break = AddExpression<UMaterialExpressionBreakMaterialAttributes>(Material, -2300, 3400);
		Break->MaterialAttributes.Connect(FromOutput, From);
		const int32 NormalOut = OutputByName(Break, TEXT("Normal"));
		if (NormalOut == INDEX_NONE)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: BreakMaterialAttributes has no Normal output."));
			return false;
		}
		UMaterialExpressionCustom* Wpo = ContactCustom(Material, TEXT("FleshDent"), WpoCode(), TEXT("P"), Ref, 0, P, nullptr, nullptr, 0, 3500);
		UMaterialExpressionCustom* Fix = ContactCustom(Material, TEXT("FleshNormal"), NormalCode(), TEXT("RP"), RefPixel, 0, P,
			TEXT("N"), Break, NormalOut, 3800);
		UMaterialExpressionSetMaterialAttributes* Set = AddExpression<UMaterialExpressionSetMaterialAttributes>(Material, -1400, 3400);
		if (Set->Inputs.IsEmpty())
		{
			Set->Inputs.AddDefaulted();
		}
		Set->Inputs[0].Connect(FromOutput, From);
		if (!Set->ConnectInputAttribute(MP_Normal, Fix, 0) || !Set->ConnectInputAttribute(MP_WorldPositionOffset, Wpo, 0))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: couldn't connect Normal and WorldPositionOffset on SetMaterialAttributes."));
			return false;
		}
		Attributes.Connect(0, Set);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: %s: inserted after %s output %d (material attributes; Break Normal output %d)."),
			*Material->GetName(), *From->GetName(), FromOutput, NormalOut);
	}
	else
	{
		UMaterialExpression* NormalFrom = Data->Normal.Expression;
		int32 NormalOutput = Data->Normal.OutputIndex;
		if (!NormalFrom)
		{
			UMaterialExpressionConstant3Vector* Up = AddExpression<UMaterialExpressionConstant3Vector>(Material, -2300, 3400);
			Up->Constant = FLinearColor(0.f, 0.f, 1.f);
			NormalFrom = Up;
			NormalOutput = 0;
		}
		UMaterialExpressionCustom* Wpo = ContactCustom(Material, TEXT("FleshDent"), WpoCode(), TEXT("P"), Ref, 0, P, nullptr, nullptr, 0, 3500);
		UMaterialExpressionCustom* Fix = ContactCustom(Material, TEXT("FleshNormal"), NormalCode(), TEXT("RP"), RefPixel, 0, P,
			TEXT("N"), NormalFrom, NormalOutput, 3800);
		Data->Normal.Connect(0, Fix);
		Data->WorldPositionOffset.Connect(0, Wpo);
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: %s: inserted on the Normal and WorldPositionOffset pins."), *Material->GetName());
	}
	// The dent is at most ftcore::flesh's cap (0.15 of the limb's thickness) plus its ring: 5 cm bounds it with room.
	Material->MaxWorldPositionOffsetDisplacement = 5.f;
	return true;
}

// The resource the game renders for Material (its own static permutation for an instance), compiled; its errors logged.
// Uses WPO goes to bOutWpo. False when there is no resource or it has errors.
bool Compiled(UMaterialInterface* Material, bool& bOutWpo)
{
	bOutWpo = false;
	FMaterialResource* Resource = Material ? Material->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
	if (!Resource)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: %s has no material resource."), Material ? *Material->GetPathName() : TEXT("(null)"));
		return false;
	}
	Resource->FinishCompilation();
	for (const FString& Error : Resource->GetCompileErrors())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: %s: %s"), *Material->GetPathName(), *Error);
	}
	bOutWpo = Resource->MaterialUsesWorldPositionOffset_GameThread();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh:   resource %s: %d errors, uses world position offset %d."), *Material->GetPathName(),
		Resource->GetCompileErrors().Num(), bOutWpo ? 1 : 0);
	return Resource->GetCompileErrors().IsEmpty();
}

// One slot's chain, leaf first, master last (Chain.Last() is a UMaterial when the chain is whole).
TArray<UMaterialInterface*> Chain(UMaterialInterface* Leaf)
{
	TArray<UMaterialInterface*> Out;
	for (UMaterialInterface* M = Leaf; M && Out.Num() < 32; )
	{
		Out.Add(M);
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(M);
		M = Instance ? Instance->Parent.Get() : nullptr;
	}
	return Out;
}

void DumpChain(int32 Slot, const FName& SlotName, const TArray<UMaterialInterface*>& Links)
{
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: slot %d (%s):"), Slot, *SlotName.ToString());
	for (const UMaterialInterface* M : Links)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh:   %s  [%s]"), *M->GetPathName(), *M->GetClass()->GetName());
	}
	if (Links.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh:   (no material)"));
	}
}

// The arm bones and every bone near them (twist, corrective), with their ref-pose parent, for the pair setup.
void DumpArmBones(const USkeletalMesh& Mesh)
{
	const FReferenceSkeleton& Ref = Mesh.GetRefSkeleton();
	for (int32 I = 0; I < Ref.GetNum(); ++I)
	{
		const FString Name = Ref.GetBoneName(I).ToString();
		if (Name.Contains(TEXT("lowerarm")) || Name.Contains(TEXT("upperarm")) || Name.StartsWith(TEXT("hand_"))
			|| Name.Contains(TEXT("metacarpal")) || Name.StartsWith(TEXT("middle_01")))
		{
			const int32 Parent = Ref.GetParentIndex(I);
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: bone %d %s (parent %s), ref local %s"), I, *Name,
				Parent >= 0 ? *Ref.GetBoneName(Parent).ToString() : TEXT("-"), *Ref.GetRefBonePose()[I].GetTranslation().ToString());
		}
	}
}

bool Save(UObject* Asset)
{
	UPackage* Package = Asset->GetPackage();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: saved %s."), *File);
	return true;
}

int32 Verify(USkeletalMesh& Mesh, const FString& Folder, const FString& MasterName)
{
	const FString MeshName = FPackageName::GetShortName(Mesh.GetPackage()->GetName());
	const FString MasterCopy = MasterName + FTFlesh::kCopySuffix;
	int32 Found = 0;
	bool bOk = true;
	for (int32 Slot = 0; Slot < Mesh.GetMaterials().Num(); ++Slot)
	{
		const TArray<UMaterialInterface*> Links = Chain(Mesh.GetMaterials()[Slot].MaterialInterface);
		if (Links.IsEmpty() || Links.Last()->GetName() != MasterName)
		{
			continue;
		}
		const FString Path = FTFlesh::CopyPath(Folder, MeshName, Links[0]->GetName());
		UMaterialInterface* Copy = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Copy)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh -Verify: slot %d: no copy at %s."), Slot, *Path);
			bOk = false;
			continue;
		}
		++Found;
		const TArray<UMaterialInterface*> CopyLinks = Chain(Copy);
		DumpChain(Slot, Mesh.GetMaterials()[Slot].MaterialSlotName, CopyLinks);
		if (CopyLinks.Last()->GetName() != MasterCopy || CopyLinks.Num() != Links.Num())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh -Verify: slot %d: the copy's chain doesn't end at %s, or is not as long as the original's."), Slot, *MasterCopy);
			bOk = false;
		}
		for (const UMaterialInterface* Link : CopyLinks)
		{
			if (!Link->GetPathName().StartsWith(Folder))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh -Verify: slot %d: %s is outside %s."), Slot, *Link->GetPathName(), *Folder);
				bOk = false;
			}
		}
		bool bWpo = false;
		bOk &= Compiled(Copy, bWpo);
		if (!bWpo)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh -Verify: slot %d: %s does not use world position offset."), Slot, *Copy->GetName());
			bOk = false;
		}
		for (int32 I = 0; I < ftcore::flesh::kMaxContacts; ++I)
		{
			const FName Names[3] = {FTFlesh::PosName(I), FTFlesh::NrmName(I), FTFlesh::RotName(I)};
			for (const FName& Name : Names)
			{
				FLinearColor Value;
				if (!Copy->GetVectorParameterValue(FHashedMaterialParameterInfo(Name), Value))
				{
					UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh -Verify: slot %d: no %s."), Slot, *Name.ToString());
					bOk = false;
					continue;
				}
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh -Verify: slot %d: %s = (%g, %g, %g, %g)"), Slot, *Name.ToString(), Value.R,
					Value.G, Value.B, Value.A);
			}
		}
		for (const TCHAR* Name : {FTFlesh::kBulgeName, FTFlesh::kBulgeWidthName})
		{
			float Value = 0.f;
			if (!Copy->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value))
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh -Verify: slot %d: no %s."), Slot, Name);
				bOk = false;
				continue;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh -Verify: slot %d: %s = %g"), Slot, Name, Value);
		}
		if (const UMaterial* Master = Cast<UMaterial>(CopyLinks.Last()))
		{
			UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh -Verify: slot %d: %s MaxWorldPositionOffsetDisplacement = %g"), Slot,
				*Master->GetName(), Master->MaxWorldPositionOffsetDisplacement);
		}
	}
	if (Found == 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh -Verify: no slot of %s has a copy under %s."), *Mesh.GetPathName(), *Folder);
		bOk = false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh -Verify: %s (%d slots on copies)."), bOk ? TEXT("OK") : TEXT("FAILED"), Found);
	return bOk ? 0 : 1;
}
} // namespace FTFleshBuild
#endif

UFTBuildFleshCommandlet::UFTBuildFleshCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildFleshCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	using namespace FTFleshBuild;
	FString MeshPath, Folder = FTFlesh::kDefaultFolder, MasterName = FTFlesh::kDefaultMaster;
	FParse::Value(*Params, TEXT("Mesh="), MeshPath);
	FParse::Value(*Params, TEXT("Out="), Folder);
	FParse::Value(*Params, TEXT("Master="), MasterName);
	const bool bDump = FParse::Param(*Params, TEXT("Dump"));
	const bool bVerify = FParse::Param(*Params, TEXT("Verify"));
	Folder.RemoveFromEnd(TEXT("/"));
	if (MeshPath.IsEmpty() || !Folder.StartsWith(TEXT("/Game/")))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: give -Mesh=<skeletal mesh object path>; -Out must be under /Game/."));
		return 1;
	}
	if (!FApp::CanEverRender())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: run with -AllowCommandletRendering, so the copies compile and are checked."));
		return 1;
	}
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().WaitForCompletion();
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: no skeletal mesh at %s."), *MeshPath);
		return 1;
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (bVerify)
	{
		return Verify(*Mesh, Folder, MasterName);
	}

	// The dump and the checks, before anything is written.
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: %s, %d slots; copies to %s; master %s."), *Mesh->GetPathName(),
		Mesh->GetMaterials().Num(), *Folder, *MasterName);
	TArray<TArray<UMaterialInterface*>> Chains;
	TArray<int32> Selected;
	for (int32 Slot = 0; Slot < Mesh->GetMaterials().Num(); ++Slot)
	{
		const FSkeletalMaterial& Entry = Mesh->GetMaterials()[Slot];
		Chains.Add(Chain(Entry.MaterialInterface));
		DumpChain(Slot, Entry.MaterialSlotName, Chains.Last());
		if (!Chains.Last().IsEmpty() && Chains.Last().Last()->GetName() == MasterName)
		{
			Selected.Add(Slot);
		}
	}
	DumpArmBones(*Mesh);
	if (Selected.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: CHECK FAILED: no slot's chain ends at a master named %s (dump above)."), *MasterName);
		return 1;
	}
	UMaterial* Master = Cast<UMaterial>(Chains[Selected[0]].Last());
	bool bOk = Master != nullptr;
	for (int32 Slot : Selected)
	{
		bOk &= Chains[Slot].Last() == Master;
	}
	if (!bOk)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: CHECK FAILED: the slots' chains end at different materials named %s, or not at a material."), *MasterName);
		return 1;
	}
	UMaterialEditorOnlyData* Data = Master->GetEditorOnlyData();
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: master %s: material attributes %d (fed %d), tangent-space normal %d, Normal pin %d, WPO pin %d, %d expressions."),
		*Master->GetPathName(), Master->bUseMaterialAttributes ? 1 : 0, Data->MaterialAttributes.Expression ? 1 : 0,
		Master->bTangentSpaceNormal ? 1 : 0, Data->Normal.Expression ? 1 : 0, Data->WorldPositionOffset.Expression ? 1 : 0,
		Master->GetExpressionCollection().Expressions.Num());
	if (!Master->bTangentSpaceNormal)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: CHECK FAILED: %s's normal is not tangent space."), *Master->GetName());
		bOk = false;
	}
	if (Master->bUseMaterialAttributes ? !Data->MaterialAttributes.Expression : false)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: CHECK FAILED: %s uses material attributes but nothing feeds them."), *Master->GetName());
		bOk = false;
	}
	if (!Master->bUseMaterialAttributes && Data->WorldPositionOffset.Expression)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: CHECK FAILED: %s's WorldPositionOffset pin is connected."), *Master->GetName());
		bOk = false;
	}
	// The compiled shaders the body renders: the master's and each instance's own permutation.
	TSet<UMaterialInterface*> Checked;
	for (int32 Slot : Selected)
	{
		for (UMaterialInterface* Link : Chains[Slot])
		{
			if (Checked.Contains(Link))
			{
				continue;
			}
			Checked.Add(Link);
			bool bWpo = false;
			if (!Compiled(Link, bWpo))
			{
				bOk = false;
			}
			if (bWpo)
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: CHECK FAILED: %s already uses world position offset."), *Link->GetPathName());
				bOk = false;
			}
		}
	}
	if (!bOk)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: checks passed on %d slots."), Selected.Num());
	if (bDump)
	{
		return 0;
	}

	// The copies: every link of every selected chain once, master first, each instance's parent the copy of its parent.
	const FString MeshName = FPackageName::GetShortName(Mesh->GetPackage()->GetName());
	TMap<UMaterialInterface*, UMaterialInterface*> Copies;
	TArray<UMaterialInterface*> Order;
	for (int32 Slot : Selected)
	{
		for (int32 I = Chains[Slot].Num() - 1; I >= 0; --I)
		{
			Order.AddUnique(Chains[Slot][I]);
		}
	}
	for (UMaterialInterface* Link : Order)
	{
		const FString File = PackageFile(PackageOf(FTFlesh::CopyPath(Folder, MeshName, Link->GetName())));
		if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: couldn't replace %s."), *File);
			return 1;
		}
	}
	TArray<UObject*> ToSave;
	UMaterial* MasterCopy = nullptr;
	for (UMaterialInterface* Link : Order)
	{
		const FString Path = FTFlesh::CopyPath(Folder, MeshName, Link->GetName());
		UPackage* Package = CreatePackage(*PackageOf(Path));
		const FName Name(*FPackageName::ObjectPathToObjectName(Path));
		if (UMaterial* AsMaster = Cast<UMaterial>(Link))
		{
			MasterCopy = DuplicateObject<UMaterial>(AsMaster, Package, Name);
			MasterCopy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			if (!Insert(MasterCopy))
			{
				return 1;
			}
			MasterCopy->PreEditChange(nullptr);
			MasterCopy->PostEditChange();
			FAssetRegistryModule::AssetCreated(MasterCopy);
			Copies.Add(Link, MasterCopy);
			ToSave.Add(MasterCopy);
			continue;
		}
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Link);
		UMaterialInterface* const* ParentCopy = Instance ? Copies.Find(Instance->Parent.Get()) : nullptr;
		if (!Instance || !ParentCopy)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: %s is not a material instance constant with a copied parent."), *Link->GetPathName());
			return 1;
		}
		UMaterialInstanceConstant* Copy = DuplicateObject<UMaterialInstanceConstant>(Instance, Package, Name);
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		Copy->SetParentEditorOnly(*ParentCopy);
		Copy->PostEditChange();
		FAssetRegistryModule::AssetCreated(Copy);
		Copies.Add(Link, Copy);
		ToSave.Add(Copy);
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	for (UObject* Asset : ToSave)
	{
		bool bWpo = false;
		if (!Compiled(Cast<UMaterialInterface>(Asset), bWpo) || !bWpo)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh: %s has compile errors or doesn't use world position offset; nothing saved."), *Asset->GetPathName());
			return 1;
		}
	}
	for (UObject* Asset : ToSave)
	{
		bOk &= Save(Asset);
	}
	if (!bOk)
	{
		return 1;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildFlesh: %d copies of %s's skin under %s/%s (master %s, %d expressions)."), ToSave.Num(),
		*MeshName, *Folder, *MeshName, *MasterCopy->GetName(), MasterCopy->GetExpressionCollection().Expressions.Num());
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildFlesh needs an editor build."));
	return 1;
#endif
}
