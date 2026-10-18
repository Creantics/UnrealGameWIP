#include "FTBuildVfxCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionParticleColor.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEmitterState.h"
#include "NiagaraTypes.h"
#include "Stateless/Modules/NiagaraStatelessModule_AccelerationForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_AddVelocity.h"
#include "Stateless/Modules/NiagaraStatelessModule_CurlNoiseForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_Drag.h"
#include "Stateless/Modules/NiagaraStatelessModule_GravityForce.h"
#include "Stateless/Modules/NiagaraStatelessModule_InitializeParticle.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleColor.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleSpriteSize.h"
#include "Stateless/Modules/NiagaraStatelessModule_ScaleSpriteSizeBySpeed.h"
#include "Stateless/Modules/NiagaraStatelessModule_ShapeLocation.h"
#include "Stateless/Modules/NiagaraStatelessModule_SolveVelocitiesAndForces.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteFacingAndAlignment.h"
#include "Stateless/Modules/NiagaraStatelessModule_SpriteRotationRate.h"
#include "Stateless/NiagaraStatelessEmitter.h"
#include "Stateless/NiagaraStatelessModule.h"
#include "Stateless/NiagaraStatelessSpawnInfo.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/Vfx.h"

#include <string>
#endif

#if WITH_EDITOR
namespace
{
const TCHAR* const TemplatePath = TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight");
const TCHAR* const VfxRoot = TEXT("/Game/VFX");
const TCHAR* const MaterialRoot = TEXT("/Game/VFX/Materials");

FString PackageFile(const FString& Package)
{
	return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

bool Save(UPackage* Package, UObject* Asset)
{
	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Package->GetName());
	if (!UPackage::SavePackage(Package, Asset, *File, Args))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: saved %s."), *File);
	return true;
}

bool RemoveFile(const FString& File)
{
	if (IFileManager::Get().FileExists(*File) && !IFileManager::Get().Delete(*File, false, true))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't delete %s."), *File);
		return false;
	}
	return true;
}

// ---- sprite materials ------------------------------------------------------------------------------------

// The procedural sprites newengine draws when an emitter has no texture: a soft round glow, and a spark with a
// hot core. Puff (an eroded cloud) uses the soft one.
const TCHAR* const SoftMaskCode = TEXT(
	"float2 d = UV * 2.0 - 1.0;\n"
	"float m = 1.0 - saturate(dot(d, d));\n"
	"return m * m;\n");
const TCHAR* const SparkMaskCode = TEXT(
	"float2 d = UV * 2.0 - 1.0;\n"
	"float r2 = saturate(dot(d, d));\n"
	"return saturate(exp(-r2 * 18.0) + 0.45 * pow(1.0 - r2, 3.0));\n");

struct FSpriteMaterialSpec
{
	const TCHAR* Name;
	bool bSpark;
	bool bAdditive;
};

const FSpriteMaterialSpec SpriteMaterials[] = {
	{TEXT("M_VfxSoft"), false, false},
	{TEXT("M_VfxSoftAdditive"), false, true},
	{TEXT("M_VfxSpark"), true, false},
	{TEXT("M_VfxSparkAdditive"), true, true},
};

