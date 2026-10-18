#pragma once

// UTF-8 text from ftcore <-> FString.

#include "CoreMinimal.h"

#include <string>
#include <string_view>

namespace FT
{
inline FString ToFString(std::string_view Text)
{
	if (Text.empty())
	{
		return FString();
	}
	const FUTF8ToTCHAR Conv(reinterpret_cast<const UTF8CHAR*>(Text.data()), static_cast<int32>(Text.size()));
	return FString::ConstructFromPtrSize(Conv.Get(), Conv.Length());
}

inline std::string ToUtf8(const FString& Text)
{
	const FTCHARToUTF8 Conv(*Text, Text.Len());
	return std::string(reinterpret_cast<const char*>(Conv.Get()), static_cast<size_t>(Conv.Length()));
}

// File bytes as UTF-8 text, without a leading byte-order mark.
inline std::string_view BytesAsText(const TArray<uint8>& Bytes)
{
	std::string_view Text(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
	if (Text.size() >= 3 && Text.substr(0, 3) == "\xEF\xBB\xBF")
	{
		Text.remove_prefix(3);
	}
	return Text;
}
} // namespace FT
