// OWNER: slice-vehicles
#include "BullyVehicle.h"

#include "BullyCharacter.h"
#include "BullyCombatComponent.h"
#include "BullyRagdollComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyVehicle, Log, All);

int32 ABullyVehicle::TotalMounts = 0;
int32 ABullyVehicle::TotalDismounts = 0;
int32 ABullyVehicle::TotalRunOvers = 0;

namespace
{
	double GLastRideToggle = -100.0;   // one E press must not both dismount and re-mount
	constexpr float StepUp = 25.f;     // curb height the ride climbs without a ramp

	bool ReadVec(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (!O->TryGetArrayField(Key, A) || A->Num() != 3) { return false; }
		Out = FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
		return true;
	}

	float Num(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, float Default)
	{
		double N = 0.0;
		return O->TryGetNumberField(Key, N) ? float(N) : Default;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// config

const TArray<FBullyVehicleDef>& FBullyVehicleDef::All()
{
	static TArray<FBullyVehicleDef> Cache;
	static bool bLoaded = false;
	if (bLoaded) { return Cache; }
	bLoaded = true;
	FString Text;
	const FString Path = FPaths::ProjectConfigDir() / TEXT("Vehicles.json");
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root)
		|| !Root.IsValid() || !Root->TryGetArrayField(TEXT("vehicles"), Arr))
	{
		UE_LOG(LogBullyVehicle, Warning, TEXT("BR_VEHICLE cannot read %s"), *Path);
		return Cache;
	}
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
		FString S;
		if (!O.IsValid() || !O->TryGetStringField(TEXT("id"), S)) { continue; }
		FBullyVehicleDef D;
		D.Id = FName(*S);
		D.Name = O->TryGetStringField(TEXT("name"), S) ? S : D.Id.ToString();
		ReadVec(O, TEXT("half_extent"), D.HalfExtent);
		D.RideHeight = Num(O, TEXT("ride_height"), D.RideHeight);
		D.MaxSpeed = Num(O, TEXT("max_speed"), D.MaxSpeed);
		D.Accel = Num(O, TEXT("accel"), D.Accel);
		D.Brake = Num(O, TEXT("brake"), D.Brake);
		D.MaxReverse = Num(O, TEXT("max_reverse"), D.MaxReverse);
		D.Coast = Num(O, TEXT("coast"), D.Coast);
		D.TurnRate = Num(O, TEXT("turn_rate"), D.TurnRate);
		D.TurnFullSpeed = FMath::Max(1.f, Num(O, TEXT("turn_full_speed"), D.TurnFullSpeed));
		ReadVec(O, TEXT("seat_offset"), D.SeatOffset);
		O->TryGetBoolField(TEXT("rider_stands"), D.bRiderStands);
		D.CameraDistance = Num(O, TEXT("camera_distance"), D.CameraDistance);
		D.CameraHeight = Num(O, TEXT("camera_height"), D.CameraHeight);
		FVector C;
		if (ReadVec(O, TEXT("color"), C)) { D.Color = FLinearColor(C.X, C.Y, C.Z); }
		D.Wheels = FMath::Clamp(int32(Num(O, TEXT("wheels"), D.Wheels)), 0, 4);
		D.WheelRadius = Num(O, TEXT("wheel_radius"), D.WheelRadius);
		O->TryGetBoolField(TEXT("cabin"), D.bCabin);
		O->TryGetStringField(TEXT("mesh"), D.MeshPath);
		ReadVec(O, TEXT("mesh_scale"), D.MeshScale);
		ReadVec(O, TEXT("mesh_offset_cm"), D.MeshOffset);
		D.MeshYaw = Num(O, TEXT("mesh_yaw"), 0.f);
		Cache.Add(D);
	}
	UE_LOG(LogBullyVehicle, Display, TEXT("BR_VEHICLE %d ride type(s) loaded from %s"), Cache.Num(), *Path);
	return Cache;
}

