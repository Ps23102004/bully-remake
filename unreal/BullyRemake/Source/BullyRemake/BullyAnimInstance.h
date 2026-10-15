// OWNER: builder-B3 (jimmy)
// Jimmy's locomotion animation, all in C++ (no AnimBlueprint asset): a speed-driven, phase-synchronised
// idle / walk / run / sprint blend plus jump, fall and land states, evaluated by a custom anim-instance proxy.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "BullyAnimInstance.generated.h"

class UAnimSequence;
class ABullyCharacter;

/** One sampled clip in the final blend. */
struct FBullyAnimLayer
{
	UAnimSequence* Seq = nullptr;   // kept alive by UBullyAnimInstance::Clips
	float Time = 0.f;
	float Weight = 0.f;
	bool bClampRootRise = false;     // standing jump: the clip lifts the pelvis; the capsule already does the jump
	bool bLoop = false;
};

UENUM()
enum class EBullyAnimState : uint8
{
	Loco,
	Jump,
	Fall,
	Land,
};

/** Evaluates the layer list prepared on the game thread: samples each clip and blends them (n-way, normalised). */
struct FBullyAnimInstanceProxy : public FAnimInstanceProxy
{
	FBullyAnimInstanceProxy() = default;
	explicit FBullyAnimInstanceProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

	TArray<FBullyAnimLayer, TInlineAllocator<8>> Layers;
	int32 RootMeshBoneIndex = INDEX_NONE;
	int32 RootUpAxis = 2;
	float RootMaxUp = 0.f;
};

/**
 * Locomotion state machine (game thread, NativeUpdateAnimation) -> layer list -> proxy evaluation.
 *
 *  Loco  : idle <-> walk <-> run <-> sprint by measured ground speed. The three cycles share one normalised phase
 *          (0 = left foot mid-stance), advanced at speed / blended-stride, so the planted foot stays planted at any
 *          speed between the anchors (the clips' own root-motion speeds: 2.05 / 3.61 / 8.01 m/s).
 *  Jump  : IDLEJUMP (standing) or JOGJUMP_L/_R (moving; the foot in stance picks the side). Take-off part at rate 1,
 *          air part time-scaled to the predicted landing, held at its end while still airborne.
 *  Fall  : FALL loop after a long drop or when the jump falls well below its take-off height.
 *  Land  : rest of the jump clip, FALL_LAND (still), JOGJUMP land part (moving) or PLAY_LAND_HURT (> fall-damage height).
 * State changes cross-fade (StateBlendTime). All tuning is Config=Game ([/Script/BullyRemake.BullyAnimInstance]).
 */
