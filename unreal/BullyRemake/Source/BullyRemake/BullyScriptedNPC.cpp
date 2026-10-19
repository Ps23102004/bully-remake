// OWNER: slice-npc (scripted NPCs)
#include "BullyScriptedNPC.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "BullyAnimInstance.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSNPC, Log, All);

namespace
{
	APawn* PlayerOf(UWorld* W) { return W ? UGameplayStatics::GetPlayerPawn(W, 0) : nullptr; }

	float Dist2D(const FVector& A, const FVector& B) { return FVector::Dist2D(A, B); }

	const TCHAR* GaitName(EBullyGait G) { return G == EBullyGait::Walk ? TEXT("walk") : G == EBullyGait::Run ? TEXT("run") : TEXT("sprint"); }
}

ABullyScriptedNPC::ABullyScriptedNPC()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ABullyScriptedNPC::BeginPlay()
{
	Super::BeginPlay();   // AAmbientWalker: pass-through between walkers, walk gait (no waypoints = no loop logic here)
	SpawnLocation = GetActorLocation();
	ProgressRefPos = SpawnLocation;
	UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: spawned (group %s, #%d, mesh %s, %s) at %s yaw %.0f"),
		*NpcId, *GroupId, Ordinal, *MeshPath, HasJimmyMesh() ? TEXT("skinned") : TEXT("PLACEHOLDER BODY"), *GetActorLocation().ToString(), GetActorRotation().Yaw);
}

void ABullyScriptedNPC::EndPlay(const EEndPlayReason::Type Reason)
{
	UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: SUMMARY commands done %d, shoves %d, script-stuck %d, queued %d"), *NpcId, CommandsDone, ShoveCount, ScriptStuck, Queue.Num() + (bHasCurrent ? 1 : 0));
	ABullyCharacter::EndPlay(Reason);   // skip the walker's loop summary: this NPC is not a loop walker
}

// ---------------------------------------------------------------------------------------------------------------
// command API

void ABullyScriptedNPC::Enqueue(FSNPCCommand&& Cmd)
{
	Queue.Add(MoveTemp(Cmd));
}

