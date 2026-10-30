/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "project_tree_traverser.h"
#include "kicad_manager_frame.h"
#include <kiway.h>
#include <kemi_format_profile.h>
#include <kemi_project_save_as.h>
#include <kiway_mail.h>
#include <project/project_file.h>
#include <project/project_local_settings.h>
#include <settings/settings_manager.h>
#include <gestfich.h>
#include <confirm.h>
#include <string_utils.h>
#include <richio.h>
#include <memory>
#include <wildcards_and_files_ext.h>


PROJECT_TREE_TRAVERSER::PROJECT_TREE_TRAVERSER( KICAD_MANAGER_FRAME* aFrame,
                                                const wxString& aSrcProjectDirPath,
                                                const wxString& aSrcProjectName,
                                                const wxString& aNewProjectDirPath,
                                                const wxString& aNewProjectName,
                                                bool aKemiOnly ) :
        m_frame( aFrame ),
        m_projectDirPath( aSrcProjectDirPath ),
        m_projectName( aSrcProjectName ),
        m_newProjectDirPath( aNewProjectDirPath ),
        m_newProjectName( aNewProjectName )
{
    m_kemiOnly = aKemiOnly || wxFileName( aSrcProjectDirPath, aSrcProjectName,
            KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::KEMI ) ).FileExists();
    m_renameSymbolCache = KEMI_FORMAT::IsManagedProjectImportCache(
            m_projectDirPath, m_projectName, KEMI_FORMAT::DOC::SYMBOL_LIB );
    m_renameFootprintCache = KEMI_FORMAT::IsManagedProjectImportCache(
            m_projectDirPath, m_projectName, KEMI_FORMAT::DOC::FOOTPRINT );
    m_errors += KEMI_FORMAT::ProjectImportCacheRenameError(
            m_projectDirPath, m_projectName, m_newProjectName );
}


bool PROJECT_TREE_TRAVERSER::CopyFiles()
{
    if( !m_errors.empty() )
        return false;

    for( const auto& copy : m_copyOperations )
    {
        try
        {
            copy();
        }
        catch( const IO_ERROR& error )
        {
            m_errors += error.What() + wxS( "\n" );
        }

        if( !m_errors.empty() )
            return false;
    }

    m_copyOperations.clear();
    return true;
}


