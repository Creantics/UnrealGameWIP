#include "FTBuildBackdrop.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FTCellInstancer.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
// The shape mask: kMaskSize pixels over kMaskSideM, centred on the engine origin (the plan's centre), as the ground
// master reads it (ShapeSideCm); ridges from kRingM rings, full at kFullPerMetre of the ring's radius.
constexpr int32 kMaskSize = 1024;
constexpr double kMaskSideM = 3600.0;
constexpr float kRingM = 15.f;
constexpr float kFullPerMetre = 0.25f;

// The skyline: far blocks, how many a band, and the band's depth past the field's edge.
constexpr int32 kSkylineSouth = 220;
constexpr int32 kSkylineNorth = 150;

bool Wants(const TCHAR* Switch)
{
	const TCHAR* Cmd = FCommandLine::Get();
	return FParse::Param(Cmd, TEXT("Backdrop")) || FParse::Param(Cmd, Switch);
}

UMaterialInterface* Paint(UPackage* Package, UMaterialInterface* Base, const TCHAR* Name, const FLinearColor& Color, float Roughness)
{
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package, Name, RF_Public);
	Mic->SetParentEditorOnly(Base);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Color);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), Roughness);
	Mic->PostEditChange();
	return Mic;
}

UStaticMesh* Shape(const TCHAR* Path)
{
	return LoadObject<UStaticMesh>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// A plan point (metres) to the engine's cm.
FVector EngineCm(double Side, double X, double Y, double Z)
{
	const ftcore::MassingVec P = ftcore::massingEnginePoint(Side, X, Y, Z);
	return FVector(P.x * 100.0, P.y * 100.0, P.z * 100.0);
}

FFTCellOptions Far(const TCHAR* Tag)
{
	FFTCellOptions Options;
	Options.Cull = EFTCellCull::None;
	Options.bCastShadow = false;
	Options.Tag = FName(Tag);
	return Options;
}
} // namespace

UMaterialInterface* FTBackdrop::HillGround(UPackage* Package, UMaterialInterface* Ground, const ftcore::HeightField& Field, double Side)
{
	UMaterialInstanceConstant* Mic = Cast<UMaterialInstanceConstant>(Ground);
	if (!Mic || !Wants(TEXT("HillLook")))
	{
		return Ground;
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(kMaskSize * kMaskSize);
	// Row 0 is the engine's -Y (the plan's north), where the material's V starts.
	ftcore::heightShapeMask(Field, Side * 0.5, Side * 0.5, kMaskSideM, kMaskSize, true, kRingM, kFullPerMetre, Bytes.GetData());
	int32 Ridge = 0, Hollow = 0;
	for (const uint8 B : Bytes)
	{
		Ridge += B > 170 ? 1 : 0;
		Hollow += B < 86 ? 1 : 0;
	}
	UTexture2D* Mask = FTScene::NewGroundMask(Package, TEXT("T_Massing_HillShape"), kMaskSize, Bytes.GetData(), RF_Public);
	Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ShapeMask")), Mask);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ShapeSideCm")), static_cast<float>(kMaskSideM * 100.0));
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ShapeRock")), 0.8f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ShapeLift")), 0.8f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("CrestPale")), 0.45f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroVariation")), 0.28f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("HueDrift")), 0.12f);
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RockTint")), FLinearColor(0.78f, 0.78f, 0.76f));
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("GroundTint")), FLinearColor(0.80f, 0.84f, 0.70f));
	Mic->PostEditChange();
	UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -HillLook: shape mask %d px over %.0f m, %.1f%% ridge and %.1f%% hollow pixels."), kMaskSize,
		kMaskSideM, 100.0 * Ridge / Bytes.Num(), 100.0 * Hollow / Bytes.Num());
	return Ground;
}

