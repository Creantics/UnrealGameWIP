#include "FTWeaponSocketsCommandlet.h"

#include "ForgottenThird.h"

#if WITH_EDITOR
#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "FTExploreCharacter.h"
#include "FTLeadWeapons.h"
#include "FTLook.h"
#include "FTText.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "StaticMeshResources.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "ftcore/Json.h"

#include <string>

// A named namespace: the unity build shares one translation unit between commandlets.
namespace FTWeaponSocketsImpl
{
// Past these a check is a warning (cm, degrees).
constexpr double kCheckCm = 1.0;
constexpr double kCheckDeg = 5.0;

bool ReadJsonFile(const FString& File, ftcore::JsonDocument& Doc)
{
	TArray<uint8> Bytes;
	if (!IFileManager::Get().FileExists(*File) || !FFileHelper::LoadFileToArray(Bytes, *File))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: can't read %s."), *File);
		return false;
	}
	std::string Error;
	if (!Doc.parse(FT::BytesAsText(Bytes), &Error))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: %s isn't JSON (%s)."), *File, *FT::ToFString(Error));
		return false;
	}
	return true;
}

bool ReadVec3(const ftcore::JsonValue& V, FVector& Out)
{
	if (!V.isArray() || V.size() != 3)
	{
		return false;
	}
	Out = FVector(V[0].number(), V[1].number(), V[2].number());
	return true;
}

bool ReadQuatXyzw(const ftcore::JsonValue& V, FQuat& Out)
{
	if (!V.isArray() || V.size() != 4)
	{
		return false;
	}
	Out = FQuat(V[0].number(), V[1].number(), V[2].number(), V[3].number());
	if (Out.SizeSquared() < 1e-6)
	{
		return false;
	}
	Out.Normalize();
	return true;
}

// The files' comp_* frame into F: (a, b, c) -> (-b, a, c), a quarter turn about Z.
const FQuat& TurnedToF()
{
	static const FQuat Turn(FVector::UpVector, FMath::DegreesToRadians(90.0));
	return Turn;
}

FQuat Yaw(double Degrees)
{
	return FQuat(FVector::UpVector, FMath::DegreesToRadians(Degrees));
}

double DegreesApart(const FQuat& A, const FQuat& B)
{
	return FMath::RadiansToDegrees(A.AngularDistance(B));
}

FString Describe(const FTransform& T)
{
	const FVector L = T.GetTranslation();
	const FRotator R = T.Rotator();
	return FString::Printf(TEXT("loc (%.3f, %.3f, %.3f) rot (P %.2f, Y %.2f, R %.2f)"), L.X, L.Y, L.Z, R.Pitch, R.Yaw, R.Roll);
}

// The body a lead wears: -<BodyKey>=<mesh>, else the PartyLooks entry -<LookKey>= names (DefaultLook when not given).
USkeletalMesh* LoadLeadBody(const FString& Params, const TCHAR* LookKey, const TCHAR* BodyKey, const TCHAR* DefaultLook)
{
	FString Path;
	FString Look = DefaultLook;
	if (!FParse::Value(*Params, BodyKey, Path))
	{
		FParse::Value(*Params, LookKey, Look);
		for (const FFTLeadLook& Member : GetDefault<AFTExploreCharacter>()->PartyLooks)
		{
			if (Member.Name.Equals(Look, ESearchCase::IgnoreCase))
			{
				Path = Member.BodyMeshPath.ToString();
				break;
			}
		}
		if (Path.IsEmpty())
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: no PartyLooks entry named %s with a body (Config/UserGame.ini); pass -%s<mesh>."),
				*Look, BodyKey);
			return nullptr;
		}
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Path);
	if (!Mesh)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: %s's body %s doesn't load."), *Look, *Path);
		return nullptr;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: %s's body is %s"), *Look, *Mesh->GetPathName());
	return Mesh;
}

