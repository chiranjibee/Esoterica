#include "ImportedAnimation.h"
#include "Base/FileSystem/FileSystemPath.h"
#include "Formats/FBX.h"
#include "Formats/GLTF.h"

//-------------------------------------------------------------------------

namespace EE::Import
{
    void ImportedAnimation::Finalize()
    {
        EE_ASSERT( m_numFrames > 0 );

        // Extract Root Motion
        //-------------------------------------------------------------------------

        m_rootTransforms.resize( m_numFrames );

        TrackData& rootTrackData = m_tracks[0];
        Vector rootMotionOriginOffset = rootTrackData.m_localTransforms[0].GetTranslation(); // Ensure that the root motion always starts at the origin

        for ( int32_t i = 0; i < m_numFrames; i++ )
        {
            // If we detect scaling on the root, log an error and exit
            if ( rootTrackData.m_localTransforms[i].HasScale() )
            {
                LogError( "Root scaling detected! This is not allowed, please remove all scaling from the root bone!" );
                return;
            }

            // Extract root position and remove the origin offset from it
            m_rootTransforms[i] = rootTrackData.m_localTransforms[i];
            m_rootTransforms[i].SetTranslation( m_rootTransforms[i].GetTranslation() - rootMotionOriginOffset );

            // Set the root tracks transform to Identity
            rootTrackData.m_localTransforms[i] = Transform::Identity;
        }

        // Global Transforms
        //-------------------------------------------------------------------------

        int32_t const numBones = GetNumBones();
        for ( auto i = 0; i < numBones; i++ )
        {
            auto& trackData = m_tracks[i];

            int32_t const parentBoneIdx = m_skeleton.GetParentBoneIndex( i );
            if ( parentBoneIdx == InvalidIndex )
            {
                trackData.m_modelSpaceTransforms = trackData.m_localTransforms;
            }
            else // Calculate global transforms
            {
                auto const& parentTrackData = m_tracks[parentBoneIdx];
                trackData.m_modelSpaceTransforms.resize( m_numFrames );

                for ( auto f = 0; f < m_numFrames; f++ )
                {
                    trackData.m_modelSpaceTransforms[f] = trackData.m_localTransforms[f] * parentTrackData.m_modelSpaceTransforms[f];
                }
            }
        }
    }

    void ImportedAnimation::RegenerateLocalTransforms()
    {
        int32_t const numBones = GetNumBones();
        for ( auto i = 0; i < numBones; i++ )
        {
            auto& trackData = m_tracks[i];

            int32_t const parentBoneIdx = m_skeleton.GetParentBoneIndex( i );
            if ( parentBoneIdx == InvalidIndex )
            {
                trackData.m_localTransforms = trackData.m_modelSpaceTransforms;
            }
            else // Calculate local transforms
            {
                auto const& parentTrackData = m_tracks[parentBoneIdx];
                trackData.m_localTransforms.resize( m_numFrames );

                for ( auto f = 0; f < m_numFrames; f++ )
                {
                    trackData.m_localTransforms[f] = Transform::Delta( parentTrackData.m_modelSpaceTransforms[f], trackData.m_modelSpaceTransforms[f] );
                }
            }
        }
    }

    void ImportedAnimation::MakeAdditiveRelativeToSkeleton()
    {
        uint32_t const numBones = m_skeleton.GetNumBones();
        for ( uint32_t boneIdx = 0; boneIdx < numBones; boneIdx++ )
        {
            Transform baseTransform = m_skeleton.GetParentSpaceTransform( boneIdx );

            for ( int32_t frameIdx = 0; frameIdx < m_numFrames; frameIdx++ )
            {
                Transform const& poseTransform = m_tracks[boneIdx].m_localTransforms[frameIdx];

                Transform additiveTransform;
                additiveTransform.SetRotation( Quaternion::Delta( baseTransform.GetRotation(), poseTransform.GetRotation() ) );
                additiveTransform.SetTranslation( poseTransform.GetTranslation() - baseTransform.GetTranslation() );
                additiveTransform.SetScale( poseTransform.GetScale() - baseTransform.GetScale() );

                m_tracks[boneIdx].m_localTransforms[frameIdx] = additiveTransform;
            }
        }

        m_isAdditive = true;
    }

    void ImportedAnimation::MakeAdditiveRelativeToFrame( int32_t baseFrameIdx )
    {
        EE_ASSERT( baseFrameIdx >= 0 && baseFrameIdx < ( m_numFrames - 1 ) );

        uint32_t const numBones = m_skeleton.GetNumBones();

        // Copy reference pose
        TVector<Transform> basePose;
        basePose.reserve( numBones );

        for ( uint32_t boneIdx = 0; boneIdx < numBones; boneIdx++ )
        {
            basePose.emplace_back( m_tracks[boneIdx].m_localTransforms[baseFrameIdx] );
        }

        // Generate Additive Data
        for ( uint32_t boneIdx = 0; boneIdx < numBones; boneIdx++ )
        {
            Transform baseTransform = basePose[boneIdx];

            for ( int32_t frameIdx = 0; frameIdx < m_numFrames; frameIdx++ )
            {
                Transform const& poseTransform = m_tracks[boneIdx].m_localTransforms[frameIdx];

                Transform additiveTransform;
                additiveTransform.SetRotation( Quaternion::Delta( baseTransform.GetRotation(), poseTransform.GetRotation() ) );
                additiveTransform.SetTranslation( poseTransform.GetTranslation() - baseTransform.GetTranslation() );
                additiveTransform.SetScale( poseTransform.GetScale() - baseTransform.GetScale() );

                m_tracks[boneIdx].m_localTransforms[frameIdx] = additiveTransform;
            }
        }

        m_isAdditive = true;
    }

    void ImportedAnimation::MakeAdditiveRelativeToAnimation( ImportedAnimation const& baseAnimation )
    {
        EE_ASSERT( !IsAdditive() );
        EE_ASSERT( baseAnimation.IsValid() && !baseAnimation.IsAdditive() );
        EE_ASSERT( baseAnimation.m_skeleton.GetName() == m_skeleton.GetName() );

        if ( baseAnimation.GetNumFrames() < GetNumFrames() )
        {
            LogWarning( "Base Additive Animation has less frames than required so we are truncating this animation to the same length as the base animation" );
            m_numFrames = baseAnimation.GetNumFrames();
            m_duration = baseAnimation.GetDuration();
        }

        //-------------------------------------------------------------------------

        uint32_t const numBones = m_skeleton.GetNumBones();
        for ( uint32_t boneIdx = 0; boneIdx < numBones; boneIdx++ )
        {
            // Resize tracks to new animation length
            m_tracks[boneIdx].m_localTransforms.resize( m_numFrames );

            TVector<Transform> const& baseLocalTransforms = baseAnimation.GetTrackData()[boneIdx].m_localTransforms;

            for ( int32_t frameIdx = 0; frameIdx < m_numFrames; frameIdx++ )
            {
                Transform const& baseTransform = baseLocalTransforms[frameIdx];
                Transform const& poseTransform = m_tracks[boneIdx].m_localTransforms[frameIdx];

                Transform additiveTransform;
                additiveTransform.SetRotation( Quaternion::Delta( baseTransform.GetRotation(), poseTransform.GetRotation()));
                additiveTransform.SetTranslation( poseTransform.GetTranslation() - baseTransform.GetTranslation() );
                additiveTransform.SetScale( poseTransform.GetScale() - baseTransform.GetScale() );

                m_tracks[boneIdx].m_localTransforms[frameIdx] = additiveTransform;
            }
        }

        m_isAdditive = true;
    }
}