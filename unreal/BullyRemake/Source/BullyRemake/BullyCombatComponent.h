// OWNER: slice-combat
// Melee combat core for the Day One fight tutorial (mission 1_02 B6/B7). One component on every fighter (Jimmy and
// ABullyCombatPawn): health, a data-driven move table (light 3-hit chain, ground kick, grapple strikes / push,
// humiliation, the bully's telegraphed swings), block with a guard meter, grapple pairs, lock-on / soft target, hit
// reactions (flinch, stagger, knockdown, ground hit, KO), get-up i-frames and hitstop.
//
// Nothing here comes from the original's fight action trees (Act/TrainingPlayer.act, Act/Anim/Fight_Tutorial.act and
// Act/AI/AI_FightTutorial.act are undecoded): timings, ranges and damage are authored. The clips are the decoded
// C_Player bank on SK_Jimmy; each move's hit window sits on the striking limb's peak reach, measured by FK over the
// imported clips (docs/re-notes/combat.md). Every number is Config=Game:
//   [/Script/BullyRemake.BullyCombatComponent] in Config/DefaultGame.ini (moves are +Moves=(...) rows), no recompile.
//
// Hit detection is an authored arc, not a bone hitbox: during a move's active window, every opposing fighter whose
// capsule is within Range (cm, capsule surface) and inside ArcDeg of the attacker's facing is hit once per swing.
// Mission hooks: UMissionSubsystem::FireEvent(<name>), names listed in docs/re-notes/combat.md.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BullyCombatComponent.generated.h"

class ACharacter;
class UAnimSequence;

UENUM()
enum class EBullyCombatState : uint8
{
	Idle,
	Attacking,      // playing a move from the table (also grapple strikes / push / humiliation as the giver)
	Blocking,
	BlockStun,      // blocked a hit (short recoil) or guard broken
	HitStun,        // flinch / stagger
	Downed,         // knocked down, on the ground (ground kicks connect)
	GettingUp,      // get-up clip, invulnerable
	Grappling,      // holding someone (giver)
	Grappled,       // held (receiver), also during the giver's grapple strikes / push / humiliation
	Beaten,         // health reached the floor (MinHealth > 0): dazed, waiting to be humiliated
	KO,
};

UENUM()
enum class EBullyHitReaction : uint8
{
	None,           // damage only (grapple strikes keep the victim held)
	Flinch,
	Stagger,
	Knockdown,
	GroundHit,      // victim already down: plays a ground hit and stays down a bit longer
	KO,             // humiliation finisher
};

UENUM()
enum class EBullyCombatTeam : uint8
{
	Player,
	Enemy,
};

