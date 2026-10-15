// OWNER: builder-B2 (doors)
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "BullyGameMode.generated.h"

/** Boots into the placeholder pawn at the level's PlayerStart (front gates). Set as the project's default game mode. */
UCLASS()
class ABullyGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ABullyGameMode();
};
