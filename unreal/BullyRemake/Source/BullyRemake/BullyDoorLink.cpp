// OWNER: builder-B2 (doors)
#include "BullyDoorLink.h"

#include "BullyPawn.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyDoor, Log, All);

ABullyDoorLink::ABullyDoorLink()
{
	PrimaryActorTick.bCanEverTick = false;
	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
	SetRootComponent(Trigger);
	Trigger->SetBoxExtent(FVector(40.f, 100.f, 150.f));
	Trigger->SetCollisionProfileName(TEXT("OverlapOnlyPawn"));
	Trigger->SetGenerateOverlapEvents(true);
	Trigger->SetHiddenInGame(true);
}

void ABullyDoorLink::BeginPlay()
{
	Super::BeginPlay();
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &ABullyDoorLink::OnTriggerBegin);
	if (ABullyDoorLink* P = GetPartner())
	{
		UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR link '%s' (%s) -> partner '%s' ready"), *LinkId.ToString(), *Label, *P->LinkId.ToString());
	}
	else
	{
		UE_LOG(LogBullyDoor, Warning, TEXT("BR_DOOR link '%s' has NO partner '%s'"), *LinkId.ToString(), *PartnerId.ToString());
	}
}

ABullyDoorLink* ABullyDoorLink::GetPartner()
{
	if (!Partner.IsValid())
	{
		for (TActorIterator<ABullyDoorLink> It(GetWorld()); It; ++It)
		{
			if (*It != this && It->LinkId == PartnerId)
			{
				Partner = *It;
				break;
			}
		}
	}
	return Partner.Get();
}

void ABullyDoorLink::OnTriggerBegin(UPrimitiveComponent*, AActor* OtherActor, UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	if (ABullyPawn* Pawn = Cast<ABullyPawn>(OtherActor))
	{
		if (Pawn->IsPlayerControlled())
		{
			UseDoor(Pawn);
		}
	}
}

bool ABullyDoorLink::UseDoor(ABullyPawn* Pawn)
{
	UWorld* World = GetWorld();
	if (!Pawn || !World || Pawn->bDoorBusy || World->GetTimeSeconds() - Pawn->LastDoorUseTime < CooldownSeconds)
	{
		return false;
	}
	ABullyDoorLink* Dest = GetPartner();
	if (!Dest)
	{
		UE_LOG(LogBullyDoor, Warning, TEXT("BR_DOOR '%s' used but partner '%s' is missing"), *LinkId.ToString(), *PartnerId.ToString());
		return false;
	}
	Pawn->bDoorBusy = true;
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	if (PC)
	{
		PC->SetIgnoreMoveInput(true);
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->StartCameraFade(0.f, 1.f, FadeSeconds, FLinearColor::Black, /*bFadeAudio*/ false, /*bHoldWhenFinished*/ true);
		}
	}
	UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR use '%s' (%s) at %s -> '%s' (fade %.2fs)"),
		*LinkId.ToString(), *Label, *Pawn->GetActorLocation().ToString(), *Dest->LinkId.ToString(), FadeSeconds);
	FTimerDelegate D = FTimerDelegate::CreateUObject(this, &ABullyDoorLink::CompleteTransition, Pawn, Dest);
	World->GetTimerManager().SetTimer(FadeTimer, D, FMath::Max(FadeSeconds, 0.02f), false);
	return true;
}

void ABullyDoorLink::CompleteTransition(ABullyPawn* Pawn, ABullyDoorLink* Dest)
{
	UWorld* World = GetWorld();
	if (!Pawn || !Dest || !World)
	{
		return;
	}
	// Snap the exit onto the floor: short complex trace around the authored height, else keep it.
	FVector Loc = Dest->ExitLocation;
	const float HalfHeight = Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FHitResult Hit;
	FCollisionQueryParams P(SCENE_QUERY_STAT(BullyDoorExit), /*bTraceComplex*/ true, Pawn);
	if (World->LineTraceSingleByChannel(Hit, Loc + FVector(0, 0, 150.f), Loc - FVector(0, 0, 400.f), ECC_Visibility, P))
	{
		Loc = Hit.ImpactPoint;
	}
	else
	{
		UE_LOG(LogBullyDoor, Warning, TEXT("BR_DOOR exit floor trace MISSED at %s; using authored height"), *Loc.ToString());
	}
	Loc.Z += HalfHeight + 2.f;
	const FRotator Face(0.f, Dest->ExitYaw, 0.f);
	Pawn->TeleportTo(Loc, Face, false, true);
	Pawn->GetCharacterMovement()->StopMovementImmediately();
	if (AController* C = Pawn->GetController())
	{
		C->SetControlRotation(FRotator(-8.f, Dest->ExitYaw, 0.f));
		if (APlayerController* PC = Cast<APlayerController>(C))
		{
			if (PC->PlayerCameraManager)
			{
				PC->PlayerCameraManager->StartCameraFade(1.f, 0.f, FadeSeconds, FLinearColor::Black, false, false);
			}
		}
	}
	Pawn->LastDoorUseTime = World->GetTimeSeconds();
	Pawn->DoorUseCount++;
	UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR arrived via '%s' at %s yaw %.0f (use #%d)"), *Dest->LinkId.ToString(), *Pawn->GetActorLocation().ToString(), Dest->ExitYaw, Pawn->DoorUseCount);
	FTimerDelegate D = FTimerDelegate::CreateUObject(this, &ABullyDoorLink::FinishTransition, Pawn);
	World->GetTimerManager().SetTimer(FadeTimer, D, FMath::Max(FadeSeconds, 0.02f), false);
}

void ABullyDoorLink::FinishTransition(ABullyPawn* Pawn)
{
	if (!Pawn)
	{
		return;
	}
	if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
	{
		PC->SetIgnoreMoveInput(false);
	}
	Pawn->LastDoorUseTime = GetWorld()->GetTimeSeconds();
	Pawn->bDoorBusy = false;
}
