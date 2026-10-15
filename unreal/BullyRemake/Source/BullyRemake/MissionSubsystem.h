// OWNER: mission-fw
// Data-driven mission / objective framework. A mission is a JSON file (Config/Missions/<id>.json): named points,
// trigger volumes and an ordered list of stages. The subsystem runs the stage machine: show the stage's objective +
// waypoint on the HUD, poll its completion condition (and optional fail condition) every frame, run actions on
// start/complete, advance, and on failure reload the stage (or the whole mission). Nothing about a mission is
// hardcoded; combat/NPC systems plug in by firing events (FireEvent) that "event" conditions listen for, and by
// subscribing to the OnStage*/OnMission* delegates.
//
// Schema (all coordinates UE cm; "game_m": [x, y, z] in game metres is accepted anywhere a location is, UE = (100x, -100y, 100z)):
//
//   { "_format": "bully-remake.mission/1", "id": "...", "title": "...",
//     "retry": "stage" | "mission" | "none",  "fail_delay": 2.5,
//     "start": { "location_cm": [x,y,z], "yaw": 0 }          // optional: teleport the player here when the mission starts
//     "trigger_file": "imported/triggers/day_one.json",       // optional extra named points (repo-relative); inline wins
//     "points":   { "name": { "location_cm": [x,y,z] } },
//     "triggers": [ { "id": "...", "center_cm": [..], "half_extent_cm": [..], "yaw": 0, "interact": false, "prompt": "..." } ],
//     "stages": [ { "id": "...", "objective": "...",
//                   "blip": "<point|trigger id>" | [x,y,z] | false,   // default: derived from the complete condition
//                   "complete": <condition>, "fail": <condition>,     // fail optional
//                   "checkpoint": { "location_cm": [..], "yaw": 0 },  // where a stage retry puts the player (default: where the stage began)
//                   "on_start": [<action>], "on_complete": [<action>] } ],
//     "on_complete": [<action>] }
//
//   conditions: { "type": "reach_location", "point": "name" | "location_cm": [..], "radius_cm": 300, "z_tolerance_cm": 400 }
//               { "type": "trigger_enter" | "trigger_exit", "trigger": "id" }
//               { "type": "interact", "trigger": "id" | "actor_tag": "tag", "radius_cm": 250, "prompt": "Open the door" }
//               { "type": "event", "name": "...", "latched": false }   // latched = also true if fired earlier in the mission
//               { "type": "timer", "seconds": 5 }   { "type": "flag", "name": "..." }
//               { "type": "all" | "any", "conditions": [ ... ] }
//   actions:    { "type": "hint", "text": "...", "seconds": 5 } | { "type": "banner", "text": "...", "sub": "..." }
//               { "type": "event", "name": "..." } | { "type": "set_flag" | "clear_flag", "name": "..." }
//               { "type": "teleport", "point": "name" | "location_cm": [..], "yaw": 0 } | { "type": "log", "text": "..." }
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MissionSubsystem.generated.h"

class AMissionTrigger;
class FBullyHUD;
class APawn;

struct FMissionCondition
{
	enum class EType : uint8 { None, Reach, TriggerEnter, TriggerExit, Interact, Event, Timer, Flag, All, Any };
	EType Type = EType::None;
	FVector Location = FVector::ZeroVector;   // Reach
	float Radius = 300.f;                     // Reach radius / Interact actor range
	float ZTolerance = 400.f;
	FName Name;                               // trigger id / event name / flag / actor tag
	bool bActorTag = false;                   // Interact: Name is an actor tag instead of a trigger id
	bool bLatched = false;                    // Event
	float Seconds = 0.f;                      // Timer
	FString Prompt;                           // Interact
	TArray<FMissionCondition> Children;       // All / Any
	bool IsValid() const { return Type != EType::None; }
};

struct FMissionAction
{
	enum class EType : uint8 { Hint, Banner, Event, SetFlag, ClearFlag, Teleport, Log };
	EType Type = EType::Log;
	FString Text, Sub;
	FName Name;
	float Seconds = 5.f;
	FVector Location = FVector::ZeroVector;
	float Yaw = 0.f;
};

struct FMissionStage
{
	FName Id;
	FString Objective;
	enum class EBlip : uint8 { Auto, None, Explicit } BlipMode = EBlip::Auto;
	FVector BlipLocation = FVector::ZeroVector;
	FMissionCondition Complete;
	FMissionCondition Fail;
	bool bHasCheckpoint = false;
	FVector CheckpointLocation = FVector::ZeroVector;
	float CheckpointYaw = 0.f;
	TArray<FMissionAction> OnStart, OnComplete;
};

struct FMissionTriggerDef
{
	FName Id;
	FVector Center = FVector::ZeroVector;
	FVector HalfExtent = FVector(100, 100, 150);
	float Yaw = 0.f;
	bool bInteract = false;
	FString Prompt;
};