void ABullyScriptedNPC::GoToPoint(const FVector& Where, EBullyGait InGait, float ArriveRadius, const FString& Label)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::GoTo;
	C.Points = { Where };
	C.Gait = InGait;
	C.Radius = ArriveRadius;
	C.Label = Label.IsEmpty() ? FString::Printf(TEXT("GoToPoint(%.0f,%.0f)"), Where.X, Where.Y) : Label;
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::FollowPath(const TArray<FVector>& Path, EBullyGait InGait, float ArriveRadius, const FString& Label)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::FollowPath;
	C.Points = Path;
	C.Gait = InGait;
	C.Radius = ArriveRadius;
	C.Label = Label.IsEmpty() ? FString::Printf(TEXT("FollowPath(%d points)"), Path.Num()) : Label;
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::FaceActor(AActor* Target, bool bTrackAfter)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::Face;
	C.TargetKind = FSNPCCommand::ETarget::Actor;
	C.TargetActor = Target;
	C.bTrack = bTrackAfter;
	C.Label = FString::Printf(TEXT("FaceActor(%s)"), *GetNameSafe(Target));
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::FacePlayer(bool bTrackAfter)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::Face;
	C.TargetKind = FSNPCCommand::ETarget::Player;
	C.bTrack = bTrackAfter;
	C.Label = TEXT("FacePlayer");
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::FaceLocation(const FVector& Where, bool bTrackAfter)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::Face;
	C.TargetKind = FSNPCCommand::ETarget::Loc;
	C.TargetLoc = Where;
	C.bTrack = bTrackAfter;
	C.Label = FString::Printf(TEXT("FaceLocation(%.0f,%.0f)"), Where.X, Where.Y);
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::PlayGesture(const TArray<FString>& ClipCandidates, bool bLoopClip, float Seconds, float PlayRate)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::Gesture;
	C.Clips = ClipCandidates;
	C.bLoop = bLoopClip;
	C.Seconds = Seconds;
	C.Rate = PlayRate;
	C.Label = FString::Printf(TEXT("PlayGesture(%s%s)"), ClipCandidates.Num() ? *ClipCandidates[0] : TEXT("?"), bLoopClip ? TEXT(",loop") : TEXT(""));
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::BlockPath(const FSNPCBlockSpec& Spec)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::Block;
	C.Block = Spec;
	C.Gait = EBullyGait::Walk;
	C.Label = Spec.bHasAt ? FString::Printf(TEXT("BlockPath(%.0f,%.0f)"), Spec.At.X, Spec.At.Y) : FString(TEXT("BlockPath(here)"));
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::Disperse(const FSNPCCommand& Spec)
{
	FSNPCCommand C = Spec;
	C.Type = ESNPCCmd::Disperse;
	if (C.Label.IsEmpty()) { C.Label = TEXT("Disperse"); }
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::Wait(float Seconds, bool bQuietWait)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::Wait;
	C.Seconds = Seconds;
	C.bQuiet = bQuietWait;
	C.Label = FString::Printf(TEXT("Wait(%.1fs)"), Seconds);
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::Despawn()
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::Despawn;
	C.Label = TEXT("Despawn");
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::MakeAmbient(const TArray<FVector>& Loop)
{
	FSNPCCommand C;
	C.Type = ESNPCCmd::MakeAmbient;
	C.Points = Loop;
	C.Label = FString::Printf(TEXT("MakeAmbient(%d-point loop)"), Loop.Num());
	Enqueue(MoveTemp(C));
}

void ABullyScriptedNPC::ClearCommands(bool bStopCurrent)
{
	TArray<FSNPCCommand> Dropped = MoveTemp(Queue);
	Queue.Reset();
	if (bStopCurrent && bHasCurrent)
	{
		FinishCurrent(TEXT("interrupted"));
		StopMoving();
	}
	for (const FSNPCCommand& C : Dropped)
	{
		if (!C.bQuiet) { UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: %s -> dropped (queue cleared)"), *NpcId, *C.Label); }
		OnCommandDone.Broadcast(this, C, TEXT("dropped"));   // the beat that issued it must not wait for it
	}
}

void ABullyScriptedNPC::NotifyEvent(FName Event)
{
	SeenEvents.Add(Event);
}

FString ABullyScriptedNPC::DescribeState() const
{
	return FString::Printf(TEXT("%s: %s cur=%s queued=%d gesture=%s pos=(%.0f,%.0f,%.0f) done=%d shoves=%d%s"),
		*NpcId, *GroupId, bHasCurrent ? *Current.Label : TEXT("-"), Queue.Num(), bGestureActive ? *GestureName : TEXT("-"),
		GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z, CommandsDone, ShoveCount, bAmbientMode ? TEXT(" [ambient loop]") : TEXT(""));
}

// ---------------------------------------------------------------------------------------------------------------
// tick

void ABullyScriptedNPC::Tick(float Dt)
{
	if (bAmbientMode)
	{
		Super::Tick(Dt);   // handed back to the loop walker
		return;
	}
	ABullyCharacter::Tick(Dt);   // keeps the scripted gait applied; the walker's loop logic is deliberately not run
	UWorld* W = GetWorld();
	if (!W) { return; }
	if (bPendingDestroy) { Destroy(); return; }
	const double Now = W->GetTimeSeconds();

	if (!bHasCurrent && Queue.Num() > 0)
	{
		Current = MoveTemp(Queue[0]);
		Queue.RemoveAt(0);
		bHasCurrent = true;
		StartCurrent(Now);
	}

	bool bMoving = false;
	if (bHasCurrent)
	{
		FString Result;
		bool bDone = false;
		switch (Current.Type)
		{
		case ESNPCCmd::GoTo:
		case ESNPCCmd::FollowPath: bDone = TickGoTo(Now, Result); bMoving = true; break;
		case ESNPCCmd::Face: bDone = TickFace(Now, Result); break;
		case ESNPCCmd::Gesture: bDone = TickGesture(Now, Result); break;
		case ESNPCCmd::Wait: bDone = (Now - CmdStart >= Current.Seconds); Result = TEXT("done"); break;
		case ESNPCCmd::Block: bDone = TickBlock(Now, Result); bMoving = true; break;
		case ESNPCCmd::Disperse: bDone = TickDisperse(Now, Result); bMoving = true; break;
		default: bDone = true; Result = TEXT("done"); break;
		}
		if (bDone) { FinishCurrent(Result); }
	}
	// standing orders: keep facing a tracked target whenever nothing else is steering
	if (!bMoving && TrackKind != FSNPCCommand::ETarget::None && !(bHasCurrent && Current.Type == ESNPCCmd::Face))
	{
		FVector At;
		bool bOk = true;
		if (TrackKind == FSNPCCommand::ETarget::Player) { APawn* P = PlayerOf(W); bOk = P != nullptr; if (P) { At = P->GetActorLocation(); } }
		else if (TrackKind == FSNPCCommand::ETarget::Actor) { AActor* A = TrackActor.Get(); bOk = A != nullptr; if (A) { At = A->GetActorLocation(); } }
		else { At = TrackLoc; }
		if (bOk) { TurnToward(At, Dt, 2.f); }
	}
	// a finished one-shot gesture hands the body back to locomotion
	if (bGestureActive && !bGestureLoop && Now - GestureStart >= GestureLength / FMath::Max(0.05f, GestureRate) + 0.05
		&& !(bHasCurrent && (Current.Type == ESNPCCmd::Gesture || Current.Type == ESNPCCmd::Block)))
	{
		StopGesture();
	}
}

void ABullyScriptedNPC::StartCurrent(double Now)
{
	CmdStart = Now;
	bGoalCached = false;
	StuckStage = 0;
	ProgressRefPos = GetActorLocation();
	ProgressRefTime = Now;
	PathIndex = 0;
	if (!Current.bQuiet)
	{
		UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: %s -> start (t=%.2f at %s)"), *NpcId, *Current.Label, Now, *GetActorLocation().ToString());
	}
	switch (Current.Type)
	{
	case ESNPCCmd::GoTo:
	case ESNPCCmd::FollowPath:
		StopGesture();
		SetScriptedGait(Current.Gait);
		bNoSnap = false;
		LogPathCheck(Current.Points);
		break;
	case ESNPCCmd::Gesture:
	{
		bool bFound = false;
		GestureLength = StartClip(Current.Clips, Current.bLoop, Current.Rate, bFound);
		if (!bFound)
		{
			UE_LOG(LogSNPC, Warning, TEXT("BR_SNPC %s: %s: no clip loaded (%s): running on its time budget (import with tools/ue/run_import_npc.sh)"),
				*NpcId, *Current.Label, *FString::Join(Current.Clips, TEXT(",")));
		}
		break;
	}
	case ESNPCCmd::Block:
		StopGesture();
		BlockPhase = 0;
		EngagedAt = -1.0;
		InShoveRangeSince = -1.0;
		LastShoveAt = -100.0;
		bShoveInFlight = false;
		bHoldStarted = false;
		bIntercepting = false;
		ShoveCount = 0;
		SeenEvents.Reset();
		ReleaseWhy.Reset();
		if (!Current.Block.bHasAt) { Current.Block.At = GetActorLocation(); }
		bNoSnap = false;
		SetScriptedGait(EBullyGait::Walk);
		break;
	case ESNPCCmd::Disperse:
		StopGesture();
		bDisperseInit = false;
		bNoSnap = Current.Then == FSNPCCommand::EThen::Despawn;
		SetScriptedGait(Current.Gait);
		break;
	case ESNPCCmd::MakeAmbient:
		StopGesture();
		StopMoving();
		bAmbientMode = true;
		ResumeLoop(Current.Points.Num() >= 2 ? Current.Points : TArray<FVector>{ GetActorLocation(), SpawnLocation }, 0);
		break;
	case ESNPCCmd::Despawn:
		StopGesture();
		StopMoving();
		break;
	default: break;
	}
}

void ABullyScriptedNPC::FinishCurrent(const FString& Result)
{
	FSNPCCommand Done = MoveTemp(Current);
	bHasCurrent = false;
	SetBlockBody(0.f);   // an interrupted BlockPath gives the body back
	++CommandsDone;
	if (!Done.bQuiet)
	{
		UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: %s -> %s (t=%.2f, %.1fs, at (%.0f,%.0f,%.0f))"), *NpcId, *Done.Label, *Result,
			GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0, (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0) - CmdStart,
			GetActorLocation().X, GetActorLocation().Y, GetActorLocation().Z);
	}
	if (Done.Type == ESNPCCmd::Despawn) { bPendingDestroy = true; }
	OnCommandDone.Broadcast(this, Done, Result);
}

// ---------------------------------------------------------------------------------------------------------------
// movement helpers

void ABullyScriptedNPC::SetBlockBody(float Radius)
{
	UCapsuleComponent* Cap = GetCapsuleComponent();
	if (Radius > 0.f)
	{
		if (SavedCapsuleRadius <= 0.f) { SavedCapsuleRadius = Cap->GetUnscaledCapsuleRadius(); }
		Cap->SetCapsuleRadius(Radius);
	}
	else if (SavedCapsuleRadius > 0.f)
	{
		Cap->SetCapsuleRadius(SavedCapsuleRadius);
		SavedCapsuleRadius = 0.f;
	}
}

void ABullyScriptedNPC::StopMoving()
{
	if (UCharacterMovementComponent* M = GetCharacterMovement()) { M->StopMovementImmediately(); }
}

void ABullyScriptedNPC::SnapTo(const FVector& Where)
{
	FHitResult Hit;
	FCollisionQueryParams P(SCENE_QUERY_STAT(SNPCSnap), true, this);
	const FVector From(Where.X, Where.Y, GetActorLocation().Z + 150.f);
	FVector Dest = FVector(Where.X, Where.Y, GetActorLocation().Z);
	if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, From, From - FVector(0, 0, 600.f), ECC_Visibility, P))
	{
		Dest.Z = Hit.ImpactPoint.Z + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f;
	}
	SetActorLocation(Dest, false, nullptr, ETeleportType::TeleportPhysics);
	StopMoving();
}

