// OWNER: builder-B3 (jimmy)
// Jimmy Hopkins: the player character (replaces the M2 placeholder BullyPawn; class redirect in DefaultEngine.ini).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "BullyCharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UStaticMeshComponent;
class UInputAction;
class UInputMappingContext;
class UBullyCombatComponent;
class UBullyWeaponComponent;

UENUM(BlueprintType)
enum class EBullyGait : uint8
{
	Walk,
	Run,
	Sprint,
};

/**
 * Third-person character on the imported skinned Jimmy (SK_Jimmy, 36 joints) with the C++ UBullyAnimInstance.
 * Falls back to the engine-primitive placeholder body when the (git-ignored) imported assets are missing.
 *
 * Movement (tools/ue/README.md "Milestone 3" has the sources): default gait is RUN (full stick / any WASD key),
 * partial stick or holding Walk (Left Alt / Left Ctrl) walks, holding Sprint (Left Shift / L3) sprints.
 * Speeds are the clips' authored root-motion speeds so the feet do not slide: walk 2.05, run 3.61, sprint
 * 8.01 m/s (the action trees' ranges are walk 1.3-1.8, run 3.6-3.9, sprint 6.5-8.8). Acceleration, deceleration
 * and turn rates are the GlobalLoco track values. Jump apex 1.10 m (JumpActions LocomotionJump param 24, inferred).
 *
 * Every tuning value is Config=Game: edit [/Script/BullyRemake.BullyCharacter] in Config/DefaultGame.ini, no
 * recompile needed (values are applied in BeginPlay).
 */