wxDirTraverseResult PROJECT_TREE_TRAVERSER::OnFile( const wxString& aSrcFilePath )
{
    // Recursion guard for a Save As to a location inside the source project.
    if( aSrcFilePath.StartsWith( m_newProjectDirPath ) )
        return wxDIR_CONTINUE;

    wxFileName destFile( aSrcFilePath );
    wxString   ext = destFile.GetExt().Lower();
    bool       atRoot = destFile.GetPath() == m_projectDirPath;
    bool       jsonProject = KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PROJECT, ext );
    bool       jsonLocal = KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PROJECT_LOCAL, ext );

    // Retained originals with native counterparts are not copied.  Do not silently
    // omit an official-only design from a restricted project's copy, either.
    if( m_kemiOnly && ( destFile.GetFullName().Lower().Contains( wxS( ".kicad_" ) )
                       || ext == FILEEXT::LegacyProjectFileExtension ) )
    {
        for( const auto& profile : KEMI_FORMAT::AllProfiles() )
        {
            if( KEMI_FORMAT::IsLegacyExt( profile.doc, ext )
                || ( profile.doc == KEMI_FORMAT::DOC::PROJECT
                     && ext == FILEEXT::LegacyProjectFileExtension ) )
            {
                wxFileName native( destFile );
                native.SetExt( wxString::FromUTF8( profile.kemiExt ) );

                if( native.FileExists() )
                    return wxDIR_CONTINUE;
            }
        }

        if( !jsonProject && !jsonLocal )
        {
            m_errors += wxString::Format(
                    _( "Cannot copy official KiCad file '%s' without a KemiCAD counterpart.\n" ),
                    aSrcFilePath );
            return wxDIR_CONTINUE;
        }
    }

    wxString destPath = destFile.GetPathWithSep();
    wxUniChar pathSep = wxFileName::GetPathSeparator();

    if( destPath.StartsWith( m_projectDirPath + pathSep ) )
        destPath.Replace( m_projectDirPath, m_newProjectDirPath, false );

    if( !m_kemiOnly )
    {
        destPath.Replace( pathSep + m_projectName + wxS( ".pretty" ) + pathSep,
                          pathSep + m_newProjectName + wxS( ".pretty" ) + pathSep );
    }
    else if( m_renameFootprintCache )
    {
        const wxString cachePath = wxFileName::DirName( m_newProjectDirPath ).GetPathWithSep()
                                   + m_projectName + wxS( ".pretty" ) + pathSep;

        if( destPath.StartsWith( cachePath ) )
        {
            destPath = wxFileName::DirName( m_newProjectDirPath ).GetPathWithSep()
                       + m_newProjectName + wxS( ".pretty" ) + pathSep
                       + destPath.Mid( cachePath.Length() );
        }
    }

    destFile.SetPath( destPath );

    if( destFile.GetName() == m_projectName && ext != wxS( "zip" )
        && !( m_frame && ( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::FOOTPRINT, ext )
                          || ( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::SYMBOL_LIB, ext )
                               && !( atRoot && m_renameSymbolCache
                                     && KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::SYMBOL_LIB, ext ) ) )
                          || ext == FILEEXT::SchematicSymbolFileExtension
                          || ext == FILEEXT::LegacySymbolLibFileExtension
                          || ext == FILEEXT::LegacySymbolDocumentFileExtension ) ) )
    {
        destFile.SetName( m_newProjectName );
    }

    if( m_frame && ( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::SYMBOL_LIB, ext )
                    || ext == FILEEXT::LegacySymbolLibFileExtension
                    || ext == FILEEXT::LegacySymbolDocumentFileExtension ) )
    {
        for( const wxString& suffix : { wxString( wxS( "-cache" ) ), wxString( wxS( "-rescue" ) ) } )
        {
            if( destFile.GetName() == m_projectName + suffix )
                destFile.SetName( m_newProjectName + suffix );
        }
    }

    // Load and validate settings now, before any directory or sibling is copied.
    std::shared_ptr<PROJECT_FILE> projectFile;
    std::shared_ptr<PROJECT_LOCAL_SETTINGS> localSettings;

    if( jsonProject || jsonLocal )
    {
        std::shared_ptr<JSON_SETTINGS> settings;

        if( jsonProject )
            settings = projectFile = std::make_shared<PROJECT_FILE>( aSrcFilePath );
        else
            settings = localSettings = std::make_shared<PROJECT_LOCAL_SETTINGS>( nullptr, aSrcFilePath );

        // LoadFromFile returns false for soft issues (e.g. missing meta.version) as well as
        // hard rejections.  Only GetLoadError() means the document must not be copied.
        settings->LoadFromFile();

        if( !settings->GetLoadError().IsEmpty() )
        {
            m_errors += wxString::Format( _( "Could not load settings '%s'.\n" ), aSrcFilePath );
            return wxDIR_CONTINUE;
        }

        settings->SetKemiOnly( m_kemiOnly );

        if( settings->IsKemiOnly() )
        {
            destFile.SetExt( KEMI_FORMAT::Ext( jsonProject ? KEMI_FORMAT::DOC::PROJECT
                                                         : KEMI_FORMAT::DOC::PROJECT_LOCAL,
                                             KEMI_FORMAT::FLAVOR::KEMI ) );
        }
    }

    const wxString target = destFile.GetFullPath();

    for( const wxString& existing : m_targets )
    {
        if( existing == target
#ifdef __WXMSW__
            || existing.CmpNoCase( target ) == 0
#endif
          )
        {
            m_errors += wxString::Format( _( "Multiple files would be copied to '%s'.\n" ), target );
            return wxDIR_CONTINUE;
        }
    }

    m_targets.push_back( target );

    m_copyOperations.push_back( [=, this]()
    {
        if( ext == FILEEXT::LegacyProjectFileExtension
          || KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PROJECT, ext )
          || KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PROJECT_LOCAL, ext ) )
        {
            if( ext == FILEEXT::LegacyProjectFileExtension )
            {
                // All paths in the settings file are relative so we can just do a straight copy
                KiCopyFile( aSrcFilePath, destFile.GetFullPath(), m_errors );
            }
            else if( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PROJECT, ext ) )
            {
                if( !projectFile->SaveAs( destFile.GetPath(), destFile.GetFullName() ) )
                    m_errors += wxString::Format( _( "Could not save project '%s'.\n" ), destFile.GetFullPath() );
            }
            else if( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PROJECT_LOCAL, ext ) )
            {
                if( !localSettings->SaveAs( destFile.GetPath(), destFile.GetFullName() ) )
                    m_errors += wxString::Format( _( "Could not save local settings '%s'.\n" ), destFile.GetFullPath() );
            }
        }
        else if( m_frame && ( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::SCHEMATIC, ext )
                 || ext == FILEEXT::KiCadSchematicFileExtension + FILEEXT::BackupFileSuffix
                 || ext == FILEEXT::LegacySchematicFileExtension
                 || ext == FILEEXT::LegacySchematicFileExtension + FILEEXT::BackupFileSuffix
                 || ext == FILEEXT::SchematicSymbolFileExtension
                 || ext == FILEEXT::LegacySymbolLibFileExtension
                 || ext == FILEEXT::LegacySymbolDocumentFileExtension
                 || KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::SYMBOL_LIB, ext )
                 || ext == FILEEXT::NetlistFileExtension
                 || destFile.GetName() == FILEEXT::SymbolLibraryTableFileName ) )
        {
            KIFACE* eeschema = m_frame->Kiway().KiFACE( KIWAY::FACE_SCH );
            eeschema->SaveFileAs( m_projectDirPath, m_projectName, m_newProjectDirPath,
                                  m_newProjectName, aSrcFilePath, m_errors );
        }
        else if( m_frame && ( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::PCB, ext )
                 || ext == FILEEXT::KiCadPcbFileExtension + FILEEXT::BackupFileSuffix
                 || ext == FILEEXT::LegacyPcbFileExtension
                 || KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::FOOTPRINT, ext )
                 || ext == FILEEXT::LegacyFootprintLibPathExtension
                 || ext == FILEEXT::FootprintAssignmentFileExtension
                 || destFile.GetName() == FILEEXT::FootprintLibraryTableFileName ) )
        {
            KIFACE* pcbnew = m_frame->Kiway().KiFACE( KIWAY::FACE_PCB );
            pcbnew->SaveFileAs( m_projectDirPath, m_projectName, m_newProjectDirPath,
                                m_newProjectName, aSrcFilePath, m_errors );
        }
        else if( m_frame && KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::DRAWING_SHEET, ext ) )
        {
            KIFACE* pleditor = m_frame->Kiway().KiFACE( KIWAY::FACE_PL_EDITOR );
            pleditor->SaveFileAs( m_projectDirPath, m_projectName, m_newProjectDirPath,
                                  m_newProjectName, aSrcFilePath, m_errors );
        }
        else if( m_frame && ( ext == FILEEXT::GerberJobFileExtension
               || ext == FILEEXT::DrillFileExtension
                 || FILEEXT::IsGerberFileExtension( ext ) ) )
        {
            KIFACE* gerbview = m_frame->Kiway().KiFACE( KIWAY::FACE_GERBVIEW );
            gerbview->SaveFileAs( m_projectDirPath, m_projectName, m_newProjectDirPath,
                                  m_newProjectName, aSrcFilePath, m_errors );
        }
        else if( destFile.GetName().StartsWith( FILEEXT::LockFilePrefix )
                 && ext == FILEEXT::LockFileExtension )
        {
            // Ignore lock files
        }
        else
        {
            // Everything we don't recognize just gets a straight copy.
            KiCopyFile( aSrcFilePath, destFile.GetFullPath(), m_errors );
        }
    } );

    if( atRoot && destFile.GetName() == m_newProjectName
        && ( jsonProject || ext == FILEEXT::LegacyProjectFileExtension ) )
    {
        m_newProjectFile = destFile;
    }

    return wxDIR_CONTINUE;
}


