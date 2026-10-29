#include "FTHairCardsRun.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Modules/ModuleManager.h"

// The console command FT.HairCards. The hair card generator works through a Python controller that its startup
// script makes on the editor's first ticks, after any pip install it waits for, so a command given with -ExecCmds
// can come before it: the command waits for the controller on the core ticker, then runs.

namespace
{
constexpr double kWaitSeconds = 300.0;
constexpr float kPollSeconds = 1.f;
constexpr double kNoteSeconds = 30.0;

struct FRequest
{
	FString SpecPath = TEXT("Data/Grooms/hair_cards.json");
	FString Only;
	bool bQuit = false;
	double Started = 0.0;
	double LastNote = 0.0;
};

FRequest Request;
FTSTicker::FDelegateHandle WaitHandle;
bool bBusy = false;
bool bRunning = false;

void Finish(const int32 Failed)
{
	WaitHandle.Reset();
	bBusy = false;
	if (Request.bQuit)
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(Failed == 0 ? 0 : 1), TEXT("FT.HairCards"));
	}
}

bool WaitForGenerator(float)
{
	// A modal loop inside the run can tick this ticker again.
	if (bRunning)
	{
		return true;
	}
	if (!FTHairCards::IsGeneratorReady())
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - Request.Started < kWaitSeconds)
		{
			if (Now - Request.LastNote >= kNoteSeconds)
			{
				Request.LastNote = Now;
				UE_LOG(LogFTHairCards, Display, TEXT("FT.HairCards: waiting for the hair card generator (%.0f s)"), Now - Request.Started);
			}
			return true;
		}
		UE_LOG(LogFTHairCards, Error, TEXT("FT.HairCards: the hair card generator's Python controller didn't come up in %.0f s. On the first launch pass -ForcePipInstallOnInit so its Python packages install before startup, and look for Python import errors above."), kWaitSeconds);
		Finish(-1);
		return false;
	}
	bRunning = true;
	const int32 Failed = FTHairCards::Run(Request.SpecPath, Request.Only);
	bRunning = false;
	Finish(Failed);
	return false;
}

void HairCardsCommand(const TArray<FString>& Args)
{
	if (bBusy)
	{
		UE_LOG(LogFTHairCards, Warning, TEXT("FT.HairCards: already running"));
		return;
	}
	Request = FRequest();
	bool bBadArg = false;
	for (const FString& Arg : Args)
	{
		if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
		{
			Request.bQuit = true;
		}
		else if (Arg.StartsWith(TEXT("only="), ESearchCase::IgnoreCase))
		{
			Request.Only = Arg.RightChop(5);
			if (Request.Only.IsEmpty())
			{
				bBadArg = true;
			}
		}
		else
		{
			Request.SpecPath = Arg;
		}
	}
	bBusy = true;
	if (bBadArg)
	{
		UE_LOG(LogFTHairCards, Error, TEXT("FT.HairCards: only= needs a groom id"));
		Finish(-1);
		return;
	}
	Request.Started = Request.LastNote = FPlatformTime::Seconds();
	WaitHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&WaitForGenerator), kPollSeconds);
}
} // namespace

class FFTHairCardsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Command = IConsoleManager::Get().RegisterConsoleCommand(TEXT("FT.HairCards"),
			TEXT("FT.HairCards [spec] [only=<id>] [quit]: builds and saves hair cards for the grooms in the hair card spec (default Data/Grooms/hair_cards.json) with the engine's hair card generator. See docs/HAIR_CARDS.md."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&HairCardsCommand), ECVF_Default);
	}

	virtual void ShutdownModule() override
	{
		if (WaitHandle.IsValid())
		{
			FTSTicker::RemoveTicker(WaitHandle);
			WaitHandle.Reset();
		}
		if (Command)
		{
			IConsoleManager::Get().UnregisterConsoleObject(Command);
			Command = nullptr;
		}
	}

private:
	IConsoleCommand* Command = nullptr;
};

IMPLEMENT_MODULE(FFTHairCardsModule, FTHairCards)
