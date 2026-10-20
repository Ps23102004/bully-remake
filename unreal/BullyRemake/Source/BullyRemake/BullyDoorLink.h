// OWNER: builder-B2 (doors)
// Paired door teleport (exterior door <-> interior pocket), the Phase 0 stand-in for the game's loading
// doors. Placed in the level by tools/ue/import_content.py from tools/ue/door_links.json.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BullyDoorLink.generated.h"

class ABullyCharacter;
class APlayerController;
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
 *
 * lane-C (owner: "no black screen when entering a building"): the DEFAULT is now an instant seamless cut. The interior
 * pockets are resident in the same (non-World-Partition) map, so a door is just a teleport; UseDoor() with
 * DoorFadeSeconds <= 0 (the default, Config/DefaultGame.ini [/Script/BullyRemake.BullyDoorLink]) teleports on the next
 * actor tick - no fade, no black frame, no input lock - and does the camera cut (history reset, boom snap) in the same
 * tick. A fade (the old FadeOut -> black -> settle -> FadeIn above) only runs when a caller asks for one:
 * UseDoor(Pawn, FadeSeconds>0) for cutscenes / missions, or DoorFadeSeconds>0 in the config. Entry hitches are attacked
 * separately: a timer pre-streams the destination's textures while the player is within PrewarmRadius of a door, and each
 * cut briefly lets Lumen capture twice the surface-cache cards (r.LumenScene.FastCameraMode) so the new room's GI fills in
 * within a few frames instead of popping in.
 */
UCLASS(Config = Game)
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
	/** Per-link override of the fade (s); < 0 = use the global DoorFadeSeconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") float FadeSeconds = -1.f;
	/** Global default fade for walk-through doors (s). 0 = instant seamless cut (default); the fade is for cutscenes / missions. */
	UPROPERTY(Config, EditAnywhere, Category = "Door") float DoorFadeSeconds = 0.f;
	/** Pre-stream the destination room's textures while the player is this close (cm) to the door; 0 = off. */
	UPROPERTY(Config, EditAnywhere, Category = "Door|Prewarm") float PrewarmRadius = 3000.f;
	/** Radius (cm) around the exit whose meshes get their textures pre-streamed, and how long (s) they are held resident. */
	UPROPERTY(Config, EditAnywhere, Category = "Door|Prewarm") float PrewarmExitRadius = 2500.f;
	UPROPERTY(Config, EditAnywhere, Category = "Door|Prewarm") float PrewarmHoldSeconds = 6.f;
	/** After a cut, Lumen captures cards at double rate for this long (s). 0 = off. */
	UPROPERTY(Config, EditAnywhere, Category = "Door|Prewarm") float LumenBoostSeconds = 1.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") float CooldownSeconds = 1.5f;
	/** Time held at full black after the teleport (and at least SettleFrames frames) before fading in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") float SettleSeconds = 0.12f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door") int32 SettleFrames = 3;

	UPROPERTY(VisibleAnywhere, Category = "Door") TObjectPtr<UBoxComponent> Trigger;

	/** Teleport `Pawn` through this door to its partner (also callable from scripts/tests). FadeOverride < 0: use this link's / the
	 *  global fade (default 0 = instant cut); > 0: that many seconds of fade (cutscene / mission use). */
	UFUNCTION(BlueprintCallable, Category = "Door") bool UseDoor(ABullyCharacter* Pawn, float FadeOverride = -1.f);
	float GetEffectiveFade(float FadeOverride) const { return FadeOverride >= 0.f ? FadeOverride : (FadeSeconds >= 0.f ? FadeSeconds : DoorFadeSeconds); }

	ABullyDoorLink* GetPartner();
	/**
	 * Where `Pawn` comes out when it uses this door. Normally the declared partner; but several outside doors can lead to ONE interior
	 * door (door_links.json: the grocery store and the bike shop each have two street doors), and the interior door only declares one of
	 * them. A pawn leaving through the door it came in by (Pawn->LastEntryDoor's partner is this door) goes back to the door it entered.
	 */
	ABullyDoorLink* ResolveDestination(const ABullyCharacter* Pawn);

	virtual void Tick(float DeltaSeconds) override;
	/** Evidence/tests: 0 idle, 1 fade out, 2 black (teleport/settle), 3 fade in, 4 instant cut pending (teleports this tick). */
	int32 GetTransitionState() const { return int32(State); }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UFUNCTION() void OnTriggerBegin(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
	void DoTeleport(ABullyCharacter* Pawn, ABullyDoorLink* Dest);
	void FinishTransition(ABullyCharacter* Pawn);
	void SetFade(ABullyCharacter* Pawn, float Alpha);

	enum class ETransition : uint8 { Idle, FadeOut, Black, FadeIn, Cut };
	void PrewarmTick();
	void ReleaseLumenBoost();
	void ReleaseInputLock();
	FTimerHandle PrewarmTimer;
	FTimerHandle LumenTimer;
	FTimerHandle RetryTimer;   // re-tries a refused use while the player still stands in the trigger
	bool bLumenBoostHeld = false;                    // this link holds one reference on the shared r.LumenScene.FastCameraMode boost
	TWeakObjectPtr<APlayerController> InputLockedPC; // set only when THIS link called SetIgnoreMoveInput(true): it releases exactly that
	FVector CutFrom = FVector::ZeroVector;           // where the traveller stood when it touched the trigger (mission-trigger path test)
	TMap<TWeakObjectPtr<UPrimitiveComponent>, double> Warmed;
	float ActiveFade = 0.f;
	ETransition State = ETransition::Idle;
	TWeakObjectPtr<ABullyDoorLink> Partner;
	TWeakObjectPtr<ABullyCharacter> Traveler;
	TWeakObjectPtr<ABullyDoorLink> Destination;
	float StateTime = 0.f;
	int32 BlackFrames = 0;
	bool bTeleported = false;
public:
	/** Evidence: seen by the headless smoke. */
	int32 PrewarmedComponents = 0;
	int32 InstantCuts = 0;
};
