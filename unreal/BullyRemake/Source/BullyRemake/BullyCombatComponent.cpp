// OWNER: slice-combat
#include "BullyCombatComponent.h"

#include "Animation/AnimSequence.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MissionSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyCombat, Log, All);

namespace
{
	FBullyCombatMove MakeMove(const TCHAR* Name, const TCHAR* Clip, float ActiveStart, float ActiveEnd, float Damage, EBullyHitReaction R)
	{
		FBullyCombatMove M;
		M.Name = Name;
		M.Clip = Clip;
		M.ActiveStart = ActiveStart;
		M.ActiveEnd = ActiveEnd;
		M.Damage = Damage;
		M.Reaction = R;
		return M;
	}

	const TCHAR* ReactionName(EBullyHitReaction R)
	{
		switch (R)
		{
		case EBullyHitReaction::None: return TEXT("none");
		case EBullyHitReaction::Flinch: return TEXT("flinch");
		case EBullyHitReaction::Stagger: return TEXT("stagger");
		case EBullyHitReaction::Knockdown: return TEXT("knockdown");
		case EBullyHitReaction::GroundHit: return TEXT("ground_hit");
		case EBullyHitReaction::KO: return TEXT("ko");
		}
		return TEXT("?");
	}

	float CapsuleRadius(const AActor* A)
	{
		const ACharacter* C = Cast<ACharacter>(A);
		return C && C->GetCapsuleComponent() ? C->GetCapsuleComponent()->GetScaledCapsuleRadius() : 30.f;
	}
}

UBullyCombatComponent::UBullyCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// Authored defaults (Config/DefaultGame.ini overrides all of them). Hit windows sit on each clip's striking-limb
	// peak reach (FK over the imported clips, docs/re-notes/combat.md): SLOP_1 0.18 s, SLOP_2 0.32, SLOP_3 0.25,
	// GROUND_KICK 0.40, GRAP_HITA 0.40, HITB 0.13, HITC 0.30, GRAP_PUSH 0.40, PUNCH_S_4 0.37, HEAVY_PUNCH_2 0.55,
	// G_PUNCH_FINISH 1.07.
	{
		FBullyCombatMove M = MakeMove(TEXT("Jab1"), TEXT("PUNCH_SLOP_1"), 0.12f, 0.24f, 5.f, EBullyHitReaction::Flinch);
		M.Next = TEXT("Jab2"); M.BufferOpen = 0.f; M.ChainTime = 0.30f; M.Lunge = 45.f; M.GuardDamage = 15.f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("Jab2"), TEXT("PUNCH_SLOP_2"), 0.25f, 0.37f, 6.f, EBullyHitReaction::Flinch);
		M.Next = TEXT("Jab3"); M.BufferOpen = 0.f; M.ChainTime = 0.42f; M.Lunge = 45.f; M.GuardDamage = 15.f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("Jab3"), TEXT("PUNCH_SLOP_3"), 0.18f, 0.31f, 9.f, EBullyHitReaction::Stagger);
		M.Lunge = 60.f; M.Knockback = 110.f; M.HitStop = 0.10f; M.GuardDamage = 35.f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("GroundKick"), TEXT("GROUND_KICK"), 0.32f, 0.46f, 10.f, EBullyHitReaction::GroundHit);
		M.bGroundAttack = true; M.Range = 110.f; M.ArcDeg = 140.f; M.Lunge = 60.f; M.HitStop = 0.08f; M.bUnblockable = true;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("GrappleStrikeA"), TEXT("GRAP_HITA_GIV"), 0.36f, 0.44f, 4.f, EBullyHitReaction::None);
		M.VictimClip = TEXT("GRAP_HITA_RCV"); M.bUnblockable = true; M.Range = 200.f; M.ArcDeg = 360.f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("GrappleStrikeB"), TEXT("GRAP_HITB_GIV"), 0.10f, 0.18f, 4.f, EBullyHitReaction::None);
		M.VictimClip = TEXT("GRAP_HITB_RCV"); M.bUnblockable = true; M.Range = 200.f; M.ArcDeg = 360.f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("GrappleStrikeC"), TEXT("GRAP_HITC_GIV"), 0.27f, 0.35f, 6.f, EBullyHitReaction::Stagger);
		M.VictimClip = TEXT("GRAP_HITC_RCV"); M.bUnblockable = true; M.Range = 200.f; M.ArcDeg = 360.f; M.Knockback = 90.f; M.HitStop = 0.09f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("GrapplePush"), TEXT("GRAP_PUSH_ACT"), 0.32f, 0.42f, 3.f, EBullyHitReaction::Knockdown);
		M.VictimClip = TEXT("GRAP_PUSH_VIC"); M.bUnblockable = true; M.Range = 200.f; M.ArcDeg = 360.f; M.Knockback = 160.f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("Humiliate"), TEXT("G_PUNCH_FINISH_GV"), 1.03f, 1.12f, 0.f, EBullyHitReaction::KO);
		M.VictimClip = TEXT("G_PUNCH_FINISH_RCV"); M.bUnblockable = true; M.Range = 200.f; M.ArcDeg = 360.f; M.HitStop = 0.12f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("GrappleMiss"), TEXT("GRAP_MISS"), -1.f, -1.f, 0.f, EBullyHitReaction::None);
		Moves.Add(M);
	}
	// the bully: a readable wind-up (PUNCH_CHARGE) that auto-chains into the swing
	{
		FBullyCombatMove M = MakeMove(TEXT("BullyJabWindup"), TEXT("PUNCH_CHARGE"), -1.f, -1.f, 0.f, EBullyHitReaction::None);
		M.Duration = 0.35f; M.Next = TEXT("BullyJab"); M.bAutoChain = true;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("BullyJab"), TEXT("PUNCH_S_4"), 0.30f, 0.42f, 6.f, EBullyHitReaction::Flinch);
		M.Duration = 0.80f; M.Lunge = 50.f; M.GuardDamage = 25.f;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("BullyHookWindup"), TEXT("PUNCH_CHARGE"), -1.f, -1.f, 0.f, EBullyHitReaction::None);
		M.Duration = 0.5f; M.Next = TEXT("BullyHook"); M.bAutoChain = true;
		Moves.Add(M);
	}
	{
		FBullyCombatMove M = MakeMove(TEXT("BullyHook"), TEXT("HEAVY_PUNCH_2"), 0.48f, 0.62f, 10.f, EBullyHitReaction::Stagger);
		M.Lunge = 70.f; M.Knockback = 100.f; M.GuardDamage = 45.f; M.HitStop = 0.09f;
		Moves.Add(M);
	}
	GrappleStrikeMoves = { TEXT("GrappleStrikeA"), TEXT("GrappleStrikeB"), TEXT("GrappleStrikeC") };
	FlinchClips = { TEXT("HIT_HEAD_JAB"), TEXT("HIT_HEAD_CROSS"), TEXT("HIT_GUT_RIB_1") };
	StaggerClips = { TEXT("HIT_HEAVY") };
}

