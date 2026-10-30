/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004-2015 Jean-Pierre Charras
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

#include <wx/dir.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <memory>

#include <confirm.h>
#include <kiplatform/ui.h>
#include <kiway.h>
#include <project/project_archiver.h>
#include <reporter.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>
#include <kemi_format_profile.h>
#include <local_history.h>

#include "kicad_manager_frame.h"


void KICAD_MANAGER_FRAME::OnFileHistory( wxCommandEvent& event )
{
    wxString filename = GetFileFromHistory( event.GetId(), _( "KemiCAD project file" ) );

    if( !filename.IsEmpty() )
        LoadProject( wxFileName( filename ) );
}


void KICAD_MANAGER_FRAME::OnClearFileHistory( wxCommandEvent& aEvent )
{
    ClearFileHistory();
}


static bool kemiDirIsEmpty( const wxString& aPath )
{
    wxDir dir( aPath );
    return dir.IsOpened() && !dir.HasFiles() && !dir.HasSubDirs();
}


void KICAD_MANAGER_FRAME::UnarchiveFiles()
{
    wxFileName fn = Prj().GetProjectFullName();

    fn.SetExt( FILEEXT::ArchiveFileExtension );

    wxFileDialog zipfiledlg( this, _( "Unzip Project" ), fn.GetPath(), fn.GetFullName(),
                             FILEEXT::ZipFileWildcard(), wxFD_OPEN | wxFD_FILE_MUST_EXIST );

    KIPLATFORM::UI::AllowNetworkFileSystems( &zipfiledlg );

    if( zipfiledlg.ShowModal() == wxID_CANCEL )
        return;

    const wxString zipPath = zipfiledlg.GetPath();
    wxString       msg = wxString::Format( _( "\nOpen '%s'\n" ), zipPath );
    PrintMsg( msg );

    // KEMI: choose the parent folder (defaults to the archive's folder); the project is
    // extracted into a new sub-folder named after the archive (or its single top folder).
    wxFileName zipFn( zipPath );

    wxDirDialog parentDlg( this, _( "Select the folder in which to create the project folder" ),
                           zipFn.GetPath(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST );

    KIPLATFORM::UI::AllowNetworkFileSystems( &parentDlg );

    if( parentDlg.ShowModal() == wxID_CANCEL )
        return;

    wxFileName parentDir = wxFileName::DirName( parentDlg.GetPath() );
    parentDir.MakeAbsolute();

    const wxString topFolder = PROJECT_ARCHIVER::FindSingleTopFolder( zipPath );
    const wxString baseName = topFolder.IsEmpty() ? zipFn.GetName() : topFolder;

    auto childPath =
            [&]( const wxString& aName ) -> wxString
            {
                wxFileName child( parentDir );
                child.AppendDir( aName );
                return child.GetPath();
            };

    wxString targetPath = childPath( baseName );

    if( wxFileExists( targetPath ) || ( wxDirExists( targetPath ) && !kemiDirIsEmpty( targetPath ) ) )
    {
        wxMessageDialog dlg( this,
                             wxString::Format( _( "The folder '%s' already exists and is not "
                                                  "empty.\n\nOverwrite files in it, or "
                                                  "extract to a new folder?" ),
                                               targetPath ),
                             _( "Unzip Project" ),
                             wxYES_NO | wxCANCEL | wxICON_WARNING );

        dlg.SetYesNoCancelLabels( _( "Overwrite" ), _( "New Folder" ), _( "Cancel" ) );

        int answer = dlg.ShowModal();

        if( answer == wxID_CANCEL )
            return;

        if( answer == wxID_NO || wxFileExists( targetPath ) )
        {
            wxString candidate;

            for( int i = 1; ; ++i )
            {
                candidate = childPath( wxString::Format( wxS( "%s_%d" ), baseName, i ) );

                if( !wxFileName::Exists( candidate ) )
                    break;

                if( i >= 100000 )
                {
                    DisplayErrorMessage( this, _( "Could not find a free folder name." ) );
                    return;
                }
            }

            targetPath = candidate;
        }
    }

    if( !wxDirExists( targetPath )
            && !wxFileName::Mkdir( targetPath, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        DisplayErrorMessage( this, wxString::Format( _( "Could not create folder '%s'." ),
                                                     targetPath ) );
        return;
    }

    wxString unzipDir = wxFileName::DirName( targetPath ).GetPathWithSep();

    msg.Printf( _( "Unzipping project in '%s'.\n" ), unzipDir );
    PrintMsg( msg );

    if( wxFileName::DirName( unzipDir ).SameAs( wxFileName::DirName( Prj().GetProjectPath() ) ) )
    {
        if( !Kiway().PlayersClose( false ) )
            return;
    }

    STATUSBAR_REPORTER reporter( GetStatusBar(), 1 );

    // Strip the single top folder so content lands directly in the target folder,
    // whatever its final name (e.g. demo_1).
    if( PROJECT_ARCHIVER::Unarchive( zipPath, unzipDir, reporter, topFolder ) )
    {
        // KEMI: prefer .kemi_pro, fall back to .kicad_pro; open only a unique project.
        const wxString projectFile = PROJECT_ARCHIVER::FindExtractedProjectFile( unzipDir );

        if( !projectFile.IsEmpty() )
            LoadProject( wxFileName( projectFile ) );
    }
    else
    {
        DisplayErrorMessage( this, wxString::Format( _( "Failed to extract '%s'. See the "
                                                        "messages for details." ),
                                                     zipPath ) );
    }
}

void KICAD_MANAGER_FRAME::RestoreLocalHistory()
{
    Kiway().LocalHistory().ShowRestoreDialog( Prj().GetProjectPath(), this );
}
