/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2017 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Maciej Suminski <maciej.suminski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
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

#include "symbol_library_manager.h"

#include <dialogs/html_message_box.h>
#include <symbol_edit_frame.h>
#include <env_paths.h>
#include <pgm_base.h>
#include <project_sch.h>
#include <kemi_format_profile.h>
#include <kiway.h>
#include <core/profile.h>
#include <core/wx_stl_compat.h>
#include <unordered_map>
#include <wx_filename.h>
#include <sch_io/kicad_legacy/sch_io_kicad_legacy.h>
#include <progress_reporter.h>
#include <list>
#include <locale_io.h>
#include <confirm.h>
#include <gestfich.h>
#include <md5_hash.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_io/sch_io_lib_cache.h>
#include <string_utils.h>
#include <wildcards_and_files_ext.h>
#include <libraries/library_manager.h>
#include <libraries/symbol_library_adapter.h>
#include <fstream>
#include <algorithm>

#include "lib_logger.h"


static wxFileName libraryFileName( const wxString& aPath )
{
    wxFileName fileName( aPath );

    if( wxFileName::DirExists( aPath ) )
        fileName.AssignDir( aPath );

    fileName.Normalize( FN_NORMALIZE_FLAGS | wxPATH_NORM_ENV_VARS );
    WX_FILENAME::ResolvePossibleSymlinks( fileName );
    return fileName;
}


static bool sameLibraryPath( const wxString& aLeft, const wxString& aRight )
{
    auto key = []( const wxString& aPath )
    {
        wxString path = libraryFileName( aPath ).GetFullPath();

        // A removed directory must retain its identity even when the save dialog omits
        // its trailing separator and wxFileName can no longer infer that it is a directory.
        while( path.length() > 1 && ( path.EndsWith( "/" ) || path.EndsWith( "\\" ) ) )
        {
            if( path.length() == 3 && path[1] == ':' )
                break;

            path.RemoveLast();
        }

        if( !wxFileName::IsCaseSensitive() )
            path.MakeLower();

        return path;
    };

    return key( aLeft ) == key( aRight );
}


SYMBOL_LIBRARY_MANAGER::SYMBOL_LIBRARY_MANAGER( SCH_BASE_FRAME& aFrame ) :
        m_frame( &aFrame )
{
    m_logger = new LIB_LOGGER();
}


SYMBOL_LIBRARY_MANAGER::SYMBOL_LIBRARY_MANAGER( PROJECT& aProject ) :
        m_project( &aProject )
{
    m_logger = new LIB_LOGGER();
}


SYMBOL_LIBRARY_MANAGER::~SYMBOL_LIBRARY_MANAGER()
{
    delete m_logger;
}


PROJECT& SYMBOL_LIBRARY_MANAGER::project() const
{
    return m_frame ? m_frame->Prj() : *m_project;
}


bool SYMBOL_LIBRARY_MANAGER::HasModifications() const
{
    for( const auto& [name, buffer] : m_libs )
    {
        if( buffer.IsModified() )
            return true;
    }

    return false;
}


int SYMBOL_LIBRARY_MANAGER::GetHash() const
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
    return adapter->GetModifyHash();
}


int SYMBOL_LIBRARY_MANAGER::GetLibraryHash( const wxString& aLibrary ) const
{
    LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();
    auto libBufIt = m_libs.find( aLibrary );

    if( std::optional<wxString> uri = manager.GetFullURI( LIBRARY_TABLE_TYPE::SYMBOL, aLibrary, true ); uri )
    {
        // Mix a file modification timestamp into the hash so that external changes (e.g. a git
        // branch switch) are detected by the library tree synchronizer without a restart.
        long long  mtime = 0;

        wxLogNull silence;

        try
        {
            mtime = SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( *uri );
        }
        catch( const IO_ERROR& )
        {
            // An unreadable directory must invalidate the previously displayed tree.
            mtime = -1;
        }

        size_t base = std::hash<std::string>{}( aLibrary.ToStdString() + uri->ToStdString() );

        if( libBufIt != m_libs.end() )
            base ^= std::hash<int>{}( libBufIt->second.GetHash() );

        return static_cast<int>( base ^ static_cast<size_t>( mtime ) );
    }

    return libBufIt != m_libs.end() ? libBufIt->second.GetHash() : -1;
}


wxArrayString SYMBOL_LIBRARY_MANAGER::GetLibraryNames() const
{
    wxArrayString res;

    LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();

    for( const LIBRARY_TABLE_ROW* row : manager.Rows( LIBRARY_TABLE_TYPE::SYMBOL ) )
    {
        // Database libraries are hidden from the symbol editor at the moment
        if( row->Type() == SCH_IO_MGR::ShowType( SCH_IO_MGR::SCH_DATABASE ) )
            continue;

        res.Add( row->Nickname() );
    }

    res.Sort( []( const wxString& lhs, const wxString& rhs ) -> int
              {
                  return StrNumCmp( lhs, rhs, true );
              } );

    return res;
}


