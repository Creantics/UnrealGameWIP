#include "FTBuildPlinths.h"

// Builder code: editor builds only (the commandlets that call it are editor-only).
#if WITH_EDITOR

#include "ForgottenThird.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
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

bool FTPlinths::ReportFloats(const ftcore::MassingGround& Ground, const std::vector<ftcore::MassingBox>& Boxes, const std::vector<bool>& Visible)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("FloatCheck")))
	{
		return true;
	}
	const ftcore::FloatSpec Spec;
	ftcore::FloatReport Report;
	std::string Error;
	if (!ftcore::findFloats(Ground, Boxes, Visible, Spec, Report, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTBuildMassingWorld: floats: %s"), UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	size_t Skirts = 0, Snaps = 0;
	FString Csv = TEXT("id,kind,gap_m,x,y,base_z,ground_z,ground_min_z,building,failing,crouch,fix\n");
	for (const ftcore::FloatBox& F : Report.floating)
	{
		const ftcore::MassingBox& B = Boxes[F.box];
		// What item 3's fix pass would do (not done yet): a skirt up to maxSkirt for a building, else a snap when over the line.
		const TCHAR* Fix = F.building ? (F.gap <= Spec.maxSkirt ? TEXT("skirt") : TEXT("snap")) : (F.gap > Spec.failAny ? TEXT("snap") : TEXT("leave"));
		Skirts += FCString::Strcmp(Fix, TEXT("skirt")) == 0 ? 1 : 0;
		Snaps += FCString::Strcmp(Fix, TEXT("snap")) == 0 ? 1 : 0;
		const FString Id = UTF8_TO_TCHAR(B.id.c_str()), Kind = UTF8_TO_TCHAR(B.kind.c_str());
		UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: float %s (%s): %.2f m at (%.1f, %.1f), base %.2f over ground %.2f (lowest %.2f)%s%s%s; fix %s."),
			*Id, *Kind, F.gap, F.x, F.y, F.base, F.ground, F.groundMin, F.building ? TEXT(", building") : TEXT(""),
			F.failing ? TEXT(", FAILS") : TEXT(""), F.crouch ? TEXT(", crouch-under") : TEXT(""), Fix);
		Csv += FString::Printf(TEXT("%s,%s,%.3f,%.2f,%.2f,%.3f,%.3f,%.3f,%d,%d,%d,%s\n"), *Id, *Kind, F.gap, F.x, F.y, F.base, F.ground, F.groundMin,
			F.building ? 1 : 0, F.failing ? 1 : 0, F.crouch ? 1 : 0, Fix);
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTBuildMassingWorld: floats (report only): %d boxes checked, %d not sunk %.1f m, %d float over %.2f m: %d fail "
		"(over %.1f m, a building over %.1f m), %d crouch-under (%.1f-%.1f m); the fix pass would skirt %d and snap %d."),
		static_cast<int32>(Report.checked), static_cast<int32>(Report.shallow), Spec.sink, static_cast<int32>(Report.floating.size()), Spec.visible,
		static_cast<int32>(Report.failing), Spec.failAny, Spec.failBuilding, static_cast<int32>(Report.crouch), Spec.crouchLow, Spec.crouchHigh,
		static_cast<int32>(Skirts), static_cast<int32>(Snaps));
	FString Path;
	if (FParse::Value(FCommandLine::Get(), TEXT("FloatReport="), Path) && !FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTBuildMassingWorld: floats: couldn't write %s."), *Path);
	}
	return true;
}

#endif // WITH_EDITOR
