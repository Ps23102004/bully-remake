// OWNER: slice-combat
#include "BullyCombatTest.h"

#include "BullyCharacter.h"
#include "BullyCombatAI.h"
#include "BullyCombatAnimInstance.h"
#include "BullyCombatComponent.h"
#include "BullyCombatPawn.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MissionSubsystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyCombatTest, Log, All);

namespace
{
	// 1_02 arena: the dorm court where the tutorial fight happens, game metres (271.2, -105.9) = `1_02_PlayerNISLocs`
	// (docs/re-notes/opening-slice-recon.md B5). UE cm = (100x, -100y).
	const FVector2D ArenaXY(27120.f, 10590.f);

	ABullyCharacter* FindPlayer(UWorld* W)
	{
		APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
		return PC ? Cast<ABullyCharacter>(PC->GetPawn()) : nullptr;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// console commands (hands playtest in PIE / -game)

static FAutoConsoleCommandWithWorldAndArgs GCombatSpawn(TEXT("Combat.SpawnEnemy"),
	TEXT("Spawn a sparring partner 3 m in front of Jimmy. Optional arg: health floor (e.g. 5 = beaten + humiliation instead of KO)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* W)
	{
		ABullyCharacter* P = FindPlayer(W);
		if (!P) { UE_LOG(LogBullyCombatTest, Warning, TEXT("BR_COMBAT no player pawn")); return; }
		UBullyCombatTest::SpawnEnemyInFront(W, P, 300.f, Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.f);
	}));

static FAutoConsoleCommandWithWorldAndArgs GCombatStatus(TEXT("Combat.Status"), TEXT("Log every fighter's combat state."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* W)
	{
		for (const TWeakObjectPtr<UBullyCombatComponent>& C : UBullyCombatComponent::Registry())
		{
			if (C.IsValid() && C->GetWorld() == W) { UE_LOG(LogBullyCombatTest, Display, TEXT("BR_COMBAT status %s"), *C->Describe()); }
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GCombatGate(TEXT("Combat.Gate"), TEXT("Combat.Gate <attack|block> <0|1>: tutorial AI permission on every sparring partner."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* W)
	{
		if (Args.Num() < 2) { return; }
		const bool bOn = FCString::Atoi(*Args[1]) != 0;
		for (TActorIterator<ABullyCombatPawn> It(W); It; ++It)
		{
			if (Args[0] == TEXT("attack")) { It->bAttackAllowed = bOn; }
			else if (Args[0] == TEXT("block")) { It->bBlockAllowed = bOn; }
		}
		UE_LOG(LogBullyCombatTest, Display, TEXT("BR_COMBAT gate %s = %d"), *Args[0], bOn ? 1 : 0);
	}));

static FAutoConsoleCommandWithWorldAndArgs GCombatInvuln(TEXT("Combat.Invulnerable"), TEXT("Combat.Invulnerable <0|1>: Jimmy takes reactions but no damage (tutorial)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* W)
	{
		if (ABullyCharacter* P = FindPlayer(W)) { P->GetCombat()->bInvulnerable = Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0; }
	}));

static FAutoConsoleCommandWithWorldAndArgs GCombatReset(TEXT("Combat.Reset"), TEXT("Restore every fighter's health and state."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* W)
	{
		for (const TWeakObjectPtr<UBullyCombatComponent>& C : UBullyCombatComponent::Registry())
		{
			if (C.IsValid() && C->GetWorld() == W) { C->ResetFighter(); }
		}
	}));

ABullyCombatPawn* UBullyCombatTest::SpawnEnemyInFront(UWorld* W, ABullyCharacter* P, float Dist, float HealthFloor, const FString& MeshOverride)
{
	const FVector Fwd = P->GetActorForwardVector().GetSafeNormal2D();
	const FVector At = P->GetActorLocation() + Fwd * Dist + FVector(0, 0, 20.f);
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	SP.bDeferConstruction = true;
	ABullyCombatPawn* E = W->SpawnActor<ABullyCombatPawn>(ABullyCombatPawn::StaticClass(), At, (-Fwd).Rotation(), SP);
	if (!E) { return nullptr; }
	E->HealthFloor = HealthFloor;
	// Optional per-spawn ped mesh (e.g. a prefect stand-in for a cutscene). Set before FinishSpawning so the pawn's
	// PostInitializeComponents loads it instead of the Config default.
	if (!MeshOverride.IsEmpty()) { E->PedMeshPath = MeshOverride; }
	E->FinishSpawning(FTransform((-Fwd).Rotation(), At));
	UE_LOG(LogBullyCombatTest, Display, TEXT("BR_COMBAT spawned %s at %s (%.0f cm ahead, floor %.0f, mesh %s)"), *E->GetName(), *E->GetActorLocation().ToString(),
		Dist, HealthFloor, *E->PedMeshPath);
	return E;
}

// ---------------------------------------------------------------------------------------------------------------
// headless test

bool UBullyCombatTest::ShouldCreateSubsystem(UObject* Outer) const
{
	return FParse::Param(FCommandLine::Get(), TEXT("BRCombatTest")) || FString(FCommandLine::Get()).Contains(TEXT("-BRCombatTest="));
}

void UBullyCombatTest::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (!FParse::Value(FCommandLine::Get(), TEXT("-BRCombatTest="), OutDir))
	{
		OutDir = FPaths::ProjectSavedDir() / TEXT("BullyCombat");
	}
	IFileManager::Get().MakeDirectory(*OutDir, true);
	bActive = true;
	Note(FString::Printf(TEXT("test armed -> %s"), *OutDir));
}

void UBullyCombatTest::Deinitialize()
{
	bActive = false;
	Super::Deinitialize();
}

TStatId UBullyCombatTest::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBullyCombatTest, STATGROUP_Tickables);
}

