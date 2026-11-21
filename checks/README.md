# Generation checks

These offline integration checks use a temporary working directory and a fake
video backend. They exercise generation counts, saved configurations, repeated
videos, independent output records, cancellation, and failure handling. They do
not launch an AI CLI or connect to Gemini.

Configure with Qt 6 and QCoro on the CMake search path:

```sh
cmake -S checks -B build-checks -DCMAKE_PREFIX_PATH="/path/to/Qt;/path/to/QCoro"
cmake --build build-checks --parallel 4
ctest --test-dir build-checks --output-on-failure
```

Research checks run in the same executable. They cover legacy account IDs,
read-only A+ catalog import with high-resolution `sizing/` photos, variant grouping
and archived/non-A+ product exclusion, account restrictions, reference deduplication,
review history, copied media, stale-write protection, project provenance, and the
visual card approval action.

To run only this slice, optionally reading a real AmazonTemplate3 catalog without
changing it and saving a UI screenshot outside the repository:

```sh
QT_QPA_PLATFORM=offscreen QS_RESEARCH_ONLY=1 \
  QS_TEST_AMAZON_SOURCE=/path/to/amazonTemplate3 \
  QS_RESEARCH_SCREENSHOT=/tmp/quantumsocial-research.png \
  ./build-checks/generation_checks
```

Manual workflow: open Research → Products, select an AmazonTemplate3 working
directory and import it. Choose a social account and configure its allowed
brands/categories with Account rules. Switch to References, collect posts via
Scroll & save… or Add…, and use the 1-click view pills (`Pending`, `Approved`,
`Rejected`, `All`) to filter the queue. Click anywhere on a card to preview its
media and adaptation notes on the right. Approving or editing notes keeps the
reference approved, and any reference can be reused across multiple products:
choose any eligible product in the `Product:` dropdown above `Create generation
project` and click `Create generation project` (clicking it on a pending card
auto-approves the card in one step). Only replacing the underlying media file
returns a reference to pending review. CLI assessments are advisory and do not
replace human approval.

The application also exposes headless research commands:

```sh
./build/QuantumSocial research --help
./build/QuantumSocial research accounts --workspace /path/to/QuantumSocial
./build/QuantumSocial research import-products --workspace /path/to/QuantumSocial --source /path/to/amazonTemplate3
./build/QuantumSocial research products --workspace /path/to/QuantumSocial --account ACCOUNT_ID
```

Commands return JSON and a nonzero exit status on failure. Account rules are
configured in the GUI; products lists only eligible products. References,
add-reference, link-product, review, and create-project are also available through `--help`.
The legacy projects/accounts stores are not a multi-process job service: avoid
editing those stores concurrently through the GUI and CLI. Research detects
stale writers and asks for a workspace reload instead of losing changes.
Browser collection, automatic selection, and TypeScript strategy loading are
subsequent milestones; this slice provides manual URL/media intake and registered
C++ sources/assessors.

The browser worker has separate local Playwright checks:

```sh
python3 -m unittest discover -s model/videogen -p 'test_gemini*.py'
```

Set `QS_TEST_CHROME=1` when running `test_gemini_downloads.py` to verify the
video-blob download workaround in visible system Chrome. The test pages are
local fixtures and require no Gemini account or generation quota.
