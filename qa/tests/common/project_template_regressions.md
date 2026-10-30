# Project template regressions: review MF1 / SF1 / MF2

Scope: the local Step 2 template path, based on `11b7037a376e064fe511d8b9a4e65f32d16c9ac3`.
The fixes, tests and this checklist belong to the same commit. Review evidence is
the internal review report s2-r5 (2026-10-29).
The report establishes the failures by source analysis; the old C++ failures were not run.
The MF2 follow-up is based on `8235b83c492749decdd6bfd43b98199b99326f63` and review
the internal review report s2-r6 (2026-10-29), which confirms
the original MF1/SF1 issues closed at source level and identifies missing project-variable
expansion in the new DRC validation. Runtime acceptance is still pending.
The MF2-R7 follow-up is based on `9ce428e94f2b40a75dbc6d3248e851930668ccdb` and review
the internal review report s2-r7 (2026-10-29), which identifies
the remaining board-context gap: `${COMMENT1}` from the PCB title block is valid in normal
DRC but was rejected by project-only validation. This failure is source-analysis evidence;
the pre-fix C++ failure was not executed.
The binding plan is the internal config-extension/rebrand plan (2026-10-28),
especially section 10 and F1-F6.

## Problems and required behavior

- **MF1:** The generic S-expression parser treated DRC comment punctuation as structure.
  A legal `# )` comment could terminate the newly added native wrapper and discard later rules.
  Template rules now use the PCB kiface's existing `SaveNativeDocument` entry, which calls
  `SaveNativeDesignRules`. The real DRC parser loads the source and reloads the written staging
  file, with equal rule counts required before publication. Existing native text is retained;
  official text is retained inside a native wrapper, with a version added for empty/comment-only
  official files. Full-input and native-marker checks come from the DRC parser.
- **SF1:** Publishing each file as it was converted left partial output that blocked a retry.
  All files are now prepared in a unique directory on the destination volume. Final references
  and project metadata still use the final paths. Publication refuses existing destinations,
  including files created after preflight. On failure only successfully published files from
  this attempt and its own staging files are removed. Pre-existing user files and directories
  are retained; newly created empty destination directories may remain and allow a retry.
- **MF2:** Directly parsing `${RULE_NET}` in a rule condition rejected legal templates whose
  project defines that text variable. Validation now passively loads the source rules'
  same-stem project and PCB, preferring `.kemi_pro`/`.kemi_pcb` to retained official counterparts.
  The board is attached to the read-only source project. As in normal DRC, validation converts
  board cross-references to KIIDs and uses `BOARD::ResolveTextVar` with per-line
  `ExpandTextVars(..., INTERNAL)`: title-block fields, project variables, cached board properties,
  filename and variant variables use the board context. Rules-only templates use an empty board
  attached to the companion project. Component-class selectors remain unexpanded. Both source
  and written-rule checks use this context; only validation copies expand. Native output bytes
  and official text inside the native wrapper retain variables/comments. Missing variables and
  parse errors still fail before publication; board/project loading errors are not suppressed.

The optional `PROJECT_TEMPLATE` converter lets headless callers supply the same native rules
writer. The PCB tests call the production `SaveNativeDesignRules` implementation. Common tests
inject a partial write/failure or a late destination collision solely to exercise storage recovery;
they are not evidence of DRC parsing behavior. The old common companion test's rule string checks
were replaced with the real parser round trips below; its other companion/container checks remain.
Both suites use uniquely owned QA temporary directories. The common fixture no longer removes
a fixed pre-existing temporary directory when it starts.

## Regression selection and status

Run both `ProjectTemplate` suites after changes to template enumeration, destination planning,
conversion, publication, failure recovery, or board/project text-variable validation. Include `KemiRules`
after changes affecting rule format handling or the PCB conversion path. These cases are
registered with `qa_common` and `qa_pcbnew` (the latter's existing `other` CTest group includes
the new suite).

All runtime items below are **not run / pending** on TD80 Windows as of 2026-10-29; compilation,
building and execution of tests requiring a build were explicitly prohibited for this task.
There is no tested binary/package for this change. Each runtime duration is **unmeasured (null)**,
with no timer source, shared setup time, wait or retry run to report.

