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
// missing, or a face shape has no morph. -Face=0 skips the face shape check (for a body without
// face morphs, such as test_human); the face bone check still runs.
//
// -AnimSampleRate=N (a .glb only) bakes every clip at N Hz. The import then runs on the project's
// default pipeline stack for the file, duplicated in memory, with one field changed: the animation
// pipeline's CustomBoneAnimationSampleRate = N. The stack goes in whole as the task's override
// (UInterchangePipelineStackOverride), which replaces the default stack rather than adding to it. Before
// anything is imported, a clip whose length is not a whole number of frames at N (1e-4 s) fails the
// run, naming the clip; nothing is snapped. The run also fails if the stack bakes at a fixed 30 Hz or
// snaps to frames, since the rate would not be the only change. Unset or 0: the import passes no
// options, as before.

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
