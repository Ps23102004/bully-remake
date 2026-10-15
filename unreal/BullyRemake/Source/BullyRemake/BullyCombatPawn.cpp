// OWNER: slice-combat
#include "BullyCombatPawn.h"

#include "BullyCombatAI.h"
#include "BullyCombatComponent.h"

ABullyCombatPawn::ABullyCombatPawn()
{
	AIControllerClass = ABullyCombatAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	if (UBullyCombatComponent* C = GetCombat())
	{
		C->Team = EBullyCombatTeam::Enemy;
	}
}

void ABullyCombatPawn::PostInitializeComponents()
{
	// ABullyCharacter loads MeshPath here: point it at the ped first
	MeshPath = PedMeshPath;
	Super::PostInitializeComponents();
}

void ABullyCombatPawn::BeginPlay()
{
	bCameraAutoFollow = false;
	bLogLocomotion = false;
	bSnapToGroundOnBeginPlay = true;
	Super::BeginPlay();
	if (!bHasTether)
	{
		TetherCenter = GetActorLocation();
		bHasTether = true;
	}
	if (UBullyCombatComponent* C = GetCombat())
	{
		C->Team = EBullyCombatTeam::Enemy;
		C->MinHealth = HealthFloor;
	}
}

void ABullyCombatPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
}
