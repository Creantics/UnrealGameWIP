#include "FTStyleParameters.h"

#include "ftcore/StylePack.h"

// Named (not anonymous) so the short enum names cannot collide with other files in a unity build.
namespace FTStyleParamsPrivate
{
FLinearColor Colour(const ftcore::StyleVec3& V)
{
	// Display-space sRGB values: the material runs after tonemapping, so they pass straight through.
	return FLinearColor(V.x, V.y, V.z, 1.f);
}

// Metres, Y up -> centimetres, Z up.
FLinearColor Position(const ftcore::StyleVec3& V)
{
	return FLinearColor(V.x * 100.f, V.z * 100.f, V.y * 100.f, 0.f);
}

enum EScalar
{
	Outline,
	OutlineWidth,
	OutlineThreshold,
	OutlineTaper,
	OutlineJitter,
	CelSteps,
	Saturation,
	Monochrome,
	MonoGamma,
	Halftone,
	HalftonePitch,
	PaperSky,
	PaperGrain,
	Island,
	IslandRadius,
	IslandEdge,
	Hatch,
	HatchScale,
	HatchDensity,
	HatchOverlay,
	HatchMaxLevel,
	HatchShadowOnly,
	HatchCurve,
	InkVoid,
	InkVoidNear,
	InkVoidFar,
	NumScalars
};

enum EVector
{
	OutlineColor,
	ShadowTint,
	HighlightTint,
	PaperColor,
	InkColor,
	IslandCenter,
	IslandInk,
	HatchInk,
	NumVectors
};

// Built once; FName construction is a table lookup, so this is not per frame.
const FName* ScalarNames()
{
	static const FName Names[NumScalars] = {
		TEXT("Outline"), TEXT("OutlineWidth"), TEXT("OutlineThreshold"), TEXT("OutlineTaper"), TEXT("OutlineJitter"),
		TEXT("CelSteps"), TEXT("Saturation"), TEXT("Monochrome"), TEXT("MonoGamma"), TEXT("Halftone"),
		TEXT("HalftonePitch"), TEXT("PaperSky"), TEXT("PaperGrain"), TEXT("Island"), TEXT("IslandRadius"),
		TEXT("IslandEdge"), TEXT("Hatch"), TEXT("HatchScale"), TEXT("HatchDensity"), TEXT("HatchOverlay"),
		TEXT("HatchMaxLevel"), TEXT("HatchShadowOnly"), TEXT("HatchCurve"), TEXT("InkVoid"), TEXT("InkVoidNear"),
		TEXT("InkVoidFar")};
	return Names;
}

const FName* VectorNames()
{
	static const FName Names[NumVectors] = {TEXT("OutlineColor"), TEXT("ShadowTint"), TEXT("HighlightTint"),
		TEXT("PaperColor"), TEXT("InkColor"), TEXT("IslandCenter"), TEXT("IslandInk"), TEXT("HatchInk")};
	return Names;
}
}


FName FTStyle::SkyDepthName()
{
	static const FName Name(TEXT("SkyDepth"));
	return Name;
}

void FTStyle::VisitParameters(const ftcore::StyleSettings& S, TFunctionRef<void(FName, float)> Scalar,
	TFunctionRef<void(FName, const FLinearColor&)> Vector)
{
	using namespace FTStyleParamsPrivate;
	float V[NumScalars];
	V[Outline] = S.outline;
	V[OutlineWidth] = S.outlineWidth;
	V[OutlineThreshold] = S.outlineThreshold;
	V[OutlineTaper] = S.outlineTaper;
	V[OutlineJitter] = S.outlineJitter;
	V[CelSteps] = S.celSteps;
	V[Saturation] = S.saturation;
	V[Monochrome] = S.monochrome;
	V[MonoGamma] = S.monoGamma;
	V[Halftone] = S.halftone;
	V[HalftonePitch] = S.halftonePitch;
	V[PaperSky] = S.paperSky;
	V[PaperGrain] = S.paperGrain;
	V[Island] = S.island;
	V[IslandRadius] = S.islandRadius * 100.f;
	V[IslandEdge] = S.islandEdge;
	V[Hatch] = S.hatch;
	V[HatchScale] = S.hatchScale;
	V[HatchDensity] = S.hatchDensity;
	V[HatchOverlay] = S.hatchOverlay;
	V[HatchMaxLevel] = S.hatchMaxLevel;
	V[HatchShadowOnly] = S.hatchShadowOnly;
	V[HatchCurve] = S.hatchCurve;
	V[InkVoid] = S.inkVoid;
	V[InkVoidNear] = S.inkVoidNear * 100.f;
	V[InkVoidFar] = S.inkVoidFar * 100.f;

	const FName* SN = ScalarNames();
	for (int32 I = 0; I < NumScalars; ++I)
	{
		Scalar(SN[I], V[I]);
	}

	const FName* VN = VectorNames();
	Vector(VN[OutlineColor], Colour(S.outlineColor));
	Vector(VN[ShadowTint], Colour(S.shadowTint));
	Vector(VN[HighlightTint], Colour(S.highlightTint));
	Vector(VN[PaperColor], Colour(S.paperColor));
	Vector(VN[InkColor], Colour(S.inkColor));
	Vector(VN[IslandCenter], Position(S.islandCenter));
	Vector(VN[IslandInk], Colour(S.islandInk));
	Vector(VN[HatchInk], Colour(S.hatchInk));
}