const FBullyVehicleDef* FBullyVehicleDef::Find(FName Id)
{
	for (const FBullyVehicleDef& D : All()) { if (D.Id == Id) { return &D; } }
	return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// actor

ABullyVehicle::ABullyVehicle()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessAI = EAutoPossessAI::Disabled;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;

	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetCollisionProfileName(TEXT("Vehicle"));
	Box->SetCanEverAffectNavigation(false);
	RootComponent = Box;

	auto Part = [this](const TCHAR* Name)
	{
		UStaticMeshComponent* M = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		M->SetupAttachment(Box);
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->SetCanEverAffectNavigation(false);
		return M;
	};
	Body = Part(TEXT("Body"));
	Cabin = Part(TEXT("Cabin"));
	WheelMeshes.Add(Part(TEXT("Wheel0")));
	WheelMeshes.Add(Part(TEXT("Wheel1")));
	WheelMeshes.Add(Part(TEXT("Wheel2")));
	WheelMeshes.Add(Part(TEXT("Wheel3")));

	Boom = CreateDefaultSubobject<USpringArmComponent>(TEXT("Boom"));
	Boom->SetupAttachment(Box);
	Boom->bUsePawnControlRotation = false;
	Boom->bInheritPitch = false;
	Boom->bInheritRoll = false;
	Boom->bInheritYaw = true;
	Boom->bEnableCameraLag = true;
	Boom->CameraLagSpeed = 8.f;
	Boom->bEnableCameraRotationLag = true;
	Boom->CameraRotationLagSpeed = 6.f;
	Boom->bDoCollisionTest = true;

	Cam = CreateDefaultSubobject<UCameraComponent>(TEXT("Cam"));
	Cam->SetupAttachment(Boom, USpringArmComponent::SocketName);
	Cam->bUsePawnControlRotation = false;
}