bool ABullyScriptedNPC::TurnToward(const FVector& Where, float Dt, float ToleranceDeg)
{
	FVector D = Where - GetActorLocation();
	D.Z = 0.f;
	if (D.SizeSquared() < 4.f) { return true; }
	const float WantYaw = FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
	const float CurYaw = GetActorRotation().Yaw;
	const float Delta = FMath::FindDeltaAngleDegrees(CurYaw, WantYaw);
	if (FMath::Abs(Delta) <= ToleranceDeg) { return true; }
	const float Step = FMath::Clamp(Delta, -TurnRateDegPerSec * Dt, TurnRateDegPerSec * Dt);
	SetActorRotation(FRotator(0.f, CurYaw + Step, 0.f));
	return FMath::Abs(FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, WantYaw)) <= ToleranceDeg;
}

bool ABullyScriptedNPC::CapsuleBlocked(const FVector& From, const FVector& To, FHitResult* OutHit) const
{
	UWorld* W = GetWorld();
	if (!W) { return false; }
	FCollisionQueryParams P(SCENE_QUERY_STAT(SNPCSweep), true, this);
	if (APawn* Pl = PlayerOf(W)) { P.AddIgnoredActor(Pl); }
	for (TActorIterator<AAmbientWalker> It(W); It; ++It) { P.AddIgnoredActor(*It); }   // walkers (scripted or ambient) pass through each other
	const float R = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float HH = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Lift(0.f, 0.f, 45.f);   // kerbs are steps, not walls
	FHitResult Hit;
	const bool bBlocked = W->SweepSingleByChannel(Hit, From + Lift, To + Lift, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(R, FMath::Max(R, HH - 45.f)), P);
	if (OutHit) { *OutHit = Hit; }
	return bBlocked;
}