struct FMissionDef
{
	FString Id, Title;
	enum class ERetry : uint8 { Stage, Mission, None } Retry = ERetry::Stage;
	float FailDelay = 2.5f;
	bool bHasStart = false;
	FVector StartLocation = FVector::ZeroVector;
	float StartYaw = 0.f;
	TMap<FName, FVector> Points;
	TArray<FMissionTriggerDef> Triggers;
	TArray<FMissionStage> Stages;
	TArray<FMissionAction> OnComplete;
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FMissionStageDelegate, const FMissionDef& /*Mission*/, int32 /*StageIndex (0-based)*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FMissionEventDelegate, FName /*Event*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FMissionDoneDelegate, const FMissionDef& /*Mission*/, bool /*bSuccess*/);

UCLASS()
class UMissionSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	enum class EState : uint8 { Idle, Running, Failed, Complete };

	static UMissionSubsystem* Get(UWorld* World);

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// --- control (console: Mission.Start / Stop / Skip / Fire / Fail / Interact / Goto / Status / List) ---
	bool StartMission(const FString& Id, int32 StageIndex = 0);
	void StopMission(const FString& Reason);
	/** `Arg` is a 1-based stage number, a stage id, or "next". Completes nothing in between (no actions run for skipped stages). */
	bool SkipToStage(const FString& Arg);
	void FailMission(const FString& Reason);
	/** Latches the event for the current stage (and the mission) and tells listeners. Combat/NPC code calls this. */
	void FireEvent(FName Event);
	/** The "press E" action: fires every interact condition of the current stage whose target is in range. Returns whether one fired. */
	bool DoInteract();
	void SetFlag(FName Flag, bool bOn);
	bool HasFlag(FName Flag) const { return Flags.Contains(Flag); }
	/** Test aid: teleport the player onto the current stage's target (if it has a positional condition). */
	bool GotoStageTarget();
	void LogStatus() const;
	static TArray<FString> ListMissionIds();

	// --- state ---
	EState GetState() const { return State; }
	bool IsActive() const { return State == EState::Running || State == EState::Failed; }
	const FMissionDef* GetMission() const { return Mission.IsValid() ? Mission.Get() : nullptr; }
	int32 GetStageIndex() const { return StageIndex; }
	int32 GetAttempt() const { return Attempt; }
	FBullyHUD* GetHUD() const { return Hud.Get(); }

	// --- triggers ---
	void RegisterTrigger(AMissionTrigger* T);
	void UnregisterTrigger(AMissionTrigger* T);
	void NotifyTrigger(AMissionTrigger* T, bool bEntered);

	FMissionStageDelegate OnStageBegan;
	FMissionStageDelegate OnStageCompleted;
	FMissionDoneDelegate OnMissionFinished;   // success or fail-without-retry/stop
	FMissionEventDelegate OnMissionEvent;     // every FireEvent + "trigger_enter:<id>" / "trigger_exit:<id>" / "interact:<key>"

private:
	bool LoadMission(const FString& Id, FMissionDef& Out, FString& Error) const;
	void BeginStage(int32 Index, bool bRetry);
	void CompleteStage();
	void CompleteMission();
	void RetryNow();
	void RunActions(const TArray<FMissionAction>& Actions);
	bool Evaluate(const FMissionCondition& C) const;
	void CollectInteracts(const FMissionCondition& C, TArray<const FMissionCondition*>& Out) const;
	bool InteractInRange(const FMissionCondition& C, FVector* OutAt = nullptr) const;
	bool ConditionTarget(const FMissionCondition& C, FVector& Out) const;
	bool StageTarget(FVector& Out) const;
	APawn* GetPlayerPawn() const;
	void TeleportPlayer(const FVector& Loc, float Yaw, bool bSetYaw);
	void SpawnTriggers();
	void DestroyOwnedTriggers();
	void EnsureHud();
	void RefreshHudPerFrame(float Dt);

	// smoke harness (-BRMissionSmoke[=id]): drives the player through the mission headless and logs PASS/FAIL
	void TickSmoke(float Dt);
	struct FSmoke
	{
		bool bEnabled = false;
		FString MissionId;
		double Elapsed = 0.0;
		double LastDrive = -10.0;
		double InsideSince = -1.0;
		bool bStarted = false;
		bool bFailTested = false;
		bool bDone = false;
		bool bWantFailTest = true;
		int32 LastStage = -1;
		TArray<int32> StagesSeen;
	} Smoke;

	TSharedPtr<FMissionDef> Mission;
	EState State = EState::Idle;
	int32 StageIndex = 0;
	int32 Attempt = 1;
	double StageStartTime = 0.0;
	double FailTime = 0.0;
	FVector StageStartPawnLoc = FVector::ZeroVector;
	FVector MissionStartPawnLoc = FVector::ZeroVector;
	float MissionStartYaw = 0.f;
	TSet<FName> StageEvents;
	TSet<FName> MissionEvents;
	TSet<FName> Flags;
	TMap<FName, TWeakObjectPtr<AMissionTrigger>> Triggers;
	TArray<TWeakObjectPtr<AMissionTrigger>> OwnedTriggers;
	TArray<TWeakObjectPtr<AActor>> TagActors;       // actors found for the current stage's actor_tag interact conditions
	TSharedPtr<FBullyHUD> Hud;
	FString PendingAutoStart;
	double AutoStartAt = 0.0;
};