bool SYMBOL_LIBRARY_MANAGER::SaveLibrary( const wxString& aLibrary, const wxString& aFileName,
                                          SCH_IO_MGR::SCH_FILE_T aFileType )
{
    wxCHECK( aFileType != SCH_IO_MGR::SCH_FILE_T::SCH_LEGACY, false );

    const bool projectRestricted = KEMI_FORMAT::HasKemiExt( project().GetProjectFullName() );
    LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();
    const std::optional<wxString> uri = manager.GetFullURI( LIBRARY_TABLE_TYPE::SYMBOL, aLibrary, true );

    try
    {
        if( !wxDirExists( aFileName ) )
        {
            if( uri && KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::SYMBOL_LIB,
                                               wxFileName( *uri ).GetExt() ) )
            {
                KEMI_FORMAT::ResolveNativeSaveTarget( KEMI_FORMAT::DOC::SYMBOL_LIB, *uri, aFileName );
            }

            KEMI_FORMAT::CheckPersistentOutput( KEMI_FORMAT::DOC::SYMBOL_LIB, aFileName,
                    KEMI_FORMAT::FlavorForPath( KEMI_FORMAT::DOC::SYMBOL_LIB, aFileName ), projectRestricted );
        }
    }
    catch( const IO_ERROR& error )
    {
        wxLogError( wxS( "%s" ), error.What() );
        return false;
    }

    wxFileName fn( aFileName );

    if( fn.FileExists() && !fn.IsFileWritable() )
        return false;

    IO_RELEASER<SCH_IO>         pi( SCH_IO_MGR::FindPlugin( aFileType ) );
    bool                        res = true;    // assume all libraries are successfully saved
    std::map<std::string, UTF8> properties;

    properties.emplace( SCH_IO_KICAD_LEGACY::PropBuffering, "" );

    auto it = m_libs.find( aLibrary );

    if( it != m_libs.end() )
    {
        // Handle buffered library
        if( projectRestricted )
        {
            for( const auto& buffer : it->second.GetBuffers() )
                buffer->GetSymbol().SetKemiOnly();
        }

        return it->second.SaveLibrary( aFileName, &*pi, uri && sameLibraryPath( *uri, aFileName ) );
    }
    else
    {
        // Handle original library
        for( LIB_SYMBOL* symbol : getOriginalSymbols( aLibrary ) )
        {
            if( projectRestricted )
                symbol->SetKemiOnly();
            try
            {
                if( symbol->IsDerived() )
                {
                    std::shared_ptr< LIB_SYMBOL > oldParent = symbol->GetParent().lock();

                    wxCHECK_MSG( oldParent, false,
                                 wxString::Format( wxT( "Derived symbol '%s' found with undefined parent." ),
                                                   symbol->GetName() ) );

                    LIB_SYMBOL* libParent = pi->LoadSymbol( aFileName, oldParent->GetName(), &properties );

                    if( !libParent )
                    {
                        std::unique_ptr<LIB_SYMBOL> newParent = std::make_unique<LIB_SYMBOL>( *oldParent );
                        pi->SaveSymbol( aFileName, std::move( newParent ), &properties );

                        // We cannot use the old parent after handing it to the plugin, we
                        // must borrow a new copy from the plugin cache to reparent the derived symbol.
                        libParent = pi->LoadSymbol( aFileName, oldParent->GetName(), &properties );
                    }
                    else
                    {
                        // Copy embedded files from the in-memory parent to the loaded parent
                        // This ensures that any embedded files added to the parent are preserved
                        //
                        // We do this manually rather than using the assignment operator to avoid
                        // potential ABI issues where the size of EMBEDDED_FILES differs between
                        // compilation units, potentially causing the assignment to overwrite
                        // members of LIB_SYMBOL (like m_me) that follow the EMBEDDED_FILES base.
                        libParent->ClearEmbeddedFiles();

                        for( const auto& [name, file] : oldParent->EmbeddedFileMap() )
                            libParent->AddFile( new EMBEDDED_FILES::EMBEDDED_FILE( *file ) );

                        libParent->SetAreFontsEmbedded( oldParent->GetAreFontsEmbedded() );
                        libParent->SetFileAddedCallback( oldParent->GetFileAddedCallback() );
                    }

                    std::unique_ptr<LIB_SYMBOL> newSymbol = std::make_unique<LIB_SYMBOL>( *symbol );
                    newSymbol->SetParent( libParent );
                    pi->SaveSymbol( aFileName, std::move( newSymbol ), &properties );
                }
                else if( !pi->LoadSymbol( aFileName, symbol->GetName(), &properties ) )
                {
                    pi->SaveSymbol( aFileName, std::make_unique<LIB_SYMBOL>( *symbol ), &properties );
                }
            }
            catch( ... )
            {
                res = false;
                break;
            }
        }
    }

    if( !res )
        return false;

    try
    {
        pi->SaveLibrary( aFileName );
    }
    catch( ... )
    {
        // return false because something happens.
        // The library is not successfully saved
        res = false;
    }

    return res;
}


bool SYMBOL_LIBRARY_MANAGER::IsLibraryModified( const wxString& aLibrary ) const
{
    auto it = m_libs.find( aLibrary );
    return it != m_libs.end() ? it->second.IsModified() : false;
}


bool SYMBOL_LIBRARY_MANAGER::IsSymbolModified( const wxString& aSymbolName, const wxString& aLibrary ) const
{
    auto libIt = m_libs.find( aLibrary );

    if( libIt == m_libs.end() )
        return false;

    const LIB_BUFFER&                    buf = libIt->second;
    const std::shared_ptr<SYMBOL_BUFFER> symbolBuf = buf.GetBuffer( aSymbolName );

    return symbolBuf ? symbolBuf->IsModified() : false;
}


void SYMBOL_LIBRARY_MANAGER::SetSymbolModified( const wxString& aSymbolName, const wxString& aLibrary )
{
    auto libIt = m_libs.find( aLibrary );

    if( libIt == m_libs.end() )
        return;

    const LIB_BUFFER&              buf = libIt->second;
    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = buf.GetBuffer( aSymbolName );

    wxCHECK( symbolBuf, /* void */ );

    symbolBuf->GetScreen()->SetContentModified();
}


bool SYMBOL_LIBRARY_MANAGER::ClearLibraryModified( const wxString& aLibrary ) const
{
    auto libIt = m_libs.find( aLibrary );

    if( libIt == m_libs.end() )
        return false;

    for( const std::shared_ptr<SYMBOL_BUFFER>& symbolBuf : libIt->second.GetBuffers() )
    {
        SCH_SCREEN* screen = symbolBuf->GetScreen();

        if( screen )
            screen->SetContentModified( false );
    }

    return true;
}


bool SYMBOL_LIBRARY_MANAGER::ClearSymbolModified( const wxString& aSymbolName, const wxString& aLibrary ) const
{
    auto libIt = m_libs.find( aLibrary );

    if( libIt == m_libs.end() )
        return false;

    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = libIt->second.GetBuffer( aSymbolName );
    wxCHECK( symbolBuf, false );

    symbolBuf->GetScreen()->SetContentModified( false );
    return true;
}


bool SYMBOL_LIBRARY_MANAGER::IsLibraryReadOnly( const wxString& aLibrary ) const
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
    return !adapter->IsSymbolLibWritable( aLibrary );
}


bool SYMBOL_LIBRARY_MANAGER::IsLibraryLoaded( const wxString& aLibrary ) const
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
    return adapter->IsLibraryLoaded( aLibrary );
}


std::list<LIB_SYMBOL*> SYMBOL_LIBRARY_MANAGER::EnumerateSymbols( const wxString& aLibrary,
                                                               bool aReloadCleanBuffer )
{
    std::list<LIB_SYMBOL*> ret;
    auto libIt = m_libs.find( aLibrary );

    if( libIt != m_libs.end() )
    {
        if( aReloadCleanBuffer )
            libIt->second.ReloadIfChanged();

        for( const std::shared_ptr<SYMBOL_BUFFER>& symbolBuf : libIt->second.GetBuffers() )
            ret.push_back( &symbolBuf->GetSymbol() );
    }
    else
    {
        SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
        std::vector<LIB_SYMBOL*> symbols = adapter->GetSymbols( aLibrary );

        std::copy( symbols.begin(), symbols.end(), std::back_inserter( ret ) );
    }

    return ret;
}


