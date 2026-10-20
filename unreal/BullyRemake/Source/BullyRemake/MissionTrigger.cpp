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

bool AMissionTrigger::TouchesSegment(const FVector& A, const FVector& B, const FVector& Margin) const
{
	const FTransform& T = Box->GetComponentTransform();
	const FVector LA = T.InverseTransformPositionNoScale(A);
	const FVector D = T.InverseTransformPositionNoScale(B) - LA;
	const FVector E = Box->GetScaledBoxExtent() + Margin;
	double T0 = 0.0, T1 = 1.0;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (FMath::IsNearlyZero(D[Axis]))
		{
			if (FMath::Abs(LA[Axis]) > E[Axis]) { return false; }   // parallel to this slab and outside it
			continue;
		}
		double Near = (-E[Axis] - LA[Axis]) / D[Axis];
		double Far = (E[Axis] - LA[Axis]) / D[Axis];
		if (Near > Far) { Swap(Near, Far); }
		T0 = FMath::Max(T0, Near);
		T1 = FMath::Min(T1, Far);
		if (T0 > T1) { return false; }
	}
	return true;
}

bool AMissionTrigger::ForceEnter()
{
	if (bInteract) { return false; }
	ForcedUntil = GetWorld()->GetTimeSeconds() + 0.3;
	if (bPlayerInside) { return false; }
	bPlayerInside = true;
	if (UMissionSubsystem* Sys = UMissionSubsystem::Get(GetWorld())) { Sys->NotifyTrigger(this, true); }
	return true;
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
	const bool bNow = (Pawn && ContainsPoint(Pawn->GetActorLocation())) || (World && World->GetTimeSeconds() < ForcedUntil);
	if (bNow != bPlayerInside)
	{
		bPlayerInside = bNow;
		if (UMissionSubsystem* Sys = UMissionSubsystem::Get(World))
		{
			Sys->NotifyTrigger(this, bNow);
		}
	}
}
