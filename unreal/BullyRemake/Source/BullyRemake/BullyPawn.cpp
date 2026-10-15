// OWNER: builder-B2 (doors)
#include "BullyPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyPawn, Log, All);

ABullyPawn::ABullyPawn()
{
	PrimaryActorTick.bCanEverTick = false;

	// Capsule: 30 cm radius, 1.8 m tall.
	GetCapsuleComponent()->InitCapsuleSize(30.f, 90.f);

	// Controller rotation drives the camera only; the body turns toward the movement direction.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->bOrientRotationToMovement = true;
	Move->RotationRate = FRotator(0.f, 540.f, 0.f);
	Move->MaxWalkSpeed = WalkSpeed;
	Move->JumpZVelocity = JumpSpeed;
	Move->AirControl = 0.3f;
	Move->MaxAcceleration = 2048.f;
	Move->BrakingDecelerationWalking = 2048.f;
	Move->GroundFriction = 8.f;
	Move->MaxStepHeight = 45.f;          // original stairs are real 15-20 cm steps; 45 is the UE default
	Move->SetWalkableFloorAngle(50.f);
	Move->NavAgentProps.bCanCrouch = false;

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = CameraDistance;
	CameraBoom->TargetOffset = FVector(0.f, 0.f, EyeHeight - 90.f);   // pivot at eye height (capsule centre is 90 cm up)
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->ProbeSize = 12.f;
	CameraBoom->bEnableCameraLag = false;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->SetFieldOfView(75.f);

	// Visible placeholder body from engine primitives (no content dependency): 60 cm cylinder + head sphere.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	BodyVis = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyVis"));
	BodyVis->SetupAttachment(RootComponent);
	BodyVis->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyVis->SetCastShadow(true);
	HeadVis = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeadVis"));
	HeadVis->SetupAttachment(RootComponent);
	HeadVis->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (CylinderFinder.Succeeded())
	{
		BodyVis->SetStaticMesh(CylinderFinder.Object);     // engine cylinder is 100 x 100 x 100 cm
		BodyVis->SetRelativeScale3D(FVector(0.55f, 0.55f, 1.35f));
		BodyVis->SetRelativeLocation(FVector(0.f, 0.f, -20.f));   // feet at -90, top at +47
	}
	if (SphereFinder.Succeeded())
	{
		HeadVis->SetStaticMesh(SphereFinder.Object);
		HeadVis->SetRelativeScale3D(FVector(0.34f));
		HeadVis->SetRelativeLocation(FVector(0.f, 0.f, 70.f));    // eye level, 160 cm above the feet
	}
}

void ABullyPawn::BeginPlay()
{
	Super::BeginPlay();
	SetRunning(false);
	if (bSnapToGroundOnBeginPlay)
	{
		// The PlayerStart is placed above the ground on purpose; drop the capsule onto the first
		// surface below it (complex collision) instead of free-falling through any gap.
		FHitResult Hit;
		FCollisionQueryParams P(SCENE_QUERY_STAT(BullyPawnSnap), /*bTraceComplex*/ true, this);
		const FVector Start = GetActorLocation() + FVector(0, 0, 50.f);
		const FVector End = Start - FVector(0, 0, 5000.f);
		if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, P))
		{
			SetActorLocation(Hit.ImpactPoint + FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f), false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogBullyPawn, Display, TEXT("BR_PAWN snapped to ground z=%.1f (%s)"), Hit.ImpactPoint.Z, *GetNameSafe(Hit.GetActor()));
		}
		else
		{
			UE_LOG(LogBullyPawn, Warning, TEXT("BR_PAWN ground snap MISSED at %s"), *GetActorLocation().ToString());
		}
	}
}

void ABullyPawn::SetRunning(bool bRun)
{
	bRunning = bRun;
	GetCharacterMovement()->MaxWalkSpeed = bRun ? RunSpeed : WalkSpeed;
}

