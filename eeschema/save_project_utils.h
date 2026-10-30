#ifndef SAVE_PROJECT_UTILS_H
#define SAVE_PROJECT_UTILS_H

#include <unordered_map>
#include <kemi_format_profile.h>
#include <wx/filename.h>

class SCHEMATIC;
class SCH_SCREEN;
class SCH_SCREENS;

/** Relocate imported screens and hierarchy references before any output is written. */
void PrepareSchematicImportFiles( SCHEMATIC& aSchematic, const wxFileName& aSource,
                                  const KEMI_FORMAT::OUTPUT_TARGET& aRootTarget );

bool PrepareSaveAsFiles( SCHEMATIC& aSchematic, SCH_SCREENS& aScreens,
                         const wxFileName& aOldRoot, const wxFileName& aNewRoot,
                         bool aSaveCopy, bool aCopySubsheets, bool aIncludeExternSheets,
                         std::unordered_map<SCH_SCREEN*, wxString>& aFilenameMap,
                         wxString& aErrorMsg );

#endif
