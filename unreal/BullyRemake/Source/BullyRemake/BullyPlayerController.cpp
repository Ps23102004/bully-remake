// OWNER: settings-menu
#include "BullyPlayerController.h"

#include "BullyCharacter.h"
#include "Containers/Ticker.h"
#include "NPCChatSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameModeBase.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullySettings, Log, All);

namespace
{
	constexpr float MinSens = ABullyCharacter::MinLookSens;
	constexpr float MaxSens = ABullyCharacter::MaxLookSens;
	float SensToSlider(float S) { return FMath::Clamp((S - MinSens) / (MaxSens - MinSens), 0.f, 1.f); }
	float SliderToSens(float V) { return MinSens + FMath::Clamp(V, 0.f, 1.f) * (MaxSens - MinSens); }
}

// ---------------------------------------------------------------------------------------------------------------
// Slate menu

class SBullySettingsMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBullySettingsMenu) {}
		SLATE_ARGUMENT(TWeakObjectPtr<ABullyPlayerController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		PC = InArgs._Controller;
		const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle("Bold", 30);
		const FSlateFontInfo Label = FCoreStyle::GetDefaultFontStyle("Regular", 16);
		const FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Regular", 13);
		const FLinearColor Text(0.95f, 0.93f, 0.88f, 1.f);
		const FLinearColor Dim(0.65f, 0.65f, 0.68f, 1.f);
		const FLinearColor Accent(0.95f, 0.72f, 0.20f, 1.f);

		auto SliderRow = [&](const FText& Name, TFunction<float()> Get, TFunction<void(float)> Set) -> TSharedRef<SWidget>
		{
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f)
					[
						SNew(STextBlock).Text(Name).Font(Label).ColorAndOpacity(Text)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock).Font(Label).ColorAndOpacity(Accent)
						.Text_Lambda([Get]() { return FText::FromString(FString::Printf(TEXT("%.2fx"), Get())); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).HeightOverride(26.f)
					[
						SNew(SSlider)
						.Value_Lambda([Get]() { return SensToSlider(Get()); })
						.OnValueChanged_Lambda([Set](float V) { Set(SliderToSens(V)); })
						.OnMouseCaptureEnd_Lambda([this]() { if (PC.IsValid()) { PC->SaveLook(); } })
						.SliderBarColor(FLinearColor(0.25f, 0.25f, 0.28f, 1.f))
						.SliderHandleColor(Accent)
						.StepSize(0.005f)
					]
				];
		};

		ChildSlot
		[
			// dim the world a little and eat clicks behind the panel
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.35f))
			.HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(560.f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FLinearColor(0.04f, 0.04f, 0.06f, 0.88f))
					.Padding(FMargin(32.f, 24.f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Settings"))).Font(Title).ColorAndOpacity(Text)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Look sensitivity (1.0x is the fastest, 0.05x the slowest)"))).Font(Small).ColorAndOpacity(Dim)
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SliderRow(FText::FromString(TEXT("Look sensitivity - horizontal (left/right)")),
								[this]() { return PC.IsValid() ? PC->GetLookX() : 0.f; },
								[this](float V) { if (PC.IsValid()) { PC->SetLookX(V); } })
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SliderRow(FText::FromString(TEXT("Look sensitivity - vertical (up/down)")),
								[this]() { return PC.IsValid() ? PC->GetLookY() : 0.f; },
								[this](float V) { if (PC.IsValid()) { PC->SetLookY(V); } })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 18, 0, 0)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this]() { return (PC.IsValid() && PC->GetInvertY()) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState S)
							{
								if (PC.IsValid()) { PC->SetInvertY(S == ECheckBoxState::Checked); PC->SaveLook(); }
							})
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("  Invert vertical look"))).Font(Label).ColorAndOpacity(Text)
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 26, 0, 0).HAlign(HAlign_Center)
						[
							SNew(SButton)
							.OnClicked_Lambda([this]() { if (PC.IsValid()) { PC->CloseSettingsMenu(); } return FReply::Handled(); })
							.ContentPadding(FMargin(40.f, 8.f))
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("Resume"))).Font(Label)
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0).HAlign(HAlign_Center)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Press Esc to resume"))).Font(Small).ColorAndOpacity(Dim)
						]
					]
				]
			]
		];
	}

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geo, const FKeyEvent& Event) override
	{
		if (Event.GetKey() == EKeys::Escape)
		{
			if (PC.IsValid()) { PC->CloseSettingsMenu(); }
			return FReply::Handled();
		}
		return SCompoundWidget::OnKeyDown(Geo, Event);
	}

private:
	TWeakObjectPtr<ABullyPlayerController> PC;
};

// ---------------------------------------------------------------------------------------------------------------
// controller

void ABullyPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (InputComponent)
	{
		FInputKeyBinding& B = InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &ABullyPlayerController::ToggleSettingsMenu);
		B.bExecuteWhenPaused = true;
		B.bConsumeInput = true;
		// ai-npc: T opens/closes the NPC chat. While the chat panel has keyboard focus Slate eats the key,
		// so typing a "t" into the input box does not re-trigger this.
		InputComponent->BindKey(EKeys::T, IE_Pressed, this, &ABullyPlayerController::NPCChat);
	}
}

void ABullyPlayerController::NPCChat()
{
	if (UWorld* W = GetWorld())
	{
		if (UNPCChatSubsystem* Chat = W->GetSubsystem<UNPCChatSubsystem>())
		{
			Chat->Toggle(this);
		}
	}
}

void ABullyPlayerController::ApplyGameInputMode()
{
	if (!IsLocalController())
	{
		return;
	}
	// Windowed -game on macOS does not grab the mouse from engine defaults, so look/clicks feel dead until the
	// viewport is told to capture permanently and lock the OS cursor (recentred each frame -> continuous delta).
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->SetMouseCaptureMode(EMouseCaptureMode::CapturePermanently);
		GEngine->GameViewport->SetMouseLockMode(EMouseLockMode::LockAlways);
		GEngine->GameViewport->SetHideCursorDuringCapture(true);
	}
	FInputModeGameOnly Mode;
	Mode.SetConsumeCaptureMouseDown(true);
	SetInputMode(Mode);
	SetShowMouseCursor(false);
	if (FSlateApplication::IsInitialized() && GEngine && GEngine->GameViewport)
	{
		// Pull focus to the viewport so the first mouse move is captured without needing a click that macOS eats.
		FSlateApplication::Get().SetUserFocusToGameViewport(0);
	}
}

void ABullyPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// Grab the mouse for look now, and once more next tick in case the game viewport is not live yet at BeginPlay.
	ApplyGameInputMode();
	if (UWorld* W = GetWorld())
	{
		FTimerHandle Tmp;
		W->GetTimerManager().SetTimer(Tmp, this, &ABullyPlayerController::ApplyGameInputMode, 0.1f, false);
	}

	if (IsLocalController() && FParse::Param(FCommandLine::Get(), TEXT("BRSettingsSmoke")))
	{
		UE_LOG(LogBullySettings, Display, TEXT("BR_SETTINGS smoke armed (PC %s, pawn %s)"), *GetName(), GetPawn() ? *GetPawn()->GetName() : TEXT("none"));
		SmokeTicker = FTSTicker::GetCoreTicker().AddTicker(TEXT("BullySettingsSmoke"), 0.f, [this](float Dt)
		{
			SmokeTime += Dt;
			RunSettingsSmoke();
			return SmokeStep < 100;
		});
	}
}

void ABullyPlayerController::EndPlay(const EEndPlayReason::Type Reason)
{
	FTSTicker::GetCoreTicker().RemoveTicker(SmokeTicker);
	if (IsSettingsMenuOpen())
	{
		CloseSettingsMenu();
	}
	Super::EndPlay(Reason);
}

void ABullyPlayerController::ToggleSettingsMenu()
{
	// Esc can arrive twice for one press (widget key handler + controller binding); ignore the echo
	const double Now = FPlatformTime::Seconds();
	if (Now - LastToggleRealTime < 0.2)
	{
		return;
	}
	LastToggleRealTime = Now;
	if (IsSettingsMenuOpen()) { CloseSettingsMenu(); } else { OpenSettingsMenu(); }
}

void ABullyPlayerController::OpenSettingsMenu()
{
	if (IsSettingsMenuOpen() || !IsLocalController())
	{
		return;
	}
	if (!GEngine || !GEngine->GameViewport || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogBullySettings, Warning, TEXT("BR_SETTINGS open refused: no game viewport / Slate (viewport=%d slate=%d)"),
			GEngine && GEngine->GameViewport ? 1 : 0, FSlateApplication::IsInitialized() ? 1 : 0);
		return;
	}
	LastToggleRealTime = FPlatformTime::Seconds();
	TSharedRef<SBullySettingsMenu> Menu = SNew(SBullySettingsMenu).Controller(this);
	SettingsWidget = Menu;
	GEngine->GameViewport->AddViewportWidgetContent(Menu, 100);

	// Stop the game (door fades / walkers simply freeze and resume; no state is touched), ignore stray look/move input,
	// and hand the mouse back: cursor shown, Game-and-UI, NOT locked to the viewport so other apps stay clickable.
	FlushPressedKeys();
	SetIgnoreLookInput(true);
	SetIgnoreMoveInput(true);
	SetPause(true);
	FInputModeGameAndUI Mode;
	Mode.SetWidgetToFocus(Menu);
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Mode.SetHideCursorDuringCapture(false);
	SetInputMode(Mode);
	SetShowMouseCursor(true);
	UE_LOG(LogBullySettings, Display, TEXT("BR_SETTINGS open: cursor=%d paused=%d lockMouse=DoNotLock inputMode=GameAndUI lookIgnored=%d X=%.3f Y=%.3f invertY=%d"),
		bShowMouseCursor ? 1 : 0, IsPaused() ? 1 : 0, IsLookInputIgnored() ? 1 : 0, GetLookX(), GetLookY(), GetInvertY() ? 1 : 0);
}

