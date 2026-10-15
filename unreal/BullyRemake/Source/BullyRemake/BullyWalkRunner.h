// OWNER: builder-B2 (doors); M3 steps (gaits, action shots, frame recording) builder-B3 (jimmy)
// Scripted gameplay evidence run: drives the REAL pawn (real collision, real door triggers) along a JSON
// script and captures offscreen PNGs. Inactive unless -BRWalkFile= is on the command line.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "BullyWalkRunner.generated.h"

class ABullyCharacter;

struct FBullyWalkStep
{
	FString Do;                       // wait | face | teleport | walk_to | wait_door | shot | trace_ground | log | jump | walk_into_walker
	FString Name;
	FVector Pos = FVector::ZeroVector; // walk_to / teleport / trace_ground (UE cm)
	float Yaw = 0.f;
	float Pitch = -8.f;
	float Seconds = 1.f;               // wait / timeout
	float Radius = 60.f;               // walk_to arrival radius
	bool bRun = true;
	bool bHasZ = false;
	bool bStopOnDoor = true;           // walk_to: also ends when a door transition happened
	// --- milestone 3 (Jimmy) ---
	FString Gait;                      // walk | run | sprint (overrides "run")
	FString Cam = TEXT("lock");        // lock: camera pitched -12 behind the walk direction (M2); follow: the character's own follow camera
	bool bHasPos = false;              // action_shot: run toward Pos (else along Yaw)
	bool bJump = false;                // action_shot: jump at JumpAt s
	float JumpAt = 0.f;
	FString Trigger = TEXT("time");    // action_shot: time | apex | air (TriggerTime s after take-off) | land | none | none_land (no shot)
	float TriggerTime = 1.f;
	float After = 1.f;                 // action_shot: keep moving this long after the shot
	float Fps = 30.f;                  // record_start
	// --- polish1 ---
	FString Student;                   // walk_into_walker: AmbientLoops student id ("" = nearest walker); footstep_probe: label
	float SpawnDist = 500.f;           // walk_into_walker head_on: the player is placed this far ahead of the walker's heading
	bool bHeadOn = true;
};

/**
 * Script format (UE cm, degrees):
 *   { "warmup_seconds": 20, "settle_seconds": 5, "max_compile_wait_seconds": 1500, "highres_size": [1920,1080],
 *     "exec": ["t.MaxFPS 30"],
 *     "steps": [ {"do":"shot","name":"gates"}, {"do":"walk_to","x":..,"y":..,"run":true,"timeout":60},
 *                {"do":"wait_door","timeout":4}, {"do":"face","yaw":180,"pitch":-8},
 *                {"do":"teleport","x":..,"y":..,"z":..,"yaw":..}, {"do":"trace_ground","x":..,"y":..} ] }
 * Output: <dir>/NN_<name>.png and <dir>/walk_result.json (positions, stuck/timeouts, door counts).
 * Milestone 3 steps: walk_to accepts "gait" (walk|run|sprint) and "cam" ("follow" leaves the camera to the character);
 *   {"do":"action_shot","name":..,"x":..,"y":.. | "yaw":..,"gait":"run","jump":true,"jump_at":0.6,
 *    "trigger":"time|apex|air|land","trigger_time":..,"after":1.0} moves the character, pauses the game at the trigger,
 *    takes a settled high-res shot of the frozen moment, unpauses and keeps moving;
 *   {"do":"record_start","fps":30} / {"do":"record_stop"}: fixed time step + one viewport PNG per frame into <dir>/frames/.
 * Polish1: {"do":"walk_into_walker","name":..,"student":"boy01","head_on":true,"spawn_dist":500,"gait":"run","timeout":10}
 *   puts the player on the walker's heading (or leaves it where it is) and walks straight at the walker for `timeout`
 *   seconds; logs the closest centre distance (a capsule-capsule contact is 2 x 30 cm, anything under ~50 cm means the
 *   capsules interpenetrated = FAILURE) and how often the walker yielded. Needs no renderer (-nullrhi runs work).
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
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }

private:
	bool LoadScript(const FString& Path);
	void BeginStep(UWorld* World, ABullyCharacter* Pawn, FBullyWalkStep& S);
	bool TickStep(UWorld* World, ABullyCharacter* Pawn, FBullyWalkStep& S, float Dt);
	void Note(const FString& Line);
	void WriteResult() const;
	void RequestShot(ABullyCharacter* Pawn, const FString& Name, bool bHighRes);
	FString PawnState(ABullyCharacter* Pawn) const;

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
	int32 DoorsAcknowledged = 0;      // door uses already consumed by a wait_door step
	float BestDist = 1e9f;
	double LastProgressAt = 0.0;
	FString OutDir;
	TArray<FString> ExecCommands;
	TArray<FBullyWalkStep> Steps;
	TArray<FString> Log;
	int32 Failures = 0;
	// walk_into_walker (polish1)
	TWeakObjectPtr<class AAmbientWalker> WalkerRef;
	float WalkerMinDist = 1e9f;
	int32 WalkerYieldsAtStart = 0;
	// action_shot
	int32 ActionPhase = 0;            // 0 moving, 1 paused/settling, 2 shot taken, 3 after
	double ActionMark = 0.0;
	int32 JumpCountAtStart = 0;
	bool bJumpIssued = false;
	// record
	bool bRecording = false;
	int32 RecordFrame = 0;
	FString RecordDir;
};
