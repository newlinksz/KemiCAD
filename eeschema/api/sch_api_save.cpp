/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <api/sch_api_save.h>

#include <base_screen.h>
#include <common.h>
#include <kiplatform/io.h>
#include <pgm_base.h>
#include <project.h>
#include <kemi_format_profile.h>
#include <project/project_file.h>
#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <sch_screen.h>
#include <sch_save_state.h>
#include <set>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <schematic.h>
#include <settings/settings_manager.h>
#include <wildcards_and_files_ext.h>

#include <map>
#include <wx/filename.h>
#include <wx/log.h>


namespace SCH_API_SAVE
{

static bool persistentPath( SCHEMATIC& aSchematic, const wxString& aPath, wxFileName& aResult )
{
    try
    {
        aResult = KEMI_FORMAT::PersistentOutputPath( KEMI_FORMAT::DOC::SCHEMATIC,
                                                    aPath, aSchematic.IsKemiOnly() );
        return true;
    }
    catch( const IO_ERROR& error )
    {
        wxLogTrace( wxS( "KI_TRACE_API" ), wxS( "Save rejected: %s" ), error.What() );
        return false;
    }
}

bool SaveSheetToFile( SCH_SHEET* aSheet, SCHEMATIC& aSchematic, const wxString& aPath )
{
    wxCHECK( aSheet, false );

    if( aPath.IsEmpty() )
        return false;

    wxFileName schematicFileName;
    if( !persistentPath( aSchematic, aPath, schematicFileName ) )
        return false;
    schematicFileName.MakeAbsolute();

    if( !schematicFileName.DirExists() && !wxMkdir( schematicFileName.GetPath() ) )
        return false;

    if( schematicFileName.FileExists() && !schematicFileName.IsFileWritable() )
        return false;

    if( schematicFileName.FileExists() )
        KIPLATFORM::IO::MakeWriteable( schematicFileName.GetFullPath() );

    SCH_IO_MGR::SCH_FILE_T pluginType = SCH_IO_MGR::GuessPluginTypeFromSchPath( schematicFileName.GetFullPath() );

    if( pluginType == SCH_IO_MGR::SCH_FILE_UNKNOWN )
        pluginType = SCH_IO_MGR::SCH_KICAD;

    IO_RELEASER<SCH_IO> pi( SCH_IO_MGR::FindPlugin( pluginType ) );

    try
    {
        pi->SaveSchematicFile( schematicFileName.GetFullPath(), aSheet, &aSchematic );
        return true;
    }
    catch( const IO_ERROR& ioe )
    {
        wxLogTrace( wxS( "KI_TRACE_API" ), wxS( "SaveSheetToFile failed: %s" ), ioe.What() );
        return false;
    }
}


void UpdateProjectFile( SCHEMATIC& aSchematic, PROJECT& aProject )
{
    if( aSchematic.IsKemiOnly() && !KEMI_FORMAT::HasKemiExt( aProject.GetProjectFullName() ) )
        return;
    SCH_SCREEN* rootScreen = aSchematic.RootScreen();

    if( !rootScreen )
        return;

    wxFileName projectFile( rootScreen->GetFileName() );
    projectFile.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT,
            aSchematic.IsKemiOnly() ? KEMI_FORMAT::FLAVOR::KEMI : KEMI_FORMAT::FLAVOR::LEGACY ) );

    if( !projectFile.HasName() || !projectFile.IsOk() )
        return;

    aSchematic.RecordERCExclusions();

    aProject.GetProjectFile().m_SchematicSettings->m_SchDrawingSheetFileName = BASE_SCREEN::m_DrawingSheetFileName;

    if( rootScreen )
    {
        aProject.GetProjectFile().m_IP2581Bom.schRevision = rootScreen->GetTitleBlock().GetRevision();
    }

    const std::vector<SCH_SHEET*>& topLevelSheets = aSchematic.GetTopLevelSheets();

    if( !topLevelSheets.empty() )
    {
        std::vector<TOP_LEVEL_SHEET_INFO>& projectSheets = aProject.GetProjectFile().GetTopLevelSheets();
        projectSheets.clear();

        wxString projectPath = aProject.GetProjectPath();

        for( SCH_SHEET* sheet : topLevelSheets )
        {
            TOP_LEVEL_SHEET_INFO info;
            info.uuid = sheet->m_Uuid;
            info.name = sheet->GetName();

            wxString filename;

            if( sheet->GetScreen() )
                filename = sheet->GetScreen()->GetFileName();

            wxFileName sheetFn( filename );

            if( sheetFn.IsAbsolute() )
                sheetFn.MakeRelativeTo( projectPath );

            info.filename = sheetFn.GetFullPath();
            projectSheets.push_back( std::move( info ) );
        }
    }

    std::vector<FILE_INFO_PAIR>& sheets = aProject.GetProjectFile().GetSheets();
    sheets.clear();

    if( !aSchematic.HasHierarchy() )
        aSchematic.RefreshHierarchy();

    for( SCH_SHEET_PATH& sheetPath : aSchematic.Hierarchy() )
    {
        SCH_SHEET* sheet = sheetPath.Last();

        wxCHECK2( sheet, continue );

        if( !sheet->IsVirtualRootSheet() )
            sheets.emplace_back( std::make_pair( sheet->m_Uuid, sheet->GetName() ) );
    }

    Pgm().GetSettingsManager().SaveProject( projectFile.GetFullPath() );
}


