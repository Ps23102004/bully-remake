// OWNER: slice-vehicles
// Driveable ride (bike / skateboard / go-kart / car) configured from Config/Vehicles.json. Arcade handling written here
// (no Chaos vehicle plugin): throttle/brake/reverse/coast, speed-scaled steering, ground following (slopes, curbs),
// wall sliding, knocking pedestrians over, chase camera with mouse look-around.
//
// Get on: walk up to an empty ride and press E (gamepad Y); the player controller possesses the ride and the rider is
// seated on it. Drive: W/S or RT/LT throttle/brake-reverse, A/D or left stick steer, Space/X handbrake, mouse/right
// stick look. Get off: E/Y.
// Look: engine primitives (body, wheels, cabin) tinted by 'color', or a local static mesh from the 'mesh' path.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "InputActionValue.h"
#include "BullyVehicle.generated.h"

class ABullyCharacter;
class APlayerController;
class UBoxComponent;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class USpringArmComponent;
class UStaticMesh;
class UStaticMeshComponent;

struct FBullyVehicleDef
{
	FName Id;
	FString Name;
	FVector HalfExtent = FVector(85.f, 22.f, 45.f);
	float RideHeight = 47.f;
	float MaxSpeed = 1500.f, Accel = 950.f, Brake = 2400.f, MaxReverse = 250.f, Coast = 300.f;
	float TurnRate = 115.f, TurnFullSpeed = 350.f;
	FVector SeatOffset = FVector(-12.f, 0.f, 70.f);
	bool bRiderStands = false;
	float CameraDistance = 430.f, CameraHeight = 110.f;
	FLinearColor Color = FLinearColor(0.75f, 0.12f, 0.10f);
	int32 Wheels = 2;
	float WheelRadius = 34.f;
	bool bCabin = false;
	FString MeshPath;
	FVector MeshScale = FVector(1.f);
	FVector MeshOffset = FVector::ZeroVector;
	float MeshYaw = 0.f;

	/** Parse Config/Vehicles.json (cached after the first call). */
	static const TArray<FBullyVehicleDef>& All();
	static const FBullyVehicleDef* Find(FName Id);
};

UCLASS()
class ABullyVehicle : public APawn
{
	GENERATED_BODY()

public:
	ABullyVehicle();

	/** Spawn ride Id at Location (XY; settled onto the ground) facing Yaw. Null if the id is unknown. */
	static ABullyVehicle* SpawnVehicle(UWorld* World, FName Id, const FVector& Location, float Yaw);

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;
	virtual void PawnClientRestart() override;

	bool Mount(ABullyCharacter* NewRider);
	bool Dismount();
	bool IsOccupied() const { return Rider.IsValid(); }
	ABullyCharacter* GetRider() const { return Rider.Get(); }
	float GetSpeed() const { return Speed; }
	const FBullyVehicleDef& GetDef() const { return Def; }

	/** Stop dead (after a teleport/reset). */
	void ResetMotion() { Speed = 0.f; VerticalSpeed = 0.f; Throttle = 0.f; Steer = 0.f; bHandbrake = false; }
	/** Headless tests: drive with fixed inputs instead of the player's (bOn=false hands control back). */
	void SetScriptedInput(bool bOn, float InThrottle = 0.f, float InSteer = 0.f, bool bInHandbrake = false);

	// evidence counters (headless smoke)
	static int32 TotalMounts;
	static int32 TotalDismounts;
	static int32 TotalRunOvers;

private:
	void Configure(const FBullyVehicleDef& D);
	void BuildInput();
	void Drive(float Dt);
	void SettleOnGround(float Dt, bool bSnap);
	void TryMountNearby();
	void UpdateCamera(float Dt);
	void UpdateWheels(float Dt);
	void ShowStatus() const;

	void OnThrottle(const FInputActionValue& V) { Throttle = V.Get<float>(); }
	void OnThrottleEnd(const FInputActionValue& V) { Throttle = 0.f; }
	void OnSteer(const FInputActionValue& V) { Steer = V.Get<float>(); }
	void OnSteerEnd(const FInputActionValue& V) { Steer = 0.f; }
	void OnHandbrake(const FInputActionValue& V) { bHandbrake = true; }
	void OnHandbrakeEnd(const FInputActionValue& V) { bHandbrake = false; }
	void OnLook(const FInputActionValue& V);
	void OnExit(const FInputActionValue& V);

	UPROPERTY() TObjectPtr<UBoxComponent> Box;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Cabin;
	UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> WheelMeshes;
	UPROPERTY() TObjectPtr<USpringArmComponent> Boom;
	UPROPERTY() TObjectPtr<UCameraComponent> Cam;

	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> Imc;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ThrottleAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> SteerAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> HandbrakeAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> LookAction;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ExitAction;

	FBullyVehicleDef Def;
	TWeakObjectPtr<ABullyCharacter> Rider;
	TWeakObjectPtr<APlayerController> DriverPC;

	float Speed = 0.f;             // cm/s along the facing, negative = reversing
	float Throttle = 0.f;          // -1..1
	float Steer = 0.f;             // -1..1
	bool bHandbrake = false;
	float VerticalSpeed = 0.f;
	bool bGrounded = false;
	FRotator LookOffset = FRotator::ZeroRotator;
	double LastLookTime = -100.0;
	float WheelSpin = 0.f;
	TMap<TWeakObjectPtr<AActor>, double> RecentRunOvers;

	bool bScripted = false;
	float ScriptThrottle = 0.f, ScriptSteer = 0.f;
	bool bScriptHandbrake = false;
};
