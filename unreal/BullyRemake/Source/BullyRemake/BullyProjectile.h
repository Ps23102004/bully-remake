// OWNER: slice-weapons
// Projectile for the weapon system. A sphere-collided, physics-moved actor configured at launch by
// UBullyWeaponComponent from Config/Weapons.json. On impact (or when its fuse runs out) it detonates: applies a
// reaction + damage to the fighter it hit, or to every fighter inside an AoE radius, then runs the weapon's status
// effect (knockback, slip, lingering gas, burn, itch/stun ticks). Visual: an optional local mesh, else an engine
// primitive sphere sized to the collision radius.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BullyCombatComponent.h"   // EBullyHitReaction
#include "BullyProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class UStaticMeshComponent;
class UStaticMesh;

enum class EBullyWeaponEffect : uint8 { None, Stun, Gas, Slip, Fire, Knockback };

struct FBullyProjectileSpec
{
	FName WeaponId;
	float Speed = 4000.f;          // cm/s (ranged: along the aim; thrown: fallback when there is no arc target)
	float GravityScale = 1.f;      // 0 = flat shot, 1 = full lob
	float Radius = 8.f;            // collision sphere (cm)
	float Lifetime = 5.f;          // fuse (s): detonates where it is if it hits nothing
	float Damage = 5.f;
	EBullyHitReaction Reaction = EBullyHitReaction::Flinch;
	float AoeRadius = 0.f;         // >0 = area blast
	EBullyWeaponEffect Effect = EBullyWeaponEffect::None;
	float EffectSeconds = 0.f;

	static EBullyHitReaction ParseReaction(const FString& S);
	static EBullyWeaponEffect ParseEffect(const FString& S);
	static const TCHAR* EffectName(EBullyWeaponEffect E);
};

UCLASS()
class ABullyProjectile : public AActor
{
	GENERATED_BODY()

public:
	ABullyProjectile();

	/** Launch from Origin with an explicit Velocity (cm/s). The shooter's own fighter is never hit. */
	void Launch(const FBullyProjectileSpec& InSpec, const FVector& Origin, const FVector& Velocity, UBullyCombatComponent* FiredBy);
	/** Visual: a local mesh if given, else the engine sphere scaled to the collision radius. */
	void SetVisual(UStaticMesh* Mesh, bool bIsFallbackSphere);

	// evidence counters (headless smoke reads these)
	static int32 TotalDetonations;
	static int32 TotalFighterHits;
	static int32 TotalEffectTicks;

protected:
	UFUNCTION()
	void OnHit(UPrimitiveComponent* HitComp, AActor* Other, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);
	virtual void LifeSpanExpired() override;

private:
	void Detonate(const FVector& At, UBullyCombatComponent* DirectHit);
	void ApplyEffect(const FVector& At, const TArray<TWeakObjectPtr<UBullyCombatComponent>>& Victims);

	UPROPERTY() TObjectPtr<USphereComponent> Collision;
	UPROPERTY() TObjectPtr<UProjectileMovementComponent> Movement;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> MeshComp;

	FBullyProjectileSpec Spec;
	TWeakObjectPtr<UBullyCombatComponent> Shooter;
	bool bDetonated = false;
};