void ABullyPlayerController::CloseSettingsMenu()
{
	if (!IsSettingsMenuOpen())
	{
		return;
	}
	LastToggleRealTime = FPlatformTime::Seconds();
	SaveLook();
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(SettingsWidget.ToSharedRef());
	}
	SettingsWidget.Reset();
	SetPause(false);
	SetIgnoreLookInput(false);
	SetIgnoreMoveInput(false);
	FlushPressedKeys();
	// Re-grab and lock the mouse exactly as at startup, so look works again after the menu closes.
	ApplyGameInputMode();
	UE_LOG(LogBullySettings, Display, TEXT("BR_SETTINGS close: cursor=%d paused=%d inputMode=GameOnly lookIgnored=%d X=%.3f Y=%.3f invertY=%d"),
		bShowMouseCursor ? 1 : 0, IsPaused() ? 1 : 0, IsLookInputIgnored() ? 1 : 0, GetLookX(), GetLookY(), GetInvertY() ? 1 : 0);
}

// ---- look settings

float ABullyPlayerController::GetLookX() const
{
	const ABullyCharacter* C = Cast<ABullyCharacter>(GetPawn());
	return C ? C->LookSensitivityX : GetDefault<ABullyCharacter>()->LookSensitivityX;
}
float ABullyPlayerController::GetLookY() const
{
	const ABullyCharacter* C = Cast<ABullyCharacter>(GetPawn());
	return C ? C->LookSensitivityY : GetDefault<ABullyCharacter>()->LookSensitivityY;
}
bool ABullyPlayerController::GetInvertY() const
{
	const ABullyCharacter* C = Cast<ABullyCharacter>(GetPawn());
	return C ? C->bInvertLookY : GetDefault<ABullyCharacter>()->bInvertLookY;
}

void ABullyPlayerController::ApplyLook(float X, float Y, bool bInvert)
{
	if (ABullyCharacter* C = Cast<ABullyCharacter>(GetPawn()))
	{
		C->SetLookSettings(X, Y, bInvert);
	}
	else
	{
		ABullyCharacter* CDO = GetMutableDefault<ABullyCharacter>();
		CDO->SetLookSettings(X, Y, bInvert);
	}
}
void ABullyPlayerController::SetLookX(float V) { ApplyLook(V, GetLookY(), GetInvertY()); }
void ABullyPlayerController::SetLookY(float V) { ApplyLook(GetLookX(), V, GetInvertY()); }
void ABullyPlayerController::SetInvertY(bool bInvert) { ApplyLook(GetLookX(), GetLookY(), bInvert); }

void ABullyPlayerController::SaveLook()
{
	if (const ABullyCharacter* C = Cast<ABullyCharacter>(GetPawn()))
	{
		C->SaveLookSettings();
	}
	else
	{
		GetDefault<ABullyCharacter>()->SaveLookSettings();
	}
}

// ---- headless smoke (-BRSettingsSmoke): open, drive the same setters the sliders call, close, reopen via Esc-path toggle

void ABullyPlayerController::RunSettingsSmoke()
{
	auto Log = [this](const TCHAR* What)
	{
		UE_LOG(LogBullySettings, Display, TEXT("BR_SETTINGS smoke %s: menuOpen=%d cursor=%d paused=%d X=%.3f Y=%.3f invertY=%d"),
			What, IsSettingsMenuOpen() ? 1 : 0, bShowMouseCursor ? 1 : 0, IsPaused() ? 1 : 0, GetLookX(), GetLookY(), GetInvertY() ? 1 : 0);
	};
	if (SmokeStep == 0 && SmokeTime > 4.f) { Log(TEXT("before-open")); ToggleSettingsMenu(); Log(TEXT("after-open")); SmokeStep = 1; }
	else if (SmokeStep == 1 && SmokeTime > 4.5f)
	{
		// slider drag equivalents: 0..1 slider positions through the same mapping the widget uses
		SetLookX(SliderToSens(0.25f)); SetLookY(SliderToSens(0.75f)); SetInvertY(true);
		Log(TEXT("after-slider-changes"));
		SaveLook();
		SmokeStep = 2;
	}
	else if (SmokeStep == 2 && SmokeTime > 5.f) { LastToggleRealTime = 0.0; ToggleSettingsMenu(); Log(TEXT("after-close")); SmokeStep = 3; }
	else if (SmokeStep == 3 && SmokeTime > 5.5f) { LastToggleRealTime = 0.0; ToggleSettingsMenu(); Log(TEXT("reopened")); SmokeStep = 4; }
	else if (SmokeStep == 4 && SmokeTime > 6.f) { LastToggleRealTime = 0.0; ToggleSettingsMenu(); Log(TEXT("reclosed")); SmokeStep = 100; }
}
