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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <memory>
#include <wx/dir.h>
#include <wx/filedlg.h>
#include <wx/fs_zip.h>
#include <wx/regex.h>
#include <wx/uri.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <core/arraydim.h>
#include <kemi_format_profile.h>
#include <macros.h>
#include <project/project_archiver.h>
#include <reporter.h>
#include <wildcards_and_files_ext.h>
#include <wxstream_helper.h>
#include <wx_filename.h>
#include <wx/log.h>
#include <kiplatform/io.h>

#include <map>
#include <regex>
#include <set>


#define ZipFileExtension wxT( "zip" )

struct PROJECT_FORMAT_STATE
{
    bool nativeProject = false;
    bool officialProject = false;
    bool nativeDocument = false;

    void Add( const wxFileName& aFile )
    {
        nativeProject |= KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::PROJECT, aFile.GetExt() );
        officialProject |= KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::PROJECT, aFile.GetExt() );
        nativeDocument |= KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::SCHEMATIC, aFile.GetExt() )
                          || KEMI_FORMAT::IsKemiExt( KEMI_FORMAT::DOC::PCB, aFile.GetExt() );
    }
};


// Project files establish ownership before standalone documents. In particular, an
// unrelated native library beside an official project must not hide its source files.
class PROJECT_FORMAT_SCOPES
{
public:
    explicit PROJECT_FORMAT_SCOPES( const wxString& aRoot, bool aReadDisk = false ) :
            m_root( directoryKey( aRoot ) ), m_readDisk( aReadDisk )
    {}

    void Add( const wxFileName& aFile )
    {
        m_states[directoryKey( aFile.GetPath() )].Add( aFile );
    }

    bool IsNative( const wxFileName& aFile )
    {
        wxFileName directory = wxFileName::DirName( aFile.GetPath() );
        bool standaloneNative = false;

        for( ;; )
        {
            const wxString key = directoryKey( directory.GetPath() );
            PROJECT_FORMAT_STATE& state = m_states[key];

            if( m_readDisk && m_scanned.insert( key ).second )
            {
                wxDir dir( directory.GetPath() );
                wxString name;

                if( dir.IsOpened() )
                {
                    for( bool more = dir.GetFirst( &name, wxEmptyString, wxDIR_FILES ); more;
                         more = dir.GetNext( &name ) )
                        state.Add( wxFileName( directory.GetPath(), name ) );
                }
            }

            if( state.nativeProject || state.officialProject )
                return state.nativeProject;

            standaloneNative |= state.nativeDocument;

            if( key == m_root || directory.GetDirCount() == 0 )
                return standaloneNative;

            directory.RemoveLastDir();
        }
    }

private:
    static wxString directoryKey( const wxString& aDirectory )
    {
        wxFileName directory = wxFileName::DirName( aDirectory );
        directory.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE );
        wxString key = directory.GetPath();
#ifdef __WXMSW__
        key.MakeLower();
#endif
        return key;
    }

    wxString m_root;
    bool     m_readDisk;
    std::map<wxString, PROJECT_FORMAT_STATE> m_states;
    std::set<wxString> m_scanned;
};


class PROJECT_ARCHIVER_DIR_ZIP_TRAVERSER : public wxDirTraverser
{
public:
    PROJECT_ARCHIVER_DIR_ZIP_TRAVERSER( const wxString& aPrjDir ) :
        m_prjDir( aPrjDir )
    {}

    virtual wxDirTraverseResult OnFile( const wxString& aFilename ) override
    {
        m_files.emplace_back( aFilename );

        return wxDIR_CONTINUE;
    }

    virtual wxDirTraverseResult OnDir( const wxString& aDirname ) override
    {
        return wxDIR_CONTINUE;
    }

    const std::vector<wxString>& GetFilesToArchive() const
    {
        return m_files;
    }

private:
    wxString              m_prjDir;
    std::vector<wxString> m_files;
};


PROJECT_ARCHIVER::PROJECT_ARCHIVER()
{
}


