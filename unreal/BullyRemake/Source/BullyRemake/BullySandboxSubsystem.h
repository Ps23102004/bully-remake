// OWNER: slice-vehicles
// Free-play start ("town sandbox"): on load, put the player on an open, flat patch of street near a configured spot
// (Config/Sandbox.json -> InteriorSpots.json key, default the town), facing a lineup of rides, all weapons unlocked.
// Off for mission boots and test-harness runs (any -BR* switch), forced on by -BRSandbox[=spot], off by -BRNoSandbox.
// -BRSandboxSmoke runs the setup and then drives every ride headlessly (mount, accelerate, steer, brake, dismount,
// run-over) and logs PASS/FAIL.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "BullySandboxSubsystem.generated.h"

class ABullyVehicle;
class ABullyCharacter;

UCLASS()
class UBullySandboxSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Find a flat street patch of HalfXY (cm) near Anchor, clear of walls/props, not water, not a roof. */
	bool FindOpenArea(const FVector& Anchor, const FVector2D& HalfXY, FVector& OutCenter, float& OutYaw) const;

private:
	bool LoadConfig();
	bool SpotLocation(const FString& Key, FVector& Out) const;
	void Setup(ABullyCharacter* Player);
	void TickSmoke(float Dt);

	bool bActive = false;
	bool bDone = false;
	bool bSmoke = false;
	double StartedAt = 0.0;
	FString StartKey = TEXT("town");
	TArray<FName> VehicleIds;
	float SearchRadius = 7000.f;
	float MinGroundZ = 200.f;

	TArray<TWeakObjectPtr<ABullyVehicle>> Lineup;
	TArray<FTransform> LineupHome;
	FVector Anchor = FVector::ZeroVector;

	struct FSmoke
	{
		int32 Step = 0;
		double StepStart = 0.0;
		int32 Index = 0;
		FVector StartLoc = FVector::ZeroVector;
		float StartYaw = 0.f;
		float PeakSpeed = 0.f;
		int32 RunOversBefore = 0;
		TArray<FString> Failures;
		int32 Passes = 0;
	} Smoke;
};
