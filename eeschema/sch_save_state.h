/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 */

#ifndef SCH_SAVE_STATE_H
#define SCH_SAVE_STATE_H

#include <common.h>
#include <kemi_format_profile.h>
#include <wx/filename.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <schematic.h>

#include <map>

/** Preserve a sheet's raw variables whenever they still express its physical save target. */
inline void UpdateSheetFileReference( SCHEMATIC& aSchematic, const SCH_SHEET_PATH& aPath )
{
    SCH_SHEET* sheet = aPath.Last();

    if( !sheet || sheet->IsVirtualRootSheet() || !sheet->GetScreen() )
        return;

    const wxString directory = sheet->IsTopLevelSheet()
            ? wxFileName( aSchematic.RootScreen()->GetFileName() ).GetPath()
            : wxFileName( aPath.at( aPath.size() - 2 )->GetScreen()->GetFileName() ).GetPath();
    wxFileName target( sheet->GetScreen()->GetFileName() );
    auto matches = [&]( const wxString& aReference )
    {
        wxFileName resolved( ExpandTextVars( aReference, &aSchematic.Project(), INTERNAL ) );
        return resolved.MakeAbsolute( directory ) && resolved.SameAs( target );
    };

    if( matches( sheet->GetFileName() ) )
        return;

    wxFileName reference( sheet->GetFileName() );

    if( KEMI_FORMAT::IsLegacyExt( KEMI_FORMAT::DOC::SCHEMATIC, reference.GetExt() ) )
    {
        reference.SetExt( KEMI_FORMAT::Ext( KEMI_FORMAT::DOC::SCHEMATIC, KEMI_FORMAT::FLAVOR::KEMI ) );

        if( matches( reference.GetFullPath() ) )
        {
            sheet->SetFileName( reference.GetFullPath() );
            return;
        }
    }

    target.MakeRelativeTo( directory );
    sheet->SetFileName( target.GetFullPath() );
}


/** Roll back in-memory save preparation on cancellation, failure, or Save Copy. */
class SCH_SAVE_STATE
{
public:
    explicit SCH_SAVE_STATE( SCHEMATIC& aSchematic ) : m_schematic( aSchematic )
    {
        SCH_SCREENS screens( aSchematic.Root() );

        for( size_t i = 0; i < screens.GetCount(); ++i )
        {
            SCH_SCREEN* screen = screens.GetScreen( i );
            m_screens.emplace( screen, SCREEN_STATE{ screen->GetFileName(),
                    screen->IsContentModified(), screen->GetVirtualPageNumber() } );
        }

        m_references.emplace( &aSchematic.Root(), aSchematic.Root().GetFileName() );

        for( const SCH_SHEET_PATH& path : aSchematic.Hierarchy() )
        {
            if( SCH_SHEET* sheet = path.Last() )
                m_references.emplace( sheet, sheet->GetFileName() );
        }
    }

    SCH_SAVE_STATE( const SCH_SAVE_STATE& ) = delete;
    SCH_SAVE_STATE& operator=( const SCH_SAVE_STATE& ) = delete;

    ~SCH_SAVE_STATE() { Restore(); }

    void Commit() { m_restore = false; }

    void Restore()
    {
        if( !m_restore )
            return;

        for( const auto& [screen, state] : m_screens )
        {
            screen->SetFileName( state.filename );
            screen->SetContentModified( state.modified );
            screen->SetVirtualPageNumber( state.page );
        }

        for( const auto& [sheet, filename] : m_references )
            sheet->SetFileName( filename );

        SCH_SCREENS screens( m_schematic.Root() );
        screens.BuildClientSheetPathList();
        m_restore = false;
    }

private:
    struct SCREEN_STATE
    {
        wxString filename;
        bool modified;
        int page;
    };

    SCHEMATIC& m_schematic;
    std::map<SCH_SCREEN*, SCREEN_STATE> m_screens;
    std::map<SCH_SHEET*, wxString> m_references;
    bool m_restore = true;
};

#endif // SCH_SAVE_STATE_H
