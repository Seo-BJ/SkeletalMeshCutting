#include "SkelToProcMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "ProceduralMeshConversion.h" // FProcMeshTangent에 필요
#include "KismetProceduralMeshLibrary.h" // RecalculateNormalsAndTangents에 필요
#include "../../../../../../../../../../Program Files/Epic Games/UE_5.5/Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.12/include/oodle2net.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h" // GetOwner()에 필요

#include "DrawDebugHelpers.h"

USkelToProcMeshComponent::USkelToProcMeshComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // 기본적으로 틱은 필요 없음
}

void USkelToProcMeshComponent::BeginPlay()
{
    Super::BeginPlay();

    if (bConvertOnBeginPlay)
    {
        ConvertSkeletalMeshToProceduralMesh(false, BoneName);
    }
}

bool USkelToProcMeshComponent::ConvertSkeletalMeshToProceduralMesh(bool bForceNewPMC /*= false*/, FName TargetBoneName)
{
    USkeletalMeshComponent* SkelComp = GetOwnerSkeletalMeshComponent();
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: 유효하지 않은 Skeletal Mesh Component 또는 Asset입니다. 변환할 수 없습니다."));
        return false;
    }

    // PMC 설정 (생성 또는 가져오기)
    if (!SetupProceduralMeshComponent(bForceNewPMC))
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: Procedural Mesh Component 설정에 실패했습니다. 변환할 수 없습니다."));
        return false;
    }

    ProceduralMeshComponent->SetWorldLocation(SkelComp->GetComponentLocation());
    ProceduralMeshComponent->SetWorldRotation(SkelComp->GetComponentRotation());

    // 렌더 데이터 사용 가능 여부 확인
    if (!SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering())
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: Skeletal Mesh Asset '%s'에 사용 가능한 렌더 리소스가 없습니다."), *SkelComp->GetSkeletalMeshAsset()->GetName());
        return false;
    }

    // 데이터 복사 진행
    bool bSuccess = CopySkeletalLODToProcedural(SkelComp, TargetBoneName, LODIndexToCopy);

    if(bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: Skeletal Mesh LOD %d를 Procedural Mesh로 성공적으로 변환했습니다."), LODIndexToCopy);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: Skeletal Mesh LOD %d 데이터 복사에 실패했습니다."), LODIndexToCopy);
    }

    return bSuccess;
}

bool USkelToProcMeshComponent::SetupProceduralMeshComponent(bool bForceNew)
{
    AActor* Owner = GetOwner();
    if (!Owner) return false;

    if (bForceNew && ProceduralMeshComponent)
    {
        // 강제로 새로 생성하는 경우 기존 PMC 파괴
        ProceduralMeshComponent->DestroyComponent();
        ProceduralMeshComponent = nullptr;
    }

    if (!ProceduralMeshComponent)
    {
        // 할당되지 않은 경우 기존 컴포넌트를 먼저 찾아봅니다.
        ProceduralMeshComponent = Owner->FindComponentByClass<UProceduralMeshComponent>();

        if(!ProceduralMeshComponent)
        {
             // 새 PMC 생성
            ProceduralMeshComponent = NewObject<UProceduralMeshComponent>(Owner, TEXT("GeneratedProceduralMesh"));
            if (ProceduralMeshComponent)
            {
                ProceduralMeshComponent->RegisterComponent();
                // 원하는 경우 씬 루트나 다른 컴포넌트에 어태치합니다.
                USceneComponent* OwnerRoot = Owner->GetRootComponent();
                if(OwnerRoot)
                {
                    ProceduralMeshComponent->AttachToComponent(OwnerRoot, FAttachmentTransformRules::KeepRelativeTransform);
                }
                UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: 새 ProceduralMeshComponent를 생성했습니다."));
            }
            else
            {
                 UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: ProceduralMeshComponent 생성에 실패했습니다."));
                 return false;
            }
        }
         else
        {
             UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: 기존 ProceduralMeshComponent를 찾았습니다."));
        }
    }

    // 이전 지오메트리 제거
    ProceduralMeshComponent->ClearAllMeshSections();
    
    return (ProceduralMeshComponent != nullptr);
}

