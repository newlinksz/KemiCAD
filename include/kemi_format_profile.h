/*
 * This program source code file is part of KemiCAD.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 */

/**
 * @file kemi_format_profile.h
 * Single source of truth for the KemiCAD native (.kemi_*) file format and its legacy
 * KiCad-compatible (.kicad_*) counterpart.
 *
 * Every reader and writer that has to know "which extension / which root token / which
 * JSON marker" asks this profile instead of hard coding strings.  See docs/kemi/格式规范.md.
 */

#ifndef KEMI_FORMAT_PROFILE_H
#define KEMI_FORMAT_PROFILE_H

#include <kicommon.h>
#include <nlohmann/json_fwd.hpp>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <wx/string.h>

// Some targets (kicad, pcm) define KICAD / KEMI as compile definitions.
#pragma push_macro( "KICAD" )
#pragma push_macro( "KEMI" )
#undef KICAD
#undef KEMI

namespace KEMI_FORMAT
{

/// Current KemiCAD container format version written into (format_version N) / JSON marker.
constexpr int FORMAT_VERSION = 1;

/// Oldest container format_version this build will still load (inclusive).
constexpr int MIN_FORMAT_VERSION = 1;

/// Name of the JSON object holding the KemiCAD marker in .kemi_pro/.kemi_prl/.kemi_jobset.
constexpr const char* JSON_MARKER_KEY = "kemicad";

enum class DOC
{
    PROJECT,
    PROJECT_LOCAL,
    JOBSET,
    SCHEMATIC,
    PCB,
    SYMBOL_LIB,
    FOOTPRINT,
    DRAWING_SHEET,
    DESIGN_RULES
};

enum class FLAVOR
{
    KEMI,    ///< KemiCAD native: .kemi_* extension, kemi_* root / JSON marker
    LEGACY    ///< Legacy / official KiCad compatible: .kicad_* extension and root
};

class KEMI_ONLY_OUTPUT_POLICY;

/** A validated final destination, independent of a writer's temporary storage path. */
class KICOMMON_API OUTPUT_TARGET
{
public:
    DOC Document() const { return m_document; }
    const wxString& FinalPath() const { return m_finalPath; }
    FLAVOR Flavor() const { return FLAVOR::KEMI; }

private:
    friend class KEMI_ONLY_OUTPUT_POLICY;

    OUTPUT_TARGET( DOC aDocument, const wxString& aFinalPath ) :
            m_document( aDocument ), m_finalPath( aFinalPath )
    {}

    DOC      m_document;
    wxString m_finalPath;
};

/**
 * Import output contract for KEMI_ONLY conversion.  This object does not select import
 * entry points or create directories.  Writers receive a validated final target and must
 * use its explicit flavor even when writing through a temporary or extensionless path.
 */
class KICOMMON_API KEMI_ONLY_OUTPUT_POLICY
{
public:
    using WRITER = std::function<bool( const wxString&, const OUTPUT_TARGET& )>;

    explicit KEMI_ONLY_OUTPUT_POLICY( const wxString& aFinalDirectory );

    const wxString& FinalDirectory() const { return m_finalDirectory; }

    /**
     * Resolve a name relative to FinalDirectory() and select the document's .kemi_* suffix.
     * Absolute names must be within that directory too.  Existing .pretty directory
     * components are preserved; a FOOTPRINT target inside them receives .kemi_mod.
     * Explicit official extensions and mismatched .kemi_* extensions throw IO_ERROR.
     */
    OUTPUT_TARGET Target( DOC aDoc, const wxString& aName ) const;

    /** Reject a forbidden physical write path before opening a formatter or file. */
    void CheckWritePath( DOC aDoc, const wxString& aPath ) const;