LIB_SYMBOL* SYMBOL_LIBRARY_MANAGER::GetBufferedSymbol( const wxString& aSymbolName, const wxString& aLibrary )
{
    // try the library buffers first
    LIB_BUFFER& libBuf = getLibraryBuffer( aLibrary );
    LIB_SYMBOL* bufferedSymbol = libBuf.GetSymbol( aSymbolName );

    if( !bufferedSymbol ) // no buffer symbol found
    {
        SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
        std::optional<LIBRARY_TABLE_ROW*> row = adapter->GetRow( aLibrary );

        // Native buffers are complete snapshots. Falling back to the adapter's older cache
        // could resurrect a symbol that was removed on disk before the clean buffer reloaded.
        if( row && ( *row )->Type() == SCH_IO_MGR::ShowType( SCH_IO_MGR::SCH_KICAD ) )
            return nullptr;

        // create a copy of the symbol
        try
        {
            LIB_SYMBOL* symbol = adapter->LoadSymbol( aLibrary, aSymbolName );

            if( symbol == nullptr )
                THROW_IO_ERROR( _( "Symbol not found." ) );

            LIB_SYMBOL* bufferedParent = nullptr;

            // Create parent symbols on demand so parent symbol can be set.
            if( symbol->IsDerived() )
            {
                std::shared_ptr<LIB_SYMBOL> parent = symbol->GetParent().lock();
                wxCHECK_MSG( parent, nullptr, wxString::Format( "Derived symbol '%s' found with undefined parent.",
                                                                symbol->GetName() ) );

                // Check if the parent symbol buffer has already be created.
                bufferedParent = libBuf.GetSymbol( parent->GetName() );

                if( !bufferedParent )
                {
                    std::unique_ptr<LIB_SYMBOL> newParent = std::make_unique<LIB_SYMBOL>( *parent );
                    bufferedParent = newParent.get();
                    libBuf.CreateBuffer( std::move( newParent ), std::make_unique<SCH_SCREEN>() );
                }
            }

            std::unique_ptr<LIB_SYMBOL> newSymbol = std::make_unique<LIB_SYMBOL>( *symbol );
            bufferedSymbol = newSymbol.get();

            if( bufferedParent )
                newSymbol->SetParent( bufferedParent );

            libBuf.CreateBuffer( std::move( newSymbol ), std::make_unique<SCH_SCREEN>() );
        }
        catch( const IO_ERROR& e )
        {
            wxString msg;

            msg.Printf( _( "Error loading symbol %s from library '%s'." ),
                        aSymbolName,
                        aLibrary );
            DisplayErrorMessage( m_frame, msg, e.What() );
            bufferedSymbol = nullptr;
        }
    }

    return bufferedSymbol;
}


SCH_SCREEN* SYMBOL_LIBRARY_MANAGER::GetScreen( const wxString& aSymbolName, const wxString& aLibrary )
{
    auto it = m_libs.find( aLibrary );
    wxCHECK( it != m_libs.end(), nullptr );

    LIB_BUFFER&                    buf = it->second;
    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = buf.GetBuffer( aSymbolName );

    return symbolBuf ? symbolBuf->GetScreen() : nullptr;
}


SYMBOL_BUFFER* SYMBOL_LIBRARY_MANAGER::GetBuffer( const wxString& aSymbolName, const wxString& aLibrary,
                                                bool aReloadCleanBuffer )
{
    if( !LibraryExists( aLibrary ) )
        return nullptr;

    LIB_BUFFER& libBuf = getLibraryBuffer( aLibrary );

    if( aReloadCleanBuffer )
        libBuf.ReloadIfChanged();

    // Callers must not retain this pointer across a library reload.
    return libBuf.GetBuffer( aSymbolName ).get();
}


bool SYMBOL_LIBRARY_MANAGER::UpdateSymbol( LIB_SYMBOL* aSymbol, const wxString& aLibrary )
{
    LIB_BUFFER&                    libBuf = getLibraryBuffer( aLibrary );
    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = libBuf.GetBuffer( aSymbol->GetName() );

    if( symbolBuf )     // Existing symbol.
    {
        LIB_SYMBOL& bufferedSymbol = symbolBuf->GetSymbol();

        // If we are coming from a different library, the library ID needs to be preserved
        const LIB_ID libId = bufferedSymbol.GetLibId();
        bufferedSymbol = *aSymbol;
        bufferedSymbol.SetLibId( libId );

        symbolBuf->GetScreen()->SetContentModified();
    }
    else                // New symbol
    {
        std::unique_ptr<LIB_SYMBOL> symbolCopy = std::make_unique<LIB_SYMBOL>( *aSymbol, nullptr );

        symbolCopy->SetLibId( LIB_ID( aLibrary, aSymbol->GetLibId().GetLibItemName() ) );

        std::unique_ptr<SCH_SCREEN> newScreen = std::make_unique<SCH_SCREEN>();
        newScreen->SetContentModified();

        libBuf.CreateBuffer( std::move( symbolCopy ), std::move( newScreen ), true );
    }

    return true;
}


bool SYMBOL_LIBRARY_MANAGER::UpdateSymbolAfterRename( LIB_SYMBOL* aSymbol, const wxString& aOldName,
                                                      const wxString& aLibrary )
{
    LIB_BUFFER&                    libBuf = getLibraryBuffer( aLibrary );
    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = libBuf.GetBuffer( aOldName );

    wxCHECK( symbolBuf && aSymbol, false );

    libBuf.UpdateBuffer( *symbolBuf, *aSymbol );
    OnDataChanged();

    return true;
}


LIB_ID SYMBOL_LIBRARY_MANAGER::RevertSymbol( const LIB_ID& aIdToRevert )
{
    auto it = m_libs.find( aIdToRevert.GetLibNickname() );

    if( it == m_libs.end() )    // no items to flush
        return aIdToRevert;

    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = it->second.GetBuffer( aIdToRevert.GetLibItemName() );
    wxCHECK( symbolBuf, aIdToRevert );
    LIB_SYMBOL original( symbolBuf->GetOriginal() );

    if( original.GetName() != aIdToRevert.GetLibItemName().wx_str() )
    {
        UpdateSymbolAfterRename( &original, aIdToRevert.GetLibItemName(), aIdToRevert.GetLibNickname() );
    }
    else
    {
        // copy the initial data to the current symbol to restore
        symbolBuf->GetSymbol() = original;
        OnDataChanged();
    }

    return LIB_ID( aIdToRevert.GetLibNickname(), original.GetName() );
}


bool SYMBOL_LIBRARY_MANAGER::RevertLibrary( const wxString& aLibrary )
{
    auto it = m_libs.find( aLibrary );

    if( it == m_libs.end() )    // nothing to reverse
        return false;

    m_libs.erase( it );
    OnDataChanged();

    return true;
}


bool SYMBOL_LIBRARY_MANAGER::RevertAll()
{
    bool retv = true;

    // Nothing to revert.
    if( GetHash() == 0 )
        return true;

    for( auto& [libName, libBuffer] : m_libs )
    {
        if( !libBuffer.IsModified() )
            continue;

        // A buffer could be modified and then deleted, so make sure it's replaced on the
        // active list first, and any previous changes can then be reverted below.
        libBuffer.RevertDeletedBuffers();

        for( const std::shared_ptr<SYMBOL_BUFFER>& buffer : libBuffer.GetBuffers() )
        {
            if( !buffer->IsModified() )
                continue;

            RevertSymbol( LIB_ID( libName, buffer->GetOriginal().GetName() ) );
        }

        libBuffer.RevertAddedBuffers();
    }

    return retv;
}


bool SYMBOL_LIBRARY_MANAGER::RemoveSymbol( const wxString& aSymbolName, const wxString& aLibrary )
{
    LIB_BUFFER&                    libBuf = getLibraryBuffer( aLibrary );
    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = libBuf.GetBuffer( aSymbolName );
    wxCHECK( symbolBuf, false );

    bool retv = true;

    retv &= libBuf.DeleteBuffer( *symbolBuf );
    OnDataChanged();

    return retv;
}


