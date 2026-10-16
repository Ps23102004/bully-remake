// OWNER: slice-npc (scripted NPCs)
// Event-driven scripted NPCs for the Day One opening slice: the gauntlet bullies of mission 1_01 and the fight crowd of
// 1_02. NOT combat AI and NOT free-roam ambient wandering. An ABullyScriptedNPC takes commands (GoToPoint, FollowPath,
// FaceActor/FaceLocation, PlayGesture, BlockPath, Disperse, Wait, Despawn, MakeAmbient) from a queue; UScriptedNPCSubsystem
// loads Config/NPCScripts/*.json (ped, spawn point, behaviour, trigger/event: authorable without recompiling) and drives
// the NPCs from mission events (UMissionSubsystem::OnMissionEvent / OnStageBegan) or the SNPC.* console.
#pragma once

#include "CoreMinimal.h"
#include "AmbientWalker.h"
#include "Subsystems/WorldSubsystem.h"
#include "Dom/JsonObject.h"
#include "MissionSubsystem.h"
#include "BullyScriptedNPC.generated.h"

class UAnimSequence;
class ABullyScriptedNPC;

enum class ESNPCCmd : uint8 { GoTo, FollowPath, Face, Gesture, Wait, Block, Disperse, Despawn, MakeAmbient };

/** BlockPath: stand in Jimmy's way until a condition, shoving him when he leans on it, then step aside. */
struct FSNPCBlockSpec
{
	FVector At = FVector::ZeroVector;
	bool bHasAt = false;                 // false = block where the NPC stands now
	float EngageRadius = 450.f;          // player within this (2D) = engaged: face him, taunt
	/** Optional goalkeeper: hold At and shuffle sideways (across the axis At->Guard, within MaxLateralCm) to wherever the
	 *  player is on it, so he cannot just slide around one capsule at a run. Guard = where he is heading. */
	bool bGuard = false;
	FVector Guard = FVector::ZeroVector;
	float MaxLateralCm = 380.f;
	float BodyRadiusCm = 0.f;            // >0: widen the capsule while blocking (a bulky body; a 30 cm capsule is too easy to slide around)
	float AsideCm = 180.f;               // on release: step this far sideways
	int32 AsideSide = 0;                 // +1 / -1 / 0 = auto (the freer side)
	/** Release conditions (any): "timer:S" (since engaged), "timer_start:S" (since the command began), "shoves:N",
	 *  "event:NAME", "x<V" "x>V" "y<V" "y>V" (player UE cm), "player_far:CM". */
	TArray<FString> Until;
	bool bShove = false;
	TArray<FString> ShoveClips;          // candidates (first that loads)
	float ShoveDelay = 1.2f;             // player must be inside ShoveRadius this long before the first shove
	float ShoveInterval = 2.6f;
	float ShoveRadius = 190.f;
	float ShoveSpeed = 520.f;            // cm/s impulse on the player, away from the NPC, horizontal only
	float ShoveContact = 0.45f;          // fraction of the clip at which the hands land
	FName ShoveEvent;                    // emitted per shove (mission event)
	TArray<FString> EngageClips;         // played once on engage
	TArray<FString> HoldClips;           // looped while holding (after the engage clip)
};

struct FSNPCCommand
{
	ESNPCCmd Type = ESNPCCmd::Wait;
	FString Label;                       // log text, e.g. "GoToPoint(1_01_BegginingBullies[1])"
	int32 Token = INDEX_NONE;            // subsystem bookkeeping (which beat issued this)
	bool bQuiet = false;                 // internal commands (stagger waits) do not log
	TArray<FVector> Points;              // GoTo (1 point) / FollowPath / MakeAmbient loop
	EBullyGait Gait = EBullyGait::Walk;
	float Radius = 70.f;                 // arrival radius (2D, cm)
	TArray<FString> Clips;               // Gesture: candidate clip names
	bool bLoop = false;
	float Seconds = 0.f;                 // Gesture loop duration (0 = start the loop and move on) / Wait / Disperse timeout
	float Rate = 1.f;
	enum class ETarget : uint8 { None, Player, Actor, Loc } TargetKind = ETarget::None;   // Face / Disperse focus
	TWeakObjectPtr<AActor> TargetActor;
	FVector TargetLoc = FVector::ZeroVector;
	bool bTrack = false;                 // Face: keep tracking the target afterwards
	FSNPCBlockSpec Block;
	float DisperseDistance = 1800.f;
	enum class EThen : uint8 { Despawn, Ambient, Stay } Then = EThen::Despawn;
};

DECLARE_MULTICAST_DELEGATE_ThreeParams(FSNPCDoneDelegate, ABullyScriptedNPC* /*Npc*/, const FSNPCCommand& /*Cmd*/, const FString& /*Result*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FSNPCEventDelegate, ABullyScriptedNPC* /*Npc*/, FName /*Event*/);

