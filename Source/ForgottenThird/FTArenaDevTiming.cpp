#include "FTArenaDevTiming.h"

#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ForgottenThird.h"
#include "FTText.h"

#include "ftcore/ArenaFight.h"
#include "ftcore/BeatChoreo.h"
#include "ftcore/Json.h"

#include <algorithm>
#include <string>

namespace
{
#if !UE_BUILD_SHIPPING
// The override file, parsed once per process (the flag can't change mid-run). Empty without the flag or on a bad file.
const ftcore::JsonDocument& OverrideFile(FString& OutPath)
{
	static ftcore::JsonDocument Doc;
	static FString Path;
	static bool bRead = false;
	if (!bRead)
	{
		bRead = true;
		FString Wanted;
		if (FParse::Value(FCommandLine::Get(), TEXT("ArenaMoveTiming="), Wanted) && !Wanted.IsEmpty())
		{
			Path = FPaths::IsRelative(Wanted) ? FPaths::ProjectDir() / Wanted : Wanted;
			FString Text;
			std::string Error;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: can't read %s; nothing overridden."), *Path);
			}
			else if (!Doc.parse(FT::ToUtf8(Text), &Error))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: %s is not JSON (%s); nothing overridden."), *Path,
					*FT::ToFString(Error));
			}
			else
			{
				UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaMoveTiming: values from %s (a capture switch)."), *Path);
			}
		}
	}
	OutPath = Path;
	return Doc;
}

// Sets Field from Value when it is a number, logging the change (or that it already held it).
void SetFloat(float& Field, const ftcore::JsonValue& Value, const FString& What)
{
	const float Was = Field;
	Field = static_cast<float>(Value.number(Was));
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaMoveTiming: %s %.3f -> %.3f."), *What, Was, Field);
}

void SetDouble(double& Field, const ftcore::JsonValue& Value, const FString& What)
{
	const double Was = Field;
	Field = Value.number(Was);
	UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaMoveTiming: %s %.3f -> %.3f."), *What, Was, Field);
}

bool KindField(ftcore::ArenaEnemyKind& Kind, std::string_view Name, const ftcore::JsonValue& Value, const FString& What)
{
	struct FField
	{
		const char* Name;
		float ftcore::ArenaEnemyKind::*Member;
	};
	static const FField Fields[] = {
		{"walkSpeed", &ftcore::ArenaEnemyKind::walkSpeed},
		{"runSpeed", &ftcore::ArenaEnemyKind::runSpeed},
		{"strafeSpeed", &ftcore::ArenaEnemyKind::strafeSpeed},
		{"accel", &ftcore::ArenaEnemyKind::accel},
		{"turnRate", &ftcore::ArenaEnemyKind::turnRate},
		{"preferredRange", &ftcore::ArenaEnemyKind::preferredRange},
		{"runRange", &ftcore::ArenaEnemyKind::runRange},
		{"attackDelay", &ftcore::ArenaEnemyKind::attackDelay},
		{"contactInset", &ftcore::ArenaEnemyKind::contactInset},
		{"trotSpeed", &ftcore::ArenaEnemyKind::trotSpeed},
		{"trotRange", &ftcore::ArenaEnemyKind::trotRange},
	};
	for (const FField& F : Fields)
	{
		if (Name == F.Name)
		{
			SetFloat(Kind.*F.Member, Value, What);
			if (F.Member == &ftcore::ArenaEnemyKind::contactInset)
			{
				// The loader's range for the inset (0..1 m): under 0 would grow the lead's circle.
				const float Asked = Kind.contactInset;
				Kind.contactInset = FMath::Clamp(Asked, 0.f, 1.f);
				if (Kind.contactInset != Asked)
				{
					UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: %s clamped to %.3f (0..1)."), *What, Kind.contactInset);
				}
			}
			return true;
		}
	}
	return false;
}

bool MoveField(ftcore::ArenaEnemyMove& Move, std::string_view Name, const ftcore::JsonValue& Value, const FString& What)
{
	struct FField
	{
		const char* Name;
		float ftcore::ArenaAttack::*Member;
	};
	static const FField Attack[] = {
		{"startup", &ftcore::ArenaAttack::startup},
		{"active", &ftcore::ArenaAttack::active},
		{"recovery", &ftcore::ArenaAttack::recovery},
		{"range", &ftcore::ArenaAttack::range},
		{"lunge", &ftcore::ArenaAttack::lunge},
		{"poiseDamage", &ftcore::ArenaAttack::poiseDamage},
	};
	for (const FField& F : Attack)
	{
		if (Name == F.Name)
		{
			SetFloat(Move.attack.*F.Member, Value, What);
			return true;
		}
	}
	if (Name == "damage")
	{
		const int Was = Move.attack.damage;
		Move.attack.damage = static_cast<int>(Value.number(Was));
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaMoveTiming: %s %d -> %d."), *What, Was, Move.attack.damage);
		return true;
	}
	struct FMoveField
	{
		const char* Name;
		float ftcore::ArenaEnemyMove::*Member;
	};
	static const FMoveField Own[] = {
		{"weight", &ftcore::ArenaEnemyMove::weight},
		{"cooldown", &ftcore::ArenaEnemyMove::cooldown},
		{"minRange", &ftcore::ArenaEnemyMove::minRange},
		{"maxRange", &ftcore::ArenaEnemyMove::maxRange},
	};
	for (const FMoveField& F : Own)
	{
		if (Name == F.Name)
		{
			SetFloat(Move.*F.Member, Value, What);
			return true;
		}
	}
	return false;
}