FString MaterialPackage(const FSpriteMaterialSpec& Spec)
{
	return FString(MaterialRoot) / Spec.Name;
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

UMaterialExpressionCustom* AddCustom(UMaterial* Material, int32 Y, const TCHAR* Description, ECustomMaterialOutputType Type,
	const FString& Code, UMaterialExpression* Color, UMaterialExpression* Mask)
{
	UMaterialExpressionCustom* Custom = AddExpression<UMaterialExpressionCustom>(Material, -300, Y);
	Custom->Description = Description;
	Custom->OutputType = Type;
	Custom->Code = Code;
	Custom->Inputs.Reset();
	FCustomInput& InColor = Custom->Inputs.AddDefaulted_GetRef();
	InColor.InputName = TEXT("Color");
	InColor.Input.Connect(5, Color); // RGBA
	FCustomInput& InMask = Custom->Inputs.AddDefaulted_GetRef();
	InMask.InputName = TEXT("Mask");
	InMask.Input.Connect(0, Mask);
	return Custom;
}

UMaterial* BuildSpriteMaterial(UPackage* Package, const FSpriteMaterialSpec& Spec)
{
	UMaterial* Material = NewObject<UMaterial>(Package, Spec.Name, RF_Public | RF_Standalone | RF_Transactional);
	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = Spec.bAdditive ? BLEND_Additive : BLEND_Translucent;
	Material->SetShadingModel(MSM_Unlit);
	Material->SetUsageByFlag(MATUSAGE_NiagaraSprites, true);

	UMaterialExpressionTextureCoordinate* UV = AddExpression<UMaterialExpressionTextureCoordinate>(Material, -900, 0);
	UMaterialExpressionParticleColor* Color = AddExpression<UMaterialExpressionParticleColor>(Material, -900, 150);

	UMaterialExpressionCustom* Mask = AddExpression<UMaterialExpressionCustom>(Material, -600, 0);
	Mask->Description = Spec.bSpark ? TEXT("SparkMask") : TEXT("SoftMask");
	Mask->OutputType = CMOT_Float1;
	Mask->Code = Spec.bSpark ? SparkMaskCode : SoftMaskCode;
	Mask->Inputs.Reset();
	FCustomInput& InUV = Mask->Inputs.AddDefaulted_GetRef();
	InUV.InputName = TEXT("UV");
	InUV.Input.Connect(0, UV);

	// Additive: light adds in proportion to alpha. Translucent: the colour over a masked alpha.
	UMaterialExpressionCustom* Emissive = AddCustom(Material, 0, TEXT("Emissive"), CMOT_Float3,
		Spec.bAdditive ? TEXT("return Color.rgb * Color.a * Mask;") : TEXT("return Color.rgb;"), Color, Mask);
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
	if (!Spec.bAdditive)
	{
		UMaterialExpressionCustom* Opacity =
			AddCustom(Material, 150, TEXT("Opacity"), CMOT_Float1, TEXT("return saturate(Color.a * Mask);"), Color, Mask);
		Material->GetEditorOnlyData()->Opacity.Connect(0, Opacity);
	}

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

// ---- Niagara distributions -------------------------------------------------------------------------------

void SetValues(FNiagaraDistributionBase& D, ENiagaraDistributionMode Mode, std::initializer_list<float> Values)
{
	D.Mode = Mode;
	D.ChannelConstantsAndRanges = TArray<float>(Values);
	D.ChannelCurves.Reset();
	D.UpdateValuesFromDistribution();
}

void SetFloat(FNiagaraDistributionBase& D, float V)
{
	SetValues(D, ENiagaraDistributionMode::UniformConstant, {V});
}

// Uniform range (one random number for every channel), or a constant when lo == hi.
void SetRange(FNiagaraDistributionBase& D, const ftcore::VfxRange& R)
{
	if (R.lo == R.hi)
	{
		SetFloat(D, R.lo);
	}
	else
	{
		SetValues(D, ENiagaraDistributionMode::UniformRange, {R.lo, R.hi});
	}
}

void SetVector(FNiagaraDistributionBase& D, const ftcore::VfxVec3& V)
{
	SetValues(D, ENiagaraDistributionMode::NonUniformConstant, {V.x, V.y, V.z});
}

void SetVectorRange(FNiagaraDistributionBase& D, const ftcore::VfxVec3& Lo, const ftcore::VfxVec3& Hi)
{
	if (Lo == Hi)
	{
		SetVector(D, Lo);
	}
	else
	{
		SetValues(D, ENiagaraDistributionMode::NonUniformRange, {Lo.x, Lo.y, Lo.z, Hi.x, Hi.y, Hi.z});
	}
}

void SetVector2(FNiagaraDistributionBase& D, const ftcore::VfxVec2& V)
{
	SetValues(D, ENiagaraDistributionMode::NonUniformConstant, {V.x, V.y});
}

FRichCurve LinearCurve(const TArray<TPair<float, float>>& Keys)
{
	FRichCurve Curve;
	for (const TPair<float, float>& K : Keys)
	{
		Curve.SetKeyInterpMode(Curve.AddKey(K.Key, K.Value), RCIM_Linear);
	}
	return Curve;
}

// Every channel follows the same keys.
void SetCurve(FNiagaraDistributionBase& D, int32 Channels, const std::vector<ftcore::VfxKey>& Keys)
{
	TArray<TPair<float, float>> K;
	for (const ftcore::VfxKey& Key : Keys)
	{
		K.Emplace(Key.t, Key.v);
	}
	D.Mode = ENiagaraDistributionMode::NonUniformCurve;
	D.ChannelConstantsAndRanges.Reset();
	D.ChannelCurves.Reset();
	for (int32 C = 0; C < Channels; ++C)
	{
		D.ChannelCurves.Add(LinearCurve(K));
	}
	D.UpdateValuesFromDistribution();
}

FLinearColor ToLinear(const ftcore::VfxColor& C)
{
	return FLinearColor(C.r, C.g, C.b, C.a);
}

// A colour distribution from stops: one stop is a constant; several are a gradient sampled at `Lookup`.
void SetColorStops(FNiagaraDistributionBase& D, const std::vector<ftcore::VfxColorStop>& Stops, uint8 Lookup)
{
	if (Stops.size() == 1)
	{
		const FLinearColor C = ToLinear(Stops[0].color);
		SetValues(D, ENiagaraDistributionMode::NonUniformConstant, {C.R, C.G, C.B, C.A});
		return;
	}
	D.Mode = ENiagaraDistributionMode::ColorGradient;
	D.ChannelConstantsAndRanges.Reset();
	D.ChannelCurves.Reset();
	for (int32 C = 0; C < 4; ++C)
	{
		TArray<TPair<float, float>> K;
		for (const ftcore::VfxColorStop& S : Stops)
		{
			const float V[4] = {S.color.r, S.color.g, S.color.b, S.color.a};
			K.Emplace(S.t, V[C]);
		}
		D.ChannelCurves.Add(LinearCurve(K));
	}
	D.SetLookupValueMode(Lookup);
	D.UpdateValuesFromDistribution();
}

void Bind(FNiagaraDistributionBase& D, const FNiagaraVariableBase& Variable)
{
	D.Mode = ENiagaraDistributionMode::Binding;
	D.ParameterBinding = Variable;
}

FNiagaraVariable SpeedScaleVariable()
{
	return FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(FString(TEXT("User.")) + UTF8_TO_TCHAR(ftcore::kVfxUserSpeedScale)));
}

FNiagaraVariable ShapeScaleVariable()
{
	return FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), FName(FString(TEXT("User.")) + UTF8_TO_TCHAR(ftcore::kVfxUserShapeScale)));
}

