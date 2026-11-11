#include "FTFightSession.h"

DEFINE_LOG_CATEGORY_STATIC(LogFTFightSession, Log, All);

static_assert(static_cast<uint8>(EFTSharedAction::Count) == static_cast<uint8>(ftcore::SharedAction::Count));
static_assert(static_cast<uint8>(EFTSharedAction::Gift) == static_cast<uint8>(ftcore::SharedAction::Gift));
static_assert(static_cast<uint8>(EFTSharedAction::RefuseDown) == static_cast<uint8>(ftcore::SharedAction::RefuseDown));
static_assert(static_cast<uint8>(EFTSharedAction::RefuseUp) == static_cast<uint8>(ftcore::SharedAction::RefuseUp));
static_assert(static_cast<uint8>(EFTSharedAction::SetTable) == static_cast<uint8>(ftcore::SharedAction::SetTable));
static_assert(static_cast<uint8>(EFTSharedAction::LeaveOpen) == static_cast<uint8>(ftcore::SharedAction::LeaveOpen));

UFTFightSession::UFTFightSession()
	: Session(MakeUnique<ftcore::FightSession>())
{
}

bool UFTFightSession::Begin(const ftcore::FightSessionDef& Def, double StartSeconds)
{
	const ftcore::FightSessionCheck Check = Session->begin(Def, StartSeconds);
	if (!Check.ok())
	{
		UE_LOG(LogFTFightSession, Warning, TEXT("Fight session: refused tuning kept its defaults:%s%s%s%s%s%s"),
			Check.count ? TEXT("") : TEXT(" count"), Check.gift ? TEXT("") : TEXT(" gift"),
			Check.places ? TEXT("") : TEXT(" places"), Check.table ? TEXT("") : TEXT(" table"),
			Check.lock ? TEXT("") : TEXT(" lock"), Check.boss ? TEXT("") : TEXT(" boss"));
	}
	return Check.ok();
}

const ftcore::FightTick& UFTFightSession::Tick(float FightDeltaSeconds)
{
	return Session->tick(static_cast<double>(FightDeltaSeconds));
}

ftcore::SharedReply UFTFightSession::Press(EFTSharedAction Action, uint8 Who, uint8 Target)
{
	return Session->press(static_cast<ftcore::SharedAction>(Action), Who, Target);
}

void UFTFightSession::BindStory(ftcore::StoryState* Story, const ftcore::FactRegistry* Registry)
{
	Session->bindStory(Story, Registry);
}
