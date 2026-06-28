#include "UvHSClientPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Camera/CameraTypes.h"

AUvHSClientPawn::AUvHSClientPawn()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	CameraComp = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComp->SetupAttachment(SceneRoot);
	
	CameraComp->SetRelativeLocation(FVector(0, 0, CameraHeightCm));
	CameraComp->SetRelativeRotation(CameraRotation);

	CameraComp->ProjectionMode = ECameraProjectionMode::Orthographic;
	CameraComp->OrthoWidth     = OrthoWidthCm;
	
	CameraComp->bUsePawnControlRotation = false;
}

void AUvHSClientPawn::BeginPlay()
{
	Super::BeginPlay();
	
	if (CameraComp)
	{
		CameraComp->SetRelativeLocation(FVector(0, 0, CameraHeightCm));
		CameraComp->ProjectionMode = ECameraProjectionMode::Orthographic;
		CameraComp->OrthoWidth     = OrthoWidthCm;
	}
	
	SetActorLocation(FVector::ZeroVector);
}
