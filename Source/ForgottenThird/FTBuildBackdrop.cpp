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
#include "ftcore/WindowFacade.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

namespace
{
// The shape mask: kMaskSize pixels over kMaskSideM, centred on the engine origin (the plan's centre), as the ground
// master reads it (ShapeSideCm); ridges from kRingM rings, full at kFullPerMetre of the ring's radius.
constexpr int32 kMaskSize = 1024;
constexpr double kMaskSideM = 3600.0;
constexpr float kRingM = 15.f;
constexpr float kFullPerMetre = 0.25f;
// The planted hill: ground from kPlantFromM over the datum takes the green, fully kPlantRampM higher.
constexpr float kPlantFromM = 14.f;
constexpr float kPlantRampM = 8.f;
// Planted up to this slope (rise over run), bare from the next: the ground master turns slopes from about 0.75 to rock.
constexpr float kPlantSteepFrom = 0.55f;
constexpr float kPlantSteepTo = 0.75f;
// The ground master's RockAltitudeRunCm on the hill: the height over -HillRock's by which even ground has joined the rock.
constexpr float kHillRockRunCm = 40000.f;

// The aviation lamps' glow at night (the lamp head master's LampStrength; the street lamps' heads take 8).
constexpr float kAviationStrength = 40.f;

// The skyline: far blocks and land plates, how many a band together (370 in all, as before the plates), past the field's
// south and north edges. Each band starts kSkylineGapM past the edge and ends kSkylineDepthM past it; its land (strips
// of about kSkylineStripM, ftcore::skylineSeat) runs from the edge itself, kSkylineMarginM further out than the blocks,
// and down to kSkylinePlateFootM under the datum. A block's foot goes kSkylineFootM under its plate's top.
constexpr int32 kSkylineSouth = 200;
constexpr int32 kSkylineNorth = 170;
constexpr double kSkylineGapM = 40.0;
constexpr double kSkylineDepthM = 600.0;
constexpr double kSkylineMarginM = 40.0;
constexpr double kSkylineStripM = 220.0;
constexpr double kSkylinePlateFootM = 5.0;
constexpr double kSkylineFootM = 100.0;
// A far block from this height wears the office grid, a lower one the apartment's.
constexpr float kSkylineOfficeM = 45.f;

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
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("CrestPale")), 0.15f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroVariation")), 0.28f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("HueDrift")), 0.12f);
	// The rock keeps to the steep faces and the high ridges: the altitude band's run is long (kHillRockRunCm), so even
	// ground far over -HillRock's height stays planted and only a ridge (the shape mask) or a slope turns to stone. With
	// the master's 40 m run every gentle slope over that height wore the rock and the hill read as bare sand.
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RockAltitudeRunCm")), kHillRockRunCm);
	// A cooler, greyer stone (the rock photo is ochre), which the cliff boxes' faces take too (RockFace reads it).
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RockTint")), FLinearColor(0.60f, 0.68f, 0.64f));
	// Planted, never bare sand: the ground's tint is green, and the plant mask (ftcore::hillPlantMask) in the master's
	// FlowMask slot takes the low ground (the wards) back toward the ground's own grey through WetRich, so only the hill
	// is green. The master's wet darkening and smoothing stay off.
	TArray<uint8> Plant;
	Plant.SetNumUninitialized(kMaskSize * kMaskSize);
	ftcore::hillPlantMask(Field, Side * 0.5, Side * 0.5, kMaskSideM, kMaskSize, true, kPlantFromM, kPlantRampM, kPlantSteepFrom, kPlantSteepTo,
		Plant.GetData());
	int32 Planted = 0;
	for (const uint8 B : Plant)
	{
		Planted += B < 128 ? 1 : 0;
	}
	UTexture2D* PlantMask = FTScene::NewGroundMask(Package, TEXT("T_Massing_HillPlant"), kMaskSize, Plant.GetData(), RF_Public);
	Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("FlowMask")), PlantMask);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("FlowStart")), 0.2f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("FlowEnd")), 0.8f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WetRich")), -0.85f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WetDarken")), 0.f);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WetSmooth")), 0.f);
	// A deeper scrub green than a lawn's, near the planting's own tints so the clumps read as thicker growth on it.
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("GroundTint")), FLinearColor(0.36f, 0.66f, 0.27f));
	Mic->PostEditChange();
	UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -HillLook: %.1f%% of the mask planted (ground over %.0f m)."),
		100.0 * Planted / Plant.Num(), static_cast<double>(kPlantFromM));
	UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -HillLook: shape mask %d px over %.0f m, %.1f%% ridge and %.1f%% hollow pixels."), kMaskSize,
		kMaskSideM, 100.0 * Ridge / Bytes.Num(), 100.0 * Hollow / Bytes.Num());
	// The share of the field the ground master's slope alone turns to rock (from a slope of about 0.75, 37 degrees).
	const double FieldX1 = Field.originX + Field.spacing * static_cast<double>(Field.samplesX - 1);
	const double FieldY1 = Field.originY + Field.spacing * static_cast<double>(Field.samplesY - 1);
	UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -HillLook: %.1f%% of the field is steep enough for the ground's rock."),
		100.0 * ftcore::heightSteepShare(Field, Field.originX, Field.originY, FieldX1, FieldY1, 4.0, 0.75));
	return Ground;
}

