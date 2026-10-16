// OWNER: builder-B4 (ambient)
// Milestone 4 "signs of life": spawns the student walkers, the courtyard ambient bed and footsteps from
// Config/AmbientLoops.json when the Bullworth map starts. Disabled with -BRNoAmbient or when the file is absent.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GameFramework/Actor.h"
#include "Containers/Ticker.h"
#include "AmbientSubsystem.generated.h"

class UAudioComponent;
class USoundBase;
class USoundAttenuation;
class UBullyFootstepComponent;
class AAmbientWalker;
class ACharacter;

/** Holds the looping outdoor ambience (wind + birds decoded from the game's Wind.SEA / Birds.SEA) as audio components. */
UCLASS()
class AAmbientSoundActor : public AActor
{
	GENERATED_BODY()

public:
	AAmbientSoundActor();
	/** Creates one looping component per sound (volume multiplier each) and starts them. */
	void Start(const TArray<USoundBase*>& Sounds, const TArray<float>& Volumes, float InnerRadius, float Falloff);
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY(Transient) TArray<TObjectPtr<UAudioComponent>> Components;
	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> Attenuation;
	double NextLog = 1.0;
};

UCLASS()
class UAmbientSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	const TArray<TWeakObjectPtr<AAmbientWalker>>& GetWalkers() const { return Walkers; }

	/** Gives a character (a scripted NPC) the same footstep component the ambient walkers get. Kind: "shoe" | "female". */
	void AttachFootsteps(ACharacter* Character, const FString& Kind, const FString& Tag);

private:
	void Spawn(UWorld& World);
	void ValidateLoop(UWorld& World, const FString& Id, const TArray<FVector>& Pts);
	void AttachPlayerFootsteps();
	/** -BRAmbientProbe=x0,y0,x1,y1,step,zTop,zBottom (UE cm): logs an ASCII walkability map (capsule overlap test), then quits. */
	void RunProbe(UWorld& World, const FString& Args);
	/** Evidence camera: -BRAmbientCam=<student>:<seconds>,<student>:<seconds>,... cuts between chase shots of walkers (3.5 m
	 *  behind-left, chest height); fixed:x:y:z:lookX:lookY:lookZ:<seconds> entries hold a free camera (UE cm), mixable with chase entries. Wide-lens (FOV 70). */
	void StartEvidenceCamera(UWorld& World, const FString& Spec);
	bool TickEvidenceCamera(float Dt);
	USoundBase* LoadGameSound(const FString& Bank, const FString& Name) const;

	TArray<TWeakObjectPtr<AAmbientWalker>> Walkers;
	TMap<FString, TArray<TObjectPtr<USoundBase>>> StepSets;   // footsteps config key -> sounds (kept for AttachFootsteps)
	UPROPERTY(Transient) TArray<TObjectPtr<USoundBase>> AllStepSounds;   // GC anchor for StepSets
	TWeakObjectPtr<AAmbientSoundActor> AmbientActor;
	TArray<TObjectPtr<USoundBase>> PlayerSteps;
	FTimerHandle PlayerAttachTimer;
	bool bPlayerAttached = false;

	struct FCamShot { FString Student; float Seconds = 8.f; bool bFixed = false; FVector Loc = FVector::ZeroVector, Look = FVector::ZeroVector; };
	TArray<FCamShot> CamShots;
	int32 CamIndex = 0;
	double CamShotStart = 0.0;
	FVector CamPos = FVector::ZeroVector;
	FVector CamHeading = FVector(1, 0, 0);
	TWeakObjectPtr<AActor> CamActor;
	FTSTicker::FDelegateHandle CamTicker;
};
