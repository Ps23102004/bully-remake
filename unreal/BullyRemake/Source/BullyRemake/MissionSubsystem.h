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
//               { "type": "start_mission", "mission": "<id>", "seconds": 1.0 }   // chain into the next mission after a beat
//               { "type": "spawn_enemy", "distance_cm": 300, "health_floor": 5, "player_invulnerable": true,
//                                        "gate_attack": true, "gate_block": true }   // self-spawn a sparring partner
//               { "type": "fade", "out": 0.6, "hold": 0.5, "in": 0.9,              // cinematic fade-to-black and back;
//                                 "point": "name" | "location_cm": [..], "yaw": 0 } // with a point, teleport at full black
//               { "type": "reward", "money": 75, "respect": 15, "text": "..." }     // pay the persistent wallet + toast
//               { "type": "letterbox", "on": true }                                 // slide cinematic bars in (true) / out (false)
//               { "type": "say", "speaker": "PREFECT", "text": "...", "seconds": 4 } // one cutscene dialogue line at the bottom
//               { "type": "camera", "point": "name" | "location_cm": [..],           // cut/blend to a scripted cutscene camera
//                                   "look": "player" | "<point>" | "look_cm": [..],  //   aimed at the player, a point, or a spot
//                                   "fov": 50, "blend": 1.2 }                         //   (blend 0 = hard cut)
//               { "type": "camera", "release": true, "blend": 0.8 }                  // hand the view back to the player's camera
//               { "type": "shake", "amp": 1.0, "seconds": 0.3 }                      // kick the player's camera (impact beats)
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MissionSubsystem.generated.h"

class AMissionTrigger;
class FBullyHUD;
class APawn;
class ACameraActor;

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
	enum class EType : uint8 { Hint, Banner, Event, SetFlag, ClearFlag, Teleport, Log, StartMission, SpawnEnemy, Fade, Reward, Letterbox, Say, Camera, Shake };
	EType Type = EType::Log;
	FString Text, Sub;
	FName Name;
	float Seconds = 5.f;
	FVector Location = FVector::ZeroVector;
	float Yaw = 0.f;
	// start_mission: Text = mission id, Seconds = delay before it starts (so the complete banner can show).
	// spawn_enemy: spawn a sparring partner in front of the player and (optionally) set the tutorial gates/invuln.
	float Dist = 300.f;          // spawn distance in front, cm
	float HealthFloor = 0.f;     // >0 = beaten + humiliation instead of KO
	int32 PlayerInvuln = -1;     // -1 leave as-is, 0 clear, 1 set Jimmy invulnerable
	int32 GateAttack = -1;       // -1 leave, 0/1 allow the spawned enemy to attack
	int32 GateBlock = -1;        // -1 leave, 0/1 allow the spawned enemy to block
	// fade: cinematic fade-to-black -> (optional) teleport the player at full black -> fade back in. Seconds each.
	// With a point/location, the teleport happens while the screen is black (no visible jump); Location/Yaw hold it.
	float FadeOut = 0.5f, FadeHold = 0.4f, FadeIn = 0.8f;
	bool bFadeTeleport = false;
	// reward: grant to the persistent wallet (UBullyProgressSubsystem) and toast it. Text = optional custom banner line.
	int32 Money = 0, Respect = 0;
	// letterbox: slide the cinematic bars in (bOn) or out. say: Sub = speaker label, Text = line, Seconds = how long it reads.
	bool bOn = true;
	// camera: Location = camera position; Look / bLookPlayer / bHasLook = aim; Fov (0 = leave); Blend = seconds (0 = hard cut).
	// bRelease hands the view back to the player's own camera.
	FVector Look = FVector::ZeroVector;
	bool bHasLook = false, bLookPlayer = false, bRelease = false;
	float Fov = 0.f, Blend = 0.5f;
	// shake: Amp = shake amplitude (1.0 = a firm hit), Seconds = duration.
	float Amp = 1.f;
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

UCLASS(Config = Game)
class UMissionSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	enum class EState : uint8 { Idle, Running, Failed, Complete };

	/** Data-driven boot mission: the id auto-started when no -BRMission= is on the command line (empty = none).
	 *  Set in DefaultGame.ini under [/Script/BullyRemake.MissionSubsystem] (e.g. BootMission=day_one) so a packaged,
	 *  double-clickable build drops straight into that mission. -BRMission= on the command line always overrides this. */
	UPROPERTY(Config)
	FString BootMission;

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
	void TickFade(float Dt);           // drives the cinematic fade (fade action); independent of the stage machine
	void SetCameraFade(float Alpha);   // 0 = clear, 1 = full black, via the player camera manager
	void ApplyCamera(const FMissionAction& A);   // cut/blend to (or release) the scripted cutscene camera
	void ReleaseCutsceneCamera(float Blend);     // hand the view back to the player; destroy the owned camera actor
	void ApplyShake(const FMissionAction& A);    // kick the player's camera (cutscene impact beats)

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

	// cinematic fade state (fade action)
	enum class EFadePhase : uint8 { None, Out, Hold, In } FadePhase = EFadePhase::None;
	float FadeOutDur = 0.5f, FadeHoldDur = 0.4f, FadeInDur = 0.8f;
	double FadePhaseStart = 0.0;
	bool bFadePendingTeleport = false;
	FVector FadeTeleportLoc = FVector::ZeroVector;
	float FadeTeleportYaw = 0.f;

	// scripted cutscene camera (camera action): an owned ACameraActor we blend the view to, and release/destroy on
	// mission end so gameplay never gets left looking through it.
	TWeakObjectPtr<ACameraActor> CutsceneCam;
};
