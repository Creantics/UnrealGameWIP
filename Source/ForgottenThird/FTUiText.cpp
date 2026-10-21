#include "FTUiText.h"

#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ForgottenThird.h"

#include "ftcore/UiText.h"

#include <string>
#include <vector>

namespace
{
struct FUiTextStore
{
	ftcore::UiTextTable English;
	ftcore::UiTextTable Primary;
	bool bHasPrimary = false;
	FString Culture = TEXT("en");
	// Resolved strings by key. Values are boxed so a pointer survives the map growing.
	TMap<FString, TUniquePtr<FString>> Resolved;
};

bool LoadTable(const FString& CultureName, ftcore::UiTextTable& Out)
{
	const FString Path = FPaths::ProjectDir() / TEXT("Data/Loc") / CultureName / TEXT("ui.json");
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*Text);
	std::string Error;
	if (!ftcore::importUiText(std::string_view(Utf8.Get(), static_cast<size_t>(Utf8.Length())), Out, &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("UiText: %s: %s"), *Path, UTF8_TO_TCHAR(Error.c_str()));
		return false;
	}
	return true;
}

FUiTextStore& Store()
{
	static FUiTextStore* Instance = []
	{
		FUiTextStore* S = new FUiTextStore();
		if (!LoadTable(TEXT("en"), S->English))
		{
			UE_LOG(LogForgottenThird, Error, TEXT("UiText: no English table; every string shows its key."));
		}
		FString Wanted;
		if (!FParse::Value(FCommandLine::Get(), TEXT("-UiCulture="), Wanted))
		{
			Wanted = FInternationalization::Get().GetCurrentCulture()->GetName();
		}
		if (Wanted.Equals(TEXT("pseudo"), ESearchCase::IgnoreCase))
		{
			S->Primary = ftcore::pseudoLocalise(S->English);
			S->bHasPrimary = true;
			S->Culture = TEXT("pseudo");
		}
		else
		{
			for (const std::string& Name : ftcore::uiCultureChain(TCHAR_TO_UTF8(*Wanted)))
			{
				if (Name == "en")
				{
					break;
				}
				const FString CultureName = UTF8_TO_TCHAR(Name.c_str());
				if (LoadTable(CultureName, S->Primary))
				{
					S->bHasPrimary = true;
					S->Culture = CultureName;
					for (const std::string& Issue : ftcore::checkUiTranslation(S->English, S->Primary))
					{
						UE_LOG(LogForgottenThird, Warning, TEXT("UiText: %s"), UTF8_TO_TCHAR(Issue.c_str()));
					}
					break;
				}
			}
		}
		UE_LOG(LogForgottenThird, Display, TEXT("UiText: culture %s (%d English strings)."), *S->Culture,
			static_cast<int32>(S->English.entries.size()));
		return S;
	}();
	return *Instance;
}
} // namespace

const FString& FTUiText::Lookup(const TCHAR* InKey)
{
	FUiTextStore& S = Store();
	const FString KeyString(InKey);
	if (const TUniquePtr<FString>* Found = S.Resolved.Find(KeyString))
	{
		return **Found;
	}
	const FTCHARToUTF8 KeyUtf8(InKey);
	const std::string_view Text = ftcore::lookupUiText(S.bHasPrimary ? &S.Primary : nullptr, S.English,
		std::string_view(KeyUtf8.Get(), static_cast<size_t>(KeyUtf8.Length())));
	const FUTF8ToTCHAR Wide(Text.data(), static_cast<int32>(Text.size()));
	TUniquePtr<FString>& Slot = S.Resolved.Add(KeyString, MakeUnique<FString>(Wide.Length(), Wide.Get()));
	return *Slot;
}

const FString& FTUiText::Culture()
{
	return Store().Culture;
}

const FString& FTUiText::Get() const
{
	if (!Cached)
	{
		Cached = &Lookup(Key);
	}
	return *Cached;
}
