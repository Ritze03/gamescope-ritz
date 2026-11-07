#pragma once

#include <set>
#include <string>
#include <vector>

// Settings export / import as JSON TEXT (clipboard), the logic only -- a later
// step draws the dialogs. No files, no ImGui.
//
// Format (version 1), pretty-printed, keys nested exactly as a profile file
// nests them:
//   { "gamescope_ritz_export": 1, "profile": "Comp", "settings": { "framegen": {...} } }
//
// Which keys are selectable is a PARTITION of the serialised Settings: every
// leaf key is in exactly one ExportGroup or in ExcludedPaths(). The test
// (tests/test_profile_export.cpp) fails on a leaf in neither, so a new
// setting is always a conscious choice.
//
// Section (de)serialisation is ConfigManager's, reused through its public
// API only: a Settings struct becomes sections by SaveProfile()-ing it under a
// throwaway profile name and reading the file back; sections become a Settings
// by writing such a file and LoadProfile()-ing it. The throwaway file exists
// for a few milliseconds and is removed again.
namespace gamescope::config::profile_export
{
    // One selectable group (a child in the tree; `parent` is its rail group).
    struct ExportGroup
    {
        std::string id;      // stable key: "display.resolution"
        std::string parent;  // rail group label: "Display"
        std::string label;   // the overlay's area label
        std::string note;    // shown beside the tick, may be empty
        bool default_ticked = true;
        // Dotted paths: a leaf ("gamescope.fps_limit") or a whole section
        // ("framegen"), matched at '.' boundaries.
        std::vector<std::string> paths;
    };

    using GroupSet = std::set<std::string>; // ExportGroup::id values

    const std::vector<ExportGroup> &Groups();
    GroupSet DefaultTicks();
    // Paths never exported.
    const std::vector<std::string> &ExcludedPaths();
    // Every leaf of the serialised Settings that is in no group of `groups` and
    // not excluded. Empty for Groups(); the test's tripwire.
    std::vector<std::string> UnassignedKeys( const std::vector<ExportGroup> &groups );
    // Every leaf of the serialised Settings (dotted), for tests.
    std::vector<std::string> AllSettingsKeys();

    // ---- export -------------------------------------------------------------
    struct ExportResult
    {
        bool ok = false;
        std::string text;  // the JSON
        std::string error; // user-facing
    };
    // `bResolved`: for a game profile that inherits, true writes full resolved
    // values, false only what it stores itself. Any other profile is full.
    // Nothing ticked is valid: an empty `settings` object.
    ExportResult ExportSettings( const std::string &sProfile, const GroupSet &ticked, bool bResolved );

    // ---- parse ----------------------------------------------------------------
    struct ParsedExport
    {
        bool valid = false;
        std::string error;   // short, user-facing, when !valid
        std::string profile; // the source profile's name
        std::vector<std::string> groups_present; // group ids with >= 1 known key
        int unknown_count = 0; // keys this build does not know + values of the wrong type
        std::vector<std::string> unknown_keys; // dotted, for showing them
        std::string text;      // the input, kept for Diff/Apply
    };
    ParsedExport ParseExport( const std::string &sText );

    // ---- diff -------------------------------------------------------------------
    struct DiffRow
    {
        std::string group; // group label
        std::string label; // no labels in this layer: the dotted key
        std::string key;
        std::string old_value; // JSON text
        std::string new_value;
    };
    struct DiffResult
    {
        std::vector<DiffRow> rows; // only values that would change
        int count = 0;             // rows.size()
        int unchanged = 0;         // pasted, ticked, already equal
        std::vector<std::string> skipped_keys; // ticked, but unreadable: kept as is
    };
    // `sDestProfile` empty: a new profile from defaults. Only keys present in
    // the pasted text count.
    DiffResult DiffImport( const ParsedExport &parsed, const std::string &sDestProfile, const GroupSet &ticked );

    // ---- apply --------------------------------------------------------------------
    struct ApplyResult
    {
        bool success = false;
        std::string error;
        std::string destination; // the profile written (the unique name when new)
        int applied = 0;
        int skipped = 0;         // ticked values that were unreadable: destination's kept
        std::vector<std::string> skipped_keys;
        int unknown = 0;         // keys of the paste this build does not know
    };
    // Validates first; on invalid input, nothing ticked, or nothing in the
    // ticked groups, nothing is touched. `bNewProfile`: create `sDest` (made
    // unique with " 2", " 3"; empty -> "Imported") from defaults. Otherwise
    // `sDest` must exist: its file is first copied to profiles/<Name>.json.bak
    // (an older .bak is overwritten). An inheriting game profile stays sparse.
    // Bumps ConfigGeneration() on success.
    ApplyResult ApplyImport( const ParsedExport &parsed, const std::string &sDest, bool bNewProfile,
                             const GroupSet &ticked );
}
