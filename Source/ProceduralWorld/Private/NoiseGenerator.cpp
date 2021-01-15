// Fill out your copyright notice in the Description page of Project Settings.


#include "NoiseGenerator.h"

#include "ProceduralMeshComponent.h"
#include "KismetProceduralMeshLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"

// Sets default values for this component's properties
ANoiseGenerator::ANoiseGenerator()
{
	const int ChunksNumber = ChunkNumberX * ChunkNumberY;

	PrimaryActorTick.bCanEverTick = false;

	NoiseGen.SetFractalType(FastNoiseLite::FractalType_FBm);
	NoiseGen.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);


	World.Reserve(ChunksNumber);

	for (int i = 0; i < ChunksNumber; ++i)
	{
		const FName ObjectName = *FString::Printf(TEXT("TerrainMesh%i"), i);
		World.Add(CreateDefaultSubobject<UProceduralMeshComponent>(ObjectName));
		World[i]->bUseAsyncCooking = true;
	}
	RootComponent = World[0];
}


TArray<float> ANoiseGenerator::CreateNoiseData()
{
	TArray<float> NoiseData;
	float MinNoiseValue = 0;
	float MaxNoiseValue = 0;
	const float HalfNoiseArrayWidth = NoiseArrayWidth / 2;
	const float HalfNoiseArrayHeight = NoiseArrayHeight / 2;

	NoiseData.Reserve(NoiseArrayWidth * NoiseArrayHeight);
	UpdateGenerator();

	if (NoiseScale <= 0)
		NoiseScale = 0.0001f;

	for (int y = 0; y < NoiseArrayHeight; y++)
	{
		for (int x = 0; x < NoiseArrayWidth; x++)
		{
			const float SampleX = (x - HalfNoiseArrayWidth) * NoiseScale + GlobalOffsetX;
			const float SampleY = (y - HalfNoiseArrayHeight) * NoiseScale + GlobalOffsetY;
			const float NoiseValue = NoiseGen.GetNoise(SampleX, SampleY);
			NoiseData.Add(NoiseValue);
			if (MinNoiseValue > NoiseValue)
				MinNoiseValue = NoiseValue;
			if (MaxNoiseValue < NoiseValue)
				MaxNoiseValue = NoiseValue;
		}
	}

	for (int y = 0; y < NoiseArrayHeight; y++)
	{
		for (int x = 0; x < NoiseArrayWidth; x++)
		{
			NoiseData[x + y * NoiseArrayHeight] = UKismetMathLibrary::NormalizeToRange(
				NoiseData[x + y * NoiseArrayHeight], MinNoiseValue, MaxNoiseValue);
		}
	}

	return NoiseData;
}

UTexture2D* ANoiseGenerator::CreateNoiseMap(TArray<float> NoiseArray)
{
	UTexture2D* NoiseMap = UTexture2D::CreateTransient(NoiseArrayWidth, NoiseArrayHeight);
	TArray<FColor> ColorMap;

	ColorMap.Reserve(NoiseArrayWidth * NoiseArrayHeight);

	for (int y = 0; y < NoiseArrayHeight; y++)
	{
		for (int x = 0; x < NoiseArrayWidth; x++)
		{
			const int Grayscale = FMath::Lerp(0, 255, NoiseArray[x + y * NoiseArrayHeight]);
			ColorMap.Add(FColor(Grayscale, Grayscale, Grayscale));
		}
	}

	if (!NoiseMap)
		return nullptr;

#if WITH_EDITORONLY_DATA
	NoiseMap->MipGenSettings = TMGS_NoMipmaps;
#endif
	NoiseMap->NeverStream = true;
	NoiseMap->SRGB = 0;

	FTexture2DMipMap& Mip = NoiseMap->PlatformData->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);

	FMemory::Memcpy(Data, ColorMap.GetData(), ColorMap.GetAllocatedSize());
	Mip.BulkData.Unlock();
	NoiseMap->UpdateResource();

	return NoiseMap;
}

