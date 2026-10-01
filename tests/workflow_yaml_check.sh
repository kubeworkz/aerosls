#!/usr/bin/env bash
# workflow_yaml_check.sh — every .github/workflows/*.yml must load.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# A one-character YAML mistake in ci.yml turned CI off for a whole branch
# without turning it red. Line 344 read
#
#       - name: aerobackup host test (E7 census target: retention prune)
#
# and the `: ` inside that plain scalar made the value a mapping key, so the
# file was not YAML: "mapping values are not allowed here". GitHub parses a
# workflow before it schedules anything, so the run failed at LOAD time with
# ZERO jobs (run 36510485789, `completed failure ... 0s`). The
# `on: push: branches: [main]` filter could not be evaluated either, so the
# push still created a run that looked like CI had run — and PR #55 showed only
# GitGuardian. The branch had never executed a single CI job.
#
# Nothing in the repo could have caught it: every guard reads C, shell, or a
# checked-in artefact, and not one of them ever asked whether the workflow
# files — the thing that decides whether ANY of them run — are well-formed.
# This guard asks that question.
#
# ─── What it asserts ──────────────────────────────────────────────────────
# For every file in .github/workflows (a directory argument overrides it, which
# is how the smoke plants its teeth):
#
#   1. IT PARSES. The file must load with a real YAML parser. GitHub's own
#      failure for the bug above is a load-time parse error, so this is the one
#      property that maps exactly onto it, message and all — PyYAML reproduces
#      "mapping values are not allowed here ... line 7, column 53".
#
#   2. IT LOADS A JOB GRAPH. Parseability alone would miss the other zero-jobs
#      load failures, so the document must also be a mapping with an `on:`
#      trigger and a non-empty `jobs:` mapping, and every job must carry
#      `runs-on` or `uses` — each of which GitHub rejects at load with the same
#      symptom: a workflow that runs nothing.
#
#   3. NO KEY IS REPEATED IN A MAPPING. YAML forbids two equal keys in one
#      mapping, and GitHub's workflow syntax does not allow it either. PyYAML,
#      left alone, keeps the LAST value and says nothing — a file that loads
#      while meaning something other than what it says. See the note below.
#
# It is deliberately NOT a re-implementation of GitHub's schema. It does not
# know action inputs or expression contexts; it knows only what makes a
# workflow UNLOADABLE, which is the class of failure that costs a whole
# branch's CI and leaves a green-looking PR behind.
#
# ─── Duplicate keys, and what GitHub does about them ──────────────────────
# This is the one check here that PyYAML does not perform for us. Given
#
#       name: first
#       name: second
#
# yaml.safe_load() returns {'name': 'second'} and reports nothing, so the file
# loads and does something other than what it says. GitHub's workflow syntax is
# not permissive there: the YAML spec allows a key once per mapping, and a
# repeated key is rejected at load — the same load-time, zero-jobs failure mode
# as the ci.yml bug this guard exists for. The corroborating instrument is
# actionlint (github.com/rhysd/actionlint), which is built to track GitHub's
# parser, is deliberately conservative about false positives, classifies a
# duplicate key as a syntax error, and documents that such syntax errors make
# the workflow run fail. So the loader below is a SafeLoader that raises on a
# repeated key instead of quietly keeping the last one. Merge keys are exempt:
# `<<: *anchor` followed by an overriding key is not a duplicate.
#
# ─── Why python3 + PyYAML ─────────────────────────────────────────────────
# There is no YAML parser in the shell and none elsewhere in the tree; PyYAML
# is the one that is present where this runs (python3 is already a hard
# dependency of CI and of a dozen guards). A missing python3 or PyYAML is an
# ABORT — and because this guard carries no GUARD-KIND marker, run_checks.sh
# reports that as a FAILURE rather than an owed skip. A guard that cannot
# check must never be allowed to look green.
#
# Exit: 0 every workflow loads, 1 at least one does not load or repeats a key,
#       2 no parser / no workflow files.
set -u
cd "$(dirname "$0")/.." || exit 1   # repo root, so the default dir resolves

# The directory is overridable so the smoke can plant teeth in a temp dir
# without dirtying the real workflows. run_checks.sh and deploy.sh call this
# with no argument, so they always inspect the committed ones.
WORKFLOWS_DIR="${1:-.github/workflows}"

# ─── A parser, or an honest abort ─────────────────────────────────────────
if ! command -v python3 >/dev/null 2>&1; then
    echo "ABORT: python3 is not on PATH — there is no YAML parser to check workflows with." >&2
    exit 2
fi
if ! python3 -c 'import yaml' >/dev/null 2>&1; then
    echo "ABORT: python3 cannot import yaml (no PyYAML) — cannot parse the workflows." >&2
    exit 2
fi

[ -d "$WORKFLOWS_DIR" ] || {
    echo "ABORT: $WORKFLOWS_DIR is not a directory." >&2
    exit 2
}

