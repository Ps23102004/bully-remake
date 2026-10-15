// OWNER: builder-B4 (ambient)
#include "AmbientWalker.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogAmbient, Log, All);

AAmbientWalker::AAmbientWalker()
{
	// No controller: the movement component must still run, and walkers do not block each other (pawn overlap).
	GetCharacterMovement()->bRunPhysicsWithNoController = true;
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	bSnapToGroundOnBeginPlay = true;
	bCameraAutoFollow = false;
}

void AAmbientWalker::BeginPlay()
{
	Super::BeginPlay();
	SetScriptedGait(EBullyGait::Walk);   // WalkSpeed (set by the spawner) is the loop speed
	NextIndex = Waypoints.Num() ? StartIndex % Waypoints.Num() : 0;
	LastPos = ProgressRefPos = GetActorLocation();
	ProgressRefTime = GetWorld()->GetTimeSeconds();
	UE_LOG(LogAmbient, Display, TEXT("BR_AMBIENT walker %s (%s) loop %s start %s -> wp %d of %d, speed %.0f cm/s"),
		*GetName(), *StudentId, *LoopId, *GetActorLocation().ToString(), NextIndex, Waypoints.Num(), WalkSpeed);
}

void AAmbientWalker::Advance()
{
	if (Waypoints.Num() == 0) { return; }
	NextIndex = (NextIndex + 1) % Waypoints.Num();
	if (NextIndex == StartIndex % Waypoints.Num()) { ++LoopsCompleted; }
}

void AAmbientWalker::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);   // keeps the scripted gait applied
	UWorld* W = GetWorld();
	if (!W || Waypoints.Num() == 0) { return; }
	const double Now = W->GetTimeSeconds();
	const FVector Pos = GetActorLocation();
	PathWalked += FVector::Dist2D(Pos, LastPos);
	LastPos = Pos;

	if (bIdling)
	{
		if (Now >= IdleUntil)
		{
			bIdling = false;
			ProgressRefPos = Pos;
			ProgressRefTime = Now;
		}
	}
	else
	{
		const FVector Target = Waypoints[NextIndex];
		const FVector To = FVector(Target.X - Pos.X, Target.Y - Pos.Y, 0.f);
		if (To.Size() <= ArriveRadius)
		{
			++WaypointsReached;
			bIdling = true;
			IdleUntil = Now + FMath::FRandRange(IdleSecondsRange.X, IdleSecondsRange.Y);
			UE_LOG(LogAmbient, Display, TEXT("BR_AMBIENT %s reached wp %d (loops %d) t=%.1f"), *GetName(), NextIndex, LoopsCompleted, Now);
			Advance();
		}
		else
		{
			AddMovementInput(To.GetSafeNormal(), 1.f);
			// stuck detection: expected progress over StuckSeconds is far above this
			if (Now - ProgressRefTime >= StuckSeconds)
			{
				const float Moved = FVector::Dist2D(Pos, ProgressRefPos);
				if (Moved < 0.25f * WalkSpeed * StuckSeconds)
				{
					++StuckEvents;
					UE_LOG(LogAmbient, Warning, TEXT("BR_AMBIENT STUCK %s at %s moved %.0f cm in %.1f s heading to wp %d %s: skipping it"),
						*GetName(), *Pos.ToString(), Moved, StuckSeconds, NextIndex, *Target.ToString());
					Advance();
				}
				ProgressRefPos = Pos;
				ProgressRefTime = Now;
			}
		}
	}
	if (Now >= NextLog)
	{
		NextLog = Now + 5.0;
		UE_LOG(LogAmbient, Display, TEXT("BR_AMBIENT t=%.1f %s pos=(%.0f,%.0f,%.0f) speed=%.0f %s next=%d reached=%d loops=%d stuck=%d walked=%.1f m"),
			Now, *GetName(), Pos.X, Pos.Y, Pos.Z, GetHorizontalSpeed(), bIdling ? TEXT("idle") : TEXT("walk"), NextIndex, WaypointsReached, LoopsCompleted, StuckEvents, PathWalked / 100.f);
	}
}

void AAmbientWalker::EndPlay(const EEndPlayReason::Type Reason)
{
	UE_LOG(LogAmbient, Display, TEXT("BR_AMBIENT SUMMARY %s (%s) loop %s: waypoints reached %d, loops completed %d, stuck %d, walked %.1f m"),
		*GetName(), *StudentId, *LoopId, WaypointsReached, LoopsCompleted, StuckEvents, PathWalked / 100.f);
	Super::EndPlay(Reason);
}
