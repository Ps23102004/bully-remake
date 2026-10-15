// OWNER: builder-B2 (doors)
#include "BullyGameMode.h"

#include "BullyCharacter.h"

ABullyGameMode::ABullyGameMode()
{
	DefaultPawnClass = ABullyCharacter::StaticClass();
}