UCLASS(Transient, NotBlueprintable, Config = Game)
class UBullyAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") FString ClipRoot = TEXT("/Game/Imported/Characters/Jimmy/Anims");

	// Blend anchors (cm/s): ground speed at which each cycle has full weight = the character's gait speeds, which are
	// the clips' root-motion (ARROW) speeds 2.05 / 3.61 / 8.01 m/s.
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float WalkBlendSpeed = 204.6f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float RunBlendSpeed = 361.4f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float SprintBlendSpeed = 801.2f;
	// Play-rate sync (cm/s): the speed at which each clip's PLANTED foot moves backward relative to the body, measured by
	// FK on the clips (tools/ue/README.md). Play rate = ground speed / foot speed, so the stance foot stays locked.
	// The ARROW speeds overstate it (walk 2.05 vs 1.79, run 3.61 vs 3.52, sprint 8.01 vs 5.08 m/s): in the raw data the
	// sprint clip's planted foot itself slides forward at 2.9 m/s. Set a foot speed equal to its blend speed to play the
	// clip at its authored rate instead (sprint then slides).
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float WalkFootSpeed = 179.f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float RunFootSpeed = 352.f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float SprintFootSpeed = 508.f;
	// Normalised clip time of the LEFT foot's mid-stance (foot passing under the hip), measured by FK on the clips.
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float WalkPhaseOffset = 0.744f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float RunPhaseOffset = 0.591f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float SprintPhaseOffset = 0.510f;
	/** Ground speed (cm/s) at which idle has fully blended out. */
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float IdleBlendSpeed = 60.f;
	/** Lowest walk-cycle play rate (very slow analog walking would otherwise crawl). */
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float MinWalkPlayRate = 0.5f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float StateBlendTime = 0.15f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float FallBlendTime = 0.3f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float LandBlendTime = 0.06f;
	/** Moving faster than this (cm/s) at take-off / landing selects the moving jump / moving land. */
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float MovingJumpSpeed = 100.f;
	// Clip segment boundaries (seconds into the clip), read off the foot heights of the clips.
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float JogJumpStart = 0.05f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float JogJumpTakeoff = 0.10f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float JogJumpAirEnd = 0.58f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float IdleJumpStart = 0.15f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float IdleJumpTakeoff = 0.22f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float IdleJumpAirEnd = 0.56f;
	/** Falling this far (cm) below the take-off height, or this long (s) off a ledge, switches to the FALL loop. */
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float FallDropThreshold = 120.f;
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float LedgeFallDelay = 0.25f;
	/** A still landing can be interrupted by movement after this long (s). */
	UPROPERTY(Config, EditAnywhere, Category = "Bully|Anim") float LandInterruptTime = 0.12f;

	// --- read-only state (logging / evidence) ---------------------------------------------------------------
	EBullyAnimState GetState() const { return State; }
	const TCHAR* GetStateName() const;
	float GetPhase() const { return Phase; }
	FString DescribeLayers() const;
	bool HasClips() const { return bClipsOk; }

protected:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override { return new FBullyAnimInstanceProxy(this); }
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override { delete InProxy; }

private:
	friend struct FBullyAnimInstanceProxy;

	UAnimSequence* Clip(const TCHAR* Name);
	void LoadClips();
	void BuildLoco(float Dt, float Speed, TArray<FBullyAnimLayer, TInlineAllocator<8>>& Out);
	void EnterState(EBullyAnimState NewState, float BlendTime);
	float ClipLength(const UAnimSequence* S) const;

	UPROPERTY(Transient) TMap<FName, TObjectPtr<UAnimSequence>> Clips;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> Idle;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> Walk;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> Run;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> Sprint;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> IdleJump;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> JogJumpL;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> JogJumpR;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> Fall;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> FallLand;
	UPROPERTY(Transient) TObjectPtr<UAnimSequence> LandHurt;

	bool bClipsOk = false;
	EBullyAnimState State = EBullyAnimState::Loco;
	float StateTime = 0.f;
	float Phase = 0.f;          // shared locomotion phase, cycles
	float IdleTime = 0.f;

	// active one-shot clip for Jump / Land
	TObjectPtr<UAnimSequence> ActionClip = nullptr;
	float ActionTime = 0.f;
	float ActionAirEnd = 0.f;
	float ActionTakeoff = 0.f;
	bool bActionClampRoot = false;
	bool bLandMoving = false;

	// character snapshot
	int32 SeenJumpCount = 0;
	int32 SeenLandCount = 0;
	float JumpStartZ = 0.f;
	float AirTime = 0.f;
	bool bWasFalling = false;

	// cross-fade: frozen outgoing layers, faded out over BlendDuration
	TArray<FBullyAnimLayer, TInlineAllocator<8>> Outgoing;
	TArray<FBullyAnimLayer, TInlineAllocator<8>> Current;      // last frame's final layers
	TArray<FBullyAnimLayer, TInlineAllocator<8>> Final;        // this frame's output (copied by the proxy)
	float BlendAlpha = 1.f;
	float BlendDuration = 0.f;

	int32 RootMeshBoneIndex = INDEX_NONE;
	int32 RootUpAxis = 2;
	float RootMaxUp = 0.f;
};