/** One authored move. Times are seconds from the move's start at PlayRate 1 (game time). */
USTRUCT()
struct FBullyCombatMove
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere) FName Name;
	/** Clip under the combat clip root (Jimmy's C_Player bank). */
	UPROPERTY(Config, EditAnywhere) FName Clip;
	/** Clip the victim plays on a hit (pair moves: GRAP_*_RCV). None = the reaction's own clip. */
	UPROPERTY(Config, EditAnywhere) FName VictimClip;
	UPROPERTY(Config, EditAnywhere) float PlayRate = 1.f;
	/** Seconds into the clip where the move starts (trims a slow wind-up). */
	UPROPERTY(Config, EditAnywhere) float ClipStart = 0.f;
	/** Total time the move owns the fighter (0 = rest of the clip). */
	UPROPERTY(Config, EditAnywhere) float Duration = 0.f;
	/** Hit window. ActiveStart < 0 = the move never hits (wind-ups, whiffs). */
	UPROPERTY(Config, EditAnywhere) float ActiveStart = -1.f;
	UPROPERTY(Config, EditAnywhere) float ActiveEnd = -1.f;
	/** Reach (cm) past the attacker's capsule to the victim's capsule surface, and the hit arc (deg, full width). */
	UPROPERTY(Config, EditAnywhere) float Range = 90.f;
	UPROPERTY(Config, EditAnywhere) float ArcDeg = 100.f;
	UPROPERTY(Config, EditAnywhere) float Damage = 5.f;
	UPROPERTY(Config, EditAnywhere) EBullyHitReaction Reaction = EBullyHitReaction::Flinch;
	/** Guard meter cost when blocked; blocked damage is Damage * BlockDamageScale. */
	UPROPERTY(Config, EditAnywhere) float GuardDamage = 20.f;
	UPROPERTY(Config, EditAnywhere) bool bUnblockable = false;
	/** Only connects with a downed victim (ground kicks); normal moves pass over downed fighters. */
	UPROPERTY(Config, EditAnywhere) bool bGroundAttack = false;
	/** Chain: attack pressed after BufferOpen is remembered; the Next move starts at ChainTime (or right away if later). */
	UPROPERTY(Config, EditAnywhere) FName Next;
	UPROPERTY(Config, EditAnywhere) float BufferOpen = 0.1f;
	UPROPERTY(Config, EditAnywhere) float ChainTime = 0.3f;
	/** Starts Next when this move ends without any input (wind-up -> swing). */
	UPROPERTY(Config, EditAnywhere) bool bAutoChain = false;
	/** Step toward the target (cm over LungeTime s from the start), stopping at LungeStopDistance from it. */
	UPROPERTY(Config, EditAnywhere) float Lunge = 0.f;
	UPROPERTY(Config, EditAnywhere) float LungeTime = 0.15f;
	/** Push the victim back (cm over 0.2 s). */
	UPROPERTY(Config, EditAnywhere) float Knockback = 0.f;
	/** Freeze both fighters this long on contact (s, game time). */
	UPROPERTY(Config, EditAnywhere) float HitStop = 0.06f;
};

/** A clip request for the anim instance (UBullyCombatAnimInstance). */
struct FBullyCombatPose
{
	FName Clip;
	class UAnimSequence* Seq = nullptr;   // kept alive by UBullyCombatComponent::ClipCache
	float Time = 0.f;
	bool bLoop = false;
	uint32 Serial = 0;      // changes whenever a new clip starts (the anim instance cross-fades on change)
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FBullyCombatEvent, class UBullyCombatComponent* /*Self*/, FName /*Event*/);