// Puts a mesh-only socket on Mesh, replacing one of the same name.
void PutBodySocket(USkeletalMesh& Mesh, const TCHAR* Name, const TCHAR* Bone, const FTransform& Socket)
{
	const FName SocketName(Name);
	TArray<TObjectPtr<USkeletalMeshSocket>>& List = Mesh.GetMeshOnlySocketList();
	for (int32 I = List.Num() - 1; I >= 0; --I)
	{
		USkeletalMeshSocket* Old = List[I];
		if (Old && Old->SocketName == SocketName)
		{
			List.RemoveAt(I);
			Old->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Old->MarkAsGarbage();
		}
	}
	USkeletalMeshSocket* New = NewObject<USkeletalMeshSocket>(&Mesh);
	New->SocketName = SocketName;
	New->BoneName = FName(Bone);
	New->RelativeLocation = Socket.GetTranslation();
	New->RelativeRotation = Socket.Rotator();
	New->RelativeScale = FVector::OneVector;
	List.Add(New);
	Mesh.RebuildSocketMap();
}

bool SaveWeaponSocketAsset(UObject& Asset)
{
	UPackage* Package = Asset.GetOutermost();
	Package->MarkPackageDirty();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, &Asset, *File, Save))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: couldn't save %s."), *File);
		return false;
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: saved %s"), *File);
	return true;
}

// The bone's component-space rest transform, or false when the body has no such bone.
bool RestOf(const USkeletalMesh& Body, const TCHAR* Bone, FTransform& Out)
{
	const FReferenceSkeleton& Ref = Body.GetRefSkeleton();
	const int32 Index = Ref.FindBoneIndex(FName(Bone));
	if (Index == INDEX_NONE)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: %s has no bone %s."), *Body.GetName(), Bone);
		return false;
	}
	Out = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Index);
	return true;
}

// The file's entry for a piece, with its bone checked against the one FTLeadWeapons.h names.
bool EntryFor(const ftcore::JsonValue& Root, const TCHAR* Key, const TCHAR* Bone, ftcore::JsonValue& Out)
{
	Out = Root[FT::ToUtf8(FString(Key))];
	if (!Out.isObject())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: the socket file has no entry %s."), Key);
		return false;
	}
	const FString FileBone = FT::ToFString(Out["bone"].string());
	if (!FileBone.Equals(Bone))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: the file puts %s on %s, FTLeadWeapons.h on %s."), Key, *FileBone, Bone);
		return false;
	}
	return true;
}

// The pistol's barrel direction as a yaw (0, 90, 180 or -90 degrees) from its bounds, logged.
double PistolYaw(const UStaticMesh& Pistol)
{
	const FBox Box = Pistol.GetBoundingBox();
	const double SpanX = Box.Max.X - Box.Min.X;
	const double SpanY = Box.Max.Y - Box.Min.Y;
	const double Degrees = SpanY >= SpanX ? (Box.Max.Y + Box.Min.Y >= 0.0 ? 90.0 : -90.0) : (Box.Max.X + Box.Min.X >= 0.0 ? 0.0 : 180.0);
	UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: %s bounds (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f) cm: the barrel points at yaw %.0f."),
		*Pistol.GetName(), Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z, Degrees);
	if (Degrees != 90.0)
	{
		UE_LOG(LogForgottenThird, Warning, TEXT("FTWeaponSockets: the pistol's barrel isn't along +Y as the import frame puts it; its sockets are turned to match."));
	}
	return Degrees;
}

// The pistol's muzzle socket. Returns the problems found.
int32 PutMuzzleSocket(UStaticMesh& Pistol, const ftcore::JsonValue& Root, double BarrelYaw)
{
	FVector Point;
	if (!ReadVec3(Root["muzzle_pistol_local"]["loc_cm"], Point))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: the pistol file has no muzzle_pistol_local.loc_cm."));
		return 1;
	}
	const FTransform Socket(Yaw(BarrelYaw), Yaw(BarrelYaw).RotateVector(Point));
	const FName Name(FTWeaponProps::MuzzleSocket);
	while (UStaticMeshSocket* Old = Pistol.FindSocket(Name))
	{
		Pistol.RemoveSocket(Old);
	}
	UStaticMeshSocket* New = NewObject<UStaticMeshSocket>(&Pistol);
	New->SocketName = Name;
	New->RelativeLocation = Socket.GetTranslation();
	New->RelativeRotation = Socket.Rotator();
	New->RelativeScale = FVector::OneVector;
	Pistol.AddSocket(New);
	// How far the muzzle sits along the barrel against where the mesh ends.
	const FBox Box = Pistol.GetBoundingBox();
	const FVector Along = Yaw(BarrelYaw).RotateVector(FVector::ForwardVector);
	const double End = FVector::DotProduct(Along, Along.X + Along.Y >= 0.0 ? Box.Max : Box.Min);
	UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: %s %s: %s; %.2f cm along the barrel, the mesh ends at %.2f."), *Pistol.GetName(),
		FTWeaponProps::MuzzleSocket, *Describe(Socket), FVector::DotProduct(Along, Socket.GetTranslation()), End);
	return 0;
}

