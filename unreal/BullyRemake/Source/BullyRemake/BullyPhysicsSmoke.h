// OWNER: lane-C (engine)
// Headless physics + combat-on-students + door smoke (-BRPhysicsSmoke[=OUTDIR], tools/ue/run_physics_smoke.sh).
// Answers, with numbers in BR_PHYS lines: does the player's punch connect with a real ambient student, do the importer's
// PhysicsAssets give a usable ragdoll, does a finisher / KO / vehicle-style hit throw the body, does it settle on the floor
// and get back up, does the player ragdoll and recover, and does a door cut show no fade (and a requested fade still work).
// Writes OUTDIR/physics_result.json and quits the engine.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "BullyPhysicsSmoke.generated.h"

class ABullyCharacter;
class AAmbientWalker;
class ABullyDoorLink;
class AMissionTrigger;

UCLASS()
class UBullyPhysicsSmoke : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	enum class EStep : uint8 { Warmup, Setup, Assets, Punch, RagdollWatch, Recovery, KO, Kick, Throw, Stagger, BeatenRag, PlayerRag, PlayerKO, DoorShared, TeleRag, Door, DoorFade, Finish };

	void Note(const FString& S);
	void Pass(const FString& Name, const FString& Detail);
	void Fail(const FString& Name, const FString& Detail);
	void Check(bool bOk, const FString& Name, const FString& Detail) { bOk ? Pass(Name, Detail) : Fail(Name, Detail); }
	void Next(EStep S);
	AAmbientWalker* SpawnTestWalker(const FVector& At, const FString& Ped);
	float FloorZ(const FVector& At, float Above = 200.f) const;
	void Place(ABullyCharacter* Player, const AActor* Target, float Dist);
	void Finish();
	void CheckBystanders();

	bool bActive = false;
	bool bDone = false;
	EStep Step = EStep::Warmup;
	double Elapsed = 0.0;
	double StepStart = 0.0;
	int32 Phase = 0;
	FString OutDir;
	TArray<FString> Lines;
	int32 Passes = 0;
	int32 Failures = 0;

	TWeakObjectPtr<ABullyCharacter> Player;
	TWeakObjectPtr<AAmbientWalker> W0, W1, W2, W3;
	bool bW0Real = false;

	// step state
	float HpBefore = 0.f;
	double HitAt = -1.0;
	FVector PosBeforeHit = FVector::ZeroVector;
	float HitPush = 0.f;
	bool bHitSeen = false;
	double LastPress = -10.0;
	FVector RagStart = FVector::ZeroVector;
	float RagMaxDist = 0.f;
	float RagMaxZ = 0.f;
	float RagMinZ = 1e9f;
	double RagStartedAt = -1.0;
	bool bRagSeen = false;
	FVector PelvisBefore = FVector::ZeroVector;
	float MaxFollowErr = 0.f;
	float MaxFade = 0.f;
	int32 DoorUsesBefore = 0;
	int32 DoorCutsBefore = 0;
	int32 DoorTicks = 0;
	FVector DoorFrom = FVector::ZeroVector;
	TWeakObjectPtr<ABullyDoorLink> DoorExt, DoorInt;

	// bystanders: a student that joins, one that flees, one out of range (the first hit on W0 alerts them)
	TWeakObjectPtr<AAmbientWalker> BJoin, BFlee, BFar;
	float FleeD0 = 0.f;
	bool bBystanderChecked = false;
	// ephemeral doors / mission triggers for the shared-interior, trigger-path, input-lock and Lumen checks
	TWeakObjectPtr<ABullyDoorLink> SOutA, SOutB, SIn;
	TWeakObjectPtr<AMissionTrigger> STouch, SDecoy;
	FVector SExitA = FVector::ZeroVector, SExitB = FVector::ZeroVector, SExitI = FVector::ZeroVector;
	int32 LumenPrev = 0;
};

/**
 * -BRWallProbe[=OUTDIR]: owner feedback "from inside a room there is no wall and you can walk through it". Fires rays + capsule
 * sweeps in 32 directions at two heights from every imported interior spot (Config/InteriorSpots.json) and each door exit, then
 * reports per spot how many rays hit nothing within 25 m (an enclosed room should be ~0), how many capsule sweeps are blocked
 * where the complex line trace sees nothing (collision is double-sided for the player), and whether any hit normal faces away
 * from the ray (back-face hits = double-sided triangle collision). BR_WALL lines + OUTDIR/wall_probe.json, then quits.
 */
UCLASS()
class UBullyWallProbe : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	void Run();
	bool bDone = false;
	double StartedAt = 0.0;
	FString OutDir;
};