void FTBackdrop::Dress(UPackage* Package, FFTCellInstancer& Cells, UMaterialInterface* Base, const ftcore::HeightField& Field, double Side,
	const std::vector<ftcore::MassingBox>& Boxes)
{
	const bool bDress = Wants(TEXT("HillDress"));
	const bool bSkyline = Wants(TEXT("Skyline"));
	UStaticMesh* Cube = Shape(TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cone = Shape(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if ((!bDress && !bSkyline) || !Base || !Cube || !Cone || !Field.grid)
	{
		return;
	}
	const TArrayView<const float> NoData;
	// The field's plan rectangle.
	const double X0 = Field.originX, Y0 = Field.originY;
	const double X1 = X0 + Field.spacing * static_cast<double>(Field.samplesX - 1);
	const double Y1 = Y0 + Field.spacing * static_cast<double>(Field.samplesY - 1);

	if (bDress)
	{
		UMaterialInterface* Leaf = Paint(Package, Base, TEXT("MI_Backdrop_Tree"), FLinearColor(0.045f, 0.07f, 0.04f), 0.9f);
		UMaterialInterface* Scrub = Paint(Package, Base, TEXT("MI_Backdrop_Scrub"), FLinearColor(0.16f, 0.15f, 0.09f), 0.95f);
		UMaterialInterface* Steel = Paint(Package, Base, TEXT("MI_Backdrop_Pylon"), FLinearColor(0.30f, 0.31f, 0.32f), 0.6f);
		ftcore::HillDressSpec Spec;
		Spec.cellM = 12.f;
		Spec.maxPoints = 12000;
		const std::vector<ftcore::BackdropPoint> Points = ftcore::hillDress(Field, Spec, X0, Y0, X1, Y1, Boxes.data(), Boxes.size());
		int32 Trees = 0, Shrubs = 0, Pylons = 0;
		const FFTCellOptions Options = Far(TEXT("Backdrop"));
		for (const ftcore::BackdropPoint& P : Points)
		{
			const FVector Foot = EngineCm(Side, P.x, P.y, P.z);
			const FRotator Yaw(0.0, P.yaw, 0.0);
			if (P.kind == ftcore::BackdropKind::Pylon)
			{
				// A mast 30 m high and two cross-arms near its top (the basic shapes are 1 m, centred).
				UMaterialInterface* const M[] = {Steel};
				Cells.Add(Cube, M, FTransform(Yaw, Foot + FVector(0, 0, 1500), FVector(0.9, 0.9, 30.0)), NoData, EFTCellKind::NoCollide, Options);
				Cells.Add(Cube, M, FTransform(Yaw, Foot + FVector(0, 0, 2850), FVector(0.5, 12.0, 0.5)), NoData, EFTCellKind::NoCollide, Options);
				Cells.Add(Cube, M, FTransform(Yaw, Foot + FVector(0, 0, 2350), FVector(0.5, 9.0, 0.5)), NoData, EFTCellKind::NoCollide, Options);
				++Pylons;
			}
			else if (P.kind == ftcore::BackdropKind::Tree)
			{
				const double H = 7.0 * P.scale, W = 3.6 * P.scale;
				UMaterialInterface* const M[] = {Leaf};
				Cells.Add(Cone, M, FTransform(Yaw, Foot + FVector(0, 0, H * 50.0 - 30.0), FVector(W, W, H)), NoData, EFTCellKind::NoCollide, Options);
				++Trees;
			}
			else
			{
				const double H = 1.4 * P.scale, W = 3.0 * P.scale;
				UMaterialInterface* const M[] = {Scrub};
				Cells.Add(Cone, M, FTransform(Yaw, Foot + FVector(0, 0, H * 50.0 - 20.0), FVector(W, W * 0.8, H)), NoData, EFTCellKind::NoCollide, Options);
				++Shrubs;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -HillDress: %d trees, %d scrub, %d pylons (%d instances), kept off %d boxes."), Trees, Shrubs,
			Pylons, Trees + Shrubs + 3 * Pylons, static_cast<int32>(Boxes.size()));
	}

	if (bSkyline)
	{
		UMaterialInterface* Mass = Paint(Package, Base, TEXT("MI_Backdrop_Skyline"), FLinearColor(0.10f, 0.11f, 0.13f), 0.85f);
		UMaterialInterface* Plate = Paint(Package, Base, TEXT("MI_Backdrop_SkylineGround"), FLinearColor(0.08f, 0.08f, 0.08f), 0.95f);
		UMaterialInterface* const M[] = {Mass};
		UMaterialInterface* const G[] = {Plate};
		const FFTCellOptions Options = Far(TEXT("Skyline"));
		// Two bands past the field's south and north edges, over the strip's x, seen from the strip's middle; a dark plate
		// under each at 2 m so their feet stand on ground, not on the sky.
		struct FBand
		{
			double X0, Y0, X1, Y1;
			int32 Count;
			uint32 Seed;
		};
		const double MidX = 0.5 * (X0 + X1), MidY = 0.5 * (Y0 + Y1);
		const FBand Bands[] = {
			{200.0, Y0 - 1100.0, 1250.0, Y0 - 120.0, kSkylineSouth, 0x50u}, // x to the south coast
			{0.0, Y1 + 120.0, 1300.0, Y1 + 900.0, kSkylineNorth, 0x40u},
		};
		int32 Blocks = 0;
		for (const FBand& B : Bands)
		{
			ftcore::SkylineSpec Spec;
			Spec.x0 = B.X0;
			Spec.y0 = B.Y0;
			Spec.x1 = B.X1;
			Spec.y1 = B.Y1;
			Spec.nearX = MidX;
			Spec.nearY = MidY;
			Spec.count = static_cast<size_t>(B.Count);
			Spec.minH = 18.f;
			Spec.maxH = 110.f;
			Spec.seed = B.Seed;
			const FVector Centre = EngineCm(Side, 0.5 * (B.X0 + B.X1), 0.5 * (B.Y0 + B.Y1), 0.0);
			// 20 m thick, its top at 2 m.
			Cells.Add(Cube, G, FTransform(FRotator::ZeroRotator, Centre + FVector(0, 0, 200.0 - 1000.0),
				FVector((B.X1 - B.X0) + 40.0, (B.Y1 - B.Y0) + 40.0, 20.0)), NoData, EFTCellKind::NoCollide, Options);
			for (const ftcore::BackdropBox& Box : ftcore::skylineBand(Spec))
			{
				// Feet 30 m down, so no block shows a base however the ground falls away.
				const double Height = Box.h + 30.0;
				const FVector At = EngineCm(Side, Box.cx, Box.cy, Box.h - 0.5 * Height);
				Cells.Add(Cube, M, FTransform(FRotator(0.0, ftcore::massingEngineYaw(Box.rot), 0.0), At, FVector(Box.sx, Box.sy, Height)), NoData, EFTCellKind::NoCollide,
					Options);
				++Blocks;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -Skyline: %d far blocks in 2 bands past the field's south and north edges."), Blocks);
	}
}

void FTBackdrop::Haze(UWorld* World)
{
	float Density = 0.05f;
	const TCHAR* Cmd = FCommandLine::Get();
	const bool bHaze = FParse::Value(Cmd, TEXT("Haze="), Density) || Wants(TEXT("Haze"));
	if (!World || !bHaze || Density <= 0.f)
	{
		return;
	}
	int32 Fogs = 0;
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		if (UExponentialHeightFogComponent* Fog = It->GetComponent())
		{
			// Thick over the sea and the low ground, thinning by about e every 15 m up, so the far water and the ground's
			// edge fade into it while the near street stays clear.
			FExponentialHeightFogData Second;
			Second.FogDensity = Density;
			Second.FogHeightFalloff = 0.65f;
			Second.FogHeightOffset = 0.f;
			Fog->SetSecondFogData(Second);
			++Fogs;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -Haze: a second fog layer at %.3f on %d height fogs."), Density, Fogs);
}