void UBullyCombatTest::Note(const FString& S)
{
	UE_LOG(LogBullyCombatTest, Display, TEXT("BR_COMBAT %s"), *S);
	Lines.Add(FString::Printf(TEXT("t=%.2f %s"), Elapsed, *S));
}

void UBullyCombatTest::Pass(const TCHAR* Name, const FString& Detail)
{
	Note(FString::Printf(TEXT("PASS %s: %s"), Name, *Detail));
	Results.Add(TPair<FString, bool>(FString::Printf(TEXT("%s: %s"), Name, *Detail), true));
}

void UBullyCombatTest::Fail(const TCHAR* Name, const FString& Detail)
{
	Note(FString::Printf(TEXT("FAIL %s: %s"), Name, *Detail));
	Results.Add(TPair<FString, bool>(FString::Printf(TEXT("%s: %s"), Name, *Detail), false));
}

void UBullyCombatTest::Next(EStep S)
{
	Step = S;
	StepStart = Elapsed;
	StepBase = Events;
	// tutorial steps start from a fresh partner (the real 1_02 partner is floored and the player invulnerable)
	if (Enemy.IsValid() && S >= EStep::Block && S <= EStep::GroundKick && !Enemy->GetCombat()->IsDown())
	{
		Enemy->GetCombat()->ResetFighter();
	}
	Phase = 0;
	Attempts = 0;
	LastAction = -10.0;
}

void UBullyCombatTest::OnMissionEvent(FName Event)
{
	Events.FindOrAdd(Event)++;
}

float UBullyCombatTest::Dist() const
{
	return Player.IsValid() && Enemy.IsValid() ? FVector::Dist2D(Player->GetActorLocation(), Enemy->GetActorLocation()) : 1e9f;
}

void UBullyCombatTest::Press(FName Action)
{
	ABullyCharacter* P = Player.Get();
	if (!P) { return; }
	LastAction = Elapsed;
	UBullyCombatComponent* C = P->GetCombat();
	if (bInjectionWorks)
	{
		APlayerController* PC = Cast<APlayerController>(P->GetController());
		UEnhancedInputLocalPlayerSubsystem* Sub = PC ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()) : nullptr;
		const UInputAction* A = P->GetCombatAction(Action);
		if (Sub && A)
		{
			Sub->InjectInputForAction(A, FInputActionValue(true), {}, {});
			if (!bInjectionChecked && PendingCheck.IsNone())
			{
				PendingCheck = Action;
				PendingCheckFrames = 0;
			}
			return;
		}
		bInjectionWorks = false;
		Note(TEXT("input injection unavailable (no Enhanced Input local player subsystem); using direct component calls"));
	}
	if (Action == TEXT("attack")) { C->RequestAttack(); }
	else if (Action == TEXT("grapple")) { C->RequestGrapple(); }
}

