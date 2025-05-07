// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "SkelToProcMeshComponent.generated.h"

class USkeletalMeshComponent;
class UProceduralMeshComponent;
struct FProcMeshTangent; 
class FSkeletalMeshLODRenderData;
enum class EProcMeshSliceCapOption : uint8;

USTRUCT(BlueprintType)
struct FProceduralVertexSkinningData
{
    GENERATED_BODY()

    // 이 프로시저럴 버텍스에 영향을 주는 원본 스켈레탈 메시의 본 인덱스 배열
    UPROPERTY()
    TArray<int32> BoneMapIndices; // 스켈레탈 메쉬의 RefSkeleton 본 인덱스

    // 각 본에 대한 가중치 배열 (BoneMapIndices와 순서 일치)
    UPROPERTY()
    TArray<float> BoneWeights;

    // 프로시저럴 메시의 로컬 공간(또는 컴포넌트 공간의 바인드 포즈)에서의 원본 버텍스 위치
    UPROPERTY()
    FVector LocalBindPosePosition;

    FProceduralVertexSkinningData() : LocalBindPosePosition(FVector::ZeroVector) {}
};

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class ADVANCEDACTIONFEATURE_API USkelToProcMeshComponent : public UActorComponent
{
    GENERATED_BODY()

public:

    USkelToProcMeshComponent();
    
    UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Procedural Mesh", meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UProceduralMeshComponent> ProceduralMeshComponent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Procedural Mesh|Runtime Skinning")
    bool bEnableRuntimeSkinning = false;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Procedural Mesh", meta = (ClampMin = "0"))
    int32 LODIndexToCopy = 0;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Procedural Mesh")
    bool bConvertOnBeginPlay = true;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Procedural Mesh")
    bool bCopyVertexColors = true;
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Procedural Mesh")
    UMaterialInterface* CapMaterialInterface;
    
    // true이면 최종 포즈 지오메트리를 기반으로 노멀을 다시 계산합니다.
    // false이면 기본 스켈레탈 메시의 노멀을 복사합니다 (더 빠르지만 변형된 형태에는 덜 정확할 수 있음).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Procedural Mesh")
    bool bRecalculateNormals = false; // 변형된 메시의 노멀 품질을 높이려면 true로 설정

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Procedural Mesh")
    FName BoneName;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Procedural Mesh")
    FName ProceduralMeshAttachSocketName;
    
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Procedural Mesh")
    FName OtherHalfMeshAttachSocketName;
    
    UPROPERTY(EditDefaultsOnly, Category = "Procedural Mesh")
    float CreateProceduralMeshDistance;

    UPROPERTY(EditDefaultsOnly, Category = "Procedural Mesh")
    float Threshold = 0.01;

    UPROPERTY(EditDefaultsOnly, Category = "Procedural Mesh")
    float ImpulseMagnitude = 10000000;
    
    UFUNCTION(BlueprintCallable, Category = "Procedural Mesh")
    bool ConvertSkeletalMeshToProceduralMesh(bool bForceNewPMC, FName TargetBoneName);

    UFUNCTION(BlueprintCallable, Category = "Procedural Mesh|Runtime Skinning")
    void UpdateProceduralMeshesSkinning();

protected:

    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;