LIB_SYMBOL* SYMBOL_LIBRARY_MANAGER::GetSymbol( const wxString& aSymbolName, const wxString& aLibrary ) const
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );

    // Try the library buffers first
    auto libIt = m_libs.find( aLibrary );

    if( libIt != m_libs.end() )
    {
        LIB_SYMBOL* symbol = libIt->second.GetSymbol( aSymbolName );

        if( symbol )
            return symbol;

        std::optional<LIBRARY_TABLE_ROW*> row = adapter->GetRow( aLibrary );

        if( row && ( *row )->Type() == SCH_IO_MGR::ShowType( SCH_IO_MGR::SCH_KICAD ) )
            return nullptr;
    }

    // Get the original symbol
    LIB_SYMBOL* symbol = nullptr;

    try
    {
        symbol = adapter->LoadSymbol( aLibrary, aSymbolName );
    }
    catch( const IO_ERROR& e )
    {
        wxString msg;

        msg.Printf( _( "Cannot load symbol '%s' from library '%s'." ),
                    aSymbolName,
                    aLibrary );
        DisplayErrorMessage( m_frame, msg, e.What() );
    }

    return symbol;
}


bool SYMBOL_LIBRARY_MANAGER::SymbolExists( const wxString& aSymbolName, const wxString& aLibrary ) const
{
    auto        libBufIt = m_libs.find( aLibrary );
    LIB_SYMBOL* symbol = nullptr;

    if( libBufIt != m_libs.end() )
        return libBufIt->second.GetBuffer( aSymbolName ) != nullptr;

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );

    try
    {
        symbol = adapter->LoadSymbol( aLibrary, aSymbolName );
    }
    catch( IO_ERROR& )
    {
        // checking if certain symbol exists, so its absence is perfectly fine
    }

    return symbol != nullptr;
}


bool SYMBOL_LIBRARY_MANAGER::SymbolNameInUse( const wxString& aName, const wxString& aLibrary )
{
    wxArrayString existing;

    // NB: GetSymbolNames() is mostly used for GUI stuff, so it returns unescaped names
    GetSymbolNames( aLibrary, existing );

    wxString unescapedName = UnescapeString( aName );

    for( wxString& candidate : existing )
    {
        if( candidate.CmpNoCase( unescapedName ) == 0 )
            return true;
    }

    return false;
}


bool SYMBOL_LIBRARY_MANAGER::LibraryExists( const wxString& aLibrary, bool aCheckEnabled ) const
{
    if( aLibrary.IsEmpty() )
        return false;

    if( m_libs.count( aLibrary ) > 0 )
        return true;

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );

    return adapter->HasLibrary( aLibrary, aCheckEnabled );
}


wxString SYMBOL_LIBRARY_MANAGER::GetUniqueLibraryName() const
{
    wxString name = "New_Library";

    if( !LibraryExists( name ) )
        return name;

    name += "_";

    for( unsigned int i = 0; i < std::numeric_limits<unsigned int>::max(); ++i )
    {
        if( !LibraryExists( name + wxString::Format( "%u", i ) ) )
            return name + wxString::Format( "%u", i );
    }

    wxFAIL;
    return wxEmptyString;
}


void SYMBOL_LIBRARY_MANAGER::GetSymbolNames( const wxString& aLibraryName, wxArrayString& aSymbolNames,
                                             SYMBOL_NAME_FILTER aFilter )
{
    LIB_BUFFER& libBuf = getLibraryBuffer( aLibraryName );

    libBuf.GetSymbolNames( aSymbolNames, aFilter );
}


size_t SYMBOL_LIBRARY_MANAGER::GetDerivedSymbolNames( const wxString& aSymbolName, const wxString& aLibraryName,
                                                      wxArrayString& aList )
{
    LIB_BUFFER& libBuf = getLibraryBuffer( aLibraryName );

    return libBuf.GetDerivedSymbolNames( aSymbolName, aList );
}


bool SYMBOL_LIBRARY_MANAGER::HasDerivedSymbols( const wxString& aSymbolName, const wxString& aLibraryName )
{
    LIB_BUFFER& libBuf = getLibraryBuffer( aLibraryName );

    return libBuf.HasDerivedSymbols( aSymbolName );
}


size_t SYMBOL_LIBRARY_MANAGER::GetLibraryCount() const
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
    return adapter->GetLibraryNames().size();
}


wxString SYMBOL_LIBRARY_MANAGER::getLibraryName( const wxString& aFilePath )
{
    wxFileName fn( aFilePath );
    return fn.GetName();
}


bool SYMBOL_LIBRARY_MANAGER::addLibrary( const wxString& aFilePath, bool aCreate, LIBRARY_TABLE_SCOPE aScope )
{
    if( aCreate )
    {
        try
        {
            KEMI_FORMAT::CheckPersistentOutput( KEMI_FORMAT::DOC::SYMBOL_LIB, aFilePath,
                    KEMI_FORMAT::FlavorForPath( KEMI_FORMAT::DOC::SYMBOL_LIB, aFilePath ),
                    KEMI_FORMAT::HasKemiExt( project().GetProjectFullName() ) );
        }
        catch( const IO_ERROR& error )
        {
            wxLogError( wxS( "%s" ), error.What() );
            return false;
        }
    }
    wxString libName = getLibraryName( aFilePath );
    wxCHECK( !LibraryExists( libName ), false );  // either create or add an existing one

    // try to use path normalized to an environmental variable or project path
    wxString relPath = NormalizePath( aFilePath, &Pgm().GetLocalEnvVariables(), &project() );

    SCH_IO_MGR::SCH_FILE_T schFileType = SCH_IO_MGR::GuessPluginTypeFromLibPath( aFilePath, aCreate ? KICTL_CREATE
                                                                                                    : 0 );

    if( schFileType == SCH_IO_MGR::SCH_FILE_UNKNOWN )
        schFileType = SCH_IO_MGR::SCH_LEGACY;

    SYMBOL_LIBRARY_ADAPTER*       adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
    LIBRARY_MANAGER&              manager = Pgm().GetLibraryManager();
    std::optional<LIBRARY_TABLE*> optTable = manager.Table( LIBRARY_TABLE_TYPE::SYMBOL, aScope );

    wxCHECK( optTable, false );
    LIBRARY_TABLE* table = optTable.value();

    try
    {
        bool               success = true;
        LIBRARY_TABLE_ROW& row = table->InsertRow();

        row.SetNickname( libName );
        row.SetURI( relPath );
        row.SetType( SCH_IO_MGR::ShowType( schFileType ) );

        table->Save().map_error(
                [&]( const LIBRARY_ERROR& aError )
                {
                    wxMessageBox( _( "Error saving library table:\n\n" ) + aError.message,
                                  _( "File Save Error" ), wxOK | wxICON_ERROR );
                    success = false;
                } );

        if( !success )
            return false;

        if( aCreate )
        {
            wxCHECK( schFileType != SCH_IO_MGR::SCH_FILE_T::SCH_LEGACY, false );

            if( !adapter->CreateLibrary( libName ) )
            {
                table->Rows().erase( table->Rows().end() - 1 );

                table->Save().map_error(
                        [&]( const LIBRARY_ERROR& aError )
                        {
                            wxMessageBox( _( "Error saving library table:\n\n" ) + aError.message,
                                          _( "File Save Error" ), wxOK | wxICON_ERROR );
                        } );

                return false;
            }
        }

        adapter->LoadOne( libName );
        OnDataChanged();
    }
    catch( const IO_ERROR& ioe )
    {
        DisplayError( nullptr, ioe.What() );
        return false;
    }

    return true;
}