void UBullyCombatTest::Hold(FName Action)
{
	ABullyCharacter* P = Player.Get();
	if (!P) { return; }
	if (bInjectionWorks)
	{
		APlayerController* PC = Cast<APlayerController>(P->GetController());
		UEnhancedInputLocalPlayerSubsystem* Sub = PC ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()) : nullptr;
		if (const UInputAction* A = P->GetCombatAction(Action); Sub && A)
		{
			Sub->InjectInputForAction(A, FInputActionValue(true), {}, {});
			return;
		}
	}
	if (Action == TEXT("block")) { P->GetCombat()->SetBlockHeld(true); }
	else if (Action == TEXT("lockon")) { P->GetCombat()->SetLockOnHeld(true); }
}

void UBullyCombatTest::Approach(float WantDist)
{
	ABullyCharacter* P = Player.Get();
	if (!P || !Enemy.IsValid() || !P->GetCombat()->CanMove()) { return; }
	const FVector D = Enemy->GetActorLocation() - P->GetActorLocation();
	if (D.Size2D() > WantDist)
	{
		P->SetScriptedGait(D.Size2D() > 400.f ? EBullyGait::Run : EBullyGait::Walk);
		P->AddMovementInput(D.GetSafeNormal2D(), 1.f);
	}
}

void UBullyCombatTest::LogPoses(const TCHAR* Tag)
{
	auto One = [](ABullyCharacter* C) -> FString
	{
		if (!C) { return TEXT("-"); }
		const UBullyCombatAnimInstance* AI = Cast<UBullyCombatAnimInstance>(C->GetMesh()->GetAnimInstance());
		const FVector Hand = C->GetActorTransform().InverseTransformPosition(C->GetMesh()->GetBoneLocation(TEXT("Root-R-Hand")));
		return FString::Printf(TEXT("%s anim=%s clip=%s w=%.2f r-hand(local)=(%.0f,%.0f,%.0f)"), *C->GetName(), *GetNameSafe(AI ? AI->GetClass() : nullptr),
			AI ? *AI->GetCombatClip().ToString() : TEXT("-"), AI ? AI->GetCombatWeight() : 0.f, Hand.X, Hand.Y, Hand.Z);
	};
	Note(FString::Printf(TEXT("pose[%s] %s | %s"), Tag, *One(Player.Get()), *One(Enemy.Get())));
}

