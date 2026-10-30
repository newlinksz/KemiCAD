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

#ifndef EESCHEMA_SAVE_FILE_AS_H
#define EESCHEMA_SAVE_FILE_AS_H

#include <wx/string.h>

/**
 * Copy/rewrite a eeschema-owned project file into a Save As destination tree.
 * Exposed for QA: the eeschema unit-test Kiface() mock does not override SaveFileAs.
 */
void EESCHEMA_SaveFileAs( const wxString& aProjectBasePath, const wxString& aSrcProjectName,
                        const wxString& aNewProjectBasePath, const wxString& aNewProjectName,
                        const wxString& aSrcFilePath, wxString& aErrors );

#endif
