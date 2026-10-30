/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * @author Wayne Stambaugh <stambaughw@gmail.com>
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

#include "sch_io_lib_cache.h"

#include <common.h>
#include <gestfich.h>
#include <kemi_format_profile.h>
#include <lib_symbol.h>
#include <richio.h>
#include <wx/dir.h>
#include <wx_filename.h>

#include <algorithm>
#include <cstdint>


std::vector<wxString> SCH_IO_LIB_CACHE::GetSymbolLibraryFiles( const wxString& aDirectory )
{
    std::vector<wxString> files;
    wxDir                dir( aDirectory );
    wxString             name;

    if( !dir.IsOpened() )
        THROW_IO_ERRORF( _( "Cannot read symbol library directory '%s'." ), aDirectory );

    if( dir.GetFirst( &name, wxEmptyString, wxDIR_FILES ) )
    {
        do
        {
            if( KEMI_FORMAT::IsReadableExt( KEMI_FORMAT::DOC::SYMBOL_LIB, wxFileName( name ).GetExt() ) )
                files.push_back( wxFileName( aDirectory, name ).GetFullPath() );
        } while( dir.GetNext( &name ) );
    }

    std::sort( files.begin(), files.end() );
    return files;
}


long long SCH_IO_LIB_CACHE::GetSymbolLibraryTimestamp( const wxString& aPath )
{
    wxFileName fileName( aPath );
    WX_FILENAME::ResolvePossibleSymlinks( fileName );

    const wxString path = fileName.GetFullPath();
    std::vector<wxString> files;
    const bool isDirectory = wxFileName::DirExists( path );

    if( isDirectory )
        files = GetSymbolLibraryFiles( path );
    else if( fileName.FileExists() )
        files.push_back( path );

    // Include names and sizes as well as high-resolution timestamps.  A rename between
    // formats must invalidate the cache even when the timestamp and size are unchanged.
    uint64_t stamp = 14695981039346656037ULL;
    auto mix = [&]( uint64_t aValue )
    {
        stamp = ( stamp ^ aValue ) * 1099511628211ULL;
    };

    mix( isDirectory ? 1 : fileName.FileExists() ? 2 : 0 );

    for( const wxString& file : files )
    {
        for( unsigned char ch : file.utf8_string() )
            mix( ch );

        std::error_code error;
        auto time = std::filesystem::last_write_time( ToFsPath( file ), error );
        mix( error ? 0 : static_cast<uint64_t>( time.time_since_epoch().count() ) );
        mix( wxFileName( file ).GetSize().GetValue() );
    }

    // Negative values are reserved for caches that have never been loaded.
    return static_cast<long long>( stamp & 0x7fffffffffffffffULL );
}


SCH_IO_LIB_CACHE::SCH_IO_LIB_CACHE( const wxString& aFullPathAndFileName ) :
    m_modHash( 1 ),
    m_fileName( aFullPathAndFileName ),
    m_libFileName(),
    m_fileModTime( -1 ),
    m_isWritable( true ),
    m_isModified( false ),
    m_hasParseError( false )
{
    m_libType = SCH_LIB_TYPE::LT_EESCHEMA;

    // Normalize the path: if it's a directory on the filesystem, ensure m_libFileName is marked
    // as a directory so that IsDir() checks work correctly.
    if( wxFileName::DirExists( aFullPathAndFileName ) )
        m_libFileName.AssignDir( aFullPathAndFileName );
    else
        m_libFileName = aFullPathAndFileName;
}


SCH_IO_LIB_CACHE::~SCH_IO_LIB_CACHE()
{
    // When the cache is destroyed, all of the alias objects on the heap should be deleted.
    for( auto& symbol : m_symbols )
        delete symbol.second;

    m_symbols.clear();
}


void SCH_IO_LIB_CACHE::Save( const std::optional<bool>& aOpt )
{
    wxCHECK( false, /* void */ );
}


wxFileName SCH_IO_LIB_CACHE::GetRealFile() const
{
    wxFileName fn( m_libFileName );

    // If m_libFileName is a symlink follow it to the real source file
    WX_FILENAME::ResolvePossibleSymlinks( fn );

    // Normalize the path: if it's a directory on the filesystem, ensure fn is marked as a
    // directory so that IsDir() checks work correctly. wxFileName::IsDir() only checks if
    // the path string ends with a separator, not if the path is actually a directory.
    if( !fn.IsDir() && wxFileName::DirExists( fn.GetFullPath() ) )
        fn.AssignDir( fn.GetFullPath() );

    return fn;
}


