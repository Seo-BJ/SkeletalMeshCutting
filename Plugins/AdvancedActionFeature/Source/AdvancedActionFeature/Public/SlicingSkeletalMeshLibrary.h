// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "ProceduralMeshComponent.h"

#include "SlicingSkeletalMeshLibrary.generated.h"

USTRUCT()
struct FBoneWeightsInfo
{
	GENERATED_BODY()

	TArray<int32> InfluencingBoneIndices;

	TArray<float> BoneWeights;
    
};

/**
 * 
 */

enum class EProcMeshSliceCapOption : uint8;

UCLASS()
class ADVANCEDACTIONFEATURE_API USlicingSkeletalMeshLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	static void SliceProceduralMesh(UProceduralMeshComponent* InProcMesh, FVector PlanePosition,
	FVector PlaneNormal, bool bCreateOtherHalf, UProceduralMeshComponent*& OutOtherHalfProcMesh,
	EProcMeshSliceCapOption CapOption, UMaterialInterface* CapMaterial, TMap<uint32, uint32>& OutSlicedToBaseVertexIndex,
	TMap<uint32, uint32>& OutOtherSlicedToBaseVertexIndex);
	

	/** 
	 *	Slice the ProceduralMeshComponent (including simple convex collision) using a plane. Optionally create 'cap' geometry. 
	 *	@param	InProcMesh				ProceduralMeshComponent to slice
	 *	@param	PlanePosition			Point on the plane to use for slicing, in world space
	 *	@param	PlaneNormal				Normal of plane used for slicing. Geometry on the positive side of the plane will be kept.
	 *	@param	bCreateOtherHalf		If true, an additional ProceduralMeshComponent (OutOtherHalfProcMesh) will be created using the other half of the sliced geometry
	 *	@param	OutOtherHalfProcMesh	If bCreateOtherHalf is set, this is the new component created. Its owner will be the same as the supplied InProcMesh.
	 *	@param	CapOption				If and how to create 'cap' geometry on the slicing plane
	 *	@param	CapMaterial				If creating a new section for the cap, assign this material to that section
	 */
	static float GetBoneWeightForVertex(int32 VertexIndex, int32 TargetBoneIndex, const FSkelMeshRenderSection* SkelMeshRenderSection,
		const FSkeletalMeshLODRenderData* LODRenderData, const FSkinWeightVertexBuffer* SkinWeightBuffer);

	
	static FBoneWeightsInfo GetBoneWeightsForVertex(int32 VertexIndex,
	const FSkelMeshRenderSection* SkelMeshRenderSection, const FSkeletalMeshLODRenderData* LODRenderData,
	const FSkinWeightVertexBuffer* SkinWeightBuffer);
	
	/**
	  * 특정 Procedural Mesh의 버텍스들에 대해 지정된 본의 Bone Weight를 계산합니다.
	  * @param InSkelComp 원본 스켈레탈 메쉬 컴포넌트.
	  * @param TargetGlobalBoneIndex Bone Weight를 계산할 대상 본의 글로벌 인덱스 (RefSkeleton 기준).
	  * @param NewPMCVertToOldPreSlicePMCVertMap SliceProceduralMesh에서 반환된 맵. Key: 새 PMC 버텍스 인덱스, Value: 슬라이스 전 PMC 로컬 인덱스.
	  * @param OriginalSkelToPreSlicePMCMap GetFilteredSkeletalMeshDataByBoneName에서 생성된 맵. Key: 원본 스켈레탈 메쉬 글로벌 인덱스, Value: 슬라이스 전 PMC 로컬 인덱스.
	  * @return TMap<int32, float> Key: 새 PMC 버텍스 인덱스, Value: 해당 버텍스의 TargetGlobalBoneIndex에 대한 Bone Weight.
	  */
	static TMap<uint32, float> GetBoneWeightMapForProceduralVertices(
	const int32 TargetLODIndex,
	const USkeletalMeshComponent* InSkelComp,
	int32 TargetGlobalBoneIndex,
	const TMap<uint32, uint32>& SlicePmcToOriginalPmc,
	const TMap<uint32, uint32>& OriginalPmcToSkeletal) ;


	static TMap<uint32, FBoneWeightsInfo> GetBoneWeightsMapForProceduralVertices(
	const int32 TargetLODIndex,
	const USkeletalMeshComponent* InSkelComp,
	int32 TargetGlobalBoneIndex,
	const TMap<uint32, uint32>& SlicePmcToOriginalPmc,
	const TMap<uint32, uint32>& OriginalPmcToSkeletal) ;


};