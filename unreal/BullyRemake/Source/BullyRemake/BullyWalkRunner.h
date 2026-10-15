// OWNER: builder-B2 (doors)
// Scripted gameplay evidence run: drives the REAL pawn (real collision, real door triggers) along a JSON
// script and captures offscreen PNGs. Inactive unless -BRWalkFile= is on the command line.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "BullyWalkRunner.generated.h"

class ABullyPawn;

struct FBullyWalkStep
{
	FString Do;                       // wait | face | teleport | walk_to | wait_door | shot | trace_ground | log | jump
	FString Name;
	FVector Pos = FVector::ZeroVector; // walk_to / teleport / trace_ground (UE cm)
	float Yaw = 0.f;
	float Pitch = -8.f;
	float Seconds = 1.f;               // wait / timeout
	float Radius = 60.f;               // walk_to arrival radius
	bool bRun = true;
	bool bHasZ = false;
	bool bStopOnDoor = true;           // walk_to: also ends when a door transition happened
};

/**
 * Script format (UE cm, degrees):
 *   { "warmup_seconds": 20, "settle_seconds": 5, "max_compile_wait_seconds": 1500, "highres_size": [1920,1080],
 *     "exec": ["t.MaxFPS 30"],
 *     "steps": [ {"do":"shot","name":"gates"}, {"do":"walk_to","x":..,"y":..,"run":true,"timeout":60},
 *                {"do":"wait_door","timeout":4}, {"do":"face","yaw":180,"pitch":-8},
 *                {"do":"teleport","x":..,"y":..,"z":..,"yaw":..}, {"do":"trace_ground","x":..,"y":..} ] }
 * Output: <dir>/NN_<name>.png and <dir>/walk_result.json (positions, stuck/timeouts, door counts).
 */
UCLASS()
class UBullyWalkRunner : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }

private:
	bool LoadScript(const FString& Path);
	void BeginStep(UWorld* World, ABullyPawn* Pawn, FBullyWalkStep& S);
	bool TickStep(UWorld* World, ABullyPawn* Pawn, FBullyWalkStep& S, float Dt);
	void Note(const FString& Line);
	void WriteResult() const;

	bool bActive = false;
	bool bExecDone = false;
	bool bQuit = false;
	bool bStepBegun = false;
	double Elapsed = 0.0;
	double ReadyAt = -1.0;
	double IdleSince = -1.0;
	double LastCompileLog = -100.0;
	double StepStart = 0.0;
	double WarmupSeconds = 15.0;
	double SettleSeconds = 5.0;
	double MaxCompileWaitSeconds = 1500.0;
	double ShotRequestedAt = -1.0;
	int32 Current = 0;
	int32 ShotIndex = 0;
	int32 HighResX = 0, HighResY = 0;
	int32 DoorCountAtStart = 0;
	float BestDist = 1e9f;
	double LastProgressAt = 0.0;
	FString OutDir;
	TArray<FString> ExecCommands;
	TArray<FBullyWalkStep> Steps;
	TArray<FString> Log;
	int32 Failures = 0;
};
