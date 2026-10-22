
#pragma once

// Dumps or edits the neutral vertex positions of one mesh in a DNA file, so a head reshaped
// outside MetaHuman keeps its face rig (joints, blend shapes and Rig Logic stay as they are):
//
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTDna -dna_in=<in.dna> -dump=<out.obj> [-mesh=0]
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTDna -dna_in=<in.dna> -deltas=<d.txt> -dna_out=<out.dna> [-mesh=0] [-nolowerlods]
//   UnrealEditor-Cmd ForgottenThird.uproject -run=FTDna -dna_in=<mesh's.dna> -dna_new=<edited.dna> -skm=<asset path> [-dry]
//
// -dump writes the mesh's raw DNA positions as OBJ "v x y z" lines (DNA axes and units, no
// conversion) after a header comment with the counts, coordinate system and translation unit.
// -deltas reads one "dx dy dz" line per vertex in the same raw frame and adds it to each
// neutral position; the mesh's lower LODs are then recalculated from it (unless -nolowerlods)
// and the result is written to -dna_out. The input file is never modified.
// -skm rewrites the render geometry of a face skeletal mesh built from -dna_in: each mesh LOD is
// matched to the DNA LOD whose vertices equal its own in order, and takes that LOD's positions from
// -dna_new. Importing a DNA only swaps the rig data, so this is how a neutral edit reaches the mesh.
// -dry reports the moves without saving.

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FTDnaCommandlet.generated.h"

UCLASS()
class UFTDnaCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFTDnaCommandlet();

	virtual int32 Main(const FString& Params) override;
};