TArray<TWeakObjectPtr<UBullyCombatComponent>>& UBullyCombatComponent::Registry()
{
	static TArray<TWeakObjectPtr<UBullyCombatComponent>> R;
	return R;
}

void UBullyCombatComponent::BeginPlay()
{
	Super::BeginPlay();
	Registry().RemoveAll([](const TWeakObjectPtr<UBullyCombatComponent>& W) { return !W.IsValid(); });
	Registry().AddUnique(this);
	MaxHealth = IsPlayer() ? PlayerMaxHealth : EnemyMaxHealth;
	Health = MaxHealth;
	Guard = GuardMax;
	if (ACharacter* C = GetCharacter())
	{
		// the movement component must see this tick's lunge / knockback velocity
		C->GetCharacterMovement()->AddTickPrerequisiteComponent(this);
		bSavedOrient = C->GetCharacterMovement()->bOrientRotationToMovement;
	}
	int32 Missing = 0;
	for (const FBullyCombatMove& M : Moves)
	{
		Missing += ResolveClip(M.Clip) ? 0 : 1;
	}
	Note(FString::Printf(TEXT("ready team=%s hp=%.0f moves=%d (clips missing %d) root=%s"),
		IsPlayer() ? TEXT("player") : TEXT("enemy"), Health, Moves.Num(), Missing, *ClipRoot));
}

void UBullyCombatComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Partner.IsValid())
	{
		Partner->Partner.Reset();
		if (Partner->State == EBullyCombatState::Grappled || Partner->State == EBullyCombatState::Grappling) { Partner->SetState(EBullyCombatState::Idle); }
	}
	Registry().Remove(this);
	Super::EndPlay(Reason);
}

ACharacter* UBullyCombatComponent::GetCharacter() const
{
	return Cast<ACharacter>(GetOwner());
}

const TCHAR* UBullyCombatComponent::StateName(EBullyCombatState S)
{
	switch (S)
	{
	case EBullyCombatState::Idle: return TEXT("idle");
	case EBullyCombatState::Attacking: return TEXT("attacking");
	case EBullyCombatState::Blocking: return TEXT("blocking");
	case EBullyCombatState::BlockStun: return TEXT("blockstun");
	case EBullyCombatState::HitStun: return TEXT("hitstun");
	case EBullyCombatState::Downed: return TEXT("downed");
	case EBullyCombatState::GettingUp: return TEXT("gettingup");
	case EBullyCombatState::Grappling: return TEXT("grappling");
	case EBullyCombatState::Grappled: return TEXT("grappled");
	case EBullyCombatState::Beaten: return TEXT("beaten");
	case EBullyCombatState::KO: return TEXT("ko");
	}
	return TEXT("?");
}

FString UBullyCombatComponent::Describe() const
{
	return FString::Printf(TEXT("%s state=%s%s hp=%.0f/%.0f guard=%.0f"), *GetNameSafe(GetOwner()), StateName(State),
		State == EBullyCombatState::Attacking ? *FString::Printf(TEXT("(%s)"), *CurrentMove.ToString()) : TEXT(""), Health, MaxHealth, Guard);
}

void UBullyCombatComponent::Note(const FString& S) const
{
	if (bLogCombat)
	{
		UE_LOG(LogBullyCombat, Display, TEXT("BR_COMBAT %s %s"), IsPlayer() ? TEXT("jimmy") : *GetNameSafe(GetOwner()), *S);
	}
}

const FBullyCombatMove* UBullyCombatComponent::FindMove(FName Name) const
{
	return Moves.FindByPredicate([Name](const FBullyCombatMove& M) { return M.Name == Name; });
}

UAnimSequence* UBullyCombatComponent::ResolveClip(FName Clip)
{
	if (Clip.IsNone()) { return nullptr; }
	if (TObjectPtr<UAnimSequence>* Found = ClipCache.Find(Clip)) { return Found->Get(); }
	if (MissingClips.Contains(Clip)) { return nullptr; }
	const FString N = Clip.ToString();
	UAnimSequence* S = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), *ClipRoot, *N, *N), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (S) { ClipCache.Add(Clip, S); }
	else
	{
		MissingClips.Add(Clip);
		UE_LOG(LogBullyCombat, Warning, TEXT("BR_COMBAT clip missing: %s/%s"), *ClipRoot, *N);
	}
	return S;
}

float UBullyCombatComponent::ClipLength(FName Clip)
{
	const UAnimSequence* S = ResolveClip(Clip);
	return S ? S->GetPlayLength() : 0.6f;
}

// ---------------------------------------------------------------------------------------------------------------
// state + pose

void UBullyCombatComponent::SetState(EBullyCombatState S)
{
	if (S != State)
	{
		Note(FString::Printf(TEXT("state %s -> %s hp=%.0f"), StateName(State), StateName(S), Health));
	}
	if (State == EBullyCombatState::Attacking && S != EBullyCombatState::Attacking)
	{
		bBufferedAttack = false;
	}
	State = S;
	StateTime = 0.f;
	StateDuration = 0.f;
	if (S == EBullyCombatState::Idle)
	{
		bPoseActive = false;   // locomotion takes over (the anim instance blends out)
	}
	UpdateOrientation();
}

