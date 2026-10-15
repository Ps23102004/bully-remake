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
 *
 * Transition (polish1): driven frame by frame from Tick, not by a timer, so the order is guaranteed and one frame is
 * never a half-faded cut. FadeOut (smoothstep to full black over FadeSeconds, exact 1.0 reached) -> one frame at full
 * black -> teleport + camera cut + spring-arm lag flush -> hold black for SettleSeconds and at least SettleFrames
 * rendered frames (lets streaming / Lumen / the exposure settle on the new view while nothing is visible) -> FadeIn
 * (smoothstep from 1.0). The old timer version teleported on the timer tick, which runs before the camera manager has
 * applied the last fade step, so the cut landed on a ~90 % faded frame and the new view then restarted at ~87 %:
 * a visible pop plus (in the fixed-step capture) one pure black frame. BR_DOOR lines log every state change.
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
	/** Time held at full black after the teleport (and at least SettleFrames frames) before fading in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") float SettleSeconds = 0.12f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") int32 SettleFrames = 3;

	UPROPERTY(VisibleAnywhere, Category = "Door") TObjectPtr<UBoxComponent> Trigger;

	/** Teleport `Pawn` through this door to its partner (also callable from scripts/tests). */
	UFUNCTION(BlueprintCallable, Category = "Door") bool UseDoor(ABullyCharacter* Pawn);

	ABullyDoorLink* GetPartner();

	virtual void Tick(float DeltaSeconds) override;
	/** Evidence/tests: 0 idle, 1 fade out, 2 black (teleport/settle), 3 fade in. */
	int32 GetTransitionState() const { return int32(State); }

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION() void OnTriggerBegin(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	void DoTeleport(ABullyCharacter* Pawn, ABullyDoorLink* Dest);
	void FinishTransition(ABullyCharacter* Pawn);
	void SetFade(ABullyCharacter* Pawn, float Alpha);

	enum class ETransition : uint8 { Idle, FadeOut, Black, FadeIn };
	ETransition State = ETransition::Idle;
	TWeakObjectPtr<ABullyDoorLink> Partner;
	TWeakObjectPtr<ABullyCharacter> Traveler;
	TWeakObjectPtr<ABullyDoorLink> Destination;
	float StateTime = 0.f;
	int32 BlackFrames = 0;
	bool bTeleported = false;
};
