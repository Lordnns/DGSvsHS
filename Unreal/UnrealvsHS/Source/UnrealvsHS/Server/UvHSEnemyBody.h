

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UvHSEnemyBody.generated.h"

class USphereComponent;

UCLASS()
class UNREALVSHS_API AUvHSEnemyBody : public AActor
{
	GENERATED_BODY()

public:
	AUvHSEnemyBody();
	
	void     SetPlanarPosition(FVector2D PosM);
	FVector2D GetPlanarPosition() const;
	FVector2D GetPlanarVelocity() const;
	
	void AddPlanarForce(FVector2D ForceN);

	UPROPERTY(VisibleAnywhere)
	USphereComponent* Sphere = nullptr;
};
