// OWNER: builder-B4 (ambient)
// An imported student walking a fixed waypoint loop (milestone 4 "signs of life"; no schedules, no AI).
#pragma once

#include "CoreMinimal.h"
#include "BullyCharacter.h"
#include "AmbientWalker.generated.h"

/**
 * A BullyCharacter (same capsule, movement and C++ locomotion anim instance as Jimmy) that follows `Waypoints` in a
 * loop at its own walk speed and idles a random time at each waypoint. Spawned by UAmbientSubsystem from
 * Config/AmbientLoops.json with the student's skeletal mesh (BullyCharacter::MeshPath) and clips (resolved from the
 * mesh's package folder by UBullyAnimInstance). Collision with the world is real: a walker that stops making progress
 * for StuckSeconds skips to its next waypoint and logs BR_AMBIENT STUCK, so a bad path shows up in the log.
 */
UCLASS()
class AAmbientWalker : public ABullyCharacter
{
	GENERATED_BODY()

public:
	AAmbientWalker();

	FString LoopId;
	FString StudentId;
	TArray<FVector> Waypoints;
	FVector2D IdleSecondsRange = FVector2D(1.5, 4.0);
	float ArriveRadius = 70.f;
	float StuckSeconds = 3.f;
	int32 StartIndex = 0;

	int32 GetLoopsCompleted() const { return LoopsCompleted; }
	int32 GetWaypointsReached() const { return WaypointsReached; }
	int32 GetStuckEvents() const { return StuckEvents; }
	float GetPathLengthWalked() const { return PathWalked; }
	int32 GetNextIndex() const { return NextIndex; }

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void Advance();

	int32 NextIndex = 0;
	bool bIdling = false;
	double IdleUntil = 0.0;
	int32 LoopsCompleted = 0;
	int32 WaypointsReached = 0;
	int32 StuckEvents = 0;
	float PathWalked = 0.f;
	FVector LastPos = FVector::ZeroVector;
	FVector ProgressRefPos = FVector::ZeroVector;
	double ProgressRefTime = 0.0;
	double NextLog = 0.0;
};
