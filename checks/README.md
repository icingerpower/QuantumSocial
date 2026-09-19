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

The browser worker has separate local Playwright checks:

```sh
python3 -m unittest discover -s model/videogen -p 'test_gemini*.py'
```

Set `QS_TEST_CHROME=1` when running `test_gemini_downloads.py` to verify the
video-blob download workaround in visible system Chrome. The test pages are
local fixtures and require no Gemini account or generation quota.