FVector3f ToVector(const ftcore::VfxVec3& V)
{
	return FVector3f(V.x, V.y, V.z);
}

// ---- emitters --------------------------------------------------------------------------------------------

template <typename T>
T* Module(UNiagaraStatelessEmitter* Emitter)
{
	T* M = Cast<T>(Emitter->GetModule(T::StaticClass()));
	if (!M)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: the template emitter has no %s."), *T::StaticClass()->GetName());
	}
	return M;
}

template <typename TValue>
TValue* EmitterProperty(UNiagaraStatelessEmitter* Emitter, const TCHAR* Name)
{
	const FProperty* Property = UNiagaraStatelessEmitter::StaticClass()->FindPropertyByName(Name);
	check(Property);
	return Property->ContainerPtrToValuePtr<TValue>(Emitter);
}

bool BuildEmitter(UNiagaraStatelessEmitter* Emitter, const ftcore::VfxNiagaraEmitter& P, UMaterialInterface* const Materials[4])
{
	using namespace ftcore;

	UNiagaraStatelessModule_InitializeParticle* Init = Module<UNiagaraStatelessModule_InitializeParticle>(Emitter);
	UNiagaraStatelessModule_ShapeLocation* Shape = Module<UNiagaraStatelessModule_ShapeLocation>(Emitter);
	UNiagaraStatelessModule_AddVelocity* Velocity = Module<UNiagaraStatelessModule_AddVelocity>(Emitter);
	UNiagaraStatelessModule_GravityForce* Gravity = Module<UNiagaraStatelessModule_GravityForce>(Emitter);
	UNiagaraStatelessModule_Drag* Drag = Module<UNiagaraStatelessModule_Drag>(Emitter);
	UNiagaraStatelessModule_AccelerationForce* Accel = Module<UNiagaraStatelessModule_AccelerationForce>(Emitter);
	UNiagaraStatelessModule_CurlNoiseForce* Curl = Module<UNiagaraStatelessModule_CurlNoiseForce>(Emitter);
	UNiagaraStatelessModule_ScaleColor* ScaleColor = Module<UNiagaraStatelessModule_ScaleColor>(Emitter);
	UNiagaraStatelessModule_ScaleSpriteSize* ScaleSize = Module<UNiagaraStatelessModule_ScaleSpriteSize>(Emitter);
	UNiagaraStatelessModule_ScaleSpriteSizeBySpeed* BySpeed = Module<UNiagaraStatelessModule_ScaleSpriteSizeBySpeed>(Emitter);
	UNiagaraStatelessModule_SpriteFacingAndAlignment* Facing = Module<UNiagaraStatelessModule_SpriteFacingAndAlignment>(Emitter);
	UNiagaraStatelessModule_SpriteRotationRate* Spin = Module<UNiagaraStatelessModule_SpriteRotationRate>(Emitter);
	UNiagaraStatelessModule_SolveVelocitiesAndForces* Solve = Module<UNiagaraStatelessModule_SolveVelocitiesAndForces>(Emitter);
	if (!Init || !Shape || !Velocity || !Gravity || !Drag || !Accel || !Curl || !ScaleColor || !ScaleSize || !BySpeed ||
		!Facing || !Spin || !Solve)
	{
		return false;
	}
	// Start from nothing but the particle setup and the solver.
	for (UNiagaraStatelessModule* M : Emitter->GetModules())
	{
		if (M)
		{
			M->SetIsModuleEnabled(M == Init || M == Solve);
		}
	}

	// Emitter state, spawning and bounds.
	FNiagaraEmitterStateData* State = EmitterProperty<FNiagaraEmitterStateData>(Emitter, TEXT("EmitterState"));
	State->LoopBehavior = P.loop ? ENiagaraLoopBehavior::Infinite : ENiagaraLoopBehavior::Once;
	State->LoopDuration.InitConstant(P.loopDuration);
	State->bLoopDelayEnabled = false;

	TArray<FNiagaraStatelessSpawnInfo>* SpawnInfos = EmitterProperty<TArray<FNiagaraStatelessSpawnInfo>>(Emitter, TEXT("SpawnInfos"));
	SpawnInfos->Reset();
	if (P.rate > 0.f)
	{
		FNiagaraStatelessSpawnInfo& Info = Emitter->AddSpawnInfo();
		Info.Type = ENiagaraStatelessSpawnInfoType::Rate;
		Info.Rate.InitConstant(P.rate);
	}
	for (const VfxNiagaraBurst& B : P.bursts)
	{
		FNiagaraStatelessSpawnInfo& Info = Emitter->AddSpawnInfo();
		Info.Type = ENiagaraStatelessSpawnInfoType::Burst;
		Info.SpawnTime = B.time;
		Info.Amount.InitConstant(int32(B.countLo));
		if (B.countHi != B.countLo)
		{
			Info.Amount.Mode = ENiagaraDistributionMode::UniformRange;
			Info.Amount.Max = int32(B.countHi);
		}
	}
	*EmitterProperty<FBox>(Emitter, TEXT("FixedBounds")) =
		FBox(FVector(ToVector(P.boundsMin)), FVector(ToVector(P.boundsMax)));

	// InitializeParticle.
	SetRange(Init->LifetimeDistribution, P.lifetime);
	SetRange(Init->SpriteSizeDistribution, P.spriteSize);
	SetRange(Init->SpriteRotationDistribution, P.spriteRotation);
	SetColorStops(Init->ColorDistribution, P.startColor, P.startColorRandom ? 0xff : 0);

	// ShapeLocation.
	if (P.shape != VfxNiagaraShape::None)
	{
		Shape->SetIsModuleEnabled(true);
		switch (P.shape)
		{
		case VfxNiagaraShape::Sphere:
			Shape->ShapePrimitive = ENSM_ShapePrimitive::Sphere;
			SetFloat(Shape->SphereRadius, P.sphereRadius);
			break;
		case VfxNiagaraShape::Box:
			Shape->ShapePrimitive = ENSM_ShapePrimitive::Box;
			SetVector(Shape->BoxSize, P.boxSize);
			break;
		case VfxNiagaraShape::Ring:
			Shape->ShapePrimitive = ENSM_ShapePrimitive::Ring;
			SetFloat(Shape->RingRadius, P.ringRadius);
			SetFloat(Shape->DiscCoverage, P.discCoverage);
			break;
		case VfxNiagaraShape::None:
			break;
		}
		SetVector(Shape->TransformOffset, P.shapeOffset);
		Bind(Shape->ShapeScale, ShapeScaleVariable());
	}

	// AddVelocity, scaled by User.SpeedScale.
	if (P.velocity != VfxNiagaraVelocity::None)
	{
		Velocity->SetIsModuleEnabled(true);
		switch (P.velocity)
		{
		case VfxNiagaraVelocity::Linear:
			Velocity->VelocityType = ENSM_VelocityType::Linear;
			SetVectorRange(Velocity->LinearVelocityDistribution, P.linearLo, P.linearHi);
			Bind(Velocity->LinearVelocityScale, SpeedScaleVariable());
			break;
		case VfxNiagaraVelocity::FromPoint:
			Velocity->VelocityType = ENSM_VelocityType::FromPoint;
			SetRange(Velocity->PointVelocityDistribution, P.speed);
			Velocity->PointOrigin = ToVector(P.pointOrigin);
			Bind(Velocity->PointVelocityScale, SpeedScaleVariable());
			break;
		case VfxNiagaraVelocity::Cone:
			Velocity->VelocityType = ENSM_VelocityType::InCone;
			SetRange(Velocity->ConeVelocityDistribution, P.speed);
			Velocity->ConeRotationType = ENSM_ConeRotationType::Direction;
			SetVector(Velocity->ConeDirection, P.coneDirection);
			Velocity->ConeAngle = P.coneAngle;
			Velocity->InnerCone = 0.f;
			Bind(Velocity->ConeVelocityScale, SpeedScaleVariable());
			break;
		case VfxNiagaraVelocity::None:
			break;
		}
	}

	// Forces.
	if (!(P.gravity == VfxVec3{}))
	{
		Gravity->SetIsModuleEnabled(true);
		SetVector(Gravity->GravityDistribution, P.gravity);
	}
	if (P.drag > 0.f)
	{
		Drag->SetIsModuleEnabled(true);
		SetFloat(Drag->DragDistribution, P.drag);
	}
	if (!(P.acceleration == VfxVec3{}))
	{
		Accel->SetIsModuleEnabled(true);
		SetVector(Accel->AccelerationDistribution, P.acceleration);
	}
	if (P.curlNoise)
	{
		Curl->SetIsModuleEnabled(true);
		Curl->NoiseStrength = P.noiseStrength;
		Curl->NoiseFrequency = P.noiseFrequency;
	}

	// Over life.
	if (!P.sizeOverLife.empty())
	{
		ScaleSize->SetIsModuleEnabled(true);
		SetCurve(ScaleSize->ScaleDistribution, 2, P.sizeOverLife);
	}
	if (!P.colorOverLife.empty())
	{
		ScaleColor->SetIsModuleEnabled(true);
		SetColorStops(ScaleColor->ScaleDistribution, P.colorOverLife, 0); // Particles.NormalizedAge
	}
	if (P.rotationRate.lo != 0.f || P.rotationRate.hi != 0.f)
	{
		Spin->SetIsModuleEnabled(true);
		SetRange(Spin->RotationRateDistribution, P.rotationRate);
	}
	if (P.scaleBySpeed)
	{
		BySpeed->SetIsModuleEnabled(true);
		SetFloat(BySpeed->VelocityThreshold, P.speedThreshold);
		SetVector2(BySpeed->MinScaleFactor, P.minScale);
		SetVector2(BySpeed->MaxScaleFactor, P.maxScale);
	}

	// Renderer.
	UNiagaraSpriteRendererProperties* Sprite = nullptr;
	for (UNiagaraRendererProperties* R : Emitter->GetRenderers())
	{
		Sprite = Sprite ? Sprite : Cast<UNiagaraSpriteRendererProperties>(R);
	}
	if (!Sprite)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: the template emitter has no sprite renderer."));
		return false;
	}
	const bool bSpark = P.sprite == VfxSprite::Spark;
	Sprite->Material = Materials[(bSpark ? 2 : 0) + (P.additive ? 1 : 0)];
	Sprite->SortMode = P.additive ? ENiagaraSortMode::None : ENiagaraSortMode::ViewDistance;
	Sprite->FacingMode = ENiagaraSpriteFacingMode::FaceCamera;
	Sprite->Alignment = ENiagaraSpriteAlignment::Unaligned;
	switch (P.facing)
	{
	case VfxNiagaraFacing::Camera:
		break;
	case VfxNiagaraFacing::VelocityAligned:
		Sprite->Alignment = ENiagaraSpriteAlignment::VelocityAligned;
		break;
	case VfxNiagaraFacing::Horizontal:
		Sprite->FacingMode = ENiagaraSpriteFacingMode::CustomFacingVector;
		Facing->SetIsModuleEnabled(true);
		Facing->bSpriteFacingEnabled = true;
		Facing->bSpriteAlignmentEnabled = false;
		SetVector(Facing->SpriteFacing, VfxVec3{0.f, 0.f, 1.f});
		break;
	case VfxNiagaraFacing::Vertical:
		Sprite->Alignment = ENiagaraSpriteAlignment::CustomAlignment;
		Facing->SetIsModuleEnabled(true);
		Facing->bSpriteFacingEnabled = false;
		Facing->bSpriteAlignmentEnabled = true;
		SetVector(Facing->SpriteAlignment, VfxVec3{0.f, 0.f, 1.f});
		break;
	}
	return true;
}

