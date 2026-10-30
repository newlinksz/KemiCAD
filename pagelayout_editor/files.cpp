/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2013 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * @author Jean-Pierre Charras, jp.charras at wanadoo.fr
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

#include <kemi_format_profile.h>
#include <confirm.h>
#include <gestfich.h>
#include <drawing_sheet/ds_data_model.h>
#include <drawing_sheet/ds_file_versions.h>
#include <paths.h>
#include <widgets/wx_infobar.h>
#include <wildcards_and_files_ext.h>

#include <kiplatform/io.h>

#include "pl_editor_frame.h"
#include "pl_editor_id.h"

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <kiplatform/ui.h>

bool PL_EDITOR_FRAME::saveCurrentPageLayout()
{
    wxCommandEvent saveEvent;
    saveEvent.SetId( wxID_SAVE );
    Files_io( saveEvent );

    return !IsContentModified();
}


void PL_EDITOR_FRAME::OnFileHistory( wxCommandEvent& event )
{
    wxString filename;

    filename = GetFileFromHistory( event.GetId(), _( "Drawing Sheet File" ) );

    if( !filename.IsEmpty() )
    {
        if( IsContentModified() )
        {
            if( !HandleUnsavedChanges( this, _( "The current drawing sheet has been modified. "
                                                "Save changes?" ),
                                       [&]() -> bool
                                       {
                                           return saveCurrentPageLayout();
                                       } ) )
            {
                return;
            }
        }

        ::wxSetWorkingDirectory( ::wxPathOnly( filename ) );

        if( LoadDrawingSheetFile( filename ) )
        {
            wxString msg;
            msg.Printf( _( "File '%s' loaded"), filename );
            SetStatusText( msg );
        }

        OnNewDrawingSheet();
    }
}


void PL_EDITOR_FRAME::OnClearFileHistory( wxCommandEvent& aEvent )
{
    ClearFileHistory();
}


/* File commands. */
void PL_EDITOR_FRAME::Files_io( wxCommandEvent& event )
{
    wxString       msg;
    int            id = event.GetId();
    wxString       filename = GetCurrentFileName();
    DS_DATA_MODEL& pglayout = DS_DATA_MODEL::GetTheInstance();

    if( filename.IsEmpty() && id == wxID_SAVE )
        id = wxID_SAVEAS;

    if( ( id == wxID_NEW || id == wxID_OPEN ) && IsContentModified() )
    {
        if( !HandleUnsavedChanges( this, _( "The current drawing sheet has been modified. "
                                            "Save changes?" ),
                                   [&]() -> bool
                                   {
                                       return saveCurrentPageLayout();
                                   } ) )
        {
            return;
        }
    }

    switch( id )
    {
    case wxID_NEW:
        pglayout.AllowVoidList( true );
        SetCurrentFileName( wxEmptyString );
        pglayout.ClearList();
        OnNewDrawingSheet();
        break;

    case ID_APPEND_DESCR_FILE:
    {
         wxFileDialog openFileDialog( this, _( "Append Existing Drawing Sheet" ),
                                      wxEmptyString, wxEmptyString,
                                      FILEEXT::DrawingSheetFileWildcard(), wxFD_OPEN );

        KIPLATFORM::UI::AllowNetworkFileSystems( &openFileDialog );

        if( openFileDialog.ShowModal() == wxID_CANCEL )
            return;

        filename = openFileDialog.GetPath();

        if( !InsertDrawingSheetFile( filename ) )
        {
            msg.Printf( _( "Unable to load %s file" ), filename );
            DisplayErrorMessage( this, msg );
        }
        else
        {
            GetScreen()->SetContentModified();
            HardRedraw();
            msg.Printf( _( "File '%s' inserted" ), filename );
            SetStatusText( msg );
        }

        break;
    }

    case wxID_OPEN:
    {
         wxFileDialog openFileDialog( this, _( "Open Drawing Sheet" ), wxEmptyString, wxEmptyString,
                                     FILEEXT::DrawingSheetFileWildcard(), wxFD_OPEN );

        KIPLATFORM::UI::AllowNetworkFileSystems( &openFileDialog );

        if( openFileDialog.ShowModal() == wxID_CANCEL )
            return;

        filename = openFileDialog.GetPath();

        if( !LoadDrawingSheetFile( filename ) )
        {
            msg.Printf( _( "Unable to load %s file" ), filename );
            DisplayErrorMessage( this, msg );
        }
        else
        {
            OnNewDrawingSheet();
            msg.Printf( _( "File '%s' saved." ), filename );
            SetStatusText( msg );
        }

        break;
    }

    case wxID_SAVE:
        try
        {
            filename = KEMI_FORMAT::ResolveNativeSaveTarget( KEMI_FORMAT::DOC::DRAWING_SHEET, filename );
        }
        catch( const IO_ERROR& error )
        {
            DisplayErrorMessage( this, error.What() );
            wxCommandEvent saveAsEvent;
            saveAsEvent.SetId( wxID_SAVEAS );
            Files_io( saveAsEvent );
            return;
        }

        if( !SaveDrawingSheetFile( filename ) )
        {
            msg.Printf( _( "Unable to write '%s'." ), filename );
            DisplayErrorMessage( this, msg );
        }
        else
        {
            SetCurrentFileName( filename );
            msg.Printf( _("File '%s' saved."), filename );
            SetStatusText( msg );
        }

        break;

    case wxID_SAVEAS:
    {
        wxString dir = PATHS::GetUserTemplatesPath();
        const bool restricted = true;
        const wxString wildcard = restricted
                ? _( "KemiCAD Drawing Sheet" ) + AddFileExtListToFilter(
                        { KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::DRAWING_SHEET, KEMI_FORMAT::FLAVOR::KEMI ) } )
                : FILEEXT::DrawingSheetFileWildcard();
        wxFileDialog openFileDialog( this, _( "Save Drawing Sheet As" ), dir, wxEmptyString,
                                     wildcard,
                                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT );

        KIPLATFORM::UI::AllowNetworkFileSystems( &openFileDialog );

        if( openFileDialog.ShowModal() == wxID_CANCEL )
            return;

        filename = openFileDialog.GetPath();
        try
        {
            filename = KEMI_FORMAT::PersistentOutputPath( KEMI_FORMAT::DOC::DRAWING_SHEET,
                                                         filename, restricted );

            if( KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::DRAWING_SHEET,
                    wxFileName( GetCurrentFileName() ).GetExt() ) && wxFileExists( filename ) )
            {
                THROW_IO_ERROR( _( "Choose a new native file name; the save target already exists." ) );
            }
        }
        catch( const IO_ERROR& error )
        {
            DisplayErrorMessage( this, error.What() );
            return;
        }

        // Ensure the file has the right extension:
        // because a name like name.subname.subsubname is legal,
        // add the right extension without replacing the wxFileName
        // extension
        wxFileName fn(filename);

        if( !KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::DRAWING_SHEET, fn.GetExt() ) )
            filename << wxT( "." ) << FILEEXT::DrawingSheetFileExtension;

        if( !SaveDrawingSheetFile( filename ) )
        {
            msg.Printf( _( "Failed to create file '%s'." ), filename );
            DisplayErrorMessage( this, msg );
        }
        else
        {
            msg.Printf( _( "File '%s' saved." ), filename );
            SetStatusText( msg );

            SetCurrentFileName( filename );
            UpdateTitleAndInfo();
        }

        break;
    }

    default:
        break;
    }
}