std::set<LIB_SYMBOL*> SYMBOL_LIBRARY_MANAGER::getOriginalSymbols( const wxString& aLibrary )
{
    std::set<LIB_SYMBOL*> symbols;
    wxCHECK( LibraryExists( aLibrary ), symbols );

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );

    for( LIB_SYMBOL* symbol : adapter->GetSymbols( aLibrary ) )
        symbols.insert( symbol );

    return symbols;
}


LIB_BUFFER& SYMBOL_LIBRARY_MANAGER::getLibraryBuffer( const wxString& aLibrary )
{
    auto it = m_libs.find( aLibrary );

    if( it != m_libs.end() )
        return it->second;

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &project() );
    std::optional<LIBRARY_TABLE_ROW*> row = adapter->GetRow( aLibrary );

    if( row && ( *row )->Type() == SCH_IO_MGR::ShowType( SCH_IO_MGR::SCH_KICAD ) )
    {
        LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();
        std::optional<wxString> uri = manager.GetFullURI( LIBRARY_TABLE_TYPE::SYMBOL, aLibrary, true );

        if( !uri )
            THROW_IO_ERRORF( _( "Cannot find symbol library '%s'." ), aLibrary );

        // A fresh native reader ties the content snapshot to the symbols actually copied,
        // rather than pairing possibly stale adapter objects with a new disk revision.
        LIB_BUFFER buffer( aLibrary );
        buffer.Load( *uri );
        return m_libs.emplace( aLibrary, std::move( buffer ) ).first->second;
    }

    // The requested buffer does not exist yet, so create one
    auto ret = m_libs.emplace( aLibrary, LIB_BUFFER( aLibrary ) );
    LIB_BUFFER& buf = ret.first->second;

    for( LIB_SYMBOL* symbol : getOriginalSymbols( aLibrary ) )
    {
        if( symbol->IsDerived() )
        {
            std::shared_ptr<LIB_SYMBOL> oldParent = symbol->GetParent().lock();

            wxCHECK_MSG( oldParent, buf, wxString::Format( "Derived symbol '%s' found with undefined parent.",
                                                           symbol->GetName() ) );

            LIB_SYMBOL* libParent = buf.GetSymbol( oldParent->GetName() );

            if( !libParent )
            {
                std::unique_ptr<LIB_SYMBOL> newParent = std::make_unique<LIB_SYMBOL>( *oldParent.get() );
                libParent = newParent.get();
                buf.CreateBuffer( std::move( newParent ), std::make_unique<SCH_SCREEN>() );
            }

            std::unique_ptr<LIB_SYMBOL> newSymbol = std::make_unique<LIB_SYMBOL>( *symbol );
            newSymbol->SetParent( libParent );
            buf.CreateBuffer( std::move( newSymbol ), std::make_unique<SCH_SCREEN>() );
        }
        else if( !buf.GetSymbol( symbol->GetName() ) )
        {
            buf.CreateBuffer( std::make_unique<LIB_SYMBOL>( *symbol ), std::make_unique<SCH_SCREEN>() );
        }
    }

    return buf;
}


bool SYMBOL_LIBRARY_MANAGER::UpdateLibraryBuffer( const wxString& aLibrary )
{
    try
    {
        m_libs.erase( aLibrary );
        getLibraryBuffer( aLibrary );
    }
    catch( const std::exception& e )
    {
        wxLogError( _( "Error updating library buffer: %s" ), e.what() );
        return false;
    }
    catch(...)
    {
        wxLogError( _( "Error updating library buffer." ) );
        return false;
    }

    getLibraryBuffer( aLibrary );

    return true;
}


SYMBOL_BUFFER::SYMBOL_BUFFER( std::unique_ptr<LIB_SYMBOL> aSymbol, std::unique_ptr<SCH_SCREEN> aScreen ) :
        m_screen( std::move( aScreen ) ),
        m_symbol( std::move( aSymbol ) )
{
    wxASSERT( m_symbol );
    m_original = std::make_unique<LIB_SYMBOL>( *m_symbol );
    wxASSERT( m_original );
}


SYMBOL_BUFFER::~SYMBOL_BUFFER()
{
}


void SYMBOL_BUFFER::SetSymbol( std::unique_ptr<LIB_SYMBOL> aSymbol )
{
    wxCHECK( m_symbol != aSymbol, /* void */ );
    wxASSERT( aSymbol );
    m_symbol = std::move( aSymbol );

    // If the symbol moves libraries then the original moves with it
    if( m_original->GetLibId().GetLibNickname() != m_symbol->GetLibId().GetLibNickname() )
    {
        m_original->SetLibId( LIB_ID( m_symbol->GetLibId().GetLibNickname(),
                                      m_original->GetLibId().GetLibItemName() ) );
    }
}


void SYMBOL_BUFFER::SetOriginal( std::unique_ptr<LIB_SYMBOL> aSymbol )
{
    wxCHECK( m_original != aSymbol, /* void */ );
    wxASSERT( aSymbol );
    m_original = std::move( aSymbol );

    // The original is not allowed to have a different library than its symbol
    if( m_original->GetLibId().GetLibNickname() != m_symbol->GetLibId().GetLibNickname() )
    {
        m_original->SetLibId( LIB_ID( m_symbol->GetLibId().GetLibNickname(),
                                      m_original->GetLibId().GetLibItemName() ) );
    }
}


bool SYMBOL_BUFFER::IsModified() const
{
    return m_screen && m_screen->IsContentModified();
}


static std::map<wxString, std::string> symbolLibraryRevision( const wxString& aPath )
{
    std::vector<wxString> files;

    if( wxFileName::DirExists( aPath ) )
        files = SCH_IO_LIB_CACHE::GetSymbolLibraryFiles( aPath );
    else
        files.push_back( aPath );

    std::map<wxString, std::string> revision;

    for( const wxString& path : files )
    {
        std::ifstream file( ToFsPath( path ), std::ios::binary );

        if( !file.is_open() )
            THROW_IO_ERRORF( _( "Cannot read symbol library file '%s'." ), path );

        MD5_HASH digest;
        uint8_t  block[16384];

        while( file )
        {
            file.read( reinterpret_cast<char*>( block ), sizeof( block ) );
            digest.Hash( block, static_cast<uint32_t>( file.gcount() ) );
        }

        if( file.bad() || !file.eof() )
            THROW_IO_ERRORF( _( "Cannot read symbol library file '%s'." ), path );

        digest.Finalize();
        revision.emplace( path, digest.Format( true ) );
    }

    return revision;
}


