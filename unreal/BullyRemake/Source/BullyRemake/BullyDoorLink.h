// OWNER: builder-B2 (doors)
// Paired door teleport (exterior door <-> interior pocket), the Phase 0 stand-in for the game's loading
// doors. Placed in the level by tools/ue/import_content.py from tools/ue/door_links.json.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BullyDoorLink.generated.h"

class ABullyCharacter;
class UBoxComponent;

/**
 * Walk into the trigger box and the pawn is teleported to the partner link's exit point, with a quick
 * camera fade. Each link has:
 *   - LinkId / PartnerId : names; the partner is resolved at BeginPlay among all ABullyDoorLink actors.
 *   - Trigger            : box in the doorway on THIS side. Entering it uses the door.
 *   - ExitLocation/Yaw   : where a pawn that came THROUGH THE PARTNER appears (on this side of this door,
 *                          just clear of this link's own Trigger so it does not bounce straight back).
 * A re-entry cooldown (CooldownSeconds) also stops ping-pong. All numbers are UE cm / degrees.
 */
UCLASS()
class ABullyDoorLink : public AActor
{
	GENERATED_BODY()

public:
	ABullyDoorLink();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") FName LinkId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") FName PartnerId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") FString Label;
	/** World location (feet level, snapped to the floor at arrival) where pawns emerge from this door. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") FVector ExitLocation = FVector::ZeroVector;
	/** Facing (UE yaw degrees) of a pawn emerging from this door; the camera is aimed the same way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") float ExitYaw = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") float FadeSeconds = 0.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") float CooldownSeconds = 1.5f;

	UPROPERTY(VisibleAnywhere, Category = "Door") TObjectPtr<UBoxComponent> Trigger;

	/** Teleport `Pawn` through this door to its partner (also callable from scripts/tests). */
	UFUNCTION(BlueprintCallable, Category = "Door") bool UseDoor(ABullyCharacter* Pawn);

	ABullyDoorLink* GetPartner();

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION() void OnTriggerBegin(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	void CompleteTransition(ABullyCharacter* Pawn, ABullyDoorLink* Dest);
	void FinishTransition(ABullyCharacter* Pawn);

	TWeakObjectPtr<ABullyDoorLink> Partner;
	FTimerHandle FadeTimer;
};
