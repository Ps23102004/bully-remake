// OWNER: builder-B2 (doors)
#include "BullyGameMode.h"

#include "BullyCharacter.h"
#include "BullyPlayerController.h"

ABullyGameMode::ABullyGameMode()
{
	DefaultPawnClass = ABullyCharacter::StaticClass();
	PlayerControllerClass = ABullyPlayerController::StaticClass();   // Esc settings menu
}