void UBullyCombatComponent::PlayClip(FName Clip, bool bLoop, float Rate, float Start, FName FollowLoop)
{
	Pose.Clip = Clip;
	Pose.Seq = ResolveClip(Clip);
	Pose.Time = Start;
	Pose.bLoop = bLoop;
	++Pose.Serial;
	PoseRate = Rate;
	FollowLoopClip = FollowLoop;
	bPoseActive = true;
}

bool UBullyCombatComponent::GetPose(FBullyCombatPose& Out) const
{
	if (!bPoseActive || !Pose.Seq) { return false; }
	Out = Pose;
	return true;
}

bool UBullyCombatComponent::CanMove() const
{
	return State == EBullyCombatState::Idle || (State == EBullyCombatState::Blocking && BlockMoveScale > 0.f);
}

float UBullyCombatComponent::GetMoveSpeedScale() const
{
	if (State == EBullyCombatState::Blocking) { return BlockMoveScale; }
	return IsLockedOn() ? LockOnMoveScale : 1.f;
}

bool UBullyCombatComponent::IsInActiveWindow() const
{
	const FBullyCombatMove* M = GetCurrentMove();
	return M && M->ActiveStart >= 0.f && StateTime >= M->ActiveStart && StateTime <= M->ActiveEnd;
}

void UBullyCombatComponent::UpdateOrientation()
{
	ACharacter* C = GetCharacter();
	if (!C) { return; }
	// orient-to-movement only while free and not locked on: knockback and lunges must not turn the body
	const bool bFree = State == EBullyCombatState::Idle && !IsLockedOn();
	UCharacterMovementComponent* Move = C->GetCharacterMovement();
	if (bFree && bOrientOverridden)
	{
		Move->bOrientRotationToMovement = bSavedOrient;
		bOrientOverridden = false;
	}
	else if (!bFree && !bOrientOverridden)
	{
		bSavedOrient = Move->bOrientRotationToMovement;
		Move->bOrientRotationToMovement = false;
		bOrientOverridden = true;
	}
}

void UBullyCombatComponent::FaceToward(const FVector& Where, float MaxStepDeg)
{
	AActor* O = GetOwner();
	const FVector D = Where - O->GetActorLocation();
	if (D.SizeSquared2D() < 1.f) { return; }
	const float Want = D.Rotation().Yaw;
	FRotator R = O->GetActorRotation();
	const float Delta = FMath::FindDeltaAngleDegrees(R.Yaw, Want);
	R.Yaw += FMath::Clamp(Delta, -MaxStepDeg, MaxStepDeg);
	R.Pitch = 0.f; R.Roll = 0.f;
	O->SetActorRotation(R);
}

void UBullyCombatComponent::Push(const FVector& Dir, float Distance, float Time)
{
	if (Distance <= 0.f || Time <= 0.f) { return; }
	PushVelocity = FVector(Dir.X, Dir.Y, 0.f).GetSafeNormal() * (Distance / Time);
	PushTimeLeft = Time;
}

void UBullyCombatComponent::StartHitStop(float Seconds)
{
	if (Seconds <= 0.f || !GetWorld()) { return; }
	GetOwner()->CustomTimeDilation = HitStopDilation;
	HitStopUntil = FMath::Max(HitStopUntil, GetWorld()->GetTimeSeconds() + Seconds);
}

FName UBullyCombatComponent::PickClip(const TArray<FName>& Clips, FName Fallback)
{
	if (Clips.Num() == 0) { return Fallback; }
	return Clips[(FlinchPick++) % Clips.Num()];
}

void UBullyCombatComponent::Fire(FName Event)
{
	Note(FString::Printf(TEXT("event %s"), *Event.ToString()));
	OnCombatEvent.Broadcast(this, Event);
}

void UBullyCombatComponent::FireMission(FName Event)
{
	Fire(Event);
	if (UMissionSubsystem* MS = UMissionSubsystem::Get(GetWorld()))
	{
		MS->FireEvent(Event);
		UE_LOG(LogBullyCombat, Display, TEXT("BR_COMBAT event fired: %s"), *Event.ToString());
	}
	else
	{
		UE_LOG(LogBullyCombat, Display, TEXT("BR_COMBAT event fired: %s (no mission subsystem in this world)"), *Event.ToString());
	}
}

// ---------------------------------------------------------------------------------------------------------------
// targeting

UBullyCombatComponent* UBullyCombatComponent::FindTarget(float Range, float ConeDeg, bool bIncludeDown) const
{
	const AActor* O = GetOwner();
	if (!O) { return nullptr; }
	const FVector Me = O->GetActorLocation();
	const FVector Fwd = O->GetActorForwardVector().GetSafeNormal2D();
	const float CosHalf = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(ConeDeg, 0.f, 360.f) * 0.5f));
	UBullyCombatComponent* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	for (const TWeakObjectPtr<UBullyCombatComponent>& W : Registry())
	{
		UBullyCombatComponent* Other = W.Get();
		if (!Other || Other == this || Other->GetWorld() != GetWorld() || Other->Team == Team || Other->IsOut() || !Other->GetOwner()) { continue; }
		if (!bIncludeDown && Other->IsDown()) { continue; }
		const FVector D = Other->GetOwner()->GetActorLocation() - Me;
		const float Dist = D.Size2D();
		if (Dist > Range || FMath::Abs(D.Z) > 200.f) { continue; }
		const float Dot = Dist > 1.f ? FVector::DotProduct(Fwd, D.GetSafeNormal2D()) : 1.f;
		if (ConeDeg < 360.f && Dot < CosHalf) { continue; }
		const float Score = Dist * (1.f + 0.5f * (1.f - Dot));
		if (Score < BestScore) { BestScore = Score; Best = Other; }
	}
	return Best;
}