ABullyVehicle* ABullyVehicle::SpawnVehicle(UWorld* World, FName Id, const FVector& Location, float Yaw)
{
	const FBullyVehicleDef* D = FBullyVehicleDef::Find(Id);
	if (!World || !D) { UE_LOG(LogBullyVehicle, Warning, TEXT("BR_VEHICLE unknown ride '%s'"), *Id.ToString()); return nullptr; }
	FVector At = Location;
	FHitResult Hit;
	FCollisionObjectQueryParams Obj(ECC_WorldStatic);
	if (World->LineTraceSingleByObjectType(Hit, Location + FVector(0, 0, 500), Location - FVector(0, 0, 3000), Obj))
	{
		At.Z = Hit.ImpactPoint.Z + D->RideHeight;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	ABullyVehicle* V = World->SpawnActor<ABullyVehicle>(ABullyVehicle::StaticClass(), At, FRotator(0.f, Yaw, 0.f), SP);
	if (V)
	{
		V->Configure(*D);
		UE_LOG(LogBullyVehicle, Display, TEXT("BR_VEHICLE spawned %s at %s yaw %.0f"), *D->Name, *V->GetActorLocation().ToCompactString(), Yaw);
	}
	return V;
}

void ABullyVehicle::Configure(const FBullyVehicleDef& D)
{
	Def = D;
	Box->SetBoxExtent(D.HalfExtent);
	Boom->TargetArmLength = D.CameraDistance;
	Boom->SocketOffset = FVector(0.f, 0.f, D.CameraHeight);
	Boom->SetRelativeRotation(FRotator(-10.f, 0.f, 0.f));

	// A local model replaces the primitive look entirely.
	if (!D.MeshPath.IsEmpty())
	{
		if (UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, *D.MeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Body->SetStaticMesh(M);
			Body->SetRelativeScale3D(D.MeshScale);
			Body->SetRelativeLocationAndRotation(D.MeshOffset, FRotator(0.f, D.MeshYaw, 0.f));
			Cabin->SetVisibility(false);
			for (UStaticMeshComponent* W : WheelMeshes) { W->SetVisibility(false); }
			return;
		}
		UE_LOG(LogBullyVehicle, Warning, TEXT("BR_VEHICLE %s: mesh %s not found, using the primitive look"), *D.Name, *D.MeshPath);
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cyl = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	auto Tint = [this, Base](UStaticMeshComponent* C, const FLinearColor& Col)
	{
		if (!Base) { return; }
		UMaterialInstanceDynamic* MI = UMaterialInstanceDynamic::Create(Base, this);
		MI->SetVectorParameterValue(TEXT("Color"), Col);
		C->SetMaterial(0, MI);
	};

	const FVector E = D.HalfExtent;
	const float R = D.WheelRadius;
	const float Ground = -D.RideHeight;          // ground plane in actor space
	// body: a slab riding on the axles
	const float BodyHalfZ = FMath::Max(3.f, E.Z * (D.bRiderStands ? 0.25f : 0.45f));
	Body->SetStaticMesh(Cube);
	Body->SetRelativeScale3D(FVector(E.X * 2.f / 100.f, E.Y * (D.Wheels == 2 ? 1.f : 2.f) / 100.f, BodyHalfZ * 2.f / 100.f));
	Body->SetRelativeLocation(FVector(0.f, 0.f, Ground + R * (D.Wheels == 2 ? 1.6f : 1.f) + BodyHalfZ));
	Tint(Body, D.Color);

	Cabin->SetVisibility(D.bCabin);
	if (D.bCabin)
	{
		Cabin->SetStaticMesh(Cube);
		Cabin->SetRelativeScale3D(FVector(E.X * 1.1f / 100.f, E.Y * 1.8f / 100.f, E.Z * 1.0f / 100.f));
		Cabin->SetRelativeLocation(FVector(-E.X * 0.12f, 0.f, Ground + R + BodyHalfZ * 2.f + E.Z * 0.5f));
		Tint(Cabin, D.Color * 0.6f + FLinearColor(0.25f, 0.28f, 0.32f) * 0.4f);
	}

	const float Width = D.Wheels == 2 ? 8.f : FMath::Clamp(R * 0.6f, 4.f, 26.f);
	for (int32 i = 0; i < WheelMeshes.Num(); ++i)
	{
		UStaticMeshComponent* W = WheelMeshes[i];
		const bool bUsed = i < D.Wheels;
		W->SetVisibility(bUsed);
		if (!bUsed) { continue; }
		W->SetStaticMesh(Cyl);
		W->SetRelativeScale3D(FVector(R * 2.f / 100.f, R * 2.f / 100.f, Width / 100.f));
		const float X = (i % 2 == 0 ? 1.f : -1.f) * E.X * (D.Wheels == 2 ? 0.78f : 0.7f);
		const float Y = D.Wheels == 2 ? 0.f : (i < 2 ? 1.f : -1.f) * (E.Y * (D.bRiderStands ? 0.7f : 1.f) + Width * 0.5f);
		W->SetRelativeLocationAndRotation(FVector(X, Y, Ground + R), FRotator(0.f, 0.f, 90.f));   // cylinder axis -> side
		Tint(W, FLinearColor(0.04f, 0.04f, 0.05f));
	}
}

// ---------------------------------------------------------------------------------------------------------------
// get on / off

void ABullyVehicle::TryMountNearby()
{
	UWorld* W = GetWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	ABullyCharacter* C = PC ? Cast<ABullyCharacter>(PC->GetPawn()) : nullptr;
	if (!C) { return; }
	const float Reach = Def.HalfExtent.X + 180.f;
	const FVector Me = GetActorLocation();
	if (FVector::DistSquared2D(C->GetActorLocation(), Me) > FMath::Square(Reach) || FMath::Abs(C->GetActorLocation().Z - Me.Z) > 250.f) { return; }
	// only the nearest empty ride answers
	for (TActorIterator<ABullyVehicle> It(W); It; ++It)
	{
		if (*It != this && !It->IsOccupied()
			&& FVector::DistSquared2D(C->GetActorLocation(), It->GetActorLocation()) < FVector::DistSquared2D(C->GetActorLocation(), Me)) { return; }
	}
	if (GEngine) { GEngine->AddOnScreenDebugMessage(uint64(GetUniqueID()) + 7000, 0.12f, FColor(250, 200, 60), FString::Printf(TEXT("E  ride the %s"), *Def.Name)); }
	if ((PC->WasInputKeyJustPressed(EKeys::E) || PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top))
		&& W->GetRealTimeSeconds() - GLastRideToggle > 0.4)
	{
		Mount(C);
	}
}

bool ABullyVehicle::Mount(ABullyCharacter* NewRider)
{
	if (IsOccupied() || !NewRider) { return false; }
	APlayerController* PC = Cast<APlayerController>(NewRider->GetController());
	if (!PC) { return false; }
	GLastRideToggle = GetWorld()->GetRealTimeSeconds();

	if (UCharacterMovementComponent* CM = NewRider->GetCharacterMovement())
	{
		CM->StopMovementImmediately();
		CM->DisableMovement();
	}
	NewRider->SetActorEnableCollision(false);
	NewRider->AttachToComponent(Box, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	NewRider->SetActorRelativeLocation(Def.SeatOffset);
	NewRider->SetActorRelativeRotation(FRotator::ZeroRotator);

	Rider = NewRider;
	DriverPC = PC;
	Throttle = Steer = 0.f;
	bHandbrake = false;
	LookOffset = FRotator::ZeroRotator;
	PC->Possess(this);
	++TotalMounts;
	UE_LOG(LogBullyVehicle, Display, TEXT("BR_VEHICLE mount %s"), *Def.Name);
	return true;
}

bool ABullyVehicle::Dismount()
{
	ABullyCharacter* C = Rider.Get();
	if (!C) { return false; }
	UWorld* W = GetWorld();
	GLastRideToggle = W->GetRealTimeSeconds();
	APlayerController* PC = DriverPC.IsValid() ? DriverPC.Get() : Cast<APlayerController>(GetController());

	C->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	// step off to the left, else right / behind / ahead, wherever a standing capsule fits
	const float HalfH = C->GetCapsuleComponent() ? C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.f;
	const float Rad = C->GetCapsuleComponent() ? C->GetCapsuleComponent()->GetScaledCapsuleRadius() : 34.f;
	const FVector Base = GetActorLocation();
	const FVector Side = GetActorRightVector(), Fwd = GetActorForwardVector();
	const FVector Tries[4] = { -Side * (Def.HalfExtent.Y + Rad + 30.f), Side * (Def.HalfExtent.Y + Rad + 30.f),
		-Fwd * (Def.HalfExtent.X + Rad + 30.f), Fwd * (Def.HalfExtent.X + Rad + 30.f) };
	FVector Land = Base + Tries[0];
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyDismount), false, this);
	Q.AddIgnoredActor(C);
	for (const FVector& T : Tries)
	{
		FVector P = Base + T;
		FHitResult Hit;
		if (W->LineTraceSingleByObjectType(Hit, P + FVector(0, 0, 200), P - FVector(0, 0, 600), FCollisionObjectQueryParams(ECC_WorldStatic), Q))
		{
			P.Z = Hit.ImpactPoint.Z + HalfH + 3.f;
		}
		if (!W->OverlapBlockingTestByChannel(P, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, HalfH), Q)) { Land = P; break; }
	}
	C->SetActorLocationAndRotation(Land, FRotator(0.f, GetActorRotation().Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	C->SetActorEnableCollision(true);
	if (UCharacterMovementComponent* CM = C->GetCharacterMovement()) { CM->SetMovementMode(MOVE_Walking); }

	Rider.Reset();
	Throttle = Steer = 0.f;
	bHandbrake = false;
	if (PC) { PC->Possess(C); }
	++TotalDismounts;
	UE_LOG(LogBullyVehicle, Display, TEXT("BR_VEHICLE dismount %s at %.0f km/h"), *Def.Name, FMath::Abs(Speed) * 0.036f);
	return true;
}

void ABullyVehicle::SetScriptedInput(bool bOn, float InThrottle, float InSteer, bool bInHandbrake)
{
	bScripted = bOn;
	ScriptThrottle = FMath::Clamp(InThrottle, -1.f, 1.f);
	ScriptSteer = FMath::Clamp(InSteer, -1.f, 1.f);
	bScriptHandbrake = bInHandbrake;
}

// ---------------------------------------------------------------------------------------------------------------
// input

void ABullyVehicle::BuildInput()
{
	if (Imc) { return; }
	Imc = NewObject<UInputMappingContext>(this, TEXT("IMC_BullyVehicle"));
	auto Make = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* A = NewObject<UInputAction>(this, Name);
		A->ValueType = Type;
		return A;
	};
	ThrottleAction = Make(TEXT("IA_VehThrottle"), EInputActionValueType::Axis1D);
	SteerAction = Make(TEXT("IA_VehSteer"), EInputActionValueType::Axis1D);
	HandbrakeAction = Make(TEXT("IA_VehHandbrake"), EInputActionValueType::Boolean);
	LookAction = Make(TEXT("IA_VehLook"), EInputActionValueType::Axis2D);
	ExitAction = Make(TEXT("IA_VehExit"), EInputActionValueType::Boolean);

	auto Negate = [this]() { UInputModifierNegate* M = NewObject<UInputModifierNegate>(this); return M; };
	auto Dead = [this]() { UInputModifierDeadZone* M = NewObject<UInputModifierDeadZone>(this); M->LowerThreshold = 0.15f; return M; };

	Imc->MapKey(ThrottleAction, EKeys::W);
	Imc->MapKey(ThrottleAction, EKeys::S).Modifiers.Add(Negate());
	Imc->MapKey(ThrottleAction, EKeys::Gamepad_RightTriggerAxis);
	Imc->MapKey(ThrottleAction, EKeys::Gamepad_LeftTriggerAxis).Modifiers.Add(Negate());
	Imc->MapKey(SteerAction, EKeys::D);
	Imc->MapKey(SteerAction, EKeys::A).Modifiers.Add(Negate());
	Imc->MapKey(SteerAction, EKeys::Gamepad_LeftX).Modifiers.Add(Dead());
	Imc->MapKey(HandbrakeAction, EKeys::SpaceBar);
	Imc->MapKey(HandbrakeAction, EKeys::Gamepad_FaceButton_Left);
	Imc->MapKey(LookAction, EKeys::Mouse2D);
	Imc->MapKey(LookAction, EKeys::Gamepad_Right2D).Modifiers.Add(Dead());
	Imc->MapKey(ExitAction, EKeys::E);
	Imc->MapKey(ExitAction, EKeys::Gamepad_FaceButton_Top);
}