void ABullyScriptedNPC::LogPathCheck(const TArray<FVector>& Legs)
{
	FVector From = GetActorLocation();
	int32 Bad = 0;
	for (int32 i = 0; i < Legs.Num(); ++i)
	{
		FHitResult Hit;
		FVector To = Legs[i];
		To.Z = From.Z;
		if (CapsuleBlocked(From, To, &Hit))
		{
			++Bad;
			UE_LOG(LogSNPC, Warning, TEXT("BR_SNPC %s: path-check leg %d (%.1f m) BLOCKED by %s at %s"), *NpcId, i, FVector::Dist2D(From, To) / 100.f, *GetNameSafe(Hit.GetActor()), *Hit.ImpactPoint.ToString());
		}
		From = To;
	}
	UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: path-check %s: %d legs, %d blocked%s"), *NpcId, *Current.Label, Legs.Num(), Bad, Bad ? TEXT(" (straight-line walk will side-step then snap: add waypoints)") : TEXT(""));
}

FVector ABullyScriptedNPC::PickFreeDirection(const FVector& Preferred, float Dist) const
{
	const FVector Pos = GetActorLocation();
	const float Probe = FMath::Min(Dist, 700.f);
	for (const float Deg : { 0.f, 30.f, -30.f, 60.f, -60.f, 90.f, -90.f, 120.f, -120.f })
	{
		const FVector D = Preferred.RotateAngleAxis(Deg, FVector::UpVector);
		if (!CapsuleBlocked(Pos, Pos + D * Probe)) { return D; }
	}
	return Preferred;
}