bool PROJECT_ARCHIVER::AreZipArchivesIdentical( const wxString& aZipFileA,
                                                const wxString& aZipFileB, REPORTER& aReporter )
{
    wxFFileInputStream streamA( aZipFileA );
    wxFFileInputStream streamB( aZipFileB );

    if( !streamA.IsOk() || !streamB.IsOk() )
    {
        aReporter.Report( _( "Could not open archive file." ), RPT_SEVERITY_ERROR );
        return false;
    }

    wxZipInputStream zipStreamA = wxZipInputStream( streamA );
    wxZipInputStream zipStreamB = wxZipInputStream( streamB );

    std::set<wxUint32> crcsA;
    std::set<wxUint32> crcsB;


    for( wxZipEntry* entry = zipStreamA.GetNextEntry(); entry; entry = zipStreamA.GetNextEntry() )
    {
        crcsA.insert( entry->GetCrc() );
    }

    for( wxZipEntry* entry = zipStreamB.GetNextEntry(); entry; entry = zipStreamB.GetNextEntry() )
    {
        crcsB.insert( entry->GetCrc() );
    }

    return crcsA == crcsB;
}


// Unarchive Files code comes from wxWidgets sample/archive/archive.cpp
bool PROJECT_ARCHIVER::Unarchive( const wxString& aSrcFile, const wxString& aDestDir,
                                  REPORTER& aReporter, const wxString& aStripTopFolder )
{
    wxFFileInputStream stream( aSrcFile );

    if( !stream.IsOk() )
    {
        aReporter.Report( _( "Could not open archive file." ), RPT_SEVERITY_ERROR );
        return false;
    }

    const wxArchiveClassFactory* archiveClassFactory =
            wxArchiveClassFactory::Find( aSrcFile, wxSTREAM_FILEEXT );

    if( !archiveClassFactory )
    {
        aReporter.Report( _( "Invalid archive file format." ), RPT_SEVERITY_ERROR );
        return false;
    }

    std::unique_ptr<wxArchiveInputStream> archiveStream( archiveClassFactory->NewStream( stream ) );

    wxString fileStatus;
    bool     ok = true;
    int      extracted = 0;

    // KEMI: optionally strip a single top-level folder so the archive content lands directly
    // in aDestDir (avoids <dest>/<name>/<name>/...).
    const wxString stripPrefix = aStripTopFolder.IsEmpty() ? wxString()
                                                           : aStripTopFolder + wxS( "/" );

    PROJECT_FORMAT_SCOPES scopes( aDestDir, true );
    std::vector<std::pair<wxFileName, wxString>> officialMembers;

    // Validate every member before the first temporary extraction file is opened. An
    // archive created before conversion must not write official files into a native project.
    for( std::unique_ptr<wxArchiveEntry> entry( archiveStream->GetNextEntry() ); entry;
         entry.reset( archiveStream->GetNextEntry() ) )
    {
        wxString name = entry->GetName( wxPATH_UNIX );

        if( !stripPrefix.IsEmpty() )
        {
            while( name.StartsWith( wxS( "/" ) ) )
                name = name.Mid( 1 );

            if( name == aStripTopFolder || name == stripPrefix )
                continue;

            if( name.StartsWith( stripPrefix ) )
                name = name.Mid( stripPrefix.length() );
        }

        wxFileName file;

        if( !WX_FILENAME::ResolveArchiveEntryPath( aDestDir, name, file ) )
        {
            aReporter.Report( wxString::Format( _( "Refusing to extract file '%s': the archive "
                                                   "entry points outside of the destination "
                                                   "directory." ), name ), RPT_SEVERITY_ERROR );
            return false;
        }

        if( entry->IsDir() )
            continue;

        scopes.Add( file );

        if( file.GetFullName().Lower().Contains( wxS( ".kicad_" ) ) )
            officialMembers.emplace_back( file, name );
    }

    if( archiveStream->GetLastError() == wxSTREAM_READ_ERROR )
    {
        aReporter.Report( _( "The archive file is damaged or could not be read completely." ),
                          RPT_SEVERITY_ERROR );
        return false;
    }

    for( const auto& [file, name] : officialMembers )
    {
        if( scopes.IsNative( file ) )
        {
            aReporter.Report( wxString::Format(
                    _( "Cannot restore official KiCad file '%s' into a KemiCAD project." ),
                    name ), RPT_SEVERITY_ERROR );
            return false;
        }
    }

    archiveStream.reset();

    if( stream.SeekI( 0 ) == wxInvalidOffset )
    {
        aReporter.Report( _( "Could not open archive file." ), RPT_SEVERITY_ERROR );
        return false;
    }

    archiveStream.reset( archiveClassFactory->NewStream( stream ) );

    for( std::unique_ptr<wxArchiveEntry> entry( archiveStream->GetNextEntry() ); entry;
         entry.reset( archiveStream->GetNextEntry() ) )
    {
        wxString entryName = entry->GetName( wxPATH_UNIX );

        if( !stripPrefix.IsEmpty() )
        {
            while( entryName.StartsWith( wxS( "/" ) ) )
                entryName = entryName.Mid( 1 );

            if( entryName == aStripTopFolder || entryName == stripPrefix )
                continue;

            if( entryName.StartsWith( stripPrefix ) )
                entryName = entryName.Mid( stripPrefix.length() );
        }

        fileStatus.Printf( _( "Extracting file '%s'." ), entryName );
        aReporter.Report( fileStatus, RPT_SEVERITY_INFO );

        // Now validate the entry name in the archive isn't trying to escape our destination path
        wxFileName target;

        if( !WX_FILENAME::ResolveArchiveEntryPath( aDestDir, entryName, target ) )
        {
            aReporter.Report( wxString::Format( _( "Refusing to extract file '%s': the archive "
                                                   "entry points outside of the destination "
                                                   "directory." ),
                                                entry->GetName() ),
                              RPT_SEVERITY_ERROR );
            return false;
        }

        wxString   fullname = target.GetFullPath();
        const bool isDir = entry->IsDir();

        // Ensure the target directory exists and create it if not.
        wxString t_path = isDir ? fullname : wxPathOnly( fullname );

        if( !wxDirExists( t_path )
                && !wxFileName::Mkdir( t_path, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        {
            aReporter.Report( wxString::Format( _( "Could not create folder '%s'." ), t_path ),
                              RPT_SEVERITY_ERROR );
            ok = false;
            continue;
        }

        // Directory entries need only be created, not extracted (0 size)
        if( isDir )
            continue;

        wxTempFileOutputStream outputFileStream( fullname );

        if( !outputFileStream.IsOk()
                || !CopyStreamData( *archiveStream, outputFileStream, entry->GetSize() )
                || !outputFileStream.Commit() )
        {
            aReporter.Report( wxString::Format( _( "Error extracting file '%s'." ), fullname ),
                              RPT_SEVERITY_ERROR );
            ok = false;
            continue;
        }

        extracted++;

        // Now let's set the filetimes based on what's in the zip
        wxFileName outputFileName( fullname );
        wxDateTime fileTime = entry->GetDateTime();

        // For now we set access, mod, create to the same datetime
        // create (third arg) is only used on Windows
        outputFileName.SetTimes( &fileTime, &fileTime, &fileTime );
    }

    if( archiveStream->GetLastError() == wxSTREAM_READ_ERROR )
    {
        aReporter.Report( _( "The archive file is damaged or could not be read completely." ),
                          RPT_SEVERITY_ERROR );
        ok = false;
    }

    if( ok && extracted == 0 )
    {
        aReporter.Report( _( "The archive file contains no files." ), RPT_SEVERITY_ERROR );
        ok = false;
    }

    if( !ok )
        return false;

    aReporter.Report( _( "Extracted project." ), RPT_SEVERITY_INFO );
    return true;
}


bool PROJECT_ARCHIVER::Archive( const wxString& aSrcDir, const wxString& aDestFile,
                                REPORTER& aReporter, bool aVerbose, bool aIncludeExtraFiles )
{

    std::set<wxString> extensions;
    std::set<wxString> files;                // File names without extensions such as fp-lib-table.

    extensions.emplace( FILEEXT::ProjectFileExtension );
    extensions.emplace( FILEEXT::ProjectLocalSettingsFileExtension );
    extensions.emplace( FILEEXT::KiCadSchematicFileExtension );
    extensions.emplace( FILEEXT::KiCadSymbolLibFileExtension );
    extensions.emplace( FILEEXT::KiCadPcbFileExtension );
    extensions.emplace( FILEEXT::KiCadFootprintFileExtension );
    extensions.emplace( FILEEXT::DesignRulesFileExtension );
    extensions.emplace( FILEEXT::DrawingSheetFileExtension );
    extensions.emplace( FILEEXT::KiCadJobSetFileExtension );
    extensions.emplace( FILEEXT::JsonFileExtension );                  // for design blocks
    extensions.emplace( FILEEXT::WorkbookFileExtension );

    for( const KEMI_FORMAT::DOC_PROFILE& profile : KEMI_FORMAT::AllProfiles() )
        extensions.emplace( wxString::FromUTF8( profile.kemiExt ) );

    files.emplace( FILEEXT::FootprintLibraryTableFileName );
    files.emplace( FILEEXT::SymbolLibraryTableFileName );
    files.emplace( FILEEXT::DesignBlockLibraryTableFileName );

    // List of additional file extensions that are only archived when aIncludeExtraFiles is true
    if( aIncludeExtraFiles )
    {
        extensions.emplace( FILEEXT::LegacyProjectFileExtension );
        extensions.emplace( FILEEXT::LegacySchematicFileExtension );
        extensions.emplace( FILEEXT::LegacySymbolLibFileExtension );
        extensions.emplace( FILEEXT::LegacySymbolDocumentFileExtension );
        extensions.emplace( FILEEXT::FootprintAssignmentFileExtension );
        extensions.emplace( FILEEXT::LegacyPcbFileExtension );
        extensions.emplace( FILEEXT::LegacyFootprintLibPathExtension );
        extensions.emplace( FILEEXT::StepFileAbrvExtension );
        extensions.emplace( FILEEXT::StepFileExtension );                       // 3d files
        extensions.emplace( FILEEXT::VrmlFileExtension );                       // 3d files
        extensions.emplace( FILEEXT::GerberJobFileExtension );                  // Gerber job files
        extensions.emplace( FILEEXT::FootprintPlaceFileExtension );             // Our position files
        extensions.emplace( FILEEXT::DrillFileExtension );                      // Fab drill files
        extensions.emplace( "nc" );                                             // Fab drill files
        extensions.emplace( "xnc" );                                            // Fab drill files
        extensions.emplace( FILEEXT::IpcD356FileExtension );
        extensions.emplace( FILEEXT::ReportFileExtension );
        extensions.emplace( FILEEXT::NetlistFileExtension );
        extensions.emplace( FILEEXT::PythonFileExtension );
        extensions.emplace( FILEEXT::PdfFileExtension );
        extensions.emplace( FILEEXT::TextFileExtension );
        extensions.emplace( FILEEXT::SpiceFileExtension );                      // SPICE files
        extensions.emplace( FILEEXT::SpiceSubcircuitFileExtension );            // SPICE files
        extensions.emplace( FILEEXT::SpiceModelFileExtension );                 // SPICE files
        extensions.emplace( FILEEXT::IbisFileExtension );
        extensions.emplace( "pkg" );
        extensions.emplace( FILEEXT::GencadFileExtension );
    }

    // Gerber files (g?, g??, .gm12 (from protel export)).
    wxRegEx gerberFiles( FILEEXT::GerberFileExtensionsRegex );
    wxASSERT( gerberFiles.IsValid() );

    bool     success = true;
    wxString msg;
    wxString oldCwd = wxGetCwd();

    wxFileName sourceDir( aSrcDir, wxEmptyString, wxEmptyString );

    wxSetWorkingDirectory( aSrcDir );

    wxFFileOutputStream ostream( aDestFile );

    if( !ostream.IsOk() )   // issue to create the file. Perhaps not writable dir
    {
        msg.Printf( _( "Failed to create file '%s'." ), aDestFile );
        aReporter.Report( msg, RPT_SEVERITY_ERROR );
        return false;
    }

    // Use a large I/O buffer to improve compatibility with cloud-synced folders.
    if( FILE* fp = ostream.GetFile()->fp() )
        setvbuf( fp, nullptr, _IOFBF, KIPLATFORM::IO::CLOUD_SYNC_BUFFER_SIZE );

    wxZipOutputStream zipstream( ostream, -1, wxConvUTF8 );

    wxDir projectDir( aSrcDir );

    if( !projectDir.IsOpened() )
    {
        msg.Printf( _( "Error opening directory: '%s'." ), aSrcDir );
        aReporter.Report( msg, RPT_SEVERITY_ERROR );

        wxSetWorkingDirectory( oldCwd );
        return false;
    }

    size_t uncompressedBytes = 0;
    PROJECT_ARCHIVER_DIR_ZIP_TRAVERSER traverser( aSrcDir );

    // Do not include hidden directories (e.g. .git, .history) or files.
    // wxDIR_DEFAULT includes wxDIR_HIDDEN, so specify flags explicitly.
    projectDir.Traverse( traverser, wxEmptyString, wxDIR_FILES | wxDIR_DIRS );

    PROJECT_FORMAT_SCOPES scopes( aSrcDir );

    for( const wxString& fileName : traverser.GetFilesToArchive() )
        scopes.Add( wxFileName( fileName ) );

    for( const wxString& fileName : traverser.GetFilesToArchive() )
    {
        wxFileName fn( fileName );

        // A converted project can retain its original files alongside the native ones.
        // Keep those originals on disk, but do not reintroduce them through a backup restore.
        if( fn.GetFullName().Lower().Contains( wxS( ".kicad_" ) ) && scopes.IsNative( fn ) )
            continue;

        wxString extLower = fn.GetExt().Lower();
        wxString fileNameLower = fn.GetName().Lower();
        bool archive = false;

        if( !extLower.IsEmpty() )
        {
            if( ( extensions.find( extLower ) != extensions.end() )
              || ( aIncludeExtraFiles && gerberFiles.Matches( extLower ) ) )
                archive = true;
        }
        else if( !fileNameLower.IsEmpty() && ( files.find( fileNameLower ) != files.end() ) )
        {
                archive = true;
        }

        if( !archive )
            continue;

        wxFileSystem fsFile;
        fn.MakeRelativeTo( aSrcDir );

        wxString relativeFn = fn.GetFullPath();

        // Read input file and add it to the zip file:
        wxFSFile* infile = nullptr;
        wxString  sysError;

        {
            // Failures are reported through aReporter, wx would also pop its own dialog.
            wxLogNull suppressSysErrorPopups;
            infile = fsFile.OpenFile( relativeFn );

            if( !infile )
            {
                if( unsigned long code = wxSysErrorCode() )
                    sysError = wxSysErrorMsgStr( code );
            }
        }

        if( infile )
        {
            bool copied = zipstream.PutNextEntry( relativeFn, infile->GetModificationTime() );

            if( copied )
            {
                infile->GetStream()->Read( zipstream );
                copied = infile->GetStream()->GetLastError() != wxSTREAM_READ_ERROR
                         && zipstream.IsOk();
            }

            copied = zipstream.CloseEntry() && copied;

            if( !copied )
            {
                msg.Printf( _( "Failed to archive file '%s'." ), relativeFn );
                aReporter.Report( msg, RPT_SEVERITY_ERROR );
                success = false;
                delete infile;
                continue;
            }

            uncompressedBytes += infile->GetStream()->GetSize();

            if( aVerbose )
            {
                msg.Printf( _( "Archived file '%s'." ), relativeFn );
                aReporter.Report( msg, RPT_SEVERITY_INFO );
            }

            delete infile;
        }
        else
        {
            if( sysError.IsEmpty() )
                msg.Printf( _( "Failed to archive file '%s'." ), relativeFn );
            else
                msg.Printf( _( "Failed to archive file '%s': %s" ), relativeFn, sysError );

            aReporter.Report( msg, RPT_SEVERITY_ERROR );
            success = false;
        }
    }

    auto reportSize =
            []( size_t aSize ) -> wxString
            {
                constexpr float KB = 1024.0;
                constexpr float MB = KB * 1024.0;

                if( aSize >= MB )
                    return wxString::Format( wxT( "%0.2f MB" ), aSize / MB );
                else if( aSize >= KB )
                    return wxString::Format( wxT( "%0.2f KB" ), aSize / KB );
                else
                    return wxString::Format( wxT( "%zu bytes" ), aSize );
            };

    if( zipstream.Close() )
    {
        // Read the final compressed size after Close() so the zip central directory
        // bytes are included in the count.
        size_t zipBytesCnt = ostream.GetSize();

        if( aVerbose && success )
        {
            msg.Printf( _( "Zip archive '%s' created (%s uncompressed, %s compressed)." ), aDestFile,
                        reportSize( uncompressedBytes ), reportSize( zipBytesCnt ) );
            aReporter.Report( msg, RPT_SEVERITY_INFO );
        }
    }
    else
    {
        msg.Printf( _( "Failed to create file '%s'." ), aDestFile );
        aReporter.Report( msg, RPT_SEVERITY_ERROR );
        success = false;
    }

    wxSetWorkingDirectory( oldCwd );
    return success;
}


/**
 * KEMI: return the single top-level folder name shared by every entry of the zip archive,
 * or an empty string if entries are not all inside one folder (or the archive is unreadable).
 */
wxString PROJECT_ARCHIVER::FindSingleTopFolder( const wxString& aZipFile )
{
    wxFFileInputStream stream( aZipFile );

    if( !stream.IsOk() )
        return wxEmptyString;

    wxZipInputStream zip( stream );
    wxString         top;
    bool             any = false;

    for( std::unique_ptr<wxZipEntry> entry( zip.GetNextEntry() ); entry;
         entry.reset( zip.GetNextEntry() ) )
    {
        wxString name = entry->GetName( wxPATH_UNIX );

        while( name.StartsWith( wxS( "/" ) ) )
            name = name.Mid( 1 );

        if( name.IsEmpty() )
            continue;

        // A file in the archive root means there is no single top-level folder.
        if( !name.Contains( wxS( "/" ) ) && !entry->IsDir() )
            return wxEmptyString;

        wxString first = name.BeforeFirst( '/' );

        if( first.IsEmpty() || first == wxS( "." ) || first == wxS( ".." ) )
            return wxEmptyString;

        if( !any )
        {
            top = first;
            any = true;
        }
        else if( first != top )
        {
            return wxEmptyString;
        }
    }

    if( zip.GetLastError() == wxSTREAM_READ_ERROR )
        return wxEmptyString;

    return any ? top : wxString();
}


wxString PROJECT_ARCHIVER::FindExtractedProjectFile( const wxString& aDir )
{
    if( !wxDirExists( aDir ) )
        return wxEmptyString;

    const wxString exts[] = {
        wxString::FromUTF8( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::PROJECT, KEMI_FORMAT::FLAVOR::KEMI ) ),
        wxString::FromUTF8( FILEEXT::ProjectFileExtension )
    };

    for( const wxString& ext : exts )
    {
        wxArrayString projectFiles;
        wxDir::GetAllFiles( aDir, &projectFiles, wxT( "*." ) + ext, wxDIR_FILES );

        if( projectFiles.empty() )
            continue;

        return projectFiles.size() == 1 ? projectFiles[0] : wxString();
    }

    return wxEmptyString;
}