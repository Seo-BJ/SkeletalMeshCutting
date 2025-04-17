#include "SkelToProcMeshComponent.h"

#include "KismetProceduralMeshLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "ProceduralMeshConversion.h" // FProcMeshTangent에 필요
#include "../../../../../../../../../../Program Files/Epic Games/UE_5.5/Engine/Plugins/Compression/OodleNetwork/Sdks/2.9.12/include/oodle2net.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h" 
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

bool USkelToProcMeshComponent::CopySkeletalLODToProcedural(USkeletalMeshComponent* SkelComp, FName TargetBoneName, int32 LODIndex)
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
            UE_LOG(LogTemp, Warning, TEXT("Vertices: %d, Normals: %d, UV0: %d, VertexColors: %d, Tangents: %d"), Vertices.Num(), Normals.Num(), UV0.Num(), VertexColors.Num(), Tangents.Num());

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

    // --- 1. 본 위치 확인 및 메쉬 슬라이스 ---
    // 본/소켓 존재 여부 먼저 확인하는 것이 더 안전합니다.
    if (!SkelComp->DoesSocketExist(TargetBoneName)) {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: Target Bone or Socket '%s' does not exist on SkelComp!"), *TargetBoneName.ToString());
        return false;
    }

    FVector BoneLocation = SkelComp->GetBoneLocation(TargetBoneName);
    // GetBoneLocation이 ZeroVector를 반환하는 경우가 유효한 경우도 있으므로, 존재 여부 체크 후 ZeroVector 체크는 선택적.
    if (BoneLocation == FVector::ZeroVector && TargetBoneName != NAME_None) {
        UE_LOG(LogTemp, Warning, TEXT("SliceAndAttach: Got ZeroVector for Bone '%s' location. Ensure this is correct."), *TargetBoneName.ToString());
    }

    UProceduralMeshComponent* OtherHalfMesh = nullptr;
    SliceMesh(ProceduralMeshComponent, BoneLocation, FVector::UpVector, /*CreateOtherHalf=*/ true, OtherHalfMesh, EProcMeshSliceCapOption::CreateNewSectionForCap, CapMaterialInterface);

    if (!OtherHalfMesh) {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: SliceMesh failed for bone '%s'. OtherHalfMesh is null."), *TargetBoneName.ToString());
        return false;
    }
        UE_LOG(LogTemp, Log, TEXT("SliceMesh successful for bone '%s'."), *TargetBoneName.ToString());


    // --- 2. 원본 메쉬의 해당 부분 숨기기 ---
    // HideOriginalMeshVerticesByBone 함수는 Threshold 값을 내부적으로 사용하거나 파라미터로 받아야 합니다.
    bool bHidden = HideOriginalMeshVerticesByBone(SkelComp, LODIndex, TargetBoneName /*, Threshold - 필요시 추가 */);
    if (bHidden) {
        UE_LOG(LogTemp, Log, TEXT("Successfully requested hiding of original vertices for bone '%s'."), *TargetBoneName.ToString());
    } else {
        UE_LOG(LogTemp, Warning, TEXT("Failed to hide original vertices for bone '%s'."), *TargetBoneName.ToString());
        // 숨기기 실패 시 진행 여부 결정 필요
    }


    // --- 3. PMC 조각들 설정 및 부착 ---

    // 생성된 PMC 조각들은 물리 시뮬레이션을 하지 않고 부모 스켈레톤을 따라가도록 설정
    ProceduralMeshComponent->SetSimulatePhysics(false);
    OtherHalfMesh->SetSimulatePhysics(false);

    // 콜리전 설정 (부착된 상태에서 어떻게 상호작용할지 결정)
    // 예: 쿼리만 가능하게 하거나, 아예 끄거나. 부모가 레그돌되면 부모의 콜리전을 따름.
    ProceduralMeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryOnly); // 또는 NoCollision
    OtherHalfMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly); // 또는 NoCollision
    UE_LOG(LogTemp, Display, TEXT("Physics Disabled for PMC pieces. Collision set to QueryOnly."));


    // 부착할 소켓/본 이름 유효성 검사
    if (ProceduralMeshAttachSocketName.IsNone() || !SkelComp->DoesSocketExist(ProceduralMeshAttachSocketName)) {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: ProceduralMeshAttachSocketName '%s' is None or does not exist!"), *ProceduralMeshAttachSocketName.ToString());
        // 대체 본 이름(예: TargetBoneName) 사용 또는 실패 처리
        // ProceduralMeshAttachSocketName = TargetBoneName; // 예시: 대체
        return false; // 또는 실패 처리
    }
    if (OtherHalfMeshAttachSocketName.IsNone() || !SkelComp->DoesSocketExist(OtherHalfMeshAttachSocketName)) {
        UE_LOG(LogTemp, Error, TEXT("SliceAndAttach: OtherHalfMeshAttachSocketName '%s' is None or does not exist!"), *OtherHalfMeshAttachSocketName.ToString());
        // 대체 본 이름(예: TargetBoneName) 사용 또는 실패 처리
        // OtherHalfMeshAttachSocketName = TargetBoneName; // 예시: 대체
        return false; // 또는 실패 처리
    }

    // 각 PMC 조각을 지정된 소켓/본에 부착
    UE_LOG(LogTemp, Log, TEXT("Attaching ProceduralMeshComponent to '%s' and OtherHalfMesh to '%s'."), *ProceduralMeshAttachSocketName.ToString(), *OtherHalfMeshAttachSocketName.ToString());

    // 보통 슬라이스 후에는 KeepRelativeTransform이 원래 형태를 유지하는 데 유리합니다.
    ProceduralMeshComponent->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, ProceduralMeshAttachSocketName);
    OtherHalfMesh->AttachToComponent(SkelComp, FAttachmentTransformRules::KeepWorldTransform, OtherHalfMeshAttachSocketName);

    UE_LOG(LogTemp, Log, TEXT("PMC Attachment completed."));


    // --- 4. 부모 스켈레탈 메쉬 컴포넌트 레그돌화 ---
    // 이 부분은 부모 SkelComp 전체를 레그돌 상태로 만들고 싶을 때 활성화합니다.
    // SkelComp에 Physics Asset이 제대로 설정되어 있어야 합니다.

    UE_LOG(LogTemp, Log, TEXT("Initiating ragdoll on parent SkelComp '%s'..."), *SkelComp->GetName());

    // 레그돌 콜리전 프로파일 설정
    SkelComp->SetCollisionProfileName(TEXT("Ragdoll")); // "Ragdoll" 프로파일이 Project Settings에 정의되어 있고, Physics Collision을 활성화해야 함

    // 물리 시뮬레이션 활성화 (프로파일 설정 후 또는 전, 순서는 크게 중요하지 않으나 후가 약간 더 안전)
    SkelComp->SetSimulatePhysics(true);

    // 절단된 본 주변의 피직스 컨스트레인트(Constraint)를 끊어서 분리 효과 강조 (선택 사항)
    // BreakConstraint는 지정된 본과 그 *부모 본* 사이의 컨스트레인트를 끊는 경향이 있습니다.
    // 정확한 동작은 Physics Asset 설정에 따라 달라질 수 있습니다.
    // 임펄스 방향 및 위치 계산 (예: 충돌 정보를 사용하거나 기본값 사용)
    FVector BreakImpulseDirection = FVector::ZeroVector; // 기본값
    FVector BreakLocation = BoneLocation; // 기본값 (절단 위치)

   // if (Hit.IsValidBlockingHit()) // 충돌 정보가 있다면 사용
    {
        BreakImpulseDirection = FVector::ForwardVector * -1.0f; // 충돌 법선 반대 방향
       //  BreakLocation = Hit.ImpactPoint;
        // 충돌 위치를 사용하는 것이 더 자연스러울 수 있음
    }
   // else // 충돌 정보 없으면 임의 방향 (예: 위쪽)
    {
        BreakImpulseDirection = FVector::UpVector;
    }

    FVector FinalBreakImpulse = BreakImpulseDirection * ImpulseMagnitude; // 최종 임펄스 계산

    UE_LOG(LogTemp, Log, TEXT("Breaking constraint at bone '%s' with Impulse: %s at Location: %s"), *TargetBoneName.ToString(), *FinalBreakImpulse.ToString(), *BreakLocation.ToString());
    SkelComp->BreakConstraint(FinalBreakImpulse, FVector::ZeroVector, TargetBoneName);

    UE_LOG(LogTemp, Log, TEXT("Parent SkelComp ragdoll initiated successfully."));

    // ------------------------------------

    return true; // 모든 과정 성공
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
        TargetBoneIndex = SkelComp->GetBoneIndex(TargetBoneName);
        if (TargetBoneIndex == INDEX_NONE)
        {
            UE_LOG(LogTemp, Warning, TEXT("GetFilteredSkeletalMeshData: TargetBone '%s' 을 찾을 수 없습니다. 필터링 실패."), *TargetBoneName.ToString());
            bShouldFilter = false;
        }
        else
        {
            if (LODRenderData.SkinWeightVertexBuffer.GetNumVertices() == 0)
            {
                UE_LOG(LogTemp, Error, TEXT("GetFilteredSkeletalMeshData: SkinWeightVertexBuffer가 LOD %d에 대해 유효하지 않습니다. 필터링 실패."), LODIndex);
                return false; // 스킨 웨이트 없으면 필터링 불가
            }
            SkinWeightBufferPtr = LODRenderData.GetSkinWeightVertexBuffer(); // 유효하면 포인터 할당
            UE_LOG(LogTemp, Log, TEXT("GetFilteredSkeletalMeshData: 필터링 by bone '%s' (Index: %d), MinWeight: %f"), *TargetBoneName.ToString(), TargetBoneIndex, MinWeight);
        }
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("GetFilteredSkeletalMeshData: TargetBone으로 필터링 하지 않음. LOD %d에서 모든 정점 사용."), LODIndex);
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
    //OutNormals.SetNumUninitialized(NumVertices);
    //OutTangents.SetNumUninitialized(NumVertices);
    //OutUV0.SetNumUninitialized(NumVertices);
    
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
    
    FTransform MeshTransform = SkelComp->GetComponentTransform();
    FVector TargetBoneLocation = SkelComp->GetBoneLocation(TargetBoneName); 
    for (const FSkelMeshRenderSection& Section : LODRenderData.RenderSections)
    {
        const uint32 SectionNumvertices = Section.NumVertices;
        const uint32 SectionBaseVertexIndex = Section.BaseVertexIndex;
        {
            for (uint32 i = 0; i < SectionNumvertices; i++)
            {
                const uint32 VertexIndex = i + SectionBaseVertexIndex;
                if (GetBoneWeightForVertex(VertexIndex, TargetBoneIndex, &Section, &LODRenderData, SkinWeightBufferPtr) > Threshold)
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
                    OutVertexColors.Add(FColor(0, 0, 0, 255));

                    /*
                    *                // 버텍스 컬러 (FColor로 저장됨)
                    if (bCopyVertexColors && StaticVertexBuffers.ColorVertexBuffer.IsInitialized() && StaticVertexBuffers.ColorVertexBuffer.GetNumVertices() > VertexIndex)
                    {
                    // 버텍스 컬러 복사 옵션이 켜져 있고, 버퍼가 초기화되었으며, 유효한 인덱스인 경우
                    OutVertexColors.Add(StaticVertexBuffers.ColorVertexBuffer.VertexColor(VertexIndex).ReinterpretAsLinear()); // FColor를 FLinearColor로 변환하여 저장
                     */
    
                    
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

bool USkelToProcMeshComponent::SliceMesh(UProceduralMeshComponent* InProcMesh, FVector PlanePosition, FVector PlaneNormal, bool bCreateOtherHalf, UProceduralMeshComponent*& OutOtherHalfProcMesh,
    EProcMeshSliceCapOption CapOption, UMaterialInterface* CapMaterial)
{
    UKismetProceduralMeshLibrary::SliceProceduralMesh(InProcMesh, PlanePosition, PlaneNormal, bCreateOtherHalf, OutOtherHalfProcMesh, CapOption, CapMaterial);
    if (OutOtherHalfProcMesh != nullptr)
    {
        return true;
    }
    return false;
}

float USkelToProcMeshComponent::GetBoneWeightForVertex(int32 VertexIndex, int32 TargetBoneIndex, const FSkelMeshRenderSection* SkelMeshRenderSection, const FSkeletalMeshLODRenderData* LODRenderData,
    const FSkinWeightVertexBuffer* SkinWeightBuffer)
{
    if (!SkinWeightBuffer || SkinWeightBuffer->GetNumVertices() == 0) return -1.f;
    const int32 MaxInfluences = SkinWeightBuffer->GetMaxBoneInfluences();
    const TArray<FBoneIndexType>& BoneMap = SkelMeshRenderSection->BoneMap;
    for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; InfluenceIdx++)
    {
        int32 LocalBoneIndex = SkinWeightBuffer->GetBoneIndex(VertexIndex, InfluenceIdx);
        if (LocalBoneIndex >= BoneMap.Num()) continue; // Ensure we are within bounds
        
        int32 GlobalBoneIndex = BoneMap[LocalBoneIndex];
        float BoneWeight = SkinWeightBuffer->GetBoneWeight(VertexIndex, InfluenceIdx) / 65535.0f;
        
        if (GlobalBoneIndex == TargetBoneIndex && BoneWeight > 0)
        {
            return BoneWeight;
        }
    }
    return -1.f;
}

USkeletalMeshComponent* USkelToProcMeshComponent::GetOwnerSkeletalMeshComponent() const
{
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: Owner를 찾을 수 없습니다."));
        return nullptr;
    }
    USkeletalMeshComponent* SkelComp = Owner->FindComponentByClass<USkeletalMeshComponent>();
    if (!SkelComp)
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: Owner '%s'에 SkeletalMeshComponent가 없습니다."), *Owner->GetName());
        return nullptr;
    }
    if (!SkelComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Warning, TEXT("SkelToProcMeshComponent: '%s'의 SkeletalMeshComponent에 Skeletal Mesh Asset이 할당되지 않았습니다."), *Owner->GetName());
        return nullptr;
    }
    return SkelComp;
}