bool USkelToProcMeshComponent::CopySkeletalLODToProcedural(const USkeletalMeshComponent* SkelComp, FName TargetBoneName, int32 LODIndex)
{
    // --- 1. 메쉬 데이터 추출 ---
    TArray<FVector> Vertices;           // 버텍스 위치 배열
    TArray<FVector> Normals;            // 노멀 배열
    TArray<FProcMeshTangent> Tangents;  // 탄젠트 배열
    TArray<FVector2D> UV0;              // UV0 배열
    TArray<FLinearColor> VertexColors;  // 버텍스 컬러 배열
    TArray<int32> SectionMaterialIndices; // 각 섹션에서 사용하는 머티리얼 인덱스
    TArray<TArray<int32>> SectionIndices; // 각 섹션의 트라이앵글 인덱스

    bool bDataExtracted = GetFilteredSkeletalMeshDataByBoneName(SkelComp, TargetBoneName, 0.f, LODIndex, Vertices, Normals, Tangents, UV0, VertexColors, SectionMaterialIndices, SectionIndices);

    if (!bDataExtracted || Vertices.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("SkelToProcMeshComponent: LOD %d에서 메쉬 데이터 추출에 실패했습니다."), LODIndex);
        return false;
    }

     // --- 선택 사항: 노멀 재계산 ---
    if (bRecalculateNormals)
    {
        // 참고: 여기서 노멀을 재계산하는 것은 느릴 수 있습니다. 모든 섹션을 일시적으로 결합해야 합니다.
        // 이 기본 예제는 *전체* 버텍스 버퍼를 기반으로 재계산하므로, 섹션 사이에 하드 에지가 있는 경우 이상적이지 않을 수 있습니다.
        // 더 견고한 솔루션은 섹션별로 재계산하거나 더 고급 노멀 계산 라이브러리를 사용하는 것입니다.

        // 계산을 위해 인덱스 결합 (모두 동일한 버텍스 버퍼를 참조한다고 가정)
        TArray<int32> AllIndices;
        for(const TArray<int32>& Indices : SectionIndices)
        {
            AllIndices.Append(Indices);
        }

        if(AllIndices.Num() > 0)
        {
            // 노멀과 탄젠트 모두 재계산
            UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices, AllIndices, UV0, Normals, Tangents);
            UE_LOG(LogTemp, Log, TEXT("SkelToProcMeshComponent: LOD %d의 노멀과 탄젠트를 재계산했습니다."), LODIndex);
        }
    }
    
    // --- 2. Procedural Mesh 섹션 생성 ---
    for (int32 SectionIdx = 0; SectionIdx < SectionIndices.Num(); ++SectionIdx)
    {
        if (SectionIndices[SectionIdx].Num() > 0) // 해당 섹션에 인덱스가 있는 경우에만 생성
        {
            // 메쉬 섹션 생성
            ProceduralMeshComponent->CreateMeshSection_LinearColor(
                SectionIdx,                     // 섹션 인덱스
                Vertices,                       // 이 LOD의 모든 버텍스
                SectionIndices[SectionIdx],     // 이 섹션의 트라이앵글 인덱스
                Normals,                        // 모든 버텍스의 노멀
                UV0,                            // 모든 버텍스의 UV0
                VertexColors,                   // 모든 버텍스의 버텍스 컬러 (선택 사항)
                Tangents,                       // 모든 버텍스의 탄젠트
                false                           // bCreateCollision - 콜리전이 필요하면 true로 설정
            );

            // --- 3. 머티리얼 적용 ---
            if (SectionMaterialIndices.IsValidIndex(SectionIdx))
            {
                int32 MaterialIndex = SectionMaterialIndices[SectionIdx];
                UMaterialInterface* Material = SkelComp->GetMaterial(MaterialIndex); // 컴포넌트에서 머티리얼 인스턴스 가져오기

                 // 컴포넌트가 재정의하지 않은 경우 메시 에셋 머티리얼로 폴백
                if (!Material && SkelComp->GetSkeletalMeshAsset()->GetMaterials().IsValidIndex(MaterialIndex))
                {
                     Material = SkelComp->GetSkeletalMeshAsset()->GetMaterials()[MaterialIndex].MaterialInterface;
                }

                if (Material)
                {
                    ProceduralMeshComponent->SetMaterial(SectionIdx, Material); // PMC 섹션에 머티리얼 설정
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: 섹션 %d의 머티리얼 인덱스 %d에 대한 머티리얼을 찾을 수 없습니다."), SectionIdx, MaterialIndex);
                }
            }
        }
    }

    return true; // 성공적으로 완료
}