UCLASS(ClassGroup = (Bully), meta = (BlueprintSpawnableComponent), Config = Game)
class UBullyCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBullyCombatComponent();

	/** External hit from a weapon/projectile or the environment (no attacker): applies Damage + Reaction, unblockable
	 *  (a thrown bottle still lands on a guarding target). Goes through the normal victim path, so camera shake, KO,
	 *  mission events and the health floor all behave as with a melee hit. */
	void ApplyWeaponHit(EBullyHitReaction Reaction, float Damage, const FVector& Launch = FVector::ZeroVector);
	/** lane-C: a Stagger finisher (Jab3, grapple strike C) thrown by the player at a student/NPC (not the tutorial sparring pawn) outside a mission is promoted to a
	 *  Knockdown (which the ragdoll turns into a visible launch). Inside a running mission the authored reaction is kept. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") bool bFinisherKnocksDownOutsideMissions = true;

	// --- tuning (Config) ----------------------------------------------------------------------------------
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Health") float PlayerMaxHealth = 100.f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Health") float EnemyMaxHealth = 60.f;
	/** Outgoing damage multipliers per team (the bully hits softer than Jimmy in the tutorial). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Health") float PlayerDamageScale = 1.f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Health") float EnemyDamageScale = 1.f;

	UPROPERTY(Config, EditAnywhere, Category = "Combat|Moves") TArray<FBullyCombatMove> Moves;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Moves") FName LightChainStart = TEXT("Jab1");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Moves") FName GroundKickMove = TEXT("GroundKick");
	/** Grapple strikes in order; the last one ends the grapple (victim staggers away). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Moves") TArray<FName> GrappleStrikeMoves;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Moves") FName GrapplePushMove = TEXT("GrapplePush");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Moves") FName HumiliateMove = TEXT("Humiliate");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Moves") FName GrappleMissMove = TEXT("GrappleMiss");

	// targeting (cm, deg)
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float LockOnRange = 900.f;
	/** Attack assist: an attack turns the attacker toward a target this close inside this cone (full width). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float AssistRange = 350.f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float AssistConeDeg = 150.f;
	/** Turn rate toward the target while winding up / locked on (deg/s). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float TrackTurnRate = 720.f;
	/** Lunges stop this far (cm, centre to centre) from the target. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float LungeStopDistance = 85.f;
	/** Locked-on movement speed (x the gait speed) and blocking movement speed. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float LockOnMoveScale = 0.55f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float BlockMoveScale = 0.0f;
	/** Ground kick: a downed target this close (cm, centre to centre) turns an attack into a ground kick. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Target") float GroundKickRange = 200.f;

	// block
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Block") float BlockArcDeg = 140.f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Block") float BlockDamageScale = 0.f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Block") float BlockStunTime = 0.25f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Block") float GuardMax = 100.f;
	/** Guard regen (per s) while not blocking a hit; guard break stun (s). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Block") float GuardRegen = 25.f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Block") float GuardBreakStun = 0.9f;
	/** A block raised less than this before the hit is a "perfect" block (no guard cost). 0 = off. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Block") float PerfectBlockWindow = 0.15f;

	// reactions (s)
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float FlinchTime = 0.38f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float StaggerTime = 0.85f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float DownedTime = 2.2f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float DownedHitExtend = 0.6f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float GetUpTime = 0.87f;
	/** Jimmy (the player team) is never out for good: this long after a KO he gets up again (once a ragdoll body has settled) with
	 *  PlayerKOHealthFraction of his health. 0 = stay down (a mission fail / retry resets him instead). Students and bullies stay down. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float PlayerKOGetUpSeconds = 4.5f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float PlayerKOHealthFraction = 0.5f;
	/** Consecutive flinches inside ComboResetTime before the victim is pushed out of range (stagger). 0 = never. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") int32 FlinchesBeforeStagger = 4;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") float ComboResetTime = 1.2f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") TArray<FName> FlinchClips;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") TArray<FName> StaggerClips;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName KnockdownClip = TEXT("HIT_FRONT_FALL_HARD");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName DownedLoopClip = TEXT("GROUND_ONBACK");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName GroundHitClip = TEXT("HIT_GROUND_HEAVY");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName GetUpClip = TEXT("GETUP");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName KOClip = TEXT("HIT_KO_COLLAPSE");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName KOLoopClip = TEXT("GROUND_HURT_IDLE");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName BeatenLoopClip = TEXT("STUNNED");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName BlockInClip = TEXT("BLOCK_IN");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName BlockLoopClip = TEXT("BLOCK_IDLE_B");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName BlockHitClip = TEXT("BLOCK_HIT_LIGHT");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|React") FName GuardBreakClip = TEXT("BLOCK_BREAK");

	/** Where the combat clips live (Jimmy's bank; other peds share the 36-joint skeleton, remapped by bone name at runtime). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Anim") FString ClipRoot = TEXT("/Game/Imported/Characters/Jimmy/Anims");

	// grapple
	/** Grab reach, centre to centre (cm). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") float GrappleRange = 130.f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") float GrappleConeDeg = 90.f;
	/** Centre-to-centre distance the victim is held at (cm). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") float GrappleHoldDistance = 62.f;
	/** The victim slips free after this long without a strike (s). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") float GrappleEscapeTime = 4.0f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") FName GrabGiverClip = TEXT("GRAP_GIV");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") FName GrabReceiverClip = TEXT("GRAP_RCV");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") FName HoldGiverClip = TEXT("GRAP_IDLE1_GIV");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") FName HoldReceiverClip = TEXT("GRAP_IDLE1_RCV");
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Grapple") FName EscapeClip = TEXT("GRAP_STAND_1_ESC_RCV");

	// feel
	/** Actor time dilation during hitstop (0.02 = near freeze). */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Feel") float HitStopDilation = 0.05f;
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Feel") bool bLogCombat = true;
	/** Camera-shake amplitude the player's camera gets when Jimmy LANDS a hit (x the reaction weight). 0 = off. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Feel") float ShakeOnHitLand = 0.45f;
	/** Camera-shake amplitude when the player TAKES a hit (x the reaction weight). Taking a hit shakes harder. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Feel") float ShakeOnHitTaken = 1.0f;
	/** Shake duration (s) for a combat impact. */
	UPROPERTY(Config, EditAnywhere, Category = "Combat|Feel") float ShakeDuration = 0.25f;

	// --- runtime state ---------------------------------------------------------------------------------------
	UPROPERTY(EditAnywhere, Category = "Combat") EBullyCombatTeam Team = EBullyCombatTeam::Player;
	/** Tutorial gates: invulnerable takes reactions but no damage; MinHealth > 0 floors health and enters Beaten. */
	UPROPERTY(EditAnywhere, Category = "Combat") bool bInvulnerable = false;
	UPROPERTY(EditAnywhere, Category = "Combat") float MinHealth = 0.f;
	float Health = 100.f;
	float MaxHealth = 100.f;
	float Guard = 100.f;

	// --- commands (input / AI / scripts) ------------------------------------------------------------------
	/** Attack button: chain / ground kick / grapple strike by context. Returns whether something started or was buffered. */
	bool RequestAttack();
	/** Grapple button: grab / push (while holding) / humiliate (target beaten). */
	bool RequestGrapple();
	void SetBlockHeld(bool bHeld);
	void SetLockOnHeld(bool bHeld);
	/** Starts a named move from the table (AI / scripts); false if busy or unknown. */
	bool StartMove(FName MoveName);
	/** Full reset (tutorial restart): health, state, partner. */
	void ResetFighter();

	// --- queries ---------------------------------------------------------------------------------------------
	EBullyCombatState GetState() const { return State; }
	static const TCHAR* StateName(EBullyCombatState S);
	bool IsDown() const { return State == EBullyCombatState::Downed || State == EBullyCombatState::KO; }
	bool IsOut() const { return State == EBullyCombatState::KO; }
	bool IsBusy() const { return State != EBullyCombatState::Idle && State != EBullyCombatState::Blocking; }
	bool CanMove() const;
	bool IsLockedOn() const { return bLockOnHeld && LockTarget.IsValid(); }
	float GetMoveSpeedScale() const;
	const FBullyCombatMove* GetCurrentMove() const { return State == EBullyCombatState::Attacking ? FindMove(CurrentMove) : nullptr; }
	FName GetCurrentMoveName() const { return State == EBullyCombatState::Attacking ? CurrentMove : NAME_None; }
	float GetStateTime() const { return StateTime; }
	bool IsInActiveWindow() const;
	UBullyCombatComponent* GetLockTarget() const { return LockTarget.Get(); }
	UBullyCombatComponent* GetGrapplePartner() const { return Partner.Get(); }
	/** Nearest opposing fighter that is not KO'd, within Range, inside ConeDeg of the facing (360 = any direction). */
	UBullyCombatComponent* FindTarget(float Range, float ConeDeg, bool bIncludeDown) const;
	bool GetPose(FBullyCombatPose& Out) const;
	ACharacter* GetCharacter() const;
	const FBullyCombatMove* FindMove(FName Name) const;
	int32 GetHitsLanded() const { return HitsLanded; }
	FString Describe() const;

	/** Every combat event of this fighter (also forwarded to the mission subsystem). */
	FBullyCombatEvent OnCombatEvent;
	/** All live fighters in all worlds (filtered by world on use). */
	static TArray<TWeakObjectPtr<UBullyCombatComponent>>& Registry();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	enum class EHitResult : uint8 { Ignored, Hit, Blocked };
	enum class EMoveCtx : uint8 { Normal, GrappleStrike, GrapplePush, Humiliate };

	void SetState(EBullyCombatState S);
	void PlayClip(FName Clip, bool bLoop, float Rate = 1.f, float Start = 0.f, FName FollowLoop = NAME_None);
	void BeginMove(const FBullyCombatMove& M, EMoveCtx Ctx = EMoveCtx::Normal);
	class UAnimSequence* ResolveClip(FName Clip);
	float ClipLength(FName Clip);
	void UpdateOrientation();
	void EndMove();
	void TickMove(float Dt);
	void TickHits(const FBullyCombatMove& M);
	void ApplyHitTo(UBullyCombatComponent* Victim, const FBullyCombatMove& M, float Damage);
	EHitResult ReceiveHit(UBullyCombatComponent* Attacker, const FBullyCombatMove& M, float Damage);
	void EnterReaction(EBullyHitReaction R, UBullyCombatComponent* Attacker, const FBullyCombatMove* M);
	void StartGrapple(UBullyCombatComponent* Victim);
	void ReleaseGrapple(bool bEscape);
	void HoldPartner();
	void FaceToward(const FVector& Where, float MaxStepDeg);
	void Push(const FVector& Dir, float Distance, float Time);
	void StartHitStop(float Seconds);
	/** lane-C: throw the body with the ragdoll component when the reaction asks for it. */
	bool TryRagdoll(EBullyHitReaction R, const FVector& Away, float Knock);
	/** The body may stand up: false while a ragdoll is still tumbling; otherwise any ragdoll is recovered and OutClip is the get-up clip to play. */
	bool RecoverRagdoll(FName& OutClip);
	void BeginGetUp(FName Clip);
	/** Kick the player's follow camera when Jimmy is in a hit (no-op for non-player fighters / no ABullyCharacter). */
	void ShakePlayerCamera(float Amplitude);
	void Fire(FName Event);
	void FireMission(FName Event);
	void Note(const FString& S) const;
	bool IsPlayer() const { return Team == EBullyCombatTeam::Player; }
	float DamageScale() const { return IsPlayer() ? PlayerDamageScale : EnemyDamageScale; }
	FName PickClip(const TArray<FName>& Clips, FName Fallback);

	EBullyCombatState State = EBullyCombatState::Idle;
	float StateTime = 0.f;       // game time in this state (dilated by hitstop)
	float StateDuration = 0.f;   // reactions: when the state ends
	FName CurrentMove;
	EMoveCtx MoveCtx = EMoveCtx::Normal;
	bool bInLightChain = false;
	bool bPairHitDone = false;
	TArray<TWeakObjectPtr<UBullyCombatComponent>> HitThisSwing;
	bool bBufferedAttack = false;
	bool bBlockHeld = false;
	bool bLockOnHeld = false;
	double BlockRaisedAt = -10.0;
	bool bSavedOrient = true;
	bool bOrientOverridden = false;
	TWeakObjectPtr<UBullyCombatComponent> LockTarget;
	TWeakObjectPtr<UBullyCombatComponent> MoveTarget;   // the target a move turned toward / lunged at
	TWeakObjectPtr<UBullyCombatComponent> Partner;      // grapple partner (either side)
	int32 GrappleStrikeIndex = 0;
	float GrappleIdleTime = 0.f;
	int32 ChainCount = 0;        // moves in the current light chain that landed
	int32 RecentFlinches = 0;
	double LastHitTakenAt = -10.0;
	int32 HitsLanded = 0;
	int32 FlinchPick = 0;

	UPROPERTY(Transient) TMap<FName, TObjectPtr<UAnimSequence>> ClipCache;
	TSet<FName> MissingClips;
	FBullyCombatPose Pose;
	FName FollowLoopClip;
	float PoseRate = 1.f;
	bool bPoseActive = false;

	FVector PendingLaunch = FVector::ZeroVector;   // ApplyWeaponHit(.., Launch): consumed by the reaction it triggers
	FVector PushVelocity = FVector::ZeroVector;
	float PushTimeLeft = 0.f;
	double HitStopUntil = -1.0;
};
