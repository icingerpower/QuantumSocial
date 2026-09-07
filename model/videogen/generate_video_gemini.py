#!/usr/bin/env python3
"""Generates one video with Gemini (Veo) driven through a browser.

Browser strategy (Google refuses logins from Playwright's bundled Chromium
as "not secure", so the REAL Google Chrome is used):
1. system Chrome with the user's DEFAULT profile (~/.config/google-chrome):
   the existing Google session is reused, no login at all — requires Chrome
   to be closed (the profile is locked while it runs);
2. fallback: system Chrome with a dedicated persistent profile at
   ~/.cache/quantumsocial/gemini-profile — one interactive login (up to 5
   minutes, the window is visible), remembered afterwards;
3. last resort: Playwright's Chromium with the dedicated profile.
Automation banners/flags are disabled so the login is not rejected.

Before generating (unless requireUltra is off), the script checks that the
selected Google account has the AI Ultra plan (needed for Veo): if not, it
opens the account switcher, tries the other signed-in accounts until one
shows Ultra, and caches that account's email in
~/.cache/quantumsocial/gemini_ultra_account.json so the next runs switch to
it directly.

Flow: open gemini.google.com/app, ensure Ultra, select the Video/Veo tool,
optionally attach the input image(s), paste the prompt, submit, wait for the
generated video, download it into the output directory. If Gemini refuses
the prompt (content policy), the refusal is reported with "rejected": true
so the caller retries with a modified prompt instead of treating it as a
technical failure.

CAVEAT: Gemini's web UI changes frequently — buttons, refusal texts, the
account switcher and the Ultra badge are matched by accessible names / page
text in English and French, best effort. If a step cannot be completed, a
clear error is reported so the selectors can be adjusted rather than
failing silently.

Note on useSystemChromeProfile=true: this drives your REAL, everyday Chrome
profile — if you keep your normal Chrome open at the same time (against the
"Chrome must be closed" requirement above), Chrome's single-instance-per-
profile behavior can make this automation SHARE your actual browsing window
instead of a separate one, so your own clicks/tab-closing can knock out the
page it is driving (and vice versa). If that keeps happening, either close
Chrome before generating, or turn useSystemChromeProfile off to use the
fully separate dedicated profile instead (one one-time login, never shared).

A page/tab that dies mid-run (a crash, or exactly the shared-window
situation above) is recovered by opening a fresh page IN THE SAME BROWSER
(_recover_page()) rather than ending the process — ending it is what forces
the caller (VideoGenerationWorkflow) to launch an entirely new, visible
browser window for the next try, which is far more disruptive. Only when
the browser/context itself is gone does the process still exit.

Prints a single JSON line to stdout:
    {"video": <absolute path|null>, "error": <string|null>, "rejected": <bool>}

Usage: generate_video_gemini.py <prompt-file> <output-dir> <settings-json> [image-path...]
Settings used: generationTimeoutSec (default 900),
useSystemChromeProfile (default true), requireUltra (default true).
"""
import json
import os
import re
import sys
import time

PROFILE_DIR = os.path.expanduser("~/.cache/quantumsocial/gemini-profile")
SYSTEM_CHROME_PROFILE = os.path.expanduser("~/.config/google-chrome")
ULTRA_CACHE = os.path.expanduser("~/.cache/quantumsocial/gemini_ultra_account.json")
GEMINI_URL = "https://gemini.google.com/app"

LOGGED_OUT_MARKERS = ("Sign in", "Connexion", "Se connecter")
LOGIN_WAIT_TIMEOUT_S = 300
LOGIN_POLL_INTERVAL_S = 2

VIDEO_TOOL_NAMES = ("Video", "Vidéo", "Veo")
DOWNLOAD_NAMES = ("Download", "Télécharger")

# Gemini's own "still working on it" indicator, shown while Veo is actively
# generating (observed live: still present after a full 5-minute stall
# timeout on a genuine, in-progress generation — the fixed per-attempt
# budget was cutting off real work, not silence). Its presence resets the
# stall clock instead of counting toward it; only the overall
# generationTimeoutSec ceiling still applies.
PROGRESS_MARKERS = ("Analyse…", "Analyse...", "Analysing…", "Analyzing…",
                    "Génération en cours", "Generating…", "Generating...")

# The prompt bar's "+" menu (observed live 2026-08: its accessible name
# contains "outils"/"tools"), and its entries for the Veo tool and file
# upload. Text-matched, since the entries' roles proved unreliable.
PLUS_MENU_NAMES = ("outils", "tools", "importation", "import", "Ajouter", "Add")
CREATE_VIDEO_TEXTS = ("Créer une vidéo", "Create a video", "Create videos",
                      "Create video")
UPLOAD_TEXTS = ("Importer des fichiers", "Upload files", "Upload file",
                "Add files")

