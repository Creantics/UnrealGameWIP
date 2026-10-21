#pragma once

// Interface text by key, from Data/Loc/<culture>/ui.json (ftcore::UiTextTable does the work).
//
//   const FTUiText kRetry(TEXT("battle.retry"));   // at file scope, like the old FString constants
//   DrawText(kRetry, ...);                          // converts to const FString&
//
// The culture is -UiCulture=<name> on the command line, else the system's; each culture falls back
// to its language and then English ("pt-BR" -> "pt" -> "en"), and a key nobody has shows as itself.
// -UiCulture=pseudo shows the accented, padded pseudo-translation for checking layout.
//
// The tables load once, on first use. A key resolves to a string that lives until exit, and the
// constant keeps a pointer to it, so drawing never builds strings.

#include "CoreMinimal.h"

class FORGOTTENTHIRD_API FTUiText
{
public:
	explicit constexpr FTUiText(const TCHAR* InKey) : Key(InKey) {}

	const FString& Get() const;
	operator const FString&() const { return Get(); }

	// The text for any key (cached the same way); for keys built at runtime.
	static const FString& Lookup(const TCHAR* Key);
	// The culture whose table answered first ("en" when only English loaded).
	static const FString& Culture();

private:
	const TCHAR* Key;
	mutable const FString* Cached = nullptr;
};