// ---------------------------------------------------------------------------------------------------------------
// commands

bool UBullyCombatComponent::RequestAttack()
{
	switch (State)
	{
	case EBullyCombatState::Grappling:
		if (GrappleStrikeMoves.IsValidIndex(GrappleStrikeIndex))
		{
			if (const FBullyCombatMove* M = FindMove(GrappleStrikeMoves[GrappleStrikeIndex]))
			{
				BeginMove(*M, EMoveCtx::GrappleStrike);
				return true;
			}
		}
		return false;
	case EBullyCombatState::Attacking:
		if (const FBullyCombatMove* M = GetCurrentMove())
		{
			if (MoveCtx == EMoveCtx::Normal && !M->Next.IsNone() && !M->bAutoChain && StateTime >= M->BufferOpen)
			{
				bBufferedAttack = true;
				return true;
			}
		}
		return false;
	case EBullyCombatState::Idle:
	case EBullyCombatState::Blocking:
		break;
	default:
		return false;
	}
	// a downed enemy close by turns the attack into a ground kick
	if (UBullyCombatComponent* Down = FindTarget(GroundKickRange, 200.f, true))
	{
		if (Down->State == EBullyCombatState::Downed)
		{
			if (const FBullyCombatMove* K = FindMove(GroundKickMove))
			{
				MoveTarget = Down;
				BeginMove(*K);
				return true;
			}
		}
	}
	if (const FBullyCombatMove* M = FindMove(LightChainStart))
	{
		BeginMove(*M);
		bInLightChain = true;
		ChainCount = 0;
		return true;
	}
	return false;
}

bool UBullyCombatComponent::StartMove(FName MoveName)
{
	if (State != EBullyCombatState::Idle && State != EBullyCombatState::Blocking) { return false; }
	const FBullyCombatMove* M = FindMove(MoveName);
	if (!M) { Note(FString::Printf(TEXT("unknown move %s"), *MoveName.ToString())); return false; }
	BeginMove(*M);
	return true;
}

bool UBullyCombatComponent::RequestGrapple()
{
	if (State == EBullyCombatState::Grappling)
	{
		if (const FBullyCombatMove* M = FindMove(GrapplePushMove))
		{
			BeginMove(*M, EMoveCtx::GrapplePush);
			return true;
		}
		return false;
	}
	if (State != EBullyCombatState::Idle && State != EBullyCombatState::Blocking) { return false; }
	UBullyCombatComponent* T = FindTarget(GrappleRange, GrappleConeDeg, false);
	if (T && T->State == EBullyCombatState::Beaten)
	{
		if (const FBullyCombatMove* H = FindMove(HumiliateMove))
		{
			StartGrapple(T);
			BeginMove(*H, EMoveCtx::Humiliate);
			return true;
		}
	}
	const bool bGrabbable = T && (T->State == EBullyCombatState::Idle || T->State == EBullyCombatState::Attacking || T->State == EBullyCombatState::Blocking
		|| T->State == EBullyCombatState::BlockStun || T->State == EBullyCombatState::HitStun);
	if (bGrabbable)
	{
		StartGrapple(T);
		return true;
	}
	if (const FBullyCombatMove* Miss = FindMove(GrappleMissMove))
	{
		Note(TEXT("grapple whiff (no target in reach)"));
		BeginMove(*Miss);
		return true;
	}
	return false;
}

void UBullyCombatComponent::SetBlockHeld(bool bHeld)
{
	if (bHeld == bBlockHeld) { return; }
	bBlockHeld = bHeld;
	if (bHeld && State == EBullyCombatState::Idle)
	{
		SetState(EBullyCombatState::Blocking);
		BlockRaisedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		PlayClip(BlockInClip, false, 1.f, 0.f, BlockLoopClip);
	}
	else if (!bHeld && State == EBullyCombatState::Blocking)
	{
		SetState(EBullyCombatState::Idle);
	}
}

void UBullyCombatComponent::SetLockOnHeld(bool bHeld)
{
	if (bHeld == bLockOnHeld) { return; }
	bLockOnHeld = bHeld;
	if (bHeld)
	{
		LockTarget = FindTarget(LockOnRange, 360.f, false);
		Note(FString::Printf(TEXT("lock-on %s"), LockTarget.IsValid() ? *GetNameSafe(LockTarget->GetOwner()) : TEXT("(no target)")));
	}
	else
	{
		LockTarget.Reset();
	}
	UpdateOrientation();
}

void UBullyCombatComponent::ResetFighter()
{
	if (Partner.IsValid())
	{
		Partner->Partner.Reset();
		Partner.Reset();
	}
	Health = MaxHealth;
	Guard = GuardMax;
	PushTimeLeft = 0.f;
	GetOwner()->CustomTimeDilation = 1.f;
	HitStopUntil = -1.0;
	SetState(EBullyCombatState::Idle);
	Note(TEXT("reset"));
}

// ---------------------------------------------------------------------------------------------------------------
// moves