void LIB_BUFFER::Load( const wxString& aFileName )
{
    wxCHECK_RET( m_symbols.empty(), "Cannot load over an existing library edit buffer." );
    m_sourceLoadError = true;

    wxFileName source( aFileName );

    if( wxFileName::DirExists( aFileName ) )
        source.AssignDir( aFileName );

    source.Normalize( FN_NORMALIZE_FLAGS | wxPATH_NORM_ENV_VARS );
    m_sourceFileName = source.GetFullPath();
    m_sourceRevision = symbolLibraryRevision( m_sourceFileName );

    SCH_IO_KICAD_SEXPR io;
    std::vector<LIB_SYMBOL*> symbols;
    io.EnumerateSymbolLib( symbols, m_sourceFileName );
    m_kemiOnly = m_kemiOnly || io.IsLibraryKemiOnly( m_sourceFileName );
    std::map<wxString, wxString> parents;

    for( LIB_SYMBOL* symbol : symbols )
    {
        if( symbol->IsDerived() )
        {
            std::shared_ptr<LIB_SYMBOL> parent = symbol->GetParent().lock();

            if( !parent )
                THROW_IO_ERRORF( _( "Symbol '%s' has an undefined parent." ), symbol->GetName() );

            parents.emplace( symbol->GetName(), parent->GetName() );
        }

        CreateBuffer( std::make_unique<LIB_SYMBOL>( *symbol ), std::make_unique<SCH_SCREEN>() );
    }

    // Both working and original copies must outlive the temporary reader's parent objects.
    for( const auto& [name, parentName] : parents )
    {
        std::shared_ptr<SYMBOL_BUFFER> buffer = GetBuffer( name );
        LIB_SYMBOL* parent = GetSymbol( parentName );
        buffer->GetSymbol().SetParent( parent );
        buffer->GetOriginal().SetParent( parent );
    }

    if( sourceChanged() )
        THROW_IO_ERRORF( _( "Library '%s' changed on disk. Reload it before saving." ), aFileName );

    m_sourceLoadError = false;
}


bool LIB_BUFFER::sourceChanged() const
{
    return !m_sourceFileName.IsEmpty()
           && symbolLibraryRevision( m_sourceFileName ) != m_sourceRevision;
}


void LIB_BUFFER::ReloadIfChanged()
{
    if( IsModified() || !sourceChanged() )
        return;

    // Parse before replacing the clean buffers so a failed reload leaves the old revision
    // available for retry, without recording the changed disk contents as already loaded.
    LIB_BUFFER replacement( m_libName );
    replacement.Load( m_sourceFileName );
    m_symbols.swap( replacement.m_symbols );
    m_added.swap( replacement.m_added );
    m_deleted.swap( replacement.m_deleted );
    m_sourceRevision.swap( replacement.m_sourceRevision );
    m_kemiOnly = m_kemiOnly || replacement.m_kemiOnly;
    m_sourceLoadError = false;
    ++m_hash;
}


bool LIB_BUFFER::SaveLibrary( const wxString& aFileName, SCH_IO* aPlugin, bool aIsOriginal )
{
    wxCHECK( aPlugin, false );

    if( !m_sourceFileName.IsEmpty() )
    {
        // Directory URIs and save dialogs may spell the same path with different trailing
        // separators. Native source protection must not depend on the caller's classification.
        aIsOriginal = sameLibraryPath( m_sourceFileName, aFileName );
    }

    auto checkSource = [&]()
    {
        if( aIsOriginal && sourceChanged() )
            THROW_IO_ERRORF( _( "Library '%s' changed on disk. Reload it before saving." ),
                             m_sourceFileName );
    };

    try
    {
        if( m_sourceLoadError )
            THROW_IO_ERROR( _( "Cannot save a library edit buffer whose loading failed." ) );

        if( !wxDirExists( aFileName ) )
        {
            if( KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::SYMBOL_LIB,
                                        wxFileName( m_sourceFileName ).GetExt() ) )
            {
                KEMI_FORMAT::ResolveNativeSaveTarget( KEMI_FORMAT::DOC::SYMBOL_LIB,
                                                    m_sourceFileName, aFileName );
            }

            const bool restricted = m_kemiOnly || KEMI_FORMAT::HasKemiExt( m_sourceFileName )
                    || std::any_of( m_symbols.begin(), m_symbols.end(),
                            []( const auto& aBuffer ) { return aBuffer->GetSymbol().IsKemiOnly(); } );
            KEMI_FORMAT::CheckPersistentOutput( KEMI_FORMAT::DOC::SYMBOL_LIB, aFileName,
                    KEMI_FORMAT::FlavorForPath( KEMI_FORMAT::DOC::SYMBOL_LIB, aFileName ), restricted );
        }

        // Check the revision from when the edit buffer was loaded, before a fresh plugin
        // can pair stale edits with the current contents of the destination.
        checkSource();

        std::vector<std::shared_ptr<SYMBOL_BUFFER>> ordered( m_symbols.begin(), m_symbols.end() );
        std::sort( ordered.begin(), ordered.end(),
                []( const std::shared_ptr<SYMBOL_BUFFER>& aLeft,
                    const std::shared_ptr<SYMBOL_BUFFER>& aRight )
                {
                    return aLeft->GetSymbol().GetInheritanceDepth()
                           < aRight->GetSymbol().GetInheritanceDepth();
                } );

        // Replacing a cached parent can invalidate descendants. Always write ancestors first,
        // so every descendant is prepared again after its final parent has been installed.
        for( const std::shared_ptr<SYMBOL_BUFFER>& buffer : ordered )
        {
            // Originals and deletions are committed only after the complete cache is flushed.
            if( !SaveBuffer( *buffer, aFileName, aPlugin, true, false ) )
                return false;
        }

        if( aIsOriginal )
        {
            std::map<std::string, UTF8> properties = { { SCH_IO_KICAD_LEGACY::PropBuffering, "" } };

            for( const std::shared_ptr<SYMBOL_BUFFER>& buffer : m_deleted )
            {
                const wxString& name = buffer->GetOriginal().GetName();

                if( aPlugin->LoadSymbol( aFileName, name ) )
                    aPlugin->DeleteSymbol( aFileName, name, &properties );
            }
        }

        checkSource();
        aPlugin->SaveLibrary( aFileName );

        if( aIsOriginal )
        {
            if( !m_sourceFileName.IsEmpty() )
                m_sourceRevision = symbolLibraryRevision( m_sourceFileName );

            for( const std::shared_ptr<SYMBOL_BUFFER>& buffer : m_symbols )
                buffer->SetOriginal( std::make_unique<LIB_SYMBOL>( buffer->GetSymbol() ) );

            ClearDeletedBuffer();
        }
    }
    catch( const IO_ERROR& error )
    {
        wxLogError( _( "Error saving symbol library '%s'." ) + wxS( "\n%s" ), aFileName, error.What() );
        return false;
    }
    catch( const std::exception& error )
    {
        wxLogError( _( "Error saving symbol library '%s'." ) + wxS( "\n%s" ), aFileName, error.what() );
        return false;
    }

    return true;
}


LIB_SYMBOL* LIB_BUFFER::GetSymbol( const wxString& aAlias ) const
{
    std::shared_ptr<SYMBOL_BUFFER> buf = GetBuffer( aAlias );

    if( !buf )
        return nullptr;

    return &buf->GetSymbol();
}


bool LIB_BUFFER::CreateBuffer( std::unique_ptr<LIB_SYMBOL> aCopy, std::unique_ptr<SCH_SCREEN> aScreen, bool aNew )
{
    wxASSERT( aCopy );
    wxASSERT( aCopy->GetLib() == nullptr );

    // Set the parent library name,
    // otherwise it is empty as no library has been given as the owner during object construction
    LIB_ID libId = aCopy->GetLibId();
    libId.SetLibNickname( m_libName );
    aCopy->SetLibId( libId );

    std::shared_ptr<SYMBOL_BUFFER> symbolBuf = std::make_shared<SYMBOL_BUFFER>( std::move( aCopy ),
                                                                                std::move( aScreen ) );
    m_symbols.push_back( std::move( symbolBuf ) );

    if( aNew )
        m_added.push_back( m_symbols.back() );

    ++m_hash;

    return true;
}


