// OWNER: lane-C (engine) - physical body response
// Ragdoll for any ABullyCharacter (Jimmy, students, bullies, scripted NPCs). Original code; uses the PhysicsAsset the
// importer generates next to every SK_ (PHYS_<ped>), no editor-only API.
//
// What it does: on a knockdown / KO / vehicle hit the skeletal mesh is detached from the capsule, switched to the
// "Ragdoll" collision profile and simulated by Chaos, launched with a velocity (so a punch or a car visibly throws
// the body). The (collision-free) capsule follows the pelvis every tick, so lock-on, hit arcs, the camera and the AI
// keep working on a ragdolled body. When combat wants the fighter back up (Downed -> GettingUp) Recover() works out
// whether the body ended belly-up or belly-down, snaps the capsule to the floor under the pelvis, turns it so the
// get-up clip's lying pose lines up with the ragdoll pose (measured from the clip itself, no hardcoded yaw), re-attaches
// the mesh and returns the clip to play. A body that falls through the world or never settles is recovered on a timer.
//
// Tuning is Config=Game: [/Script/BullyRemake.BullyRagdollComponent] in Config/DefaultGame.ini, no recompile.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BullyRagdollComponent.generated.h"

class ABullyCharacter;
class USkeletalMeshComponent;
class UAnimSequence;

UCLASS(ClassGroup = (Bully), meta = (BlueprintSpawnableComponent), Config = Game)
class UBullyRagdollComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBullyRagdollComponent();

	// --- tuning (Config) ------------------------------------------------------------------------------------
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") bool bEnabled = true;
	/** Which reactions throw the body (the clip-only reaction stays for the rest). */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") bool bOnKnockdown = true;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") bool bOnKO = true;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") bool bOnStagger = false;
	/** Player's own body ragdolls too (knockdown / KO / vehicle). Off = Jimmy always plays the clips. */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") bool bPlayerRagdoll = true;
	/** Launch speed (cm/s) of a normal knockdown / KO hit at strength 1.0, and the extra upward kick. */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float KnockdownLaunchSpeed = 420.f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float KOLaunchSpeed = 300.f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float LaunchUpSpeed = 160.f;
	/** Speed a kick/punch adds to a body that is already down (cm/s at strength 1.0). */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float GroundHitSpeed = 180.f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float MaxLaunchSpeed = 1800.f;
	/** The tutorial sparring partner (ABullyCombatPawn) gets this fraction of the launch so the ground-kick step stays in reach. */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float PartnerLaunchScale = 0.3f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float LinearDamping = 0.25f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float AngularDamping = 1.2f;
	/** Settled = pelvis slower than these for SettleHold seconds (after at least MinSeconds). */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float SettleLinear = 30.f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float SettleAngularDeg = 60.f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float SettleHold = 0.4f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float MinSeconds = 0.9f;
	/** Hard cap: a ragdoll that has not settled by now is treated as settled (rolling down a slope, jittering). */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float MaxSeconds = 7.f;
	/** Pelvis dropping this far (cm) below where the ragdoll started = fell through the world: recover it at the start. */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float FallThroughDepth = 1500.f;
	/** The capsule is moved only by this component while a ragdoll runs. If it is found this far (cm) from where the last tick put it, something
	 *  else teleported the actor (door, mission, console, respawn): the ragdoll is dropped where the capsule now stands instead of dragging it back. */
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") float ExternalMoveTolerance = 60.f;
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") FName GetUpBackClip = TEXT("GETUP");
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") FName GetUpBellyClip = TEXT("GETUP_ONBELLY");
	UPROPERTY(Config, EditAnywhere, Category = "Ragdoll") bool bLog = true;

	// --- commands ------------------------------------------------------------------------------------------------
	/** Would this mesh ragdoll at all (skeletal mesh with a PhysicsAsset that has bodies and a pelvis bone)? */
	bool CanRagdoll() const;
	/** Config gate for a reaction kind: knockdown / KO / stagger / vehicle. */
	bool WantsRagdoll(bool bKnockdown, bool bKO, bool bStagger) const;
	/** Start (or, when already active, add to) a ragdoll. Velocity = launch in cm/s added to the body's current velocity. */
	bool Start(const FVector& LaunchVelocity, FName Reason);
	/** Shove a ragdoll that is already active (kick on a downed body, second car hit). */
	void AddLaunch(const FVector& Velocity);
	/**
	 * Ends the ragdoll for a get-up. BackClip / BellyClip are the two get-up sequences (belly-up / belly-down); returns
	 * the one matching how the body ended, or null when no ragdoll was active. Capsule, yaw and mesh are restored.
	 */
	UAnimSequence* Recover(UAnimSequence* BackClip, UAnimSequence* BellyClip, bool* bOutBelly = nullptr);
	/** Drop the ragdoll and restore the body without picking a get-up (tutorial reset, destroy). Call it BEFORE any teleport of the actor. */
	void Abort();

	// --- queries ------------------------------------------------------------------------------------------------
	bool IsActive() const { return bActive; }
	bool IsSettled() const { return bActive && bSettled; }
	float GetActiveSeconds() const;
	FVector GetPelvisLocation() const;
	int32 GetBodyCount() const;
	int32 GetConstraintCount() const;
	int32 GetStartCount() const { return StartCount; }
	float GetPeakSpeed() const { return PeakSpeed; }
	/** Last Recover(): true if it ended belly-down. */
	bool LastWasBelly() const { return bLastBelly; }
	/** Smoke/diagnostics: dot of the measured front direction with the actor's forward while upright (1 = calibrated, -1 = flipped). */
	float GetFrontSign() const { return FrontSign; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	ABullyCharacter* GetChar() const;
	USkeletalMeshComponent* GetMeshComp() const;
	bool ResolveBones();
	void Restore(bool bPlaceOnFloor);
	/** Component-space (mesh local) transform of `Bone` at t=0 of `Clip`; false if the clip/bone is unusable. */
	bool ClipBoneCS(UAnimSequence* Clip, int32 Bone, FTransform& Out) const;
	/** Horizontal facing of the body pelvis->head in world space plus whether it ended belly-down (front vector points at the floor). */
	bool MeasureBody(FVector& OutHeadDirWorld, bool& bOutBelly) const;
	void CalibrateFront();

	bool bActive = false;
	bool bSettled = false;
	bool bBonesResolved = false;
	bool bLastBelly = false;
	float FrontSign = 1.f;
	double StartTime = 0.0;
	double SettledSince = -1.0;
	FVector StartLocation = FVector::ZeroVector;
	FVector LastSnapLoc = FVector::ZeroVector;   // where the previous tick put the capsule (see ExternalMoveTolerance)
	float PeakSpeed = 0.f;
	int32 StartCount = 0;
	FName Reason;

	int32 BonePelvis = INDEX_NONE;
	int32 BoneHead = INDEX_NONE;
	int32 BoneLThigh = INDEX_NONE;
	int32 BoneRThigh = INDEX_NONE;
	FName PelvisName;

	// restored on Recover/Abort
	FVector SavedRelLoc = FVector::ZeroVector;
	FRotator SavedRelRot = FRotator::ZeroRotator;
	FName SavedProfile;
	uint8 SavedAnimTick = 0;
	TEnumAsByte<ECollisionEnabled::Type> SavedMeshCollision = ECollisionEnabled::NoCollision;
	TEnumAsByte<ECollisionEnabled::Type> SavedCapsuleCollision = ECollisionEnabled::QueryAndPhysics;
};
