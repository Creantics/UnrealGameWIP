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
// imported meshes and saves them again (a piece that must never collide, such as a clock hand).

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