bool PL_EDITOR_FRAME::LoadDrawingSheetFile( const wxString& aFullFileName )
{
    if( wxFileExists( aFullFileName ) )
    {
        wxString msg;

        if( !DS_DATA_MODEL::GetTheInstance().LoadDrawingSheet( aFullFileName, &msg ) )
        {
            DisplayErrorMessage( this,
                                 wxString::Format( _( "Error loading drawing sheet '%s'." ),
                                                   aFullFileName ),
                                 msg );
            return false;
        }

        SetCurrentFileName( aFullFileName );
        UpdateFileHistory( aFullFileName );
        GetScreen()->SetContentModified( false );

        wxFileName fn = aFullFileName;
        m_infoBar->Dismiss();

        if( DS_DATA_MODEL::GetTheInstance().GetFileFormatVersionAtLoad() < SEXPR_WORKSHEET_FILE_VERSION )
        {
            m_infoBar->RemoveAllButtons();
            m_infoBar->AddCloseButton();
            m_infoBar->ShowMessage( _( "This file was created by an older version of KiCad. "
                                       "It will be converted to the new format when saved." ),
                                    wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );
        }

        if( fn.FileExists() && !fn.IsFileWritable() )
        {
            m_infoBar->RemoveAllButtons();
            m_infoBar->AddCloseButton();
            m_infoBar->ShowMessage( _( "Layout file is read only." ),
                                    wxICON_WARNING, WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE );
        }

        return true;
    }

    return false;
}


bool PL_EDITOR_FRAME::InsertDrawingSheetFile( const wxString& aFullFileName )
{
    if( wxFileExists( aFullFileName ) )
    {
        const bool append = true;
        SaveCopyInUndoList();
        return DS_DATA_MODEL::GetTheInstance().LoadDrawingSheet( aFullFileName, nullptr, append );
    }

    return false;
}


bool PL_EDITOR_FRAME::SaveDrawingSheetFile( const wxString& aFullFileName )
{
    if( !aFullFileName.IsEmpty() )
    {
        wxString tempFile;

        try
        {
            KEMI_FORMAT::CheckPersistentOutput( KEMI_FORMAT::DOC::DRAWING_SHEET, aFullFileName,
                    KEMI_FORMAT::FlavorForPath( KEMI_FORMAT::DOC::DRAWING_SHEET, aFullFileName ),
                    DS_DATA_MODEL::GetTheInstance().IsKemiOnlyOutput() );
            tempFile = wxFileName::CreateTempFileName(
                    wxFileName( aFullFileName ).GetPathWithSep() + wxS( "pledit" ) );

            DS_DATA_MODEL::GetTheInstance().Save(
                    tempFile, KEMI_FORMAT::FlavorForPath( KEMI_FORMAT::DOC::DRAWING_SHEET,
                                                        aFullFileName ) );
        }
        catch( const IO_ERROR& )
        {
            // In case we started a file but didn't fully write it, clean up
            if( !tempFile.IsEmpty() )
                wxRemoveFile( tempFile );

            return false;
        }

        // Preserve the permissions of the current file
        KIPLATFORM::IO::DuplicatePermissions( aFullFileName, tempFile );

        if( !wxRenameFile( tempFile, aFullFileName ) )
            return false;

        if( m_infoBar->GetMessageType() == WX_INFOBAR::MESSAGE_TYPE::OUTDATED_SAVE )
            m_infoBar->Dismiss();

        GetScreen()->SetContentModified( false );
        UpdateTitleAndInfo();
        return true;
    }

    return false;
}

void PL_EDITOR_FRAME::DoWithAcceptedFiles()
{
    for( const wxFileName& file : m_AcceptedFiles )
        OpenProjectFiles( { file.GetFullPath() }, KICTL_CREATE );
}
