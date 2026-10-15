// OWNER: builder-B (ue)
// Offscreen evidence capture for standalone (-game) runs. Inactive unless -BRShotFile= is on the
// command line. See tools/ue/README.md ("Screenshots").
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "BullyShotRunner.generated.h"

class ACameraActor;

struct FBullyShot
{
	FString Name;
	FVector Location = FVector::ZeroVector;   // UE cm
	FVector LookAt = FVector::ZeroVector;     // UE cm
	float Fov = 70.f;
	bool bGroundTrace = false;                // snap Location.Z to ground + EyeHeight
	float EyeHeight = 170.f;
	bool bLookAtGroundTrace = false;          // snap LookAt.Z to ground + LookAtHeight
	float LookAtHeight = 0.f;

	// runtime
	bool bPlaced = false;
	bool bRequested = false;
	double PlacedAt = 0.0;
	FString File;
	FString GroundInfo;
};

/**
 * Reads a JSON shot list, places a camera actor per shot, waits for Lumen/exposure to settle,
 * writes a PNG of the game viewport per shot, then quits.
 *
 *   -BRShotFile=<shots.json>  -BRShotDir=<output dir>
 *
 * shots.json: { "warmup_seconds": 12, "settle_seconds": 6, "max_compile_wait_seconds": 1200, "exec": ["r.MotionBlurQuality 0"],
 *               "highres_size": [3840, 2160],   (optional: HighResShot at exactly this size)
 *               "shots": [ { "name": "gates", "location_cm": [x,y,z], "look_at_cm": [x,y,z],
 *                            "fov": 70, "ground_trace": true, "eye_height_cm": 170 } ] }
 */
UCLASS()
class UBullyShotRunner : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return bActive; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }

private:
	bool LoadShots(const FString& Path);
	bool PlaceShot(UWorld* World, FBullyShot& Shot);
	bool TraceGround(UWorld* World, const FVector& At, FVector& OutHit) const;
	void WriteSidecar() const;

	bool bActive = false;
	bool bExecDone = false;
	bool bQuitRequested = false;
	double Elapsed = 0.0;
	double WarmupSeconds = 12.0;
	double SettleSeconds = 6.0;
	double QuitAfterLastShot = 3.0;
	double LastShotAt = -1.0;
	double ReadyAt = -1.0;            // compile queues drained (or wait capped)
	double IdleSince = -1.0;
	double LastCompileLog = -100.0;
	double MaxCompileWaitSeconds = 1200.0;
	int32 Current = 0;
	int32 HighResX = 0;               // "highres_size": [x, y] -> HighResShot instead of viewport grab
	int32 HighResY = 0;
	FString ShotDir;
	TArray<FString> ExecCommands;
	TArray<FBullyShot> Shots;
	TWeakObjectPtr<ACameraActor> Camera;
};