/**
 * Scripted NPC. Shares the AAmbientWalker base (same capsule, movement, locomotion anim instance, mesh/clip folder
 * convention, real world collision, solid to the player) but never runs the loop-walking logic until MakeAmbient hands
 * it back (Disperse "ambient"). Movement is straight-line with capsule collision plus the walker's stuck logic: a command
 * that makes no progress for StuckSeconds first side-steps, then snaps the NPC to its goal and logs BR_SNPC STUCK so a bad
 * script point shows up in the log. Gestures play as single-node clips from the ped's own Anims folder (a missing clip is
 * logged and the command runs on its time budget, so the beats never stall on an unimported clip).
 */
UCLASS()
class ABullyScriptedNPC : public AAmbientWalker
{
	GENERATED_BODY()

public:
	ABullyScriptedNPC();

	FString NpcId;                       // script id, e.g. "bully_a"
	FString GroupId;                     // script group, e.g. "gauntlet_1_01"
	int32 Ordinal = 0;                   // index within the group
	float TurnRateDegPerSec = 540.f;
	float StuckCheckSeconds = 2.5f;

	// --- the command set (each enqueues; log "BR_SNPC <name>: <action> -> done" when finished) -----------------------
	void GoToPoint(const FVector& Where, EBullyGait InGait = EBullyGait::Walk, float ArriveRadius = 70.f, const FString& Label = FString());
	void FollowPath(const TArray<FVector>& Path, EBullyGait InGait = EBullyGait::Walk, float ArriveRadius = 70.f, const FString& Label = FString());
	void FaceActor(AActor* Target, bool bTrackAfter = true);
	void FacePlayer(bool bTrackAfter = true);
	void FaceLocation(const FVector& Where, bool bTrackAfter = false);
	void PlayGesture(const TArray<FString>& ClipCandidates, bool bLoopClip, float Seconds = 0.f, float PlayRate = 1.f);
	void BlockPath(const FSNPCBlockSpec& Spec);
	void Disperse(const FSNPCCommand& Spec);
	void Wait(float Seconds, bool bQuietWait = false);
	void Despawn();
	void MakeAmbient(const TArray<FVector>& Loop);

	void Enqueue(FSNPCCommand&& Cmd);
	/** Drops queued commands; bStopCurrent also aborts the running one (logged "-> interrupted"). */
	void ClearCommands(bool bStopCurrent);
	/** Mission/script events reach a running BlockPath here ("event:NAME" release conditions). */
	void NotifyEvent(FName Event);

	FSNPCDoneDelegate OnCommandDone;
	FSNPCEventDelegate OnNpcEvent;       // "engaged", "shove", "released" (the subsystem maps them to mission events)

	bool IsBusy() const { return bHasCurrent || Queue.Num() > 0; }
	bool IsGestureActive() const { return bGestureActive; }
	const FString& GetGestureName() const { return GestureName; }
	FString DescribeState() const;
	int32 GetShoveCount() const { return ShoveCount; }
	int32 GetCommandsDone() const { return CommandsDone; }
	int32 GetScriptStuckEvents() const { return ScriptStuck; }
	bool IsAmbientNow() const { return bAmbientMode; }

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	// current command lifecycle
	void StartCurrent(double Now);
	void FinishCurrent(const FString& Result);
	/** Tick helpers return true when the command is finished (Result filled). */
	bool TickGoTo(double Now, FString& Result);
	bool TickFace(double Now, FString& Result);
	bool TickGesture(double Now, FString& Result);
	bool TickBlock(double Now, FString& Result);
	bool TickDisperse(double Now, FString& Result);

	// movement
	void MoveToward(const FVector& Target, EBullyGait InGait);
	bool StepToward(const FVector& Target, float Radius, double Now, bool& bStuckSnapped);
	void StopMoving();
	bool TurnToward(const FVector& Where, float Dt, float ToleranceDeg);
	FVector ActorToLoc(const FSNPCCommand& C, bool* bOk = nullptr) const;
	bool CapsuleBlocked(const FVector& From, const FVector& To, FHitResult* OutHit = nullptr) const;
	/** Authoring aid (like the ambient loops' path-check): sweeps the capsule along the legs and logs BR_SNPC path-check. */
	void LogPathCheck(const TArray<FVector>& Legs);
	FVector PickFreeDirection(const FVector& Preferred, float Dist) const;
	void SnapTo(const FVector& Where);
	void SetBlockBody(float Radius);   // Radius<=0 restores the capsule

	// gestures
	UAnimSequence* FindClip(const FString& Name);
	/** Plays the first loadable candidate; returns its length (or the fallback if none loads, bFound=false). */
	float StartClip(const TArray<FString>& Candidates, bool bLoopClip, float Rate, bool& bFound);
	void StopGesture();
	float GestureProgress() const;

