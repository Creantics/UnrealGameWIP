#pragma once

// Imports a static prop (.glb) into the project, checks it against the asset rules and moves its
// materials onto the project masters, so a prop needs no editor clicks:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportProp -Source=Data/Models/<stem>.glb
//       -Dest=/Game/Props/<Name> -Pack=<pack> -NullRHI -unattended -nopause -nosplash
//
// The .glb is checked first (ftcore::propRules: textures up to 1024 px, no skin needed, the
// Lattice emissive split with -Pack=Lattice; -Kit lets a building-kit piece's maps reach 2048 px); nothing is imported when a rule fails. It then logs
// every imported asset and, for each static mesh, its triangle count, material slots and simple
// collision. When a mesh comes out with Nanite, its project materials get the Nanite usage
// (FT::AllowNanite). Returns 1 when a rule fails, no static mesh comes out, a skinned mesh comes out (use
// FTImportCharacter), or a material can't be moved. A mesh without simple collision is a warning:
// Interchange adds a box when the file has none of its own UCX_ shapes. -NoCollision strips every collision shape from the
// imported meshes and saves them again (a piece that must never collide, such as a clock hand). -NoNanite turns Nanite
// off on the imported meshes and saves them again, so a prop with thin parts draws at full detail, not a reduced fallback.
// -Nanite turns it on whatever the importer's default. With -Kit (or -Nanite) a Nanite mesh keeps its raster fallback and
// ray-tracing proxy at every triangle, and the import fails when the fallback keeps under 90% of the source's triangles.
//
// The materials go onto the prop masters (FT::UsePropMasters: M_FTPropFlat, M_FTPropTex under
// /Game/Props/Shared/Materials), whose roughness break-up, dust and wet layers sit over the authored values; an instance
// left on M_FTCharacter or M_FTCharacterTex by an earlier import moves across with its values. Grime comes from the world
// (ftcore::propGrimeFor): -World=<Solhaven|Lattice|Unwritten|Restored|InkSolhaven>, else -Pack=, else Solhaven; an unknown
// -World= fails before anything is imported. -Held keeps a prop carried in a hand on the character masters instead.
// -Fresh deletes an earlier import's <Dest>/<file>/Materials and /Textures first, so a re-import's values and textures
// are new; without it an instance kept on a prop master keeps its old ones (logged as a Warning).
// Then the gloss guard (FT::CheckPropGloss, ftcore::propGlossCheck) logs each material's roughness p10/p50/p90 and
// metallic, read from its MetallicRoughnessTexture's source mip times the factors (the factors alone on a flat material,
// whose spread rule is skipped), and the import returns 1 when any material fails: median roughness under 0.40 on a name
// not on the smooth list (glaze, glass, water, mirror, polish, chrome), a map's p90 - p10 under 0.10, or metallic over 0.5
// on a name that is not a metal. The meshes and materials are saved by then; there is no override.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTImportPropCommandlet.generated.h"

UCLASS()
class UFTImportPropCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTImportPropCommandlet();

	virtual int32 Main(const FString& Params) override;
};