UMaterialInterface* FTBackdrop::RockFace(UPackage* Package, UMaterialInterface* Wear, const ftcore::MassingBox& Box, UMaterialInterface* Ground)
{
	if (!Package || !Wear || !Ground || !ftcore::backdropRockFace(Box) || !(Wants(TEXT("RockFaces")) || Wants(TEXT("HillLook"))))
	{
		return nullptr;
	}
	// One instance per surface instance, made on the first box that wears it.
	static TMap<const UMaterialInterface*, TWeakObjectPtr<UMaterialInstanceConstant>> Made;
	if (const TWeakObjectPtr<UMaterialInstanceConstant>* Found = Made.Find(Wear))
	{
		if (Found->IsValid() && Found->Get()->GetOuter() == Package)
		{
			return Found->Get();
		}
	}
	UTexture* Maps[3] = {};
	const TCHAR* const From[3] = {TEXT("RockAlbedo"), TEXT("RockNormal"), TEXT("RockArm")};
	const TCHAR* const To[3] = {TEXT("AlbedoTex"), TEXT("NormalTex"), TEXT("ArmTex")};
	for (int32 M = 0; M < 3; ++M)
	{
		if (!Ground->GetTextureParameterValue(FHashedMaterialParameterInfo(From[M]), Maps[M]) || !Maps[M])
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Backdrop: -RockFaces: %s holds no %s; the rock faces keep their surface."), *Ground->GetName(), From[M]);
			return nullptr;
		}
	}
	const ftcore::RockFaceLook Look;
	FLinearColor Tint(0.78f, 0.78f, 0.76f);
	Ground->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("RockTint")), Tint);
	float Tile = 0.f, Rough = -1.f;
	Ground->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("RockTileCm")), Tile);
	Ground->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("RockRoughness")), Rough);
	UMaterialInstanceConstant* Mic = NewObject<UMaterialInstanceConstant>(Package,
		MakeUniqueObjectName(Package, UMaterialInstanceConstant::StaticClass(), FName(TEXT("MI_Backdrop_RockFace"))), RF_Public);
	Mic->SetParentEditorOnly(Wear);
	for (int32 M = 0; M < 3; ++M)
	{
		Mic->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(To[M]), Maps[M]);
	}
	Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), Tint * Look.shade);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TileCm")), FMath::Max(Tile, Look.tileCm));
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RoughnessOverride")), Rough);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("NormalStrength")), Look.normalStrength);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Variation")), Look.variation);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("VariationScale")), Look.variationScale);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroVariation")), Look.macroVariation);
	Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MacroScale")), Look.macroScale);
	Mic->PostEditChange();
	Made.Add(Wear, Mic);
	UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -RockFaces: %s wears %s's rock (%s) over %s, %.0f cm a tile."), *Mic->GetName(),
		*Ground->GetName(), *Maps[0]->GetName(), *Wear->GetName(), FMath::Max(Tile, Look.tileCm));
	return Mic;
}