void UBullyCombatTest::Tick(float Dt)
{
	if (!bActive || bFinished) { return; }
	UGameInstance* GI = GetGameInstance();
	UWorld* W = GI ? GI->GetWorld() : nullptr;
	if (!W || !W->HasBegunPlay()) { return; }
	Elapsed += Dt;
	const double T = Elapsed - StepStart;
	ABullyCharacter* P = Player.Get();
	ABullyCombatPawn* E = Enemy.Get();
	UBullyCombatComponent* PC = P ? P->GetCombat() : nullptr;
	UBullyCombatComponent* EC = E ? E->GetCombat() : nullptr;

	// did the first injected press reach the pawn through the bindings?
	if (!PendingCheck.IsNone() && PC && ++PendingCheckFrames >= 2)
	{
		const bool bReached = PC->GetState() != EBullyCombatState::Idle;
		if (bReached || PendingCheckFrames >= 6)
		{
			bInjectionChecked = true;
			bInjectionWorks = bReached;
			Note(FString::Printf(TEXT("input path: injected IA_%s %s (player state %s)"), *PendingCheck.ToString(),
				bReached ? TEXT("reached the pawn through Enhanced Input") : TEXT("did NOT reach the pawn; falling back to direct component calls"),
				UBullyCombatComponent::StateName(PC->GetState())));
			if (!bReached) { if (PendingCheck == TEXT("grapple")) { PC->RequestGrapple(); } else { PC->RequestAttack(); } }
			PendingCheck = NAME_None;
		}
	}
	if (Elapsed - LastPoseLog > 0.5 && P && E && (PC->GetState() == EBullyCombatState::Attacking || EC->GetState() == EBullyCombatState::Attacking))
	{
		LastPoseLog = Elapsed;
		LogPoses(PC->GetState() == EBullyCombatState::Attacking ? *PC->GetCurrentMoveName().ToString() : *EC->GetCurrentMoveName().ToString());
	}

	switch (Step)
	{
	case EStep::Warmup:
		if (Elapsed > 3.0 && FindPlayer(W))
		{
			Next(EStep::Setup);
		}
		else if (Elapsed > 120.0)
		{
			Fail(TEXT("setup"), TEXT("no ABullyCharacter player pawn after 120 s"));
			Finish();
		}
		return;
	case EStep::Setup:
	{
		P = FindPlayer(W);
		Player = P;
		PC = P->GetCombat();
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyCombatArena), true, P);
		const FVector Top(ArenaXY.X, ArenaXY.Y, 5000.f);
		FVector Ground(ArenaXY.X, ArenaXY.Y, P->GetActorLocation().Z);
		if (W->LineTraceSingleByChannel(Hit, Top, Top - FVector(0, 0, 10000.f), ECC_Visibility, Q)) { Ground = Hit.ImpactPoint; }
		ArenaCenter = Ground;
		P->SetActorLocationAndRotation(Ground + FVector(0, 0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f), FRotator(0, 90.f, 0),
			false, nullptr, ETeleportType::TeleportPhysics);
		if (APlayerController* PCtl = Cast<APlayerController>(P->GetController())) { PCtl->SetControlRotation(FRotator(-8.f, 90.f, 0.f)); }
		E = SpawnEnemyInFront(W, P, 220.f, 0.f);
		Enemy = E;
		if (!E) { Fail(TEXT("setup"), TEXT("enemy spawn failed")); Finish(); return; }
		E->bAttackAllowed = false;
		E->bBlockAllowed = false;
		if (UMissionSubsystem* MS = UMissionSubsystem::Get(W))
		{
			MissionHandle = MS->OnMissionEvent.AddUObject(this, &UBullyCombatTest::OnMissionEvent);
		}
		else
		{
			Note(TEXT("WARNING no mission subsystem: events cannot be verified"));
		}
		Note(FString::Printf(TEXT("setup arena ground %s (hit %s); player %s hp=%.0f; enemy %s hp=%.0f mesh=%s"), *Ground.ToString(),
			*GetNameSafe(Hit.GetActor()), *P->GetName(), PC->Health, *E->GetName(), E->GetCombat()->Health,
			*GetNameSafe(E->GetMesh()->GetSkeletalMeshAsset())));
		Next(EStep::Block);
		return;
	}
	default:
		break;
	}
	if (!P || !E || !PC || !EC)
	{
		Fail(TEXT("run"), TEXT("player or enemy vanished"));
		Finish();
		return;
	}
	const float D = Dist();
	bool bHoldBlock = false;

	switch (Step)
	{
	case EStep::Block:   // tutorial 1: block a hit, twice
		if (T < 1.0) { break; }
		if (Phase == 0)
		{
			Note(TEXT("step 1 block: the partner may attack; Jimmy holds block (Q / LB) locked on"));
			E->bAttackAllowed = true;
			if (ABullyCombatAIController* AI = Cast<ABullyCombatAIController>(E->GetController())) { AI->AttackSoon(0.4f); }
			Phase = 1;
		}
		bHoldBlock = true;
		Hold(TEXT("lockon"));
		if (Count(TEXT("block_success")) >= 2)
		{
			Pass(TEXT("1 block"), FString::Printf(TEXT("%d blocks, jimmy hp=%.0f guard=%.0f"), Count(TEXT("block_success")), PC->Health, PC->Guard));
			E->bAttackAllowed = false;
			Next(EStep::Combo);
		}
		else if (T > 20.0)
		{
			Fail(TEXT("1 block"), FString::Printf(TEXT("%d blocks in 20 s"), Count(TEXT("block_success"))));
			E->bAttackAllowed = false;
			Next(EStep::Combo);
		}
		break;

	case EStep::Combo:   // tutorial 2: light 3-hit chain
		if (T < 1.5) { break; }
		if (Count(TEXT("combo_done")) >= 1)
		{
			Pass(TEXT("2 combo"), FString::Printf(TEXT("3-hit chain landed (attempt %d), enemy hp=%.0f"), Attempts, EC->Health));
			Next(EStep::Grapple);
			break;
		}
		if (Phase == 0)
		{
			if (EC->GetState() != EBullyCombatState::Idle || PC->GetState() != EBullyCombatState::Idle) { break; }
			if (D > 115.f) { Approach(110.f); break; }
			if (++Attempts > 4) { Fail(TEXT("2 combo"), TEXT("no full chain in 4 attempts")); Next(EStep::Grapple); break; }
			Note(FString::Printf(TEXT("step 2 combo attempt %d at %.0f cm: attack x3 (LMB / B)"), Attempts, D));
			Press(TEXT("attack"));
			Phase = 1;
		}
		else if (Phase == 1 && Elapsed - LastAction >= 0.2) { Press(TEXT("attack")); Phase = 2; }
		else if (Phase == 2 && Elapsed - LastAction >= 0.25) { Press(TEXT("attack")); Phase = 3; }
		else if (Phase == 3 && Elapsed - LastAction >= 1.6) { Phase = 0; }
		break;

	case EStep::Grapple:   // tutorial 3-4: grapple, three grapple strikes
		if (T < 1.0) { break; }
		if (Count(TEXT("grapple_strikes_done")) >= 1)
		{
			Pass(TEXT("3-4 grapple+strikes"), FString::Printf(TEXT("grapple_start=%d strikes=%d, enemy hp=%.0f"), Count(TEXT("grapple_start")), Count(TEXT("grapple_strike")), EC->Health));
			Next(EStep::Push);
			break;
		}
		if (T > 15.0) { Fail(TEXT("3-4 grapple+strikes"), FString::Printf(TEXT("grapple_start=%d strikes=%d"), Count(TEXT("grapple_start")), Count(TEXT("grapple_strike")))); Next(EStep::Push); break; }
		if (PC->GetState() == EBullyCombatState::Grappling)
		{
			if (Elapsed - LastAction >= 0.3) { Press(TEXT("attack")); }
			break;
		}
		if (PC->GetState() == EBullyCombatState::Idle && EC->GetState() == EBullyCombatState::Idle && Elapsed - LastAction > 0.5)
		{
			if (D > 105.f) { Approach(100.f); break; }
			Note(FString::Printf(TEXT("step 3 grapple at %.0f cm (F / RB), then attack x3"), D));
			Press(TEXT("grapple"));
		}
		break;

	case EStep::Push:   // tutorial 5-6: grapple again, push to the ground
		if (T < 1.0) { break; }
		if (Count(TEXT("grapple_done")) >= 1 && EC->GetState() == EBullyCombatState::Downed)
		{
			Pass(TEXT("5-6 grapple+push"), FString::Printf(TEXT("pushed down, enemy state %s hp=%.0f"), UBullyCombatComponent::StateName(EC->GetState()), EC->Health));
			Next(EStep::GroundKick);
			break;
		}
		if (T > 15.0) { Fail(TEXT("5-6 grapple+push"), FString::Printf(TEXT("grapple_done=%d enemy %s"), Count(TEXT("grapple_done")), UBullyCombatComponent::StateName(EC->GetState()))); Next(EStep::GroundKick); break; }
		if (PC->GetState() == EBullyCombatState::Grappling)
		{
			if (Elapsed - LastAction >= 0.4) { Press(TEXT("grapple")); }
			break;
		}
		if (PC->GetState() == EBullyCombatState::Idle && EC->GetState() == EBullyCombatState::Idle && Elapsed - LastAction > 0.5)
		{
			if (D > 105.f) { Approach(100.f); break; }
			Note(TEXT("step 5-6 grapple again, then grapple (push)"));
			Press(TEXT("grapple"));
		}
		break;

	case EStep::GroundKick:   // tutorial 7: kick him while he is down
		if (Count(TEXT("ground_kick")) >= 1)
		{
			Pass(TEXT("7 ground kick"), FString::Printf(TEXT("enemy hp=%.0f state %s"), EC->Health, UBullyCombatComponent::StateName(EC->GetState())));
			Next(EStep::FreeFight);
			break;
		}
		if (T > 8.0) { Fail(TEXT("7 ground kick"), FString::Printf(TEXT("no ground kick (enemy %s at %.0f cm)"), UBullyCombatComponent::StateName(EC->GetState()), D)); Next(EStep::FreeFight); break; }
		if (EC->GetState() == EBullyCombatState::Downed && PC->GetState() == EBullyCombatState::Idle && Elapsed - LastAction > 0.4)
		{
			if (D > 150.f) { Approach(140.f); break; }
			Note(FString::Printf(TEXT("step 7 ground kick at %.0f cm (attack near a downed enemy)"), D));
			Press(TEXT("attack"));
		}
		break;

	case EStep::FreeFight:   // B7: everything on, to the KO
	case EStep::Humiliate:   // B7 floored partner: beat to the floor, then humiliate
	{
		const bool bHum = Step == EStep::Humiliate;
		if (Phase == 0)
		{
			if (T < 1.0) { break; }
			if (bHum)
			{
				E->Destroy();
				E = SpawnEnemyInFront(W, P, 250.f, 5.f);
				Enemy = E;
				EC = E ? E->GetCombat() : nullptr;
				if (!E) { Fail(TEXT("humiliate"), TEXT("respawn failed")); Finish(); return; }
				Note(TEXT("B7 floored partner (health floor 5): beat him down, then grapple to humiliate"));
			}
			else
			{
				EC->ResetFighter();
				PC->ResetFighter();
				Note(TEXT("B7 free fight: partner attacks and blocks; Jimmy fights to the KO"));
			}
			E->bAttackAllowed = true;
			E->bBlockAllowed = true;
			Phase = 1;
			break;
		}
		const int32 KOs = Count(TEXT("enemy_knocked_out"));
		if ((!bHum && KOs >= 1) || (bHum && Count(TEXT("humiliation_done")) >= 1 && EC->IsOut()))
		{
			Note(FString::Printf(TEXT("enemy KO (%s) after %.1f s; jimmy hp=%.0f, hits landed %d"), bHum ? TEXT("humiliated") : TEXT("damage"), T, PC->Health, PC->GetHitsLanded()));
			if (bHum) { Pass(TEXT("B7 humiliate"), FString::Printf(TEXT("beaten=%d humiliation_done=%d enemy_knocked_out=%d"), Count(TEXT("enemy_beaten")), Count(TEXT("humiliation_done")), KOs)); Next(EStep::Done); }
			else { Pass(TEXT("B7 free fight KO"), FString::Printf(TEXT("%.1f s, jimmy hp=%.0f, blocks=%d"), T, PC->Health, Count(TEXT("block_success")))); Next(EStep::Humiliate); }
			break;
		}
		if (PC->IsOut() || T > 90.0)
		{
			Fail(bHum ? TEXT("B7 humiliate") : TEXT("B7 free fight KO"), FString::Printf(TEXT("jimmy %s hp=%.0f, enemy %s hp=%.0f after %.0f s"), UBullyCombatComponent::StateName(PC->GetState()), PC->Health,
				UBullyCombatComponent::StateName(EC->GetState()), EC->Health, T));
			Next(bHum ? EStep::Done : EStep::Humiliate);
			break;
		}
		Hold(TEXT("lockon"));
		// scripted player: block the telegraphed wind-ups, otherwise close in and throw chains / ground kicks / humiliate
		const FBullyCombatMove* EMv = EC->GetCurrentMove();
		if (EMv && EMv->bAutoChain && EMv->Name != LastEnemyMove) { ++EnemySwings; }
		LastEnemyMove = EMv ? EMv->Name : NAME_None;
		// every third telegraphed swing is left unblocked (and not interrupted) so Jimmy's own hurt path runs too
		const bool bTakeIt = EnemySwings % 3 == 0 && EMv != nullptr;
		if (bTakeIt && PC->GetState() != EBullyCombatState::Attacking) { break; }
		const bool bThreat = EMv && D < 230.f && (EMv->bAutoChain || (EMv->ActiveStart >= 0.f && EC->GetStateTime() <= EMv->ActiveEnd));
		if (bThreat && (PC->GetState() == EBullyCombatState::Idle || PC->GetState() == EBullyCombatState::Blocking))
		{
			bHoldBlock = true;
			Phase = 1;
			break;
		}
		if (EC->GetCurrentMove() && EC->IsInActiveWindow() && PC->GetState() == EBullyCombatState::Blocking)
		{
			bHoldBlock = true;
			break;
		}
		if (PC->GetState() == EBullyCombatState::Attacking)
		{
			if (Phase == 2 && Elapsed - LastAction >= 0.2) { Press(TEXT("attack")); Phase = 3; }
			else if (Phase == 3 && Elapsed - LastAction >= 0.25) { Press(TEXT("attack")); Phase = 1; }
			break;
		}
		if (PC->GetState() != EBullyCombatState::Idle) { break; }
		if (bHum && EC->GetState() == EBullyCombatState::Beaten)
		{
			if (D > 105.f) { Approach(100.f); break; }
			if (Elapsed - LastAction > 0.5) { Note(TEXT("partner beaten: grapple (F / RB) to humiliate")); Press(TEXT("grapple")); }
			break;
		}
		if (EC->GetState() == EBullyCombatState::Downed)
		{
			if (D > 150.f) { Approach(140.f); break; }
			if (Elapsed - LastAction > 0.4) { Press(TEXT("attack")); }
			break;
		}
		if (D > 125.f) { Approach(115.f); break; }
		if (Elapsed - LastAction > 0.35 && EC->GetState() != EBullyCombatState::GettingUp)
		{
			Press(TEXT("attack"));
			Phase = 2;
		}
		break;
	}

	case EStep::Done:
		Finish();
		return;
	default:
		break;
	}

	if (bHoldBlock) { Hold(TEXT("block")); }
	else if (!bInjectionWorks && PC->GetState() == EBullyCombatState::Blocking) { PC->SetBlockHeld(false); }
}

