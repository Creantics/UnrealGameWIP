#pragma once

// FTBuildMassingWorld's kit frontage: wraps ftcore/BuildingLots.h.
// -Lots=<runs csv> -KitTable=<kit csv>: the boxes on each frontage run are replaced by lots of modular kit buildings
// (pieces imported by FTImportProp -Kit under FTLots::PieceFolder, one per kit id), each piece an instance with
// per-instance custom data (FTLots::Data*) that the kit master (built into the map package) reads for its palette
// tints, dirt, seeded lit windows and night signs. Without -Lots the build is unchanged.

#include "CoreMinimal.h"

#include "ftcore/BuildingLots.h"
#include "ftcore/HeroCorner.h"
#include "ftcore/MassingPlan.h"

#include <string>
#include <vector>

class FFTCellInstancer;
class UPackage;
class UStaticMesh;

namespace FTLots
{
inline constexpr const TCHAR* PieceFolder = TEXT("/Game/Props/BKIT");

// Per-instance custom data: packed 8-bit RGB (r * 65536 + g * 256 + b) for the palette slots, then the rest.
enum EData : int32
{
	DataWall = 0,
	DataTrim,
	DataAccent,
	DataTimber,
	DataRoof,
	DataSignFg,
	DataSeed,  // 0..1
	DataDirt,  // 0..1
	DataShop,  // 1 on a shop storey's glass, 2 on the glass proof front's
	DataCount
};

struct FBuild
{
	bool bOn = false;
	ftcore::KitTable Kit;
	ftcore::FrontagePlan Plan;
	std::vector<ftcore::KitPlacement> Placements;
	std::vector<bool> Replaced; // by box: a lot stands in its place (no box, no windows, no frontage, no roof dressing)
	// The glass proof fronts, whose shop glass takes the look's own night level: -GlassProofLot=<lot>, and the hero
	// corner's lit lots (AddHeroLots), glazed or timber lattice.
	TArray<int32> GlassProofLots;
	bool bGlassProofPane = false; // -GlassProofPane: their glass is true panes over a real bay (else glass over a picture)
	bool IsGlassProof(int32 Lot) const { return GlassProofLots.Contains(Lot); }
	// The hero corner's lots: dark stained timber frames and trims with plaster walls (no bare concrete or metal), the
	// shutters weathered; and the one whose lattice room is the eatery's (a counter, not shelves).
	TArray<int32> TimberLots;
	int32 CounterLot = -1;
	// The hero corner's lit rooms' own glow (AddHeroLots): -HeroPanelGlow=<x> the ceiling panels' LampPanel (1),
	// -HeroWallGlow=<x> the back wall's (0: lit by the shop slot alone), -HeroGlowCap=<x> their ShopGlowCap (0: the master's).
	float HeroPanelGlow = 1.f;
	float HeroWallGlow = 0.f;
	float HeroGlowCap = 0.f;
	// Set by Place: the proof bay's ceiling fixtures (engine cm, a rotation facing down, the source's size in cm), which the
	// massing build spawns as rect lights tagged FTNight::BayLightTag.
	struct FBayLight
	{
		FVector At = FVector::ZeroVector;
		FRotator Turn = FRotator::ZeroRotator;
		float WidthCm = 0.f;
		float HeightCm = 0.f;
	};
	TArray<FBayLight> BayLights;
};

// Reads -Lots= and -KitTable=, plans the lots on Boxes and lays out every lot's pieces. True with bOn false without
// -Lots; false (logged) on a table that doesn't read or a piece the grammar needs that the kit table lacks.
bool Plan(const TCHAR* Params, const std::vector<ftcore::MassingBox>& Boxes, FBuild& Out);

// The hero corner's bays as kit lots (ftcore::heroCornerLots): its run and lots appended to Out's plan, their pieces laid
// out, the bay boxes (BayIds, by bay) marked replaced, the lit lots made glass proof fronts and every lot a timber lot
// (the eatery's the counter lot). The kit table is
// -KitTable= or Data/Look/building_kit_pieces.csv when Plan read none; -GlassProofPane is read here too. Sets bOn. False
// (logged) on a kit table that doesn't read or a piece the grammar needs that it lacks.
bool AddHeroLots(const TCHAR* Params, const ftcore::HeroLots& Hero, const std::string* BayIds, const std::vector<ftcore::MassingBox>& Boxes,
	FBuild& Out);

// Places the planned pieces and boxes as instances; the kit master and its instances go into Package. False (logged)
// when a piece mesh is missing or no axis fit maps the kit table onto the imported meshes.
bool Place(FBuild& Build, FFTCellInstancer& Cells, UPackage* Package, UStaticMesh* Cube, double Side);
} // namespace FTLots
