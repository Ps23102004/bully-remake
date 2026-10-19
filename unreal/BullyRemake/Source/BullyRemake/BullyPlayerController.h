// OWNER: settings-menu
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "BullyPlayerController.generated.h"

class SWidget;
class ABullyCharacter;

/**
 * Player controller with the Esc pause/settings menu. The menu is a pure-C++ Slate overlay (SBullySettingsMenu,
 * BullyPlayerController.cpp) added to the game viewport, so it works in a cooked -game run with no UMG asset.
 *
 * Open: world paused, look/move ignored, mouse cursor shown, input mode Game-and-UI with the mouse NOT locked to the
 * viewport (the cursor is free to leave the window and click other apps). Closed (Esc / Resume): unpaused, cursor hidden,
 * input mode Game-only (mouse captured for look again).
 */
UCLASS()
class ABullyPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void SetupInputComponent() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** Console: `BullyMenu`. Esc is bound in SetupInputComponent. */
	UFUNCTION(Exec, BlueprintCallable, Category = "Bully|Menu") void ToggleSettingsMenu();
	/** ai-npc: talk to the nearest NPC (UNPCChatSubsystem). Console: `NPCChat`; T is bound in SetupInputComponent. */
	UFUNCTION(Exec, BlueprintCallable, Category = "Bully|Chat") void NPCChat();
	UFUNCTION(BlueprintCallable, Category = "Bully|Menu") void OpenSettingsMenu();
	UFUNCTION(BlueprintCallable, Category = "Bully|Menu") void CloseSettingsMenu();
	UFUNCTION(BlueprintCallable, Category = "Bully|Menu") bool IsSettingsMenuOpen() const { return SettingsWidget.IsValid(); }

	/** Teleport menu: pick any imported area and warp there (all unlocked). Console: `TeleportMenu`; G is bound in SetupInputComponent. */
	UFUNCTION(Exec, BlueprintCallable, Category = "Bully|Menu") void ToggleTeleportMenu();
	UFUNCTION(BlueprintCallable, Category = "Bully|Menu") void OpenTeleportMenu();
	UFUNCTION(BlueprintCallable, Category = "Bully|Menu") void CloseTeleportMenu();
	UFUNCTION(BlueprintCallable, Category = "Bully|Menu") bool IsTeleportMenuOpen() const { return TeleportWidget.IsValid(); }

	/** Look settings, as sensitivity multipliers (ABullyCharacter::MinLookSens..MaxLookSens). Applied live; Save persists. */
	float GetLookX() const;
	float GetLookY() const;
	bool GetInvertY() const;
	void SetLookX(float V);
	void SetLookY(float V);
	void SetInvertY(bool bInvert);
	void SaveLook();

private:
	void ApplyLook(float X, float Y, bool bInvert);
	/** Capture + lock the mouse to the viewport for game-only look (windowed -game on Mac needs this set explicitly). */
	void ApplyGameInputMode();
	void RunSettingsSmoke();

	TSharedPtr<SWidget> SettingsWidget;
	TSharedPtr<SWidget> TeleportWidget;
	double LastToggleRealTime = 0.0;
	FTSTicker::FDelegateHandle SmokeTicker;
	float SmokeTime = 0.f;
	int32 SmokeStep = 0;
};
