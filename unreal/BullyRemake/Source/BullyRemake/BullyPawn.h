// OWNER: builder-B2 (doors)
// Placeholder third-person pawn for Phase 0 (no Jimmy yet; the real character is another builder's job).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "BullyPawn.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UStaticMeshComponent;
class UInputAction;
class UInputMappingContext;

/**
 * Capsule + follow camera + Enhanced Input (WASD / mouse / gamepad), built entirely in C++ (no asset
 * dependencies, so a fresh clone plays). All movement numbers are PLACEHOLDERS (see the UPROPERTY
 * defaults and tools/ue/README.md); the real values come from the Act.img locomotion notes later.
 *
 *   Capsule  radius 30 cm, half height 90 cm (1.8 m tall); eye height 160 cm (camera pivot).
 *   Walk 160 cm/s, run 450 cm/s (hold Shift / left stick click), jump 420 cm/s (~0.9 m), step 45 cm.
 */
UCLASS()
class ABullyPawn : public ACharacter
{
	GENERATED_BODY()

public:
	ABullyPawn();

	// --- placeholder tuning (cm, cm/s) -----------------------------------------------------------
	UPROPERTY(EditAnywhere, Category = "Bully|Tuning") float WalkSpeed = 160.f;
	UPROPERTY(EditAnywhere, Category = "Bully|Tuning") float RunSpeed = 450.f;
	UPROPERTY(EditAnywhere, Category = "Bully|Tuning") float JumpSpeed = 420.f;
	UPROPERTY(EditAnywhere, Category = "Bully|Tuning") float EyeHeight = 160.f;
	UPROPERTY(EditAnywhere, Category = "Bully|Tuning") float CameraDistance = 300.f;
	UPROPERTY(EditAnywhere, Category = "Bully|Tuning") float LookSensitivity = 1.0f;
	UPROPERTY(EditAnywhere, Category = "Bully|Tuning") bool bSnapToGroundOnBeginPlay = true;

	/** Hold-to-run state. The scripted walk runner drives this directly. */
	UFUNCTION(BlueprintCallable, Category = "Bully") void SetRunning(bool bRun);
	UFUNCTION(BlueprintCallable, Category = "Bully") bool IsRunning() const { return bRunning; }

	/** Set by ABullyDoorLink: world time of the last door transition and a running counter (evidence/logging). */
	double LastDoorUseTime = -1000.0;
	int32 DoorUseCount = 0;
	bool bDoorBusy = false;

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;

protected:
	virtual void BeginPlay() override;

private:
	void OnMove(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnRunStarted(const FInputActionValue& Value) { SetRunning(true); }
	void OnRunCompleted(const FInputActionValue& Value) { SetRunning(false); }
	void BuildInputAssets();

	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<USpringArmComponent> CameraBoom;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UCameraComponent> FollowCamera;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UStaticMeshComponent> BodyVis;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UStaticMeshComponent> HeadVis;

	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY(Transient) TObjectPtr<UInputAction> MoveAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LookAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> JumpAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> RunAction;

	bool bRunning = false;
};