void UBullyCombatTest::Finish()
{
	if (bFinished) { return; }
	bFinished = true;
	int32 Passed = 0;
	for (const TPair<FString, bool>& R : Results) { Passed += R.Value ? 1 : 0; }
	Note(FString::Printf(TEXT("SUMMARY %d/%d steps passed; input via %s; events: %s"), Passed, Results.Num(),
		bInjectionWorks ? TEXT("Enhanced Input injection (IA_Attack/IA_Block/IA_Grapple/IA_LockOn)") : TEXT("direct component calls"),
		*FString::JoinBy(Events, TEXT(", "), [](const TPair<FName, int32>& E) { return FString::Printf(TEXT("%s=%d"), *E.Key.ToString(), E.Value); })));

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("passed"), Passed);
	Root->SetNumberField(TEXT("steps"), Results.Num());
	Root->SetBoolField(TEXT("input_injection"), bInjectionWorks);
	TArray<TSharedPtr<FJsonValue>> Steps;
	for (const TPair<FString, bool>& R : Results)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("step"), R.Key);
		O->SetBoolField(TEXT("pass"), R.Value);
		Steps.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("results"), Steps);
	TSharedRef<FJsonObject> Ev = MakeShared<FJsonObject>();
	for (const TPair<FName, int32>& E : Events) { Ev->SetNumberField(E.Key.ToString(), E.Value); }
	Root->SetObjectField(TEXT("mission_events"), Ev);
	TArray<TSharedPtr<FJsonValue>> L;
	for (const FString& S : Lines) { L.Add(MakeShared<FJsonValueString>(S)); }
	Root->SetArrayField(TEXT("log"), L);
	FString Json;
	TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Wr);
	FFileHelper::SaveStringToFile(Json, *(OutDir / TEXT("combat_result.json")));
	Note(FString::Printf(TEXT("wrote %s; requesting exit"), *(OutDir / TEXT("combat_result.json"))));
	FPlatformMisc::RequestExit(false);
}
