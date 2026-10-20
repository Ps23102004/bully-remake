// OWNER: slice-weapons
#include "BullyProjectile.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogBullyWeapon, Log, All);

ABullyProjectile::ABullyProjectile()
{
	PrimaryActorTick.bCanEverTick = false;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(8.f);
	Collision->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	Collision->SetNotifyRigidBodyCollision(true);   // hit events
	Collision->OnComponentHit.AddDynamic(this, &ABullyProjectile::OnHit);
	RootComponent = Collision;

	MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	MeshComp->SetupAttachment(Collision);
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->bRotationFollowsVelocity = true;
	Movement->bShouldBounce = false;
	Movement->InitialSpeed = 0.f;   // set on Launch
}

void ABullyProjectile::BeginPlay()
{
	Super::BeginPlay();
	SetLifeSpan(FMath::Max(0.25f, Spec.Lifetime));   // fuse: detonate on expiry via EndPlay path below if not yet hit
}

void ABullyProjectile::SetMesh(UStaticMesh* Mesh)
{
	if (MeshComp && Mesh) { MeshComp->SetStaticMesh(Mesh); }
}

void ABullyProjectile::Launch(const FBullyProjectileSpec& InSpec, const FVector& Origin, const FVector& Dir, UBullyCombatComponent* FiredBy)
{
	Spec = InSpec;
	Shooter = FiredBy;
	Collision->SetSphereRadius(FMath::Max(1.f, Spec.Radius));
	SetActorLocation(Origin);
	const FVector D = Dir.GetSafeNormal();
	if (Movement)
	{
		Movement->ProjectileGravityScale = Spec.GravityScale;
		Movement->InitialSpeed = Spec.Speed;
		Movement->MaxSpeed = Spec.Speed * 1.2f;
		Movement->Velocity = D * Spec.Speed;
	}
	// Do not collide with whoever fired it.
	if (FiredBy)
	{
		if (AActor* OwnerActor = FiredBy->GetOwner()) { Collision->IgnoreActorWhenMoving(OwnerActor, true); }
	}
	UE_LOG(LogBullyWeapon, Display, TEXT("BR_WEAPON projectile %s launched spd=%.0f grav=%.2f aoe=%.0f dmg=%.0f"),
		*Spec.WeaponId.ToString(), Spec.Speed, Spec.GravityScale, Spec.AoeRadius, Spec.Damage);
}

void ABullyProjectile::OnHit(UPrimitiveComponent*, AActor* Other, UPrimitiveComponent*, FVector, const FHitResult& Hit)
{
	if (bDetonated) { return; }
	UBullyCombatComponent* DirectHit = Other ? Other->FindComponentByClass<UBullyCombatComponent>() : nullptr;
	Detonate(Hit.ImpactPoint.IsNearlyZero() ? GetActorLocation() : Hit.ImpactPoint, DirectHit);
}

void ABullyProjectile::Detonate(const FVector& At, UBullyCombatComponent* DirectHit)
{
	if (bDetonated) { return; }
	bDetonated = true;
	UWorld* W = GetWorld();
	int32 Hits = 0;

	auto HitOne = [&](UBullyCombatComponent* C)
	{
		if (!C || C == Shooter.Get()) { return; }
		C->ApplyWeaponHit(Spec.Reaction, Spec.Damage);
		++Hits;
	};

	if (Spec.AoeRadius > 0.f && W)
	{
		// Area blast: every fighter whose owner is within the radius.
		TArray<AActor*> Actors;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (!A) { continue; }
			if (FVector::DistSquared(A->GetActorLocation(), At) <= FMath::Square(Spec.AoeRadius))
			{
				HitOne(A->FindComponentByClass<UBullyCombatComponent>());
			}
		}
	}
	else
	{
		HitOne(DirectHit);
	}

	UE_LOG(LogBullyWeapon, Display, TEXT("BR_WEAPON %s detonated at %s: %d fighter(s) hit, effect=%d for %.1fs"),
		*Spec.WeaponId.ToString(), *At.ToCompactString(), Hits, int32(Spec.Effect), Spec.EffectSeconds);
	// TODO(assets/fx): spawn the local effect VFX/SFX here; status effects (gas/slip/fire/stun) applied as timers.
	Destroy();
}
