/*
 * This program source code file is part of KemiCAD.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 */

#include <kemi_project_import.h>

#include <import_proj_properties.h>

#include <kemi_format_profile.h>
#include <ki_exception.h>
#include <project.h>
#include <project/project_file.h>
#include <project/project_local_settings.h>
#include <project/project_archiver.h>
#include <reporter.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <utility>
#include <wx/filename.h>
#include <wx/intl.h>

namespace KEMI_FORMAT
{

PROJECT_IMPORT_TARGETS ResolveProjectImportTargets( const wxString& aOutput,
                                                    const wxString& aInput,
                                                    const wxString& aDefaultDirectory )
{
    wxFileName project;

    if( aOutput.IsEmpty() )
    {
        project.AssignDir( aDefaultDirectory );
        project.SetName( wxFileName( aInput ).GetName() );
    }
    else
    {
        project.Assign( aOutput );
    }

    if( !project.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE, aDefaultDirectory ) )
        THROW_IO_ERROR( _( "Could not resolve the imported project's destination." ) );

    KEMI_ONLY_OUTPUT_POLICY policy( project.GetPath() );
    policy.CheckWritePath( DOC::PROJECT, project.GetFullPath() );

    // Unlike a file name, an output stem may contain dots that belong to its name.
    if( project.HasExt() && !IsKemiExt( DOC::PROJECT, project.GetExt() ) )
        project.SetName( project.GetFullName() );

    // Drop OrCAD/Allegro -YYYYMMDD-HHMM export stamps from the engineering name.
    project.SetName( IMPORT_PROJ_PROPS::StripImportDateSuffix( project.GetName() ) );

    project.SetExt( Ext( DOC::PROJECT, FLAVOR::KEMI ) );
    PROJECT_IMPORT_TARGETS targets;
    targets.project = policy.Target( DOC::PROJECT, project.GetFullPath() ).FinalPath();

    auto sibling = [&]( DOC aDoc )
    {
        wxFileName target( project );
        target.SetExt( Ext( aDoc, FLAVOR::KEMI ) );
        return policy.Target( aDoc, target.GetFullPath() ).FinalPath();
    };

    targets.local = sibling( DOC::PROJECT_LOCAL );
    targets.schematic = sibling( DOC::SCHEMATIC );
    targets.board = sibling( DOC::PCB );
    return targets;
}


bool IsProjectTopLevelSchematic( const PROJECT& aProject, const wxString& aFilePath )
{
    if( aFilePath.IsEmpty() || aProject.IsNullProject() )
        return false;

    wxFileName candidate( aFilePath );

    if( !candidate.MakeAbsolute( aProject.GetProjectPath() ) )
        return false;

    for( const TOP_LEVEL_SHEET_INFO& sheet : aProject.GetProjectFile().GetTopLevelSheets() )
    {
        if( sheet.filename.IsEmpty() )
            continue;

        wxFileName declared( sheet.filename );

        if( declared.MakeAbsolute( aProject.GetProjectPath() ) && declared.SameAs( candidate ) )
            return true;
    }

    return false;
}


void RegisterImportedBoards( PROJECT& aProject, const std::vector<wxString>& aBoardPaths )
{
    KEMI_ONLY_OUTPUT_POLICY policy( aProject.GetProjectPath() );
    std::vector<FILE_INFO_PAIR> boards = aProject.GetProjectFile().GetBoards();

    for( const wxString& path : aBoardPaths )
    {
        wxFileName board( policy.Target( DOC::PCB, path ).FinalPath() );

        if( !IsKemiExt( DOC::PCB, wxFileName( path ).GetExt() ) || !board.FileExists() )
            THROW_IO_ERROR( _( "An imported board must be saved before registering it in the project." ) );

        const auto existing = std::find_if( boards.begin(), boards.end(),
                [&]( const FILE_INFO_PAIR& aEntry )
                {
                    wxFileName registered( aEntry.second );
                    return registered.MakeAbsolute( aProject.GetProjectPath() )
                           && registered.SameAs( board );
                } );

        if( existing == boards.end() )
        {
            if( !board.MakeRelativeTo( aProject.GetProjectPath() ) )
                THROW_IO_ERROR( _( "Could not make the imported board path relative to the project." ) );

            boards.emplace_back( KIID(), board.GetFullPath() );
        }
    }

    aProject.GetProjectFile().GetBoards() = std::move( boards );
}


void ValidateProjectImportTargets( const PROJECT_IMPORT_TARGETS& aTargets )
{
    const wxFileName project( aTargets.project );
    KEMI_ONLY_OUTPUT_POLICY policy( project.GetPath() );
    const std::array<std::pair<DOC, wxString>, 4> targets = {
        std::make_pair( DOC::PROJECT, aTargets.project ),
        std::make_pair( DOC::PROJECT_LOCAL, aTargets.local ),
        std::make_pair( DOC::SCHEMATIC, aTargets.schematic ),
        std::make_pair( DOC::PCB, aTargets.board )
    };

    for( const auto& [doc, path] : targets )
    {
        const OUTPUT_TARGET target = policy.Target( doc, path );

        if( !IsKemiExt( doc, wxFileName( path ).GetExt() )
            || !wxFileName( path ).SameAs( wxFileName( target.FinalPath() ) ) )
        {
            THROW_IO_ERROR( _( "Project import requires native KemiCAD destinations." ) );
        }

        wxFileName existing( target.FinalPath() );

        for( FLAVOR flavor : { FLAVOR::KEMI, FLAVOR::LEGACY } )
        {
            existing.SetExt( Ext( doc, flavor ) );

            if( existing.Exists() )
            {
                THROW_IO_ERROR( wxString::Format(
                        _( "Import destination '%s' already exists. Choose a different name or directory." ),
                        existing.GetFullPath() ) );
            }
        }
    }
}


