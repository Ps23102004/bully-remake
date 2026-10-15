// OWNER: builder-B2 (doors)
#include "BullyDoorLink.h"

#include "BullyCharacter.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyDoor, Log, All);

ABullyDoorLink::ABullyDoorLink()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;   // enabled only while a transition runs
	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
	SetRootComponent(Trigger);
	Trigger->SetBoxExtent(FVector(40.f, 100.f, 150.f));
	// Overlap the pawn only; ignore everything else so floor/camera traces never hit the trigger.
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionObjectType(ECC_WorldDynamic);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
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
	if (ABullyCharacter* Pawn = Cast<ABullyCharacter>(OtherActor))
	{
		if (Pawn->IsPlayerControlled())
		{
			UseDoor(Pawn);
		}
	}
}

bool ABullyDoorLink::UseDoor(ABullyCharacter* Pawn)
{
	UWorld* World = GetWorld();
	if (!Pawn || !World || Pawn->bDoorBusy || State != ETransition::Idle || World->GetTimeSeconds() - Pawn->LastDoorUseTime < CooldownSeconds)
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
	if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
	{
		PC->SetIgnoreMoveInput(true);
	}
	Traveler = Pawn;
	Destination = Dest;
	State = ETransition::FadeOut;
	StateTime = 0.f;
	BlackFrames = 0;
	bTeleported = false;
	SetActorTickEnabled(true);
	UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR use '%s' (%s) at %s -> '%s' (fade %.2fs, settle %.2fs/%d frames) state=FadeOut t=%.3f"),
		*LinkId.ToString(), *Label, *Pawn->GetActorLocation().ToString(), *Dest->LinkId.ToString(), FadeSeconds, SettleSeconds, SettleFrames, World->GetTimeSeconds());
	return true;
}

namespace
{
	float Smooth01(float X)
	{
		X = FMath::Clamp(X, 0.f, 1.f);
		return X * X * (3.f - 2.f * X);
	}
}

void ABullyDoorLink::SetFade(ABullyCharacter* Pawn, float Alpha)
{
	static const bool bTrace = FParse::Param(FCommandLine::Get(), TEXT("BRDoorTrace"));
	if (bTrace)
	{
		// one line per frame while a transition runs (evidence: monotone ramp to 1.0, black hold, monotone ramp back, no pop)
		UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR_TRACE frame=%llu t=%.3f state=%d alpha=%.3f pawn=%s"), GFrameCounter,
			GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0, int32(State), Alpha, *Pawn->GetActorLocation().ToString());
	}
	if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
	{
		if (PC->PlayerCameraManager)
		{
			if (Alpha <= 0.f)
			{
				PC->PlayerCameraManager->StopCameraFade();
			}
			else
			{
				PC->PlayerCameraManager->SetManualCameraFade(FMath::Min(Alpha, 1.f), FLinearColor::Black, /*bFadeAudio*/ false);
			}
		}
	}
}

void ABullyDoorLink::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ABullyCharacter* Pawn = Traveler.Get();
	ABullyDoorLink* Dest = Destination.Get();
	UWorld* World = GetWorld();
	if (State == ETransition::Idle || !World)
	{
		SetActorTickEnabled(false);
		return;
	}
	if (!Pawn || !Dest)
	{
		State = ETransition::Idle;   // traveller destroyed mid-transition
		SetActorTickEnabled(false);
		return;
	}
	const float F = FMath::Max(FadeSeconds, 0.02f);
	StateTime += DeltaSeconds;
	switch (State)
	{
	case ETransition::FadeOut:
	{
		const float A = Smooth01(StateTime / F);
		SetFade(Pawn, A);
		if (StateTime >= F)
		{
			State = ETransition::Black;
			StateTime = 0.f;
			BlackFrames = 0;
			UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR '%s' state=Black (alpha 1.0 reached) t=%.3f"), *LinkId.ToString(), World->GetTimeSeconds());
		}
		break;
	}
	case ETransition::Black:
	{
		SetFade(Pawn, 1.f);
		++BlackFrames;
		if (!bTeleported)
		{
			// the camera has applied alpha 1.0 for one full frame before anything moves
			if (BlackFrames >= 2)
			{
				DoTeleport(Pawn, Dest);
				bTeleported = true;
				StateTime = 0.f;
				BlackFrames = 0;
			}
		}
		else if (StateTime >= SettleSeconds && BlackFrames >= SettleFrames)
		{
			State = ETransition::FadeIn;
			StateTime = 0.f;
			UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR '%s' state=FadeIn after %.2fs / %d black frames settled t=%.3f"), *LinkId.ToString(), SettleSeconds, SettleFrames, World->GetTimeSeconds());
		}
		break;
	}
	case ETransition::FadeIn:
	{
		const float A = 1.f - Smooth01(StateTime / F);
		SetFade(Pawn, A);
		if (StateTime >= F)
		{
			SetFade(Pawn, 0.f);
			FinishTransition(Pawn);
			State = ETransition::Idle;
			SetActorTickEnabled(false);
			UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR '%s' state=Idle (transition complete) t=%.3f"), *LinkId.ToString(), World->GetTimeSeconds());
		}
		break;
	}
	default:
		break;
	}
}

void ABullyDoorLink::DoTeleport(ABullyCharacter* Pawn, ABullyDoorLink* Dest)
{
	UWorld* World = GetWorld();
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
				PC->PlayerCameraManager->SetGameCameraCutThisFrame();   // no temporal history / motion blur across the teleport
			}
		}
	}
	Pawn->SnapCamera();   // drop the spring arm's location lag so the camera does not swoop from the old door
	Pawn->LastDoorUseTime = World->GetTimeSeconds();
	Pawn->DoorUseCount++;
	UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR teleported via '%s' to %s yaw %.0f (use #%d) t=%.3f"), *Dest->LinkId.ToString(), *Pawn->GetActorLocation().ToString(), Dest->ExitYaw, Pawn->DoorUseCount, World->GetTimeSeconds());
}

void ABullyDoorLink::FinishTransition(ABullyCharacter* Pawn)
{
	if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
	{
		PC->SetIgnoreMoveInput(false);
	}
	Pawn->LastDoorUseTime = GetWorld()->GetTimeSeconds();
	Pawn->bDoorBusy = false;
}