UNiagaraSystem* BuildSystem(UPackage* Package, UNiagaraSystem* Template, const ftcore::VfxNiagaraPlan& Plan,
	UMaterialInterface* const Materials[4])
{
	const FString Name = FPackageName::GetShortName(Package->GetName());
	UNiagaraSystem* System = CastChecked<UNiagaraSystem>(StaticDuplicateObject(Template, Package, *Name,
		RF_Public | RF_Standalone | RF_Transactional, UNiagaraSystem::StaticClass()));
	System->TemplateAssetDescription = FText();
	System->Category = FText();

	// One emitter handle per plan emitter, all copies of the template's.
	const FNiagaraEmitterHandle First = System->GetEmitterHandles()[0];
	for (size_t I = 1; I < Plan.emitters.size(); ++I)
	{
		System->DuplicateEmitterHandle(First, FName(UTF8_TO_TCHAR(Plan.emitters[I].name.c_str())));
	}
	System->GetEmitterHandles()[0].SetName(FName(UTF8_TO_TCHAR(Plan.emitters[0].name.c_str())), *System);
	if (System->GetEmitterHandles().Num() != int32(Plan.emitters.size()))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: %s has %d emitter handles, expected %d."), *Name,
			System->GetEmitterHandles().Num(), int32(Plan.emitters.size()));
		return nullptr;
	}

	// The runtime knobs, at their identity values.
	FNiagaraUserRedirectionParameterStore& User = System->GetExposedParameters();
	const FNiagaraVariable Speed = SpeedScaleVariable();
	const FNiagaraVariable ShapeScale = ShapeScaleVariable();
	User.AddParameter(Speed, true, false);
	User.SetParameterValue(1.0f, Speed);
	User.AddParameter(ShapeScale, true, false);
	User.SetParameterValue(FVector3f(1.0f), ShapeScale);

	for (int32 I = 0; I < System->GetEmitterHandles().Num(); ++I)
	{
		UNiagaraStatelessEmitter* Emitter = System->GetEmitterHandles()[I].GetStatelessEmitter();
		if (!Emitter || !BuildEmitter(Emitter, Plan.emitters[size_t(I)], Materials))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't build emitter %d of %s."), I, *Name);
			return nullptr;
		}
		Emitter->PostEditChange();
	}

	System->RequestCompile(false);
	System->WaitForCompilationComplete();
	return System;
}