	// block
	bool BlockConditionMet(const FSNPCCommand& C, double Now, FString& Why) const;
	/** BlockPath guard: steps to stay between the player and the guarded destination. Returns true while it moved the NPC. */
	bool TickIntercept(const FSNPCBlockSpec& B, APawn* Pl);
	void DoShove(APawn* Player);

	TArray<FSNPCCommand> Queue;
	FSNPCCommand Current;
	bool bHasCurrent = false;
	double CmdStart = 0.0;
	int32 CommandsDone = 0;
	int32 ScriptStuck = 0;
	bool bAmbientMode = false;
	bool bPendingDestroy = false;

	// standing orders (face tracking outlives the command that set it)
	FSNPCCommand::ETarget TrackKind = FSNPCCommand::ETarget::None;
	TWeakObjectPtr<AActor> TrackActor;
	FVector TrackLoc = FVector::ZeroVector;

	// goto progress
	int32 PathIndex = 0;
	FVector ProgressRefPos = FVector::ZeroVector;
	double ProgressRefTime = 0.0;
	int32 StuckStage = 0;                // consecutive checks without progress toward the goal (0 none, 1 side-stepping, 2 snap)
	bool bNoSnap = false;                // a despawning disperse never teleports
	double SideStepUntil = 0.0;
	FVector SideStepDir = FVector::ZeroVector;
	FVector GoalCached = FVector::ZeroVector;
	bool bGoalCached = false;

	// gesture state
	bool bGestureActive = false;
	bool bGestureLoop = false;
	bool bGestureFallback = false;       // clip missing: time-budget only
	double GestureStart = 0.0;
	float GestureLength = 1.f;
	float GestureRate = 1.f;
	FString GestureName;
	UPROPERTY(Transient) TMap<FName, TObjectPtr<UAnimSequence>> ClipCache;
	TSet<FName> MissingClips;
	bool bLocoAnimActive = true;

	// block state
	int32 BlockPhase = 0;                // 0 approach, 1 hold, 2 engaged, 3 aside
	double EngagedAt = -1.0;
	double InShoveRangeSince = -1.0;
	double LastShoveAt = -100.0;
	bool bShoveInFlight = false;
	bool bShoveApplied = false;
	double ShoveStart = 0.0;
	double ShoveCheckAt = -1.0;
	double LastOutOfRange = -1.0;
	FVector ShovePlayerBefore = FVector::ZeroVector;
	int32 ShoveCount = 0;
	bool bHoldStarted = false;
	bool bIntercepting = false;
	double NextBlockTrace = 0.0;
	float SavedCapsuleRadius = 0.f;
	TSet<FName> SeenEvents;
	FString ReleaseWhy;
	FVector AsideTarget = FVector::ZeroVector;

	// disperse state
	FVector DisperseTarget = FVector::ZeroVector;
	bool bDisperseInit = false;
	FVector SpawnLocation = FVector::ZeroVector;
};

// ---------------------------------------------------------------------------------------------------------------

struct FSNPCPoint { FVector Loc = FVector::ZeroVector; float YawUE = 0.f; bool bHasYaw = false; };

struct FSNPCNpcDef
{
	FString Id, Ped, Footsteps, Tag;
	TSharedPtr<FJsonValue> Spawn;        // location ref
	TSharedPtr<FJsonValue> Face;         // optional: face this ref at spawn (else the spawn point's yaw)
	float WalkSpeed = 0.f;               // 0 = the character default
};

struct FSNPCBeat
{
	FString Id;
	TArray<FString> On;                  // cues, any-of: spawn | event:NAME | beat_done:ID | timer:S | near:POINT[:R] | stage_begin:M:S
	TArray<TSharedPtr<FJsonObject>> Steps;
	TArray<FName> Emit;                  // mission events fired when every command of the beat has finished
	float Delay = 0.f;
	bool bInterrupt = false;             // clear the target NPCs' queues first
	bool bRepeat = false;
	// runtime
	bool bFired = false, bDone = false;
	int32 Pending = 0;
	bool bEnqueueing = false;
	TSet<TWeakObjectPtr<ABullyScriptedNPC>> Interrupted;   // NPCs whose queue this firing already cleared
	double FiredAt = 0.0;
	double ScheduledAt = -1.0;
};

struct FSNPCGroup
{
	FString Id;
	FString EventPrefix;                 // mission events emitted: <prefix>_spawned / _engaged / _shove / _released / _despawned
	TSet<FName> Once;
	TArray<FString> SpawnOn, DespawnOn;
	TArray<FSNPCNpcDef> Npcs;
	TArray<FSNPCBeat> Beats;
	// runtime
	bool bActive = false;
	double SpawnTime = 0.0;
	TArray<TWeakObjectPtr<ABullyScriptedNPC>> Spawned;
};

