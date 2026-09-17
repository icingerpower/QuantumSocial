# Repository Guidelines

## Project Structure & Module Organization

QuantumSocial is a C++20/Qt Widgets desktop application for social accounts, statistics, and media generation. `main.cpp` initializes the application and working-directory setup.

- `gui/` contains windows, dialogs, and Qt Designer `.ui` files; `gui/panes/` contains feature panes.
- `model/` contains persistence and Qt models; `accounts/`, `properties/`, `imagegen/`, `videogen/`, and `videos/` separate domain features. Python browser workers live alongside their C++ integrations.
- Module `.cmake` files enumerate sources; register new files in the appropriate list.
- Runtime assets live in the selected working directory, including `reference_images/` and `projects/<id>/generations/`. Keep generated media outside the repository.

## Build, Test, and Development Commands

Install a C++20 compiler, CMake, Qt Widgets/Multimedia/MultimediaWidgets, and QCoro Core. Prefer Qt 6: the current QCoro helper explicitly searches for QCoro6. The sibling `../common/` checkout must provide `aicli`, `gui/progress`, and `workingdirectory`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
cmake --build build --parallel
./build/QuantumSocial
```

These commands configure, compile, and launch a Linux development build. Python workers require `python3` on `PATH`; install browser dependencies in your active virtual environment:

```sh
python3 -m pip install playwright
python3 -m playwright install chromium
```

## Coding Style & Naming Conventions

Match surrounding code: four-space indentation, C++ braces on separate lines, and paired `PascalCase.h`/`.cpp` files. Use camelCase methods, `m_` member prefixes, and existing feature prefixes such as `Pane`, `Dialog`, and `Table`. Python uses snake_case functions. No formatter or linter configuration is checked in. Preserve workers' JSON stdout contract; send diagnostics to stderr.

## Testing Guidelines

No automated test framework, coverage threshold, or test naming convention is configured; `.gitignore` currently ignores `tests`. Build changes and manually exercise affected panes, save/reload behavior, and worker success/error paths. For browser changes, verify authentication, timeout, and download recovery as applicable. Record reproduction steps and results in the PR.

## Commit & Pull Request Guidelines

History uses short, descriptive subjects such as “Add pane-based UI” and “Fix worker browser relaunch”; follow that style and keep commits focused. PRs should explain behavior changes, link relevant issues, list validation performed, and include screenshots for UI changes.

## Security & Configuration

Keep browser profiles, session cookies, credentials, and generated output out of commits. Preserve portable working-directory paths and compatibility with existing saved project data.
