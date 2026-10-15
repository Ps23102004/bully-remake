// OWNER: mission-fw
#include "MissionTrigger.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "MissionSubsystem.h"

AMissionTrigger::AMissionTrigger()
{
	PrimaryActorTick.bCanEverTick = true;
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	SetRootComponent(Box);
	Box->SetBoxExtent(FVector(100.f, 100.f, 150.f));
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // pure data volume; containment is polled
	Box->SetGenerateOverlapEvents(false);
	Box->SetHiddenInGame(true);
}

bool AMissionTrigger::ContainsPoint(const FVector& WorldPoint) const
{
	const FVector Local = Box->GetComponentTransform().InverseTransformPositionNoScale(WorldPoint);
	const FVector E = Box->GetScaledBoxExtent();
	return FMath::Abs(Local.X) <= E.X && FMath::Abs(Local.Y) <= E.Y && FMath::Abs(Local.Z) <= E.Z;
}

void AMissionTrigger::BeginPlay()
{
	Super::BeginPlay();
	if (UMissionSubsystem* Sys = UMissionSubsystem::Get(GetWorld()))
	{
		Sys->RegisterTrigger(this);
	}
}

void AMissionTrigger::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UMissionSubsystem* Sys = UMissionSubsystem::Get(GetWorld()))
	{
		Sys->UnregisterTrigger(this);
	}
	Super::EndPlay(Reason);
}

void AMissionTrigger::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	APawn* Pawn = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
	const bool bNow = Pawn && ContainsPoint(Pawn->GetActorLocation());
	if (bNow != bPlayerInside)
	{
		bPlayerInside = bNow;
		if (UMissionSubsystem* Sys = UMissionSubsystem::Get(World))
		{
			Sys->NotifyTrigger(this, bNow);
		}
	}
}