UCLASS()
class UScriptedNPCSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	static UScriptedNPCSubsystem* Get(UWorld* World);

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Loads Config/NPCScripts/<Id>.json (groups, gestures, points). Returns the number of groups added. */
	int32 LoadScript(const FString& Id);
	/** A cue from outside (console / mission): spawns matching groups and fires matching beats. */
	void Dispatch(const FString& Cue);
	bool SpawnGroup(const FString& GroupId, const FString& Why);
	void DespawnGroup(const FString& GroupId, const FString& Why);
	void DespawnAll();
	void LogStatus() const;
	ABullyScriptedNPC* FindNpc(const FString& NpcId) const;

	/** Events the subsystem fired on the mission subsystem (name, world time), for the smoke harness / evidence. */
	const TArray<TPair<FName, double>>& GetEmitted() const { return Emitted; }
	/** Quick console command: "SNPC.Do <npc> goto x y z | gesture CLIP | face player | disperse | block". */
	void DoQuick(const TArray<FString>& Args);
	/** Group lookup for the console / harness. */
	const FSNPCGroup* FindGroup(const FString& GroupId) const;

private:
	// events
	void OnMissionEvent(FName Event);
	void OnStageBegan(const FMissionDef& M, int32 StageIndex);
	void OnMissionFinished(const FMissionDef& M, bool bSuccess);
	void FireEvent(FName Event);
	bool CueMatches(const FString& Pattern, const FString& Cue) const;

	// groups / beats
	bool GroupMatchesCue(const TArray<FString>& List, const FString& Cue) const;
	void DispatchGroup(int32 GroupIdx, const FString& Cue);
	void FireBeat(int32 GroupIdx, int32 BeatIdx, const FString& Cue);
	void RunBeat(int32 GroupIdx, int32 BeatIdx);
	void EnqueueStep(int32 GroupIdx, int32 BeatIdx, const TSharedPtr<FJsonObject>& Step);
	void OnNpcCommandDone(ABullyScriptedNPC* Npc, const FSNPCCommand& Cmd, const FString& Result);
	void OnNpcEvent(ABullyScriptedNPC* Npc, FName Event);
	void BeatMaybeDone(int32 GroupIdx, int32 BeatIdx);
	void TickCues(double Now);

	// data resolution
	struct FResolveCtx { int32 Ordinal = 0; int32 Count = 1; ABullyScriptedNPC* Npc = nullptr; };
	bool ResolveLoc(const TSharedPtr<FJsonValue>& V, const FResolveCtx& C, FVector& Out, float* OutYaw = nullptr, bool* bOutHasYaw = nullptr) const;
	bool ResolveLocs(const TSharedPtr<FJsonValue>& V, const FResolveCtx& C, TArray<FVector>& Out) const;
	bool BuildCommand(const TSharedPtr<FJsonObject>& Step, const FResolveCtx& C, FSNPCCommand& Out, FString& Err) const;
	void ResolveClips(const TSharedPtr<FJsonObject>& O, const TCHAR* GestureKey, const TCHAR* ClipKey, TArray<FString>& Out, int32 Ordinal) const;
	void ResolveGestureAlias(const FString& Alias, int32 Ordinal, TArray<FString>& Out) const;
	TArray<ABullyScriptedNPC*> ResolveTargets(int32 GroupIdx, const FString& Who) const;
	void LoadPointsFile(const FString& RepoRelPath);
	bool AddGroup(const TSharedPtr<FJsonObject>& G, FString& Err);
	ABullyScriptedNPC* SpawnNpc(FSNPCGroup& Group, int32 GroupIdx, const FSNPCNpcDef& Def, int32 Ordinal);

	// smoke harness: -BRNPCSmoke[=gauntlet,crowd,stage]
	void TickSmoke(float Dt);

	TMap<FName, TArray<FSNPCPoint>> Points;
	TMap<FString, TArray<FString>> Gestures;      // alias -> candidate clips
	TArray<FSNPCGroup> Groups;
	TArray<FString> LoadedScripts;
	TArray<TPair<FName, double>> Emitted;
	TArray<TWeakObjectPtr<ABullyScriptedNPC>> AllNpcs;
	int32 DispatchDepth = 0;
	FDelegateHandle EvHandle, StageHandle, DoneHandle;

	struct FSmoke
	{
		bool bEnabled = false;
		FString Phases;
		int32 Step = 0;
		double StepStart = 0.0;
		double Elapsed = 0.0;
		bool bAborted = false;
		TArray<FString> Failures;
		double NextLog = 0.0;
		FVector Target = FVector::ZeroVector;
		bool bWalking = false;
		TArray<FString> Report;
	} Smoke;
};
