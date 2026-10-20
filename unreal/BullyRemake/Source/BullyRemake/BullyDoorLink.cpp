// OWNER: builder-B2 (doors)
#include "BullyDoorLink.h"

#include "BullyCharacter.h"
#include "BullyRagdollComponent.h"
#include "MissionTrigger.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/MeshComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyDoor, Log, All);

namespace
{
	// r.LumenScene.FastCameraMode is a global: every link that boosts it holds a reference, the first remembers the value to put back.
	int32 GLumenBoostHolders = 0;
	int32 GLumenBoostPrev = 0;
	const TCHAR* LumenCVar = TEXT("r.LumenScene.FastCameraMode");
}

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
	if (PrewarmRadius > 0.f && GetWorld() && GetWorld()->IsGameWorld())
	{
		GetWorldTimerManager().SetTimer(PrewarmTimer, this, &ABullyDoorLink::PrewarmTick, 0.5f, true, FMath::FRandRange(0.2f, 0.7f));
	}
}

void ABullyDoorLink::EndPlay(const EEndPlayReason::Type Reason)
{
	// level change / PIE end / destroy while a boost or an input lock is pending: the timer dies with the world, the CVar and the pawn's input would not
	ReleaseLumenBoost();
	ReleaseInputLock();
	if (UWorld* W = GetWorld()) { W->GetTimerManager().ClearTimer(PrewarmTimer); }
	Super::EndPlay(Reason);
}

void ABullyDoorLink::ReleaseLumenBoost()
{
	if (UWorld* W = GetWorld()) { W->GetTimerManager().ClearTimer(LumenTimer); }
	if (!bLumenBoostHeld) { return; }
	bLumenBoostHeld = false;
	if (--GLumenBoostHolders <= 0)
	{
		GLumenBoostHolders = 0;
		if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(LumenCVar)) { CV->Set(GLumenBoostPrev, ECVF_SetByGameSetting); }
		UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR lumen boost released (%s back to %d)"), LumenCVar, GLumenBoostPrev);
	}
}

void ABullyDoorLink::ReleaseInputLock()
{
	if (APlayerController* PC = InputLockedPC.Get()) { PC->SetIgnoreMoveInput(false); }   // the counter is balanced against OUR set(true) only
	InputLockedPC.Reset();
}

// Pre-stream the textures of everything around the partner's exit while the player is near this door, so the first frame
// after the cut already has real mips instead of the 1-2 lowest (the "pop-in after entering"). Cheap: one overlap query per
// 0.5 s per nearby door, components re-requested only when their hold is about to run out.
void ABullyDoorLink::PrewarmTick()
{
	UWorld* W = GetWorld();
	ABullyDoorLink* Dest = GetPartner();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	APawn* P = PC ? PC->GetPawn() : nullptr;
	if (!Dest || !P || FVector::Dist(P->GetActorLocation(), GetActorLocation()) > PrewarmRadius) { return; }
	const double Now = W->GetTimeSeconds();
	TArray<FOverlapResult> Hits;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyDoorPrewarm), /*bTraceComplex*/ true);
	W->OverlapMultiByObjectType(Hits, Dest->ExitLocation, FQuat::Identity,
		FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllStaticObjects), FCollisionShape::MakeSphere(PrewarmExitRadius), Q);
	int32 Requested = 0;
	for (const FOverlapResult& H : Hits)
	{
		UMeshComponent* MC = Cast<UMeshComponent>(H.GetComponent());
		if (!MC) { continue; }
		double& Until = Warmed.FindOrAdd(MC, -1.0);
		if (Until > Now + 1.0) { continue; }   // still held
		MC->PrestreamTextures(PrewarmHoldSeconds, /*bPrioritizeCharacterTextures*/ false, 0);
		Until = Now + PrewarmHoldSeconds;
		++Requested;
	}
	if (Requested > 0)
	{
		PrewarmedComponents += Requested;
		UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR prewarm '%s': %d mesh component(s) around '%s' held resident for %.0fs (%d overlapped)"),
			*LinkId.ToString(), Requested, *Dest->LinkId.ToString(), PrewarmHoldSeconds, Hits.Num());
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

ABullyDoorLink* ABullyDoorLink::ResolveDestination(const ABullyCharacter* Pawn)
{
	ABullyDoorLink* Natural = GetPartner();
	ABullyDoorLink* Entry = Pawn ? Pawn->LastEntryDoor.Get() : nullptr;
	if (Entry && Entry != this && Entry != Natural && Entry->PartnerId == LinkId)
	{
		UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR '%s' is shared: '%s' entered through '%s', going back out there (declared partner '%s')"),
			*LinkId.ToString(), *GetNameSafe(Pawn), *Entry->LinkId.ToString(), Natural ? *Natural->LinkId.ToString() : TEXT("none"));
		return Entry;
	}
	return Natural;
}

void ABullyDoorLink::OnTriggerBegin(UPrimitiveComponent*, AActor* OtherActor, UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	if (ABullyCharacter* Pawn = Cast<ABullyCharacter>(OtherActor))
	{
		if (Pawn->IsPlayerControlled() && !UseDoor(Pawn) && GetWorld())
		{
			// refused (re-entry cooldown, or the pawn is mid-transition): an overlap begins only once, so a player who walked
			// straight back into the doorway right after arriving would stand in it forever. Retry while still inside.
			TWeakObjectPtr<ABullyCharacter> Weak(Pawn);
			GetWorldTimerManager().SetTimer(RetryTimer, FTimerDelegate::CreateWeakLambda(this, [this, Weak]()
			{
				ABullyCharacter* P = Weak.Get();
				if (P && Trigger && Trigger->IsOverlappingActor(P) && P->IsPlayerControlled())
				{
					UseDoor(P);
				}
				else
				{
					GetWorldTimerManager().ClearTimer(RetryTimer);
				}
			}), 0.25f, true);
		}
	}
}