void UBullyCombatComponent::BeginMove(const FBullyCombatMove& M, EMoveCtx Ctx)
{
	const bool bChained = State == EBullyCombatState::Attacking && bInLightChain && Ctx == EMoveCtx::Normal;
	SetState(EBullyCombatState::Attacking);
	CurrentMove = M.Name;
	MoveCtx = Ctx;
	bInLightChain = bChained;
	bPairHitDone = false;
	HitThisSwing.Reset();
	bBufferedAttack = false;
	const float Len = ClipLength(M.Clip);
	StateDuration = M.Duration > 0.f ? M.Duration : FMath::Max(0.1f, (Len - M.ClipStart) / FMath::Max(0.05f, M.PlayRate));
	PlayClip(M.Clip, false, M.PlayRate, M.ClipStart);

	// target assist: turn toward the lock target / nearest enemy in front and step in
	if (Ctx != EMoveCtx::Normal)
	{
		MoveTarget = Partner;
		// pair clip: the held partner starts its receive half in sync with our give half
		if (Partner.IsValid() && !M.VictimClip.IsNone())
		{
			Partner->PlayClip(M.VictimClip, false, M.PlayRate, M.ClipStart, HoldReceiverClip);
		}
	}
	else if (!(MoveTarget.IsValid() && M.bGroundAttack))
	{
		UBullyCombatComponent* T = IsLockedOn() ? LockTarget.Get() : nullptr;
		if (!T) { T = FindTarget(AssistRange, AssistConeDeg, M.bGroundAttack); }
		MoveTarget = T;
	}
	if (MoveTarget.IsValid() && Ctx == EMoveCtx::Normal)
	{
		const FVector To = MoveTarget->GetOwner()->GetActorLocation();
		FaceToward(To, 180.f);
		if (M.Lunge > 0.f)
		{
			const float Gap = FVector::Dist2D(To, GetOwner()->GetActorLocation()) - LungeStopDistance;
			Push(To - GetOwner()->GetActorLocation(), FMath::Clamp(Gap, 0.f, M.Lunge), M.LungeTime);
		}
	}
	Note(FString::Printf(TEXT("attack %s start (clip %s %.2fs, active %.2f-%.2f)%s"), *M.Name.ToString(), *M.Clip.ToString(), StateDuration,
		M.ActiveStart, M.ActiveEnd, MoveTarget.IsValid() ? *FString::Printf(TEXT(" -> %s at %.0f cm"), *GetNameSafe(MoveTarget->GetOwner()),
		FVector::Dist2D(MoveTarget->GetOwner()->GetActorLocation(), GetOwner()->GetActorLocation())) : TEXT("")));
}

void UBullyCombatComponent::EndMove()
{
	const FBullyCombatMove* M = FindMove(CurrentMove);
	if (M && M->ActiveStart >= 0.f && HitThisSwing.Num() == 0)
	{
		Note(FString::Printf(TEXT("attack %s whiff"), *CurrentMove.ToString()));
	}
	const EMoveCtx Ctx = MoveCtx;
	MoveCtx = EMoveCtx::Normal;
	bInLightChain = false;
	if (Ctx == EMoveCtx::GrappleStrike)
	{
		++GrappleStrikeIndex;
		if (GrappleStrikeIndex >= GrappleStrikeMoves.Num())
		{
			FireMission(TEXT("grapple_strikes_done"));
		}
	}
	if (Ctx != EMoveCtx::Normal && Partner.IsValid() && Partner->State == EBullyCombatState::Grappled)
	{
		// back to holding
		SetState(EBullyCombatState::Grappling);
		GrappleIdleTime = 0.f;
		PlayClip(HoldGiverClip, true);
		return;
	}
	if (Partner.IsValid() && Ctx != EMoveCtx::Normal)
	{
		Partner.Reset();
	}
	SetState(bBlockHeld ? EBullyCombatState::Blocking : EBullyCombatState::Idle);
	if (State == EBullyCombatState::Blocking)
	{
		PlayClip(BlockLoopClip, true);
	}
}

void UBullyCombatComponent::TickMove(float Dt)
{
	const FBullyCombatMove* M = FindMove(CurrentMove);
	if (!M) { EndMove(); return; }
	if (MoveCtx != EMoveCtx::Normal)
	{
		if (Partner.IsValid() && Partner->State == EBullyCombatState::Grappled) { HoldPartner(); }
	}
	else if (MoveTarget.IsValid() && (M->ActiveStart < 0.f || StateTime < M->ActiveStart))
	{
		FaceToward(MoveTarget->GetOwner()->GetActorLocation(), TrackTurnRate * Dt);
	}
	if (M->ActiveStart >= 0.f && StateTime >= M->ActiveStart && StateTime <= M->ActiveEnd + KINDA_SMALL_NUMBER)
	{
		TickHits(*M);
	}
	if (bBufferedAttack && !M->Next.IsNone() && StateTime >= M->ChainTime)
	{
		if (const FBullyCombatMove* N = FindMove(M->Next))
		{
			BeginMove(*N);
			return;
		}
	}
	if (StateTime >= StateDuration)
	{
		if (M->bAutoChain && !M->Next.IsNone())
		{
			if (const FBullyCombatMove* N = FindMove(M->Next))
			{
				BeginMove(*N);
				return;
			}
		}
		EndMove();
	}
}

void UBullyCombatComponent::TickHits(const FBullyCombatMove& M)
{
	const float Dmg = M.Damage * DamageScale();
	if (MoveCtx != EMoveCtx::Normal)
	{
		// pair moves always connect with the held partner, once
		if (!bPairHitDone && Partner.IsValid())
		{
			bPairHitDone = true;
			UBullyCombatComponent* V = Partner.Get();
			HitThisSwing.Add(V);
			ApplyHitTo(V, M, Dmg);
		}
		return;
	}
	AActor* O = GetOwner();
	const FVector Me = O->GetActorLocation();
	const FVector Fwd = O->GetActorForwardVector().GetSafeNormal2D();
	const float CosHalf = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(M.ArcDeg, 0.f, 360.f) * 0.5f));
	const float MyR = CapsuleRadius(O);
	for (const TWeakObjectPtr<UBullyCombatComponent>& W : Registry())
	{
		UBullyCombatComponent* V = W.Get();
		if (!V || V == this || V->GetWorld() != GetWorld() || V->Team == Team || V->IsOut() || !V->GetOwner() || HitThisSwing.Contains(V)) { continue; }
		if (M.bGroundAttack != V->IsDown()) { continue; }   // standing swings pass over downed fighters; ground kicks only hit them
		const FVector D = V->GetOwner()->GetActorLocation() - Me;
		const float Gap = D.Size2D() - MyR - CapsuleRadius(V->GetOwner());
		if (Gap > M.Range || FMath::Abs(D.Z) > 150.f) { continue; }
		if (M.ArcDeg < 360.f && D.Size2D() > 1.f && FVector::DotProduct(Fwd, D.GetSafeNormal2D()) < CosHalf) { continue; }
		HitThisSwing.Add(V);
		ApplyHitTo(V, M, Dmg);
	}
}