| Stable ID | Issue | Test / purpose | Result | Duration ms |
| --- | --- | --- | --- | --- |
| PT-MF1-01 | MF1 | `qa_pcbnew`: `ProjectTemplate/OfficialRulesPreserveCommentsAndConstraints`; ordinary comments, comment parentheses/quotes, escaped quotes and `#` in strings; LF and CRLF; reload twice and compare count, names, conditions, layer/severity, constraint types and min/opt/max; source bytes unchanged | Pending | null |
| PT-MF1-02 | MF1 | `qa_pcbnew`: `ProjectTemplate/NativeRulesPreserveCommentsAndConstraints`; same checks, wrapper comments, output byte equality to native source | Pending | null |
| PT-MF1-03 | MF1 | `qa_pcbnew`: `ProjectTemplate/InvalidRulesDoNotPublishTemplateFiles`; content after a closed native wrapper must fail before publication; source and user sentinel unchanged | Pending | null |
| PT-MF1-04 | MF1 | `qa_pcbnew`: `ProjectTemplate/CommentOnlyOfficialRulesGainNativeVersion`; preserve comment text and source bytes and reload zero rules from the native output | Pending | null |
| PT-MF2-01 | MF2 | `qa_pcbnew`: `ProjectTemplate/RulesUseProjectTextVariablesWithoutExpandingOutput`; official/native rules and matching projects use `${RULE_NET}` in conditions and `${RULE_WIDTH}` in a constraint; native project wins over the retained official source; creation succeeds, generated project definitions and raw variables/comments survive, final-project DRC loading succeeds with two rules and correct conditions/constraints; all template file bytes unchanged. Missing `RULE_NET` fails for both formats with no project/rules published and the user sentinel unchanged | Pending | null |
| PT-MF2-02 | MF2-R7 | `qa_pcbnew`: `ProjectTemplate/RulesUseBoardTextVariablesWithoutExpandingOutput`; official/native templates have PCB title-block `COMMENT1=GND` and `COMMENT2=${RULE_WIDTH}` with project `RULE_WIDTH=0.3mm`; condition also uses a board property and footprint cross-reference. Native board/project win over retained official companions lacking the required variables. Creation succeeds; reopen the actual final project and PCB, load through DRC and explicitly check two rules, conditions, clearance min/opt/max and track width; check filename and variant/description expansion for default and named variants. Raw rule variables/comments survive (native byte equality, official text retained); snapshot every source file including metadata and retained companions and require unchanged bytes/no extra files | Pending | null |
| PT-SF1-01 | SF1 | `qa_common`: `ProjectTemplate/TemplateWriteFailureCanBeRetried`; one rule output prepared, second partial write fails; staging/final files cleaned, source and user files unchanged; fix failure and retry the same target successfully | Pending | null |
| PT-SF1-02 | SF1 | `qa_common`: `ProjectTemplate/TemplatePublishFailureKeepsUserFilesAndCanBeRetried`; a second rule destination appears after preflight; first published output rolled back, user collision preserved; move that file aside without deleting it and retry successfully | Pending | null |
| PT-HISTORY-01 | MF1/SF1 | Remaining `qa_common` `ProjectTemplate` cases: native companions, library containers/references, read-only sources, pre-existing conflicts, default templates and official output rejection | Pending | null |
| PT-HISTORY-02 | MF1 | Existing `qa_pcbnew` `KemiRules` suite: native boundaries, rule load/save and source preservation | Pending | null |
| PT-UI-01 | MF1/SF1/MF2 | Manager template creation through the default PCB kiface, board/project-variable rule reopening and an in-place retry after failure | Pending | null |

## Execution when the owner authorizes Step 8

Use matching owner-built Windows executables from this commit; these commands do not build.
Set `$qaCommon` and `$qaPcbnew` to their actual executable paths and keep logs outside source files.

```powershell
& $qaCommon --run_test=ProjectTemplate --report_level=detailed --log_level=test_suite
& $qaPcbnew '--run_test=ProjectTemplate:KemiRules' --report_level=detailed --log_level=test_suite
```

Retain the framework's per-case timings/logs under the IDs above, executable version/commit,
platform, actual command, shared preparation, retries and total elapsed time separately.
Do not report any pending item as passed solely because it is registered or compiles.

For PT-UI-01, create separate official/native templates with the comment/string cases from
`../pcbnew/test_project_template.cpp`. Create projects in fresh directories, reopen the generated
rules in the rule editor and verify both rules and their constraints. For failure recovery,
retain an unrelated user sentinel, arrange a destination write/publish failure, capture the
error, fix the blocking condition without deleting user files and retry the same destination.
Compare source/sentinel bytes before and after and capture the reopened rule list and constraints.
This manual item covers the GUI/default-kiface boundary; headless tests cover parsing and storage.
Also include the MF2 official/native variable fixtures: define `RULE_NET=GND` and
`RULE_WIDTH=0.3mm` in the source project, create and reopen the generated project, then verify
two rules, expanded conditions/constraints, retained variable definitions and unexpanded rule
file references. Remove `RULE_NET` in a separate fixture for the failure control: expect an
error and no published project/rules. Preserve before/after source hashes and the error text.
For PT-MF2-02, also put `GND` in PCB title-block comment 1 and `${RULE_WIDTH}` in comment 2,
with no project `COMMENT1`/`COMMENT2` definitions. Use `${COMMENT1}` in the rule condition
and `${COMMENT2}` in the width constraint, as in the new fixture. Reopen the generated PCB
with its generated project and verify both constraints and original variable references.
Include the fixture's board property, footprint cross-reference, filename and variant cases;
for native templates retain official companions without those definitions to verify native
selection. Capture the reopened rules and compare all source-file hashes, including metadata.