long long SCH_IO_LIB_CACHE::GetLibModificationTime()
{
    wxFileName fn = GetRealFile();

    // Update the writable flag while we have a wxFileName, in a network this is possibly quite dynamic anyway.
    if( !fn.IsDir() )
    {
        m_isWritable = fn.IsFileWritable();
        return fn.GetModificationTime().GetValue().GetValue();
    }
    else
    {
        m_isWritable = fn.IsDirWritable();
    }

    return GetSymbolLibraryTimestamp( fn.GetFullPath() );
}


bool SCH_IO_LIB_CACHE::IsFile( const wxString& aFullPathAndFileName ) const
{
    return m_fileName == aFullPathAndFileName;
}


void SCH_IO_LIB_CACHE::SetFileName( const wxString& aFileName )
{
    // Update both m_fileName and m_libFileName to keep them in sync
    m_fileName = aFileName;

    // Normalize the path: if it's a directory on the filesystem, ensure m_libFileName is marked
    // as a directory so that IsDir() checks work correctly.
    if( wxFileName::DirExists( aFileName ) )
        m_libFileName.AssignDir( aFileName );
    else
        m_libFileName = aFileName;
}


bool SCH_IO_LIB_CACHE::IsFileChanged() const
{
    wxFileName fn = GetRealFile();

    if( m_fileModTime < 0 )
        return false;

    if( !fn.IsOk() )
        return false;

    if( !fn.IsDir() && fn.IsFileReadable() )
        return fn.GetModificationTime().GetValue().GetValue() != m_fileModTime;

    if( fn.IsDir() && fn.IsDirReadable() )
    {
        return GetSymbolLibraryTimestamp( fn.GetFullPath() ) != m_fileModTime;
    }

    return true;
}


LIB_SYMBOL* SCH_IO_LIB_CACHE::removeSymbol( LIB_SYMBOL* aSymbol )
{
    wxCHECK_MSG( aSymbol != nullptr, nullptr, "NULL pointer cannot be removed from library." );

    LIB_SYMBOL* firstChild = nullptr;
    LIB_SYMBOL_MAP::iterator it = m_symbols.find( aSymbol->GetName() );

    if( it == m_symbols.end() )
        return nullptr;

    // If the entry pointer doesn't match the name it is mapped to in the library, we
    // have done something terribly wrong.
    wxCHECK_MSG( &*it->second == aSymbol, nullptr,
                 "Pointer mismatch while attempting to remove alias entry <" + aSymbol->GetName() +
                 "> from library cache <" + m_libFileName.GetName() + ">." );

    // If the symbol is a root symbol used by other symbols find the first derived symbol that uses
    // the root symbol and make it the new root.
    if( aSymbol->IsRoot() )
    {
        for( const std::pair<const wxString, LIB_SYMBOL*>& entry : m_symbols )
        {
            if( entry.second->IsDerived()
              && entry.second->GetParent().lock() == aSymbol->SharedPtr() )
            {
                firstChild = entry.second;
                break;
            }
        }

        if( firstChild )
        {
            for( SCH_ITEM& drawItem : aSymbol->GetDrawItems() )
            {
                if( drawItem.Type() == SCH_FIELD_T )
                {
                    SCH_FIELD& field = static_cast<SCH_FIELD&>( drawItem );

                    if( firstChild->GetField( field.GetUntranslatedName() ) )
                        continue;
                }

                SCH_ITEM* newItem = (SCH_ITEM*) drawItem.Clone();
                drawItem.SetParent( firstChild );
                firstChild->AddDrawItem( newItem );
            }

            // Reparent the remaining derived symbols.
            for( const std::pair<const wxString, LIB_SYMBOL*>& entry : m_symbols )
            {
                if( entry.second->IsDerived()
                      && entry.second->GetParent().lock() == aSymbol->SharedPtr() )
                {
                    entry.second->SetParent( firstChild );
                }
            }
        }
    }

    m_symbols.erase( it );
    delete aSymbol;
    m_isModified = true;
    IncrementModifyHash();
    return firstChild;
}


void SCH_IO_LIB_CACHE::AddSymbol( std::unique_ptr<LIB_SYMBOL> aSymbol )
{
    wxString name = aSymbol->GetName();
    LIB_SYMBOL_MAP::iterator it = m_symbols.find( name );

    if( it != m_symbols.end() )
    {
        removeSymbol( it->second );
    }

    m_symbols[name] = aSymbol.release();
    m_isModified = true;
    IncrementModifyHash();
}


LIB_SYMBOL* SCH_IO_LIB_CACHE::GetSymbol( const wxString& aName )
{
    LIB_SYMBOL_MAP::iterator it = m_symbols.find( aName );

    if( it != m_symbols.end() )
    {
        return it->second;
    }

    return nullptr;
}
