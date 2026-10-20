// OWNER: mission-fw
#include "BullyHUD.h"

#include "BullyWeaponComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyHUD, Log, All);

namespace
{
	const FLinearColor Gold(0.95f, 0.72f, 0.20f, 1.f);
	const FLinearColor Cream(0.95f, 0.93f, 0.88f, 1.f);
	const FLinearColor Dim(0.65f, 0.65f, 0.68f, 1.f);
	const FLinearColor Panel(0.03f, 0.03f, 0.05f, 0.72f);
	const FLinearColor Red(0.95f, 0.30f, 0.25f, 1.f);
}

// ---------------------------------------------------------------------------------------------------------------
// Waypoint marker: full-screen leaf widget that paints a diamond at the projected target, or an arrow on the screen
// edge pointing toward it.

class SBullyWaypoint : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SBullyWaypoint) {}
		SLATE_ARGUMENT(TSharedPtr<FBullyHUDState>, State)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		State = InArgs._State;
		SetCanTick(false);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1.f, 1.f); }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geo, const FSlateRect& Clip, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle& Style, bool bParentEnabled) const override
	{
		TSharedPtr<FBullyHUDState> S = State.Pin();
		if (!S.IsValid() || !S->bBlip) { return Layer; }
		APlayerController* PC = S->Controller.Get();
		if (!PC || !PC->PlayerCameraManager) { return Layer; }

		const FVector2f Size = Geo.GetLocalSize();
		if (Size.X < 4.f || Size.Y < 4.f) { return Layer; }
		const float Margin = 56.f;
		const FVector2f Center = Size * 0.5f;

		// camera-space direction (x forward, y right, z up) so behind-camera targets still point the right way
		const FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
		const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
		const FVector Rel = FRotationMatrix(CamRot).GetTransposed().TransformVector(S->BlipWorld - CamLoc);

		FVector2D Proj;
		bool bOnScreen = false;
		FVector2f Pos = Center;
		if (Rel.X > 1.f && PC->ProjectWorldLocationToScreen(S->BlipWorld + FVector(0, 0, 120.f), Proj, false))
		{
			Pos = FVector2f(float(Proj.X), float(Proj.Y)) / FMath::Max(Geo.Scale, 0.01f);
			bOnScreen = Pos.X > Margin && Pos.X < Size.X - Margin && Pos.Y > Margin && Pos.Y < Size.Y - Margin;
		}

		const float DistM = FVector::Dist(S->BlipWorld, PC->GetPawn() ? PC->GetPawn()->GetActorLocation() : CamLoc) / 100.f;
		const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
		const float Pulse = 0.85f + 0.15f * FMath::Sin(float(FPlatformTime::Seconds()) * 4.f);
		int32 L = Layer;

		auto Diamond = [&](const FVector2f& P, float Half, const FLinearColor& Col)
		{
			const FVector2f Sz(Half * 2.f, Half * 2.f);
			FSlateDrawElement::MakeRotatedBox(Out, L++, Geo.ToPaintGeometry(Sz, FSlateLayoutTransform(P - Sz * 0.5f)), White,
				ESlateDrawEffect::None, UE_PI * 0.25f, FVector2f(Half, Half), FSlateDrawElement::RelativeToElement, Col);
		};

		FVector2f LabelAt;
		if (bOnScreen)
		{
			Diamond(Pos, 13.f * Pulse, FLinearColor(0.f, 0.f, 0.f, 0.7f));
			Diamond(Pos, 10.f * Pulse, Gold);
			Diamond(Pos, 4.f, FLinearColor(0.05f, 0.05f, 0.07f, 1.f));
			LabelAt = Pos + FVector2f(0.f, 22.f);
		}
		else
		{
			// project the camera-relative direction onto the screen plane and clamp it to the edge rectangle
			FVector2f Dir(float(Rel.Y), float(-Rel.Z));
			if (Rel.X < 0.f && Dir.SizeSquared() < 1.f) { Dir = FVector2f(0.f, 1.f); }   // directly behind: point down
			if (Dir.SizeSquared() < 1e-4f) { Dir = FVector2f(0.f, -1.f); }
			Dir.Normalize();
			const FVector2f Half = Size * 0.5f - FVector2f(Margin, Margin);
			const float T = FMath::Min(Dir.X != 0.f ? Half.X / FMath::Abs(Dir.X) : 1e9f, Dir.Y != 0.f ? Half.Y / FMath::Abs(Dir.Y) : 1e9f);
			const FVector2f P = Center + Dir * T;
			const FVector2f Perp(-Dir.Y, Dir.X);
			const FVector2f Tip = P + Dir * 18.f, A = P - Dir * 8.f + Perp * 14.f, B = P - Dir * 8.f - Perp * 14.f;
			TArray<FVector2f> Pts = { A, Tip, B };
			FSlateDrawElement::MakeLines(Out, L++, Geo.ToPaintGeometry(), Pts, ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.75f), true, 8.f);
			FSlateDrawElement::MakeLines(Out, L++, Geo.ToPaintGeometry(), Pts, ESlateDrawEffect::None, Gold, true, 4.f);
			LabelAt = P - Dir * 30.f + FVector2f(0.f, 0.f);
		}

		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 13);
		const FString Txt = FString::Printf(TEXT("%d m"), FMath::RoundToInt(DistM));
		FSlateDrawElement::MakeText(Out, L++, Geo.ToPaintGeometry(FVector2f(80.f, 20.f), FSlateLayoutTransform(LabelAt - FVector2f(20.f, 0.f) + FVector2f(1.f, 1.f))), Txt, Font,
			ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.8f));
		FSlateDrawElement::MakeText(Out, L++, Geo.ToPaintGeometry(FVector2f(80.f, 20.f), FSlateLayoutTransform(LabelAt - FVector2f(20.f, 0.f))), Txt, Font,
			ESlateDrawEffect::None, Cream);
		return L;
	}

