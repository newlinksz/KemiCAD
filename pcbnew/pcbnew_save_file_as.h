/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef PCBNEW_SAVE_FILE_AS_H
#define PCBNEW_SAVE_FILE_AS_H

#include <wx/string.h>

/**
 * Copy/rewrite a pcbnew-owned project file into a Save As destination tree.
 * Exposed for QA: the pcbnew unit-test Kiface() mock does not override SaveFileAs.
 */
void PCBNEW_SaveFileAs( const wxString& aProjectBasePath, const wxString& aSrcProjectName,
                        const wxString& aNewProjectBasePath, const wxString& aNewProjectName,
                        const wxString& aSrcFilePath, wxString& aErrors );

#endif