bool USkelToProcMeshComponent::GetFilteredSkeletalMeshDataByBoneName(const USkeletalMeshComponent* SkelComp, FName TargetBoneName, float MinWeight, int32 LODIndex, TArray<FVector>& OutVertices,
    TArray<FVector>& OutNormals, TArray<FProcMeshTangent>& OutTangents, TArray<FVector2D>& OutUV0, TArray<FLinearColor>& OutVertexColors,
    TArray<int32>& OutSectionMaterialIndices, TArray<TArray<int32>>& OutSectionIndices)
{
    // 0. 필수 컴포넌트 및 데이터 유효성 검사
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset() || !SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering()) return false;
    
    FSkeletalMeshRenderData* RenderData = SkelComp->GetSkeletalMeshAsset()->GetResourceForRendering();
    if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex)) // LOD 인덱스 유효성 검사
    {
        UE_LOG(LogTemp, Warning, TEXT("LODRenderData[%d]이 유효하지 않습니다."), LODIndex);
        return false; 
    }
    FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[LODIndex];
    
    // 1. Bone Name으로 버텍스 필터링
    int32 TargetBoneIndex = INDEX_NONE;
    bool bShouldFilter = (TargetBoneName != NAME_None);
    const FSkinWeightVertexBuffer* SkinWeightBufferPtr = nullptr; // 필터링 시에만 사용

    if (bShouldFilter)
    {
        TargetBoneIndex = SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton().FindBoneIndex(TargetBoneName);
        if (TargetBoneIndex == INDEX_NONE)
        {
            UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshData: Target Bone '%s' not found. Filtering disabled."), *TargetBoneName.ToString());
            bShouldFilter = false;
        }
        else
        {
            if (LODRenderData.SkinWeightVertexBuffer.GetNumVertices() == 0)
            {
                UE_LOG(LogTemp, Error, TEXT("GetFilteredSkeletalMeshData: SkinWeightVertexBuffer not valid for LOD %d. Cannot filter."), LODIndex);
                return false; // 스킨 웨이트 없으면 필터링 불가
            }
            SkinWeightBufferPtr = &LODRenderData.SkinWeightVertexBuffer; // 유효하면 포인터 할당
            UE_LOG(LogTemp, Log, TEXT("GetFilteredSkeletalMeshData: Filtering by bone '%s' (Index: %d), MinWeight: %f"), *TargetBoneName.ToString(), TargetBoneIndex, MinWeight);
        }
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("GetFilteredSkeletalMeshData: No bone filter applied. Extracting all vertices for LOD %d."), LODIndex);
    }
    
    // --- 스키닝된 버텍스 위치 가져오기 ---
    // *포즈가 적용된* 메쉬를 얻기 위한 핵심 부분
    
    if(LODRenderData.GetNumVertices() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: LOD %d에 대해 GetNumVertices가 0개의 버텍스를 반환했습니다."), LODIndex);
        return false; // 버텍스가 없으면 실패 처리
    }


    // --- 정적 버퍼 가져오기 (노멀, 탄젠트, UV, 컬러) ---
    // 참고: 이들은 일반적으로 *기본* 메쉬의 데이터이며, 나중에 bRecalculateNormals가 true가 아닌 이상 프레임마다 동적으로 재계산되지 않습니다.
    FStaticMeshVertexBuffers& StaticVertexBuffers = LODRenderData.StaticVertexBuffers;
    uint32 NumVertices = StaticVertexBuffers.PositionVertexBuffer.GetNumVertices(); // 버텍스 수 가져오기

    // 버퍼 크기 적절하게 조정
    OutNormals.SetNumUninitialized(NumVertices);
    OutTangents.SetNumUninitialized(NumVertices);
    OutUV0.SetNumUninitialized(NumVertices);
    
    if (bCopyVertexColors && StaticVertexBuffers.ColorVertexBuffer.IsInitialized())
    {
        // 버텍스 컬러 복사 옵션이 켜져 있고 컬러 버퍼가 초기화된 경우 크기 설정
        OutVertexColors.SetNumUninitialized(NumVertices);
    } else {
        // 그렇지 않으면 버텍스 컬러 배열 비우기
        OutVertexColors.Empty();
    }
    
    // Filter 전 후 Vertex Index를 비교하기 위한 TMap
    TMap<uint32, uint32> FilteredVertexIndexMap;
    uint32 FilteredVertexIndex = 0;
    // Section 별 버텍스 처리

    for (const FSkelMeshRenderSection& Section : LODRenderData.RenderSections)
    {
        const uint32 SectionNumvertices = Section.NumVertices;
        const uint32 SectionBaseVertexIndex = Section.BaseVertexIndex;
        {
            for (uint32 i = 0; i < SectionNumvertices; i++)
            {
                const uint32 VertexIndex = i + SectionBaseVertexIndex;
                if (IsVertexInfluencedByBone(VertexIndex, TargetBoneIndex, 0, SkinWeightBufferPtr))
                {
                    // Vertex 처리
                    const FVector SkinnedVectorPosition = FVector(StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex));
                    OutVertices.Add(SkinnedVectorPosition);
                    
                    // FilteredVertexIndexMap 업데이트
                    FilteredVertexIndexMap.Add(VertexIndex, FilteredVertexIndex);
                    FilteredVertexIndex += 1;
                    
                    // 노멀 (압축된 FPackedNormal로 저장됨)
                    // 버퍼 유형에 따라 FPackedNormal/FVector3f 변환 필요
                    const FVector Normal = FVector(StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(VertexIndex)); // Z는 노멀 포함
                    OutNormals.Add(Normal);
                    
                    const FVector TangentX = FVector(StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(VertexIndex)); // X는 탄젠트 포함
                    auto ProcMeshTangent = FProcMeshTangent(TangentX, false);
                    ProcMeshTangent.TangentX = TangentX;
                    OutTangents.Add(ProcMeshTangent);

                    // 외적과 부호를 사용하여 바이노멀(TangentY) 계산
                    // FVector TangentY = FVector::CrossProduct(Normal, TangentX) * TangentSign;
                    // CreateMeshSection용 FProcMeshTangent에는 TangentY를 직접 저장하지 않지만, Normal과 TangentX로부터 파생됨
                    
                    // UV (단순화를 위해 UV 채널 1개만 가정)
                    // 버퍼 유형에 따라 FVector2f 변환 필요
                    const FVector2D VertexUV = FVector2D(StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(VertexIndex, 0));
                    OutUV0.Add(VertexUV);

                    // 버텍스 컬러 (FColor로 저장됨)
                    if (bCopyVertexColors && StaticVertexBuffers.ColorVertexBuffer.IsInitialized() && StaticVertexBuffers.ColorVertexBuffer.GetNumVertices() > VertexIndex)
                    {
                        // 버텍스 컬러 복사 옵션이 켜져 있고, 버퍼가 초기화되었으며, 유효한 인덱스인 경우
                        OutVertexColors.Add(StaticVertexBuffers.ColorVertexBuffer.VertexColor(VertexIndex).ReinterpretAsLinear()); // FColor를 FLinearColor로 변환하여 저장
                    }
                }
            }
        }
    }
    

    // --- 인덱스 재구성 단계 (섹션 구조 유지) ---

    // 원본 인덱스 버퍼 데이터 가져오기
    TArray<uint32> IndexBuffer;
    LODRenderData.MultiSizeIndexContainer.GetIndexBuffer(IndexBuffer);

    const int32 NumSections = LODRenderData.RenderSections.Num();

    // 출력 배열 크기 설정 및 초기화 (OutSectionIndices는 TArray<TArray<int32>> 타입)
    OutSectionIndices.Empty(NumSections); // 이전 데이터 비우기
    OutSectionIndices.SetNum(NumSections); // 섹션 수만큼 내부 TArray 생성

    // 원본 머티리얼 인덱스 저장 (CopySkeletalLODToProcedural에서 사용하기 위해)
    OutSectionMaterialIndices.Empty(NumSections);
    OutSectionMaterialIndices.SetNum(NumSections);

    // 섹션별 루프
    for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
    {
        const FSkelMeshRenderSection& Section = LODRenderData.RenderSections[SectionIdx];
        const int32 NumTrianglesInSection = Section.NumTriangles;

        // 현재 섹션의 머티리얼 인덱스 저장
        OutSectionMaterialIndices[SectionIdx] = Section.MaterialIndex;

        // 현재 섹션의 필터링된 인덱스를 저장할 내부 배열 준비 (예상 크기 지정 가능)
        OutSectionIndices[SectionIdx].Empty(NumTrianglesInSection * 3);

        // 현재 섹션의 트라이앵글별 루프
        for (int32 TriIdx = 0; TriIdx < NumTrianglesInSection; ++TriIdx)
        {
            uint32 IndexOffset = Section.BaseIndex + TriIdx * 3;

            // 인덱스 버퍼 범위 안전 확인
            if (!IndexBuffer.IsValidIndex(IndexOffset + 2))
            {
                UE_LOG(LogTemp, Error, TEXT("Section %d: Index Buffer access out of bounds! Offset: %u"), SectionIdx, IndexOffset);
                continue; // 이 트라이앵글 건너뛰기
            }

            // 1. 원본 버텍스 인덱스 읽기
            uint32 OrigIdx0 = IndexBuffer[IndexOffset + 0];
            uint32 OrigIdx1 = IndexBuffer[IndexOffset + 1];
            uint32 OrigIdx2 = IndexBuffer[IndexOffset + 2];

            // 2. 세 원본 인덱스가 모두 FilteredVertexIndexMap에 있는지 확인 (Find 사용)
            const uint32* PtrNewIdx0 = FilteredVertexIndexMap.Find(OrigIdx0);
            const uint32* PtrNewIdx1 = FilteredVertexIndexMap.Find(OrigIdx1);
            const uint32* PtrNewIdx2 = FilteredVertexIndexMap.Find(OrigIdx2);

            // 3. 모두 존재하면, 새로운 인덱스를 현재 섹션의 출력 배열에 추가
            if (PtrNewIdx0 != nullptr && PtrNewIdx1 != nullptr && PtrNewIdx2 != nullptr)
            {
                OutSectionIndices[SectionIdx].Add(*PtrNewIdx0); // 새 인덱스 추가
                OutSectionIndices[SectionIdx].Add(*PtrNewIdx1);
                OutSectionIndices[SectionIdx].Add(*PtrNewIdx2);
            }
            // else: 트라이앵글의 버텍스 중 하나라도 필터링되었다면, 이 트라이앵글은 결과에 포함되지 않음
        }
    }

    // 중요: 모든 처리가 끝난 후, 일부 섹션의 인덱스 배열이 비어있을 수 있습니다.
    // CopySkeletalLODToProcedural 함수에서는 비어있지 않은 섹션에 대해서만 CreateMeshSection을 호출해야 합니다.

    // 필터링 후 유효한 트라이앵글이 하나도 없는 경우 체크 (선택 사항)
    bool bFoundAnyTriangles = false;
    for(const auto& Indices : OutSectionIndices)
    {
        if (Indices.Num() > 0)
        {
            bFoundAnyTriangles = true;
            break;
        }
    }
    if (!bFoundAnyTriangles && OutVertices.Num() > 0) // 버텍스는 있는데 삼각형이 없으면
    {
         UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshDataByBoneName: No triangles remained after filtering."));
         // return false; // 여기서 실패 처리할 수도 있음
    }
    return true; // 성
}