// Logs what a saved system holds (for -Verify, after a reload from disk).
void Describe(const UNiagaraSystem* System)
{
	TArray<FNiagaraVariable> Params;
	System->GetExposedParameters().GetParameters(Params);
	FString Names;
	for (const FNiagaraVariable& V : Params)
	{
		Names += V.GetName().ToString() + TEXT(" ");
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: %s user parameters: %s"), *System->GetName(), *Names);
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		UNiagaraStatelessEmitter* Emitter = Handle.GetStatelessEmitter();
		if (!Emitter)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: %s/%s is not a Lightweight emitter."), *System->GetName(),
				*Handle.GetName().ToString());
			continue;
		}
		FString Modules;
		for (const UNiagaraStatelessModule* M : Emitter->GetModules())
		{
			if (M && M->IsModuleEnabled())
			{
				Modules += M->GetClass()->GetName().Replace(TEXT("NiagaraStatelessModule_"), TEXT("")) + TEXT(" ");
			}
		}
		FString Bindings;
		for (const UNiagaraStatelessModule* M : Emitter->GetModules())
		{
			if (M && M->IsModuleEnabled())
			{
				const FString ModuleName = M->GetClass()->GetName().Replace(TEXT("NiagaraStatelessModule_"), TEXT(""));
				FNiagaraDistributionBase::ForEachParameterBinding(M,
					[&Bindings, &ModuleName](const FNiagaraVariableBase& V)
					{ Bindings += ModuleName + TEXT(":") + V.GetName().ToString() + TEXT(" "); });
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx:   %s: %d spawn infos; modules %s; bindings %s"),
			*Handle.GetName().ToString(), Emitter->GetNumSpawnInfos(), *Modules, *Bindings);
	}
}

