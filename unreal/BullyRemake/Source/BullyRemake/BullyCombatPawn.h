// OWNER: slice-combat
// The tutorial sparring partner (1_02 B6/B7): a ped on ABullyCharacter's movement + anim stack with an enemy-team
// UBullyCombatComponent, driven by ABullyCombatAIController. Spawn it anywhere (console `Combat.SpawnEnemy`, the
// headless test, or later the mission/NPC layer); tutorial gates are public fields (attack / block permission,
// tether, health floor) so a mission script can flip them per step.
#pragma once

#include "CoreMinimal.h"
#include "BullyCharacter.h"
#include "BullyCombatPawn.generated.h"

UCLASS(Config = Game)
class ABullyCombatPawn : public ABullyCharacter
{
	GENERATED_BODY()

public:
	ABullyCombatPawn();

	/**
	 * Body mesh: any imported ped (/Game/Imported/Characters/<Name>/SK_<Name>). The slice's real partner GN_Bully05 is
	 * not imported yet (`python3 -m tools.importers ped GN_Bully05` + run_import_character.sh, then point this at it).
	 * The fight clips come from Jimmy's bank either way (same 36-joint skeleton, remapped by bone name).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Enemy") FString PedMeshPath = TEXT("/Game/Imported/Characters/GN_Boy02/SK_GN_Boy02.SK_GN_Boy02");

	// --- tutorial gates (the original's F_CanBullyAttack / F_CanBlock, tether radius 5 m, health floor 5) -------
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Enemy") bool bAttackAllowed = true;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Enemy") bool bBlockAllowed = true;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Enemy") float TetherRadius = 500.f;
	/** >0: health is floored here and the partner becomes Beaten (humiliation prompt) instead of KO. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Enemy") float HealthFloor = 0.f;
	/** Arena centre for the tether (defaults to the spawn point). */
	FVector TetherCenter = FVector::ZeroVector;
	bool bHasTether = false;

	virtual void PostInitializeComponents() override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
};
