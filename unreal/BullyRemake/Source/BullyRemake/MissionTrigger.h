// OWNER: mission-fw
// Box volume that tells UMissionSubsystem when the player is inside it. Spawned from the mission definition's
// "triggers" list (or hand-placed in a level; it registers itself by TriggerId). Containment is polled in Tick
// (point-in-oriented-box on the player pawn), not driven by physics overlap events, so teleports, door fades and
// -nullrhi runs behave identically and no collision setup can silently miss an entry.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MissionTrigger.generated.h"

class UBoxComponent;

UCLASS()
class AMissionTrigger : public AActor
{
	GENERATED_BODY()

public:
	AMissionTrigger();

	/** Name the mission definition refers to ("trigger": "<id>"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission") FName TriggerId;
	/** If true the trigger is an interaction point: while the player is inside, the HUD shows "Press E to <Prompt>". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission") bool bInteract = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mission") FString Prompt;
	UPROPERTY(VisibleAnywhere, Category = "Mission") TObjectPtr<UBoxComponent> Box;

	UFUNCTION(BlueprintCallable, Category = "Mission") bool IsPlayerInside() const { return bPlayerInside; }

	/** lane-C: the player was carried through this volume without a tick inside it (an instant door cut): report the entry now
	 *  (non-interact triggers only). It reads as "inside" for 0.3 s (the mission subsystem polls IsPlayerInside), then the exit is reported as usual. Returns whether an entry was reported.
	 *  The caller (ABullyDoorLink) only forces triggers the traveller actually touched on the way in (TouchesSegment). */
	bool ForceEnter();

	/** Does the segment A->B, thickened by `Margin` (a capsule's half extents: radius, radius, half height), touch this box? (slab test in box space) */
	bool TouchesSegment(const FVector& A, const FVector& B, const FVector& Margin) const;

	/** Test helper: is this world-space point inside the box (box transform and extents)? */
	bool ContainsPoint(const FVector& WorldPoint) const;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	bool bPlayerInside = false;
	double ForcedUntil = -1.0;   // ForceEnter: stays "inside" until then so the mission tick (level-triggered) can see it
};