bool SaveSchematic( SCHEMATIC& aSchematic, PROJECT& aProject )
{
    SCH_SCREEN* rootScreen = aSchematic.RootScreen();

    if( !rootScreen || rootScreen->GetFileName().IsEmpty() )
        return false;

    SCH_SCREENS screens( aSchematic.Root() );
    SCH_SAVE_STATE saveState( aSchematic );
    std::map<SCH_SCREEN*, wxString> targets;
    std::set<SCH_SHEET*> rewrittenSheets;

    try
    {
        for( size_t i = 0; i < screens.GetCount(); ++i )
        {
            SCH_SCREEN* screen = screens.GetScreen( i );

            if( !screen || screen->GetFileName().IsEmpty() )
                continue;

            if( screen->HasLoadError() )
                THROW_IO_ERROR( _( "Cannot save an incompletely loaded schematic." ) );

            targets[screen] = KEMI_FORMAT::ResolveNativeSaveTarget(
                    KEMI_FORMAT::DOC::SCHEMATIC, screen->GetFileName() );
        }
    }
    catch( const IO_ERROR& error )
    {
        wxLogTrace( wxS( "KI_TRACE_API" ), wxS( "Save rejected: %s" ), error.What() );
        return false;
    }

    for( const auto& [screen, target] : targets )
        screen->SetFileName( target );

    for( SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
    {
        SCH_SHEET* sheet = path.Last();

        if( !sheet || sheet->IsVirtualRootSheet() || !sheet->GetScreen() )
            continue;

        if( !rewrittenSheets.insert( sheet ).second )
            continue;

        UpdateSheetFileReference( aSchematic, path );
    }

    screens.BuildClientSheetPathList();

    bool success = true;

    for( size_t i = 0; i < screens.GetCount(); i++ )
    {
        SCH_SCREEN* screen = screens.GetScreen( i );

        wxCHECK2( screen, continue );

        wxFileName fileName = screen->GetFileName();

        if( !fileName.IsOk() )
            continue;

        std::vector<SCH_SHEET_PATH>& sheets = screen->GetClientSheetPaths();

        if( sheets.size() == 1 )
            screen->SetVirtualPageNumber( 1 );
        else
            screen->SetVirtualPageNumber( 0 );

        success &= SaveSheetToFile( screens.GetSheet( i ), aSchematic, fileName.GetFullPath() );

    }

    if( success )
    {
        try
        {
            UpdateProjectFile( aSchematic, aProject );
        }
        catch( const IO_ERROR& error )
        {
            wxLogTrace( wxS( "KI_TRACE_API" ), wxS( "Save project failed: %s" ), error.What() );
            return false;
        }

        for( const auto& [screen, target] : targets )
            screen->SetContentModified( false );

        saveState.Commit();
    }

    return success;
}


bool SaveSchematicCopy( SCHEMATIC& aSchematic, PROJECT& aProject, const wxString& aFileName, bool aCreateProject )
{
    wxFileName schematicFileName;
    if( !persistentPath( aSchematic, aFileName, schematicFileName ) )
        return false;
    schematicFileName.MakeAbsolute();

    if( !schematicFileName.IsOk() || !schematicFileName.IsDirWritable() )
        return false;

    // Root() is the virtual root, whose screen only holds the top-level sheets
    SCH_SHEET*  rootSheet = aSchematic.GetTopLevelSheet();
    SCH_SCREEN* rootScreen = aSchematic.RootScreen();

    if( !rootSheet || !rootScreen )
        return false;

    if( schematicFileName.GetFullPath() == rootScreen->GetFileName() )
        return SaveSchematic( aSchematic, aProject );

    if( !SaveSheetToFile( rootSheet, aSchematic, schematicFileName.GetFullPath() ) )
        return false;

    if( aCreateProject )
    {
        wxFileName projectFile( schematicFileName );
        projectFile.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT,
                aSchematic.IsKemiOnly() ? KEMI_FORMAT::FLAVOR::KEMI : KEMI_FORMAT::FLAVOR::LEGACY ) );

        if( !projectFile.FileExists() )
        {
            if( !Pgm().GetSettingsManager().SaveProjectCopy( projectFile.GetFullPath() ) )
                return false;
        }
    }

    return true;
}