UCLASS(Config = Game)
class ABullyCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ABullyCharacter();

	// --- body ---------------------------------------------------------------------------------------------
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Bully|Body") FString MeshPath = TEXT("/Game/Imported/Characters/Jimmy/SK_Jimmy.SK_Jimmy");
	/** Yaw (deg) that turns the clips' forward (game +y = UE -Y) onto the actor's +X. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Bully|Body") float MeshYawOffset = 90.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Bully|Body") float CapsuleRadius = 30.f;
	/** Jimmy's bind pose is 1.70 m tall; standing in the idle clip the head top is ~1.65 m. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Bully|Body") float CapsuleHalfHeight = 85.f;

	// --- movement (cm, cm/s, s, deg/s) ---------------------------------------------------------------------
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float WalkSpeed = 204.6f;
	/** Slowest analog walk (action tree WalkBasic base speed 1.3 m/s). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float WalkSpeedMin = 130.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float RunSpeed = 361.4f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float SprintSpeed = 801.2f;
	/** Stick magnitude below which the gait is walk (action-tree intention: walk 0.2-0.4, run from 0.5). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float WalkStickThreshold = 0.5f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float StickDeadZone = 0.15f;
	/** Time to reach the gait's speed from rest (GlobalLoco param 68: walk 0.2, run 0.333, sprint 0.333). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float WalkAccelTime = 0.2f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float RunAccelTime = 0.333f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float SprintAccelTime = 0.333f;
	/** Time to stop from the gait's speed (param 72: walk 0.3, run 0.333). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float WalkStopTime = 0.3f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float RunStopTime = 0.333f;
	/** Turn rates (param 44, rad/s -> deg/s): walk 10, run 5.5, sprint 6, air 0.8. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float WalkTurnRate = 573.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float RunTurnRate = 315.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float SprintTurnRate = 344.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float AirTurnRate = 46.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float GroundFriction = 8.f;
	/** Jump apex height (cm) of the capsule; the take-off speed is derived from gravity. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float JumpHeight = 110.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float AirControl = 0.15f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float GravityScale = 1.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float MaxStepHeight = 45.f;
	/** Drops above this (cm) are damaging falls (JumpActions Fall_Damage: DistToGround > 5 m). Damage is a stub. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Movement") float FallDamageHeight = 500.f;

	// --- camera (cm, deg) ---------------------------------------------------------------------------------
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraDistance = 450.f;
	/** Height above the feet of the point the camera orbits and looks at (Jimmy's upper chest). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraLookAtHeight = 140.f;
	/** Resting pitch (negative looks down). With distance 450 and look-at 140, -8 puts the lens ~2.0 m up, ~0.4 m above Jimmy's head. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraDefaultPitch = -8.f;
	/** Horizontal FOV (UE convention). The original's CameraDefaultFOV() value is OPEN. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraFOV = 75.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraLagSpeed = 8.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraLagMaxDistance = 80.f;
	/** GTA-style follow: with no look input for CameraFollowDelay s while moving, swing behind the direction of travel. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") bool bCameraAutoFollow = true;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraFollowDelay = 1.0f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraFollowYawRate = 120.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraFollowPitchRate = 30.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraMinPitch = -60.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float CameraMaxPitch = 30.f;
	/**
	 * Look feel. Degrees of camera turn per raw mouse count = LookBaseDegPerCount * LookSensitivity * LookSensitivityX (yaw) /
	 * LookSensitivityY (pitch). Raw Mouse2D was previously applied 1:1 (1 deg per count = far too twitchy); at the defaults
	 * (0.30 * 1.0 * 0.6 = 0.18 deg/count horizontal, 0.15 vertical) a ~500-count comfortable flick turns ~90 deg.
	 * LookSensitivity is the master multiplier; X/Y are the two sliders in the Esc menu (range 0.05..1.0).
	 * Persisted by the menu to the user config layer (Saved/Config/<platform>/Game.ini, [/Script/BullyRemake.BullyCharacter]),
	 * which overrides Config/DefaultGame.ini on the next launch.
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float LookSensitivity = 1.0f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float LookSensitivityX = 0.6f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float LookSensitivityY = 0.5f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") bool bInvertLookY = false;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float LookBaseDegPerCount = 0.30f;
	/** Gamepad right stick: deg/s at full deflection with the default X/Y sensitivity (scaled by X/Y relative to the defaults). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float GamepadLookYawRate = 150.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float GamepadLookPitchRate = 100.f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") bool bSnapToGroundOnBeginPlay = true;
	/** slice-combat: while locked on, the camera yaw swings to frame the target at this rate (deg/s; 0 = off). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Camera") float LockOnCameraYawRate = 240.f;

	// --- camera shake (combat impacts, cutscene punches) -------------------------------------------------
	/**
	 * Additive shake applied to the follow camera's relative transform each tick (no UMG / no CameraShake asset,
	 * matching the pure-C++ HUD). A shake is a decaying multi-frequency jitter: amplitude 1.0 = a firm hit.
	 * Combat drives it from UBullyCombatComponent (hits landed / taken, scaled by reaction); the mission `shake`
	 * action drives it for cutscene beats. Every magnitude is Config=Game.
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|CameraShake") bool bCameraShake = true;
	/** Peak camera translation (cm) at amplitude 1.0. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|CameraShake") float ShakeTranslateScale = 7.f;
	/** Peak camera rotation (deg) at amplitude 1.0. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|CameraShake") float ShakeRotateScale = 1.3f;
	/** Jitter base frequency (rough oscillations/s; per-axis multipliers detune it). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|CameraShake") float ShakeFrequency = 24.f;
	/** Once-a-second BR_CHAR locomotion / foot-slide diagnostics (the combat pawn turns them off). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "Bully|Body") bool bLogLocomotion = true;

	// --- runtime API ---------------------------------------------------------------------------------------
	/** Re-applies every Config value to the components (also called from BeginPlay). */
	UFUNCTION(BlueprintCallable, Category = "Bully") void ApplyTuning();
	/** Live look settings (menu sliders): clamps to the slider range, applies to this pawn and to the class defaults. */
	UFUNCTION(BlueprintCallable, Category = "Bully") void SetLookSettings(float NewX, float NewY, bool bNewInvertY);
	/** Writes X/Y/invert to the user config layer ([/Script/BullyRemake.BullyCharacter] in GGameIni) so they survive a relaunch. */
	UFUNCTION(BlueprintCallable, Category = "Bully") void SaveLookSettings() const;
	static constexpr float MinLookSens = 0.05f;
	static constexpr float MaxLookSens = 1.0f;
	/** Scripted control (walk runner, cutscenes): forces a gait regardless of buttons; ClearScriptedGait releases. */
	UFUNCTION(BlueprintCallable, Category = "Bully") void SetScriptedGait(EBullyGait Gait);
	UFUNCTION(BlueprintCallable, Category = "Bully") void ClearScriptedGait() { bHasScriptedGait = false; }
	UFUNCTION(BlueprintCallable, Category = "Bully") EBullyGait GetGait() const { return Gait; }
	/** Compatibility with the M2 scripts: run (true) / walk (false). */
	UFUNCTION(BlueprintCallable, Category = "Bully") void SetRunning(bool bRun) { SetScriptedGait(bRun ? EBullyGait::Run : EBullyGait::Walk); }
	float GetHorizontalSpeed() const;
	/** slice-combat: melee (attack / block / grapple / lock-on), health and hit reactions. */
	UBullyCombatComponent* GetCombat() const { return Combat; }
	/** slice-weapons: the arsenal (Config/Weapons.json). LMB fires the equipped weapon; with Fists it melees. */
	UBullyWeaponComponent* GetWeapons() const { return Weapons; }
	bool HasJimmyMesh() const { return bHasJimmyMesh; }
	/** Drops the camera boom's location lag for two frames (door teleports: no swoop from the old position). */
	void SnapCamera();
	/** Called by scripts/input when the camera was steered explicitly (suspends auto-follow for CameraFollowDelay). */
	void NoteLookInput() { LastLookInputTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0; }
	/** Kick the follow camera: Amplitude ~1.0 = a firm hit, Duration in seconds. A stronger shake overrides a weaker lingering one. */
	UFUNCTION(BlueprintCallable, Category = "Bully") void AddCameraShake(float Amplitude, float Duration);

	// jump / land bookkeeping read by the anim instance and the evidence runner
	int32 JumpCount = 0;
	int32 LandCount = 0;
	float LastJumpZ = 0.f;
	float LastLandDrop = 0.f;       // cm, apex to landing
	float FallApexZ = 0.f;
	float MaxJumpRise = 0.f;        // cm above take-off reached by the last jump (evidence)
	int32 HardLandings = 0;

	/** Set by ABullyDoorLink: world time of the last door transition and a running counter (evidence/logging). */
	double LastDoorUseTime = -1000.0;
	int32 DoorUseCount = 0;
	bool bDoorBusy = false;

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnJumped_Implementation() override;
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode = 0) override;
	virtual void PostInitializeComponents() override;

