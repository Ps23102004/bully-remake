// OWNER: builder-B2 (doors)
#include "BullyGameMode.h"

#include "BullyPawn.h"

ABullyGameMode::ABullyGameMode()
{
	DefaultPawnClass = ABullyPawn::StaticClass();
}
