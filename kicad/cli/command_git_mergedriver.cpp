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
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/gpl-3.0.html
 * or you may search the http://www.gnu.org website for the version 3 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "command_git_mergedriver.h"

#include <cli/exit_codes.h>
#include <diff_merge/merge_dispatch.h>
#include <string_utils.h>
#include <kemi_format_profile.h>
#include <kiface_base.h>
#include <kiway.h>
#include <kiid.h>
#include <kiplatform/io.h>
#include <wx/ffile.h>
#include <wildcards_and_files_ext.h>

#include <wx/crt.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>


#define ARG_KIND     "--kind"
#define ARG_ANCESTOR "ancestor"
#define ARG_OURS     "ours"
#define ARG_THEIRS   "theirs"
#define ARG_OUTPUT_F "--output"
#define ARG_PATH     "--path"


CLI::GIT_MERGEDRIVER_COMMAND::GIT_MERGEDRIVER_COMMAND() :
        COMMAND( "git-mergedriver" )
{
    // Invoked by git, not by users: keep it out of `kicad-cli --help`. The
    // argparse stream operator skips suppressed subparsers while still routing
    // and parsing them, so no help-rendering code is needed.
    m_argParser.set_suppress( true );

    m_argParser.add_description(
            UTF8STDSTR( _( "Internal git merge-driver hook (configured automatically by KiCad's "
                           "git integration; not intended for direct use)." ) ) );

    m_argParser.add_argument( ARG_KIND )
            .required()
            .help( UTF8STDSTR( _( "Document kind: pcb, sch, sym, or fp" ) ) )
            .metavar( "KIND" );

    m_argParser.add_argument( ARG_ANCESTOR ).metavar( "ANCESTOR" );
    m_argParser.add_argument( ARG_OURS ).metavar( "OURS" );
    m_argParser.add_argument( ARG_THEIRS ).metavar( "THEIRS" );

    m_argParser.add_argument( ARG_PATH ).default_value( std::string() )
            .help( UTF8STDSTR( _( "Native document path supplied by Git as %P" ) ) );

    m_argParser.add_argument( "-o", ARG_OUTPUT_F ).required().metavar( "PATH" );
}