bool USkelToProcMeshComponent::IsVertexInfluencedByBone(int32 VertexIndex, int32 TargetBoneIndex, float MinWeight, const FSkinWeightVertexBuffer* SkinWeightBuffer)
{
    if (!SkinWeightBuffer || TargetBoneIndex == INDEX_NONE)
    {
        return false; // 유효하지 않은 버퍼 또는 본 인덱스
    }

    const int32 MaxInfluences = SkinWeightBuffer->GetMaxBoneInfluences(); // 이 버텍스에 영향을 줄 수 있는 최대 본 개수

    // 버텍스에 영향을 주는 각 본 확인
    for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; ++InfluenceIdx)
    {
        // GetBoneIndex와 GetBoneWeight는 CPU 접근 가능한 버퍼에서만 안전하게 호출 가능
        const float BoneWeight = SkinWeightBuffer->GetBoneWeight(VertexIndex, InfluenceIdx);

        // 웨이트가 0이면 더 이상 유효한 영향 없음 (보통 웨이트 순으로 정렬됨)
        if (BoneWeight <= 0.0f)
        {
            break;
        }

        if (BoneWeight >= MinWeight)
        {
            const int32 BoneIndex = SkinWeightBuffer->GetBoneIndex(VertexIndex, InfluenceIdx);
            if (BoneIndex == TargetBoneIndex)
            {
                return true; // 목표 본의 영향을 충분히 받음
            }
        }
    }

    return false; // 목표 본의 영향을 받지 않음

    
}




USkeletalMeshComponent* USkelToProcMeshComponent::GetOwnerSkeletalMeshComponent() const
{
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: 소유자 액터(Owner Actor)를 찾을 수 없습니다."));
        return nullptr;
    }

    // 소유자에서 첫 번째 Skeletal Mesh Component를 찾습니다.
    USkeletalMeshComponent* SkelComp = Owner->FindComponentByClass<USkeletalMeshComponent>();
    if (!SkelComp)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: 소유자 액터 '%s'에 SkeletalMeshComponent가 없습니다."), *Owner->GetName());
        return nullptr;
    }
    if (!SkelComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: '%s'의 SkeletalMeshComponent에 SkeletalMesh가 할당되지 않았습니다."), *Owner->GetName());
        return nullptr;
    }

    return SkelComp;
}
