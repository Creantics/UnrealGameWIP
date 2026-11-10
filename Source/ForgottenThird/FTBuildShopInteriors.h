#pragma once

// Round 10: the shallow modelled shop interiors behind the shop glass of the street faces on kerbed streets, and the
// two masters' paths FTBuildSurfaces -Windows writes for them.
//
// FTBuildSurfaces -Windows -FacadeR5 -ShopHoles builds the window master masked, its shop glass cut open where the box's
// StreetBits carry the face's kerb bit (bits 4..7, ftcore::windowKerbBits), and ShopMaterial. FTBuildMassingWorld
// -Windows -FacadeR5 -ShopHoles then ORs each windowed box's kerb bits into its StreetBits and lays, behind each kerbed
// face's shop bays, ftcore::windowShopInterior's pieces (a shell, shelves with goods, a counter or a table) as cube
// instances of the build's cells (FFTCellInstancer), no collision, no shadow, culled as props. Without -ShopHoles nothing
// here changes the build. Every other shop keeps the master's flat lit picture.

#include "CoreMinimal.h"

#include "ftcore/MassingDress.h"
#include "ftcore/MassingPlan.h"

#include <vector>

class FFTCellInstancer;
struct FFTCellHandle;
class UMaterialInterface;
class UPackage;
class UStaticMesh;

namespace FTShopInteriors
{
// The shop interiors' master: Color, Roughness, a night glow on a lit bay's pieces (custom data slot 0).
inline constexpr const TCHAR* ShopMaterial = TEXT("/Game/Rendering/Windows/M_ShopInterior");
// The building-kit modules' master with night glass (-KitGlass): the imported textured master's parameters by name.
inline constexpr const TCHAR* KitGlassMaterial = TEXT("/Game/Rendering/Windows/M_KitGlass");

class FORGOTTENTHIRD_API FPlacer
{
public:
	// On when Params carry -ShopHoles and -FacadeR5 and the cube and ShopMaterial load (else logged and off). The
	// pieces' colour instances are made in Package.
	FPlacer(UPackage* Package, const FString& Params);
	bool IsOn() const { return bOn; }
	// A windowed box (Piece, its instance; Data, its window data): ORs its kerb bits (faces fronting a street wider than
	// SharedUpTo, of Streets) into its StreetBits and lays its kerbed faces' shop pieces. Side: the world's side (m).
	// Nothing when off.
	void Place(FFTCellInstancer& Cells, const FFTCellHandle& Piece, const ftcore::MassingBox& Box, const ftcore::MassingWindowData& Data,
		const std::vector<ftcore::MassingStreet>& Streets, double SharedUpTo, double Side);
	// The census: kerbed faces, bays' pieces and instances.
	void Log() const;

private:
	bool bOn = false;
	UStaticMesh* Cube = nullptr;
	FVector CubeSize = FVector(100.0);
	FVector CubeCentre = FVector::ZeroVector;
	TArray<UMaterialInterface*> Colours;
	int32 Boxes = 0, Faces = 0, Pieces = 0, LitPieces = 0;
};
} // namespace FTShopInteriors