private:


    /** Procedural Mesh Component를 가져오거나 생성하는 헬퍼 함수 */
    bool SetupProceduralMeshComponent(bool bForceNew);

    /** Skeletal Mesh LOD 섹션에서 Procedural Mesh로 메쉬 데이터를 복사하는 함수 */
    bool CopySkeletalLODToProcedural(USkeletalMeshComponent* SkelComp, FName TargetBoneName, int32 LODIndex);

    /** Skeletal Mesh LOD에서 필요한 데이터 버퍼를 추출하는 함수 */
    // 아래 함수 시그니처 변경: 스키닝 정보 빌드를 위해 OriginalToProcVertexMap을 반환하도록 함

    bool GetFilteredSkeletalMeshDataByBoneName(
        const USkeletalMeshComponent* SkelComp,
        FName TargetBoneName,
        float MinWeight,
        int32 LODIndex,
        TArray<FVector>& OutVertices,       // 출력: 버텍스 위치 배열
        TArray<FVector>& OutNormals,        // 출력: 노멀 배열
        TArray<FProcMeshTangent>& OutTangents, // 출력: 탄젠트 배열
        TArray<FVector2D>& OutUV0,          // 출력: UV0 배열
        TArray<FLinearColor>& OutVertexColors, // 출력: 버텍스 컬러 배열
        TArray<int32>& SectionMaterialIndices, // 출력: 각 섹션의 머티리얼 인덱스 배열
        TArray<TArray<int32>>& SectionIndices, // 출력: 각 섹션의 인덱스(트라이앵글) 배열
        TMap<uint32, uint32>& OutOriginalToProcVertexMap // 이 맵을 채워서 반환
        );

    

    bool SliceMesh(
        UProceduralMeshComponent* InProcMesh,
        FVector PlanePosition,
        FVector PlaneNormal,
        bool bCreateOtherHalf,
        UProceduralMeshComponent*& OutOtherHalfProcMesh,
        EProcMeshSliceCapOption CapOption,
        UMaterialInterface* CapMaterial
        );


    
    float GetBoneWeightForVertex(int32 VertexIndex, int32 TargetBoneIndex,  const FSkelMeshRenderSection* SkelMeshRenderSection, const FSkeletalMeshLODRenderData* LODRenderData, const FSkinWeightVertexBuffer* SkinWeightBuffer);

    /** 소유자에서 대상 Skeletal Mesh Component를 찾는 헬퍼 함수 */
    USkeletalMeshComponent* GetOwnerSkeletalMeshComponent() const;

    /**
     * 원본 스켈레탈 메시 컴포넌트에서 특정 본에 연결된 버텍스들을 숨깁니다.
     * @param SourceSkeletalMeshComp 숨길 대상 스켈레탈 메시 컴포넌트
     * @param LODIndex 처리할 LOD 인덱스
     * @param TargetBoneName 숨길 기준이 되는 본의 이름
     * @param Threshold 가중치 임계값 (이 값보다 커야 숨김 처리)
     * @param bClearOverride 숨길 버텍스가 없을 때 이전 오버라이드를 제거할지 여부
     * @return 성공 여부
     */
    
    bool HideOriginalMeshVerticesByBone(USkeletalMeshComponent* SourceSkeletalMeshComp, int32 LODIndex, FName TargetBoneName, bool bClearOverride = true);
    
  /**
     * Procedural Mesh 생성을 위해 필터링된 버텍스들의 스키닝 정보를 빌드합니다.
     * @param SkelComp 소스 스켈레탈 메시 컴포넌트
     * @param LODIndex 대상 LOD 인덱스
     * @param InProcMeshVertices 프로시저럴 메쉬를 구성하는 로컬 바인드 포즈상의 버텍스 위치들
     * @param InOriginalToProcVertexMap 원본 스켈레탈 메쉬 버텍스 인덱스에서 현재 프로시저럴 메쉬의 버텍스 인덱스로의 매핑
     * @param OutSkinningData 결과를 저장할 스키닝 데이터 배열
     * @return 성공 여부
     */
    bool BuildSkinningDataForProceduralMesh(
        const USkeletalMeshComponent* SkelComp,
        int32 LODIndex,
        const TArray<FVector>& InProcMeshVertices,
        const TMap<uint32, uint32>& InOriginalToProcVertexMap, // Key: Original SkelMesh VertexIdx, Value: ProcMesh VertexIdx for InProcMeshVertices
        TArray<FProceduralVertexSkinningData>& OutSkinningData);

    /**
     * 원본 스켈레탈 메시의 특정 버텍스에 대한 스킨 웨이트 정보를 가져옵니다.
     * @param SkelComp 소스 스켈레탈 메시 컴포넌트
     * @param OriginalSkelVertexIndex 스키닝 정보를 가져올 원본 스켈레탈 메시의 버텍스 인덱스
     * @param LODRenderData 해당 LOD의 렌더 데이터
     * @param SkinWeightBuffer 스킨 웨이트 버퍼
     * @param OutVertexSkinningData 결과를 저장할 구조체 (BoneMapIndices, BoneWeights 채워짐)
     * @return 성공 여부
     */
    bool GetSkinWeightsForOriginalVertex(
        const USkeletalMeshComponent* SkelComp,
        uint32 OriginalSkelVertexIndex,
        const FSkeletalMeshLODRenderData& LODRenderData, // 직접 포함 대신 전방선언 후 cpp에서 include
        const FSkinWeightVertexBuffer* SkinWeightBuffer, // 직접 포함 대신 전방선언 후 cpp에서 include
        FProceduralVertexSkinningData& OutVertexSkinningData);


    // --- 멤버 변수 추가 ---

    // 주 프로시저럴 메시 컴포넌트에 대한 스키닝 데이터
    UPROPERTY()
    TArray<FProceduralVertexSkinningData> MainProcMeshSkinningData;

    // 절단된 다른 쪽 프로시저럴 메시 컴포넌트에 대한 스키닝 데이터
    UPROPERTY()
    TArray<FProceduralVertexSkinningData> OtherHalfProcMeshSkinningData;

    // 원본 스켈레탈 메시의 각 본에 대한 역 바인드 포즈 변환 행렬 (컴포넌트 공간 기준)
    UPROPERTY()
    TArray<FMatrix> RefBoneInverseBindMatrices;

    // 생성된 다른 쪽 프로시저럴 메시 컴포넌트의 참조 (업데이트를 위해 저장)
    UPROPERTY()
    TObjectPtr<UProceduralMeshComponent> OtherHalfProceduralMeshComponent;


    // GetFilteredSkeletalMeshDataByBoneName에서 생성되는, 원본 스켈레탈 메쉬 버텍스 인덱스 -> Procedural Mesh 버텍스 인덱스 맵.
    // 스키닝 정보 빌드 시 필요.
    // Key: Original SkelMesh Vertex Index, Value: ProcMesh Vertex Index (MainProcMesh 용)
    TMap<uint32, uint32> OriginalToMainProcVertexMap;

    // (OtherHalf 용 맵도 필요하다면 선언. SliceMesh의 결과에 따라 복잡도가 달라짐)
    // TMap<uint32, uint32> OriginalToOtherHalfProcVertexMap;

};


