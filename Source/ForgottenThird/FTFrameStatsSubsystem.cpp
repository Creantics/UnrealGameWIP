#include "FTFrameStatsSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ForgottenThird.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RHI.h"
#include "RenderCore.h"
#include "UObject/UObjectIterator.h"

#include "ftcore/FrameStats.h"

namespace
{
constexpr std::size_t kFrameStatsCapacity = 30000;
}

UFTFrameStatsSubsystem::UFTFrameStatsSubsystem() = default;
UFTFrameStatsSubsystem::~UFTFrameStatsSubsystem() = default;

bool UFTFrameStatsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && FParse::Param(FCommandLine::Get(), TEXT("FrameStats")) && Super::ShouldCreateSubsystem(Outer);
}

void UFTFrameStatsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("FrameStatsSkip="), Skip);
	FParse::Value(FCommandLine::Get(), TEXT("FrameStatsCensusAt="), CensusAt);
	Stats = std::make_unique<ftcore::FrameStats>(kFrameStatsCapacity);
}

void UFTFrameStatsSubsystem::Deinitialize()
{
	Report();
	Super::Deinitialize();
}

TStatId UFTFrameStatsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFTFrameStatsSubsystem, STATGROUP_Tickables);
}

void UFTFrameStatsSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	++Frames;
	if (Frames == CensusAt)
	{
		Census();
	}
	const double FrameMs = FApp::GetDeltaTime() * 1000.0;
	if (!Stats || Frames <= Skip || FrameMs >= 2000.0)
	{
		return;
	}
	ftcore::FrameSample S;
	S.frameMs = static_cast<float>(FrameMs);
	S.gameMs = static_cast<float>(FPlatformTime::ToMilliseconds(GGameThreadTime));
	S.renderMs = static_cast<float>(FPlatformTime::ToMilliseconds(GRenderThreadTime));
	S.gpuMs = static_cast<float>(FPlatformTime::ToMilliseconds64(RHIGetGPUFrameCycles(0)));
	S.drawCalls = static_cast<float>(GNumDrawCallsRHI[0]);
	S.primitives = static_cast<float>(GNumPrimitivesDrawnRHI[0]);
	Stats->add(S);
}

void UFTFrameStatsSubsystem::Census() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	int32 Actors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Actors;
	}
	int32 Meshes = 0, Instanced = 0, Lights = 0, LightsVisible = 0;
	int64 Instances = 0;
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		if (It->GetWorld() != World || !It->IsRegistered())
		{
			continue;
		}
		if (const UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(*It))
		{
			++Instanced;
			Instances += Ism->GetInstanceCount();
		}
		else
		{
			++Meshes;
		}
	}
	for (TObjectIterator<ULocalLightComponent> It; It; ++It)
	{
		if (It->GetWorld() != World || !It->IsRegistered())
		{
			continue;
		}
		++Lights;
		LightsVisible += It->IsVisible() ? 1 : 0;
	}
	UE_LOG(LogForgottenThird, Display,
		TEXT("FrameStats census at frame %d: %d actors, %d static mesh components, %d instanced components (%lld instances), %d local lights registered (%d visible)."),
		Frames, Actors, Meshes, Instanced, Instances, Lights, LightsVisible);
}

void UFTFrameStatsSubsystem::Report()
{
	if (bReported || !Stats)
	{
		return;
	}
	bReported = true;
	const ftcore::FrameSummary Sum = Stats->summary();
	UE_LOG(LogForgottenThird, Display,
		TEXT("FrameStats: %d frames; frame %.1f ms median (p95 %.1f, max %.1f); game %.1f (p95 %.1f); render %.1f (p95 %.1f); GPU %.1f (p95 %.1f); draw calls %.0f (p95 %.0f); primitives %.0f."),
		static_cast<int32>(Sum.count), Sum.frame.median, Sum.frame.p95, Sum.frame.max, Sum.game.median, Sum.game.p95,
		Sum.render.median, Sum.render.p95, Sum.gpu.median, Sum.gpu.p95, Sum.drawCalls.median, Sum.drawCalls.p95,
		Sum.primitives.median);
}