bool USkelToProcMeshComponent::HideOriginalMeshVerticesByBone(USkeletalMeshComponent* SourceSkeletalMeshComp, int32 LODIndex, FName TargetBoneName, bool bClearOverride)
{
    if (!SourceSkeletalMeshComp || !SourceSkeletalMeshComp->GetSkeletalMeshAsset())
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Invalid Skeletal Mesh Component or Asset."));
        return false;
    }

    USkeletalMesh* SkelMesh = SourceSkeletalMeshComp->GetSkeletalMeshAsset();
    const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
    int32 TargetBoneIndex = RefSkeleton.FindBoneIndex(TargetBoneName);

    if (TargetBoneIndex == INDEX_NONE)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Bone '%s' not found."), *TargetBoneName.ToString());
        return false;
    }

    // FSkeletalMeshRenderData 접근은 게임 스레드에서 직접 하면 위험할 수 있으나, 읽기 목적 및 컴포넌트 상태 기반이므로
    // 일부 상황에서는 가능할 수 있습니다. 복잡한 경우 렌더 스레드 접근 고려.
    FSkeletalMeshRenderData* SkelRenderData = SourceSkeletalMeshComp->GetSkeletalMeshRenderData();
    if (!SkelRenderData || SkelRenderData->LODRenderData[LODIndex].GetNumVertices() == 0)
    {
         UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Invalid LOD Index %d."), LODIndex);
        return false;
    }

    FSkeletalMeshLODRenderData& LODRenderData = SkelRenderData->LODRenderData[LODIndex];
    FSkinWeightVertexBuffer* SkinWeightBuffer = SourceSkeletalMeshComp->GetSkinWeightBuffer(LODIndex); // 인스턴스별 버퍼 가져오기

    if (!SkinWeightBuffer)
    {
         UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: Could not get SkinWeightVertexBuffer for LOD %d."), LODIndex);
        return false;
    }

    uint32 NumVertices = LODRenderData.GetNumVertices();
    if (NumVertices == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("HideOriginalMeshVerticesByBone: LOD %d has no vertices."), LODIndex);
        return false;
    }

    TArray<int32> VerticesToHideIndices;
    VerticesToHideIndices.Reserve(NumVertices / 4); // 예상 크기 예약

    // 1. 숨길 버텍스 식별 (GetBoneWeightForVertex 구현 필요)
    for (uint32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
    {
        // GetBoneWeightForVertex 함수는 FSkinWeightVertexBuffer 구조에 맞춰 구현해야 합니다.
        // 아래는 가상의 함수 호출입니다. 실제 구현은 SkinWeightBuffer->GetWeightForVertex 등을 사용해야 합니다.
        float Weight = GetBoneWeightForVertex(VertexIndex, TargetBoneIndex, &LODRenderData.RenderSections[LODIndex], &LODRenderData, SkinWeightBuffer);

        if (Weight > Threshold)
        {
            VerticesToHideIndices.Add(VertexIndex);
        }
    }

    // 2. 숨길 버텍스가 있는 경우에만 오버라이드 적용
    if (VerticesToHideIndices.Num() > 0)
    {
        // 3. 버텍스 컬러 오버라이드 배열 준비
        TArray<FLinearColor> OverrideColors;
        OverrideColors.SetNum(NumVertices);

        // 기존 컬러 버퍼 확인
        const FColorVertexBuffer* ExistingColorBuffer = LODRenderData.StaticVertexBuffers.ColorVertexBuffer.IsInitialized() ?
                                                        &LODRenderData.StaticVertexBuffers.ColorVertexBuffer : nullptr;

        // 배열 초기화 (기존 색상 또는 흰색)
        for (uint32 i = 0; i < NumVertices; ++i)
        {
            if (ExistingColorBuffer && i < ExistingColorBuffer->GetNumVertices())
            {
                OverrideColors[i] = ExistingColorBuffer->VertexColor(i).ReinterpretAsLinear();
            }
            else
            {
                OverrideColors[i] = FLinearColor::White; // 기본값 (Alpha = 1)
            }
        }

        // 4. 숨길 버텍스의 알파 값을 0으로 설정
        for (int32 HideIndex : VerticesToHideIndices)
        {
            if (OverrideColors.IsValidIndex(HideIndex))
            {
                OverrideColors[HideIndex].A = 0.0f; // 알파를 0으로 만들어 숨김
            }
        }

        // 5. 컴포넌트에 오버라이드 적용
        SourceSkeletalMeshComp->SetVertexColorOverride_LinearColor(LODIndex, OverrideColors);
         UE_LOG(LogTemp, Log, TEXT("Applied vertex color override to hide %d vertices for bone '%s' on LOD %d."), VerticesToHideIndices.Num(), *TargetBoneName.ToString(), LODIndex);
        return true;
    }
    else if (bClearOverride)
    {
        // 숨길 버텍스가 없고, 이전 오버라이드를 지우도록 설정된 경우
        SourceSkeletalMeshComp->ClearVertexColorOverride(LODIndex);
        UE_LOG(LogTemp, Log, TEXT("No vertices to hide for bone '%s' on LOD %d. Cleared override (if any)."), *TargetBoneName.ToString(), LODIndex);
        return true; // 작업은 성공적으로 완료됨 (숨길 것이 없었음)
    }

    return true; // 숨길 것이 없었음
}