void ABullyVehicle::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	BuildInput();
	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EIC->BindAction(ThrottleAction, ETriggerEvent::Triggered, this, &ABullyVehicle::OnThrottle);
		EIC->BindAction(ThrottleAction, ETriggerEvent::Completed, this, &ABullyVehicle::OnThrottleEnd);
		EIC->BindAction(SteerAction, ETriggerEvent::Triggered, this, &ABullyVehicle::OnSteer);
		EIC->BindAction(SteerAction, ETriggerEvent::Completed, this, &ABullyVehicle::OnSteerEnd);
		EIC->BindAction(HandbrakeAction, ETriggerEvent::Started, this, &ABullyVehicle::OnHandbrake);
		EIC->BindAction(HandbrakeAction, ETriggerEvent::Completed, this, &ABullyVehicle::OnHandbrakeEnd);
		EIC->BindAction(LookAction, ETriggerEvent::Triggered, this, &ABullyVehicle::OnLook);
		EIC->BindAction(ExitAction, ETriggerEvent::Started, this, &ABullyVehicle::OnExit);
	}
}

void ABullyVehicle::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	DriverPC = Cast<APlayerController>(NewController);
}

void ABullyVehicle::PawnClientRestart()
{
	Super::PawnClientRestart();
	BuildInput();
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (UEnhancedInputLocalPlayerSubsystem* Sub = PC ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()) : nullptr)
	{
		Sub->AddMappingContext(Imc, 1);   // above the on-foot controls: W/S/A/D, mouse and E drive the ride
	}
}