void ABullyPawn::BuildInputAssets()
{
	if (MappingContext)
	{
		return;
	}
	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_BullyDefault"));
	MoveAction = NewObject<UInputAction>(this, TEXT("IA_Move"));
	MoveAction->ValueType = EInputActionValueType::Axis2D;
	LookAction = NewObject<UInputAction>(this, TEXT("IA_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	JumpAction = NewObject<UInputAction>(this, TEXT("IA_Jump"));
	JumpAction->ValueType = EInputActionValueType::Boolean;
	RunAction = NewObject<UInputAction>(this, TEXT("IA_Run"));
	RunAction->ValueType = EInputActionValueType::Boolean;

	auto Swizzle = [this]() { UInputModifierSwizzleAxis* M = NewObject<UInputModifierSwizzleAxis>(this); M->Order = EInputAxisSwizzle::YXZ; return M; };
	auto Negate = [this](bool X, bool Y, bool Z) { UInputModifierNegate* M = NewObject<UInputModifierNegate>(this); M->bX = X; M->bY = Y; M->bZ = Z; return M; };
	auto Deadzone = [this]() { UInputModifierDeadZone* M = NewObject<UInputModifierDeadZone>(this); M->LowerThreshold = 0.2f; return M; };

	// Move: W/S on Y, A/D on X (Axis2D = (right, forward)); left stick direct.
	MappingContext->MapKey(MoveAction, EKeys::D);
	MappingContext->MapKey(MoveAction, EKeys::A).Modifiers.Add(Negate(true, false, false));
	{ FEnhancedActionKeyMapping& W = MappingContext->MapKey(MoveAction, EKeys::W); W.Modifiers.Add(Swizzle()); }
	{ FEnhancedActionKeyMapping& S = MappingContext->MapKey(MoveAction, EKeys::S); S.Modifiers.Add(Swizzle()); S.Modifiers.Add(Negate(true, false, false)); }
	MappingContext->MapKey(MoveAction, EKeys::Gamepad_Left2D).Modifiers.Add(Deadzone());

	// Look: mouse delta, right stick (Y negated as in the stock templates so stick-up looks up).
	MappingContext->MapKey(LookAction, EKeys::Mouse2D).Modifiers.Add(Negate(false, true, false));
	{
		FEnhancedActionKeyMapping& R = MappingContext->MapKey(LookAction, EKeys::Gamepad_Right2D);
		R.Modifiers.Add(Deadzone());
		R.Modifiers.Add(Negate(false, true, false));
		UInputModifierScalar* Sc = NewObject<UInputModifierScalar>(this);
		Sc->Scalar = FVector(1.5f, 1.5f, 1.f);   // stick axes arrive per-frame-scaled; gamepad needs more gain than mouse deltas
		R.Modifiers.Add(Sc);
	}

	MappingContext->MapKey(JumpAction, EKeys::SpaceBar);
	MappingContext->MapKey(JumpAction, EKeys::Gamepad_FaceButton_Bottom);
	MappingContext->MapKey(RunAction, EKeys::LeftShift);
	MappingContext->MapKey(RunAction, EKeys::Gamepad_LeftThumbstick);
}

void ABullyPawn::PawnClientRestart()
{
	Super::PawnClientRestart();
	BuildInputAssets();
	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Sub = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Sub->AddMappingContext(MappingContext, 0);
		}
	}
}

void ABullyPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	BuildInputAssets();
	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EIC->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ABullyPawn::OnMove);
		EIC->BindAction(LookAction, ETriggerEvent::Triggered, this, &ABullyPawn::OnLook);
		EIC->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EIC->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
		EIC->BindAction(RunAction, ETriggerEvent::Started, this, &ABullyPawn::OnRunStarted);
		EIC->BindAction(RunAction, ETriggerEvent::Completed, this, &ABullyPawn::OnRunCompleted);
	}
	else
	{
		UE_LOG(LogBullyPawn, Error, TEXT("BR_PAWN input component is not an EnhancedInputComponent"));
	}
}

void ABullyPawn::OnMove(const FInputActionValue& Value)
{
	const FVector2D V = Value.Get<FVector2D>();
	if (!Controller)
	{
		return;
	}
	const FRotator Yaw(0.f, Controller->GetControlRotation().Yaw, 0.f);
	const FVector Fwd = FRotationMatrix(Yaw).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y);
	AddMovementInput(Fwd, V.Y);
	AddMovementInput(Right, V.X);
}

void ABullyPawn::OnLook(const FInputActionValue& Value)
{
	const FVector2D V = Value.Get<FVector2D>() * LookSensitivity;
	AddControllerYawInput(V.X);
	AddControllerPitchInput(V.Y);
}
