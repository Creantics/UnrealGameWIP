#include "FTBuildShopInteriors.h"

// Built by FTBuildMassingWorld (a commandlet) only: editor builds only, so the game targets, Shipping included, carry none of it.
#if WITH_EDITOR

#include "Engine/StaticMesh.h"
#include "FTCellInstancer.h"
#include "FTSceneLook.h"
#include "ForgottenThird.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"

#include "ftcore/WindowFacade.h"

#include <array>

// Named, not anonymous: the module builds in unity files.
namespace FTShopInteriors::Impl
{
constexpr const TCHAR* kCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
// Pieces a face may take: kWindowShopPiecesPerBay a bay, far more bays than any face holds.
constexpr std::size_t kFacePieces = 96 * ftcore::kWindowShopPiecesPerBay;
// The share of shop bays lit at night (the window master's ShopLit default).
constexpr float kShopLit = 0.8f;
} // namespace FTShopInteriors::Impl

namespace FTShopInteriors
{
FPlacer::FPlacer(UPackage* Package, const FString& Params)
{
	if (!FParse::Param(*Params, TEXT("ShopHoles")))
	{
		return;
	}
	if (!FParse::Param(*Params, TEXT("FacadeR5")))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -ShopHoles needs -FacadeR5; no shop interiors."));
		return;
	}
	FParse::Value(*Params, TEXT("ShopModelShare="), ModelShare);
	ModelShare = FMath::Clamp(ModelShare, 0.f, 1.f);
	Cube = LoadObject<UStaticMesh>(nullptr, Impl::kCubePath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr,
		*FString::Printf(TEXT("%s.%s"), ShopMaterial, *FPackageName::GetShortName(ShopMaterial)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Cube || !Master)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: -ShopHoles: couldn't load %s; build it with FTBuildSurfaces -Windows "
			"-FacadeR5 -ShopHoles. No shop interiors."), !Cube ? Impl::kCubePath : ShopMaterial);
		Cube = nullptr;
		return;
	}
	const FBox Bounds = Cube->GetBoundingBox();
	CubeSize = Bounds.GetSize().ComponentMax(FVector(1.0));
	CubeCentre = Bounds.GetCenter();
	for (std::uint32_t C = 0; C < ftcore::kWindowShopColourCount; ++C)
	{
		const ftcore::WindowTint& Tint = ftcore::windowShopColour(C);
		UMaterialInstanceConstant* Mic =
			NewObject<UMaterialInstanceConstant>(Package, *FString::Printf(TEXT("MI_ShopInterior_%u"), C), RF_Public);
		Mic->SetParentEditorOnly(Master);
		Mic->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Color")), FLinearColor(Tint.r, Tint.g, Tint.b));
		// The shell and floor matt, the fittings and goods a little glossier.
		Mic->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), C < 2 ? 0.75f : 0.5f);
		Mic->PostEditChange();
		Colours.Add(Mic);
	}
	bOn = true;
}

void FPlacer::Place(FFTCellInstancer& Cells, const FFTCellHandle& Piece, const ftcore::MassingBox& Box, const ftcore::MassingWindowData& Data,
	const std::vector<ftcore::MassingStreet>& Streets, double SharedUpTo, double Side)
{
	if (!bOn || Data.streetBits == 0)
	{
		return;
	}
	const ftcore::WindowType Type = ftcore::massingFacadeData(Box, Data).type;
	if (Type == ftcore::WindowType::Legacy)
	{
		return;
	}
	const std::uint8_t Kerb = ftcore::windowKerbBits(Box, Data.streetBits, Streets, SharedUpTo, ftcore::kWindowKerbReach);
	if (Kerb == 0)
	{
		return;
	}
	// Round 11: only ModelShare of the kerbed buildings are modelled; the rest keep no kerb bit, so the master shows
	// their shops' flat lit picture (no hole, no instances).
	if (!ftcore::windowShopModelled(Data.seed, ModelShare))
	{
		++Flat;
		return;
	}
	Cells.SetCustomData(Piece, FTScene::WindowDataStreet, static_cast<float>(Data.streetBits | (Kerb << 4)));
	++Boxes;

	static std::array<ftcore::WindowShopPiece, Impl::kFacePieces> Out;
	const ftcore::WindowFacadeSpec Spec;
	FFTCellOptions Options;
	Options.Cull = EFTCellCull::Interior;
	Options.bCastShadow = false;
	Options.Tag = TEXT("ShopInterior");
	const double Height = Box.z1 - Box.z0;
	for (int32 F = 0; F < 4; ++F)
	{
		if (((Kerb >> F) & 1u) == 0)
		{
			continue;
		}
		const bool bAcrossX = F == 0 || F == 2;
		const float FaceWidth = static_cast<float>(bAcrossX ? Box.sy : Box.sx);
		const float BoxDepth = static_cast<float>(bAcrossX ? Box.sx : Box.sy);
		const float FaceHeight = static_cast<float>(Height - Data.base[F]);
		const std::size_t N = ftcore::windowShopInterior(Spec, Type, Data.seed, FaceWidth, FaceHeight, BoxDepth, Impl::kShopLit, Out.data(),
			Out.size());
		if (N == 0)
		{
			continue;
		}
		++Faces;
		const double StreetFloor = Box.z0 + Data.base[F];
		for (std::size_t I = 0; I < N; ++I)
		{
			const ftcore::WindowShopPiece& P = Out[I];
			const ftcore::MassingBox B = ftcore::windowShopPieceBox(Box, F, StreetFloor, P);
			const ftcore::MassingVec C = ftcore::massingEnginePoint(Side, B.cx, B.cy, 0.5 * (B.z0 + B.z1));
			const FRotator Rot(0.0, ftcore::massingEngineYaw(B.rot), 0.0);
			const FVector Scale = FVector(B.sx, B.sy, B.z1 - B.z0) * 100.0 / CubeSize;
			const FVector Location = FVector(C.x, C.y, C.z) * 100.0 - Rot.RotateVector(CubeCentre * Scale);
			UMaterialInterface* const M[] = {Colours[static_cast<int32>(FMath::Min(P.colour, ftcore::kWindowShopColourCount - 1))]};
			// The bay's light for the master: unlit, or its tint and level (ftcore::windowShopLightCode).
			const float Lit[] = {P.light};
			Cells.Add(Cube, TArrayView<UMaterialInterface* const>(M, 1), FTransform(Rot, Location, Scale), TArrayView<const float>(Lit, 1),
				EFTCellKind::NoCollide, Options);
			++Pieces;
			LitPieces += P.lit ? 1 : 0;
		}
	}
}

void FPlacer::Log() const
{
	if (!bOn)
	{
		return;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: shop interiors: %d boxes with kerbed shop faces modelled (-ShopModelShare=%.2f; "
		"%d more keep the flat picture), %d faces furnished, %d cube instances (%d lit at night), no collision or shadow, culled as props; "
		"their StreetBits carry the kerb bits (4..7)."),
		Boxes, ModelShare, Flat, Faces, Pieces, LitPieces);
}
} // namespace FTShopInteriors

#endif // WITH_EDITOR