bool LoadEffect(const FString& File, ftcore::VfxEffect& Effect)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't read %s."), *File);
		return false;
	}
	const FTCHARToUTF8 Utf8(*Text);
	std::string Error;
	std::vector<std::string> Warnings;
	const bool bOk = ftcore::importVfxEffect(std::string_view(Utf8.Get(), Utf8.Length()), Effect, &Error, &Warnings);
	for (const std::string& W : Warnings)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildVfx: %s: %s"), *File, UTF8_TO_TCHAR(W.c_str()));
	}
	if (!bOk)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: %s: %s"), *File, UTF8_TO_TCHAR(Error.c_str()));
	}
	return bOk;
}
} // namespace
#endif

UFTBuildVfxCommandlet::UFTBuildVfxCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTBuildVfxCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	TArray<FString> Files;
	const FString DataDir = FPaths::ProjectDir() / TEXT("Data/Vfx");
	IFileManager::Get().FindFiles(Files, *(DataDir / TEXT("*.json")), true, false);
	Files.Sort();
	if (Files.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: no effects in %s."), *DataDir);
		return 1;
	}

	if (FParse::Param(*Params, TEXT("Verify")))
	{
		int32 Missing = 0;
		for (const FString& File : Files)
		{
			const FString Name = TEXT("NS_") + FPaths::GetBaseFilename(File);
			const FString Path = FString(VfxRoot) / Name + TEXT(".") + Name;
			if (const UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path))
			{
				Describe(System);
			}
			else
			{
				UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: couldn't load %s."), *Path);
				++Missing;
			}
		}
		return Missing == 0 ? 0 : 1;
	}

	UNiagaraSystem* Template = LoadObject<UNiagaraSystem>(nullptr, TemplatePath);
	if (!Template || Template->GetEmitterHandles().Num() < 1 || !Template->GetEmitterHandles()[0].GetStatelessEmitter())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx: the Lightweight template %s is missing."), TemplatePath);
		return 1;
	}

	// Read and plan every effect before touching any file.
	TArray<ftcore::VfxNiagaraPlan> Plans;
	for (const FString& File : Files)
	{
		ftcore::VfxEffect Effect;
		if (!LoadEffect(DataDir / File, Effect))
		{
			return 1;
		}
		ftcore::VfxNiagaraPlan& Plan = Plans.AddDefaulted_GetRef();
		ftcore::planVfxNiagara(Effect, Plan);
		Plan.name = TCHAR_TO_UTF8(*FPaths::GetBaseFilename(File)); // the asset is named after the file
		for (const ftcore::VfxNiagaraEmitter& E : Plan.emitters)
		{
			for (const std::string& Gap : E.gaps)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: gap %s/%s: %s"), UTF8_TO_TCHAR(Plan.name.c_str()),
					UTF8_TO_TCHAR(E.name.c_str()), UTF8_TO_TCHAR(Gap.c_str()));
			}
		}
	}

	// Start from nothing: delete every NS_* and the sprite materials up front, before anything loads them.
	TArray<FString> Old;
	IFileManager::Get().FindFiles(Old, *(FPaths::ProjectContentDir() / TEXT("VFX/NS_*.uasset")), true, false);
	for (const FString& File : Old)
	{
		if (!RemoveFile(FPaths::ProjectContentDir() / TEXT("VFX") / File))
		{
			return 1;
		}
	}
	for (const FSpriteMaterialSpec& Spec : SpriteMaterials)
	{
		if (!RemoveFile(PackageFile(MaterialPackage(Spec))))
		{
			return 1;
		}
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFile(MaterialPackage(SpriteMaterials[0]))), true);

	bool bOk = true;
	UMaterialInterface* Materials[4] = {};
	for (int32 I = 0; I < 4; ++I)
	{
		UPackage* Package = CreatePackage(*MaterialPackage(SpriteMaterials[I]));
		UMaterial* Material = BuildSpriteMaterial(Package, SpriteMaterials[I]);
		Materials[I] = Material;
		FAssetCompilingManager::Get().FinishAllCompilation();
		bOk &= Save(Package, Material);
	}
	if (!bOk)
	{
		return 1;
	}

	int32 Emitters = 0;
	for (const ftcore::VfxNiagaraPlan& Plan : Plans)
	{
		const FString PackageName = FString(VfxRoot) / TEXT("NS_") + UTF8_TO_TCHAR(Plan.name.c_str());
		UPackage* Package = CreatePackage(*PackageName);
		UNiagaraSystem* System = BuildSystem(Package, Template, Plan, Materials);
		if (!System)
		{
			return 1;
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (!Save(Package, System))
		{
			return 1;
		}
		Emitters += System->GetEmitterHandles().Num();
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildVfx: built %d systems (%d emitters) and %d sprite materials."), Plans.Num(),
		Emitters, int32(UE_ARRAY_COUNT(SpriteMaterials)));
	return 0;
#else
	UE_LOG(LogForgottenThird, Error, TEXT("FTBuildVfx needs an editor build."));
	return 1;
#endif
}