    /**
     * Validate both destinations before invoking aWriter.  An empty aStoragePath writes
     * directly to the final target; otherwise aWriter must finish the temporary write
     * and promote it to the supplied target's FinalPath().  It returns true on success;
     * false is reported as IO_ERROR, and exceptions propagate.  Only successful calls
     * enter Manifest(), once per final target; temporary files are never listed.
     */
    void Write( DOC aDoc, const wxString& aName, const WRITER& aWriter,
                const wxString& aStoragePath = wxEmptyString );

    const std::vector<OUTPUT_TARGET>& Manifest() const { return m_manifest; }

private:
    wxString                  m_finalDirectory;
    std::vector<OUTPUT_TARGET> m_manifest;
    std::set<wxString>         m_manifestPaths;
};

struct DOC_PROFILE
{
    DOC         doc;
    const char* kemiExt;       ///< extension without dot, e.g. "kemi_sch"
    const char* kicadExt;      ///< legacy extension without dot, e.g. "kicad_sch"
    const char* kemiRoot;      ///< s-expr root token for KEMI flavor (nullptr for JSON docs)
    const char* kicadRoot;     ///< s-expr root token for KICAD flavor (nullptr for JSON / DRU)
    const char* jsonType;      ///< "document_type" value of the JSON marker (nullptr for s-expr)
};

KICOMMON_API const DOC_PROFILE& Profile( DOC aDoc );

KICOMMON_API const std::vector<DOC_PROFILE>& AllProfiles();

/// Extension (no dot) for the requested flavor.
KICOMMON_API std::string Ext( DOC aDoc, FLAVOR aFlavor );

/// Root token for the requested flavor (s-expr documents only).
KICOMMON_API std::string RootToken( DOC aDoc, FLAVOR aFlavor );

/// All extensions this build can read for aDoc (legacy first during P1).
KICOMMON_API std::vector<std::string> ReadableExts( DOC aDoc );

/// True when aExt (no dot, case-insensitive) is the KemiCAD extension of aDoc.
KICOMMON_API bool IsKemiExt( DOC aDoc, const wxString& aExt );

/// True when aExt (no dot, case-insensitive) is the legacy extension of aDoc.
KICOMMON_API bool IsLegacyExt( DOC aDoc, const wxString& aExt );

/// True when aExt is any readable extension of aDoc.
KICOMMON_API bool IsReadableExt( DOC aDoc, const wxString& aExt );

/// True when aPath carries a .kemi_* extension of any document type.
KICOMMON_API bool HasKemiExt( const wxString& aPath );

/// Native and official documents, and documents belonging to either project, require native saves.
KICOMMON_API bool IsRestrictedOutput( const wxString& aDocumentPath,
                                     const wxString& aProjectPath = wxEmptyString );

/**
 * Resolve an implicit Save destination. Official sources get a sibling .kemi_* file;
 * an existing sibling is a conflict, never an overwrite. Project imports use their own transaction.
 */
KICOMMON_API wxString ResolveNativeSaveTarget( DOC aDoc, const wxString& aSourcePath );

/** Apply the same first-save conflict protection to a user-selected native destination. */
KICOMMON_API wxString ResolveNativeSaveTarget( DOC aDoc, const wxString& aSourcePath,
                                             const wxString& aRequestedPath );

/**
 * Resolve an explicit save destination; official targets are rejected as import-only.
 * aRestricted is retained for source compatibility; all physical outputs require native format.
 */
KICOMMON_API wxString PersistentOutputPath( DOC aDoc, const wxString& aRequestedPath,
                                            bool aRestricted );

/** Check a writer before it opens any output, including temporary and formatter paths. */
KICOMMON_API void CheckPersistentOutput( DOC aDoc, const wxString& aPath, FLAVOR aFlavor,
                                        bool aRestricted );

/** Rewrite only sheet filename properties, preserving every other serialized byte. */
KICOMMON_API void RemapSchematicSheetPaths( std::string& aContent, const wxString& aSourcePath,
        const wxString& aTargetPath, const std::map<wxString, wxString>& aTargets,
        std::vector<wxString>* aReferencedPaths = nullptr );

/** Reject native roots/JSON markers before an export copies even a renamed source file. */
KICOMMON_API void CheckLegacyExportSource( const wxString& aPath );

/**
 * Flavor a writer must use for the final target path.  A .kemi_* extension selects KEMI;
 * legacy, unknown and empty paths select KICAD so fragments keep the upstream syntax.
 * Callers writing temporary files or snapshots must pass the actual document target.
 */
KICOMMON_API FLAVOR FlavorForPath( DOC aDoc, const wxString& aPath );

/// Policy-bound writes always use KEMI, including temporary paths; forbidden paths throw.
KICOMMON_API FLAVOR FlavorForPath( DOC aDoc, const wxString& aPath,
                                  const KEMI_ONLY_OUTPUT_POLICY& aPolicy );

/**
 * Verify that the root found while parsing matches the extension of the source file.
 *
 * @param aSource is the reader source name (file path, or e.g. "clipboard").
 * @param aRootIsKemi true if the root token was the kemi_* one.
 * @return empty string when consistent, otherwise a translated error message.
 *         Legacy content inside a .kemi_* file is an error.  KemiCAD content inside a
 *         .kicad_* file is accepted (it can only have been produced by KemiCAD).
 */
KICOMMON_API wxString CheckRootForSource( DOC aDoc, const wxString& aSource, bool aRootIsKemi );

/**
 * Verify a (format_version N) value.
 * @return empty string when supported, otherwise a translated error message.
 */
KICOMMON_API wxString CheckFormatVersion( DOC aDoc, int aVersion );

/**
 * The value written to (generator_version ...) in every file KemiCAD saves: the KemiCAD
 * brand version (BRAND_VERSION in brand.h), never the upstream KiCad version.
 */
KICOMMON_API wxString GeneratorVersion();

/**
 * True when a (generator_version ...) value is syntactically a KemiCAD version: exactly
 * MAJOR.MINOR.PATCH, digits only, no leading zeros, no suffix or surrounding text
 * (e.g. "0.9.7", "1.0.1", "5.0.0").  Upstream KiCad writes two parts ("10.99", "9.0",
 * "5.1") and malformed values return false.  This alone does not prove the file came from
 * KemiCAD; use ShowKemiGeneratorVersion() for user-facing text.
 */
KICOMMON_API bool IsKemiGeneratorVersion( const wxString& aGeneratorVersion );

/**
 * True only when the file's own root / format marker is KemiCAD's (aKemiOrigin, e.g. a
 * kemi_pcb / kemi_sch / kemi_wks root) AND the generator value is a strict KemiCAD version.
 * When the origin is unclear the caller must show the generic "upgrade KemiCAD" text.
 */
KICOMMON_API bool ShowKemiGeneratorVersion( const wxString& aGeneratorVersion, bool aKemiOrigin );

/// Write the KemiCAD marker into a JSON settings document.
KICOMMON_API void StampJsonMarker( nlohmann::json& aDocument, DOC aDoc );

/// Remove the KemiCAD marker (used when exporting to the legacy flavor).
KICOMMON_API void ClearJsonMarker( nlohmann::json& aDocument );

/**
 * Validate the KemiCAD marker of a freshly loaded JSON document.
 *
 * Rules: a .kemi_* file must carry a marker; a marker, when present, must have the right
 * document_type and a supported format_version.
 *
 * @return empty string when valid, otherwise a translated error message.
 */
KICOMMON_API wxString ValidateJsonMarker( const nlohmann::json& aDocument, DOC aDoc,
                                          const wxString& aPath );

/// Extensions used by file dialogs and directory scans, in ReadableExts order.
KICOMMON_API std::vector<std::string> WildcardExts( DOC aDoc );

} // namespace KEMI_FORMAT

#pragma pop_macro( "KEMI" )
#pragma pop_macro( "KICAD" )

#endif // KEMI_FORMAT_PROFILE_H