/** One tick of walking to `Target`. Returns true on arrival (2D within Radius) or after a stuck snap. */
bool ABullyScriptedNPC::StepToward(const FVector& Target, float Radius, double Now, bool& bStuckSnapped)
{
	bStuckSnapped = false;
	const FVector Pos = GetActorLocation();
	FVector To(Target.X - Pos.X, Target.Y - Pos.Y, 0.f);
	if (To.Size() <= Radius)
	{
		return true;
	}
	FVector Dir = To.GetSafeNormal();
	if (Now < SideStepUntil && !SideStepDir.IsNearlyZero()) { Dir = (SideStepDir * 0.9f + Dir * 0.3f).GetSafeNormal(); }
	AddMovementInput(Dir, 1.f);

	if (Now - ProgressRefTime >= StuckCheckSeconds)
	{
		const float Speed = GetCharacterMovement()->MaxWalkSpeed;
		const float Gained = Dist2D(ProgressRefPos, Target) - Dist2D(Pos, Target);   // progress toward the goal, not mere motion
		if (Gained < 0.25f * Speed * StuckCheckSeconds)
		{
			++ScriptStuck;
			++StuckStage;
			if (StuckStage == 1)
			{
				const FVector Perp(-Dir.Y, Dir.X, 0.f);
				const float R = GetCapsuleComponent()->GetScaledCapsuleRadius();
				SideStepDir = Perp;
				for (const float Sign : { 1.f, -1.f })
				{
					if (!CapsuleBlocked(Pos, Pos + Perp * Sign * (R * 4.f))) { SideStepDir = Perp * Sign; break; }
				}
				SideStepUntil = Now + 0.8;
				UE_LOG(LogSNPC, Warning, TEXT("BR_SNPC STUCK %s at %s: gained %.0f cm toward %s in %.1f s: side-stepping"), *NpcId, *Pos.ToString(), Gained, *Target.ToString(), StuckCheckSeconds);
			}
			else
			{
				APawn* Pl = PlayerOf(GetWorld());
				if (bNoSnap)
				{
					UE_LOG(LogSNPC, Warning, TEXT("BR_SNPC STUCK %s at %s: still blocked toward %s: giving up (despawning disperse does not teleport)"), *NpcId, *Pos.ToString(), *Target.ToString());
				}
				else if (Pl && Dist2D(Pl->GetActorLocation(), Target) < 160.f)
				{
					UE_LOG(LogSNPC, Warning, TEXT("BR_SNPC STUCK %s at %s: the player stands on the goal %s: counting as arrived"), *NpcId, *Pos.ToString(), *Target.ToString());
				}
				else
				{
					UE_LOG(LogSNPC, Warning, TEXT("BR_SNPC STUCK %s at %s: still blocked toward %s: snapping to the goal"), *NpcId, *Pos.ToString(), *Target.ToString());
					SnapTo(Target);
				}
				bStuckSnapped = true;
				StuckStage = 0;
				ProgressRefPos = GetActorLocation();
				ProgressRefTime = Now;
				return true;
			}
		}
		else { StuckStage = 0; }
		ProgressRefPos = Pos;
		ProgressRefTime = Now;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// GoTo / FollowPath / Face

bool ABullyScriptedNPC::TickGoTo(double Now, FString& Result)
{
	if (Current.Points.Num() == 0) { Result = TEXT("done (no points)"); return true; }
	const bool bLast = PathIndex >= Current.Points.Num() - 1;
	bool bSnapped = false;
	const float Radius = bLast ? Current.Radius : FMath::Max(Current.Radius, 90.f);
	if (StepToward(Current.Points[PathIndex], Radius, Now, bSnapped))
	{
		if (!bLast)
		{
			++PathIndex;
			ProgressRefPos = GetActorLocation();
			ProgressRefTime = Now;
			return false;
		}
		StopMoving();
		const double Took = Now - CmdStart;
		Result = bSnapped ? FString::Printf(TEXT("done (stuck-snapped, %.1fs)"), Took) : FString::Printf(TEXT("done (reached, %.1f m, %s)"), Dist2D(GetActorLocation(), SpawnLocation) / 100.f, GaitName(Current.Gait));
		return true;
	}
	return false;
}

bool ABullyScriptedNPC::TickFace(double Now, FString& Result)
{
	UWorld* W = GetWorld();
	FVector At = FVector::ZeroVector;
	bool bOk = false;
	switch (Current.TargetKind)
	{
	case FSNPCCommand::ETarget::Player: if (APawn* P = PlayerOf(W)) { At = P->GetActorLocation(); bOk = true; } break;
	case FSNPCCommand::ETarget::Actor: if (AActor* A = Current.TargetActor.Get()) { At = A->GetActorLocation(); bOk = true; } break;
	case FSNPCCommand::ETarget::Loc: At = Current.TargetLoc; bOk = true; break;
	default: break;
	}
	if (!bOk) { Result = TEXT("done (no target)"); return true; }
	const float Dt = W ? W->GetDeltaSeconds() : 0.016f;
	const bool bFacing = TurnToward(At, Dt, 6.f);
	if (bFacing || Now - CmdStart > 3.0)
	{
		if (Current.bTrack)
		{
			TrackKind = Current.TargetKind;
			TrackActor = Current.TargetActor;
			TrackLoc = Current.TargetLoc;
		}
		else
		{
			TrackKind = FSNPCCommand::ETarget::None;
		}
		Result = FString::Printf(TEXT("done (%s%s)"), bFacing ? TEXT("facing") : TEXT("turn timed out"), Current.bTrack ? TEXT(", tracking") : TEXT(""));
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// gestures

UAnimSequence* ABullyScriptedNPC::FindClip(const FString& Name)
{
	const FName Key(*Name);
	if (const TObjectPtr<UAnimSequence>* Hit = ClipCache.Find(Key)) { return Hit->Get(); }
	if (MissingClips.Contains(Key)) { return nullptr; }
	const FString Path = FString::Printf(TEXT("%s/Anims/%s.%s"), *FPaths::GetPath(MeshPath), *Name, *Name);
	UAnimSequence* S = LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (S) { ClipCache.Add(Key, S); }
	else { MissingClips.Add(Key); }
	return S;
}

namespace
{
	// Shared ped gesture clips are authored for the male C_Player proportions; played on a differently-proportioned
	// skeleton (e.g. GN_Asiangirl, the only female crowd ped) their absolute bone translations stretch/detach the
	// forearms. Take bone *lengths* from the target skeleton (Skeleton mode, rotation-only) and keep the root on
	// Animation for placement/root motion. One-time, idempotent per unique skeleton; runtime-only (never saved).
	void EnsureGestureRetargeting(USkeletalMeshComponent* M)
	{
		USkeletalMesh* Mesh = M ? M->GetSkeletalMeshAsset() : nullptr;
		USkeleton* Sk = Mesh ? Mesh->GetSkeleton() : nullptr;
		if (!Sk) { return; }
		static TSet<TWeakObjectPtr<USkeleton>> Configured;
		if (Configured.Contains(Sk)) { return; }
		Configured.Add(Sk);
		const int32 NumBones = Sk->GetReferenceSkeleton().GetNum();
		for (int32 i = 0; i < NumBones; ++i)
		{
			Sk->SetBoneTranslationRetargetingMode(i, i == 0 ? EBoneTranslationRetargetingMode::Animation : EBoneTranslationRetargetingMode::Skeleton);
		}
		UE_LOG(LogSNPC, Display, TEXT("BR_SNPC retargeting set on skeleton %s (%d bones: root=Animation, rest=Skeleton) so shared clips stop stretching limbs"), *Sk->GetName(), NumBones);
	}
}

float ABullyScriptedNPC::StartClip(const TArray<FString>& Candidates, bool bLoopClip, float Rate, bool& bFound)
{
	UWorld* W = GetWorld();
	const double Now = W ? W->GetTimeSeconds() : 0.0;
	bFound = false;
	bGestureActive = true;
	bGestureLoop = bLoopClip;
	GestureStart = Now;
	GestureRate = FMath::Max(0.1f, Rate);
	GestureName = Candidates.Num() ? Candidates[0] : FString();
	GestureLength = 1.5f;
	bGestureFallback = true;
	USkeletalMeshComponent* M = GetMesh();
	if (!M || !M->GetSkeletalMeshAsset()) { return GestureLength; }
	EnsureGestureRetargeting(M);   // fix shared-clip limb stretch once per skeleton before the first gesture plays
	for (const FString& C : Candidates)
	{
		if (UAnimSequence* S = FindClip(C))
		{
			M->PlayAnimation(S, bLoopClip);
			M->SetPlayRate(GestureRate);
			M->SetPosition(0.f);
			GestureLength = FMath::Max(0.05f, S->GetPlayLength());
			GestureName = C;
			bGestureFallback = false;
			bFound = true;
			bLocoAnimActive = false;
			UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: gesture %s playing (%.2f s%s, rate %.2f)"), *NpcId, *C, GestureLength, bLoopClip ? TEXT(", looping") : TEXT(""), GestureRate);
			return GestureLength;
		}
	}
	return GestureLength;
}

void ABullyScriptedNPC::StopGesture()
{
	if (!bGestureActive) { return; }
	bGestureActive = false;
	bGestureLoop = false;
	if (!bLocoAnimActive)
	{
		if (USkeletalMeshComponent* M = GetMesh())
		{
			M->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			M->SetAnimInstanceClass(UBullyAnimInstance::StaticClass());
		}
		bLocoAnimActive = true;
	}
	GestureName.Reset();
}

float ABullyScriptedNPC::GestureProgress() const
{
	if (!bGestureActive) { return 0.f; }
	const USkeletalMeshComponent* M = GetMesh();
	if (!bGestureFallback && M && M->GetSingleNodeInstance()) { return M->GetPosition(); }
	return GetWorld() ? float(GetWorld()->GetTimeSeconds() - GestureStart) : 0.f;
}

bool ABullyScriptedNPC::TickGesture(double Now, FString& Result)
{
	const float Elapsed = float(Now - CmdStart);
	const float OneShotTime = GestureLength / GestureRate;
	if (Current.bLoop)
	{
		if (Current.Seconds <= 0.f)
		{
			Result = FString::Printf(TEXT("done (%s looping%s)"), *GestureName, bGestureFallback ? TEXT(", clip MISSING") : TEXT(""));
			return true;   // start the loop and move on; it persists until the next move/gesture
		}
		if (Elapsed >= Current.Seconds)
		{
			const float P = GestureProgress();
			const FString Name = GestureName;
			StopGesture();
			Result = FString::Printf(TEXT("done (%s looped %.1fs, anim t=%.2f)"), *Name, Elapsed, P);
			return true;
		}
		return false;
	}
	if (Elapsed >= OneShotTime + 0.02f)
	{
		const FString Name = GestureName;
		const float AnimT = GestureProgress();
		StopGesture();
		Result = FString::Printf(TEXT("done (%s played %.2fs, anim t=%.2f%s)"), *Name, OneShotTime, AnimT, bGestureFallback ? TEXT(", clip MISSING: time budget") : TEXT(""));
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// BlockPath

bool ABullyScriptedNPC::BlockConditionMet(const FSNPCCommand& C, double Now, FString& Why) const
{
	APawn* Pl = PlayerOf(GetWorld());
	for (const FString& S : C.Block.Until)
	{
		FString Key, Val;
		if (S.Split(TEXT(":"), &Key, &Val))
		{
			if (Key == TEXT("timer")) { if (EngagedAt >= 0.0 && Now - EngagedAt >= FCString::Atod(*Val)) { Why = S; return true; } }
			else if (Key == TEXT("timer_start")) { if (Now - CmdStart >= FCString::Atod(*Val)) { Why = S; return true; } }
			else if (Key == TEXT("shoves")) { if (ShoveCount >= FCString::Atoi(*Val)) { Why = S; return true; } }
			else if (Key == TEXT("event")) { if (SeenEvents.Contains(FName(*Val))) { Why = S; return true; } }
			else if (Key == TEXT("player_far")) { if (EngagedAt >= 0.0 && Pl && Dist2D(Pl->GetActorLocation(), C.Block.At) > FCString::Atof(*Val)) { Why = S; return true; } }
		}
		else if (Pl && S.Len() > 2 && (S[1] == TEXT('<') || S[1] == TEXT('>')) && (S[0] == TEXT('x') || S[0] == TEXT('y')))
		{
			const float V = FCString::Atof(*S.Mid(2));
			const float P = S[0] == TEXT('x') ? Pl->GetActorLocation().X : Pl->GetActorLocation().Y;
			if ((S[1] == TEXT('<') && P < V) || (S[1] == TEXT('>') && P > V)) { Why = S; return true; }
		}
	}
	return false;
}

void ABullyScriptedNPC::DoShove(APawn* Player)
{
	if (!Player) { return; }
	FVector D = Player->GetActorLocation() - GetActorLocation();
	D.Z = 0.f;
	D = D.GetSafeNormal();
	ShovePlayerBefore = Player->GetActorLocation();
	if (ACharacter* PC = Cast<ACharacter>(Player))
	{
		if (UCharacterMovementComponent* M = PC->GetCharacterMovement())
		{
			M->AddImpulse(D * Current.Block.ShoveSpeed, true);   // horizontal velocity change: stays grounded, no jump/land bookkeeping
		}
	}
	ShoveCheckAt = GetWorld()->GetTimeSeconds() + 0.5;
	UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: shove #%d contact -> impulse %.0f cm/s on the player (dist %.0f cm)"), *NpcId, ShoveCount, Current.Block.ShoveSpeed, Dist2D(GetActorLocation(), Player->GetActorLocation()));
	OnNpcEvent.Broadcast(this, FName(TEXT("shove")));
}

bool ABullyScriptedNPC::TickIntercept(const FSNPCBlockSpec& B, APawn* Pl)
{
	if (!B.bGuard || !Pl) { return false; }
	// Hold the post (longitudinally fixed) and shuffle only ACROSS the axis post->guard, to wherever the player is on it:
	// a goalkeeper, not a retreating escort.
	const FVector Pos = GetActorLocation();
	FVector Axis = B.Guard - B.At;
	Axis.Z = 0.f;
	Axis = Axis.GetSafeNormal();
	const FVector Perp(-Axis.Y, Axis.X, 0.f);
	const float Lateral = FMath::Clamp(float(FVector::DotProduct(Pl->GetActorLocation() - B.At, Perp)), -B.MaxLateralCm, B.MaxLateralCm);
	FVector Want = B.At + Perp * Lateral;
	Want.Z = Pos.Z;
	FVector Err = Want - Pos;
	Err.Z = 0.f;
	if (Err.Size() > (bIntercepting ? 4.f : 10.f))
	{
		if (!bIntercepting)
		{
			bIntercepting = true;
			if (bGestureActive && !bShoveInFlight) { StopGesture(); bHoldStarted = false; }   // moving: back to the locomotion pose
			SetScriptedGait(EBullyGait::Run);   // the shuffle has to keep up with a running player
		}
		AddMovementInput(Err.GetSafeNormal(), 1.f);
		return true;
	}
	if (bIntercepting) { bIntercepting = false; StopMoving(); SetScriptedGait(EBullyGait::Walk); }
	return false;
}

bool ABullyScriptedNPC::TickBlock(double Now, FString& Result)
{
	UWorld* W = GetWorld();
	const float Dt = W->GetDeltaSeconds();
	APawn* Pl = PlayerOf(W);
	const FSNPCBlockSpec& B = Current.Block;
	const FVector Pos = GetActorLocation();
	const float PlayerDist = Pl ? Dist2D(Pl->GetActorLocation(), Pos) : 1e9f;

	// the shove's measured result, half a second later
	if (ShoveCheckAt > 0.0 && Now >= ShoveCheckAt && Pl)
	{
		UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: shove #%d result -> player moved %.0f cm from (%.0f,%.0f) to (%.0f,%.0f)"), *NpcId, ShoveCount,
			Dist2D(Pl->GetActorLocation(), ShovePlayerBefore), ShovePlayerBefore.X, ShovePlayerBefore.Y, Pl->GetActorLocation().X, Pl->GetActorLocation().Y);
		ShoveCheckAt = -1.0;
	}

	if (BlockPhase >= 1 && BlockPhase < 3 && Pl && Now >= NextBlockTrace && PlayerDist < 800.f)
	{
		NextBlockTrace = Now + 0.25;
		UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: block trace t=%.2f phase %d npc=(%.0f,%.0f) player=(%.0f,%.0f) v=%.0f cm/s dist %.0f cm%s"), *NpcId, Now, BlockPhase,
			Pos.X, Pos.Y, Pl->GetActorLocation().X, Pl->GetActorLocation().Y, Pl->GetVelocity().Size2D(), PlayerDist, bIntercepting ? TEXT(" [intercepting]") : TEXT(""));
	}
	if (BlockPhase < 3)
	{
		FString Why;
		if (BlockConditionMet(Current, Now, Why))
		{
			ReleaseWhy = Why;
			StopGesture();
			bIntercepting = false;
			SetBlockBody(0.f);
			BlockPhase = 3;
			// aside: perpendicular to the line to the player, toward the freer side
			FVector ToPl = Pl ? (Pl->GetActorLocation() - Pos) : GetActorForwardVector();
			ToPl.Z = 0.f;
			ToPl = ToPl.GetSafeNormal();
			FVector Perp(-ToPl.Y, ToPl.X, 0.f);
			float Sign = B.AsideSide != 0 ? float(B.AsideSide) : 1.f;
			if (B.AsideSide == 0)
			{
				for (const float S : { 1.f, -1.f })
				{
					if (!CapsuleBlocked(Pos, Pos + Perp * S * B.AsideCm)) { Sign = S; break; }
				}
			}
			AsideTarget = Pos + Perp * Sign * B.AsideCm;
			UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: BlockPath released (%s) after %.1f s, %d shoves -> stepping aside to %s"), *NpcId, *Why, Now - CmdStart, ShoveCount, *AsideTarget.ToString());
			OnNpcEvent.Broadcast(this, FName(TEXT("released")));
			SetScriptedGait(EBullyGait::Walk);
			ProgressRefPos = Pos;
			ProgressRefTime = Now;
		}
	}

	switch (BlockPhase)
	{
	case 0:   // walk to the blocking spot
	{
		bool bSnapped = false;
		if (StepToward(B.At, 50.f, Now, bSnapped))
		{
			StopMoving();
			BlockPhase = 1;
			SetBlockBody(B.BodyRadiusCm);
			UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: BlockPath engaged position -> holding at %s%s (waiting for the player within %.0f cm)"), *NpcId, *GetActorLocation().ToString(), bSnapped ? TEXT(" [stuck-snapped]") : TEXT(""), B.EngageRadius);
		}
		if (Pl && PlayerDist <= B.EngageRadius * 0.6f) { StopMoving(); BlockPhase = 1; SetBlockBody(B.BodyRadiusCm); }   // the player got here first: hold where we are
		break;
	}
	case 1:   // hold, watching
		if (!TickIntercept(B, Pl) && Pl) { TurnToward(Pl->GetActorLocation(), Dt, 3.f); }
		if (Pl && PlayerDist <= B.EngageRadius)
		{
			BlockPhase = 2;
			EngagedAt = Now;
			UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: BlockPath engaged -> player at %.0f cm"), *NpcId, PlayerDist);
			OnNpcEvent.Broadcast(this, FName(TEXT("engaged")));
			bHoldStarted = false;
			if (B.EngageClips.Num())
			{
				bool bFound = false;
				StartClip(B.EngageClips, false, 1.f, bFound);
			}
		}
		break;
	case 2:   // engaged: face him, taunt, hold, shove
	{
		const bool bMoved = !bShoveInFlight && TickIntercept(B, Pl);
		if (Pl && !bMoved) { TurnToward(Pl->GetActorLocation(), Dt, 3.f); }
		if (bShoveInFlight)
		{
			const float Prog = float(Now - ShoveStart) * GestureRate;
			if (!bShoveApplied && Prog >= B.ShoveContact * GestureLength) { bShoveApplied = true; DoShove(Pl); }
			if (Now - ShoveStart >= GestureLength / GestureRate + 0.05)
			{
				bShoveInFlight = false;
				if (!bShoveApplied) { bShoveApplied = true; DoShove(Pl); }
				bHoldStarted = false;
			}
			break;
		}
		const bool bEngageClipDone = !bGestureActive || (!bGestureLoop && Now - GestureStart >= GestureLength / GestureRate + 0.05);
		if (!bMoved && !bHoldStarted && bEngageClipDone)
		{
			bHoldStarted = true;
			if (B.HoldClips.Num()) { bool bFound = false; StartClip(B.HoldClips, true, 1.f, bFound); }
			else { StopGesture(); }
		}
		if (B.bShove && Pl)
		{
			if (PlayerDist <= B.ShoveRadius)
			{
				if (InShoveRangeSince < 0.0) { InShoveRangeSince = Now; }
				LastOutOfRange = -1.0;
			}
			else
			{
				if (LastOutOfRange < 0.0) { LastOutOfRange = Now; }
				if (Now - LastOutOfRange > 0.8) { InShoveRangeSince = -1.0; }
			}
			if (InShoveRangeSince >= 0.0 && Now - InShoveRangeSince >= B.ShoveDelay && Now - LastShoveAt >= B.ShoveInterval && bHoldStarted)
			{
				++ShoveCount;
				LastShoveAt = Now;
				ShoveStart = Now;
				bShoveInFlight = true;
				bShoveApplied = false;
				bool bFound = false;
				StartClip(B.ShoveClips, false, 1.f, bFound);
				UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: shove #%d start -> gesture %s (%.2f s%s), player %.0f cm away"), *NpcId, ShoveCount, *GestureName, GestureLength, bFound ? TEXT("") : TEXT(", clip MISSING"), PlayerDist);
			}
		}
		break;
	}
	case 3:   // step aside
	{
		bool bSnapped = false;
		if (StepToward(AsideTarget, 50.f, Now, bSnapped))
		{
			StopMoving();
			// keep an eye on him as he walks past
			TrackKind = FSNPCCommand::ETarget::Player;
			Result = FString::Printf(TEXT("done (released by %s, %d shoves, aside reached%s)"), *ReleaseWhy, ShoveCount, bSnapped ? TEXT(" [stuck-snapped]") : TEXT(""));
			return true;
		}
		break;
	}
	default: break;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Disperse

bool ABullyScriptedNPC::TickDisperse(double Now, FString& Result)
{
	UWorld* W = GetWorld();
	const FVector Pos = GetActorLocation();
	if (!bDisperseInit)
	{
		bDisperseInit = true;
		if (Current.Points.Num() > 0)
		{
			DisperseTarget = Current.Points.Last();
			LogPathCheck(Current.Points);
		}
		else
		{
			FVector Focus = Pos + GetActorForwardVector();
			if (Current.TargetKind == FSNPCCommand::ETarget::Player) { if (APawn* P = PlayerOf(W)) { Focus = P->GetActorLocation(); } }
			else if (Current.TargetKind == FSNPCCommand::ETarget::Actor) { if (AActor* A = Current.TargetActor.Get()) { Focus = A->GetActorLocation(); } }
			else if (Current.TargetKind == FSNPCCommand::ETarget::Loc) { Focus = Current.TargetLoc; }
			FVector Away = Pos - Focus;
			Away.Z = 0.f;
			if (Away.SizeSquared() < 25.f) { Away = FVector(1, 0, 0).RotateAngleAxis(Ordinal * 67.f, FVector::UpVector); }
			Away = Away.GetSafeNormal();
			const FVector Dir = PickFreeDirection(Away, Current.DisperseDistance);
			DisperseTarget = Pos + Dir * Current.DisperseDistance;
		}
		UE_LOG(LogSNPC, Display, TEXT("BR_SNPC %s: Disperse heading to %s (%.1f m, %s, then %s)"), *NpcId, *DisperseTarget.ToString(), Dist2D(Pos, DisperseTarget) / 100.f, GaitName(Current.Gait),
			Current.Then == FSNPCCommand::EThen::Despawn ? TEXT("despawn") : Current.Then == FSNPCCommand::EThen::Ambient ? TEXT("ambient") : TEXT("stay"));
	}
	const float Timeout = Current.Seconds > 0.f ? Current.Seconds : 20.f;
	bool bSnapped = false;
	bool bArrived;
	if (Current.Points.Num() > 1 && PathIndex < Current.Points.Num() - 1)
	{
		// explicit exit path: walk the legs in order (authored around fences), the last point is the dispersal point
		if (StepToward(Current.Points[PathIndex], FMath::Max(Current.Radius, 90.f), Now, bSnapped)) { ++PathIndex; ProgressRefPos = GetActorLocation(); ProgressRefTime = Now; }
		bArrived = false;
	}
	else
	{
		bArrived = StepToward(DisperseTarget, Current.Radius, Now, bSnapped);
	}
	const bool bTimedOut = Now - CmdStart > Timeout;
	if (bArrived || bTimedOut)
	{
		StopMoving();
		if (Current.Then == FSNPCCommand::EThen::Despawn) { bPendingDestroy = true; }
		else if (Current.Then == FSNPCCommand::EThen::Ambient)
		{
			bAmbientMode = true;
			ResumeLoop({ GetActorLocation(), SpawnLocation }, 0);
		}
		Result = FString::Printf(TEXT("done (%s, %s)"), bArrived ? TEXT("dispersed") : TEXT("timeout"),
			Current.Then == FSNPCCommand::EThen::Despawn ? TEXT("despawned") : Current.Then == FSNPCCommand::EThen::Ambient ? TEXT("now an ambient walker") : TEXT("standing"));
		return true;
	}
	return false;
}
