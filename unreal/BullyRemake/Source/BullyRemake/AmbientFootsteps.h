// OWNER: builder-B4 (ambient)
// Footsteps from the decoded game audio (tools/importers `audio`): one-shot on every foot plant, detected from the
// ankle bones of the playing animation, so it works for any gait and for both the player and the ambient walkers.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AmbientFootsteps.generated.h"

class USoundBase;
class USoundAttenuation;

UCLASS(ClassGroup = "Bully", meta = (BlueprintSpawnableComponent))
class UBullyFootstepComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBullyFootstepComponent();

	/** Candidate one-shots (a random one per step, never the same twice in a row). */
	UPROPERTY(Transient) TArray<TObjectPtr<USoundBase>> Sounds;
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float Volume = 0.8f;
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float PitchVariation = 0.06f;
	/** Steps farther than this from the listener (the local player's pawn) are skipped entirely. */
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float MaxAudibleDistance = 3500.f;
	/** Attenuation inner / falloff distances (cm). */
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float InnerRadius = 150.f;
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float FalloffDistance = 2200.f;
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float MinStepInterval = 0.22f;
	/** A foot counts as planted when its ankle is within this height (cm) of the lowest ankle height seen. */
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float PlantTolerance = 3.5f;
	UPROPERTY(EditAnywhere, Category = "Bully|Audio") float MinSpeed = 30.f;
	FString Tag;   // log label

	int32 GetStepCount() const { return StepCount; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void PlayStep(int32 Foot, const FVector& Where);

	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> Attenuation;
	bool bDown[2] = { true, true };
	bool bInit = false;
	float MinZ[2] = { 1e9f, 1e9f };
	double LastStep[2] = { -10.0, -10.0 };
	int32 LastIndex = -1;
	int32 StepCount = 0;
	double Born = 0.0;
};
