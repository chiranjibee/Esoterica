#include "ClangVisitors_Structure.h"
#include "ClangVisitors_Enum.h"
#include "Applications/Reflector/TypeReflection/ReflectionDatabase.h"
#include "Base/TypeSystem/TypeRegistry.h"
#include "ClangVisitors_Macro.h"

//-------------------------------------------------------------------------

namespace EE::TypeSystem::Reflection
{
    struct FieldTypeInfo
    {
        inline void GetFlattenedTemplateArgs( String& flattenedArgs ) const
        {
            if ( !m_templateArgs.empty() )
            {
                for ( auto arg : m_templateArgs )
                {
                    flattenedArgs.append( arg.m_name );

                    if ( !arg.m_templateArgs.empty() )
                    {
                        flattenedArgs.append( "<" );
                        arg.GetFlattenedTemplateArgs( flattenedArgs );
                        flattenedArgs.append( ">" );
                    }

                    flattenedArgs.append( ", " );
                }

                flattenedArgs = flattenedArgs.substr( 0, flattenedArgs.length() - 2 );
            }
        }

        bool AllowsTemplateArguments() const
        {
            if ( m_name == "EE::String" )
            {
                return false;
            }

            return true;
        }

        String                          m_name;
        TVector<FieldTypeInfo>          m_templateArgs;
        bool                            m_isConstantArray = false;
    };

    static void GetFieldTypeInfo( ClangParserContext* pContext, ReflectedType* pType, CXType type, FieldTypeInfo& info )
    {
        clang::QualType const fieldQualType = ClangUtils::GetQualType( type );

        // Check if this is a valid qualified type
        if ( fieldQualType.isNull() )
        {
            String typeSpelling = ClangUtils::GetString( clang_getTypeSpelling( type ) );
            return pContext->LogError( "Failed to qualify typename for member: %s in class: %s and of type: %s", info.m_name.c_str(), pType->m_name.c_str(), typeSpelling.c_str() );
        }

        // Get typename
        if ( !ClangUtils::GetQualifiedNameForType( fieldQualType, info.m_name ) )
        {
            String typeSpelling = ClangUtils::GetString( clang_getTypeSpelling( type ) );
            return pContext->LogError( "Failed to qualify typename for member: %s in class: %s and of type: %s", info.m_name.c_str(), pType->m_name.c_str(), typeSpelling.c_str() );
        }

        // Is this a constant array
        info.m_isConstantArray = ( type.kind == CXType_ConstantArray );

        // Get info for template types
        if ( info.AllowsTemplateArguments() )
        {
            auto const numTemplateArguments = clang_Type_getNumTemplateArguments( type );
            if ( numTemplateArguments > 0 )
            {
                // We only support one template arg for now
                CXType const argType = clang_Type_getTemplateArgumentAsType( type, 0 );

                FieldTypeInfo templateFieldInfo;
                templateFieldInfo.m_isConstantArray = ( argType.kind == CXType_ConstantArray );
                GetFieldTypeInfo( pContext, pType, argType, templateFieldInfo );
                info.m_templateArgs.push_back( templateFieldInfo );
            }
        }
    }

    static void GetAllDerivedProperties( ReflectionDatabase const* pDatabase, TypeSystem::TypeID parentTypeID, TVector<ReflectedProperty>& results )
    {
        ReflectedType const* pParentDesc = pDatabase->GetType( parentTypeID );
        if ( pParentDesc != nullptr )
        {
            GetAllDerivedProperties( pDatabase, pParentDesc->m_parentID, results );
            for ( auto& parentProperty : pParentDesc->m_properties )
            {
                results.push_back( parentProperty );
            }
        }
    }

    //-------------------------------------------------------------------------

