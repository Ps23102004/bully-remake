// OWNER: mission-fw
#include "BullyProgressSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyProgress, Log, All);

UBullyProgressSubsystem* UBullyProgressSubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext) { return nullptr; }
	const UWorld* World = WorldContext->GetWorld();
	const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UBullyProgressSubsystem>() : nullptr;
}

void UBullyProgressSubsystem::AddMoney(int32 Delta)
{
	if (Delta == 0) { return; }
	Money = FMath::Max(0, Money + Delta);
	UE_LOG(LogBullyProgress, Display, TEXT("BR_PROGRESS money %+d -> %d"), Delta, Money);
	OnChanged.Broadcast();
}

void UBullyProgressSubsystem::AddRespect(int32 Delta)
{
	if (Delta == 0) { return; }
	Respect = FMath::Max(0, Respect + Delta);
	UE_LOG(LogBullyProgress, Display, TEXT("BR_PROGRESS respect %+d -> %d"), Delta, Respect);
	OnChanged.Broadcast();
}

void UBullyProgressSubsystem::ResetProgress()
{
	Money = 0;
	Respect = 0;
	UE_LOG(LogBullyProgress, Display, TEXT("BR_PROGRESS reset -> money 0 respect 0"));
	OnChanged.Broadcast();
}

// ---------------------------------------------------------------------------------------------------------------
// console commands (testing / headless verification)

namespace
{
	UBullyProgressSubsystem* SysFor(UWorld* W) { return UBullyProgressSubsystem::Get(W); }

	FAutoConsoleCommandWithWorldAndArgs GMoney(TEXT("Progress.Money"), TEXT("Progress.Money <n>: add n to the wallet"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W)
		{
			if (UBullyProgressSubsystem* S = SysFor(W)) { S->AddMoney(A.Num() >= 1 ? FCString::Atoi(*A[0]) : 0); }
		}));

	FAutoConsoleCommandWithWorldAndArgs GRespect(TEXT("Progress.Respect"), TEXT("Progress.Respect <n>: add n respect"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& A, UWorld* W)
		{
			if (UBullyProgressSubsystem* S = SysFor(W)) { S->AddRespect(A.Num() >= 1 ? FCString::Atoi(*A[0]) : 0); }
		}));

	FAutoConsoleCommandWithWorld GStatus(TEXT("Progress.Status"), TEXT("Log money / respect"),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W)
		{
			if (UBullyProgressSubsystem* S = SysFor(W))
			{
				UE_LOG(LogBullyProgress, Display, TEXT("BR_PROGRESS status: money=%d respect=%d"), S->GetMoney(), S->GetRespect());
			}
		}));

	FAutoConsoleCommandWithWorld GReset(TEXT("Progress.Reset"), TEXT("Zero money and respect"),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W)
		{
			if (UBullyProgressSubsystem* S = SysFor(W)) { S->ResetProgress(); }
		}));
}