int CLI::GIT_MERGEDRIVER_COMMAND::doPerform( KIWAY& aKiway )
{
    const std::string kindStr  = m_argParser.get<std::string>( ARG_KIND );
    const wxString    ancestor = From_UTF8( m_argParser.get<std::string>( ARG_ANCESTOR ).c_str() );
    const wxString    ours     = From_UTF8( m_argParser.get<std::string>( ARG_OURS ).c_str() );
    const wxString    theirs   = From_UTF8( m_argParser.get<std::string>( ARG_THEIRS ).c_str() );
    const wxString    output   = From_UTF8( m_argParser.get<std::string>( ARG_OUTPUT_F ).c_str() );

    const wxString pathArg = From_UTF8( m_argParser.get<std::string>( ARG_PATH ).c_str() );
    wxFileName logicalPath( pathArg.IsEmpty() ? output : pathArg );
    logicalPath.MakeAbsolute();
    KICAD_DIFF::DOC_KIND kind = KICAD_DIFF::DOC_KIND::UNKNOWN;
    KEMI_FORMAT::DOC document = KEMI_FORMAT::DOC::PCB;
    bool singleFile = false;

    if( kindStr == "pcb" )
        kind = KICAD_DIFF::DOC_KIND::PCB;
    else if( kindStr == "sch" )
    {
        kind = KICAD_DIFF::DOC_KIND::SCH;
        document = KEMI_FORMAT::DOC::SCHEMATIC;
    }
    else if( kindStr == "sym" )
    {
        kind = KICAD_DIFF::DOC_KIND::SYM_LIB;
        document = KEMI_FORMAT::DOC::SYMBOL_LIB;
    }
    else if( kindStr == "fp" )
    {
        kind = KICAD_DIFF::DOC_KIND::FP_LIB;
        document = KEMI_FORMAT::DOC::FOOTPRINT;
        singleFile = true;
    }
    else
    {
        wxFprintf( stderr, _( "Unknown --kind '%s' (expected pcb, sch, sym, or fp)\n" ),
                   wxString::FromUTF8( kindStr ) );
        return EXIT_CODES::ERR_ARGS;
    }

    if( !KEMI_FORMAT::IsKemiExt( document, logicalPath.GetExt() )
            || logicalPath.GetFullName().Lower().Contains( wxS( ".kicad_" ) )
            || wxFileName( output ).GetFullName().Lower().Contains( wxS( ".kicad_" ) )
            || ( KEMI_FORMAT::HasKemiExt( output )
                 && !KEMI_FORMAT::IsKemiExt( document, wxFileName( output ).GetExt() ) ) )
    {
        wxFprintf( stderr, _( "Git merge requires a native document path (--path %%P).\n" ) );
        return EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
    }

    const wxString tmpDir = wxStandardPaths::Get().GetTempDir() + wxFILE_SEP_PATH
                           + wxS( "kemicad_mergedriver_" ) + KIID().AsString();

    if( !wxFileName::Mkdir( tmpDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;

    auto staged = [&]( const wxString& aName )
    {
        return wxFileName( tmpDir, aName, KEMI_FORMAT::Ext( document, KEMI_FORMAT::FLAVOR::KEMI ) )
                .GetFullPath();
    };
    auto readBytes = []( const wxString& aPath )
    {
        wxFFile file( aPath, wxS( "rb" ) );

        if( !file.IsOpened() || file.Length() < 0 )
            THROW_IO_ERROR( _( "Could not read a merge input." ) );

        std::string bytes( static_cast<size_t>( file.Length() ), '\0' );

        if( file.Read( bytes.data(), bytes.size() ) != bytes.size() )
            THROW_IO_ERROR( _( "Could not read a merge input." ) );

        return bytes;
    };

    const wxString stagedAncestor = staged( wxS( "ancestor" ) );
    const wxString stagedOurs = staged( wxS( "ours" ) );
    const wxString stagedTheirs = staged( wxS( "theirs" ) );
    const wxString stagedOut = staged( wxS( "merged" ) );
    std::vector<wxString> referencedPaths;
    int rc = EXIT_CODES::ERR_INVALID_INPUT_FILE;

    try
    {
        KIFACE* iface = aKiway.KiFACE( kindStr == "sch" || kindStr == "sym"
                                              ? KIWAY::FACE_SCH : KIWAY::FACE_PCB );
        auto stageInput = [&]( const wxString& aSource, const wxString& aName )
        {
            // Raw blobs are import inputs with no document suffix. Only the real native
            // writer produces a document, so legacy bytes never masquerade as .kemi_*.
            std::string bytes = readBytes( aSource );
            const wxString raw = wxFileName( tmpDir, aName + wxS( ".input" ) ).GetFullPath();

            if( kind == KICAD_DIFF::DOC_KIND::SCH )
                KEMI_FORMAT::RemapSchematicSheetPaths( bytes, logicalPath.GetFullPath(), raw, {},
                                                       &referencedPaths );

            return iface && KIPLATFORM::IO::AtomicWriteFile( raw, bytes.data(), bytes.size() )
                    && iface->SaveNativeDocument( raw, staged( aName ) );
        };

        if( stageInput( ancestor, wxS( "ancestor" ) ) && stageInput( ours, wxS( "ours" ) )
                && stageInput( theirs, wxS( "theirs" ) ) )
        {
            rc = KICAD_DIFF::DispatchMerge( aKiway, kind, stagedAncestor, stagedOurs, stagedTheirs,
                                            stagedOut, false, singleFile, nullptr );
        }

        if( ( rc == EXIT_CODES::SUCCESS || rc == EXIT_CODES::ERR_RC_VIOLATIONS )
                && wxFileExists( stagedOut ) )
        {
            std::string bytes = readBytes( stagedOut );

            if( kind == KICAD_DIFF::DOC_KIND::SCH )
            {
                // Git merges each versioned sheet separately. The root blob must retain
                // worktree references rather than the handler's temporary flattened layout.
                std::map<wxString, wxString> worktreeTargets;

                for( const wxString& reference : referencedPaths )
                {
                    wxFileName flattened( tmpDir, wxFileName( reference ).GetName(),
                            KEMI_FORMAT::Ext( document, KEMI_FORMAT::FLAVOR::KEMI ) );
                    worktreeTargets.emplace( flattened.GetFullPath(), reference );
                }

                KEMI_FORMAT::RemapSchematicSheetPaths( bytes, stagedOut, logicalPath.GetFullPath(),
                                                       worktreeTargets );
            }

            // %A may have no suffix, but its resolved write target must still protect import sources.
            KEMI_FORMAT::KEMI_ONLY_OUTPUT_POLICY( logicalPath.GetPath() )
                    .CheckWritePath( document, output );

            if( !KIPLATFORM::IO::AtomicWriteFile( output, bytes.data(), bytes.size() ) )
                rc = EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
        }
    }
    catch( const IO_ERROR& error )
    {
        wxFprintf( stderr, wxS( "%s\n" ), error.What() );
        rc = EXIT_CODES::ERR_INVALID_OUTPUT_CONFLICT;
    }

    wxFileName::Rmdir( tmpDir, wxPATH_RMDIR_RECURSIVE );
    return rc;
}
