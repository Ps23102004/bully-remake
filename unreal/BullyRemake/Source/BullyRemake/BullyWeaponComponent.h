// OWNER: slice-weapons
// The player's arsenal. Loads Config/Weapons.json, tracks ammo per weapon, cycles the equipped weapon (Fists = none)
// and fires ABullyProjectiles with third-person aim:
//   ranged: toward whatever the camera's centre ray hits (or the lock-on target);
//   thrown: a ballistic arc onto the lock-on target / the camera-ray point, else a fixed-pitch lob.
// Every weapon starts unlocked with its starting ammo (console BRGiveWeapons refills). -BRWeaponSmoke runs a headless
// check that fires each weapon at a spawned target and logs PASS/FAIL per weapon.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BullyProjectile.h"
#include "BullyWeaponComponent.generated.h"

class UStaticMesh;
class ABullyCombatPawn;

struct FBullyWeaponDef
{
	FName Id;
	FString Name;
	bool bThrown = false;
	FBullyProjectileSpec Spec;
	int32 AmmoStart = 10;
	int32 AmmoMax = 20;
	float FireInterval = 0.5f;     // s between shots
	float ThrowPitchDeg = 18.f;    // thrown, no target: lob this far above the camera ray
	FString MeshPath;              // optional local projectile mesh; empty -> engine sphere
};

UCLASS(ClassGroup = (Bully), meta = (BlueprintSpawnableComponent))
class UBullyWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBullyWeaponComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	bool HasEquipped() const { return Defs.IsValidIndex(Equipped); }
	const FBullyWeaponDef* GetEquipped() const { return HasEquipped() ? &Defs[Equipped] : nullptr; }
	int32 GetAmmo(FName Id) const { const int32* A = Ammo.Find(Id); return A ? *A : 0; }
	const TArray<FBullyWeaponDef>& GetDefs() const { return Defs; }

	/** Step +1/-1 through Fists and every weapon that has ammo. */
	void Cycle(int32 Dir);
	/** Equip by id (NAME_None = Fists). False if unknown or out of ammo. */
	bool Equip(FName Id);
	/** Fire the equipped weapon along the current aim. False if nothing was fired (holstered, cooldown, busy, empty). */
	bool Fire();
	/** Fire the equipped weapon at a world point (smoke tests, scripted throws). */
	bool FireAt(const FVector& Target);
	void RefillAll();

	/** "WEAPON  Slingshot  30/40" for the local player's HUD (empty if there is no local armed player). */
	static FString HudLine();

private:
	bool LoadDefs();
	bool CanFireNow() const;
	bool LaunchEquipped(const FVector& Target, bool bHasTarget);
	FVector MuzzleLocation() const;
	bool AimPoint(FVector& Out) const;
	UStaticMesh* VisualFor(const FBullyWeaponDef& D, bool& bIsFallback);
	void TickSmoke();

	TArray<FBullyWeaponDef> Defs;
	TMap<FName, int32> Ammo;
	int32 Equipped = -1;           // index into Defs, -1 = Fists
	double NextFireAt = 0.0;

	UPROPERTY(Transient) TMap<FName, TObjectPtr<UStaticMesh>> MeshCache;
	UPROPERTY(Transient) TObjectPtr<UStaticMesh> FallbackSphere;

	struct FSmoke
	{
		bool bEnabled = false;
		int32 Step = 0;
		double StepStart = 0.0;
		int32 Weapon = 0;
		int32 Passes = 0;
		TArray<FString> Failures;
		int32 HitsBefore = 0;
		int32 DetonationsBefore = 0;
		float HealthBefore = 0.f;
		TWeakObjectPtr<ABullyCombatPawn> Target;
	} Smoke;

	static TWeakObjectPtr<UBullyWeaponComponent> LocalArmed;
};
