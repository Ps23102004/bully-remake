// OWNER: slice-combat
// Combat layer over Jimmy's locomotion anim instance: the base class builds the idle/walk/run/jump layers, this
// subclass cross-fades the owner's UBullyCombatComponent clip request (attacks, blocks, hit reactions, grapple pairs,
// knockdown / KO loops) over them, and swaps the run cycle for 4-way strafes while locked on.
// Used by ABullyCharacter and ABullyCombatPawn (the anim instance class is set in ABullyCharacter).
#pragma once

#include "CoreMinimal.h"
#include "BullyAnimInstance.h"
#include "BullyCombatComponent.h"
#include "BullyCombatAnimInstance.generated.h"

UCLASS(Transient, NotBlueprintable, Config = Game)
class UBullyCombatAnimInstance : public UBullyAnimInstance
{
	GENERATED_BODY()

public:
	/** Fade in / out of the combat layer and between consecutive combat clips (s). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Anim") float CombatBlendIn = 0.06f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Anim") float CombatBlendOut = 0.18f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Anim") float ClipCrossFade = 0.08f;
	/** Locked-on movement plays the STRAFE_* clips instead of the forward run cycle. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Anim") bool bStrafeWhenLocked = true;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Anim") float StrafeMinSpeed = 40.f;

	float GetCombatWeight() const { return Weight; }
	FName GetCombatClip() const { return Cur.Seq ? Cur.Seq->GetFName() : NAME_None; }

protected:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

private:
	UAnimSequence* Strafe(const TCHAR* Name);

	FBullyAnimLayer Cur;
	FBullyAnimLayer Prev;        // previous combat clip, fading out
	float PrevAlpha = 0.f;       // 1 = Prev fully visible
	uint32 Serial = 0;
	float Weight = 0.f;          // combat layer weight over locomotion
	float StrafeTime = 0.f;
	UPROPERTY(Transient) TMap<FName, TObjectPtr<UAnimSequence>> StrafeClips;
};