bool ClipField(ftcore::BeatClip& Clip, std::string_view Name, const ftcore::JsonValue& Value, const FString& What)
{
	if (Name == "length_s")
	{
		SetDouble(Clip.length, Value, What);
		return true;
	}
	if (Name == "impact_s")
	{
		SetDouble(Clip.impact, Value, What);
		return true;
	}
	if (Name == "speed_mps")
	{
		SetDouble(Clip.speed, Value, What);
		return true;
	}
	if (Name == "blend_s")
	{
		SetDouble(Clip.blend, Value, What);
		return true;
	}
	if (Name == "asset" && Value.isString())
	{
		const std::string_view Asset = Value.string();
		if (Asset.empty() || Asset.size() > ftcore::kMaxClipAsset || Asset.front() != '/')
		{
			return false;
		}
		const FString Was = FT::ToFString(Clip.asset());
		Clip.assetText.fill('\0');
		std::copy(Asset.begin(), Asset.end(), Clip.assetText.begin());
		UE_LOG(LogForgottenThird, Display, TEXT("Arena: -ArenaMoveTiming: %s %s -> %s."), *What, *Was, *FT::ToFString(Clip.asset()));
		return true;
	}
	return false;
}
#endif
} // namespace

namespace FTArenaDevTiming
{
void ApplyKinds(ftcore::ArenaTable& Table, const TCHAR* Who)
{
#if !UE_BUILD_SHIPPING
	FString Path;
	const ftcore::JsonValue Kinds = OverrideFile(Path).root()["kinds"];
	for (uint32 K = 0; K < Kinds.size(); ++K)
	{
		const std::string Id(Kinds.key(K));
		const ftcore::JsonValue Values = Kinds.value(K);
		const auto Found = std::find(Table.enemyIds.begin(), Table.enemyIds.end(), Id);
		if (Found == Table.enemyIds.end())
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming (%s): no enemy kind \"%s\"; skipped."), Who, *FT::ToFString(Id));
			continue;
		}
		ftcore::ArenaEnemyKind& Kind = Table.enemies[static_cast<size_t>(Found - Table.enemyIds.begin())];
		const FString KindName = FT::ToFString(Id);
		for (uint32 F = 0; F < Values.size(); ++F)
		{
			const std::string_view Name = Values.key(F);
			const ftcore::JsonValue Value = Values.value(F);
			if (Name == "moves")
			{
				for (uint32 M = 0; M < Value.size(); ++M)
				{
					const std::string_view Clip = Value.key(M);
					const ftcore::JsonValue MoveValues = Value.value(M);
					bool bFound = false;
					for (uint32 I = 0; I < Kind.moveCount; ++I)
					{
						ftcore::ArenaEnemyMove& Move = Kind.moves[I];
						if (Move.clip() != Clip)
						{
							continue;
						}
						bFound = true;
						for (uint32 V = 0; V < MoveValues.size(); ++V)
						{
							const FString What = FString::Printf(TEXT("%s (%s) move %u (%s) %s"), *KindName, Who, I,
								*FT::ToFString(Clip), *FT::ToFString(MoveValues.key(V)));
							if (!MoveField(Move, MoveValues.key(V), MoveValues.value(V), What))
							{
								UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: %s: not a move value; skipped."), *What);
							}
						}
					}
					if (!bFound)
					{
						UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: %s has no move with clip \"%s\"; skipped."),
							*KindName, *FT::ToFString(Clip));
					}
				}
				continue;
			}
			const FString What = FString::Printf(TEXT("%s (%s) %s"), *KindName, Who, *FT::ToFString(Name));
			if (!KindField(Kind, Name, Value, What))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: %s: not a kind value; skipped."), *What);
			}
		}
	}
#else
	(void)Table;
	(void)Who;
#endif
}

void ApplyClips(ftcore::BeatClips& Clips, const FString& TablePath, int32 Enemy)
{
#if !UE_BUILD_SHIPPING
	FString Path;
	const ftcore::JsonValue Tables = OverrideFile(Path).root()["clips"];
	const ftcore::JsonValue Table = Tables[std::string_view(FT::ToUtf8(TablePath))];
	for (uint32 C = 0; C < Table.size(); ++C)
	{
		const std::string_view Id = Table.key(C);
		const int Index = Clips.find(Id);
		if (Index < 0)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: %s has no clip \"%s\"; skipped."), *TablePath, *FT::ToFString(Id));
			continue;
		}
		ftcore::BeatClip& Clip = Clips.items[static_cast<size_t>(Index)];
		const ftcore::JsonValue Values = Table.value(C);
		for (uint32 V = 0; V < Values.size(); ++V)
		{
			const FString What = FString::Printf(TEXT("enemy %d clip %s %s"), Enemy, *FT::ToFString(Id), *FT::ToFString(Values.key(V)));
			if (!ClipField(Clip, Values.key(V), Values.value(V), What))
			{
				UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: %s: not a clip value; skipped."), *What);
			}
		}
		if (Clip.impact > Clip.length)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("Arena: -ArenaMoveTiming: enemy %d clip %s: impact %.3f past its length %.3f."), Enemy,
				*FT::ToFString(Id), Clip.impact, Clip.length);
		}
	}
#else
	(void)Clips;
	(void)TablePath;
	(void)Enemy;
#endif
}
} // namespace FTArenaDevTiming
