
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "UvHSClientPawn.generated.h"

class UCameraComponent;
class USceneComponent;

UCLASS()
class UNREALVSHS_API AUvHSClientPawn : public APawn
{
	GENERATED_BODY()

public:
	AUvHSClientPawn();
	
	UPROPERTY(EditAnywhere, Category="Camera")
	float OrthoWidthCm = 6000.0f;

	UPROPERTY(EditAnywhere, Category="Camera")
	float CameraHeightCm = 4000.0f;

	UPROPERTY(EditAnywhere, Category="Camera")
	FRotator CameraRotation = FRotator(-90.0f, 0.0f, 0.0f); 
	
protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category="Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category="Components")
	TObjectPtr<UCameraComponent> CameraComp;
};
