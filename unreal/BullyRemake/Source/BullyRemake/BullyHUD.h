// OWNER: mission-fw
// Mission HUD: pure-C++ Slate overlay (same approach as the Esc settings menu: no editor-authored UMG asset).
//   - objective panel (top left): mission title, "n / N", current objective text, brief gold flash when it changes
//   - waypoint marker toward the active objective: a diamond + distance when the target is on screen, an edge
//     arrow pointing the way when it is off screen or behind the camera
//   - button prompt (lower centre): "[E] Open the door"
//   - hint toast (under the objective) and a centre banner ("MISSION COMPLETE" / "MISSION FAILED")
// FBullyHUD holds the state; the widget only reads it. UMissionSubsystem is the only writer.
#pragma once

#include "CoreMinimal.h"

class SWidget;
class UWorld;
class APlayerController;

/** Plain data the Slate widgets read each frame. */
struct FBullyHUDState
{
	FString MissionTitle;
	FString ObjectiveText;
	int32 StageNumber = 0;      // 1-based, 0 = no mission
	int32 StageCount = 0;
	float ObjectiveFlash = 0.f; // seconds left of the "new objective" highlight

	bool bBlip = false;
	FVector BlipWorld = FVector::ZeroVector;

	FString PromptKey = TEXT("E");
	FString PromptText;         // empty = no prompt

	FString HintText;
	float HintLeft = 0.f;

	FString BannerText;
	FString BannerSub;
	float BannerLeft = 0.f;
	float BannerTotal = 1.f;
	bool bBannerBad = false;

	FString WalletText;         // reward loop: running "$money" (+ respect) shown top-right; empty = hidden
	float WalletFlash = 0.f;    // seconds left of the gold flash when it changes

	// cinematic (in-engine cutscenes): letterbox bars slide in/out; a dialogue line reads at the bottom
	float Letterbox = 0.f;       // current bar coverage 0..1 (eased toward LetterboxTarget each tick)
	float LetterboxTarget = 0.f; // 0 = retracted, 1 = full cinematic bars
	FString SpeakerText;         // dialogue speaker label (upper-cased on screen); empty = no speaker row
	FString DialogueText;        // current spoken line
	float DialogueLeft = 0.f;    // seconds left on the current line (0 = dialogue box hidden)
	float DialogueTotal = 1.f;

	TWeakObjectPtr<APlayerController> Controller;
};

class FBullyHUD : public TSharedFromThis<FBullyHUD>
{
public:
	FBullyHUD();

	/** Adds the overlay to the game viewport. Returns false (and logs why) if there is no viewport / Slate (e.g. a commandlet). */
	bool Show(UWorld* World);
	void Hide();
	bool IsShown() const { return Root.IsValid(); }

	void Tick(float Dt);

	void SetObjective(const FString& MissionTitle, int32 StageNumber, int32 StageCount, const FString& Text);
	void ClearObjective();
	void SetBlip(bool bOn, const FVector& World = FVector::ZeroVector);
	void SetPrompt(const FString& Text, const FString& Key = TEXT("E"));
	void ShowHint(const FString& Text, float Seconds);
	void ShowBanner(const FString& Text, const FString& Sub, float Seconds, bool bBad);
	/** Reward loop: set the running wallet readout (top-right). bFlash gives it a brief gold highlight. */
	void SetWallet(int32 Money, int32 Respect, bool bFlash);
	/** Cinematic: slide the letterbox bars in (bOn) or out. */
	void SetLetterbox(bool bOn);
	/** Cinematic: show one cutscene dialogue line (speaker label + text) for Seconds. */
	void Say(const FString& Speaker, const FString& Line, float Seconds);

	const FBullyHUDState& GetState() const { return State.Get(); }

private:
	TSharedRef<FBullyHUDState> State;
	TSharedPtr<SWidget> Root;
	TWeakObjectPtr<UWorld> WorldRef;
};