private:
	TWeakPtr<FBullyHUDState> State;
};

// ---------------------------------------------------------------------------------------------------------------
// HUD root

class SBullyHUDRoot : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBullyHUDRoot) {}
		SLATE_ARGUMENT(TSharedPtr<FBullyHUDState>, State)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		TSharedRef<FBullyHUDState> S = InArgs._State.ToSharedRef();
		const FSlateFontInfo Small = FCoreStyle::GetDefaultFontStyle("Bold", 11);
		const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle("Regular", 19);
		const FSlateFontInfo Hint = FCoreStyle::GetDefaultFontStyle("Regular", 15);
		const FSlateFontInfo Key = FCoreStyle::GetDefaultFontStyle("Bold", 20);
		const FSlateFontInfo Big = FCoreStyle::GetDefaultFontStyle("Bold", 38);
		const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");

		SetVisibility(EVisibility::HitTestInvisible);

		ChildSlot
		[
			SNew(SOverlay)
			// waypoint (full screen)
			+ SOverlay::Slot() [ SNew(SBullyWaypoint).State(S) ]

			// cinematic letterbox: black bars that slide in from top and bottom (S->Letterbox 0..1)
			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Top)
			[
				SNew(SBox).HeightOverride_Lambda([S]() { return S->Letterbox * 92.f; })
				.Visibility_Lambda([S]() { return S->Letterbox > 0.004f ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[ SNew(SBorder).BorderImage(White).BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 1.f)) ]
			]
			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Bottom)
			[
				SNew(SBox).HeightOverride_Lambda([S]() { return S->Letterbox * 92.f; })
				.Visibility_Lambda([S]() { return S->Letterbox > 0.004f ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[ SNew(SBorder).BorderImage(White).BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 1.f)) ]
			]

			// cutscene dialogue line, lower centre (sits just above the bottom letterbox bar)
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(FMargin(0.f, 0.f, 0.f, 116.f))
			[
				SNew(SBox).MaxDesiredWidth(760.f)
				.Visibility_Lambda([S]() { return S->DialogueLeft > 0.f ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[
					SNew(SBorder).BorderImage(White).Padding(FMargin(20.f, 12.f, 22.f, 14.f))
					.BorderBackgroundColor_Lambda([S]() { return FLinearColor(0.02f, 0.02f, 0.04f, 0.82f * FMath::Clamp(S->DialogueLeft * 3.f, 0.f, 1.f)); })
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Font(Small)
							.Visibility_Lambda([S]() { return S->SpeakerText.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
							.ColorAndOpacity_Lambda([S]() { FLinearColor C = Gold; C.A = FMath::Clamp(S->DialogueLeft * 3.f, 0.f, 1.f); return C; })
							.Text_Lambda([S]() { return FText::FromString(S->SpeakerText.ToUpper()); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
						[
							SNew(STextBlock).Font(Body).AutoWrapText(true)
							.ColorAndOpacity_Lambda([S]() { return FLinearColor(0.95f, 0.93f, 0.88f, FMath::Clamp(S->DialogueLeft * 3.f, 0.f, 1.f)); })
							.ShadowOffset(FVector2D(1, 1)).ShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.85f))
							.Text_Lambda([S]() { return FText::FromString(S->DialogueText); })
						]
					]
				]
			]

			// wallet readout (reward loop), top right
			+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(FMargin(28.f, 24.f))
			[
				SNew(SBox)
				.Visibility_Lambda([S]() { return S->WalletText.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
				[
					SNew(SBorder).BorderImage(White).BorderBackgroundColor(Panel).Padding(FMargin(14.f, 7.f, 16.f, 8.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock).Font(Small).ColorAndOpacity(Gold).Text(FText::FromString(TEXT("CASH")))
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
						[
							SNew(STextBlock).Font(Key)
							.ColorAndOpacity_Lambda([S]() { return S->WalletFlash > 0.f ? FLinearColor::White : Cream; })
							.Text_Lambda([S]() { return FText::FromString(S->WalletText); })
						]
					]
				]
			]

			// objective panel, top left
			+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(FMargin(28.f, 24.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBox).MaxDesiredWidth(520.f)
					.Visibility_Lambda([S]() { return S->ObjectiveText.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBorder).BorderImage(White).Padding(FMargin(2.f, 0.f))
							.BorderBackgroundColor_Lambda([S]() { return S->ObjectiveFlash > 0.f ? FLinearColor::White : Gold; })
							[ SNew(SBox).WidthOverride(2.f) ]
						]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SNew(SBorder).BorderImage(White).BorderBackgroundColor(Panel).Padding(FMargin(14.f, 9.f, 18.f, 10.f))
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(STextBlock).Font(Small).ColorAndOpacity(Gold)
									.Text_Lambda([S]()
									{
										return FText::FromString(FString::Printf(TEXT("%s   %d / %d"), *S->MissionTitle.ToUpper(), S->StageNumber, S->StageCount));
									})
								]
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
								[
									SNew(STextBlock).Font(Body).AutoWrapText(true)
									.ColorAndOpacity_Lambda([S]() { return S->ObjectiveFlash > 0.f ? FLinearColor(1.f, 0.93f, 0.62f, 1.f) : Cream; })
									.ShadowOffset(FVector2D(1, 1)).ShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.8f))
									.Text_Lambda([S]() { return FText::FromString(S->ObjectiveText); })
								]
							]
						]
					]
				]
				// hint toast
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
				[
					SNew(SBox).MaxDesiredWidth(520.f)
					.Visibility_Lambda([S]() { return S->HintLeft > 0.f ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					[
						SNew(SBorder).BorderImage(White).Padding(FMargin(14.f, 7.f))
						.BorderBackgroundColor_Lambda([S]() { return FLinearColor(0.03f, 0.03f, 0.05f, 0.6f * FMath::Clamp(S->HintLeft * 2.f, 0.f, 1.f)); })
						[
							SNew(STextBlock).Font(Hint).AutoWrapText(true)
							.ColorAndOpacity_Lambda([S]() { return FLinearColor(0.85f, 0.85f, 0.88f, FMath::Clamp(S->HintLeft * 2.f, 0.f, 1.f)); })
							.Text_Lambda([S]() { return FText::FromString(S->HintText); })
						]
					]
				]
			]

			// controls legend, bottom left (always on; hidden while cinematic bars are up)
			+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(FMargin(28.f, 0.f, 0.f, 22.f))
			[
				SNew(SBox)
				.Visibility_Lambda([S]() { return S->Letterbox > 0.004f ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
				[
					SNew(SBorder).BorderImage(White).BorderBackgroundColor(FLinearColor(0.03f, 0.03f, 0.05f, 0.45f)).Padding(FMargin(12.f, 6.f, 14.f, 7.f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Font(Small).ColorAndOpacity(Dim)
							.Text(FText::FromString(TEXT("MOVE WASD    WALK ALT    SPRINT SHIFT    JUMP SPACE    INTERACT E    MENU ESC")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
						[
							SNew(STextBlock).Font(Small).ColorAndOpacity(Dim)
							.Text(FText::FromString(TEXT("ATTACK/FIRE LMB    BLOCK hold Q    GRAPPLE F    LOCK-ON hold RMB    WEAPON wheel")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 3, 0, 0)
						[
							SNew(STextBlock).Font(Small).ColorAndOpacity(Dim)
							.Text(FText::FromString(TEXT("TELEPORT G    TALK T")))
						]
						// equipped weapon + ammo (slice-weapons)
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
						[
							SNew(STextBlock).Font(Small).ColorAndOpacity(Gold)
							.Visibility_Lambda([]() { return UBullyWeaponComponent::HudLine().IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
							.Text_Lambda([]() { return FText::FromString(UBullyWeaponComponent::HudLine()); })
						]
					]
				]
			]

			// button prompt, lower centre
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(FMargin(0.f, 0.f, 0.f, 170.f))
			[
				SNew(SBox)
				.Visibility_Lambda([S]() { return S->PromptText.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
				[
					SNew(SBorder).BorderImage(White).BorderBackgroundColor(Panel).Padding(FMargin(12.f, 8.f, 20.f, 8.f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SBorder).BorderImage(White).BorderBackgroundColor(Gold).Padding(FMargin(11.f, 3.f))
							[
								SNew(STextBlock).Font(Key).ColorAndOpacity(FLinearColor(0.05f, 0.05f, 0.07f, 1.f))
								.Text_Lambda([S]() { return FText::FromString(S->PromptKey); })
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(14, 0, 0, 0)
						[
							SNew(STextBlock).Font(Body).ColorAndOpacity(Cream)
							.Text_Lambda([S]() { return FText::FromString(S->PromptText); })
						]
					]
				]
			]

			// banner, upper centre
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(FMargin(0.f, 150.f, 0.f, 0.f))
			[
				SNew(SBox)
				.Visibility_Lambda([S]() { return S->BannerLeft > 0.f ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						SNew(STextBlock).Font(Big)
						.ColorAndOpacity_Lambda([S]()
						{
							const float A = FMath::Clamp(S->BannerLeft * 2.f, 0.f, 1.f) * FMath::Clamp((S->BannerTotal - S->BannerLeft) * 4.f, 0.f, 1.f);
							FLinearColor C = S->bBannerBad ? Red : Gold; C.A = A; return C;
						})
						.ShadowOffset(FVector2D(2, 2)).ShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.8f))
						.Text_Lambda([S]() { return FText::FromString(S->BannerText); })
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 4, 0, 0)
					[
						SNew(STextBlock).Font(Body)
						.ColorAndOpacity_Lambda([S]() { return FLinearColor(0.95f, 0.93f, 0.88f, FMath::Clamp(S->BannerLeft * 2.f, 0.f, 1.f)); })
						.Text_Lambda([S]() { return FText::FromString(S->BannerSub); })
					]
				]
			]
		];
	}
};

// ---------------------------------------------------------------------------------------------------------------

FBullyHUD::FBullyHUD() : State(MakeShared<FBullyHUDState>()) {}

bool FBullyHUD::Show(UWorld* World)
{
	if (IsShown()) { return true; }
	if (!World || !GEngine || !GEngine->GameViewport || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogBullyHUD, Warning, TEXT("BR_HUD show refused: no game viewport / Slate (world=%d viewport=%d slate=%d)"),
			World ? 1 : 0, GEngine && GEngine->GameViewport ? 1 : 0, FSlateApplication::IsInitialized() ? 1 : 0);
		return false;
	}
	WorldRef = World;
	State->Controller = World->GetFirstPlayerController();
	TSharedRef<SBullyHUDRoot> W = SNew(SBullyHUDRoot).State(State);
	Root = W;
	GEngine->GameViewport->AddViewportWidgetContent(W, 50);   // below the settings menu (100)
	UE_LOG(LogBullyHUD, Display, TEXT("BR_HUD constructed ok (objective panel, waypoint, prompt, hint, banner) controller=%s"),
		State->Controller.IsValid() ? *State->Controller->GetName() : TEXT("none yet"));
	return true;
}

void FBullyHUD::Hide()
{
	if (Root.IsValid())
	{
		if (GEngine && GEngine->GameViewport) { GEngine->GameViewport->RemoveViewportWidgetContent(Root.ToSharedRef()); }
		Root.Reset();
	}
}

void FBullyHUD::Tick(float Dt)
{
	FBullyHUDState& S = State.Get();
	if (!S.Controller.IsValid() && WorldRef.IsValid()) { S.Controller = WorldRef->GetFirstPlayerController(); }
	S.ObjectiveFlash = FMath::Max(0.f, S.ObjectiveFlash - Dt);
	S.HintLeft = FMath::Max(0.f, S.HintLeft - Dt);
	S.BannerLeft = FMath::Max(0.f, S.BannerLeft - Dt);
	S.WalletFlash = FMath::Max(0.f, S.WalletFlash - Dt);
	S.DialogueLeft = FMath::Max(0.f, S.DialogueLeft - Dt);
	// ease the letterbox toward its target (~0.35 s open/close); exponential so it's framerate-independent
	S.Letterbox = FMath::FInterpTo(S.Letterbox, S.LetterboxTarget, Dt, 9.f);
	if (FMath::Abs(S.Letterbox - S.LetterboxTarget) < 0.002f) { S.Letterbox = S.LetterboxTarget; }
}

void FBullyHUD::SetObjective(const FString& MissionTitle, int32 StageNumber, int32 StageCount, const FString& Text)
{
	FBullyHUDState& S = State.Get();
	S.MissionTitle = MissionTitle;
	S.StageNumber = StageNumber;
	S.StageCount = StageCount;
	S.ObjectiveText = Text;
	S.ObjectiveFlash = 1.6f;
	UE_LOG(LogBullyHUD, Display, TEXT("BR_HUD objective %d/%d: %s"), StageNumber, StageCount, *Text);
}

void FBullyHUD::SetWallet(int32 Money, int32 Respect, bool bFlash)
{
	FBullyHUDState& S = State.Get();
	S.WalletText = Respect > 0
		? FString::Printf(TEXT("$%d   %d REP"), Money, Respect)
		: FString::Printf(TEXT("$%d"), Money);
	if (bFlash) { S.WalletFlash = 1.4f; }
}

void FBullyHUD::ClearObjective()
{
	FBullyHUDState& S = State.Get();
	S.ObjectiveText.Reset(); S.MissionTitle.Reset(); S.StageNumber = 0; S.StageCount = 0; S.bBlip = false; S.PromptText.Reset();
	// retract any cinematic furniture so a mission hand-off / end never leaves bars or a line stuck on screen
	S.LetterboxTarget = 0.f; S.DialogueLeft = 0.f; S.SpeakerText.Reset(); S.DialogueText.Reset();
}

void FBullyHUD::SetBlip(bool bOn, const FVector& World)
{
	FBullyHUDState& S = State.Get();
	S.bBlip = bOn;
	if (bOn) { S.BlipWorld = World; }
}

void FBullyHUD::SetPrompt(const FString& Text, const FString& Key)
{
	FBullyHUDState& S = State.Get();
	if (S.PromptText != Text) { UE_LOG(LogBullyHUD, Display, TEXT("BR_HUD prompt: %s"), Text.IsEmpty() ? TEXT("(none)") : *FString::Printf(TEXT("[%s] %s"), *Key, *Text)); }
	S.PromptText = Text;
	S.PromptKey = Key;
}

void FBullyHUD::ShowHint(const FString& Text, float Seconds)
{
	FBullyHUDState& S = State.Get();
	S.HintText = Text;
	S.HintLeft = Seconds;
	UE_LOG(LogBullyHUD, Display, TEXT("BR_HUD hint (%.1fs): %s"), Seconds, *Text);
}

void FBullyHUD::ShowBanner(const FString& Text, const FString& Sub, float Seconds, bool bBad)
{
	FBullyHUDState& S = State.Get();
	S.BannerText = Text; S.BannerSub = Sub; S.BannerLeft = Seconds; S.BannerTotal = Seconds; S.bBannerBad = bBad;
	UE_LOG(LogBullyHUD, Display, TEXT("BR_HUD banner: %s | %s"), *Text, *Sub);
}

void FBullyHUD::SetLetterbox(bool bOn)
{
	State.Get().LetterboxTarget = bOn ? 1.f : 0.f;
	UE_LOG(LogBullyHUD, Display, TEXT("BR_HUD letterbox %s"), bOn ? TEXT("on") : TEXT("off"));
}

void FBullyHUD::Say(const FString& Speaker, const FString& Line, float Seconds)
{
	FBullyHUDState& S = State.Get();
	S.SpeakerText = Speaker;
	S.DialogueText = Line;
	S.DialogueLeft = Seconds;
	S.DialogueTotal = FMath::Max(0.01f, Seconds);
	UE_LOG(LogBullyHUD, Display, TEXT("BR_HUD say (%.1fs) %s: %s"), Seconds, Speaker.IsEmpty() ? TEXT("-") : *Speaker, *Line);
}