// One of the pistol's body sockets from the file's entry Key. Returns the problems found.
int32 PutPistolSocket(USkeletalMesh& Body, const ftcore::JsonValue& Root, const TCHAR* Key, const FTWeaponProps::FPiece& Piece, double BarrelYaw)
{
	ftcore::JsonValue Entry;
	FTransform Rest;
	FVector Offset;
	FQuat Turn;
	if (!EntryFor(Root, Key, Piece.Bone, Entry) || !RestOf(Body, Piece.Bone, Rest))
	{
		return 1;
	}
	if (!ReadVec3(Entry["loc_cm"], Offset) || !ReadQuatXyzw(Entry["quat_xyzw"], Turn))
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: %s needs loc_cm and quat_xyzw."), Key);
		return 1;
	}
	const FTransform FromFile(Turn, Offset);
	const FTransform Socket = FTransform(Yaw(90.0 - BarrelYaw)) * FromFile;
	FString Checks;
	FVector FileAt;
	if (ReadVec3(Entry["comp_loc_cm"], FileAt))
	{
		const double Cm = FVector::Dist((Socket * Rest).GetTranslation(), TurnedToF().RotateVector(FileAt));
		Checks += FString::Printf(TEXT("; at rest %.2f cm from the file's"), Cm);
		if (Cm > kCheckCm)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTWeaponSockets: %s sits %.1f cm from where the file measured it at rest."), Piece.Socket, Cm);
		}
	}
	FQuat FileFacing;
	if (ReadQuatXyzw(Entry["comp_quat_xyzw"], FileFacing))
	{
		// The file's mesh-to-component rotation, turned into F on both sides, against the socket's (mesh in F).
		const FQuat Want = TurnedToF() * FileFacing * TurnedToF().Inverse();
		const double Deg = DegreesApart(Rest.GetRotation() * Turn, Want);
		Checks += FString::Printf(TEXT("; at rest %.2f deg from the file's"), Deg);
		if (Deg > kCheckDeg)
		{
			UE_LOG(LogForgottenThird, Warning, TEXT("FTWeaponSockets: %s turns the pistol %.1f deg from how the file holds it at rest; check the quat convention."),
				Piece.Socket, Deg);
		}
	}
	PutBodySocket(Body, Piece.Socket, Piece.Bone, Socket);
	UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: %s %s on %s: %s%s."), *Body.GetName(), Piece.Socket, Piece.Bone, *Describe(Socket), *Checks);
	return 0;
}

FString FullPath(const FString& File)
{
	return FPaths::IsRelative(File) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / File) : File;
}
// LOD1.. of the pistol from the static meshes in List (comma separated asset paths, made in Blender and imported with
// FTImportProp): each one's mesh description becomes the next LOD, its one section on the pistol's material 0.
int32 PutPistolLods(UStaticMesh& Pistol, const FString& List)
{
	static constexpr float Screen[] = {1.f, 0.4f, 0.15f, 0.06f};
	TArray<FString> Paths;
	List.ParseIntoArray(Paths, TEXT(","));
	if (Paths.Num() < 1 || Paths.Num() > 3)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: -PistolLods takes 1 to 3 mesh paths, got %d."), Paths.Num());
		return 1;
	}
	Pistol.SetNumSourceModels(Paths.Num() + 1);
	Pistol.SetAutoComputeLODScreenSize(false);
	Pistol.GetSourceModel(0).ScreenSize.Default = Screen[0];
	for (int32 I = 0; I < Paths.Num(); ++I)
	{
		const UStaticMesh* Lod = LoadObject<UStaticMesh>(nullptr, *Paths[I]);
		const FMeshDescription* From = Lod ? Lod->GetMeshDescription(0) : nullptr;
		if (!From)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: no LOD mesh %s."), *Paths[I]);
			return 1;
		}
		const int32 Index = I + 1;
		FStaticMeshSourceModel& Model = Pistol.GetSourceModel(Index);
		Model.ReductionSettings = FMeshReductionSettings();
		Model.BuildSettings = Pistol.GetSourceModel(0).BuildSettings;
		Model.ScreenSize.Default = Screen[Index];
		Pistol.CreateMeshDescription(Index, FMeshDescription(*From));
		Pistol.CommitMeshDescription(Index);
		Pistol.GetSectionInfoMap().Set(Index, 0, FMeshSectionInfo(0));
	}
	Pistol.Build(false);
	Pistol.PostEditChange();
	const FStaticMeshRenderData* Render = Pistol.GetRenderData();
	for (int32 I = 0; Render && I < Render->LODResources.Num(); ++I)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: %s LOD%d %d tris, screen %.2f."), *Pistol.GetName(), I,
			Render->LODResources[I].GetNumTriangles(), Screen[FMath::Min(I, 3)]);
	}
	return Render && Render->LODResources.Num() == Paths.Num() + 1 ? 0 : 1;
}
} // namespace FTWeaponSocketsImpl
#endif

