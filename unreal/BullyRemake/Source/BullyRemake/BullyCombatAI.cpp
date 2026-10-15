// OWNER: slice-combat
#include "BullyCombatAI.h"

#include "BullyCombatComponent.h"
#include "BullyCombatPawn.h"
#include "Engine/World.h"
#include "MissionSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyCombatAI, Log, All);

ABullyCombatAIController::ABullyCombatAIController()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	Rng.Initialize(1202);   // deterministic: the headless test replays identically
}

void ABullyCombatAIController::Note(const FString& S) const
{
	if (bLogAI)
	{
		UE_LOG(LogBullyCombatAI, Display, TEXT("BR_COMBAT ai %s %s"), *GetNameSafe(GetPawn()), *S);
	}
}

void ABullyCombatAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	bEngaged = false;
	Note(TEXT("possessed"));
}

void ABullyCombatAIController::AttackSoon(float Delay)
{
	NextAttackAt = GetWorld() ? GetWorld()->GetTimeSeconds() + Delay : 0.0;
}

void ABullyCombatAIController::Tick(float Dt)
{
	Super::Tick(Dt);
	ABullyCombatPawn* P = Cast<ABullyCombatPawn>(GetPawn());
	UBullyCombatComponent* C = P ? P->GetCombat() : nullptr;
	UWorld* W = GetWorld();
	if (!C || !W) { return; }
	const double Now = W->GetTimeSeconds();

	// hits taken in a row -> back off
	if (LastHealth >= 0.f && C->Health < LastHealth) { ++HitsInRow; }
	LastHealth = C->Health;
	if (RetreatAfterHits > 0 && HitsInRow >= RetreatAfterHits && C->GetState() == EBullyCombatState::Idle)
	{
		HitsInRow = 0;
		RetreatUntil = Now + RetreatTime;
		Note(TEXT("retreat"));
	}

	UBullyCombatComponent* T = C->FindTarget(5000.f, 360.f, true);
	if (C->IsOut() || C->GetState() == EBullyCombatState::Beaten || !T || T->IsOut())
	{
		C->SetBlockHeld(false);
		C->SetLockOnHeld(false);
		return;
	}
	const UMissionSubsystem* MS = UMissionSubsystem::Get(W);
	const bool bMayAttack = P->bAttackAllowed && !(MS && MS->HasFlag(TEXT("combat_ai_passive")));
	const bool bMayBlock = P->bBlockAllowed && !(MS && MS->HasFlag(TEXT("combat_ai_noblock")));

	const FVector Me = P->GetActorLocation();
	const FVector Them = T->GetOwner()->GetActorLocation();
	const float D = FVector::Dist2D(Me, Them);
	if (!bEngaged && D < RunDistance)
	{
		bEngaged = true;
		NextAttackAt = FMath::Max(NextAttackAt, Now + FirstAttackDelay);
		Note(FString::Printf(TEXT("engage at %.0f cm"), D));
	}

	// block: answer a fresh player swing (rising edge of a new move) with a block, sometimes
	const FName TM = T->GetCurrentMoveName();
	if (!TM.IsNone() && (TM != SeenTargetMove || T->GetStateTime() < SeenTargetMoveTime))
	{
		const FBullyCombatMove* M = T->GetCurrentMove();
		if (bMayBlock && M && M->ActiveStart >= 0.f && !M->bUnblockable && D < 260.f && C->GetState() == EBullyCombatState::Idle && Rng.FRand() < BlockChance)
		{
			BlockAt = Now + BlockReactTime;
			BlockUntil = BlockAt + Rng.FRandRange(BlockHoldMin, BlockHoldMax);
			Note(FString::Printf(TEXT("will block %s"), *TM.ToString()));
		}
	}
	SeenTargetMove = TM;
	SeenTargetMoveTime = T->GetStateTime();
	const bool bBlockNow = bMayBlock && Now >= BlockAt && Now < BlockUntil;
	C->SetBlockHeld(bBlockNow);

	if (C->GetState() != EBullyCombatState::Idle && C->GetState() != EBullyCombatState::Blocking)
	{
		return;   // swinging / reacting / held: the component owns the body
	}
	C->SetLockOnHeld(D < RunDistance);
	P->SetScriptedGait(D > RunDistance ? EBullyGait::Run : EBullyGait::Walk);

	// attack
	if (bMayAttack && !bBlockNow && Now >= NextAttackAt && D <= AttackRange && !T->IsDown() && Now >= RetreatUntil
		&& T->GetState() != EBullyCombatState::Grappled)
	{
		const FName Move = Rng.FRand() < HookChance ? HookMove : JabMove;
		if (C->StartMove(Move))
		{
			NextAttackAt = Now + Rng.FRandRange(AttackIntervalMin, AttackIntervalMax);
			HitsInRow = 0;
			Note(FString::Printf(TEXT("swing %s at %.0f cm (next in %.1fs)"), *Move.ToString(), D, NextAttackAt - Now));
			return;
		}
	}
	if (bBlockNow) { return; }

	// movement: tether, retreat, approach, circle
	FVector Dir = FVector::ZeroVector;
	if (P->bHasTether && FVector::Dist2D(Me, P->TetherCenter) > P->TetherRadius)
	{
		Dir = (P->TetherCenter - Me).GetSafeNormal2D();
	}
	else if (Now < RetreatUntil)
	{
		Dir = (Me - Them).GetSafeNormal2D();
	}
	else if (D > EngageDistance + 25.f)
	{
		Dir = (Them - Me).GetSafeNormal2D();
	}
	else if (D < EngageDistance - 45.f)
	{
		Dir = (Me - Them).GetSafeNormal2D();
	}
	else if (!T->IsDown())
	{
		if (Now >= NextCircleFlip)
		{
			CircleDir = Rng.FRand() < 0.5f ? -1.f : 1.f;
			NextCircleFlip = Now + Rng.FRandRange(CircleFlipMin, CircleFlipMax);
		}
		Dir = FVector::CrossProduct(FVector::UpVector, (Them - Me).GetSafeNormal2D()) * CircleDir * 0.6f;
	}
	if (!Dir.IsNearlyZero())
	{
		P->AddMovementInput(Dir, 1.f);
	}
}