bool LIB_BUFFER::UpdateBuffer( SYMBOL_BUFFER& aSymbolBuf, const LIB_SYMBOL& aCopy )
{
    LIB_SYMBOL& bufferedSymbol = aSymbolBuf.GetSymbol();

    bufferedSymbol = aCopy;
    ++m_hash;

    return true;
}


bool LIB_BUFFER::DeleteBuffer( const SYMBOL_BUFFER& aSymbolBuf )
{
    const auto sameBufferPredicate =
            [&]( const std::shared_ptr<SYMBOL_BUFFER>& aBuf )
            {
                return aBuf.get() == &aSymbolBuf;
            };

    auto symbolBufIt = std::find_if( m_symbols.begin(), m_symbols.end(), sameBufferPredicate );
    wxCHECK( symbolBufIt != m_symbols.end(), false );

    bool retv = true;

    // Remove all derived symbols to prevent broken inheritance.
    if( HasDerivedSymbols( aSymbolBuf.GetSymbol().GetName() )
        && ( removeChildSymbols( aSymbolBuf ) == 0 ) )
    {
        retv = false;
    }

    m_deleted.emplace_back( *symbolBufIt );
    m_symbols.erase( symbolBufIt );
    ++m_hash;

    return retv;
}


void LIB_BUFFER::RevertDeletedBuffers()
{
    for( std::shared_ptr<SYMBOL_BUFFER>& buffer : m_deleted )
        m_symbols.emplace_back( buffer );

    m_deleted.clear();
    ++m_hash;
}


void LIB_BUFFER::RevertAddedBuffers()
{
    std::erase_if( m_symbols,
                   [&]( const std::shared_ptr<SYMBOL_BUFFER>& item )
                   {
                       for( const std::shared_ptr<SYMBOL_BUFFER>& added : m_added )
                       {
                           if( added.get() == item.get() )
                               return true;
                       }

                       return false;
                   } );

    m_added.clear();
    ++m_hash;
}


/// Name of the parent a buffered symbol inherits from, or empty if it is a root symbol.
static wxString getParentName( const SYMBOL_BUFFER& aSymbolBuf )
{
    std::shared_ptr<LIB_SYMBOL> parent = aSymbolBuf.GetSymbol().GetParent().lock();

    return parent ? parent->GetName() : wxString();
}


bool LIB_BUFFER::SaveBuffer( SYMBOL_BUFFER& aSymbolBuf, const wxString& aFileName, SCH_IO* aPlugin,
                              bool aBuffer, bool aUpdateOriginal )
{
    wxCHECK( !aFileName.IsEmpty(), false );

    wxString errorMsg = _( "Error saving symbol %s to library '%s'." ) + wxS( "\n%s" );

    // Set properties to prevent saving the file on every symbol save.
    std::map<std::string, UTF8> properties;
    properties.emplace( SCH_IO_KICAD_LEGACY::PropBuffering, "" );

    LIB_SYMBOL& libSymbol = aSymbolBuf.GetSymbol();

    // Collect symbols that are *directly* derived from this symbol before renaming it
    wxArrayString derivedSymbols;

    for( const std::shared_ptr<SYMBOL_BUFFER>& buf : m_symbols )
    {
        if( getParentName( *buf ) == libSymbol.GetName() )
            derivedSymbols.push_back( buf->GetSymbol().GetName() );
    }

    {
        LIB_SYMBOL&    originalSymbol = aSymbolBuf.GetOriginal();
        const wxString originalName = originalSymbol.GetName();

        // Delete the original symbol if the symbol name has been changed.
        if( libSymbol.GetName() != originalSymbol.GetName() )
        {
            try
            {
                if( aPlugin->LoadSymbol( aFileName, originalName, aBuffer ? &properties : nullptr ) )
                    aPlugin->DeleteSymbol( aFileName, originalName, &properties );
            }
            catch( const IO_ERROR& ioe )
            {
                wxLogError( errorMsg, UnescapeString( originalName ), aFileName, ioe.What() );
                return false;
            }
        }
    }

    if( libSymbol.IsDerived() )
    {
        std::unique_ptr<LIB_SYMBOL> newCachedSymbol = std::make_unique<LIB_SYMBOL>( libSymbol );
        std::shared_ptr<LIB_SYMBOL> bufferedParent = libSymbol.GetParent().lock();

        wxCHECK( bufferedParent, false );

        // A borrowed pointer to the cached parent symbol in the library.
        LIB_SYMBOL* cachedParent = nullptr;

        try
        {
            cachedParent = aPlugin->LoadSymbol( aFileName, bufferedParent->GetName(),
                                                aBuffer ? &properties : nullptr );
        }
        catch( const IO_ERROR& )
        {
            return false;
        }

        if( !cachedParent )
        {
            // The parent symbol does not exist in the library, so we need to save it first.

            std::unique_ptr<LIB_SYMBOL> newParent = std::make_unique<LIB_SYMBOL>( *bufferedParent );
            newCachedSymbol->SetParent( newParent.get() );

            const wxString cachedParentName = newParent->GetName();

            try
            {
                aPlugin->SaveSymbol( aFileName, std::move( newParent ), aBuffer ? &properties : nullptr );
            }
            catch( const IO_ERROR& ioe )
            {
                wxLogError( errorMsg, UnescapeString( cachedParentName ), aFileName, ioe.What() );
                return false;
            }

            const wxString newCachedSymbolName = newCachedSymbol->GetName();

            try
            {
                aPlugin->SaveSymbol( aFileName, std::move( newCachedSymbol ), aBuffer ? &properties : nullptr );
            }
            catch( const IO_ERROR& ioe )
            {
                wxLogError( errorMsg, UnescapeString( newCachedSymbolName ), aFileName, ioe.What() );
                return false;
            }

            if( aUpdateOriginal )
            {
                std::unique_ptr<LIB_SYMBOL> originalParent = std::make_unique<LIB_SYMBOL>( *bufferedParent );
                LIB_SYMBOL&                 parentRef = *originalParent;
                aSymbolBuf.SetOriginal( std::move( originalParent ) );

                std::unique_ptr<LIB_SYMBOL> newSymbol = std::make_unique<LIB_SYMBOL>( libSymbol );
                newSymbol->SetParent( &parentRef );
                aSymbolBuf.SetOriginal( std::move( newSymbol ) );
            }
        }
        else
        {
            // Copy embedded files from the buffered parent to the cached parent
            // This ensures that any embedded files added to the parent are preserved
            //
            // We do this manually rather than using the assignment operator to avoid
            // potential ABI issues where the size of EMBEDDED_FILES differs between
            // compilation units, potentially causing the assignment to overwrite
            // members of LIB_SYMBOL (like m_me) that follow the EMBEDDED_FILES base.
            cachedParent->ClearEmbeddedFiles();

            for( const auto& [name, file] : bufferedParent->EmbeddedFileMap() )
                cachedParent->AddFile( new EMBEDDED_FILES::EMBEDDED_FILE( *file ) );

            cachedParent->SetAreFontsEmbedded( bufferedParent->GetAreFontsEmbedded() );
            cachedParent->SetFileAddedCallback( bufferedParent->GetFileAddedCallback() );

            newCachedSymbol->SetParent( cachedParent );

            const wxString newCachedSymbolName = newCachedSymbol->GetName();

            try
            {
                aPlugin->SaveSymbol( aFileName, std::move( newCachedSymbol ), aBuffer ? &properties : nullptr );
            }
            catch( const IO_ERROR& ioe )
            {
                wxLogError( errorMsg, UnescapeString( newCachedSymbolName ), aFileName, ioe.What() );
                return false;
            }

            if( aUpdateOriginal )
            {
                std::shared_ptr<SYMBOL_BUFFER> originalBufferedParent = GetBuffer( bufferedParent->GetName() );
                wxCHECK( originalBufferedParent, false );

                std::unique_ptr<LIB_SYMBOL> newSymbol = std::make_unique<LIB_SYMBOL>( libSymbol );
                newSymbol->SetParent( &originalBufferedParent->GetSymbol() );
                aSymbolBuf.SetOriginal( std::move( newSymbol ) );
            }
        }
    }
    else
    {
        std::unique_ptr<LIB_SYMBOL> newSymbol = std::make_unique<LIB_SYMBOL>( libSymbol );

        try
        {
            aPlugin->SaveSymbol( aFileName, std::move( newSymbol ), aBuffer ? &properties : nullptr );
        }
        catch( const IO_ERROR& ioe )
        {
            wxLogError( errorMsg, UnescapeString( libSymbol.GetName() ), aFileName, ioe.What() );
            return false;
        }

        if( aUpdateOriginal )
            aSymbolBuf.SetOriginal( std::make_unique<LIB_SYMBOL>( libSymbol ) );
    }

    bool res = true;

    // Reparent all symbols derived from the saved symbol.
    if( !derivedSymbols.IsEmpty() )
    {
        // The saved copy is owned by the plugin cache. Borrow it back to reparent the
        // derived symbols. The lookup cannot normally fail since it was just added.
        LIB_SYMBOL* parentSymbol = aPlugin->LoadSymbol( aFileName, libSymbol.GetName(), aBuffer ? &properties
                                                                                                : nullptr );

        wxCHECK_MSG( parentSymbol, false, wxS( "Failed to borrow the saved symbol back from the plugin cache." ) );

        // Save the derived symbols.
        for( const wxString& entry : derivedSymbols )
        {
            std::shared_ptr<SYMBOL_BUFFER> symbol = GetBuffer( entry );

            wxCHECK2( symbol, continue );

            std::unique_ptr<LIB_SYMBOL> derivedSymbol = std::make_unique<LIB_SYMBOL>( symbol->GetSymbol() );

            derivedSymbol->SetParent( parentSymbol );

            const wxString derivedSymbolName = derivedSymbol->GetName();

            try
            {
                aPlugin->SaveSymbol( aFileName, std::move( derivedSymbol ), aBuffer ? &properties : nullptr );
            }
            catch( const IO_ERROR& ioe )
            {
                wxLogError( errorMsg, UnescapeString( derivedSymbolName ), aFileName, ioe.What() );

                // Something went wrong, but try to save other derived symbols
                res = false;
            }
        }
    }

    ++m_hash;
    return res;
}