# The model picker next to the prompt bar — root-caused live (screenshot
# dump on a "no_tool" failure): "Créer une vidéo" ("Create a video") can be
# present in the "+" menu but visibly GREYED OUT/disabled while a
# lightweight model like "Flash-Lite" is selected, which does not support
# Veo. Matched by accessible name — best effort, exact model names/labels
# change over time (same caveat as the rest of this script).
LITE_MODEL_MARKERS = ("Flash-Lite", "Lite")
PREFERRED_MODEL_MARKERS = ("Pro", "Advanced")

# Content-policy refusal texts (English / French). Checked in the answer
# area after submitting: their presence means "rejected", not "failed".
REJECTION_MARKERS = (
    "I can't create", "I cannot create", "I can't generate", "I cannot generate",
    "I can't make", "I cannot make", "videos of real people",
    "can't help with", "cannot help with", "unable to create", "not able to create",
    "violates", "against our policy",
    "Je ne peux pas créer", "Je ne peux pas générer", "je ne peux pas vous aider",
    "Je ne peux pas faire", "personnes réelles",
    "contraire à", "nos règles",
)

EMAIL_RE = re.compile(r"[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}")

# Root-caused live: "Créer une vidéo" can be greyed out/unclickable not
# because the UI changed at all, but because the account's Veo QUOTA is
# exhausted for the moment — Gemini says so plainly in the page (French UI
# observed: "Vous n'avez plus de vidéos pour le moment. Les vidéos seront de
# nouveau disponibles le <date> à <heure>."). Distinguishing this matters:
# no amount of prompt rewriting, model switching or retrying can work
# around a rate limit — only waiting until the reset time can. Matched
# before ever attempting the video tool, so the reported error is accurate
# instead of the misleading generic "web UI may have changed" one.
VIDEO_QUOTA_MARKERS = (
    "n'avez plus de vidéos",
    "You don't have any more videos",
    "You have no more videos",
    "out of videos for now",
    "no more videos for now",
)

# Every run traces its steps into /tmp so automation bugs can be debugged
# after the fact (the AI assistant reads these): one log file per run, plus
# a screenshot + page text dump on failure.
LOG_DIR = "/tmp/quantumsocial"
LOG_PATH = os.path.join(
    LOG_DIR, time.strftime("gemini_%Y%m%d_%H%M%S.log"))