void ANoiseGenerator::GenerateTerrain(UProceduralMeshComponent* Terrain, TArray<float> NoiseArray, int ChunkOffsetX,
                                      int ChunkOffsetY)
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector2D> UV;
	TArray<FVector> Normals;
	TArray<FProcMeshTangent> Tangents;
	TArray<FColor> Colors;
	const float StartingPositionX = (ChunkOffsetX) ? ChunkOffsetX - 1 : 0;
	const float StartingPositionY = (ChunkOffsetY) ? ChunkOffsetY - 1 : 0;

	// The numbers are number of times array is accessed inside loop
	Vertices.Reserve(NoiseArrayWidth * NoiseArrayHeight);
	Triangles.Reserve(6 * MapTileHeight * MapTileWidth);
	UV.Reserve(NoiseArrayWidth * NoiseArrayHeight);
	Normals.Reserve(NoiseArrayWidth * NoiseArrayHeight);
	Tangents.Reserve(NoiseArrayWidth * NoiseArrayHeight);
	Colors.Reserve(NoiseArrayWidth * NoiseArrayHeight);

	ActorMutex.Lock();
	UE_LOG(LogTemp, Warning, TEXT("Terrain generation thread calculations: %s"), *Terrain->GetName());
	UE_LOG(LogTemp, Warning, TEXT("Terrain generation: StartingPositionX - %f"), StartingPositionX);
	UE_LOG(LogTemp, Warning, TEXT("Terrain generation: StartingPositiony - %f"), StartingPositionY);
	ActorMutex.Unlock();

	// UE_LOG(LogTemp, Warning, TEXT("Terrain generation: size calculation - %d"), 4 * MapTileHeight * MapTileWidth);

	/* Mesh building schematic. First triangle is TL->BL->TR, second one is TR->BL->BR.
	 * TL---TR x++
	 * |  /  |
	 * BL---BR
	 * y++;
	 */

	// First double loop allocates Vertex coordinate. There are 2 loops because triangles need immediate access to vertices.
	for (int y = 0; y <= MapTileHeight; y++)
	{
		for (int x = 0; x <= MapTileWidth; x++)
		{
			const int Height = HeightMultiplier * HeightCurve->GetFloatValue(NoiseArray[x + y * NoiseArrayHeight]);
			Vertices.Add(FVector(StartingPositionX + VertexSize * x, StartingPositionY + VertexSize * y, Height));
			UV.Add(FVector2D(0.0f + x * 1.0f, 0.0f + y * 1.0f));
		}
	}

	// Second double loop combines correct vertices into triangles. 
	for (int y = 0; y < MapTileHeight; y++)
	{
		for (int x = 0; x < MapTileWidth; x++)
		{
			// TL
			Triangles.Add(x + y * NoiseArrayHeight);
			// BL
			Triangles.Add(x + (y + 1) * NoiseArrayHeight);
			// TR
			Triangles.Add(x + 1 + y * NoiseArrayHeight);
			// TR
			Triangles.Add(x + 1 + y * NoiseArrayHeight);
			// BL
			Triangles.Add(x + (y + 1) * NoiseArrayHeight);
			// BR
			Triangles.Add(x + 1 + (y + 1) * NoiseArrayHeight);
		}
	}
	// UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices, Triangles, UV, Normals, Tangents);

	ActorMutex.Lock();
	AsyncTask(ENamedThreads::GameThread, [=]()
	{
		Terrain->CreateMeshSection(0, Vertices, Triangles, Normals, UV, Colors, Tangents, false);
		Terrain->SetMaterial(0, TerrainMaterial);
		// ReSharper disable once CppExpressionWithoutSideEffects
		// Terrain->ContainsPhysicsTriMeshData(true);
	});
	ActorMutex.Unlock();

	UE_LOG(LogTemp, Warning, TEXT("Terrain generation thread completed: %s"), *Terrain->GetName());
}

// Called when the game starts
void ANoiseGenerator::BeginPlay()
{
	// Super::BeginPlay();
	if (!HeightCurve)
	{
		UE_LOG(LogTemp, Error, TEXT("Terrain generation: HeightCurve not set"));
		return;
	}
	if (bApplyRandomSeed)
		RandomiseSeed();

	TArray<float> NoiseArray = CreateNoiseData();

	for (int y = 0; y < ChunkNumberY; y++)
	{
		for (int x = 0; x < ChunkNumberX; x++)
		{
			AsyncTask(ENamedThreads::HighTaskPriority, [&, NoiseArray, x, y]()
			{
				GenerateTerrain(World[x + y * ChunkNumberY], NoiseArray, x * MapTileWidth, y * MapTileHeight);
			});
		}
	}
}

void ANoiseGenerator::UpdateGenerator()
{
	NoiseGen.SetFractalOctaves(Octaves);
	NoiseGen.SetFractalLacunarity(Lacunarity);
}

void ANoiseGenerator::RandomiseSeed()
{
	Seed = rand();
	NoiseGen.SetSeed(Seed);
}

// Called every frame
void ANoiseGenerator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