void CreateProjectImportFiles( const PROJECT_IMPORT_TARGETS& aTargets )
{
    ValidateProjectImportTargets( aTargets );
    const wxFileName projectPath( aTargets.project );

    if( !projectPath.DirExists()
        && !wxFileName::Mkdir( projectPath.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        THROW_IO_ERROR( wxString::Format( _( "Could not create output directory: %s" ),
                                          projectPath.GetPath() ) );
    }

    PROJECT project;
    PROJECT_FILE projectFile( aTargets.project );
    PROJECT_LOCAL_SETTINGS localSettings( &project, aTargets.local );
    projectFile.SetProject( &project );
    projectFile.ResetToDefaults();
    localSettings.ResetToDefaults();

    KEMI_ONLY_OUTPUT_POLICY policy( projectPath.GetPath() );
    policy.Write( DOC::PROJECT, aTargets.project,
            [&]( const wxString&, const OUTPUT_TARGET& )
            {
                return projectFile.SaveToFile( wxEmptyString, true );
            } );
    policy.Write( DOC::PROJECT_LOCAL, aTargets.local,
            [&]( const wxString&, const OUTPUT_TARGET& )
            {
                return localSettings.SaveToFile( wxEmptyString, true );
            } );
}




namespace
{

bool sameFilesystemEntity( const wxString& aLeft, const wxString& aRight )
{
    if( aLeft.IsEmpty() || aRight.IsEmpty() )
        return false;

    if( aLeft.IsSameAs( aRight, wxFileName::IsCaseSensitive() ) )
        return true;

    // Resolve identity without lexical ".." folding so symlink+parent paths stay correct.
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    const fs::path left( aLeft.ToStdWstring() );
    const fs::path right( aRight.ToStdWstring() );
#else
    const fs::path left( static_cast<const char*>( aLeft.utf8_str() ) );
    const fs::path right( static_cast<const char*>( aRight.utf8_str() ) );
#endif

    if( fs::exists( left, ec ) && fs::exists( right, ec ) && fs::equivalent( left, right, ec ) )
        return true;

    ec.clear();
    const fs::path leftCanon = fs::weakly_canonical( left, ec );

    if( ec )
        return false;

    ec.clear();
    const fs::path rightCanon = fs::weakly_canonical( right, ec );

    if( ec )
        return false;

    return leftCanon == rightCanon;
}

} // namespace

bool WriteImportZip( const wxString& aProjectFilePath, REPORTER* aReporter,
                     const std::vector<wxString>& aProtectedSourcePaths )
{
    wxFileName projectFn( aProjectFilePath );

    if( !projectFn.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE ) )
        return false;

    const wxString projectDir = projectFn.GetPath();
    const wxString engName = projectFn.GetName();

    if( engName.IsEmpty() || projectDir.IsEmpty() )
        return false;

    // GUI imports may use an engineering-name directory instead of a "kemi" leaf.
    // Always keep the archive beside that directory, independent of its name.
    wxFileName parent;
    parent.AssignDir( projectDir );

    if( parent.GetDirCount() == 0 )
        return false;

    parent.RemoveLastDir();
    wxFileName zipFn( parent.GetPath(), engName, wxS( "zip" ) );

    if( !zipFn.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE ) )
        return false;

    NULL_REPORTER nullReporter;
    REPORTER& reporter = aReporter ? *aReporter : nullReporter;
    const wxString zipPath = zipFn.GetFullPath();

    for( const wxString& protectedPath : aProtectedSourcePaths )
    {
        if( protectedPath.IsEmpty() )
            continue;

        // Absolute only: do NOT fold ".." lexically before identity checks.
        // Lexical DOTS would turn "symlink/../file.zip" into the wrong parent on POSIX.
        wxFileName protectedFn( protectedPath );

        if( !protectedFn.Normalize( wxPATH_NORM_ABSOLUTE ) )
            continue;

        if( sameFilesystemEntity( protectedFn.GetFullPath(), zipPath )
            || sameFilesystemEntity( protectedPath, zipPath ) )
        {
            reporter.Report( wxString::Format(
                                     _( "Refusing to overwrite import source archive '%s' "
                                        "with the converted project zip." ),
                                     zipPath ),
                             RPT_SEVERITY_ERROR );
            return false;
        }
    }

    // aIncludeExtraFiles=false: native archive set; PROJECT_ARCHIVER skips .kicad_* in native scopes.
    return PROJECT_ARCHIVER::Archive( projectDir, zipPath, reporter, true, false );
}

} // namespace KEMI_FORMAT