/*
*지오메트리 데이터 (Geometry Data - 메시 자체):
버텍스 (Vertices): 3D 공간에서의 점들로, 메시의 기본 형태를 정의합니다. 각 버텍스는 위치(X, Y, Z 좌표) 정보를 가집니다.
인덱스 (Indices) / 트라이앵글 (Triangles): 버텍스들을 연결하여 면(주로 삼각형)을 만드는 방법을 정의합니다. 이 인덱스 배열은 렌더링 파이프라인이 어떤 버텍스들을 연결해서 삼각형을 그릴지 알려줍니다.
UV 좌표 (UV Coordinates): 2D 텍스처 이미지를 3D 모델 표면에 매핑하는 방법을 정의하는 2차원 좌표입니다. 텍스처링(질감 입히기)에 필수적입니다. 여러 UV 채널을 가질 수 있습니다 (예: 라이트맵 UV).
노멀 (Normals): 각 버텍스에서 표면이 바라보는 방향을 나타내는 벡터입니다. 빛 계산(셰이딩)에 사용되어 표면의 명암과 입체감을 표현하는 데 중요합니다.
탄젠트 및 바이노멀 (Tangents and Binormals/Bitangents): 노멀과 함께 표면의 로컬 좌표계를 정의하는 벡터입니다. 주로 노멀 매핑과 같은 고급 셰이딩 기법에서 텍스처 공간의 방향을 정의하는 데 사용됩니다.
버텍스 컬러 (Vertex Colors): 각 버텍스에 저장할 수 있는 색상(RGBA) 정보입니다. 텍스처 블렌딩, 간단한 셰이딩, 특정 영역 마스크 등 다양한 효과에 활용될 수 있습니다.

*스켈레톤 데이터 (Skeleton Data - 뼈대):
본 (Bones) / 조인트 (Joints): 메시를 움직이게 하는 가상의 뼈대 구조입니다. 각 본은 이름과 부모 본에 대한 상대적인 위치, 회전, 스케일 값(트랜스폼)을 가집니다.
계층 구조 (Hierarchy): 본들 간의 부모-자식 관계입니다. 예를 들어, 상완골(Upper Arm)이 움직이면 하완골(Lower Arm)과 손(Hand) 본이 따라서 움직이는 것은 이 계층 구조 때문입니다. 루트 본(Root Bone)에서 시작하여 트리 구조를 이룹니다.
바인드 포즈 (Bind Pose): 스켈레톤과 메시가 처음 연결될 때의 기본 자세(Neutral Pose)입니다. 스키닝 계산의 기준이 되는 상태입니다.

* 스키닝 데이터 (Skinning Data - 메시와 뼈대 연결 정보):
스킨 웨이트 (Skin Weights) / 버텍스 가중치 (Vertex Weights/Influences): 스켈레탈 메시의 핵심입니다. 각 버텍스가 어떤 본(Bone)에 얼마나 많은 영향을 받는지를 정의하는 가중치 값입니다. 하나의 버텍스는 여러 본의 영향을 받을 수 있으며 (예: 팔꿈치 버텍스는 상완골과 하완골 모두의 영향을 받음), 보통 한 버텍스에 대한 모든 본 가중치의 합은 1.0이 됩니다. 이 가중치 정보에 따라 본이 움직일 때 버텍스가 부드럽게 따라 움직이며 메시가 변형됩니다.

*기타 연관 데이터:
애니메이션 데이터 (Animation Data): 스켈레톤의 각 본이 시간에 따라 어떻게 움직이는지를 정의합니다. (종종 별도의 에셋으로 저장되지만 스켈레탈 메시와 밀접하게 연관됩니다.)
LODs (Level of Detail): 카메라와의 거리에 따라 다른 복잡도(폴리곤 수)를 가진 메시 버전들을 여러 개 준비하여 성능을 최적화하기 위한 데이터입니다.
머티리얼 슬롯 / 섹션 (Material Slots / Sections): 메시의 어떤 부분이 어떤 머티리얼(재질)을 사용하는지에 대한 정보입니다. 메시는 종종 여러 섹션으로 나뉘고, 각 섹션은 특정 머티리얼 슬롯에 할당됩니다.
*/

