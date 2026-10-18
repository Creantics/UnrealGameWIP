#include "FTStorySaveGame.h"

#include "FTStorySubsystem.h"
#include "FTText.h"

#include "ftcore/StorySave.h"

bool UFTStorySaveGame::CaptureFrom(const UFTStorySubsystem* Story)
{
	FFTStorySave Captured;
	if (!Story || !Story->CaptureSave(Captured))
	{
		return false;
	}
	ftcore::StorySave Save;
	Save.scene = FT::ToUtf8(Captured.Scene);
	Save.beat = Captured.Beat < 0 ? 0u : static_cast<uint32>(Captured.Beat);
	for (const FFTStoryFlagSave& F : Captured.Flags)
	{
		ftcore::SavedStoryFlag Out;
		Out.id = FT::ToUtf8(F.Id);
		Out.kind = F.Kind == EFTFlagKind::Int      ? ftcore::FlagValue::Kind::Int
				 : F.Kind == EFTFlagKind::String ? ftcore::FlagValue::Kind::String
												 : ftcore::FlagValue::Kind::Bool;
		Out.number = F.Number;
		Out.text = FT::ToUtf8(F.Text);
		Save.flags.push_back(std::move(Out));
	}
	SaveJson = FT::ToFString(ftcore::encodeStorySave(Save));
	return true;
}

bool UFTStorySaveGame::ApplyTo(UFTStorySubsystem* Story, FString& OutError, TArray<FString>& OutSkippedFlags) const
{
	OutError.Reset();
	OutSkippedFlags.Reset();
	if (!Story)
	{
		OutError = TEXT("no story subsystem");
		return false;
	}
	ftcore::StorySave Save;
	std::string Error;
	if (!ftcore::decodeStorySave(FT::ToUtf8(SaveJson), Save, &Error))
	{
		OutError = FT::ToFString(Error);
		return false;
	}
	FFTStorySave Loaded;
	Loaded.Scene = FT::ToFString(Save.scene);
	Loaded.Beat = static_cast<int32>(FMath::Min<uint32>(Save.beat, MAX_int32));
	for (const ftcore::SavedStoryFlag& F : Save.flags)
	{
		FFTStoryFlagSave& Out = Loaded.Flags.AddDefaulted_GetRef();
		Out.Id = FT::ToFString(F.id);
		Out.Kind = F.kind == ftcore::FlagValue::Kind::Int      ? EFTFlagKind::Int
				 : F.kind == ftcore::FlagValue::Kind::String ? EFTFlagKind::String
															 : EFTFlagKind::Bool;
		Out.Number = F.number;
		Out.Text = FT::ToFString(F.text);
	}
	if (!Story->ApplySave(Loaded, OutSkippedFlags))
	{
		OutError = TEXT("the saved scene no longer exists, or the story is not loaded");
		return false;
	}
	return true;
}
