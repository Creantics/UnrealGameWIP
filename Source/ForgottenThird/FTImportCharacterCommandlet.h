#pragma once

// Imports a character file (.glb / .gltf / .fbx) into the project and checks its rig against the
// face code, so a rigging test needs no editor clicks:
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTImportCharacter -Source=Data/Models/proof_head.glb
//       -Dest=/Game/Characters/ProofHead -NullRHI -unattended -nopause -nosplash
//
// It logs every imported asset, then for the skeletal mesh: how many face shapes from
// Data/Rig/face_presets.json have a morph target of the same name, and whether the face bones
// (jaw, tongue_01..03, head, eye_l, eye_r) exist. Returns 1 when the import fails, a face bone is
// missing, or a face shape has no morph.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTImportCharacterCommandlet.generated.h"

UCLASS()
class UFTImportCharacterCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTImportCharacterCommandlet();

	virtual int32 Main(const FString& Params) override;
};