def _log(message):
    try:
        os.makedirs(LOG_DIR, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as handle:
            handle.write(time.strftime("%H:%M:%S ") + message + "\n")
    except OSError:
        pass


def _dump_state(page, tag):
    """Best-effort forensic dump next to the log: screenshot + page text."""
    base = LOG_PATH[:-4] + "_" + tag
    try:
        _log(f"dump[{tag}]: url={page.url}")
    except Exception:
        pass
    try:
        page.screenshot(path=base + ".png")
        _log(f"dump[{tag}]: screenshot {base}.png")
    except Exception as exc:
        _log(f"dump[{tag}]: screenshot failed: {exc}")
    try:
        with open(base + ".txt", "w", encoding="utf-8") as handle:
            handle.write(page.locator("body").inner_text()[:20000])
        _log(f"dump[{tag}]: page text {base}.txt")
    except Exception as exc:
        _log(f"dump[{tag}]: page text failed: {exc}")


def _emit(video=None, error=None, rejected=False):
    if error:
        _log("EMIT error: " + error)
        error = f"{error} [debug log: {LOG_PATH}]"
    else:
        _log("EMIT video: " + str(video))
    print(json.dumps({"video": video, "error": error, "rejected": rejected}))


def _is_logged_out(text):
    return any(marker in text for marker in LOGGED_OUT_MARKERS)


def _login_in_progress(page):
    """True while the user is anywhere in Google's sign-in flow. The URL
    check matters: the password/challenge pages ("Hi <name>") do NOT contain
    any signed-out marker text, and navigating away from them would wipe the
    form while the user is typing."""
    try:
        url = page.url or ""
    except Exception:
        return True
    if "accounts.google.com" in url:
        return True
    try:
        text = page.locator("body").inner_text()
    except Exception:
        return True
    return _is_logged_out(text)


def _gemini_ready(page):
    """The prompt editor is visible — Gemini is usable."""
    try:
        return page.get_by_role("textbox").first.is_visible()
    except Exception:
        return False


def _wait_ready_or_login(page, timeout_s=25):
    """Polls fast (0.5 s) instead of sleeping a fixed delay: returns "ready"
    as soon as the prompt editor shows up, "login" as soon as a sign-in flow
    is detected, "timeout" otherwise."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if _login_in_progress(page):
            return "login"
        if _gemini_ready(page):
            return "ready"
        time.sleep(0.5)
    return "timeout"


def _open_plus_menu(page):
    """Opens the prompt bar's "+" menu; True when a candidate was clicked."""
    for name in PLUS_MENU_NAMES:
        try:
            page.get_by_role("button", name=name, exact=False).first.click(timeout=800)
            page.wait_for_timeout(800)
            return True
        except Exception:
            continue
    return False


def _switch_to_video_capable_model(page):
    """Best effort: if the current model looks like a lightweight variant
    that disables Veo (observed live: "Créer une vidéo" present but greyed
    out while "Flash-Lite" was selected), open the model picker and try an
    option that looks more capable. Returns True if a switch was attempted
    (does not guarantee it landed on a video-capable model — the caller
    still has to retry the tool click and can fail again)."""
    button = None
    for marker in LITE_MODEL_MARKERS:
        try:
            candidate = page.get_by_role("button", name=marker, exact=False).first
            if candidate.is_visible():
                button = candidate
                break
        except Exception:
            continue
    if button is None:
        return False
    try:
        current = button.inner_text()
        button.click(timeout=1500)
        page.wait_for_timeout(500)
    except Exception:
        return False
    for marker in PREFERRED_MODEL_MARKERS:
        # Role-scoped locators first — a long conversation history can
        # contain unrelated "Pro"/"Advanced" text elsewhere on the page, and
        # a plain get_by_text(...).first could click one of those instead of
        # the just-opened dropdown's actual option.
        for locator in (
            page.get_by_role("menuitem", name=marker, exact=False),
            page.get_by_role("option", name=marker, exact=False),
            page.get_by_text(marker, exact=False),
        ):
            try:
                locator.first.click(timeout=1200)
                _log(f"model: switched from '{current}' to an option matching '{marker}'")
                page.wait_for_timeout(800)
                return True
            except Exception:
                continue
    _log(f"model: opened the picker (was '{current}') but no preferred "
         "option matched — closing without switching")
    try:
        page.keyboard.press("Escape")
    except Exception:
        pass
    return False


def _select_video_tool(page):
    """Activates the Veo tool: the "Créer une vidéo" entry of the "+" menu
    (possibly already open from a previous click), else legacy chips, else
    (last resort) switching away from a lightweight model that disables it
    and trying once more."""
    for attempt in range(2):
        for text in CREATE_VIDEO_TEXTS:
            try:
                page.get_by_text(text, exact=False).first.click(timeout=700)
                page.wait_for_timeout(800)
                _log(f"tool: clicked '{text}' (attempt {attempt + 1})")
                return True
            except Exception:
                continue
        if attempt == 0 and not _open_plus_menu(page):
            break
    # Legacy direct chips (older / English UI). Last: generic names could
    # also match the sidebar's video library.
    for name in VIDEO_TOOL_NAMES:
        try:
            page.get_by_role("button", name=name, exact=False).first.click(timeout=800)
            page.wait_for_timeout(800)
            _log(f"tool: clicked legacy button '{name}'")
            return True
        except Exception:
            continue
    if _switch_to_video_capable_model(page):
        _open_plus_menu(page)
        for text in CREATE_VIDEO_TEXTS:
            try:
                page.get_by_text(text, exact=False).first.click(timeout=1200)
                page.wait_for_timeout(800)
                _log(f"tool: clicked '{text}' after switching model")
                return True
            except Exception:
                continue
    return False


def _attach_image(page, image):
    """Uploads one input image through the "+" menu's upload entry; the menu
    usually closed when the video tool was selected, so reopen it if needed.
    Best-effort: Veo also works text-only."""
    for attempt in range(2):
        for text in UPLOAD_TEXTS:
            try:
                with page.expect_file_chooser(timeout=1500) as chooser_info:
                    page.get_by_text(text, exact=False).first.click(timeout=800)
                chooser_info.value.set_files(image)
                page.wait_for_timeout(2000)
                _log(f"attach: uploaded {image} via '{text}'")
                return True
            except Exception:
                continue
        if attempt == 0 and not _open_plus_menu(page):
            break
    _log(f"attach: no upload entry found for {image} — continuing text-only")
    return False


def _select_aspect(page, prompt):
    """Best-effort: align the Veo page's format selector (e.g. the button
    showing "Paysage (16:9)") with the format the prompt asks for."""
    want_portrait = "9:16" in prompt
    want_landscape = "16:9" in prompt and not want_portrait
    if not (want_portrait or want_landscape):
        return
    opened = False
    for name in ("Paysage", "Portrait", "Landscape", "16:9", "9:16"):
        try:
            page.get_by_role("button", name=name, exact=False).first.click(timeout=800)
            opened = True
            break
        except Exception:
            continue
    if not opened:
        _log("aspect: format selector not found")
        return
    page.wait_for_timeout(600)
    targets = ("Portrait", "9:16") if want_portrait else ("Paysage", "Landscape", "16:9")
    for text in targets:
        try:
            page.get_by_text(text, exact=False).last.click(timeout=800)
            _log(f"aspect: selected '{text}'")
            page.wait_for_timeout(500)
            return
        except Exception:
            continue
    _log("aspect: wanted option not found")


def _submit_prompt(page, editor, prompt):
    """Submits via the send button (Enter alone proved unreliable in the Veo
    editor), verifies the editor emptied, falls back to Enter."""
    submitted_via = None
    for name in ("Envoyer", "Send", "Submit"):
        try:
            page.get_by_role("button", name=name, exact=False).first.click(timeout=1200)
            submitted_via = f"button '{name}'"
            break
        except Exception:
            continue
    if submitted_via is None:
        page.keyboard.press("Enter")
        submitted_via = "Enter"
    page.wait_for_timeout(2000)
    try:
        still = editor.inner_text().strip()
        if still and prompt[:30] in still:
            _log(f"submit via {submitted_via}: editor still filled — pressing Enter")
            editor.click(timeout=2000)
            page.keyboard.press("Enter")
            page.wait_for_timeout(2000)
        else:
            _log(f"submitted via {submitted_via}")
    except Exception:
        _log(f"submitted via {submitted_via} (editor state unknown)")


def _is_target_closed_error(exc):
    """True for Playwright's "Target page, context or browser has been
    closed" family of errors — a live page/tab crash or an external close,
    as opposed to an ordinary selector/timeout failure. Distinguishing this
    matters: it is recoverable WITHIN THE SAME BROWSER (open a fresh page,
    same persistent context) instead of forcing the whole process to exit,
    which is what used to happen — and every exit means the CALLER
    (VideoGenerationWorkflow's retry loop) launches an entirely new browser
    window for the next try, far more disruptive than it needs to be."""
    text = str(exc)
    return "has been closed" in text or "Target closed" in text or "Target page" in text


def _recover_page(context):
    """Opens a fresh page in the SAME (already-launched) browser context
    after the previous page died mid-run. Returns the new page in the
    "ready" state, or None when the browser/context itself is gone too (the
    caller must then let the process exit — there is nothing left to reuse)."""
    try:
        page = context.new_page()
        page.goto(GEMINI_URL, wait_until="domcontentloaded", timeout=60000)
        if _wait_ready_or_login(page) == "ready":
            _log("recover: opened a fresh page in the same browser")
            return page
        _log("recover: fresh page did not become ready")
    except Exception as exc:  # noqa: BLE001 - browser is gone, nothing to salvage
        _log(f"recover: failed, the browser is gone too: {exc}")
    return None


def _rejection_in(text):
    for marker in REJECTION_MARKERS:
        if marker.lower() in text.lower():
            return marker
    return None


def _video_quota_message(page):
    """Returns the quota-exhaustion notice (first line, with the reset time
    if shown) when Gemini's own "no more videos right now" wall is up, else
    None. See VIDEO_QUOTA_MARKERS."""
    try:
        text = page.locator("body").inner_text()
    except Exception:
        return None
    lowered = text.lower()
    for marker in VIDEO_QUOTA_MARKERS:
        idx = lowered.find(marker.lower())
        if idx >= 0:
            return text[idx:idx + 200].split("\n")[0].strip()
    return None


def _prompt_variant(prompt, attempt):
    """The human technique that unblocks most refusals: tiny meaningless
    edits — add/remove the final dot, then small tasteful suffixes."""
    base = prompt.strip()
    if attempt == 0:
        return base
    toggled = base[:-1].strip() if base.endswith(".") else base + "."
    if attempt == 1:
        return toggled
    suffixes = (" Elegant and tasteful.", " Classy commercial style.",
                " Refined product aesthetic.")
    return toggled + suffixes[(attempt - 2) % len(suffixes)]


def _run_attempt(page, prompt, image_paths, deadline, attempt_deadline, stall_timeout_s):
    """One full generation attempt in the current chat. Returns
    ("video", download) / ("rejected", marker) / ("stalled", note) /
    ("error", message). "stalled" = no reply at all before the attempt
    timeout — observed on forbidden prompts, where Gemini sometimes just
    never answers instead of refusing in text. A visible PROGRESS_MARKERS
    hit (Gemini's own "still working" indicator) resets attempt_deadline
    instead of counting toward it, since that is proof of real activity,
    not silence — only the overall `deadline` still bounds a run that keeps
    showing progress forever."""
    quota_message = _video_quota_message(page)
    if quota_message:
        _dump_state(page, "quota")
        return ("error", f"Gemini's video generation quota is exhausted for "
                         f"now: {quota_message}")
    if not _select_video_tool(page):
        _dump_state(page, "no_tool")
        return ("error", "Could not find Gemini's video (Veo) tool button — "
                         "the web UI may have changed; update the selectors in "
                         "generate_video_gemini.py.")
    for image in image_paths:
        _attach_image(page, image)

    # Snapshot the page text BEFORE submitting so refusal markers are only
    # matched in what appears after our prompt.
    before_text = page.locator("body").inner_text()

    _select_aspect(page, prompt)
    editor = page.get_by_role("textbox").first
    editor.click(timeout=10000)
    editor.fill(prompt)
    _submit_prompt(page, editor, prompt)
    _log("waiting for the video or a refusal")

    while time.time() < min(deadline, attempt_deadline):
        try:
            after_text = page.locator("body").inner_text()
        except Exception:
            after_text = ""
        new_text = after_text.replace(before_text, "")
        marker = _rejection_in(new_text)
        if marker:
            return ("rejected", marker)

        for name in DOWNLOAD_NAMES:
            try:
                with page.expect_download(timeout=4000) as download_info:
                    page.get_by_role("button", name=name, exact=False)\
                        .last.click(timeout=2000)
                return ("video", download_info.value)
            except Exception:
                continue

        if any(marker in new_text for marker in PROGRESS_MARKERS):
            attempt_deadline = max(attempt_deadline, time.time() + stall_timeout_s)
        time.sleep(5)

    _dump_state(page, "stalled")
    return ("stalled", "no reply before the attempt timeout")


# ---------------------------------------------------------------------------
# Browser launch
# ---------------------------------------------------------------------------

def _launch_context(playwright, use_system_profile):
    """Tries system Chrome + default profile, then Chrome + dedicated
    profile, then bundled Chromium. Returns (context, note, error)."""
    base_kwargs = {
        "headless": False,
        "viewport": {"width": 1400, "height": 900},
        "accept_downloads": True,
        # Google's login rejects browsers that advertise automation.
        "ignore_default_args": ["--enable-automation"],
        "args": ["--disable-blink-features=AutomationControlled",
                 "--no-first-run", "--no-default-browser-check"],
    }
    attempts = []
    if use_system_profile and os.path.isdir(SYSTEM_CHROME_PROFILE):
        attempts.append((SYSTEM_CHROME_PROFILE, "chrome",
                         "system Chrome with the default profile"))
    attempts.append((PROFILE_DIR, "chrome", "system Chrome, dedicated profile"))
    attempts.append((PROFILE_DIR, None, "bundled Chromium, dedicated profile"))

    errors = []
    for user_dir, channel, note in attempts:
        kwargs = dict(base_kwargs)
        if channel:
            kwargs["channel"] = channel
        try:
            os.makedirs(user_dir, exist_ok=True)
            context = playwright.chromium.launch_persistent_context(user_dir, **kwargs)
            _log(f"launched: {note}")
            return context, note, None
        except Exception as exc:  # noqa: BLE001 - try the next launch flavor
            _log(f"launch failed: {note}: {exc}")
            errors.append(f"{note}: {exc}")
    return None, None, " | ".join(errors)


# ---------------------------------------------------------------------------
# Ultra account selection
# ---------------------------------------------------------------------------

def _load_cached_account():
    try:
        with open(ULTRA_CACHE, encoding="utf-8") as handle:
            return json.load(handle).get("email")
    except Exception:
        return None


def _save_cached_account(email):
    try:
        os.makedirs(os.path.dirname(ULTRA_CACHE), exist_ok=True)
        with open(ULTRA_CACHE, "w", encoding="utf-8") as handle:
            json.dump({"email": email}, handle)
    except OSError:
        pass


def _page_has_ultra(page):
    try:
        text = page.locator("body").inner_text()
    except Exception:
        return False
    return re.search(r"\bUltra\b", text) is not None


def _current_email(page):
    """Google apps expose the account in the avatar link's aria-label."""
    try:
        label = page.locator('a[aria-label*="@"]').first.get_attribute(
            "aria-label", timeout=3000)
        match = EMAIL_RE.search(label or "")
        return match.group(0) if match else None
    except Exception:
        return None


def _open_account_panel(page):
    page.locator('a[aria-label*="@"]').first.click(timeout=5000)
    page.wait_for_timeout(2500)


def _list_account_emails(page):
    """Opens the account switcher and scrapes the signed-in accounts."""
    emails = []
    try:
        _open_account_panel(page)
        texts = []
        for frame in page.frames:
            if "accounts.google.com" in (frame.url or ""):
                try:
                    texts.append(frame.locator("body").inner_text())
                except Exception:
                    pass
        try:
            texts.append(page.locator("body").inner_text())
        except Exception:
            pass
        for text in texts:
            for email in EMAIL_RE.findall(text):
                if email not in emails:
                    emails.append(email)
        page.keyboard.press("Escape")
        page.wait_for_timeout(500)
    except Exception:
        pass
    return emails


def _switch_account(page, email):
    """Clicks the given account in the switcher; True when it likely worked."""
    try:
        _open_account_panel(page)
        for frame in page.frames:
            if "accounts.google.com" in (frame.url or ""):
                try:
                    frame.get_by_text(email, exact=False).first.click(timeout=4000)
                    page.wait_for_timeout(6000)
                    return True
                except Exception:
                    continue
        page.get_by_text(email, exact=False).first.click(timeout=3000)
        page.wait_for_timeout(6000)
        return True
    except Exception:
        return False


def _ensure_ultra(page):
    """Makes sure the selected account has the AI Ultra plan; returns an
    error message, or None on success."""
    if _page_has_ultra(page):
        current = _current_email(page)
        _log(f"ultra: already on an Ultra account ({current or 'email unknown'})")
        if current:
            _save_cached_account(current)
        return None

    current = _current_email(page)
    ordered = []
    cached = _load_cached_account()
    if cached and cached != current:
        ordered.append(cached)
    for email in _list_account_emails(page):
        if email != current and email not in ordered:
            ordered.append(email)
    _log(f"ultra: current={current}, cached={cached}, candidates={ordered}")

    tried = []
    for email in ordered:
        tried.append(email)
        _log(f"ultra: switching to {email}")
        if not _switch_account(page, email):
            _log(f"ultra: switch to {email} failed")
            continue
        try:
            page.goto(GEMINI_URL, wait_until="domcontentloaded", timeout=60000)
            page.wait_for_timeout(4000)
        except Exception:
            continue
        if _page_has_ultra(page):
            _log(f"ultra: {email} has Ultra — cached")
            _save_cached_account(email)
            return None

    # The Ultra badge can render after the initial check (the no_ultra dumps
    # captured "Ultra" in the body a few seconds later); re-check before
    # concluding the plan is missing.
    for _ in range(5):
        if _page_has_ultra(page):
            current = _current_email(page) or current
            _log(f"ultra: badge appeared late — {current or 'email unknown'} "
                 "has Ultra")
            if current:
                _save_cached_account(current)
            return None
        page.wait_for_timeout(2000)

    _dump_state(page, "no_ultra")
    return ("No Google account with the AI Ultra plan was found "
            f"(current: {current or 'unknown'}; tried: {', '.join(tried) or 'none'}). "
            "Select the Ultra account manually once in the opened browser, "
            "then retry.")


# ---------------------------------------------------------------------------
# One generation, on an already-prepared page
# ---------------------------------------------------------------------------

def _prepare_page(context, require_ultra, existing_page=None):
    """Brings the browser to a usable, logged-in, Ultra-checked Gemini page
    on a FRESH chat. Reuses existing_page when given (worker mode: the same
    tab serves every request) — only the very first call pays the login/
    Ultra cost. Returns (page, error_message)."""
    page = existing_page
    if page is None:
        page = context.pages[0] if context.pages else context.new_page()
    page.goto(GEMINI_URL, wait_until="domcontentloaded", timeout=60000)
    state = _wait_ready_or_login(page)
    _log(f"page state: {state}")

    if state == "login":
        _log("waiting for the human to log in...")
        # Wait passively for the human to finish the whole sign-in flow
        # (email, password, 2FA...) — never navigate or reload while it
        # lasts, that would wipe the form being filled.
        deadline = time.time() + LOGIN_WAIT_TIMEOUT_S
        while time.time() < deadline and _login_in_progress(page):
            time.sleep(LOGIN_POLL_INTERVAL_S)
        if _login_in_progress(page):
            return page, "Timed out waiting for the Google login."
        # Login finished (possibly landed elsewhere): go to Gemini once,
        # now that it is safe.
        page.wait_for_timeout(2000)
        page.goto(GEMINI_URL, wait_until="domcontentloaded", timeout=60000)
        state = _wait_ready_or_login(page)
        _log(f"post-login state: {state}")
    if state != "ready":
        _dump_state(page, "not_ready")
        return page, "Gemini did not become ready (the prompt editor never appeared)."

    if require_ultra:
        ultra_error = _ensure_ultra(page)
        if ultra_error:
            return page, ultra_error
    return page, None


def _generate_once(context, page, prompt, image_paths, output_dir, settings):
    """Runs the full attempt loop for ONE request on the given page and
    returns (result_dict, page) — the page comes back because a mid-run
    crash can replace it (see _recover_page). result_dict is exactly the
    JSON contract: {"video", "error", "rejected"}."""
    generation_timeout_s = int(settings.get("generationTimeoutSec", 900))
    in_page_retries = int(settings.get("inPageRejectionRetries", 3))
    stall_timeout_s = int(settings.get("attemptStallTimeoutSec", 300))

    # Generation attempts: a refusal is retried IN THE SAME browser with a
    # fresh chat and a tiny prompt edit (the add/remove-a-dot technique) —
    # much faster than relaunching, and it unblocks most refusals. One
    # shared deadline covers all attempts (refusals arrive in seconds).
    deadline = time.time() + generation_timeout_s
    last_rejection = None
    download = None
    for attempt in range(in_page_retries + 1):
        variant = _prompt_variant(prompt, attempt)
        if attempt > 0:
            _log(f"in-page retry {attempt}/{in_page_retries} after "
                 f"rejection — fresh chat, edited prompt")
            try:
                page.goto(GEMINI_URL, wait_until="domcontentloaded", timeout=60000)
                ready = _wait_ready_or_login(page) == "ready"
            except Exception as exc:  # noqa: BLE001 - recovered below
                if not _is_target_closed_error(exc):
                    raise
                _log(f"in-page retry: page died mid-navigation ({exc}) — "
                     "recovering in the same browser instead of restarting...")
                page = _recover_page(context)
                ready = page is not None
            if not ready:
                last_rejection = last_rejection or "the browser tab closed unexpectedly"
                break
        attempt_deadline = time.time() + stall_timeout_s
        try:
            status, payload = _run_attempt(page, variant, image_paths,
                                           deadline, attempt_deadline,
                                           stall_timeout_s)
        except Exception as exc:  # noqa: BLE001 - recovered below
            # A page/tab crash (or an external close) mid-run used to end
            # the whole process — recovering a fresh page IN THIS SAME
            # BROWSER is far less disruptive.
            if not _is_target_closed_error(exc):
                raise
            _log(f"attempt {attempt + 1}: page/context died mid-run ({exc}) — "
                 "recovering in the same browser instead of restarting...")
            page = _recover_page(context)
            if page is None:
                return ({"video": None,
                         "error": f"the browser tab closed unexpectedly ({exc})",
                         "rejected": False}, page)
            last_rejection = f"the browser tab closed mid-attempt ({exc}) — retried"
            continue
        _log(f"attempt {attempt + 1}: {status}"
             + (f" ({payload})" if status != "video" else ""))
        if status == "video":
            download = payload
            break
        if status == "error":
            return ({"video": None, "error": payload, "rejected": False}, page)
        # "rejected" and "stalled" both retry with an edited prompt: a
        # silent stall is how Gemini sometimes handles a prompt it will
        # not fulfill.
        last_rejection = payload
        if time.time() >= deadline:
            break

    if download is None:
        return ({"video": None,
                 "error": f"Gemini did not produce a video (last outcome: "
                          f"{last_rejection}), including the lightly edited "
                          "retry variants.",
                 "rejected": True}, page)

    target = os.path.join(output_dir,
                          download.suggested_filename or "generated_video.mp4")
    try:
        download.save_as(target)
    except Exception as exc:  # noqa: BLE001 - reported, not a fatal crash
        if not _is_target_closed_error(exc):
            raise
        return ({"video": None,
                 "error": "Gemini generated the video, but the browser closed "
                          f"before the download could be saved: {exc}",
                 "rejected": False}, page)
    return ({"video": os.path.abspath(target), "error": None, "rejected": False}, page)


def _read_request(request):
    """Normalises one worker request into (prompt, output_dir, images,
    settings) or raises ValueError with a reportable message."""
    output_dir = request.get("outputDir") or ""
    prompt_path = request.get("promptFile") or ""
    settings = request.get("settings") or {}
    images = [p for p in (request.get("images") or []) if os.path.isfile(p)]
    if not output_dir:
        raise ValueError("the request has no outputDir")
    try:
        with open(prompt_path, encoding="utf-8") as handle:
            prompt = handle.read().strip()
    except OSError as exc:
        raise ValueError(f"could not read the prompt file: {exc}") from exc
    if not prompt:
        raise ValueError("the prompt file is empty")
    os.makedirs(output_dir, exist_ok=True)
    return prompt, output_dir, images, settings


# ---------------------------------------------------------------------------
# Worker mode: ONE browser, many requests
# ---------------------------------------------------------------------------

def _worker_main():
    """Long-lived mode (`--worker`): launches the browser ONCE, then serves
    one JSON request per stdin line, answering with one JSON result line
    each, until stdin closes. This is what keeps a SINGLE browser window
    across the caller's whole retry loop instead of relaunching one per
    attempt (which used to pop a new window every try, and raced the
    previous Chrome's profile lock on teardown).

    Request : {"promptFile": ..., "outputDir": ..., "settings": {...},
               "images": [...]}
    Response: {"video": path|null, "error": str|null, "rejected": bool}
    """
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        _respond({"video": None, "rejected": False,
                  "error": "Playwright is not installed (pip install playwright "
                           "&& playwright install chromium)"})
        return 1

    _log("worker: started, waiting for requests on stdin")
    context = None
    page = None
    playwright = None
    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                request = json.loads(line)
            except json.JSONDecodeError as exc:
                _respond({"video": None, "rejected": False,
                          "error": f"malformed request: {exc}"})
                continue
            try:
                prompt, output_dir, images, settings = _read_request(request)
            except ValueError as exc:
                _respond({"video": None, "rejected": False, "error": str(exc)})
                continue

            _log(f"worker: request output={output_dir}, images={images}, "
                 f"prompt={prompt[:150]!r}")
            try:
                if context is None:
                    # First request pays the launch + login + Ultra cost;
                    # every later one reuses this very browser and tab.
                    playwright = sync_playwright().start()
                    context, launch_note, launch_error = _launch_context(
                        playwright, bool(settings.get("useSystemChromeProfile", True)))
                    if context is None:
                        _respond({"video": None, "rejected": False,
                                  "error": "Could not launch a browser. If Chrome "
                                           "is currently open with your default "
                                           "profile, close it first. Details: "
                                           f"{launch_error}"})
                        continue
                    _log(f"worker: browser ready ({launch_note})")
                page, prepare_error = _prepare_page(
                    context, bool(settings.get("requireUltra", True)), page)
                if prepare_error:
                    _respond({"video": None, "rejected": False, "error": prepare_error})
                    continue
                result, page = _generate_once(context, page, prompt, images,
                                              output_dir, settings)
                _respond(result)
            except Exception as exc:  # noqa: BLE001 - one bad request must not
                # kill the worker: report it and stay alive for the next one.
                _log(f"worker: request failed: {exc}")
                if _is_target_closed_error(exc):
                    # The browser itself is gone — drop it so the NEXT
                    # request relaunches cleanly instead of failing forever.
                    _log("worker: browser lost, will relaunch on the next request")
                    context = None
                    page = None
                _respond({"video": None, "rejected": False, "error": str(exc)})
    finally:
        _log("worker: stdin closed, shutting the browser down")
        try:
            if context is not None:
                context.close()
        except Exception:
            pass
        try:
            if playwright is not None:
                playwright.stop()
        except Exception:
            pass
    return 0


def _respond(result):
    """One JSON result line on stdout, flushed immediately — the caller
    reads line by line and would otherwise block on the pipe buffer."""
    error = result.get("error")
    if error:
        _log("RESPOND error: " + str(error))
        result = dict(result, error=f"{error} [debug log: {LOG_PATH}]")
    else:
        _log("RESPOND video: " + str(result.get("video")))
    sys.stdout.write(json.dumps(result) + "\n")
    sys.stdout.flush()


# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--worker":
        return _worker_main()
    if len(sys.argv) < 4:
        _emit(error="usage: generate_video_gemini.py <prompt-file> <output-dir> "
                    "<settings-json> [image...]   |   generate_video_gemini.py --worker")
        return 1
    prompt_path, output_dir = sys.argv[1], sys.argv[2]
    try:
        settings = json.loads(sys.argv[3]) if sys.argv[3].strip() else {}
    except json.JSONDecodeError:
        settings = {}
    try:
        prompt, output_dir, _unused, settings = _read_request({
            "promptFile": prompt_path, "outputDir": output_dir,
            "settings": settings, "images": []})
    except ValueError as exc:
        _emit(error=str(exc))
        return 1
    image_paths = [p for p in sys.argv[4:] if os.path.isfile(p)]

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        _emit(error="Playwright is not installed "
                    "(pip install playwright && playwright install chromium)")
        return 1

    _log(f"start: output={output_dir}, images={image_paths}, "
         f"systemProfile={settings.get('useSystemChromeProfile', True)}, "
         f"requireUltra={settings.get('requireUltra', True)}, prompt={prompt[:150]!r}")

    try:
        with sync_playwright() as p:
            context, launch_note, launch_error = _launch_context(
                p, bool(settings.get("useSystemChromeProfile", True)))
            if context is None:
                _emit(error="Could not launch a browser. If Chrome is currently "
                            "open with your default profile, close it first. "
                            f"Details: {launch_error}")
                return 1
            try:
                page, prepare_error = _prepare_page(
                    context, bool(settings.get("requireUltra", True)))
                if prepare_error:
                    _emit(error=f"{prepare_error} ({launch_note})")
                    return 1
                result, _page = _generate_once(context, page, prompt, image_paths,
                                               output_dir, settings)
                _emit(video=result["video"], error=result["error"],
                      rejected=result["rejected"])
                return 0 if result["video"] else 1
            finally:
                context.close()
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())
