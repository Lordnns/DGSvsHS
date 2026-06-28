
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Net/WireTypes.h"
#include "UvHSClientPlayerController.generated.h"

UCLASS()
class UNREALVSHS_API AUvHSClientPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AUvHSClientPlayerController();
	
	UnrealvsHS::Wire::FInputCmd SampleInput(uint32 ClientTick, const FVector2D& PredictedPlayerPos) const;

protected:
	virtual void BeginPlay() override;
};
