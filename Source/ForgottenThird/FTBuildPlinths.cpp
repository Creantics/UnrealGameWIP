#include "FTBuildPlinths.h"

#include "ForgottenThird.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "ftcore/TerrainPocket.h"

#include <string>
#include <utility>

// Named, not anonymous: the module builds in unity files.
namespace FTPlinths::Impl
{
// Pockets left after the plinths that the log names one by one.
constexpr size_t kPocketsShown = 20;
} // namespace FTPlinths::Impl

bool FTPlinths::Plan(const ftcore::MassingGround& Ground, const std::vector<ftcore::MassingBox>& Boxes, const std::vector<bool>& Visible,
	double EntryX, double EntryY, std::vector<ftcore::MassingBox>& Out)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("Plinths")))
	{
		return true;
	}
	const ftcore::PocketRect Rect = ftcore::pocketRectOf(Ground, Boxes, Visible, 10.0);
	const std::vector<ftcore::PocketPoint> Anchors = {ftcore::PocketPoint{EntryX, EntryY}};
	ftcore::PlinthSpec Spec;
	FString ExemptText;
	if (FParse::Value(FCommandLine::Get(), TEXT("PlinthExempt="), ExemptText, false) &&
		!ftcore::parsePocketRects(TCHAR_TO_UTF8(*ExemptText), Spec.pocket.exempt))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: plinths: -PlinthExempt is X0,Y0,X1,Y1[;X0,Y0,X1,Y1] (plan m), not %s"), *ExemptText);
		return false;
	}
	std::vector<ftcore::MassingBox> Made;
	ftcore::PlinthStats Stats;
	std::string Error;
	if (!ftcore::planPlinths(Ground, Boxes, Visible, Rect, Anchors, Spec, Made, Stats, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: plinths: %s"), UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("FTBuildMassingWorld: plinths: over x %.0f..%.0f, y %.0f..%.0f: %d pockets (%d cells) before; %d buildings on sloping ground, "
			 "%d skirt boxes, %d fill boxes over %d pockets, %d stair steps out of %d pockets; %d pockets (%d cells) after, %d no wider "
			 "than %.1f m, %d wider, %d exempt."),
		Rect.minX, Rect.maxX, Rect.minY, Rect.maxY, static_cast<int32>(Stats.pocketsBefore), static_cast<int32>(Stats.pocketCellsBefore),
		static_cast<int32>(Stats.slopedBuildings), static_cast<int32>(Stats.skirts), static_cast<int32>(Stats.fills),
		static_cast<int32>(Stats.filledPockets), static_cast<int32>(Stats.stairs), static_cast<int32>(Stats.stairedPockets),
		static_cast<int32>(Stats.pocketsAfter), static_cast<int32>(Stats.pocketCellsAfter),
		static_cast<int32>(Stats.narrowLeft), Spec.pocket.narrowWidth, static_cast<int32>(Stats.wideLeft),
		static_cast<int32>(Stats.after.exempted));
	for (size_t I = 0; I < Stats.after.pockets.size() && I < Impl::kPocketsShown; ++I)
	{
		const ftcore::Pocket& P = Stats.after.pockets[I];
		const FString Spill = P.hasSpill ? FString::Printf(TEXT("spill %.2f m"), P.spill) : FString(TEXT("no way out"));
		const FString Near = P.nearBox >= 0 && static_cast<size_t>(P.nearBox) < Boxes.size()
			? TEXT(", by ") + FString(UTF8_TO_TCHAR(Boxes[static_cast<size_t>(P.nearBox)].id.c_str()))
			: FString();
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: plinths: left at (%.1f, %.1f): %.1f m2, %.1f m wide, floor %.2f m, %s%s."), P.cx,
			P.cy, P.area, P.width, P.floorMin, *Spill, *Near);
	}
	Out.reserve(Out.size() + Made.size());
	for (ftcore::MassingBox& Box : Made)
	{
		// The retaining walls' surface (ftcore::massingLookSurface); the checker's tables keep kind "plinth".
		Box.kind = "terrace";
		Out.push_back(std::move(Box));
	}
	return true;
}
