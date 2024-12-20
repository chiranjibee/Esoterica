#pragma once

#include "ResourceID.h"
#include "Base/Serialization/BinarySerialization.h"

//-------------------------------------------------------------------------

namespace EE::Resource
{
    // Describes the contents of a resource, every resource has a header
    struct ResourceHeader
    {
        EE_SERIALIZE( m_version, m_resourceType, m_installDependencies, m_sourceResourceHash );

    public:

        ResourceHeader() = default;

        ResourceHeader( int32_t version, ResourceTypeID type, uint64_t sourceResourceHash, uint64_t advancedUpToDateHash )
            : m_version( version )
            , m_resourceType( type )
            , m_sourceResourceHash( sourceResourceHash )
            , m_advancedUpToDateHash( advancedUpToDateHash )
        {}

        void Clear() { *this = ResourceHeader(); }

        ResourceTypeID GetResourceTypeID() const { return m_resourceType; }

        void AddInstallDependency( ResourceID resourceID ) { m_installDependencies.push_back( resourceID ); }

    public:

        int32_t                 m_version = -1;
        ResourceTypeID          m_resourceType;
        TVector<ResourceID>     m_installDependencies;
        uint64_t                m_sourceResourceHash = 0;
        uint64_t                m_advancedUpToDateHash = 0;
    };
}