UFTWeaponSocketsCommandlet::UFTWeaponSocketsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFTWeaponSocketsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	namespace WS = FTWeaponSocketsImpl;
	FString PistolFile;
	FParse::Value(*Params, TEXT("Pistol="), PistolFile);
	if (PistolFile.IsEmpty())
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: pass -Pistol=<socket .json> (the gauntlets are skinned pieces and need no sockets)."));
		return 1;
	}
	const bool bDryRun = FParse::Param(*Params, TEXT("DryRun"));
	int32 Problems = 0;
	TArray<UObject*> ToSave;

	if (!PistolFile.IsEmpty())
	{
		ftcore::JsonDocument Doc;
		USkeletalMesh* Body = WS::LoadLeadBody(Params, TEXT("Duy="), TEXT("DuyBody="), TEXT("Duy"));
		FString MeshPath;
		UStaticMesh* Pistol = FParse::Value(*Params, TEXT("PistolMesh="), MeshPath) ? LoadObject<UStaticMesh>(nullptr, *MeshPath)
			: FTWeaponProps::FindMesh(FTWeaponProps::PistolGrip);
		if (!Pistol)
		{
			UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: no pistol mesh (%s); import it with FTImportProp into %s first."),
				MeshPath.IsEmpty() ? FTWeaponProps::PistolGrip.Mesh : *MeshPath, FTWeaponProps::Folder);
		}
		if (!Body || !Pistol || !WS::ReadJsonFile(WS::FullPath(PistolFile), Doc))
		{
			++Problems;
		}
		else
		{
			const double BarrelYaw = WS::PistolYaw(*Pistol);
			Problems += WS::PutMuzzleSocket(*Pistol, Doc.root(), BarrelYaw);
			Problems += WS::PutPistolSocket(*Body, Doc.root(), TEXT("grip_hand_r"), FTWeaponProps::PistolGrip, BarrelYaw);
			Problems += WS::PutPistolSocket(*Body, Doc.root(), TEXT("holster_rest_thigh_r"), FTWeaponProps::PistolHolster, BarrelYaw);
			FString Lods;
			if (FParse::Value(*Params, TEXT("PistolLods="), Lods, false))
			{
				Problems += WS::PutPistolLods(*Pistol, Lods);
			}
			ToSave.AddUnique(Pistol);
			ToSave.AddUnique(Body);
		}
	}

	if (Problems > 0)
	{
		UE_LOG(LogForgottenThird, Error, TEXT("FTWeaponSockets: %d problem(s); nothing saved."), Problems);
		return 1;
	}
	if (bDryRun)
	{
		UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: dry run; %d asset(s) not saved."), ToSave.Num());
		return 0;
	}
	for (UObject* Asset : ToSave)
	{
		if (!WS::SaveWeaponSocketAsset(*Asset))
		{
			return 1;
		}
	}
	UE_LOG(LogForgottenThird, Display, TEXT("FTWeaponSockets: done, %d asset(s) saved."), ToSave.Num());
	return 0;
#else
	return 1;
#endif
}