bool ABullyDoorLink::UseDoor(ABullyCharacter* Pawn, float FadeOverride)
{
	UWorld* World = GetWorld();
	if (!Pawn || !World || Pawn->bDoorBusy || State != ETransition::Idle || World->GetTimeSeconds() - Pawn->LastDoorUseTime < CooldownSeconds)
	{
		return false;
	}
	ABullyDoorLink* Dest = ResolveDestination(Pawn);
	if (!Dest)
	{
		UE_LOG(LogBullyDoor, Warning, TEXT("BR_DOOR '%s' used but partner '%s' is missing"), *LinkId.ToString(), *PartnerId.ToString());
		return false;
	}
	GetWorldTimerManager().ClearTimer(RetryTimer);
	CutFrom = Pawn->GetActorLocation();
	ActiveFade = GetEffectiveFade(FadeOverride);
	Pawn->bDoorBusy = true;
	Traveler = Pawn;
	Destination = Dest;
	StateTime = 0.f;
	BlackFrames = 0;
	bTeleported = false;
	if (ActiveFade <= 0.f)
	{
		// instant seamless cut: teleport on the next tick (not inside the physics overlap callback that got us here)
		State = ETransition::Cut;
		SetActorTickEnabled(true);
		UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR use '%s' (%s) at %s -> '%s' (instant cut, no fade) t=%.3f"),
			*LinkId.ToString(), *Label, *Pawn->GetActorLocation().ToString(), *Dest->LinkId.ToString(), World->GetTimeSeconds());
		return true;
	}
	if (APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
	{
		PC->SetIgnoreMoveInput(true);
		InputLockedPC = PC;
	}
	State = ETransition::FadeOut;
	SetActorTickEnabled(true);
	UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR use '%s' (%s) at %s -> '%s' (fade %.2fs, settle %.2fs/%d frames) state=FadeOut t=%.3f"),
		*LinkId.ToString(), *Label, *Pawn->GetActorLocation().ToString(), *Dest->LinkId.ToString(), ActiveFade, SettleSeconds, SettleFrames, World->GetTimeSeconds());
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
		ReleaseInputLock();
		SetActorTickEnabled(false);
		return;
	}
	const float F = FMath::Max(ActiveFade, 0.02f);
	StateTime += DeltaSeconds;
	switch (State)
	{
	case ETransition::Cut:
	{
		// A mission trigger (polled once per tick) that shares the doorway - "go in through the main door" - would never see a
		// player who is teleported away in the same tick he reaches it (the old 0.25 s fade hid this). Report the entry first, but only
		// for triggers the traveller actually touched on the way in (its path from where it hit the door to where it stands, thickened by
		// the capsule): a trigger that merely sits near the doorway (a neighbouring room, a loose AABB overlap) must not fire.
		if (Pawn->IsPlayerControlled())
		{
			const UCapsuleComponent* Cap = Pawn->GetCapsuleComponent();
			const FVector Margin(Cap->GetScaledCapsuleRadius(), Cap->GetScaledCapsuleRadius(), Cap->GetScaledCapsuleHalfHeight());
			for (TActorIterator<AMissionTrigger> It(World); It; ++It)
			{
				if (It->Box && It->TouchesSegment(CutFrom, Pawn->GetActorLocation(), Margin) && It->ForceEnter())
				{
					UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR '%s' reported mission trigger '%s' entered before the cut"), *LinkId.ToString(), *It->TriggerId.ToString());
				}
			}
		}
		DoTeleport(Pawn, Dest);
		++InstantCuts;
		FinishTransition(Pawn);
		State = ETransition::Idle;
		SetActorTickEnabled(false);
		UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR '%s' instant cut complete t=%.3f (cut #%d)"), *LinkId.ToString(), World->GetTimeSeconds(), InstantCuts);
		break;
	}
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
	if (UBullyRagdollComponent* RD = Pawn->GetRagdoll()) { RD->Abort(); }   // else its tick drags the capsule straight back to the body
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
	if (LumenBoostSeconds > 0.f)
	{
		// the new room's Lumen surface cache starts cold: let it capture cards at double rate for a moment (engine's own
		// camera-cut mode), then restore the value it had. Visual effect is for a rendered check; headless it only logs.
		if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(LumenCVar))
		{
			if (!bLumenBoostHeld)
			{
				bLumenBoostHeld = true;
				if (GLumenBoostHolders++ == 0) { GLumenBoostPrev = CV->GetInt(); }
			}
			CV->Set(1, ECVF_SetByGameSetting);
			World->GetTimerManager().SetTimer(LumenTimer, this, &ABullyDoorLink::ReleaseLumenBoost, LumenBoostSeconds, false);   // re-arms on a second cut
		}
	}
	Pawn->LastDoorUseTime = World->GetTimeSeconds();
	Pawn->DoorUseCount++;
	Pawn->LastEntryDoor = this;
	UE_LOG(LogBullyDoor, Display, TEXT("BR_DOOR teleported via '%s' to %s yaw %.0f (use #%d) t=%.3f"), *Dest->LinkId.ToString(), *Pawn->GetActorLocation().ToString(), Dest->ExitYaw, Pawn->DoorUseCount, World->GetTimeSeconds());
}

void ABullyDoorLink::FinishTransition(ABullyCharacter* Pawn)
{
	ReleaseInputLock();   // only if this link took it (the instant cut never does): a chat / menu's own lock is not ours to drop
	Pawn->LastDoorUseTime = GetWorld()->GetTimeSeconds();
	Pawn->bDoorBusy = false;
}