void ABullyVehicle::UnPossessed()
{
	if (APlayerController* PC = DriverPC.Get())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Sub = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Sub->RemoveMappingContext(Imc);
		}
	}
	Super::UnPossessed();
}

void ABullyVehicle::OnLook(const FInputActionValue& V)
{
	const FVector2D D = V.Get<FVector2D>();
	LookOffset.Yaw = FRotator::NormalizeAxis(LookOffset.Yaw + D.X * 0.2f);
	LookOffset.Pitch = FMath::Clamp(LookOffset.Pitch + D.Y * 0.15f, -35.f, 25.f);
	LastLookTime = GetWorld()->GetRealTimeSeconds();
}

void ABullyVehicle::OnExit(const FInputActionValue& V)
{
	if (GetWorld()->GetRealTimeSeconds() - GLastRideToggle > 0.4) { Dismount(); }
}

// ---------------------------------------------------------------------------------------------------------------
// driving

void ABullyVehicle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.05f);
	if (!IsOccupied() && !bScripted) { TryMountNearby(); }
	Drive(Dt);
	if (IsOccupied())
	{
		UpdateCamera(Dt);
		ShowStatus();
	}
	UpdateWheels(Dt);
}

void ABullyVehicle::Drive(float Dt)
{
	const float T = bScripted ? ScriptThrottle : (IsOccupied() ? Throttle : 0.f);
	const float S = bScripted ? ScriptSteer : (IsOccupied() ? Steer : 0.f);
	const bool HB = bScripted ? bScriptHandbrake : (IsOccupied() && bHandbrake);
	const float Sign = Speed >= 0.f ? 1.f : -1.f;

	if (bGrounded)
	{
		if (HB)
		{
			Speed -= Sign * FMath::Min(FMath::Abs(Speed), Def.Brake * 1.2f * Dt);
		}
		else if (T > 0.05f)
		{
			Speed += (Speed < 0.f ? Def.Brake : Def.Accel * (1.f - 0.35f * FMath::Clamp(Speed / Def.MaxSpeed, 0.f, 1.f))) * T * Dt;
		}
		else if (T < -0.05f)
		{
			Speed -= (Speed > 0.f ? Def.Brake : Def.Accel * 0.6f) * -T * Dt;
		}
		else
		{
			Speed -= Sign * FMath::Min(FMath::Abs(Speed), Def.Coast * Dt);
		}
		Speed = FMath::Clamp(Speed, -Def.MaxReverse, Def.MaxSpeed);
	}

	FRotator Rot = GetActorRotation();
	if (bGrounded)
	{
		const float Factor = FMath::Clamp(FMath::Abs(Speed) / Def.TurnFullSpeed, 0.f, 1.f);
		Rot.Yaw += S * Def.TurnRate * (HB ? 1.4f : 1.f) * Factor * (Speed >= 0.f ? 1.f : -1.f) * Dt;
	}
	SetActorRotation(Rot);

	if (FMath::Abs(Speed) > 1.f)
	{
		UWorld* W = GetWorld();
		const FVector Fwd = FRotator(0.f, Rot.Yaw, 0.f).Vector();
		const FVector Delta = Fwd * Speed * Dt;
		const bool bStepped = bGrounded;
		if (bStepped) { AddActorWorldOffset(FVector(0.f, 0.f, StepUp)); }   // lift so the box can roll over kerbs
		FHitResult Hit;
		AddActorWorldOffset(Delta, true, &Hit);
		if (Hit.bBlockingHit)
		{
			AActor* Other = Hit.GetActor();
			UBullyCombatComponent* Victim = Other ? Other->FindComponentByClass<UBullyCombatComponent>() : nullptr;
			const double Now = W->GetTimeSeconds();
			if (Victim && Other != Rider.Get() && FMath::Abs(Speed) > 450.f)
			{
				// knock a pedestrian over (once a second per victim) and keep rolling
				const double* Last = RecentRunOvers.Find(Other);
				if (!Last || Now - *Last > 1.0)
				{
					RecentRunOvers.Add(Other, Now);
					// lane-C: the ragdoll takes the vehicle's own velocity (a car throws a student far, a bike barely); with no
					// physics asset on the victim the old capsule launch stays as the fallback.
					const FVector Away2D = (Other->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
					const FVector Throw = (Fwd * FMath::Sign(Speed) * 0.5f + Away2D * 0.5f).GetSafeNormal() * FMath::Clamp(FMath::Abs(Speed) * 0.9f, 350.f, 1600.f) + FVector(0.f, 0.f, 260.f);
					Victim->ApplyWeaponHit(EBullyHitReaction::Knockdown, FMath::Clamp(FMath::Abs(Speed) / 120.f, 4.f, 25.f), Throw);
					ABullyCharacter* VC = Cast<ABullyCharacter>(Other);
					if (VC && !(VC->GetRagdoll() && VC->GetRagdoll()->IsActive()))
					{
						VC->LaunchCharacter(Away2D * 500.f + FVector(0.f, 0.f, 250.f), true, true);
					}
					++TotalRunOvers;
					UE_LOG(LogBullyVehicle, Display, TEXT("BR_VEHICLE %s ran into %s at %.0f km/h"), *Def.Name, *Other->GetName(), FMath::Abs(Speed) * 0.036f);
				}
				Speed *= 0.75f;
			}
			else
			{
				// wall: lose speed by how head-on it was, bounce off a hard hit, slide along the rest
				const FVector N = FVector(Hit.ImpactNormal.X, Hit.ImpactNormal.Y, 0.f).GetSafeNormal();
				const float HeadOn = FMath::Abs(FVector::DotProduct(Fwd, N));
				if (HeadOn > 0.85f && FMath::Abs(Speed) > 600.f) { Speed = -Speed * 0.2f; }
				else { Speed *= (1.f - 0.8f * HeadOn); }
			}
			FVector Slide = FVector::VectorPlaneProject(Delta * (1.f - Hit.Time), Hit.ImpactNormal);
			Slide.Z = 0.f;
			if (!Slide.IsNearlyZero()) { AddActorWorldOffset(Slide, true); }
		}
		if (bStepped) { AddActorWorldOffset(FVector(0.f, 0.f, -StepUp), true); }   // and back down (stops on a kerb top)
	}
	SettleOnGround(Dt, false);
}

void ABullyVehicle::SettleOnGround(float Dt, bool bSnap)
{
	UWorld* W = GetWorld();
	FVector Loc = GetActorLocation();
	FRotator Rot = GetActorRotation();
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(BullyVehicleGround), true, this);
	if (Rider.IsValid()) { Q.AddIgnoredActor(Rider.Get()); }
	const bool bHit = W->LineTraceSingleByObjectType(Hit, Loc + FVector(0, 0, Def.HalfExtent.Z + 60.f),
		Loc - FVector(0, 0, Def.RideHeight + 400.f), FCollisionObjectQueryParams(ECC_WorldStatic), Q);

	FRotator Target = FRotator(0.f, Rot.Yaw, 0.f);
	if (bHit && Hit.ImpactNormal.Z > 0.6f)
	{
		const float TargetZ = Hit.ImpactPoint.Z + Def.RideHeight;
		if (bSnap || Loc.Z - TargetZ < 45.f)
		{
			Loc.Z = Loc.Z < TargetZ ? TargetZ : FMath::FInterpTo(Loc.Z, TargetZ, Dt, 18.f);
			VerticalSpeed = 0.f;
			bGrounded = true;
			Target = FRotationMatrix::MakeFromXZ(FRotator(0.f, Rot.Yaw, 0.f).Vector(), Hit.ImpactNormal).Rotator();
			Target.Yaw = Rot.Yaw;
		}
		else
		{
			VerticalSpeed -= 980.f * Dt;   // off a ledge: fall to the ground below
			Loc.Z = FMath::Max(TargetZ, Loc.Z + VerticalSpeed * Dt);
			bGrounded = Loc.Z <= TargetZ + 1.f;
			if (bGrounded) { VerticalSpeed = 0.f; }
		}
	}
	else
	{
		VerticalSpeed -= 980.f * Dt;
		Loc.Z += VerticalSpeed * Dt;
		bGrounded = false;
	}
	Rot.Pitch = FMath::FInterpTo(Rot.Pitch, Target.Pitch, Dt, 10.f);
	Rot.Roll = FMath::FInterpTo(Rot.Roll, Target.Roll, Dt, 10.f);
	SetActorLocationAndRotation(Loc, Rot);
}

