// OWNER: slice-combat
// Headless combat test (-BRCombatTest[=OUTDIR], run with tools/ue/run_combat_nullrhi.sh): puts Jimmy in the 1_02
// dorm-court arena, spawns one ABullyCombatPawn and plays the fight tutorial through the REAL input bindings
// (Enhanced Input injection on IA_Attack / IA_Block / IA_Grapple), then a free fight to KO and a floored partner
// that is humiliated. Logs BR_COMBAT lines and writes OUTDIR/combat_result.json, then quits.
//
// Also registers the playtest console commands (any build): Combat.SpawnEnemy [floor], Combat.Status,
// Combat.Gate <attack|block> <0|1>, Combat.Invulnerable <0|1>, Combat.Reset.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "BullyCombatTest.generated.h"

class ABullyCharacter;
class ABullyCombatPawn;
class UBullyCombatComponent;

UCLASS()
class UBullyCombatTest : public UGameInstanceSubsystem, public FTickableGameObject
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

	/** Spawns a sparring partner Dist cm in front of the player (console + test). */
	static ABullyCombatPawn* SpawnEnemyInFront(UWorld* World, ABullyCharacter* Player, float Dist, float HealthFloor, const FString& MeshOverride = FString());

private:
	enum class EStep : uint8 { Warmup, Setup, Block, Combo, Grapple, Push, GroundKick, FreeFight, Humiliate, Done };

	void Note(const FString& S);
	void Pass(const TCHAR* Step, const FString& Detail);
	void Fail(const TCHAR* Step, const FString& Detail);
	void Next(EStep S);
	void Press(FName Action);          // one injected press (Started this frame, Completed the next)
	void Hold(FName Action);           // injected every tick while held
	void Approach(float WantDist);     // walks Jimmy toward the enemy until within WantDist (centre to centre)
	float Dist() const;
	/** Events fired since the current step began. */
	int32 Count(FName Event) const { return Events.FindRef(Event) - StepBase.FindRef(Event); }
	void OnMissionEvent(FName Event);
	void LogPoses(const TCHAR* Tag);
	void Finish();

	bool bActive = false;
	bool bFinished = false;
	FString OutDir;
	EStep Step = EStep::Warmup;
	double Elapsed = 0.0;
	double StepStart = 0.0;
	double LastAction = -10.0;
	int32 Phase = 0;
	int32 Attempts = 0;
	bool bInjectionChecked = false;
	bool bInjectionWorks = true;
	FName PendingCheck;
	int32 PendingCheckFrames = 0;
	TMap<FName, int32> Events;
	TMap<FName, int32> StepBase;
	TWeakObjectPtr<ABullyCharacter> Player;
	TWeakObjectPtr<ABullyCombatPawn> Enemy;
	FVector ArenaCenter = FVector::ZeroVector;
	TArray<FString> Lines;
	TArray<TPair<FString, bool>> Results;
	FDelegateHandle MissionHandle;
	double LastPoseLog = -10.0;
	FName LastEnemyMove;
	int32 EnemySwings = 0;
};