bool SaveSchematicAs( SCHEMATIC& aSchematic, PROJECT& aProject, const wxString& aFileName )
{
    wxFileName schematicFileName;
    if( !persistentPath( aSchematic, aFileName, schematicFileName ) )
        return false;
    schematicFileName.MakeAbsolute();

    if( !schematicFileName.IsOk() || schematicFileName.GetName().IsEmpty() )
        return false;

    if( aSchematic.RootScreen()
            && KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::SCHEMATIC,
                    wxFileName( aSchematic.RootScreen()->GetFileName() ).GetExt() )
            && schematicFileName.Exists() )
    {
        return false;
    }

    if( ( !schematicFileName.DirExists() && !schematicFileName.Mkdir() ) || !schematicFileName.IsDirWritable() )
        return false;

    SCH_SAVE_STATE saveState( aSchematic );
    aSchematic.Root().SetFileName( schematicFileName.GetFullName() );
    aSchematic.RootScreen()->SetFileName( schematicFileName.GetFullPath() );

    if( !SaveSchematic( aSchematic, aProject ) )
        return false;

    if( KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::PROJECT,
                                  wxFileName( aProject.GetProjectFullName() ).GetExt() )
            && wxFileExists( aProject.GetProjectFullName() ) )
    {
        saveState.Commit();
        return true;
    }

    aProject.SetReadOnly( false );

    wxFileName projectFile( schematicFileName );
    projectFile.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT,
            aSchematic.IsKemiOnly() ? KEMI_FORMAT::FLAVOR::KEMI : KEMI_FORMAT::FLAVOR::LEGACY ) );

    try
    {
        Pgm().GetSettingsManager().SaveProjectAs( projectFile.GetFullPath(), &aProject );
    }
    catch( const IO_ERROR& error )
    {
        wxLogTrace( wxS( "KI_TRACE_API" ), wxS( "SaveProjectAs failed: %s" ), error.What() );
        return false;
    }

    saveState.Commit();
    return true;
}

} // namespace SCH_API_SAVE
