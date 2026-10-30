/*
 * This program source code file is part of KemiCAD.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 */

#ifndef KEMI_PROJECT_IMPORT_H
#define KEMI_PROJECT_IMPORT_H

#include <kicommon.h>
#include <wx/string.h>

#include <vector>

class REPORTER;
class PROJECT;

namespace KEMI_FORMAT
{

/** Native destinations shared by the Manager and command-line project importers. */
struct KICOMMON_API PROJECT_IMPORT_TARGETS
{
    wxString project;
    wxString local;
    wxString schematic;
    wxString board;
};

/**
 * Resolve an explicit output stem (or the input basename when it is empty).  Dots in a
 * stem are retained; only the explicit native project suffix is removed.  No files or
 * directories are created, and official or mismatched native suffixes throw IO_ERROR.
 */
KICOMMON_API PROJECT_IMPORT_TARGETS ResolveProjectImportTargets(
        const wxString& aOutput, const wxString& aInput, const wxString& aDefaultDirectory );

/** Reject invalid destinations and existing native or official counterparts before writing. */
KICOMMON_API void ValidateProjectImportTargets( const PROJECT_IMPORT_TARGETS& aTargets );

/**
 * Create fresh JSON project/local settings for a Manager import, without loading a source
 * project or copying the official project template.  Existing destinations are never replaced.
 * Throws IO_ERROR on a conflict or write failure.
 */
KICOMMON_API void CreateProjectImportFiles( const PROJECT_IMPORT_TARGETS& aTargets );

/**
 * Register successfully saved native boards using project-relative paths. Existing board IDs
 * are preserved and repeat registration is harmless. Validate every path before changing the
 * project; throws IO_ERROR for missing, outside-project or non-native boards. Does not save it.
 */
KICOMMON_API void RegisterImportedBoards( PROJECT& aProject,
                                        const std::vector<wxString>& aBoardPaths );

/** Match a declared top-level schematic by its full path relative to the project directory. */
KICOMMON_API bool IsProjectTopLevelSchematic( const PROJECT& aProject, const wxString& aFilePath );

/**
 * Archive a converted native project directory into a sibling <工程名>.zip.
 *
 * The zip is written in the parent of the project directory regardless of that
 * directory's name (including a GUI-created engineering-name subdirectory).
 * A project directly at a filesystem root has no sibling location and is rejected.
 * Contents are the project directory tree via PROJECT_ARCHIVER (skips .kicad_* in
 * native scopes).  When any path in aProtectedSourcePaths normalizes to the same
 * path as the zip destination, refuse to write so an input archive is never
 * overwritten (covers multi-input CLI imports).  Returns false on failure; never throws.
 */
KICOMMON_API bool WriteImportZip( const wxString& aProjectFilePath,
                                  REPORTER* aReporter = nullptr,
                                  const std::vector<wxString>& aProtectedSourcePaths = {} );


} // namespace KEMI_FORMAT

#endif // KEMI_PROJECT_IMPORT_H