std::shared_ptr<SYMBOL_BUFFER> LIB_BUFFER::GetBuffer( const wxString& aSymbolName ) const
{
    for( std::shared_ptr<SYMBOL_BUFFER> entry : m_symbols )
    {
        if( entry->GetSymbol().GetName() == aSymbolName )
            return entry;
    }

    return std::shared_ptr<SYMBOL_BUFFER>( nullptr );
}


bool LIB_BUFFER::HasDerivedSymbols( const wxString& aParentName ) const
{
    wxCHECK( !aParentName.IsEmpty(), false );

    for( const std::shared_ptr<SYMBOL_BUFFER>& entry : m_symbols )
    {
        if( getParentName( *entry ) == aParentName )
            return true;
    }

    return false;
}


void LIB_BUFFER::GetSymbolNames( wxArrayString& aSymbolNames, SYMBOL_NAME_FILTER aFilter )
{
    for( std::shared_ptr<SYMBOL_BUFFER>& entry : m_symbols )
    {
        const LIB_SYMBOL& symbol = entry->GetSymbol();

        if( ( symbol.IsDerived() && ( aFilter == SYMBOL_NAME_FILTER::ROOT_ONLY ) )
            || ( symbol.IsRoot() && ( aFilter == SYMBOL_NAME_FILTER::DERIVED_ONLY ) ) )
        {
            continue;
        }

        aSymbolNames.Add( UnescapeString( symbol.GetName() ) );
    }
}


size_t LIB_BUFFER::GetDerivedSymbolNames( const wxString& aSymbolName, wxArrayString& aList ) const
{
    wxCHECK( !aSymbolName.IsEmpty(), 0 );

    // Links resolve by parent name rather than by parent object, because a buffered child can
    // still point at an unbuffered copy of its parent
    std::unordered_map<wxString, std::vector<wxString>> derivedMap;

    // Resolve every parent link up front so the walk below is only map lookups
    for( const std::shared_ptr<SYMBOL_BUFFER>& entry : m_symbols )
    {
        const wxString parentName = getParentName( *entry );

        if( !parentName.IsEmpty() )
            derivedMap[parentName].emplace_back( entry->GetSymbol().GetName() );
    }

    // Seeded with the query so that a name cycle in a corrupt library neither recurses forever nor
    // reports the root as its own descendant, which would have removeChildSymbols() delete it
    std::set<wxString> visited{ aSymbolName };

    // Assign to std::function to allow recursion
    const std::function<void( const wxString& )> getDerived =
            [&]( const wxString& aParentName )
            {
                auto it = derivedMap.find( aParentName );

                if( it != derivedMap.end() )
                {
                    for( const wxString& derivedName : it->second )
                    {
                        if( !visited.insert( derivedName ).second )
                            continue;

                        aList.Add( derivedName );

                        // Recurse to get symbols derived from this one
                        getDerived( derivedName );
                    }
                }
            };

    // A name that is not buffered simply has no children here
    getDerived( aSymbolName );

    return aList.GetCount();
}


int LIB_BUFFER::removeChildSymbols( const SYMBOL_BUFFER& aSymbolBuf )
{
    int                                                  cnt = 0;
    wxArrayString                                        derivedSymbolNames;
    std::deque<std::shared_ptr<SYMBOL_BUFFER>>::iterator it;

    if( GetDerivedSymbolNames( aSymbolBuf.GetSymbol().GetName(), derivedSymbolNames ) )
    {
        for( const wxString& symbolName : derivedSymbolNames )
        {
            it = std::find_if( m_symbols.begin(), m_symbols.end(),
                               [symbolName]( std::shared_ptr<SYMBOL_BUFFER>& buf )
                               {
                                   return buf->GetSymbol().GetName() == symbolName;
                               } );

            wxCHECK2( it != m_symbols.end(), continue );

            m_deleted.emplace_back( *it );
            m_symbols.erase( it );
            cnt += 1;
        }
    }

    return cnt;
}