wxDirTraverseResult PROJECT_TREE_TRAVERSER::OnDir( const wxString& aSrcDirPath )
{
    wxUniChar pathSep = wxFileName::GetPathSeparator();

    // Skip the local-history git repository.
    wxFileName    srcDir = wxFileName::DirName( aSrcDirPath );
    wxArrayString dirs = srcDir.GetDirs();

    if( !dirs.IsEmpty() && dirs.Last() == wxS( ".history" ) )
        return wxDIR_IGNORE;

    // Recursion guard for a Save As to a location inside the source project.
    if( ( aSrcDirPath + pathSep ).StartsWith( m_newProjectDirPath ) )
        return wxDIR_IGNORE;

    wxFileName destDir( aSrcDirPath );
    wxString   destDirPath = destDir.GetPathWithSep();

    if( destDirPath.StartsWith( m_projectDirPath + pathSep )
      || destDirPath.StartsWith( m_projectDirPath + PROJECT_BACKUPS_DIR_SUFFIX ) )
    {
        destDirPath.Replace( m_projectDirPath, m_newProjectDirPath, false );
        destDir.SetPath( destDirPath );
    }

    if( destDir.GetName() == m_projectName )
    {
        if( destDir.GetExt() == "pretty"
            && ( !m_kemiOnly
                 || ( m_renameFootprintCache
                      && wxFileName( aSrcDirPath ).GetPath() == m_projectDirPath ) ) )
            destDir.SetName( m_newProjectName );
#if 0
        // WAYNE STAMBAUGH TODO:
        // If we end up with a symbol equivalent to ".pretty" we'll want to handle it here....
        else if( destDir.GetExt() == "sym_lib_dir_extension" )
            destDir.SetName( m_newProjectName );
#endif
    }

    m_copyOperations.push_back( [this, destDir]()
    {
        if( !wxMkdir( destDir.GetFullPath() ) )
        {
            wxString msg;

            if( !m_errors.empty() )
                m_errors += "\n";

            msg.Printf( _( "Cannot copy folder '%s'." ), destDir.GetFullPath() );
            m_errors += msg;
        }
    } );

    return wxDIR_CONTINUE;
}