    CXChildVisitResult VisitStructureContents( CXCursor cr, CXCursor parent, CXClientData pClientData )
    {
        auto pContext = reinterpret_cast<ClangParserContext*>( pClientData );
        auto pClass = reinterpret_cast<ReflectedType*>( pContext->m_pParentReflectedType );

        uint32_t const lineNumber = ClangUtils::GetLineNumberForCursor( cr );
        CXCursorKind kind = clang_getCursorKind( cr );
        switch ( kind )
        {
            // Add base class
            case CXCursor_CXXBaseSpecifier:
            {
                if ( pClass->m_parentID.IsValid() )
                {
                    pContext->LogError( "Multiple inheritance detected for class: %s", pClass->m_name.c_str() );
                    return CXChildVisit_Break;
                }

                // Get qualified base type
                clang::CXXBaseSpecifier* pBaseSpecifier = (clang::CXXBaseSpecifier*) cr.data[0];
                String fullyQualifiedName;
                if ( !ClangUtils::GetQualifiedNameForType( pBaseSpecifier->getType(), fullyQualifiedName ) )
                {
                    pContext->LogError( "Failed to qualify typename for base class: %s, base class = %s", pClass->m_name.c_str(), ClangUtils::GetCursorDisplayName( cr ).c_str() );
                    return CXChildVisit_Break;
                }

                pClass->m_parentID = TypeSystem::TypeID( fullyQualifiedName );
                GetAllDerivedProperties( pContext->m_pDatabase, pClass->m_parentID, pClass->m_properties );

                // Remove duplicate properties added via the parent property traversal - do not change the order of the array
                for ( int32_t i = 0; i < (int32_t) pClass->m_properties.size(); i++ )
                {
                    for ( int32_t j = i + 1; j < (int32_t) pClass->m_properties.size(); j++ )
                    {
                        if ( pClass->m_properties[i].m_propertyID == pClass->m_properties[j].m_propertyID )
                        {
                            pClass->m_properties.erase( pClass->m_properties.begin() + j );
                            j--;
                        }
                    }
                }
            }
            break;

            case CXCursor_Constructor:
            {
                int32_t numArgs = clang_Cursor_getNumArguments( cr );
                if ( numArgs == 1 )
                {
                    CXCursor const argCursor = clang_Cursor_getArgument( cr, 0 );
                    CXType const ctorArgType = clang_getCursorType( argCursor );

                    String argumentTypeName;
                    if ( ClangUtils::GetQualifiedNameForType( ClangUtils::GetQualType( ctorArgType ), argumentTypeName ) )
                    {
                        if ( argumentTypeName == ReflectedType::s_defaultInstanceCustomCtorArgTypename )
                        {
                            pClass->m_hasCustomDefaultInstanceCtor = true;
                        }
                    }
                }
            }
            break;

            // Extract property info
            case CXCursor_FieldDecl:
            {
                ReflectionMacro propertyReflectionMacro;
                if ( pContext->FindReflectionMacroForProperty( pClass->m_headerID, lineNumber, propertyReflectionMacro ) )
                {
                    pClass->m_properties.push_back( ReflectedProperty( ClangUtils::GetCursorDisplayName( cr ), lineNumber ) );
                    ReflectedProperty& propertyDesc = pClass->m_properties.back();

                    // Try read any user comments for this field
                    CXString const commentString = clang_Cursor_getBriefCommentText( cr );
                    if ( commentString.data != nullptr )
                    {
                        propertyDesc.m_reflectedDescription = clang_getCString( commentString );
                        StringUtils::RemoveAllOccurrencesInPlace( propertyDesc.m_reflectedDescription, "\r" );
                        propertyDesc.m_reflectedDescription.ltrim();
                        propertyDesc.m_reflectedDescription.rtrim();
                    }
                    clang_disposeString( commentString );

                    // If we dont have an explicit comment for the property, try to get it from the macro declaration
                    if ( propertyDesc.m_reflectedDescription.empty() )
                    {
                        propertyDesc.m_reflectedDescription = propertyReflectionMacro.m_macroComment;
                    }

                    CXType type = clang_getCursorType( cr );
                    clang::QualType const fieldQualType = ClangUtils::GetQualType( type );
                    String typeSpelling = ClangUtils::GetString( clang_getTypeSpelling( type ) );

                    // Check if template parameter
                    if ( fieldQualType->isTemplateTypeParmType() )
                    {
                        pContext->LogError( "Cannot expose template argument member (%s) in class (%s)!", propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                        return CXChildVisit_Break;
                    }

                    // Check if this field is a c-style array
                    //-------------------------------------------------------------------------

                    if ( fieldQualType->isArrayType() )
                    {
                        if ( fieldQualType->isVariableArrayType() || fieldQualType->isIncompleteArrayType() )
                        {
                            pContext->LogError( "Variable size array properties are not supported! Please change to TVector or fixed size!" );
                            return CXChildVisit_Break;
                        }

                        auto const pArrayType = (clang::ConstantArrayType*) fieldQualType.getTypePtr();
                        propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsArray );
                        propertyDesc.m_arraySize = (int32_t) pArrayType->getSize().getSExtValue();

                        // Set property type to array type
                        type = clang_getElementType( type );
                    }

                    // Get field typename
                    //-------------------------------------------------------------------------

                    FieldTypeInfo fieldTypeInfo;
                    GetFieldTypeInfo( pContext, pClass, type, fieldTypeInfo );
                    EE_ASSERT( !fieldTypeInfo.m_name.empty() );
                    StringID fieldTypeID( fieldTypeInfo.m_name );

                    // Additional processing for special types
                    //-------------------------------------------------------------------------

                    if ( GetCoreTypeID( CoreTypeID::TVector ) == fieldTypeID || GetCoreTypeID( CoreTypeID::TInlineVector ) == fieldTypeID )
                    {
                        // We need to flag this in advance as we are about to change the field type ID
                        propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsDynamicArray );

                        // We need to remove the TVector type from the property info as we allow for templated types to be contained within arrays
                        FieldTypeInfo const& templateTypeInfo = fieldTypeInfo.m_templateArgs.front();
                        fieldTypeInfo = templateTypeInfo;
                        fieldTypeID = StringID( fieldTypeInfo.m_name );

                        if ( fieldTypeInfo.m_isConstantArray )
                        {
                            pContext->LogError( "We dont support arrays of arrays. Property: %s in class: %s", propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                            return CXChildVisit_Break;
                        }
                    }
                    else if ( GetCoreTypeID( CoreTypeID::String ) == fieldTypeID )
                    {
                        // We need to clear the template args since we have a type alias and clang is detected the template args for eastl::basic_string
                        fieldTypeInfo.m_templateArgs.clear();
                    }

                    //-------------------------------------------------------------------------

                    // Set property typename and validate
                    // If it is a templated type, we only support one level of specialization for exposed properties, so flatten the type
                    propertyDesc.m_typeName = fieldTypeInfo.m_name;
                    propertyDesc.m_typeID = fieldTypeID;
                    propertyDesc.m_rawMetaDataStr = propertyReflectionMacro.m_macroContents;

                    if ( !fieldTypeInfo.m_templateArgs.empty() )
                    {
                        String flattenedArgs;
                        fieldTypeInfo.GetFlattenedTemplateArgs( flattenedArgs );
                        propertyDesc.m_templateArgTypeName = flattenedArgs;
                    }

                    // Check for unsupported types
                    //-------------------------------------------------------------------------

                    // Core Types
                    if ( IsCoreType( propertyDesc.m_typeID ) )
                    {
                        // Check if this field is a generic resource ptr
                        if ( propertyDesc.m_typeID == CoreTypeID::ResourcePtr )
                        {
                            pContext->LogError( "Generic resource pointers are not allowed to be exposed, please use a TResourcePtr instead! ( property: %s in class: %s )", propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                            return CXChildVisit_Break;
                        }

                        // Specific resource ptr
                        if ( propertyDesc.m_typeID == CoreTypeID::TResourcePtr )
                        {
                            propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsResourcePtr );

                            if ( propertyDesc.m_templateArgTypeName == "EE::Resource::IResource" )
                            {
                                pContext->LogError( "Generic resource pointers ( TResourcePtr<IResource> ) are not allowed to be exposed, please use a specific resource type instead! ( property: %s in class: %s )", propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                                return CXChildVisit_Break;
                            }
                        }

                        // Type Instance
                        if ( propertyDesc.m_typeID == CoreTypeID::TypeInstance || propertyDesc.m_typeID == CoreTypeID::TTypeInstance )
                        {
                            propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsTypeInstance );

                            if ( propertyDesc.m_typeID == CoreTypeID::TTypeInstance )
                            {
                                // Perform validation on the type for the instance
                                ReflectedType const* pInstanceTypeDesc = pContext->m_pDatabase->GetType( propertyDesc.m_templateArgTypeName );
                                if ( pInstanceTypeDesc == nullptr )
                                {
                                    pContext->LogError( "Unsupported type encountered: %s for TTypeInstance property: %s in class: %s", propertyDesc.m_typeName.c_str(), propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                                    return CXChildVisit_Break;
                                }
                            }
                        }

                        // Bit flags
                        if ( propertyDesc.m_typeID == CoreTypeID::BitFlags )
                        {
                            propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsBitFlags );
                        }
                        else if ( propertyDesc.m_typeID == CoreTypeID::TBitFlags )
                        {
                            propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsBitFlags );

                            // Perform validation on the enum type for the bit-flags
                            ReflectedType const* pFlagTypeDesc = pContext->m_pDatabase->GetType( propertyDesc.m_templateArgTypeName );
                            if ( pFlagTypeDesc == nullptr || !pFlagTypeDesc->IsEnum() )
                            {
                                pContext->LogError( "Unsupported type encountered: %s for bit-flags property: %s in class: %s", propertyDesc.m_typeName.c_str(), propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                                return CXChildVisit_Break;
                            }
                        }

                        // Arrays
                        if ( propertyDesc.m_typeID == CoreTypeID::TVector || propertyDesc.m_typeID == CoreTypeID::TInlineVector )
                        {
                            pContext->LogError( "We dont support arrays of arrays. Property: %s in class: %s", propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                            return CXChildVisit_Break;
                        }
                    }
                    else // Non-Core Types
                    {
                        // Non-core types must have a valid type descriptor
                        ReflectedType const* pPropertyTypeDesc = pContext->m_pDatabase->GetType( propertyDesc.m_typeID );
                        if ( pPropertyTypeDesc == nullptr )
                        {
                            pContext->LogError( "Unsupported type encountered: %s for property: %s in class: %s", propertyDesc.m_typeName.c_str(), propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                            return CXChildVisit_Break;
                        }

                        // Check for enum types - bitflags are a special case and are not an enum
                        if ( pPropertyTypeDesc->IsEnum() )
                        {
                            propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsEnum );
                        }
                        else
                        {
                            propertyDesc.m_flags.SetFlag( PropertyInfo::Flags::IsStructure );
                        }

                        // Check if this field is a entity type
                        if ( pPropertyTypeDesc->IsEntity() || pPropertyTypeDesc->IsEntityComponent() || pPropertyTypeDesc->IsEntitySystem() || pPropertyTypeDesc->IsEntityWorldSystem() )
                        {
                            pContext->LogError( "You are not allowed to have ptrs to entity types ( property: %s in class: %s )", propertyDesc.m_name.c_str(), pClass->m_name.c_str() );
                            return CXChildVisit_Break;
                        }
                    }
                }
                break;
            }

            default: break;
        }

        return CXChildVisit_Continue;
    }

    CXChildVisitResult VisitResourceStructureContents( CXCursor cr, CXCursor parent, CXClientData pClientData )
    {
        auto pContext = reinterpret_cast<ClangParserContext*>( pClientData );
        auto pResource = reinterpret_cast<ReflectedResourceType*>( pContext->m_pParentReflectedType );

        CXCursorKind kind = clang_getCursorKind( cr );
        switch ( kind )
        {
            case CXCursor_CXXBaseSpecifier:
            {
                clang::CXXBaseSpecifier* pBaseSpecifier = (clang::CXXBaseSpecifier*) cr.data[0];
                if ( !ClangUtils::GetAllBaseClasses( pResource->m_parents, *pBaseSpecifier ) )
                {
                    pContext->LogError( "Failed to get all base classes type for resource type: %s", pResource->m_className.c_str() );
                    return CXChildVisit_Break;
                }
            }
            break;

            default: break;
        }

        return CXChildVisit_Continue;
    }

    CXChildVisitResult VisitStructure( ClangParserContext* pContext, CXCursor& cr, FileSystem::Path const& headerFilePath, StringID const headerID )
    {
        auto cursorName = ClangUtils::GetCursorDisplayName( cr );

        String fullyQualifiedCursorName;
        if ( !ClangUtils::GetQualifiedNameForType( clang_getCursorType( cr ), fullyQualifiedCursorName ) )
        {
            pContext->LogError( "Failed to get qualified type for cursor: %s", fullyQualifiedCursorName.c_str() );
            return CXChildVisit_Break;
        }

        //-------------------------------------------------------------------------

        ReflectionMacro macro;
        if ( pContext->GetReflectionMacroForType( headerID, cr, macro ) )
        {
            if ( !pContext->IsInEngineNamespace() )
            {
                pContext->LogError( "You cannot register types for reflection that are outside the engine namespace (%s). Type: %s, File: %s", Reflection::Settings::g_engineNamespace, fullyQualifiedCursorName.c_str(), headerFilePath.c_str() );
                return CXChildVisit_Break;
            }

            EE_ASSERT( macro.IsValid() );

            // Modules
            if ( macro.IsModuleMacro() && !pContext->m_detectDevOnlyTypesAndProperties )
            {
                String const moduleName = pContext->GetCurrentNamespace() + cursorName;

                if ( !pContext->SetModuleClassName( headerFilePath, moduleName ) )
                {
                    // Could not find originating project for detected registered module class
                    pContext->LogError( "Cant find the source project for this module class: %s", headerFilePath.c_str() );
                    return CXChildVisit_Break;
                }
            }

            //-------------------------------------------------------------------------

            if ( macro.IsRegisteredResourceMacro() )
            {
                // Fill out the resource type data
                ReflectedResourceType resource;
                resource.m_typeID = pContext->GenerateTypeID( fullyQualifiedCursorName );
                resource.m_headerID = headerID;
                resource.m_className = cursorName;
                resource.m_namespace = pContext->GetCurrentNamespace();

                if ( !resource.TryParseResourceRegistrationMacroString( macro.m_macroContents ) )
                {
                    pContext->LogError( "Invalid macro registration string for resource detected (only lower case letters allowed in resource type ID): %s", macro.m_macroContents.c_str() );
                    return CXChildVisit_Break;
                }

                pContext->m_pParentReflectedType = &resource;
                clang_visitChildren( cr, VisitResourceStructureContents, pContext );

                if ( pContext->HasErrorOccured() )
                {
                    return CXChildVisit_Break;
                }

                // Verify the resource hierarchy
                static TypeSystem::TypeID const resourceTypeID( ReflectedResourceType::s_baseResourceFullTypeName );
                if ( !VectorContains( resource.m_parents, resourceTypeID ) )
                {
                    pContext->LogError( "Resource %s doesnt derive from %s", resource.m_className.c_str(), ReflectedResourceType::s_baseResourceFullTypeName );
                    return CXChildVisit_Break;
                }

                // Register resource info
                //-------------------------------------------------------------------------

                if ( pContext->m_detectDevOnlyTypesAndProperties )
                {
                    if ( pContext->m_pDatabase->IsResourceRegistered( resource.m_resourceTypeID ) )
                    {
                        pContext->m_pDatabase->UpdateRegisteredResource( &resource );
                    }
                    else
                    {
                        pContext->LogError( "Trying to flag unknown type as a runtime type: %s in file: %s", resource.m_resourceTypeID.ToString().c_str(), headerFilePath.c_str() );
                        return CXChildVisit_Break;
                    }
                }
                else // Register type
                {
                    if ( !pContext->m_pDatabase->IsResourceRegistered( resource.m_resourceTypeID ) )
                    {
                        pContext->m_pDatabase->RegisterResource( &resource );
                    }
                    else // We do not allow multiple resources registered with the same ID
                    {
                        pContext->LogError( "Duplicate resource type ID encountered: %s in file: %s", resource.m_resourceTypeID.ToString().c_str(), headerFilePath.c_str() );
                        return CXChildVisit_Break;
                    }
                }
            }

            //-------------------------------------------------------------------------

            if ( macro.IsDataFileMacro() )
            {
                // Fill out the resource type data
                ReflectedDataFileType dataFile;
                dataFile.m_typeID = pContext->GenerateTypeID( fullyQualifiedCursorName );
                dataFile.m_headerID = headerID;
                dataFile.m_className = cursorName;
                dataFile.m_namespace = pContext->GetCurrentNamespace();

                if ( !dataFile.TryParseDataFileRegistrationMacroString( macro.m_macroContents ) )
                {
                    pContext->LogError( "Invalid macro registration string for data file detected (only lower case letters allowed in extension): %s", macro.m_macroContents.c_str() );
                    return CXChildVisit_Break;
                }

                // Register data file info
                //-------------------------------------------------------------------------

                if ( pContext->m_detectDevOnlyTypesAndProperties )
                {
                    if ( pContext->m_pDatabase->IsDataFileRegistered( dataFile.m_typeID ) )
                    {
                        pContext->m_pDatabase->UpdateRegisteredDataFile( &dataFile );
                    }
                    else
                    {
                        pContext->LogError( "Trying to flag unknown type as a runtime type: %s in file: %s", dataFile.m_typeID.c_str(), headerFilePath.c_str() );
                        return CXChildVisit_Break;
                    }
                }
                else // Register type
                {
                    if ( !pContext->m_pDatabase->IsDataFileRegistered( dataFile.m_typeID ) )
                    {
                        pContext->m_pDatabase->RegisterDataFile( &dataFile );
                    }
                    else // We do not allow multiple resources registered with the same ID
                    {
                        pContext->LogError( "Duplicate data file ID encountered: %s in file: %s", dataFile.m_typeID.c_str(), headerFilePath.c_str() );
                        return CXChildVisit_Break;
                    }
                }
            }

            //-------------------------------------------------------------------------

            if ( macro.IsReflectedTypeMacro() )
            {
                auto cursorType = clang_getCursorType( cr );
                auto* pRecordDecl = (clang::CXXRecordDecl*) cr.data[0];

                // Macro Validation
                String const userSuppliedTypeName = macro.GetReflectedTypeName();
                if ( fullyQualifiedCursorName.find( userSuppliedTypeName ) == String::npos )
                {
                    if ( macro.IsDataFileMacro() )
                    {
                        pContext->LogError( "EE_DATA_FILE macro for %s has the wrong type listed: %s", fullyQualifiedCursorName.c_str(), userSuppliedTypeName.c_str() );
                    }
                    else
                    {
                        pContext->LogError( "EE_REFLECT_TYPE macro for %s has the wrong type listed: %s", fullyQualifiedCursorName.c_str(), userSuppliedTypeName.c_str() );
                    }
                    return CXChildVisit_Break;
                }

                //-------------------------------------------------------------------------

                TypeID typeID = pContext->GenerateTypeID( fullyQualifiedCursorName );
                ReflectedType classDescriptor( typeID, cursorName );
                classDescriptor.m_headerID = headerID;
                classDescriptor.m_namespace = pContext->GetCurrentNamespace();
                classDescriptor.m_flags.SetFlag( ReflectedType::Flags::IsEntity, ( cursorName == ReflectedType::s_baseEntityClassName ) );
                classDescriptor.m_flags.SetFlag( ReflectedType::Flags::IsEntityComponent, ( macro.IsEntityComponentMacro() || cursorName == ReflectedType::s_baseEntityComponentClassName ) );
                classDescriptor.m_flags.SetFlag( ReflectedType::Flags::IsEntitySystem, ( macro.IsEntitySystemMacro() || cursorName == ReflectedType::s_baseEntitySystemClassName ) );
                classDescriptor.m_flags.SetFlag( ReflectedType::Flags::IsEntityWorldSystem, ( macro.IsEntityWorldSystemMacro() ) );
                classDescriptor.m_flags.SetFlag( ReflectedType::Flags::IsAbstract, pRecordDecl->isAbstract() );

                // Record current parent type, and update it to the new type
                void* pPreviousParentReflectedType = pContext->m_pParentReflectedType;
                pContext->m_pParentReflectedType = &classDescriptor;
                {
                    clang_visitChildren( cr, VisitStructureContents, pContext );
                }
                // Reset parent type back to original parent
                pContext->m_pParentReflectedType = pPreviousParentReflectedType;

                if ( pContext->HasErrorOccured() )
                {
                    return CXChildVisit_Break;
                }

                // Register type
                //-------------------------------------------------------------------------

                if ( pContext->m_detectDevOnlyTypesAndProperties )
                {
                    if ( pContext->m_pDatabase->IsTypeRegistered( classDescriptor.m_ID ) )
                    {
                        pContext->m_pDatabase->UpdateRegisteredType( &classDescriptor );
                    }
                    else
                    {
                        pContext->LogError( "Trying to flag unknown type as a runtime type: %s in file: %s", classDescriptor.m_ID.c_str(), headerID.c_str() );
                        return CXChildVisit_Break;
                    }
                }
                else // Register type
                {
                    if ( !pContext->m_pDatabase->IsTypeRegistered( classDescriptor.m_ID ) )
                    {
                        pContext->m_pDatabase->RegisterType( &classDescriptor );
                    }
                    else // We do not allow multiple resources registered with the same ID
                    {
                        pContext->LogError( "Duplicate type ID encountered: %s in file: %s", classDescriptor.m_ID.c_str(), headerID.c_str() );
                        return CXChildVisit_Break;
                    }
                }
            }
        }

        return CXChildVisit_Continue;
    }
}