void UBullyCombatComponent::ApplyHitTo(UBullyCombatComponent* V, const FBullyCombatMove& M, float Dmg)
{
	const EHitResult R = V->ReceiveHit(this, M, Dmg);
	if (R == EHitResult::Ignored)
	{
		Note(FString::Printf(TEXT("attack %s ignored by %s (%s)"), *M.Name.ToString(), *GetNameSafe(V->GetOwner()), StateName(V->State)));
		return;
	}
	StartHitStop(M.HitStop);
	V->StartHitStop(M.HitStop);
	if (R == EHitResult::Blocked)
	{
		Note(FString::Printf(TEXT("attack %s blocked by %s guard=%.0f hp=%.0f"), *M.Name.ToString(), *GetNameSafe(V->GetOwner()), V->Guard, V->Health));
		return;
	}
	++HitsLanded;
	Note(FString::Printf(TEXT("attack %s hit %s dmg=%.0f hp=%.0f react=%s state=%s"), *M.Name.ToString(), *GetNameSafe(V->GetOwner()),
		V->bInvulnerable ? 0.f : Dmg, V->Health, ReactionName(M.Reaction), StateName(V->State)));
	if (!IsPlayer())
	{
		return;
	}
	// tutorial events (the player's side)
	if (bInLightChain || M.Name == LightChainStart)
	{
		++ChainCount;
		if (M.Next.IsNone() && ChainCount >= 3)
		{
			FireMission(TEXT("combo_done"));
		}
	}
	switch (MoveCtx)
	{
	case EMoveCtx::GrappleStrike: FireMission(TEXT("grapple_strike")); break;
	case EMoveCtx::GrapplePush: FireMission(TEXT("grapple_done")); break;
	case EMoveCtx::Humiliate: FireMission(TEXT("humiliation_done")); break;
	default: break;
	}
	if (M.bGroundAttack)
	{
		FireMission(TEXT("ground_kick"));
	}
}

UBullyCombatComponent::EHitResult UBullyCombatComponent::ReceiveHit(UBullyCombatComponent* Attacker, const FBullyCombatMove& M, float Damage)
{
	if (State == EBullyCombatState::KO) { return EHitResult::Ignored; }
	if (State == EBullyCombatState::GettingUp) { return EHitResult::Ignored; }   // get-up i-frames
	const double Now = GetWorld()->GetTimeSeconds();
	AActor* O = GetOwner();

	// block: facing the attacker inside the block arc
	if ((State == EBullyCombatState::Blocking || State == EBullyCombatState::BlockStun) && !M.bUnblockable && Attacker)
	{
		const FVector ToAttacker = (Attacker->GetOwner()->GetActorLocation() - O->GetActorLocation()).GetSafeNormal2D();
		const float CosHalf = FMath::Cos(FMath::DegreesToRadians(BlockArcDeg * 0.5f));
		if (FVector::DotProduct(O->GetActorForwardVector().GetSafeNormal2D(), ToAttacker) >= CosHalf)
		{
			const bool bPerfect = PerfectBlockWindow > 0.f && Now - BlockRaisedAt <= PerfectBlockWindow;
			Guard = FMath::Max(0.f, Guard - (bPerfect ? 0.f : M.GuardDamage));
			if (!bInvulnerable) { Health = FMath::Max(MinHealth > 0.f ? MinHealth : 0.f, Health - Damage * BlockDamageScale); }
			FaceToward(Attacker->GetOwner()->GetActorLocation(), 180.f);
			if (Guard <= 0.f)
			{
				SetState(EBullyCombatState::HitStun);
				StateDuration = GuardBreakStun;
				PlayClip(GuardBreakClip, false);
				Fire(TEXT("guard_break"));
			}
			else
			{
				SetState(EBullyCombatState::BlockStun);
				StateDuration = BlockStunTime;
				PlayClip(BlockHitClip, false, 1.f, 0.f, BlockLoopClip);
				Note(FString::Printf(TEXT("blocked %s%s guard=%.0f"), *M.Name.ToString(), bPerfect ? TEXT(" (perfect)") : TEXT(""), Guard));
				if (IsPlayer()) { FireMission(TEXT("block_success")); }
			}
			return EHitResult::Blocked;
		}
	}

	// damage + the health floor (tutorial partner: MinHealth 5, then Beaten until humiliated)
	LastHitTakenAt = Now;
	if (!bInvulnerable || M.Reaction == EBullyHitReaction::KO)
	{
		Health -= Damage;
	}
	if (M.Reaction == EBullyHitReaction::KO)
	{
		Health = 0.f;
	}
	else if (MinHealth > 0.f && Health <= MinHealth)
	{
		Health = MinHealth;
		if (Partner.IsValid() && Partner.Get() != Attacker)
		{
			Partner->Partner.Reset();
			Partner.Reset();
		}
		if (State != EBullyCombatState::Beaten)
		{
			if (Partner.IsValid()) { Partner->Partner.Reset(); Partner.Reset(); }
			SetState(EBullyCombatState::Beaten);
			PlayClip(PickClip(StaggerClips, KOClip), false, 1.f, 0.f, BeatenLoopClip);
			Fire(TEXT("beaten"));
			if (!IsPlayer()) { FireMission(TEXT("enemy_beaten")); }
		}
		else
		{
			PlayClip(PickClip(FlinchClips, BeatenLoopClip), false, 1.f, 0.f, BeatenLoopClip);
		}
		return EHitResult::Hit;
	}
	if (Health <= 0.f)
	{
		Health = 0.f;
		if (!M.VictimClip.IsNone() && M.Reaction == EBullyHitReaction::KO)
		{
			// humiliation: the pair clip plays out, then the KO loop
			if (Partner.IsValid()) { Partner->Partner.Reset(); Partner.Reset(); }
			SetState(EBullyCombatState::KO);
			if (Pose.Clip == M.VictimClip && bPoseActive) { FollowLoopClip = KOLoopClip; }
			else { PlayClip(M.VictimClip, false, 1.f, 0.f, KOLoopClip); }
			Fire(TEXT("knocked_out"));
			FireMission(IsPlayer() ? FName(TEXT("player_knocked_out")) : FName(TEXT("enemy_knocked_out")));
			return EHitResult::Hit;
		}
		EnterReaction(EBullyHitReaction::KO, Attacker, &M);
		return EHitResult::Hit;
	}
	EnterReaction(IsDown() ? EBullyHitReaction::GroundHit : M.Reaction, Attacker, &M);
	return EHitResult::Hit;
}