void FTBackdrop::Dress(UPackage* Package, FFTCellInstancer& Cells, UMaterialInterface* Base, const ftcore::HeightField& Field, double Side,
	const std::vector<ftcore::MassingBox>& Boxes, UMaterialInterface* SkylineFacade)
{
	const bool bDress = Wants(TEXT("HillDress"));
	const bool bSkyline = Wants(TEXT("Skyline"));
	const bool bAviation = Wants(TEXT("AviationLamps"));
	UStaticMesh* Cube = Shape(TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Lump = Shape(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if ((!bDress && !bSkyline && !bAviation) || !Base || !Cube || !Lump || !Field.grid)
	{
		return;
	}
	const TArrayView<const float> NoData;
	// The field's plan rectangle.
	const double X0 = Field.originX, Y0 = Field.originY;
	const double X1 = X0 + Field.spacing * static_cast<double>(Field.samplesX - 1);
	const double Y1 = Y0 + Field.spacing * static_cast<double>(Field.samplesY - 1);
	// The far blocks placed, for the aviation lamps on the tallest.
	std::vector<ftcore::BackdropBox> FarBlocks;

	if (bDress)
	{
		// Planting tints, both green: canopy green and a fresh mid green, a step under the hill's ground green so a clump
		// reads as thicker growth on it, not a dark dot on sand. Two materials, not one a tint of
		// ftcore::kBackdropScrubTints: the third tint takes the first, which saves a component in every hill cell.
		// Trunks in a bark brown.
		UMaterialInterface* const ScrubA = Paint(Package, Base, TEXT("MI_Backdrop_Scrub"), FLinearColor(0.085f, 0.17f, 0.05f), 0.85f);
		UMaterialInterface* const ScrubB = Paint(Package, Base, TEXT("MI_Backdrop_ScrubDry"), FLinearColor(0.13f, 0.23f, 0.065f), 0.88f);
		UMaterialInterface* const Scrub[ftcore::kBackdropScrubTints] = {ScrubA, ScrubB, ScrubA};
		UMaterialInterface* Bark = Paint(Package, Base, TEXT("MI_Backdrop_Bark"), FLinearColor(0.07f, 0.055f, 0.04f), 0.9f);
		UMaterialInterface* Steel = Paint(Package, Base, TEXT("MI_Backdrop_Pylon"), FLinearColor(0.30f, 0.31f, 0.32f), 0.6f);
		ftcore::HillDressSpec Spec;
		Spec.cellM = 12.f;
		Spec.maxPoints = 12000;
		const std::vector<ftcore::BackdropPoint> Points = ftcore::hillDress(Field, Spec, X0, Y0, X1, Y1, Boxes.data(), Boxes.size());
		int32 Clumps = 0, Lumps = 0, Pylons = 0, Trunks = 0;
		ftcore::BackdropLump Parts[ftcore::kBackdropClumpMax];
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
			else
			{
				// Trees (a trunk under a rounded crown) and rounded bushes, never cones.
				const std::size_t N = ftcore::backdropClump(P, Parts);
				for (std::size_t K = 0; K < N; ++K)
				{
					const ftcore::BackdropLump& L = Parts[K];
					const FVector At = EngineCm(Side, P.x + L.dx, P.y + L.dy, P.z) + FVector(0, 0, L.dz * 100.0);
					UMaterialInterface* const M[] = {L.trunk ? Bark : Scrub[L.tint]};
					Cells.Add(L.trunk ? Cube : Lump, M, FTransform(FRotator(0.0, L.yaw, 0.0), At, FVector(L.sx, L.sy, L.sz)), NoData,
						EFTCellKind::NoCollide, Options);
					Trunks += L.trunk ? 1 : 0;
				}
				Lumps += static_cast<int32>(N);
				Clumps += N ? 1 : 0;
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -HillDress: %d planting clumps (%d trees, %d parts), %d pylons (%d instances), kept off %d boxes."),
			Clumps, Trunks, Lumps, Pylons, Lumps + 3 * Pylons, static_cast<int32>(Boxes.size()));
	}

	if (bSkyline)
	{
		// The far blocks wear the build's window instance (SkylineFacade: a grid of windows, some lit at night) when the
		// build has one, else the plain dark mass (-SkylinePlain keeps the mass).
		const bool bFacade = SkylineFacade && !FParse::Param(FCommandLine::Get(), TEXT("SkylinePlain"));
		UMaterialInterface* Mass =
			bFacade ? SkylineFacade : Paint(Package, Base, TEXT("MI_Backdrop_Skyline"), FLinearColor(0.10f, 0.11f, 0.13f), 0.85f);
		// The land under them: paved grey where it's low, the hill's green from the planted height up.
		UMaterialInterface* Plate = Paint(Package, Base, TEXT("MI_Backdrop_SkylineGround"), FLinearColor(0.17f, 0.17f, 0.16f), 0.95f);
		UMaterialInterface* PlateHill = Paint(Package, Base, TEXT("MI_Backdrop_SkylineHill"), FLinearColor(0.12f, 0.21f, 0.09f), 0.95f);
		UMaterialInterface* const M[] = {Mass};
		const FFTCellOptions Options = Far(TEXT("Skyline"));
		// Two bands past the field's south and north edges, seen from the field's middle. Each stands on land plates that
		// carry on from the field's own edge at the edge's height (ftcore::skylineSeat), over the reach of the edge that
		// is land: no block and no plate stands in open water.
		struct FBand
		{
			double EdgeY, Out; // the field's edge row, and the way out of the field along plan y (-1 south, +1 north)
			int32 Count;
			uint32 Seed;
		};
		const double MidX = 0.5 * (X0 + X1), MidY = 0.5 * (Y0 + Y1);
		const FBand Bands[] = {
			{Y0, -1.0, kSkylineSouth, 0x50u},
			{Y1, 1.0, kSkylineNorth, 0x40u},
		};
		int32 Blocks = 0, Plates = 0;
		for (const FBand& B : Bands)
		{
			ftcore::SkylineSeatSpec Seat;
			Seat.x0 = X0;
			Seat.x1 = X1;
			Seat.edgeY = B.EdgeY;
			Seat.stripM = kSkylineStripM;
			ftcore::SkylineStrip Strips[ftcore::kSkylineStripMax];
			const std::size_t StripCount = ftcore::skylineSeat(Field, Seat, Strips);
			if (StripCount == 0)
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -Skyline: no land along the field's edge at y %.0f; no band there."), B.EdgeY);
				continue;
			}
			const double LandX0 = static_cast<double>(Strips[0].x0), LandX1 = static_cast<double>(Strips[StripCount - 1].x1);
			const double PlateDepth = kSkylineDepthM + kSkylineMarginM;
			float Lowest = Strips[0].topM, Highest = Strips[0].topM;
			for (std::size_t S = 0; S < StripCount; ++S)
			{
				const ftcore::SkylineStrip& Strip = Strips[S];
				const double Top = static_cast<double>(Strip.topM);
				const double Thick = Top + kSkylinePlateFootM;
				const double Width = static_cast<double>(Strip.x1) - static_cast<double>(Strip.x0);
				const FVector Centre = EngineCm(Side, 0.5 * (static_cast<double>(Strip.x0) + static_cast<double>(Strip.x1)),
					B.EdgeY + B.Out * 0.5 * PlateDepth, Top - 0.5 * Thick);
				UMaterialInterface* const G[] = {Strip.topM >= kPlantFromM ? PlateHill : Plate};
				Cells.Add(Cube, G, FTransform(FRotator::ZeroRotator, Centre, FVector(Width, PlateDepth, Thick)), NoData, EFTCellKind::NoCollide,
					Options);
				Lowest = FMath::Min(Lowest, Strip.topM);
				Highest = FMath::Max(Highest, Strip.topM);
				++Plates;
			}
			ftcore::SkylineSpec Spec;
			Spec.x0 = LandX0;
			Spec.x1 = LandX1;
			Spec.y0 = FMath::Min(B.EdgeY + B.Out * kSkylineGapM, B.EdgeY + B.Out * kSkylineDepthM);
			Spec.y1 = FMath::Max(B.EdgeY + B.Out * kSkylineGapM, B.EdgeY + B.Out * kSkylineDepthM);
			Spec.nearX = MidX;
			Spec.nearY = MidY;
			// The plates come out of the band's count: the backdrop adds no instance.
			Spec.count = static_cast<size_t>(FMath::Max(B.Count - static_cast<int32>(StripCount), 1));
			Spec.minH = 18.f;
			Spec.maxH = 110.f;
			Spec.minW = 18.f;
			Spec.maxW = 50.f;
			Spec.seed = B.Seed;
			uint32 Index = 0;
			for (const ftcore::BackdropBox& Box : ftcore::skylineBand(Spec))
			{
				// On its plate, its foot kSkylineFootM under the plate's top: no base shows where the plates step.
				const double Ground = static_cast<double>(ftcore::skylineSeatAt(Strips, StripCount, static_cast<double>(Box.cx)));
				const double Height = static_cast<double>(Box.h) + kSkylineFootM;
				const FVector At = EngineCm(Side, Box.cx, Box.cy, Ground + static_cast<double>(Box.h) - 0.5 * Height);
				const FTransform Place(FRotator(0.0, ftcore::massingEngineYaw(Box.rot), 0.0), At, FVector(Box.sx, Box.sy, Height));
				if (bFacade)
				{
					// The window master's custom data (FTScene::WindowData*): a seed under 2^24, no street face, each face's
					// first storey at the plate's top, the window type, the wall as photographed, the first roof finish.
					float Data[FTScene::WindowDataCountR5] = {};
					Data[FTScene::WindowDataSeed] = static_cast<float>((B.Seed * 7919u + Index * 2671u + 17u) & 0xFFFFFFu);
					Data[FTScene::WindowDataStreet] = 0.f;
					for (int32 F = 0; F < 4; ++F)
					{
						Data[FTScene::WindowDataBase + F] = static_cast<float>(kSkylineFootM);
					}
					Data[FTScene::WindowDataType] =
						static_cast<float>(Box.h >= kSkylineOfficeM ? ftcore::WindowType::Office : ftcore::WindowType::Apartment);
					Cells.Add(Cube, M, Place, MakeArrayView(Data), EFTCellKind::NoCollide, Options);
				}
				else
				{
					Cells.Add(Cube, M, Place, NoData, EFTCellKind::NoCollide, Options);
				}
				// For the aviation lamps: the block's roof over the datum.
				ftcore::BackdropBox Roof = Box;
				Roof.h = static_cast<float>(Ground) + Box.h;
				FarBlocks.push_back(Roof);
				++Blocks;
				++Index;
			}
			UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -Skyline: band past y %.0f: land x %.0f to %.0f in %d plates (tops %.1f to %.1f m), %d blocks."),
				B.EdgeY, LandX0, LandX1, static_cast<int32>(StripCount), static_cast<double>(Lowest), static_cast<double>(Highest),
				static_cast<int32>(Index));
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -Skyline: %d far blocks on %d land plates in 2 bands past the field's south and north edges (%d instances), %s."),
			Blocks, Plates, Blocks + Plates, bFacade ? *FString::Printf(TEXT("wearing %s"), *SkylineFacade->GetName()) : TEXT("plain mass"));
	}

	if (bAviation)
	{
		// Red obstruction lamps on the tallest roofs (ftcore::aviationLamps): a small emissive head each, instanced, lit by
		// the window collection's Night through the street lamps' head master (FTScene::NightLampMaterial); no lights.
		const FString GlowPath = FTScene::NightLampMaterial;
		UMaterialInterface* Glow = LoadObject<UMaterialInterface>(nullptr, *(GlowPath + TEXT(".") + FPackageName::GetShortName(GlowPath)), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		if (!Glow)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Backdrop: -AviationLamps: %s not built (FTBuildSurfaces -Windows); no lamps."), *GlowPath);
			return;
		}
		UMaterialInstanceConstant* Red = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_Backdrop_AviationLamp"), RF_Public);
		Red->SetParentEditorOnly(Glow);
		Red->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LampBase")), FLinearColor(0.30f, 0.04f, 0.03f));
		Red->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LampColor")), FLinearColor(1.f, 0.04f, 0.02f));
		Red->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LampStrength")), kAviationStrength);
		Red->PostEditChange();
		const ftcore::AviationSpec Spec;
		const std::vector<ftcore::AviationLamp> Lamps =
			ftcore::aviationLamps(Spec, Boxes.data(), Boxes.size(), FarBlocks.data(), FarBlocks.size());
		UMaterialInterface* const M[] = {Red};
		const FFTCellOptions Options = Far(TEXT("AviationLamp"));
		const double HeadM = static_cast<double>(Spec.headM);
		for (const ftcore::AviationLamp& L : Lamps)
		{
			// The head's centre stands headM over the roof (L.z is its foot, headM over the roof): a ball headM across.
			const FVector At = EngineCm(Side, L.x, L.y, L.z);
			Cells.Add(Lump, M, FTransform(FRotator::ZeroRotator, At, FVector(HeadM, HeadM, HeadM)), NoData, EFTCellKind::NoCollide, Options);
		}
		UE_LOG(LogForgottenThird, Display, TEXT("Backdrop: -AviationLamps: %d red lamps (%d boxes from %.0f m, %d far blocks), strength %.0f."),
			static_cast<int32>(Lamps.size()), static_cast<int32>(Boxes.size()), static_cast<double>(Spec.minHeightM),
			static_cast<int32>(FarBlocks.size()), static_cast<double>(kAviationStrength));
	}
}

void FTBackdrop::Haze(UWorld* World)
{
	float Density = 0.02f;
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
