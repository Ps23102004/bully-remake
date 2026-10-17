// OWNER: mission-fw
// Persistent player progress: the reward loop's wallet. Money and respect accumulate across missions and persist for
// the whole session (a GameInstance subsystem outlives individual worlds / level loads). Missions pay out through the
// mission framework's `reward` action; the mission HUD reads these to show a running total. Nothing game-specific is
// hardcoded here beyond the two counters; it is the minimal economy primitive the objective->reward loop needs.
//
// Console (testing): Progress.Status  (log money/respect) | Progress.Money <n> (add) | Progress.Respect <n> (add) |
//                    Progress.Reset   (zero both).
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "BullyProgressSubsystem.generated.h"

UCLASS()
class UBullyProgressSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static UBullyProgressSubsystem* Get(const UObject* WorldContext);

	void AddMoney(int32 Delta);
	void AddRespect(int32 Delta);
	void ResetProgress();

	int32 GetMoney() const { return Money; }
	int32 GetRespect() const { return Respect; }

	/** Fires whenever Money or Respect changes (the HUD listens to refresh its wallet readout). */
	DECLARE_MULTICAST_DELEGATE(FProgressChanged);
	FProgressChanged OnChanged;

private:
	int32 Money = 0;
	int32 Respect = 0;
};
