// OWNER: slice-weapons
// Original projectile for the weapon system. A sphere-collided, physics-moved actor configured at spawn by
// UBullyWeaponComponent from Config/Weapons.json. On impact (or fuse end) it detonates: applies a reaction + damage to
// the combat component it hits, or to every fighter inside an AoE radius, and tags a status effect. No game assets:
// the visual is an optional local mesh (set via SetMesh) or nothing.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BullyCombatComponent.h"   // EBullyHitReaction
#include "BullyProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class UStaticMeshComponent;

UENUM()
enum class EBullyWeaponEffect : uint8 { None, Stun, Gas, Slip, Fire, Knockback };

USTRUCT()
struct FBullyProjectileSpec
{
	GENERATED_BODY()

	float Speed = 4000.f;          // cm/s along the fire direction
	float GravityScale = 1.f;      // 0 = flat shot, 1 = full lob
	float Radius = 8.f;            // collision sphere (cm)
	float Lifetime = 5.f;          // fuse (s) if it hits nothing
	float Damage = 5.f;
	EBullyHitReaction Reaction = EBullyHitReaction::Flinch;
	float AoeRadius = 0.f;         // >0 = detonate as an area blast
	EBullyWeaponEffect Effect = EBullyWeaponEffect::None;
	float EffectSeconds = 0.f;
	FName WeaponId;
};

UCLASS()
class ABullyProjectile : public AActor
{
	GENERATED_BODY()

public:
	ABullyProjectile();

	/** Launch from Origin along Dir (unit). Instigator's own combat component is spared. */
	void Launch(const FBullyProjectileSpec& InSpec, const FVector& Origin, const FVector& Dir, UBullyCombatComponent* FiredBy);
	/** Optional local (git-ignored) visual mesh; no-op if null. */
	void SetMesh(UStaticMesh* Mesh);

protected:
	UFUNCTION()
	void OnHit(UPrimitiveComponent* HitComp, AActor* Other, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

	virtual void BeginPlay() override;

private:
	void Detonate(const FVector& At, UBullyCombatComponent* DirectHit);

	UPROPERTY() USphereComponent* Collision = nullptr;
	UPROPERTY() UProjectileMovementComponent* Movement = nullptr;
	UPROPERTY() UStaticMeshComponent* MeshComp = nullptr;

	FBullyProjectileSpec Spec;
	TWeakObjectPtr<UBullyCombatComponent> Shooter;
	bool bDetonated = false;
};