# ─── Discovery: .yml and .yaml, the only two extensions GitHub loads ──────
shopt -s nullglob
workflows=("$WORKFLOWS_DIR"/*.yml "$WORKFLOWS_DIR"/*.yaml)
shopt -u nullglob

if [ "${#workflows[@]}" -eq 0 ]; then
    # A guard that inspects nothing protects nothing — the same rule
    # stack_frame_budget_check.sh applies when it scans zero sources. It is also
    # exactly the vacuity this guard exists to prevent one level up: no workflow
    # files found must not read as "all workflow files are fine".
    echo "ABORT: no $WORKFLOWS_DIR/*.yml found — nothing to check." >&2
    exit 2
fi

fails=0
for f in "${workflows[@]}"; do
    out="$(python3 - "$f" <<'PY' 2>&1
import sys
import yaml

path = sys.argv[1]

# PyYAML implements YAML 1.1, where the bare keys `on`/`off`/`yes`/`no` are
# booleans — so a workflow's `on:` trigger arrives as the key True. Normalising
# keys so the structural checks can name them the way the file does.
def norm(key):
    if key is True:
        return "on"
    if key is False:
        return "off"
    return str(key)

def fail(msg):
    print("FAIL  %s: %s" % (path, msg))
    sys.exit(1)

class DuplicateKey(Exception):
    """A mapping defines the same key more than once."""

class WorkflowLoader(yaml.SafeLoader):
    """SafeLoader that refuses a repeated key instead of silently keeping last.

    PyYAML — like most parsers, and unlike the YAML spec — keeps the last value
    when a mapping repeats a key and says nothing. GitHub's workflow syntax
    allows a key only once per mapping, so a repeated key is a load-time error
    (see the header). Merge keys are exempted, because `<<: *anchor` followed by
    an overriding key is not a duplicate.
    """

    def __init__(self, stream):
        super().__init__(stream)
        # MappingNodes already scanned, kept as objects (not ids) so a node
        # reused through an anchor is not re-scanned after flattening, and so
        # this set also holds them alive for the duration of the load.
        self._dup_checked = set()

    def construct_mapping(self, node, deep=False):
        if node not in self._dup_checked:
            self._dup_checked.add(node)
            self._reject_duplicates(node, deep)
        return super().construct_mapping(node, deep=deep)

    def _reject_duplicates(self, node, deep):
        seen = {}
        for key_node, _ in node.value:
            # Merge keys may legitimately repeat, and an explicit key that
            # overrides a merged one is not a duplicate either. Both are handled
            # by PyYAML's flatten_mapping — which has NOT run yet, so the RAW
            # key list is what is inspected here.
            if key_node.tag == "tag:yaml.org,2002:merge" or key_node.value == "<<":
                continue
            key = self.construct_object(key_node, deep=deep)
            try:
                hash(key)
            except TypeError:
                continue                       # an unhashable key cannot collide
            display = key_node.value if isinstance(key_node, yaml.ScalarNode) else repr(key)
            if key in seen:
                first_mark = seen[key]
                raise DuplicateKey(
                    "duplicate mapping key %r at line %d, column %d — already "
                    "defined at line %d, column %d; GitHub loads a workflow "
                    "with one key per mapping, so this file would not load, and "
                    "a parser that allows it would silently keep one of the two "
                    "values"
                    % (display,
                       key_node.start_mark.line + 1, key_node.start_mark.column + 1,
                       first_mark.line + 1, first_mark.column + 1))
            seen[key] = key_node.start_mark

try:
    with open(path, "rb") as fh:
        doc = yaml.load(fh, Loader=WorkflowLoader)
except DuplicateKey as e:
    # Not a PyYAML error: PyYAML would have accepted the file by keeping the
    # last value, so the message is ours and names both occurrences.
    fail(str(e))
except yaml.YAMLError as e:
    # PyYAML's message already carries the line and column of the fault, which
    # is the whole point: the failure has to say WHERE the file stopped being a
    # workflow, or the next reader is back to bisecting by hand. Collapse the
    # multi-line render to one line so the runner's indenting keeps it aligned.
    fail("does not parse as YAML — %s" % " ".join(str(e).split()))
except OSError as e:
    fail("cannot be read — %s" % e)

if doc is None:
    fail("is empty — an empty workflow loads as zero jobs")

if not isinstance(doc, dict):
    fail("top level is %s, not a mapping" % type(doc).__name__)

top = {norm(k): v for k, v in doc.items()}

if "on" not in top:
    fail("has no `on:` trigger — GitHub refuses to load a workflow without one")

jobs = top.get("jobs")
if not isinstance(jobs, dict) or not jobs:
    fail("has no non-empty `jobs:` mapping — this is the zero-jobs load failure")

for jid, job in jobs.items():
    if not isinstance(job, dict):
        fail("job `%s` is %s, not a mapping" % (jid, type(job).__name__))
    keys = {norm(k) for k in job}
    if "runs-on" not in keys and "uses" not in keys:
        fail("job `%s` has neither `runs-on` nor `uses` — GitHub cannot schedule it" % jid)

print("parses; %d job(s): %s" % (len(jobs), ", ".join(str(j) for j in jobs)))
PY
)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "ok    $f — $out"
    else
        printf '%s\n' "$out" | sed 's/^/      /'
        fails=$((fails + 1))
    fi
done

echo
if [ "$fails" -eq 0 ]; then
    echo "ok:   all ${#workflows[@]} workflow file(s) in $WORKFLOWS_DIR parse, repeat no key, and carry a loadable job graph"
    exit 0
fi
echo "FAIL: $fails of ${#workflows[@]} workflow file(s) in $WORKFLOWS_DIR would not load —"
echo "      a workflow that does not load runs NO jobs, and CI can stay green anyway."
exit 1
