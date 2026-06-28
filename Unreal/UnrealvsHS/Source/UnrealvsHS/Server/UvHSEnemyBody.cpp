#include "UvHSEnemyBody.h"
#include "Gameplay/UvHSConstants.h"
#include "Client/UvHSWorldRenderer.h"  // UnrealsPerMeter / EntityZ
#include "Components/SphereComponent.h"

AUvHSEnemyBody::AUvHSEnemyBody()
{
	PrimaryActorTick.bCanEverTick = false;

	Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("Sphere"));
	RootComponent = Sphere;
	
	Sphere->InitSphereRadius(UnrealvsHS::Constants::EnemyRadius * UnrealvsHS::Client::FUvHSWorldRenderer::UnrealsPerMeter);
	Sphere->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Sphere->SetCollisionObjectType(ECC_Pawn);
	Sphere->SetCollisionResponseToAllChannels(ECR_Block);
	Sphere->SetGenerateOverlapEvents(false);
	
	Sphere->BodyInstance.bSimulatePhysics  = true;
	Sphere->BodyInstance.bEnableGravity    = false;
	Sphere->BodyInstance.bOverrideMass     = true;
	Sphere->BodyInstance.SetMassOverride(UnrealvsHS::Constants::EnemyMass, true);
	Sphere->BodyInstance.LinearDamping     = UnrealvsHS::Constants::EnemyLinearDamping;
	Sphere->BodyInstance.AngularDamping    = 0.0f;
	Sphere->BodyInstance.bLockZTranslation = true;
	Sphere->BodyInstance.bLockXRotation    = true;
	Sphere->BodyInstance.bLockYRotation    = true;
	Sphere->BodyInstance.bLockZRotation    = true;
}

void AUvHSEnemyBody::SetPlanarPosition(FVector2D PosM)
{
	const float U = UnrealvsHS::Client::FUvHSWorldRenderer::UnrealsPerMeter;
	const float Z = UnrealvsHS::Client::FUvHSWorldRenderer::EntityZ;
	SetActorLocation(FVector((double)PosM.X * U, (double)PosM.Y * U, Z), false, nullptr, ETeleportType::TeleportPhysics);
}

FVector2D AUvHSEnemyBody::GetPlanarPosition() const
{
	const FVector L = GetActorLocation();
	const float U = UnrealvsHS::Client::FUvHSWorldRenderer::UnrealsPerMeter;
	return FVector2D(L.X / (double)U, L.Y / (double)U);
}

FVector2D AUvHSEnemyBody::GetPlanarVelocity() const
{
	if (!Sphere) return FVector2D::ZeroVector;
	const FVector V = Sphere->GetPhysicsLinearVelocity();
	const float U = UnrealvsHS::Client::FUvHSWorldRenderer::UnrealsPerMeter;
	return FVector2D(V.X / (double)U, V.Y / (double)U);
}

void AUvHSEnemyBody::AddPlanarForce(FVector2D ForceN)
{
	if (!Sphere) return;
	const float U = UnrealvsHS::Client::FUvHSWorldRenderer::UnrealsPerMeter;
	Sphere->AddForce(FVector((double)ForceN.X * U, (double)ForceN.Y * U, 0.0), NAME_None, false);
}
