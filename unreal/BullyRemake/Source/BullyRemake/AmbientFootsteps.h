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

	/** Legacy fixed set (milestone 4: cement only). Used only when the per-surface cue tables are not available. */
	UPROPERTY(Transient) TArray<TObjectPtr<USoundBase>> Sounds;
	/**
	 * Per-surface cue selection (polish1): "player" | "student" | "female". Every foot plant probes the surface under the
	 * foot (base-texture / model name -> surface, Config/FootstepSurfaces.json), picks the game's own footstep cue family
	 * for that surface, role and gait (Content/Imported/Audio/footstep_cues.json, decoded SDBK cue graph) and plays the
	 * wave with the cue's pitch (+ its random range). See docs/re-notes/audio-formats.md section 6.
	 */
	FString Role = TEXT("student");
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
	const FString& GetLastSurface() const { return LastSurface; }
	/** True when Config/FootstepSurfaces.json and Content/Imported/Audio/footstep_cues.json were both loaded. */
	static bool TablesAvailable();
	/** Surface name under `Where` (feet level); fills the reason (texture/model/component) and whether it is an interior pocket. */
	FString ProbeSurface(const FVector& Where, FString& OutWhy, bool& bOutInterior);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void PlayStep(int32 Foot, const FVector& Where);
	USoundBase* LoadCueSound(const FString& Asset);

	UPROPERTY(Transient) TObjectPtr<USoundAttenuation> Attenuation;
	UPROPERTY(Transient) TMap<FString, TObjectPtr<USoundBase>> CueSounds;
	FString LastSurface;
	FString LastCue;
	bool bDown[2] = { true, true };
	bool bInit = false;
	float MinZ[2] = { 1e9f, 1e9f };
	double LastStep[2] = { -10.0, -10.0 };
	int32 LastIndex = -1;
	int32 StepCount = 0;
	double Born = 0.0;
};