void UBullyCombatComponent::EnterReaction(EBullyHitReaction R, UBullyCombatComponent* Attacker, const FBullyCombatMove* M)
{
	const double Now = GetWorld()->GetTimeSeconds();
	const FVector Away = Attacker ? (GetOwner()->GetActorLocation() - Attacker->GetOwner()->GetActorLocation()).GetSafeNormal2D() : -GetOwner()->GetActorForwardVector();
	const float Knock = M ? M->Knockback : 0.f;

	// held by the attacker and the strike keeps the hold: play the pair's receive clip and stay held
	if (State == EBullyCombatState::Grappled && Partner.IsValid() && Partner.Get() == Attacker && R == EBullyHitReaction::None)
	{
		if (M && !M->VictimClip.IsNone() && Pose.Clip != M->VictimClip) { PlayClip(M->VictimClip, false, 1.f, 0.f, Attacker->HoldReceiverClip); }
		return;
	}
	// anything else breaks a grapple (either side)
	if (Partner.IsValid())
	{
		UBullyCombatComponent* P = Partner.Get();
		P->Partner.Reset();
		if (P->State == EBullyCombatState::Grappling || P->State == EBullyCombatState::Grappled) { P->SetState(EBullyCombatState::Idle); }
		Partner.Reset();
	}
	if (Attacker && R != EBullyHitReaction::GroundHit && R != EBullyHitReaction::KO)
	{
		FaceToward(Attacker->GetOwner()->GetActorLocation(), 180.f);
	}
	if (R == EBullyHitReaction::Flinch)
	{
		RecentFlinches = (Now - LastHitTakenAt < ComboResetTime && State == EBullyCombatState::HitStun) ? RecentFlinches + 1 : 1;
		if (FlinchesBeforeStagger > 0 && RecentFlinches >= FlinchesBeforeStagger)
		{
			R = EBullyHitReaction::Stagger;
			RecentFlinches = 0;
		}
	}
	FName Victim = M ? M->VictimClip : NAME_None;
	if (!Victim.IsNone() && Pose.Clip == Victim && bPoseActive)
	{
		Victim = NAME_None;   // the pair's receive half is already playing in sync: keep it, only the follow-up changes
	}
	const bool bKeepPair = M && !M->VictimClip.IsNone() && Victim.IsNone();
	auto React = [this, bKeepPair](FName Clip, FName FollowLoop)
	{
		if (bKeepPair) { FollowLoopClip = FollowLoop; }
		else { PlayClip(Clip, false, 1.f, 0.f, FollowLoop); }
	};
	switch (R)
	{
	case EBullyHitReaction::None:
	case EBullyHitReaction::Flinch:
		SetState(EBullyCombatState::HitStun);
		StateDuration = FlinchTime;
		React(Victim.IsNone() ? PickClip(FlinchClips, TEXT("HIT_HEAD_JAB")) : Victim, NAME_None);
		Push(Away, FMath::Max(Knock, 25.f), 0.15f);
		break;
	case EBullyHitReaction::Stagger:
		SetState(EBullyCombatState::HitStun);
		StateDuration = StaggerTime;
		React(Victim.IsNone() ? PickClip(StaggerClips, TEXT("HIT_HEAVY")) : Victim, NAME_None);
		Push(Away, FMath::Max(Knock, 80.f), 0.25f);
		break;
	case EBullyHitReaction::Knockdown:
		SetState(EBullyCombatState::Downed);
		StateDuration = DownedTime;
		React(Victim.IsNone() ? KnockdownClip : Victim, DownedLoopClip);
		Push(Away, FMath::Max(Knock, 100.f), 0.3f);
		Fire(TEXT("knocked_down"));
		break;
	case EBullyHitReaction::GroundHit:
	{
		const float Left = (State == EBullyCombatState::Downed) ? FMath::Max(0.f, StateDuration - StateTime) : 0.f;
		SetState(EBullyCombatState::Downed);
		StateDuration = FMath::Max(Left, DownedHitExtend);
		PlayClip(GroundHitClip, false, 1.f, 0.f, DownedLoopClip);
		break;
	}
	case EBullyHitReaction::KO:
		SetState(EBullyCombatState::KO);
		React(Victim.IsNone() ? KOClip : Victim, KOLoopClip);
		Push(Away, FMath::Max(Knock, 60.f), 0.3f);
		Fire(TEXT("knocked_out"));
		FireMission(IsPlayer() ? FName(TEXT("player_knocked_out")) : FName(TEXT("enemy_knocked_out")));
		break;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// grapple

void UBullyCombatComponent::StartGrapple(UBullyCombatComponent* V)
{
	if (!V) { return; }
	if (V->Partner.IsValid())
	{
		V->Partner->Partner.Reset();
		if (V->Partner->State == EBullyCombatState::Grappling) { V->Partner->SetState(EBullyCombatState::Idle); }
	}
	Partner = V;
	V->Partner = this;
	V->bBufferedAttack = false;
	V->MoveCtx = EMoveCtx::Normal;
	SetState(EBullyCombatState::Grappling);
	V->SetState(EBullyCombatState::Grappled);
	GrappleStrikeIndex = 0;
	GrappleIdleTime = 0.f;
	FaceToward(V->GetOwner()->GetActorLocation(), 180.f);
	PlayClip(GrabGiverClip, false, 1.f, 0.f, HoldGiverClip);
	V->PlayClip(GrabReceiverClip, false, 1.f, 0.f, HoldReceiverClip);
	HoldPartner();
	Note(FString::Printf(TEXT("grapple %s"), *GetNameSafe(V->GetOwner())));
	if (IsPlayer()) { FireMission(TEXT("grapple_start")); }
	else { Fire(TEXT("grapple_start")); }
}

void UBullyCombatComponent::HoldPartner()
{
	UBullyCombatComponent* V = Partner.Get();
	ACharacter* VC = V ? V->GetCharacter() : nullptr;
	if (!VC) { return; }
	const AActor* O = GetOwner();
	const FVector Fwd = O->GetActorForwardVector().GetSafeNormal2D();
	FVector Want = O->GetActorLocation() + Fwd * GrappleHoldDistance;
	Want.Z = VC->GetActorLocation().Z;
	VC->SetActorLocationAndRotation(Want, FRotator(0.f, (-Fwd).Rotation().Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	VC->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	V->PushTimeLeft = 0.f;
}

void UBullyCombatComponent::ReleaseGrapple(bool bEscape)
{
	UBullyCombatComponent* V = Partner.Get();
	Partner.Reset();
	if (V)
	{
		V->Partner.Reset();
		if (bEscape)
		{
			V->SetState(EBullyCombatState::HitStun);
			V->StateDuration = V->ClipLength(EscapeClip);
			V->PlayClip(EscapeClip, false);
			V->Push(V->GetOwner()->GetActorLocation() - GetOwner()->GetActorLocation(), 60.f, 0.25f);
			V->Fire(TEXT("grapple_escape"));
		}
		else if (V->State == EBullyCombatState::Grappled)
		{
			V->SetState(EBullyCombatState::Idle);
		}
	}
	if (State == EBullyCombatState::Grappling)
	{
		SetState(EBullyCombatState::Idle);
	}
	Note(bEscape ? TEXT("grapple broken (escape)") : TEXT("grapple released"));
}

// ---------------------------------------------------------------------------------------------------------------
// tick

void UBullyCombatComponent::TickComponent(float Dt, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(Dt, TickType, ThisTickFunction);
	UWorld* W = GetWorld();
	ACharacter* C = GetCharacter();
	if (!W || !C) { return; }

	// hitstop ends in world time (the actor's own clock is the one that is slowed)
	if (HitStopUntil > 0.0 && W->GetTimeSeconds() >= HitStopUntil)
	{
		C->CustomTimeDilation = 1.f;
		HitStopUntil = -1.0;
	}
	StateTime += Dt;

	// pose clock + follow-up loop
	if (bPoseActive)
	{
		Pose.Time += Dt * PoseRate;
		if (!Pose.bLoop && Pose.Seq && Pose.Time >= Pose.Seq->GetPlayLength() && !FollowLoopClip.IsNone())
		{
			PlayClip(FollowLoopClip, true);
		}
	}

	// lunge / knockback: horizontal velocity override, gravity untouched
	if (PushTimeLeft > 0.f)
	{
		PushTimeLeft -= Dt;
		UCharacterMovementComponent* Move = C->GetCharacterMovement();
		Move->Velocity.X = PushVelocity.X;
		Move->Velocity.Y = PushVelocity.Y;
		if (PushTimeLeft <= 0.f)
		{
			Move->Velocity.X = 0.f;
			Move->Velocity.Y = 0.f;
		}
	}
	if (State != EBullyCombatState::BlockStun && Guard < GuardMax)
	{
		Guard = FMath::Min(GuardMax, Guard + GuardRegen * Dt);
	}

	// lock-on: keep a target, face it while free
	if (bLockOnHeld)
	{
		if (!LockTarget.IsValid() || LockTarget->IsOut() || FVector::Dist2D(LockTarget->GetOwner()->GetActorLocation(), C->GetActorLocation()) > LockOnRange * 1.2f)
		{
			LockTarget = FindTarget(LockOnRange, 360.f, true);
		}
		if (LockTarget.IsValid() && (State == EBullyCombatState::Idle || State == EBullyCombatState::Blocking))
		{
			FaceToward(LockTarget->GetOwner()->GetActorLocation(), TrackTurnRate * Dt);
		}
	}
	UpdateOrientation();

	switch (State)
	{
	case EBullyCombatState::Idle:
		if (bBlockHeld) { bBlockHeld = false; SetBlockHeld(true); }
		break;
	case EBullyCombatState::Blocking:
		if (!bBlockHeld) { SetState(EBullyCombatState::Idle); }
		break;
	case EBullyCombatState::Attacking:
		TickMove(Dt);
		break;
	case EBullyCombatState::BlockStun:
	case EBullyCombatState::HitStun:
		if (StateTime >= StateDuration)
		{
			if (bBlockHeld)
			{
				SetState(EBullyCombatState::Blocking);
				BlockRaisedAt = W->GetTimeSeconds();
				PlayClip(BlockLoopClip, true);
			}
			else
			{
				SetState(EBullyCombatState::Idle);
			}
		}
		break;
	case EBullyCombatState::Downed:
		if (StateTime >= StateDuration)
		{
			SetState(EBullyCombatState::GettingUp);
			StateDuration = GetUpTime;
			PlayClip(GetUpClip, false);
		}
		break;
	case EBullyCombatState::GettingUp:
		if (StateTime >= StateDuration) { SetState(EBullyCombatState::Idle); }
		break;
	case EBullyCombatState::Grappling:
		GrappleIdleTime += Dt;
		if (!Partner.IsValid() || Partner->State != EBullyCombatState::Grappled)
		{
			Partner.Reset();
			SetState(EBullyCombatState::Idle);
		}
		else if (GrappleIdleTime >= GrappleEscapeTime)
		{
			ReleaseGrapple(true);
		}
		else
		{
			HoldPartner();
		}
		break;
	case EBullyCombatState::Grappled:
		if (!Partner.IsValid()) { SetState(EBullyCombatState::Idle); }
		break;
	case EBullyCombatState::Beaten:
	case EBullyCombatState::KO:
		break;
	}
}