protected:
	virtual void BeginPlay() override;

private:
	void OnMove(const FInputActionValue& Value);
	void OnMoveCompleted(const FInputActionValue& Value) { MoveIntent = 0.f; }
	void OnLook(const FInputActionValue& Value);
	void OnLookPad(const FInputActionValue& Value);
	void OnSprint(const FInputActionValue& Value) { bSprintHeld = true; }
	void OnSprintEnd(const FInputActionValue& Value) { bSprintHeld = false; }
	void OnWalk(const FInputActionValue& Value) { bWalkHeld = true; }
	void OnWalkEnd(const FInputActionValue& Value) { bWalkHeld = false; }
	void OnJumpPressed(const FInputActionValue& Value);
	void OnAttack(const FInputActionValue& Value);
	void OnGrapple(const FInputActionValue& Value);
	void OnBlock(const FInputActionValue& Value);
	void OnBlockEnd(const FInputActionValue& Value);
	void OnLockOn(const FInputActionValue& Value);
	void OnLockOnEnd(const FInputActionValue& Value);
	void OnWeaponNext(const FInputActionValue& Value);
	void OnWeaponPrev(const FInputActionValue& Value);
	void BuildInputAssets();
	void UpdateGait();
	void UpdateCamera(float Dt);
	void UpdateCameraShake(float Dt);
	void UpdateFootDiagnostics(float Dt);
	void LoadJimmy();

	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<USpringArmComponent> CameraBoom;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UCameraComponent> FollowCamera;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UStaticMeshComponent> BodyVis;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UStaticMeshComponent> HeadVis;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UBullyCombatComponent> Combat;
	UPROPERTY(VisibleAnywhere, Category = "Bully") TObjectPtr<UBullyWeaponComponent> Weapons;

	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> MappingContext;
	UPROPERTY(Transient) TObjectPtr<UInputAction> MoveAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LookAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LookPadAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> JumpAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> SprintAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> WalkAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> AttackAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> GrappleAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> BlockAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LockOnAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> WeaponNextAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> WeaponPrevAction;

public:
	/** Input actions (the headless combat test injects through them, exercising the real bindings). */
	const UInputAction* GetCombatAction(FName Which) const;

	/** Console: teleport into an imported interior pocket by name (Config/InteriorSpots.json); "BRGo list" lists them. */
	UFUNCTION(Exec) void BRGo(const FString& Name);
	/** Console: refill every weapon to max ammo. */
	UFUNCTION(Exec) void BRGiveWeapons();
	/** Console: spawn a ride (bike / skateboard / gokart / car, Config/Vehicles.json) a few metres ahead. */
	UFUNCTION(Exec) void BRSpawnVehicle(const FString& Id);
private:

	EBullyGait Gait = EBullyGait::Run;
	EBullyGait ScriptedGait = EBullyGait::Run;
	bool bHasScriptedGait = false;
	bool bSprintHeld = false;
	bool bWalkHeld = false;
	float MoveIntent = 0.f;          // stick magnitude of the last move input (0..1)
	double LastMoveInputTime = -100.0;
	double LastLookInputTime = -100.0;
	bool bHasJimmyMesh = false;
	int32 LagRestoreFrames = 0;      // >0: boom lag is off for this many more ticks (SnapCamera)

	// camera shake (decaying additive offset on FollowCamera's relative transform)
	float ShakeAmp = 0.f;            // peak amplitude of the active shake
	float ShakeDuration = 0.f;       // total length of the active shake (s)
	float ShakeTimeLeft = 0.f;       // remaining (s)
	float ShakePhase = 0.f;          // advancing oscillator phase
	bool bShakeSettled = true;       // camera offset is currently zeroed

	// foot-slide diagnostic: horizontal world speed of a planted foot (should be ~0)
	FVector PrevFoot[2] = { FVector::ZeroVector, FVector::ZeroVector };
	bool bPrevFootValid = false;
	TArray<float> SlideSamples;
	double SpeedSum = 0.0;
	int32 SpeedSamples = 0;
	double NextDiagLog = 0.0;
	float FootPlantHeight = 0.f;
};