void ABullyVehicle::UpdateCamera(float Dt)
{
	// look-around springs back behind the ride once you stop steering the camera and are moving
	if (GetWorld()->GetRealTimeSeconds() - LastLookTime > 1.2 && FMath::Abs(Speed) > 200.f)
	{
		LookOffset.Yaw = FMath::FInterpTo(LookOffset.Yaw, 0.f, Dt, 2.5f);
		LookOffset.Pitch = FMath::FInterpTo(LookOffset.Pitch, 0.f, Dt, 2.5f);
	}
	Boom->SetRelativeRotation(FRotator(-10.f + LookOffset.Pitch, LookOffset.Yaw, 0.f));
}

void ABullyVehicle::UpdateWheels(float Dt)
{
	if (!Def.MeshPath.IsEmpty() || Def.Wheels == 0) { return; }
	const float SteerVis = (bScripted ? ScriptSteer : Steer) * 25.f;
	for (int32 i = 0; i < Def.Wheels && i < WheelMeshes.Num(); ++i)
	{
		const bool bFront = i % 2 == 0;
		WheelMeshes[i]->SetRelativeRotation(FRotator(0.f, bFront ? SteerVis : 0.f, 90.f));   // front wheels turn with the steering
	}
}

void ABullyVehicle::ShowStatus() const
{
	if (GEngine && IsLocallyControlled())
	{
		GEngine->AddOnScreenDebugMessage(uint64(GetUniqueID()) + 8000, 0.12f, FColor(250, 200, 60),
			FString::Printf(TEXT("%s  %3.0f km/h    W/S drive  A/D steer  SPACE handbrake  E get off"), *Def.Name, FMath::Abs(Speed) * 0.036f));
	}
}
