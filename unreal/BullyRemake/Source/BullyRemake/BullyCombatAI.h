// OWNER: slice-combat
// Tutorial sparring AI for ABullyCombatPawn (stands in for the undecoded Act/AI/AI_FightTutorial.act):
//   approach to EngageDistance -> circle / hold spacing -> telegraphed swing (PUNCH_CHARGE wind-up auto-chained into
//   PUNCH_S_4 jab or HEAVY_PUNCH_2 hook) on a random cooldown -> sometimes raise a block when the player swings ->
//   react to hits (the combat component) -> beaten / KO.
// Gates: pawn bAttackAllowed / bBlockAllowed, the tether, and two mission flags a mission JSON can set_flag:
//   combat_ai_passive (no attacks) and combat_ai_noblock (no blocks).
// A plain AController (no AIModule / navmesh needed: the arena is open ground and the bully steers directly).
// Tuning: [/Script/BullyRemake.BullyCombatAIController] in Config/DefaultGame.ini.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Controller.h"
#include "BullyCombatAI.generated.h"

class UBullyCombatComponent;

UCLASS(Config = Game)
class ABullyCombatAIController : public AController
{
	GENERATED_BODY()

public:
	ABullyCombatAIController();

	/** Preferred centre-to-centre spacing (cm) and the distance a swing is started from. */
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float EngageDistance = 140.f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float AttackRange = 175.f;
	/** Farther than this the bully runs (and drops the lock-on facing); closer it walks / strafes. */
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float RunDistance = 450.f;
	/** Random cooldown between swings (s) and the first swing's delay after engaging. */
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float AttackIntervalMin = 1.4f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float AttackIntervalMax = 2.6f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float FirstAttackDelay = 1.0f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") FName JabMove = TEXT("BullyJabWindup");
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") FName HookMove = TEXT("BullyHookWindup");
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float HookChance = 0.3f;
	/** Chance to answer a player swing with a block, its reaction delay and hold time (s). */
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float BlockChance = 0.3f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float BlockReactTime = 0.08f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float BlockHoldMin = 0.5f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float BlockHoldMax = 1.0f;
	/** Circling while waiting: side-step direction flips every CircleFlipMin..Max s. */
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float CircleFlipMin = 1.2f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float CircleFlipMax = 2.8f;
	/** After taking this many hits in a row the bully backs off for RetreatTime s. 0 = never. */
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") int32 RetreatAfterHits = 3;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") float RetreatTime = 0.9f;
	UPROPERTY(Config, EditAnywhere, Category = "CombatAI") bool bLogAI = true;

	/** Scripts/tests: swing at the next opportunity. */
	void AttackSoon(float Delay = 0.f);
	double NextAttackAt = 0.0;

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPossess(APawn* InPawn) override;

private:
	void Note(const FString& S) const;

	bool bEngaged = false;
	double BlockAt = -1.0;        // raise the block at this time
	double BlockUntil = -1.0;
	FName SeenTargetMove;
	float SeenTargetMoveTime = 0.f;
	double NextCircleFlip = 0.0;
	float CircleDir = 1.f;
	int32 HitsInRow = 0;
	float LastHealth = -1.f;
	double RetreatUntil = -1.0;
	FRandomStream Rng;
};