Failures use the existing `CreateProject(..., aErrorMsg)` path and manager error dialog.
Cleanup failures additionally append their path-specific diagnostic and call `wxLogWarning`.
Capture that error text/local application log, the attempted path, commit and failure stage;
rule bodies and credentials are not added to diagnostics.

## Local verification record

2026-10-29, TD80 Windows, source base `11b7037a` plus this commit's changes:

- PT-STATIC-01: `git diff --check`, exit 0, 66.558 ms, measured with
  `System.Diagnostics.Stopwatch` (monotonic). Whitespace only; not a C++ runtime check.
- PT-STATIC-02: strict UTF-8 decoding, BOM and CRLF inspection of all nine changed/new files,
  passed, 14.7222 ms, same timer source. The original missing EOF newline in
  `pcbnew/import_design_rules.cpp` was retained. Final staging/whitespace checks are repeated
  after the last source/doc edits and before commit.
- PT-STATIC-03: manually inspected declarations/call sites, final-vs-staging reference paths,
  no-overwrite publication, owned-file cleanup and CMake registration. Duration unmeasured.
- No C++ compilation, linking, automated runtime regression, manager/GUI or plugin execution,
  external publication, push or merge was performed. Default-kiface startup and actual Windows
  file-system behavior remain unverified until the owner runs the pending checks.

MF2 follow-up, 2026-10-29, TD80 Windows, source base `8235b83c` plus this commit's changes:

- PT-MF2-01 and the affected PT-MF1/PT-SF1/PT-HISTORY/PT-UI runtime checks remain **not run**;
  duration is **unmeasured (null)** with no timer source. No matching binary was built, and
  the pre-fix failure was not executed. The r6 source-analysis evidence above is retained.
- PT-MF2-STATIC-01: `git diff --check`, exit 0, **57.7639 ms**, measured with
  `System.Diagnostics.Stopwatch` (monotonic); whitespace only. Rechecked after recording these
  results and before the local commit.
- PT-MF2-STATIC-02: strict UTF-8 decoding, no BOM and CRLF checks of all five changed files,
  passed, **12.1641 ms**, same timer source. The original missing EOF newline in
  `pcbnew/import_design_rules.cpp` is retained. Rechecked after the final documentation edit.
- PT-MF2-STATIC-03: manual source inspection of same-stem native/official project selection,
  passive loading without save/activation, `PROJECT::TextVarResolver`, per-line expansion in
  both validation passes, unchanged output text and existing `qa_pcbnew` suite registration.
  Duration unmeasured; this is not execution evidence. Shared preparation/wait/retry/total
  elapsed time was not measured.
- No compilation, executable C++ QA, GUI/reopen acceptance, push or merge was performed.

MF2-R7 follow-up, 2026-10-29, TD80 Windows, source base `9ce428e9` plus this commit's changes:

- PT-MF2-02 is added to the existing `qa_pcbnew` `ProjectTemplate` suite. PT-MF2-01,
  PT-MF1-01 through PT-MF1-04 and the common SF1 tests remain unchanged. All runtime checks
  remain **not run / pending**, with **unmeasured (null)** durations and no timer source;
  no matching binary was built because compilation is prohibited. Neither the old failing
  title-block fixture nor the new passing behavior has been executed.
- PT-MF2-R7-STATIC-01: `git --no-optional-locks diff --check`, exit 0, **63.1790 ms**,
  measured with `System.Diagnostics.Stopwatch` (monotonic); whitespace only.
- PT-MF2-R7-STATIC-02: strict UTF-8 decoding, no BOM, CRLF and original EOF-convention checks
  for all five changed files, passed, **15.8181 ms**, same timer source. The missing final
  newline in `pcbnew/import_design_rules.cpp` is retained.
- PT-MF2-R7-STATIC-03: compare the five existing PCB test-case bodies with
  `git show HEAD:qa/tests/pcbnew/test_project_template.cpp`; check
  `git --no-optional-locks diff --exit-code HEAD -- qa/tests/common/test_project_template.cpp
  qa/tests/common/CMakeLists.txt qa/tests/pcbnew/CMakeLists.txt`. All unchanged,
  **164.0370 ms**, same timer source. This checks preservation/registration, not runtime behavior.
- PT-MF2-R7-STATIC-04: manual declaration/call-site inspection against
  `DRC_ENGINE::loadRules`, native companion selection, passive read-only source-project loading,
  PCB loading/attachment, cross-reference conversion, board resolution, both parser passes,
  original output text and error propagation. Duration unmeasured. The official QA fixture
  changes only the native serialization's header; it does not invoke the forbidden official
  output writer. No production export policy is changed.
- Final whitespace/encoding checks are repeated after this record is added and before the
  local commit; repeat durations are unmeasured. Shared preparation/wait/retry/total elapsed
  time was not measured. No compilation, C++ QA, GUI/reopen acceptance, push or merge was
  performed; no files were deleted and no other worktree was modified.
