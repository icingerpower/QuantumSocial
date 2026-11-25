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
useSystemChromeProfile (default true), requireUltra (default true),
deleteChatOnFinish (default true).
"""
import datetime
import glob
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import time

from gemini_downloads import install_video_capture

PROFILE_DIR = os.path.expanduser("~/.cache/quantumsocial/gemini-profile")
SYSTEM_CHROME_PROFILE = os.path.expanduser("~/.config/google-chrome")
ULTRA_CACHE = os.path.expanduser("~/.cache/quantumsocial/gemini_ultra_account.json")
CHAT_CACHE_DEFAULT_PATH = os.path.expanduser("~/.cache/quantumsocial/gemini_chat_cache.json")
CHAT_CACHE_ENV = "QS_GEMINI_CHAT_CACHE_PATH"
GEMINI_URL = "https://gemini.google.com/app"

LOGGED_OUT_MARKERS = ("Sign in", "Connexion", "Se connecter")
LOGIN_WAIT_TIMEOUT_S = 300
LOGIN_POLL_INTERVAL_S = 2

VIDEO_TOOL_NAMES = ("Video", "Vidéo", "Veo")
DOWNLOAD_NAMES = ("Download", "Télécharger")
CHAT_ID_RE = re.compile(r"/app/([a-zA-Z0-9_-]{6,})")
DELETE_ACTION_RE = re.compile(r"\b(Supprimer|Delete)\b", re.IGNORECASE)
CANCEL_ACTION_RE = re.compile(r"\b(Annuler|Cancel)\b", re.IGNORECASE)
SIDEBAR_TOGGLE_SELECTORS = (
    'button[data-test-id="side-nav-menu-button"]',
    'button[aria-label*="Menu principal" i]',
    'button[aria-label*="Main menu" i]',
    'button[aria-label*="Ouvrir le menu" i]',
    'button[aria-label*="Expand menu" i]',
    'button[aria-label="Menu"]',
)

# Gemini's own "still working on it" indicator, shown while Veo is actively
# generating (observed live: still present after a full 5-minute stall
# timeout on a genuine, in-progress generation — the fixed per-attempt
# budget was cutting off real work, not silence). Its presence resets the
# stall clock instead of counting toward it; only the overall
# generationTimeoutSec ceiling still applies.
PROGRESS_MARKERS = (
    "Analyse…", "Analyse...", "Analysing…", "Analyzing…",
    "Génération en cours", "Génération de votre vidéo",
    "Generating…", "Generating...", "generating your video",
    "Creating your video", "Creating video",
    "Cela peut prendre quelques minutes", "This could take a few minutes",
    "check back to see when your video is ready",
)

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
LITE_MODEL_RE = re.compile(r"\b(?:Flash-Lite|Lite)\b", re.IGNORECASE)
PREFERRED_MODEL_MARKERS = ("Pro", "Advanced", "Plus")

REJECTION_MARKERS = (
    "I can't create", "I cannot create", "I can't generate", "I cannot generate",
    "I can't make", "I cannot make", "can't make videos", "cannot make videos",
    "can't generate your video", "cannot generate your video",
    "can't generate videos", "cannot generate videos",
    "can't generate video", "cannot generate video",
    "try another prompt",
    "having a hard time fulfilling your request",
    "help you with something else instead",
    "videos of real people", "videos of people", "real people in situations",
    "situations like that", "in situations like", "situations like",
    "I can't depict", "cannot depict", "some public figures", "public figures",
    "can't help with", "cannot help with", "unable to create", "not able to create",
    "unable to generate", "not able to generate",
    "violates", "against our policy", "against our", "safety policy", "content policy",
    "Je ne peux pas créer", "Je ne peux pas générer", "je ne peux pas vous aider",
    "Je ne peux pas faire", "ne peux pas créer de vidéos", "ne peux pas générer de vidéos",
    "impossible de générer votre vidéo", "essayez une autre invite",
    "du mal à répondre à votre demande", "aider avec autre chose",
    "personnes réelles", "de personnes réelles", "vidéos de personnes",
    "dans ce genre de situation", "dans de telles situations", "ce genre de situation",
    "contraire à", "nos règles", "règles de sécurité", "conditions d'utilisation",
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
    "can't generate more video",
    "cannot generate more video",
    "can't generate more videos",
    "cannot generate more videos",
    "can't create more video",
    "cannot create more video",
    "can't create more videos",
    "cannot create more videos",
    "can't make more video",
    "cannot make more video",
    "can't make more videos",
    "cannot make more videos",
    "no more video for you today",
    "no more videos for you today",
    "more video for you today",
    "more videos for you today",
    "plus de vidéo pour aujourd'hui",
    "plus de vidéos pour aujourd'hui",
    "plus de vidéos pour le moment",
    "plus de vidéo pour le moment",
    "ne peux plus générer de vidéo",
    "ne peux pas générer plus de vidéo",
    "reached your daily limit",
    "reach your daily limit",
    "reached the daily limit",
    "maximum I can do at one time",
    "maximum que je peux faire",
    "requests running right now",
    "génération de vidéo en cours",
    "générations de vidéo en cours",
    "reach the daily limit",
    "daily video limit",
    "daily limit for video",
    "daily generation limit",
    "daily limit",
    "limite quotidienne",
)

# Transient Google Veo server saturation notices (observed live: "I'm sorry,
# I'm experiencing unusually high traffic right now generating videos and am
# at full capacity. Please try again later."). Unlike prompt refusals, tweaking
# the prompt immediately wastes chats; waiting a short cooldown often unblocks
# it, and if saturation persists it escalates as a limit/capacity pause.
CAPACITY_MARKERS = (
    "unusually high traffic",
    "at full capacity",
    "high traffic right now",
    "trafic inhabituellement élevé",
    "à pleine capacité",
    "temporarily overloaded",
    "serveurs sont surchargés",
)

# Every run traces its steps into /tmp so automation bugs can be debugged
# after the fact (the AI assistant reads these): one log file per run, plus
# a screenshot + page text dump on failure.
LOG_DIR = "/tmp/quantumsocial"
LOG_PATH = os.path.join(
    LOG_DIR, time.strftime("gemini_%Y%m%d_%H%M%S.log"))
# Chrome's own log (see the launch args): Playwright only ever reports
# "target closed" when the browser dies, never the reason.
CHROME_LOG_PATH = LOG_PATH[:-4] + "_chrome.log"


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


def _emit(video=None, error=None, rejected=False, retryable=True, quotaExceeded=False):
    if error:
        _log("EMIT error: " + error)
        error = f"{error} [debug log: {LOG_PATH}]"
    else:
        _log("EMIT video: " + str(video))
    print(json.dumps({"video": video, "error": error, "rejected": rejected,
                      "retryable": retryable, "quotaExceeded": quotaExceeded}))


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


def _handle_tool_switch_dialog(page, action="confirm"):
    """If Gemini shows the 'Commencer une nouvelle discussion ? / Start a new chat?'
    confirmation dialog when selecting the video tool:
    - action == 'dismiss': close the dialog (Escape / close button) to STAY in
      the current conversation (preserving character, background, continuity).
    - action == 'confirm': toggle 'Ne plus me demander' and click 'Nouvelle discussion'
      (used when starting a fresh chat)."""
    dialog_indicators = (
        "Commencer une nouvelle discussion",
        "Start a new chat",
        "Sélectionner cet outil lancera une nouvelle discussion",
        "Selecting this tool will start a new chat",
        "Selecting this tool will start a new discussion",
    )
    has_dialog = False
    for indicator in dialog_indicators:
        try:
            loc = page.get_by_text(indicator, exact=False).first
            if loc.is_visible():
                has_dialog = True
                break
        except Exception:
            continue

    if not has_dialog:
        return False

    # Find the dialog container to avoid interacting with sidebar buttons
    dialog = None
    for selector in ("[role='dialog']", "mat-dialog-container", ".cdk-overlay-pane", "dialog"):
        try:
            candidate = page.locator(selector).filter(
                has_text="Nouvelle discussion"
            ).last
            if candidate.is_visible():
                dialog = candidate
                break
        except Exception:
            continue

    if dialog is None:
        try:
            candidate = page.locator("[role='dialog'], mat-dialog-container, .cdk-overlay-pane, dialog").first
            if candidate.is_visible():
                dialog = candidate
        except Exception:
            pass

    if action == "dismiss":
        _log("tool dialog: dismissing dialog to remain in existing conversation")
        try:
            if dialog is not None:
                close_btn = dialog.locator(
                    "button[aria-label*='Fermer'], button[aria-label*='Close'], "
                    "[data-mat-icon-name='close']"
                ).first
                if close_btn.is_visible():
                    close_btn.click(timeout=800)
                    page.wait_for_timeout(500)
                    return True
        except Exception:
            pass
        try:
            page.keyboard.press("Escape")
            page.wait_for_timeout(500)
            return True
        except Exception:
            pass
        return False

    # Optional: toggle "Ne plus me demander" / "Don't ask again"
    for dont_ask in ("Ne plus me demander", "Don't ask again", "Do not ask again"):
        try:
            scope = dialog if dialog is not None else page
            switch = scope.locator("button[role='switch'], [role='switch'], mat-slide-toggle, md-switch").first
            if switch.is_visible():
                switch.click()
                page.wait_for_timeout(300)
                break
            toggle = scope.get_by_text(dont_ask, exact=False).first
            if toggle.is_visible():
                toggle.click()
                page.wait_for_timeout(300)
                break
        except Exception:
            pass

    # Click "Nouvelle discussion" / "New chat" / "Start new chat"
    # Scoped to dialog first to avoid clicking sidebar 'Nouvelle discussion'
    new_chat_buttons = ("Nouvelle discussion", "New chat", "Start new chat", "Continuer", "Continue")
    for name in new_chat_buttons:
        if dialog is not None:
            try:
                btn = dialog.get_by_role("button", name=name, exact=False).first
                if btn.is_visible():
                    btn.click()
                    page.wait_for_timeout(1500)
                    _log(f"tool dialog: clicked scoped dialog button '{name}' to start new chat")
                    return True
            except Exception:
                pass
            try:
                btn = dialog.get_by_text(name, exact=True).first
                if btn.is_visible():
                    btn.click()
                    page.wait_for_timeout(1500)
                    _log(f"tool dialog: clicked scoped dialog text '{name}' to start new chat")
                    return True
            except Exception:
                pass

        # Fallback to page-level, using .last to target modal over sidebar
        try:
            btn = page.get_by_role("button", name=name, exact=False).last
            if btn.is_visible():
                btn.click()
                page.wait_for_timeout(1500)
                _log(f"tool dialog: clicked page button '{name}' (last) to start new chat")
                return True
        except Exception:
            pass

    return False


def _wait_ready_or_login(page, timeout_s=25):
    """Polls fast (0.5 s) instead of sleeping a fixed delay: returns "ready"
    as soon as the prompt editor shows up, "login" as soon as a sign-in flow
    is detected, "timeout" otherwise."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            if _login_in_progress(page):
                return "login"
            url_str = page.url if isinstance(getattr(page, "url", None), str) else ""
            in_existing = bool(CHAT_ID_RE.search(url_str))
            _handle_tool_switch_dialog(page, action="dismiss" if in_existing else "confirm")
            if _gemini_ready(page):
                return "ready"
        except Exception as exc:
            if _is_target_closed_error(exc):
                raise
        time.sleep(0.5)
    return "timeout"


def _extract_chat_id(url):
    if not isinstance(url, str) or not url:
        return None
    match = CHAT_ID_RE.search(url)
    return match.group(1) if match else None


def _base_gemini_url(page):
    """Preserves Google multi-account /u/<N>/app prefix (e.g. /u/4/app) when
    opening a fresh chat so retries stay on the active Ultra account slot."""
    url_str = page.url if page is not None and isinstance(getattr(page, "url", None), str) else ""
    m = re.match(r"^(https://gemini\.google\.com/u/\d+/app)\b", url_str)
    return m.group(1) if m else GEMINI_URL


def _chat_cache_path():
    override = os.environ.get(CHAT_CACHE_ENV, "").strip()
    return override if override else CHAT_CACHE_DEFAULT_PATH


def _utc_now_iso():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _load_chat_cache():
    path = _chat_cache_path()
    try:
        with open(path, encoding="utf-8") as handle:
            raw = json.load(handle)
    except (OSError, ValueError):
        return []
    items = raw.get("chats", []) if isinstance(raw, dict) else (raw if isinstance(raw, list) else [])
    cleaned = []
    seen = set()
    for item in items:
        if not isinstance(item, dict):
            continue
        url = str(item.get("url") or "").strip()
        chat_id = str(item.get("chatId") or "").strip() or (_extract_chat_id(url) or "")
        if not chat_id or not url or chat_id in seen:
            continue
        seen.add(chat_id)
        cleaned.append({
            "chatId": chat_id,
            "url": url,
            "createdAt": str(item.get("createdAt") or "").strip() or _utc_now_iso(),
            "updatedAt": str(item.get("updatedAt") or item.get("createdAt") or "").strip() or _utc_now_iso(),
            "status": str(item.get("status") or "active").strip() or "active",
        })
    return cleaned


def _save_chat_cache(entries):
    path = _chat_cache_path()
    parent = os.path.dirname(os.path.abspath(path))
    try:
        os.makedirs(parent, exist_ok=True)
        fd, tmp_path = tempfile.mkstemp(prefix=".gemini_chat_cache_", suffix=".tmp", dir=parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump({"chats": entries}, handle, indent=2)
            os.replace(tmp_path, path)
        finally:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass
    except OSError as exc:
        _log(f"chat_cache: could not save {path}: {exc}")


def _record_cached_chat(url, status="active"):
    """Persists a created Gemini chat link + creation timestamp so crashed or
    failed runs can always be cleaned up later from the Workflow UI."""
    if not isinstance(url, str):
        return False
    url = url.strip()
    chat_id = _extract_chat_id(url)
    if not chat_id:
        return False
    entries = _load_chat_cache()
    now = _utc_now_iso()
    for entry in entries:
        if entry["chatId"] == chat_id:
            if url:
                entry["url"] = url
            entry["updatedAt"] = now
            entry["status"] = status
            _save_chat_cache(entries)
            return True
    entries.insert(0, {
        "chatId": chat_id,
        "url": url,
        "createdAt": now,
        "updatedAt": now,
        "status": status,
    })
    _save_chat_cache(entries)
    _log(f"chat_cache: recorded {chat_id} ({status})")
    return True


def _remove_cached_chat(url_or_id):
    if not isinstance(url_or_id, str):
        return False
    raw = url_or_id.strip()
    if not raw:
        return False
    chat_id = _extract_chat_id(raw) or raw
    entries = _load_chat_cache()
    filtered = [e for e in entries if e.get("chatId") != chat_id and e.get("url") != raw]
    if len(filtered) != len(entries):
        _save_chat_cache(filtered)
        _log(f"chat_cache: removed {chat_id}")
        return True
    return False


def _click_delete_in_open_menu_and_confirm(page, chat_id):
    """Clicks 'Supprimer' / 'Delete' in an already-opened Angular Material menu,
    then confirms the deletion dialog and verifies the chat actually disappeared."""
    had_sidebar_link_before = False
    try:
        had_sidebar_link_before = page.locator(f'a[href*="{chat_id}"], [href*="{chat_id}"]').count() > 0
    except Exception:
        pass

    delete_clicked = False
    overlay_scopes = page.locator(
        ".cdk-overlay-pane, [role='menu'], .mat-mdc-menu-panel, mat-menu"
    )
    direct_del = page.locator(
        '.cdk-overlay-pane [data-test-id="delete-button"], '
        '[role="menu"] [data-test-id="delete-button"], '
        '.cdk-overlay-pane [role="menuitem"]:has(mat-icon[fonticon="delete"]), '
        '.cdk-overlay-pane [role="menuitem"]:has(mat-icon[data-mat-icon-name="delete"]), '
        '.cdk-overlay-pane button:has(mat-icon[fonticon="delete"]), '
        '.cdk-overlay-pane button:has(mat-icon[data-mat-icon-name="delete"])'
    )
    try:
        if direct_del.count() > 0 and direct_del.first.is_visible():
            direct_del.first.click(timeout=1500)
            delete_clicked = True
    except Exception:
        pass

    if not delete_clicked:
        candidates = overlay_scopes.locator("[role='menuitem'], .mat-mdc-menu-item, button")
        try:
            total = candidates.count()
            for idx in range(total):
                item = candidates.nth(idx)
                if not item.is_visible():
                    continue
                label = (
                    (item.get_attribute("aria-label") or "")
                    + " "
                    + (item.inner_text() or "")
                ).strip()
                if DELETE_ACTION_RE.search(label) and not CANCEL_ACTION_RE.search(label):
                    item.click(timeout=1500)
                    delete_clicked = True
                    break
        except Exception:
            pass

    if not delete_clicked:
        try:
            page.keyboard.press("Escape")
            page.wait_for_timeout(200)
        except Exception:
            pass
        return False

    page.wait_for_timeout(350)

    # Confirm in the modal dialog ("Supprimer la discussion ?" / "Delete chat?")
    confirmed = False
    confirm_deadline = time.monotonic() + 3.5
    while time.monotonic() < confirm_deadline and not confirmed:
        try:
            direct_confirm = page.locator(
                'button[data-test-id="confirm-button"], '
                'mat-dialog-container button[data-test-id="confirm-button"], '
                '.mat-mdc-dialog-container button[data-test-id="confirm-button"]'
            )
            if direct_confirm.count() > 0 and direct_confirm.last.is_visible():
                direct_confirm.last.click(timeout=1500)
                confirmed = True
                break
        except Exception:
            pass

        dialog_btns = page.locator(
            "mat-dialog-actions button, .mat-mdc-dialog-actions button, "
            "message-dialog button, mat-dialog-container button, "
            ".mat-mdc-dialog-container button, [role='dialog'] button"
        )
        try:
            btn_count = dialog_btns.count()
            for idx in range(btn_count):
                btn = dialog_btns.nth(idx)
                if not btn.is_visible():
                    continue
                label = (
                    (btn.get_attribute("aria-label") or "")
                    + " "
                    + (btn.inner_text() or "")
                ).strip()
                if DELETE_ACTION_RE.search(label) and not CANCEL_ACTION_RE.search(label):
                    btn.click(timeout=1500)
                    confirmed = True
                    break
        except Exception:
            pass

        if not confirmed:
            page.wait_for_timeout(150)

    if not confirmed:
        _log(f"delete_chat: clicked Delete menu item for {chat_id}, but confirmation button was not found")
        try:
            page.keyboard.press("Escape")
        except Exception:
            pass
        return False

    # Verify deletion actually took effect (URL redirected away from chat_id or sidebar row removed)
    deleted_confirmed = False
    gone_deadline = time.monotonic() + 4.5
    while time.monotonic() < gone_deadline:
        try:
            cur_url = page.url if isinstance(getattr(page, "url", None), str) else ""
            link_count = page.locator(f'a[href*="{chat_id}"], [href*="{chat_id}"]').count()
            if chat_id not in cur_url:
                deleted_confirmed = True
                break
            if had_sidebar_link_before and link_count == 0:
                deleted_confirmed = True
                break
        except Exception:
            break
        page.wait_for_timeout(150)

    if deleted_confirmed:
        _log(f"delete_chat: deleted Gemini conversation {chat_id}")
        return True
    _log(f"delete_chat: confirmed dialog for {chat_id}, but URL/sidebar still showed {chat_id}")
    return False


def _try_sidebar_delete_for_chat(page, chat_id):
    """Finds the sidebar row for chat_id (expanding the sidebar if collapsed and
    allowing a brief settle window for newly-created chats), opens its 3-dots
    actions menu, and deletes it."""
    for expand_pass in range(2):
        found_row = False
        settle_attempts = 1 if expand_pass == 0 else 5
        for settle_try in range(settle_attempts):
            try:
                found_row = bool(page.evaluate("""({ chatId, allowFirstFallback }) => {
                    document.querySelectorAll('[data-qs-delete-target]').forEach(
                        el => el.removeAttribute('data-qs-delete-target'));
                    const isVisible = (el) => {
                        if (!el) return false;
                        const r = el.getBoundingClientRect();
                        return r.width > 0 && r.height > 0;
                    };
                    const climbToRow = (node) => {
                        if (!node) return null;
                        let cur = node;
                        for (let depth = 0; depth < 6 && cur && cur.parentElement; depth++) {
                            const parent = cur.parentElement;
                            const convLinks = parent.querySelectorAll(
                                'a[href*="/app/"], [data-test-id="conversation"], conversations-list-item');
                            if (convLinks.length > 1) break;
                            if (cur.matches && cur.matches(
                                'conversations-list-item, .conversation-items-container, li, [role="listitem"]')) {
                                break;
                            }
                            if (parent.querySelector(
                                'button[data-test-id="actions-menu-button"], conversation-actions button, '
                                + 'button.mat-mdc-menu-trigger, mat-icon[fonticon="more_vert"], '
                                + 'mat-icon[data-mat-icon-name="more_vert"]')) {
                                cur = parent;
                                break;
                            }
                            cur = parent;
                        }
                        return cur;
                    };

                    // 1) Exact chatId match (handles /app/<id>, /u/4/app/<id>, relative app/<id>, jslog)
                    const exactCandidates = Array.from(document.querySelectorAll(
                        `a[href*="${chatId}"], [href*="${chatId}"], [jslog*="${chatId}"]`
                    )).filter(isVisible);
                    if (exactCandidates.length > 0) {
                        const row = climbToRow(exactCandidates[0]);
                        if (row) {
                            row.setAttribute('data-qs-delete-target', '1');
                            return true;
                        }
                    }

                    // 2) Active / selected conversation in sidebar when current URL matches chatId
                    const urlMatches = (location.href || '').includes(chatId);
                    if (urlMatches) {
                        const navScope = document.querySelector(
                            'conversations-list, mat-sidenav, nav, aside, [role="navigation"]');
                        if (navScope && isVisible(navScope)) {
                            const activeItem = navScope.querySelector(
                                'conversations-list-item.selected, [data-test-id="conversation"].selected, '
                                + '.conversation.selected, [aria-current="page"], [aria-selected="true"]');
                            if (activeItem && isVisible(activeItem)) {
                                const row = climbToRow(activeItem);
                                if (row) {
                                    row.setAttribute('data-qs-delete-target', '1');
                                    return true;
                                }
                            }
                            // 3) Last-resort fallback: if URL matches chatId and a user-query is rendered
                            //    (proving this chat was just active) and no href/selected matched, pick #1 in Recent
                            if (allowFirstFallback && document.querySelector('user-query')) {
                                const firstItem = navScope.querySelector(
                                    'conversations-list-item, [data-test-id="conversation"]');
                                if (firstItem && isVisible(firstItem)) {
                                    const row = climbToRow(firstItem);
                                    if (row) {
                                        row.setAttribute('data-qs-delete-target', '1');
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                    return false;
                }""", {"chatId": chat_id, "allowFirstFallback": (expand_pass == 1 and settle_try == settle_attempts - 1)}))
            except Exception:
                found_row = False
            if found_row:
                break
            if settle_try + 1 < settle_attempts:
                page.wait_for_timeout(450)

        if found_row:
            row_loc = page.locator('[data-qs-delete-target="1"]').first
            try:
                row_loc.scroll_into_view_if_needed(timeout=1000)
            except Exception:
                pass
            try:
                row_loc.hover(timeout=1500)
                row_loc.dispatch_event("mouseover")
                page.wait_for_timeout(250)
            except Exception:
                pass

            menu_selectors = (
                'button[data-test-id="actions-menu-button"]',
                'conversation-actions button',
                'button:has(mat-icon[fonticon="more_vert"])',
                'button:has(mat-icon[data-mat-icon-name="more_vert"])',
                'button[aria-label*="Plus d\'options" i]',
                'button[aria-label*="More options" i]',
                'button[aria-label*="Options" i]',
                'button[aria-label*="More" i]',
                'button.mat-mdc-menu-trigger',
                'button',
            )
            clicked_menu = False
            for sel in menu_selectors:
                btn_group = row_loc.locator(sel)
                try:
                    if btn_group.count() == 0:
                        continue
                    target_btn = btn_group.last
                    try:
                        target_btn.click(timeout=1200)
                    except Exception:
                        target_btn.click(timeout=1200, force=True)
                    clicked_menu = True
                    break
                except Exception:
                    continue

            if clicked_menu:
                page.wait_for_timeout(350)
                if _click_delete_in_open_menu_and_confirm(page, chat_id):
                    return True

        if expand_pass == 0:
            # Sidebar may be collapsed — click the hamburger menu button and retry once
            toggled = False
            for toggle_sel in SIDEBAR_TOGGLE_SELECTORS:
                try:
                    toggle_btn = page.locator(toggle_sel).first
                    if toggle_btn.count() > 0 and toggle_btn.is_visible():
                        toggle_btn.click(timeout=1200)
                        page.wait_for_timeout(550)
                        toggled = True
                        break
                except Exception:
                    continue
            if not toggled:
                break
    return False


def _delete_current_chat(page, target_url=None):
    """Best-effort deletion of the Gemini chat identified by target_url or page.url.

    Safety rule: ONLY deletes when a concrete /app/<chat_id> URL (including
    multi-account /u/<N>/app/<chat_id> URLs) is present so an uncreated chat
    on bare /app never deletes an unrelated conversation from the user's history."""
    if page is None:
        return False
    try:
        if hasattr(page, "is_closed") and page.is_closed():
            return False
    except Exception:
        return False

    page_url = page.url if isinstance(getattr(page, "url", None), str) else ""
    chat_id = _extract_chat_id(target_url) or _extract_chat_id(page_url)
    if not chat_id:
        _log("delete_chat: no active /app/<chat_id> URL — nothing to delete")
        return False

    try:
        if chat_id not in page_url and isinstance(target_url, str) and chat_id in target_url:
            _log(f"delete_chat: navigating to {target_url} to delete chat {chat_id}")
            page.goto(target_url, wait_until="domcontentloaded", timeout=30000)
            nav_state = _wait_ready_or_login(page, timeout_s=10)
            page_url = page.url if isinstance(getattr(page, "url", None), str) else ""
            if chat_id not in page_url:
                if nav_state == "ready" and "gemini.google.com" in page_url and not _login_in_progress(page):
                    _log(f"delete_chat: chat {chat_id} redirected to {page_url} (already gone)")
                    _remove_cached_chat(chat_id)
                    return True
                _log(f"delete_chat: chat {chat_id} redirected to {page_url} (state={nav_state})")
                return False

        # Strategy 1: Top active-conversation header actions / title menu (works even when sidebar is closed)
        if chat_id in page_url:
            header_triggers = (
                'conversation-actions button[data-test-id="actions-menu-button"]',
                'button[data-test-id="conversation-actions-menu-button"]',
                '[data-test-id="conversation-title"]',
                'conversation-actions button',
                'button:has(mat-icon[fonticon="more_vert"])',
                'button:has(mat-icon[data-mat-icon-name="more_vert"])',
                'button[aria-label*="options de la discussion" i]',
                'button[aria-label*="conversation options" i]',
            )
            for sel in header_triggers:
                try:
                    locs = page.locator(sel)
                    for idx in range(locs.count()):
                        cand = locs.nth(idx)
                        if not cand.is_visible():
                            continue
                        in_header = bool(cand.evaluate("""el => {
                            if (el.closest('nav, aside, [role="navigation"], [class*="sidenav"], [class*="sidebar"], conversations-list, user-query, model-response'))
                                return false;
                            const r = el.getBoundingClientRect();
                            return r.width > 0 && r.height > 0 && r.top < 160;
                        }"""))
                        if not in_header:
                            continue
                        cand.click(timeout=1200)
                        page.wait_for_timeout(350)
                        if _click_delete_in_open_menu_and_confirm(page, chat_id):
                            _remove_cached_chat(chat_id)
                            return True
                        break
                except Exception:
                    continue

        # Strategy 2: Sidebar conversation list item matching chat_id (handles /u/<N>/app/<id>)
        if _try_sidebar_delete_for_chat(page, chat_id):
            _remove_cached_chat(chat_id)
            return True

        _log(f"delete_chat: could not confirm deletion for chat {chat_id} (url={page_url})")
        _dump_state(page, "delete_chat_failed")
    except Exception as exc:  # noqa: BLE001 - cleanup must never abort a completed download
        _log(f"delete_chat: best-effort cleanup failed for {chat_id}: {exc}")
    return False


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


def _is_model_picker_candidate(locator):
    """True when locator is a compact UI control outside the chat history and sidebar."""
    try:
        if not locator.is_visible():
            return False
        return bool(locator.evaluate("""el => {
            if (el.closest('nav, aside, [role="navigation"], [class*="sidenav"], [class*="sidebar"], conversations-list, .conversations-container, user-query, model-response'))
                return false;
            const label = (el.getAttribute('aria-label') || el.innerText || '').trim();
            return label.length > 0 && label.length <= 45;
        }"""))
    except Exception:
        return False


def _switch_to_video_capable_model(page):
    """Best effort: if the current model looks like a lightweight variant
    that disables Veo (observed live: "Créer une vidéo" present but greyed
    out while "Flash-Lite" was selected), open the model picker and try an
    option that looks more capable. Returns True if a switch was attempted
    (does not guarantee it landed on a video-capable model — the caller
    still has to retry the tool click and can fail again)."""
    button = None
    for locator_group in (
        page.get_by_role("button", name=LITE_MODEL_RE),
        page.get_by_text(LITE_MODEL_RE),
    ):
        try:
            count = locator_group.count()
            for idx in range(count):
                candidate = locator_group.nth(idx)
                if _is_model_picker_candidate(candidate):
                    button = candidate
                    break
            if button is not None:
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
        marker_re = re.compile(rf"\b{re.escape(marker)}\b", re.IGNORECASE)
        # Role-scoped and overlay-scoped locators only — a conversation or
        # sidebar can contain unrelated "Pro"/"Advanced" text elsewhere on the
        # page, and a page-wide get_by_text(...) could click one of those
        # instead of the just-opened dropdown's actual option.
        for locator in (
            page.get_by_role("menuitem", name=marker_re),
            page.get_by_role("menuitemradio", name=marker_re),
            page.get_by_role("option", name=marker_re),
            page.locator("[role='menu'], [role='listbox'], .cdk-overlay-pane, mat-menu").get_by_text(marker_re),
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


def _is_video_tool_disabled(page):
    """Returns True if a video tool entry is visible in the '+' menu but disabled/greyed out."""
    for text in CREATE_VIDEO_TEXTS:
        try:
            loc = page.get_by_text(text, exact=False).first
            if loc.is_visible():
                is_dis = loc.evaluate("""el => {
                    const btn = el.closest('button, [role=menuitem], [role=menuitemcheckbox], mat-list-item, div.gem-menu-item') || el;
                    return btn.hasAttribute('disabled') ||
                           btn.getAttribute('aria-disabled') === 'true' ||
                           btn.classList.contains('disabled') ||
                           btn.classList.contains('mdc-list-item--disabled');
                }""")
                if is_dis:
                    return True
        except Exception:
            continue
    return False


def _select_video_tool(page):
    """Activates the Veo tool: the "Créer une vidéo" entry of the "+" menu
    (possibly already open from a previous click), else legacy chips, else
    (last resort) switching away from a lightweight model that disables it
    and trying once more.

    Returns:
        "ok": successfully clicked and activated
        "disabled": the tool is visible in the menu but disabled/greyed out
                    (indicates Gemini daily video quota or capacity limit reached)
        "not_found": could not find the tool button
    """
    for attempt in range(2):
        for text in CREATE_VIDEO_TEXTS:
            try:
                page.get_by_text(text, exact=False).first.click(timeout=700)
                page.wait_for_timeout(800)
                _handle_tool_switch_dialog(page)
                _log(f"tool: clicked '{text}' (attempt {attempt + 1})")
                return "ok"
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
            _handle_tool_switch_dialog(page)
            _log(f"tool: clicked legacy button '{name}'")
            return "ok"
        except Exception:
            continue
    if _switch_to_video_capable_model(page):
        _open_plus_menu(page)
        for text in CREATE_VIDEO_TEXTS:
            try:
                page.get_by_text(text, exact=False).first.click(timeout=1200)
                page.wait_for_timeout(800)
                _handle_tool_switch_dialog(page)
                _log(f"tool: clicked '{text}' after switching model")
                return "ok"
            except Exception:
                continue

    if _is_video_tool_disabled(page):
        _log("tool: video creation entry is present in the '+' menu but DISABLED (quota/capacity limit in effect)")
        return "disabled"

    return "not_found"


def _attachment_count(page):
    """Count loaded image previews in the composer, excluding chat history.

    A file chooser accepting a path is not proof that Gemini attached it.
    Fail closed if a future UI no longer exposes a recognizable preview.
    """
    return page.locator(
        'file-preview img, [class*="file-preview"] img, '
        '[class*="attachment"] img, img[src^="blob:"]'
    ).evaluate_all("""images => images.filter(img => {
        if (img.closest('user-query, model-response, [data-message-id]'))
            return false;
        const box = img.getBoundingClientRect();
        const preview = img.closest('file-preview, [class*="file-preview"], '
                                    + '[class*="attachment"]');
        const busy = preview && preview.querySelector(
            '[role="progressbar"], [aria-busy="true"], mat-spinner');
        return box.width > 0 && box.height > 0 && img.checkVisibility()
            && img.complete && img.naturalWidth > 0 && !busy;
    }).length""")


def _attach_image(page, image, timeout_s=45):
    """Choose the file once, then wait for its loaded attachment preview."""
    if not os.path.isfile(image):
        _log(f"attach: image file is missing: {image}")
        return False
    before = _attachment_count(page)
    chosen = False
    for attempt in range(2):
        for text in UPLOAD_TEXTS:
            try:
                with page.expect_file_chooser(timeout=1500) as chooser_info:
                    page.get_by_text(text, exact=False).first.click(timeout=800)
                chooser_info.value.set_files(image)
                chosen = True
                break
            except Exception:
                continue
        if chosen:
            break
        if attempt == 0 and not _open_plus_menu(page):
            break
    if chosen:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if _attachment_count(page) > before:
                _log(f"attach: loaded preview confirmed for {image}")
                return True
            page.wait_for_timeout(250)
    _log(f"attach: could not confirm attachment for {image}; prompt will not be sent")
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


def _submit_prompt(page, editor, prompt, timeout_s=30, retry_interval_s=5,
                   required_attachments=0):
    """Wait for Send to be enabled and confirm the composer actually clears or generation starts.

    Retry ignored clicks, with a grace period for asynchronous submission.
    Never interpret a missing/detached editor or a click returning as success.
    """
    deadline = time.monotonic() + timeout_s
    next_click = 0
    attempts = 0
    send_buttons = page.get_by_role(
        "button", name=re.compile(r"Envoyer|Send|Submit", re.IGNORECASE))
    stop_selectors = (
        "button[aria-label*='Arrêter']", "button[aria-label*='Stop']",
        "[data-test-id*='stop-button']", "button:has([data-icon*='stop'])",
    )
    while time.monotonic() < deadline:
        try:
            if attempts > 0:
                for sel in stop_selectors:
                    if page.locator(sel).count() > 0 and page.locator(sel).first.is_visible():
                        _log("submit: confirmed generation active (stop button visible)")
                        return True

            if editor.is_visible():
                value = editor.evaluate(
                    "el => ('value' in el ? el.value : el.innerText)", timeout=500)
                if attempts > 0 and not value.strip():
                    _log("submit: confirmed composer cleared")
                    return True
                # Chromium inserts an extra newline into innerText for blank
                # lines in contenteditable editors. Compare the words after
                # normalizing whitespace so that formatting alone does not
                # make an unchanged draft look like a user edit.
                if re.sub(r"\s+", " ", value).strip() != re.sub(r"\s+", " ", prompt).strip():
                    _log("submit: draft changed; stopping automatic submission")
                    return False
                if time.monotonic() >= next_click and attempts < 3:
                    if _attachment_count(page) < required_attachments:
                        _log("submit: required image preview disappeared; stopping")
                        return False
                    for index in range(send_buttons.count()):
                        button = send_buttons.nth(index)
                        if not button.is_visible() or not button.is_enabled():
                            continue
                        # Set the retry delay even if Playwright times out after
                        # dispatching the click; Gemini may still accept it.
                        attempts += 1
                        next_click = time.monotonic() + retry_interval_s
                        button.click(timeout=1000)
                        _log(f"submit: clicked Send (attempt {attempts}), awaiting confirmation")
                        break
        except Exception as exc:
            if _is_target_closed_error(exc):
                raise
        page.wait_for_timeout(250)
    _log("submit: no confirmed submission before timeout")
    return False


def _edit_last_message(page, prompt, timeout_s=25):
    """Edits the last user prompt in the chat history instead of posting a new one.

    In multi-video workflows, editing the last message avoids inflating the
    conversation length across takes and retries, keeping the Gemini context
    clean and preventing the Veo video generator from becoming disabled.
    """
    user_queries = page.locator("user-query")
    count = user_queries.count()
    if count == 0:
        _log("edit: no user-query found to edit")
        return False

    last_query = user_queries.nth(count - 1)
    edit_button = None

    for btn in last_query.locator("button").all():
        try:
            label = (btn.get_attribute("aria-label") or btn.inner_text() or btn.get_attribute("title") or "").strip().lower()
            if any(marker in label for marker in ("modifier", "edit")):
                edit_button = btn
                break
        except Exception:
            continue

    if edit_button is None:
        try:
            last_query.hover(timeout=2000)
            page.wait_for_timeout(400)
            for btn in last_query.locator("button").all():
                label = (btn.get_attribute("aria-label") or btn.inner_text() or btn.get_attribute("title") or "").strip().lower()
                if any(marker in label for marker in ("modifier", "edit")):
                    edit_button = btn
                    break
        except Exception:
            pass

    if edit_button is None:
        _log("edit: edit button (Modifier/Edit) not found on the last user-query")
        return False

    try:
        edit_button.click(timeout=3000)
        page.wait_for_timeout(600)
    except Exception as exc:
        _log(f"edit: clicking edit button failed: {exc}")
        return False

    editor = last_query.locator("textarea, [contenteditable='true'], [role='textbox']").first
    try:
        editor.wait_for(state="visible", timeout=5000)
        editor.click(timeout=2000)
        editor.fill(prompt)
        page.wait_for_timeout(400)
    except Exception as exc:
        _log(f"edit: could not fill edit textarea: {exc}")
        return False

    update_button = None
    for btn in last_query.locator("button").all():
        try:
            label = (btn.get_attribute("aria-label") or btn.inner_text() or "").strip().lower()
            if any(marker in label for marker in ("mettre à jour", "update", "envoyer", "send")):
                update_button = btn
                break
        except Exception:
            continue

    if update_button is None:
        _log("edit: update button (Mettre à jour/Update) not found")
        return False

    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            if update_button.is_enabled():
                update_button.click(timeout=2000)
                _log(f"edit: clicked update button on edited user query ({len(prompt)} chars)")
                page.wait_for_timeout(800)
                return True
        except Exception:
            pass
        page.wait_for_timeout(250)

    _log("edit: update button did not become enabled before timeout")
    return False



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


def _download_path(download):
    """Playwright's own path for a finished download. Still works when only
    the PAGE died; raises once the whole browser is gone."""
    return download.path()


def _download_artifact_path(download):
    """Last resort when even download.path() needs a browser that no longer
    exists: Playwright stores downloads under its artifacts directory, so
    look for the newest file there. Best effort — the layout is internal, so
    a miss just means the caller reports the loss as before."""
    roots = [os.path.join(os.environ.get("TMPDIR", "/tmp"), name)
             for name in ("playwright-artifacts", )]
    candidates = []
    for root in roots:
        candidates.extend(glob.glob(os.path.join(root, "**", "*"), recursive=True))
    candidates.extend(glob.glob(
        os.path.join(os.environ.get("TMPDIR", "/tmp"),
                     "playwright-artifacts-*", "**", "*"), recursive=True))
    files = [path for path in candidates if os.path.isfile(path)]
    if not files:
        return None
    newest = max(files, key=os.path.getmtime)
    # Only trust something written in the last couple of minutes — this
    # directory is shared with any other Playwright run on the machine.
    if time.time() - os.path.getmtime(newest) > 120:
        return None
    return newest


def _rejection_in(text):
    for marker in REJECTION_MARKERS:
        if marker.lower() in text.lower():
            return marker
    return None


def _video_quota_in(text):
    """Returns the quota-exhaustion notice if text contains a known quota marker, else None."""
    if not text:
        return None
    lowered = text.lower()
    for marker in VIDEO_QUOTA_MARKERS:
        idx = lowered.find(marker.lower())
        if idx >= 0:
            return text[idx:idx + 200].split("\n")[0].strip()
    return None


def _video_capacity_in(text):
    """Returns the high-traffic/capacity notice if text indicates temporary server saturation."""
    if not text:
        return None
    lowered = text.lower()
    for marker in CAPACITY_MARKERS:
        idx = lowered.find(marker.lower())
        if idx >= 0:
            return text[idx:idx + 200].split("\n")[0].strip()
    return None


def _video_quota_message(page):
    """Returns the quota-exhaustion notice (first line, with the reset time
    if shown) when Gemini's own "no more videos right now" wall is up, else
    None. See VIDEO_QUOTA_MARKERS.

    In an existing conversation, we must NOT search past chat turns
    (<model-response> elements), otherwise an old quota message from hours ago
    in the conversation history would block every future attempt forever."""
    try:
        banner_text = page.evaluate("""() => {
            const clone = document.body.cloneNode(true);
            const responses = clone.querySelectorAll('model-response');
            responses.forEach(el => el.remove());
            return clone.innerText;
        }""")
    except Exception:
        try:
            banner_text = page.locator("body").inner_text()
        except Exception:
            return None
    return _video_quota_in(banner_text)


def _ensure_imperative_video_prompt(prompt):
    """Ensures the prompt starts with an imperative command like 'Generate a video: '.

    Without an imperative action verb, Gemini's web interface frequently treats
    declarative scene descriptions as prompt-consultation requests (returning
    textual advice like 'This prompt is exceptionally well-structured...') instead
    of triggering the Veo video generation tool.
    """
    if not prompt or not prompt.strip():
        return prompt

    cleaned = prompt.strip()

    # Matches if the prompt already begins with an imperative video instruction:
    # e.g. "Generate a video:", "Generate video of", "Create a video",
    # "Make a video", "Produce a video", "Génère une vidéo", "Générer une vidéo",
    # "Generate a 9:16 vertical video of...", etc.
    imperative_pattern = re.compile(
        r"^\s*(?:generate|create|make|produce|génère|générer)\s+(?:an?\s+|une?\s+)?(?:[\w:()-]+\s+){0,3}vid[eé]o\b",
        re.IGNORECASE,
    )
    if imperative_pattern.match(cleaned):
        return cleaned

    video_of_pattern = re.compile(r"^\s*vid[eé]o\s+of\b\s*", re.IGNORECASE)
    if video_of_pattern.match(cleaned):
        return video_of_pattern.sub("Generate a video of ", cleaned)

    video_colon_pattern = re.compile(r"^\s*vid[eé]o\s*:\s*", re.IGNORECASE)
    if video_colon_pattern.match(cleaned):
        return video_colon_pattern.sub("Generate a video: ", cleaned)

    return f"Generate a video: {cleaned}"


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


def _find_target_response(page, target_query_index=None, initial_response_count=0, is_new_message=False):
    """Return locator for the model-response following user-query[target_query_index], or None.

    Ensures we NEVER inspect an older turn's response or an un-updated stale response.
    """
    try:
        info = page.evaluate("""({ idx, initialCount, isNew }) => {
            const queries = Array.from(document.querySelectorAll('user-query'));
            const allResponses = Array.from(document.querySelectorAll('model-response'));
            if (allResponses.length === 0) return { found: false };

            if (queries.length === 0) {
                // In minimal DOM or tests without user-query tags, fallback to the last response
                if (allResponses.length <= initialCount) {
                    return { found: false };
                }
                const lastResp = allResponses[allResponses.length - 1];
                if (lastResp.getAttribute('data-qs-stale') === '1') {
                    return { found: false, stale: true };
                }
                return { found: true, index: allResponses.length - 1 };
            }

            if (idx === null || idx === undefined || idx < 0 || idx >= queries.length) {
                return { found: false };
            }

            const qIdx = idx;
            const targetQ = queries[qIdx];
            const following = allResponses.filter(resp =>
                Boolean(targetQ.compareDocumentPosition(resp) & Node.DOCUMENT_POSITION_FOLLOWING)
            );
            if (following.length === 0) return { found: false };
            const firstResp = following[0];
            const between = queries.filter(q =>
                (targetQ.compareDocumentPosition(q) & Node.DOCUMENT_POSITION_FOLLOWING) &&
                (q.compareDocumentPosition(firstResp) & Node.DOCUMENT_POSITION_FOLLOWING)
            );
            if (between.length > 0) return { found: false };
            const respIdx = allResponses.indexOf(firstResp);
            if (isNew && respIdx < initialCount) {
                return { found: false };
            }
            if (firstResp.getAttribute('data-qs-stale') === '1') {
                return { found: false, stale: true };
            }
            return {
                found: true,
                index: respIdx
            };
        }""", {"idx": target_query_index, "initialCount": initial_response_count, "isNew": is_new_message})
        if info and info.get("found") and "index" in info and info["index"] >= 0:
            return page.locator("model-response").nth(info["index"])
    except Exception:
        pass
    return None


def _last_query_identity(page):
    """Identify the submitted turn, including its position in the conversation."""
    try:
        return page.locator("user-query").evaluate_all("""queries => {
            if (!queries.length) return null;
            const query = queries[queries.length - 1];
            const copy = query.cloneNode(true);
            copy.querySelectorAll('button').forEach(button => button.remove());
            return {
                count: queries.length,
                text: copy.textContent.trim(),
                ids: [query.id, ...Array.from(query.querySelectorAll('[id]'), el => el.id)],
                images: Array.from(query.querySelectorAll('img'),
                    img => [img.getAttribute('src'), img.getAttribute('alt')])
            };
        }""")
    except Exception:
        return None


def _image_fingerprints(image_paths):
    # Paths can stay the same when a CLI replaces an image, or change when a
    # project moves. Compare the actual attachments before reusing a message.
    fingerprints = []
    try:
        for path in image_paths:
            digest = hashlib.sha256()
            with open(path, "rb") as handle:
                for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                    digest.update(chunk)
            fingerprints.append(digest.hexdigest())
    except OSError:
        return None
    return fingerprints


def _can_edit_submission(page, state, image_fingerprints):
    return bool(state and state.get("promptSubmitted")
                and state.get("conversationUrl") == page.url
                and state.get("userQuery")
                and state["userQuery"] == _last_query_identity(page)
                and image_fingerprints is not None
                and state.get("imageFingerprints") == image_fingerprints)


def _run_attempt(page, prompt, image_paths, output_dir, deadline, attempt_deadline,
                 stall_timeout_s, message_action="new", chat_mode="fresh",
                 submission_state=None):
    """One full generation attempt in the current chat. Returns
    ("video", saved_path) / ("rejected", marker) / ("stalled", note) /
    ("error", message) / ("browser_lost", message). "stalled" = no reply at all before the attempt
    timeout — observed on forbidden prompts, where Gemini sometimes just
    never answers instead of refusing in text. A visible PROGRESS_MARKERS
    hit (Gemini's own "still working" indicator) resets attempt_deadline
    instead of counting toward it, since that is proof of real activity,
    not silence — only the overall `deadline` still bounds a run that keeps
    showing progress forever.

    When message_action == "edit_last", edits the previous user query in the
    same conversation instead of submitting a new message at the bottom.

    The download is saved to disk HERE, the instant the download event
    fires and while the browser is known alive — returning a live Download
    object to be saved by the caller lost a finished video every time the
    browser died in between (observed live: "Gemini generated the video,
    but the browser closed before the download could be saved")."""
    quota_message = _video_quota_message(page)
    if quota_message:
        _dump_state(page, "quota")
        return ("quota", quota_message)
    capture = install_video_capture(page, output_dir, _log)
    _switch_to_video_capable_model(page)

    image_fingerprints = _image_fingerprints(image_paths)
    if message_action == "edit_last" and not _can_edit_submission(
            page, submission_state, image_fingerprints):
        # A conversation URL alone is not a take identity: later steps share
        # the same chat, often with identical prompts but different images.
        # Legacy receipts also lack proof, so reattach the requested inputs.
        _log("run_attempt: last message is not verified for this take and its "
             "images — submitting a new message with the requested attachments")
        message_action = "new"
        if submission_state is not None:
            submission_state.clear()

    target_query_index = None
    attempt_start_time = time.time()
    saw_progress = False
    initial_response_count = page.locator("model-response").count()

    edited = False
    if message_action == "edit_last":
        user_queries = page.locator("user-query")
        if user_queries.count() > 0:
            target_query_index = user_queries.count() - 1
            # Mark any existing response following this query as stale before editing
            try:
                page.evaluate("""(idx) => {
                    const queries = document.querySelectorAll('user-query');
                    if (idx >= 0 && idx < queries.length) {
                        const targetQ = queries[idx];
                        const allResponses = Array.from(document.querySelectorAll('model-response'));
                        for (const resp of allResponses) {
                            if (targetQ.compareDocumentPosition(resp) & Node.DOCUMENT_POSITION_FOLLOWING) {
                                resp.setAttribute('data-qs-stale', '1');
                            }
                        }
                    }
                }""", target_query_index)
            except Exception:
                pass
        edited = _edit_last_message(page, prompt)
        if not edited:
            _log("run_attempt: edit_last was requested but editing failed — falling back to new message")
            # A failed edit may have opened an inline editor. Close it before
            # locating the normal composer for the new-message fallback.
            cancel = page.locator("user-query").get_by_role(
                "button", name=re.compile(r"^(cancel|annuler)$", re.I))
            if cancel.count() and cancel.last.is_visible():
                cancel.last.click(timeout=3000)
            target_query_index = None
            message_action = "new"

    in_existing_conversation = (chat_mode == "conversation" and
                                bool(re.search(r"/app/[a-zA-Z0-9_-]{6,}", page.url or "")))

    if not edited:
        if not in_existing_conversation:
            tool_status = _select_video_tool(page)
            if tool_status == "disabled":
                _dump_state(page, "video_tool_disabled")
                return ("quota", "Gemini video creation tool ('Créer une vidéo') is currently disabled by Google (daily video quota or concurrent limit reached).")
            if tool_status != "ok" and tool_status is not True:
                _dump_state(page, "no_tool")
                return ("error", "Could not find Gemini's video (Veo) tool button — "
                                 "the web UI may have changed; update the selectors in "
                                 "generate_video_gemini.py.")
            _handle_tool_switch_dialog(page, action="confirm")
            # Changing the video format can rebuild the composer. Do it before
            # attaching files so a rebuilt editor cannot silently drop the image.
            _select_aspect(page, prompt)
        else:
            _log("run_attempt: continuing in existing conversation (preserving chat context)")
            _switch_to_video_capable_model(page)

        attachment_baseline = _attachment_count(page)
        for image in image_paths:
            if not _attach_image(page, image):
                _dump_state(page, "attachment_failed")
                return ("error", f"Could not confirm the image attachment: {image}. "
                                 "The prompt was not sent.")

        initial_user_query_count = page.locator("user-query").count()
        initial_response_count = page.locator("model-response").count()

        editor = page.get_by_role("textbox").last
        editor.click(timeout=10000)
        editor.fill(prompt)
        if not _submit_prompt(page, editor, prompt,
                              required_attachments=attachment_baseline + len(image_paths)):
            _dump_state(page, "submission_failed")
            return ("error", "Gemini submission could not be confirmed. "
                             "Check the prompt and image attachment in the browser.")

        # Wait for the new user-query to appear in the conversation feed
        target_query_index = None
        deadline_query = min(time.monotonic() + 3.0, attempt_deadline)
        while time.monotonic() < deadline_query:
            current_q_count = page.locator("user-query").count()
            if current_q_count > initial_user_query_count:
                target_query_index = current_q_count - 1
                break
            if capture.get("video") or page.locator("model-response").count() > initial_response_count:
                break
            page.wait_for_timeout(100)

        # In DOMs with user-query tags, target_query_index MUST be a newly added query
        if page.locator("user-query").count() > 0 and target_query_index is None:
            _dump_state(page, "query_not_posted")
            return ("error", "Gemini submission could not be confirmed: new user prompt did not appear in the chat.")

    if submission_state is not None:
        submission_state.update(promptSubmitted=True, conversationUrl=page.url,
                                userQuery=_last_query_identity(page),
                                imageFingerprints=image_fingerprints)
        # A take's working directory survives app/worker restarts. Record only
        # confirmed submissions, so a pre-submit failure cannot edit step 1.
        with open(os.path.join(output_dir, "gemini_submission.json"), "w",
                  encoding="utf-8") as handle:
            json.dump(submission_state, handle)

    recorded_chat_id = None
    initial_page_url = page.url if isinstance(getattr(page, "url", None), str) else ""
    if _extract_chat_id(initial_page_url):
        _record_cached_chat(initial_page_url, status="active")
        recorded_chat_id = _extract_chat_id(initial_page_url)

    _log("waiting for the video or a refusal")

    existing_video_srcs = set()
    try:
        existing_video_srcs = set(page.evaluate("""() => {
            return Array.from(document.querySelectorAll('model-response video'))
                .map(v => v.src || v.currentSrc || '')
                .filter(Boolean);
        }"""))
    except Exception:
        pass

    while time.time() < min(deadline, attempt_deadline):
        cur_page_url = page.url if isinstance(getattr(page, "url", None), str) else ""
        cur_chat_id = _extract_chat_id(cur_page_url)
        if cur_chat_id and cur_chat_id != recorded_chat_id:
            _record_cached_chat(cur_page_url, status="active")
            recorded_chat_id = cur_chat_id
        if capture["video"]:
            return ("video", capture["video"])
        if capture["error"]:
            return ("error", capture["error"])
        try:
            page.locator("body").is_visible()
        except Exception as exc:  # noqa: BLE001 - see below
            # A DEAD page must break out immediately instead of being
            # polled for the rest of the stall timeout: swallowing this
            # burned a full 5 minutes on a corpse after a
            # browser crash, then reported a misleading "no reply before
            # the attempt timeout".
            if _is_target_closed_error(exc):
                raise

        target_response = _find_target_response(
            page, target_query_index, initial_response_count,
            is_new_message=(message_action == "new"))
        if target_response is None or not target_response.is_visible():
            time.sleep(2)
            continue

        # Reject if target_response only contains pre-existing video elements from prior turns
        if existing_video_srcs:
            try:
                curr_srcs = set(target_response.evaluate("""el => {
                    return Array.from(el.querySelectorAll('video'))
                        .map(v => v.src || v.currentSrc || '')
                        .filter(Boolean);
                }"""))
                if curr_srcs and curr_srcs.issubset(existing_video_srcs):
                    time.sleep(2)
                    continue
            except Exception:
                pass

        resp_text = ""
        try:
            resp_text = target_response.inner_text()
        except Exception:
            pass

        if any(marker.lower() in resp_text.lower() for marker in PROGRESS_MARKERS):
            saw_progress = True
            attempt_deadline = max(attempt_deadline, time.time() + stall_timeout_s)

        # Quota check MUST precede rejection check: Gemini's daily quota refusal
        # ("Sorry I can't generate more video for you today") matches words like
        # "I can't generate" in REJECTION_MARKERS, but prompt rewrites or retries
        # cannot work around a quota limit.
        # Scoped strictly to target_response or UI banners outside chat history!
        quota_hit = _video_quota_in(resp_text) or _video_quota_message(page)
        if quota_hit:
            _log(f"quota exhaustion detected: '{quota_hit}'")
            return ("quota", quota_hit)

        capacity_hit = _video_capacity_in(resp_text)
        if capacity_hit:
            _log(f"capacity saturation detected: '{capacity_hit}'")
            return ("capacity", capacity_hit)

        marker = _rejection_in(resp_text)
        if marker:
            _log(f"refusal detected: '{marker}'")
            return ("rejected", marker)

        # Look for download button ONLY within target_response (never on page or old responses!)
        has_download_btn = False
        elapsed = time.time() - attempt_start_time
        for name in DOWNLOAD_NAMES:
            dl_buttons = target_response.get_by_role("button", name=name, exact=False)
            if dl_buttons.count() == 0 or not dl_buttons.first.is_visible():
                continue
            # Guardrail: Veo takes at least 45s to generate a video. If a button appears instantly
            # without seeing any progress markers, ignore it until generation actually runs.
            if not saw_progress and elapsed < 45:
                _log(f"waiting: download button visible early ({elapsed:.1f}s) without progress — awaiting generation...")
                break
            has_download_btn = True
            try:
                with page.expect_download(timeout=4000) as download_info:
                    dl_buttons.first.click(timeout=2000)
            except Exception:
                if capture["video"]:
                    return ("video", capture["video"])
                if capture["error"]:
                    return ("error", capture["error"])
                continue
            # Saved immediately, before returning anywhere: the browser can
            # die at any moment and a Download object is worthless once it
            # does.
            download = download_info.value
            target = os.path.join(
                output_dir, download.suggested_filename or "generated_video.mp4")
            try:
                download.save_as(target)
                _log(f"download: saved {target}")
                return ("video", os.path.abspath(target))
            except Exception as exc:  # noqa: BLE001 - recovery attempt below
                _log(f"download: save_as failed: {exc}")
                browser_lost = _is_target_closed_error(exc)
            # save_as needs a live browser to move the file, and this browser
            # keeps dying exactly at download time — but the bytes are often
            # ALREADY complete in Playwright's own download directory. Copy
            # them straight out of there rather than throwing away a video
            # Gemini really did produce (and charged quota for).
            for recover in (_download_path, _download_artifact_path):
                try:
                    source = recover(download)
                except Exception as exc:  # noqa: BLE001 - try the next way
                    _log(f"download: {recover.__name__} failed: {exc}")
                    browser_lost = browser_lost or _is_target_closed_error(exc)
                    continue
                if source and os.path.isfile(source):
                    try:
                        shutil.copyfile(source, target)
                        _log(f"download: recovered from {source} -> {target}")
                        return ("video", os.path.abspath(target))
                    except OSError as exc:
                        _log(f"download: copy from {source} failed: {exc}")
            return ("browser_lost" if browser_lost else "error",
                             "Gemini generated the video, but it could not be "
                             "saved (the browser died at download time and the "
                             "file could not be recovered from disk either).")

        # Check if response finished but has NO video (text-only hallucination/refusal)
        has_video_tag = target_response.locator("video").count() > 0
        has_action_buttons = (
            target_response.locator(
                "button[aria-label*='Bonne réponse'], button[aria-label*='Good response'], "
                "button[aria-label*='Partager'], button[aria-label*='Share'], "
                "button[aria-label*='Copier'], button[aria-label*='Copy']"
            ).count() > 0 or
            bool(re.search(r"Your video is ready|vidéo est prête", resp_text, re.IGNORECASE))
        )

        is_still_generating = (
            any(marker.lower() in resp_text.lower() for marker in PROGRESS_MARKERS)
        )

        if not has_video_tag and not has_download_btn and has_action_buttons and not is_still_generating:
            _log(f"response finished without a video: {resp_text[:150]!r}")
            quota_hit = _video_quota_in(resp_text) or _video_quota_message(page)
            if quota_hit:
                _log(f"quota exhaustion detected: '{quota_hit}'")
                return ("quota", quota_hit)
            capacity_hit = _video_capacity_in(resp_text)
            if capacity_hit:
                _log(f"capacity saturation detected: '{capacity_hit}'")
                return ("capacity", capacity_hit)
            marker = _rejection_in(resp_text)
            if marker:
                return ("rejected", marker)
            return ("rejected", f"Gemini returned text without a video: {resp_text.strip()[:100]}")

        time.sleep(3)

    _dump_state(page, "stalled")
    return ("stalled", "no reply before the attempt timeout")


# ---------------------------------------------------------------------------
# Browser launch
# ---------------------------------------------------------------------------

# Set once the default Chrome profile has proved to be held by the user's
# own running browser: every later launch skips straight to the dedicated
# profile instead of poking their Chrome again (see _launch_context).
_SYSTEM_PROFILE_IN_USE = False


def _launch_context(playwright, use_system_profile):
    """Tries system Chrome + default profile, then Chrome + dedicated
    profile, then bundled Chromium. Returns (context, note, error)."""
    base_kwargs = {
        "headless": False,
        "viewport": {"width": 1400, "height": 900},
        "accept_downloads": True,
        # Google's login rejects browsers that advertise automation.
        "ignore_default_args": ["--enable-automation"],
        # Software rendering avoids dependence on the host's GPU drivers.
        # This does not prevent browser-process crashes such as the Chrome
        # download-bubble failure handled by gemini_downloads.py.
        "args": ["--disable-blink-features=AutomationControlled",
                 "--no-first-run", "--no-default-browser-check",
                 "--disable-gpu", "--disable-gpu-compositing",
                 "--disable-accelerated-video-decode",
                 "--disable-accelerated-2d-canvas",
                 # Chrome's own log, next to ours: when the browser dies
                 # mid-generation, Playwright only reports "target closed"
                 # and never WHY — this is the file that says what Chrome
                 # itself hit on the way down.
                 "--enable-logging", f"--log-file={CHROME_LOG_PATH}"],
    }
    attempts = []
    if use_system_profile and not _SYSTEM_PROFILE_IN_USE \
            and os.path.isdir(SYSTEM_CHROME_PROFILE):
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
            if user_dir == SYSTEM_CHROME_PROFILE \
                    and "existing browser session" in str(exc):
                # Your everyday Chrome owns that profile and will for as
                # long as it runs — retrying it on every relaunch only
                # wastes a launch AND hands a command line to your real
                # browser each time (Chrome answers "Opening in existing
                # browser session", which can pop a stray tab there).
                # Remember and skip it for the rest of this worker's life.
                globals()["_SYSTEM_PROFILE_IN_USE"] = True
                _log("launch: the default profile is in use by your own "
                     "Chrome — skipping it from now on")
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

def _prepare_page(context, require_ultra, existing_page=None, chat_mode="fresh",
                  conversation_url=None):
    """Brings the browser to a usable, logged-in, Ultra-checked Gemini page.

    In "fresh" mode (single-video): navigates to GEMINI_URL to open a new chat.
    In "conversation" mode (multi-video): reuses the current conversation if
    already loaded, or navigates to conversation_url if provided.
    """
    page = existing_page
    if page is None:
        page = context.pages[0] if context.pages else context.new_page()

    target_url = (conversation_url if (conversation_url and chat_mode == "conversation")
                  else GEMINI_URL)

    # In conversation mode, if the page is already open and ready in Gemini,
    # avoid navigating away unless we specifically need to open a target conversation URL.
    need_navigation = True
    if chat_mode == "conversation" and existing_page is not None and not existing_page.is_closed():
        current_url = existing_page.url or ""
        if conversation_url and conversation_url in current_url:
            need_navigation = False
        elif not conversation_url and "gemini.google.com/app" in current_url:
            need_navigation = False

    if need_navigation:
        page.goto(target_url, wait_until="domcontentloaded", timeout=60000)

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
        page.goto(target_url, wait_until="domcontentloaded", timeout=60000)
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


def _saved_submission(output_dir):
    try:
        with open(os.path.join(output_dir, "gemini_submission.json"), encoding="utf-8") as handle:
            state = json.load(handle)
        return state if isinstance(state, dict) else {}
    except (OSError, ValueError):
        return {}


def _generate_once(context, page, prompt, image_paths, output_dir, settings,
                   chat_mode="fresh", message_action="new"):
    state = _saved_submission(output_dir) if chat_mode == "conversation" else {}
    if state.get("promptSubmitted") and state.get("conversationUrl") == page.url:
        message_action = "edit_last"
    else:
        state = {}
    try:
        result, page = _generate_once_impl(context, page, prompt, image_paths, output_dir,
            settings, chat_mode, message_action, state)
    except Exception as exc:
        browser_lost = _is_target_closed_error(exc)
        if (not browser_lost and chat_mode != "conversation"
                and bool(settings.get("deleteChatOnFinish", True))):
            _delete_current_chat(page, state.get("conversationUrl"))
            state.pop("conversationUrl", None)
        result = {"video": None, "error": str(exc), "rejected": False,
                  "browserLost": browser_lost}
    result["promptSubmitted"] = bool(state.get("promptSubmitted"))
    if state.get("conversationUrl"):
        result["conversationUrl"] = state["conversationUrl"]
    return result, page


def _generate_once_impl(context, page, prompt, image_paths, output_dir, settings,
                        chat_mode, message_action, submission_state):
    """Runs the full attempt loop for ONE request on the given page and
    returns (result_dict, page) — the page comes back because a mid-run
    crash can replace it (see _recover_page). result_dict is exactly the
    JSON contract: {"video", "error", "rejected", "conversationUrl"}."""
    generation_timeout_s = int(settings.get("generationTimeoutSec", 900))
    in_page_retries = int(settings.get("inPageRejectionRetries", 3))
    preserve_prompt = bool(settings.get("preservePrompt", False))
    stall_timeout_s = int(settings.get("attemptStallTimeoutSec", 300))
    capacity_cooldown_s = int(settings.get("capacityCooldownSec", 30))
    delete_chat_on_finish = bool(settings.get("deleteChatOnFinish", True))
    keep_conversation = bool(settings.get("keepConversation", False))
    should_cleanup_fresh_chat = delete_chat_on_finish and chat_mode != "conversation"

    deadline = time.time() + generation_timeout_s
    original_conversation_url = page.url if chat_mode == "conversation" else None
    last_rejection = None
    last_capacity_note = None
    video_path = None
    for attempt in range(in_page_retries + 1):
        if preserve_prompt or last_capacity_note:
            # Keep the exact prompt on capacity retries (and first preservePrompt retry)
            variant = prompt.strip() if (last_capacity_note or attempt <= 1) else _prompt_variant(prompt, attempt - 1)
        else:
            variant = _prompt_variant(prompt, attempt)
        current_action = ("edit_last" if attempt > 0 and chat_mode == "conversation"
                          and submission_state.get("promptSubmitted")
                          else message_action)

        if attempt > 0:
            if current_action == "edit_last":
                _log(f"in-page retry {attempt}/{in_page_retries} in conversation — "
                     "editing last message with variant prompt")
            elif chat_mode == "conversation":
                _log(f"in-page retry {attempt}/{in_page_retries} in conversation — "
                     "posting new variant prompt")
            else:
                note = "unchanged prompt" if variant == prompt.strip() else "edited prompt"
                _log(f"in-page retry {attempt}/{in_page_retries} after "
                     f"{'capacity cooldown' if last_capacity_note else 'rejection'} — fresh chat, {note}")
                try:
                    page.goto(_base_gemini_url(page), wait_until="domcontentloaded", timeout=60000)
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
            status, payload = _run_attempt(page, variant, image_paths, output_dir,
                                           deadline, attempt_deadline,
                                           stall_timeout_s, message_action=current_action,
                                           chat_mode=chat_mode, submission_state=submission_state)
        except Exception as exc:  # noqa: BLE001 - recovered below
            if not _is_target_closed_error(exc):
                raise
            if settings.get("preservePrompt", False):
                return ({"video": None, "error": str(exc), "rejected": False,
                         "browserLost": True}, None)
            _log(f"attempt {attempt + 1}: page/context died mid-run ({exc}) — "
                 "recovering in the same browser instead of restarting...")
            page = _recover_page(context)
            recovery_url = submission_state.get("conversationUrl") or original_conversation_url
            if page is not None and chat_mode == "conversation" and recovery_url:
                page.goto(recovery_url, wait_until="domcontentloaded", timeout=60000)
                _wait_ready_or_login(page)
            if page is None:
                return ({"video": None,
                         "error": f"the browser tab closed unexpectedly ({exc})",
                         "browserLost": True,
                         "rejected": False}, page)
            last_rejection = f"the browser tab closed mid-attempt ({exc}) — retried"
            continue
        _log(f"attempt {attempt + 1}: {status}"
             + (f" ({payload})" if status != "video" else ""))
        if status in ("video", "rejected", "stalled"):
            submission_state["promptSubmitted"] = True
            if page and not page.is_closed():
                submission_state["conversationUrl"] = page.url
        active_chat_url = submission_state.get("conversationUrl") or (
            page.url if page and not page.is_closed() and isinstance(getattr(page, "url", None), str) else None
        )
        if active_chat_url and _extract_chat_id(active_chat_url):
            _record_cached_chat(active_chat_url, status="active")
        if status == "video":
            video_path = payload  # already saved to disk by _run_attempt
            with open(os.path.join(output_dir, "generation_prompt.txt"), "w",
                      encoding="utf-8") as handle:
                handle.write(variant)
            if should_cleanup_fresh_chat and not keep_conversation:
                if _delete_current_chat(page, active_chat_url):
                    submission_state.pop("conversationUrl", None)
                elif active_chat_url and _extract_chat_id(active_chat_url):
                    _record_cached_chat(active_chat_url, status="delete_failed")
            elif active_chat_url and _extract_chat_id(active_chat_url):
                _record_cached_chat(active_chat_url, status="kept")
            break
        if status == "quota":
            if should_cleanup_fresh_chat:
                if _delete_current_chat(page, active_chat_url):
                    submission_state.pop("conversationUrl", None)
                elif active_chat_url and _extract_chat_id(active_chat_url):
                    _record_cached_chat(active_chat_url, status="delete_failed")
            conv_url = page.url if page and not page.is_closed() else None
            return ({"video": None,
                     "error": f"Gemini daily video quota reached: {payload}",
                     "rejected": False,
                     "retryable": False,
                     "quotaExceeded": True,
                     "conversationUrl": conv_url}, page)
        if status == "capacity":
            if should_cleanup_fresh_chat:
                if _delete_current_chat(page, active_chat_url):
                    submission_state.pop("conversationUrl", None)
                elif active_chat_url and _extract_chat_id(active_chat_url):
                    _record_cached_chat(active_chat_url, status="delete_failed")
            last_capacity_note = payload
            last_rejection = payload
            remaining_s = deadline - time.time()
            if attempt < in_page_retries and remaining_s > 10:
                wait_s = min(max(0, capacity_cooldown_s) * (attempt + 1), 90, int(remaining_s - 5))
                if wait_s > 0:
                    _log(f"capacity: waiting {wait_s}s cooldown before retrying (attempt {attempt + 1}/{in_page_retries})...")
                    time.sleep(wait_s)
                continue
            conv_url = page.url if page and not page.is_closed() else None
            return ({"video": None,
                     "error": f"Gemini video servers at full capacity (limit reached): {payload}",
                     "rejected": False,
                     "retryable": False,
                     "quotaExceeded": True,
                     "conversationUrl": conv_url}, page)
        last_capacity_note = None
        if status in ("error", "browser_lost"):
            if status != "browser_lost" and should_cleanup_fresh_chat:
                if _delete_current_chat(page, active_chat_url):
                    submission_state.pop("conversationUrl", None)
                elif active_chat_url and _extract_chat_id(active_chat_url):
                    _record_cached_chat(active_chat_url, status="delete_failed")
            elif active_chat_url and _extract_chat_id(active_chat_url):
                _record_cached_chat(active_chat_url, status="failed")
            conv_url = page.url if page and not page.is_closed() else None
            return ({"video": None, "error": payload, "rejected": False,
                     "browserLost": status == "browser_lost",
                     "conversationUrl": conv_url}, page)
        # "rejected" and "stalled" both abandon this fresh chat and retry with an edited prompt
        if should_cleanup_fresh_chat:
            if _delete_current_chat(page, active_chat_url):
                submission_state.pop("conversationUrl", None)
            elif active_chat_url and _extract_chat_id(active_chat_url):
                _record_cached_chat(active_chat_url, status="delete_failed")
        last_rejection = payload
        if time.time() >= deadline:
            break

    conv_url = page.url if page and not page.is_closed() else None
    if video_path is None:
        retry_note = (", including the lightly edited retry variants."
                      if in_page_retries > 0 else ".")
        return ({"video": None,
                 "error": f"Gemini did not produce a video (last outcome: "
                          f"{last_rejection}){retry_note}",
                 "rejected": True,
                 "conversationUrl": conv_url}, page)
    return ({"video": video_path, "error": None, "rejected": False,
             "conversationUrl": conv_url}, page)


def _read_request(request):
    """Normalises one worker request into (prompt, output_dir, images,
    settings) or raises ValueError with a reportable message."""
    output_dir = request.get("outputDir") or ""
    prompt_path = request.get("promptFile") or ""
    settings = request.get("settings") or {}
    images = request.get("images") or []
    for image in images:
        if not os.path.isfile(image):
            raise ValueError(f"image file is missing: {image}")
    if not output_dir:
        raise ValueError("the request has no outputDir")
    try:
        with open(prompt_path, encoding="utf-8") as handle:
            prompt = handle.read().strip()
    except OSError as exc:
        raise ValueError(f"could not read the prompt file: {exc}") from exc
    if not prompt:
        raise ValueError("the prompt file is empty")
    prompt = _ensure_imperative_video_prompt(prompt)
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
               "images": [...], "chatMode": "fresh"|"conversation",
               "messageAction": "new"|"edit_last", "conversationUrl": ...}
    Response: {"video": path|null, "error": str|null, "rejected": bool,
               "conversationUrl": str|null}
    """
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        _respond({"video": None, "rejected": False,
                  "retryable": False,
                  "error": "Playwright is not installed (pip install playwright "
                           "&& playwright install chromium)"})
        return 1

    _log("worker: started, waiting for requests on stdin")
    context = None
    page = None
    try:
        playwright = sync_playwright().start()
    except Exception as exc:  # noqa: BLE001 - nothing can run without it
        _respond({"video": None, "rejected": False,
                  "error": f"Could not start Playwright: {exc}"})
        return 1
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
            if request.get("deleteConversation"):
                target_conv_url = request.get("conversationUrl") or ""
                req_settings = request.get("settings") or {}
                deleted = False
                if context is not None and target_conv_url and bool(req_settings.get("deleteChatOnFinish", True)):
                    try:
                        if page is None or page.is_closed():
                            page = context.pages[0] if context.pages else context.new_page()
                        deleted = _delete_current_chat(page, target_conv_url)
                    except Exception as exc:  # noqa: BLE001
                        _log(f"worker: deleteConversation failed: {exc}")
                _respond({"video": None, "error": None, "rejected": False, "deleted": deleted})
                continue
            if request.get("deleteConversations"):
                raw_urls = request.get("conversationUrls") or []
                urls = [str(u).strip() for u in raw_urls if isinstance(u, str) and str(u).strip()]
                req_settings = request.get("settings") or {}
                if not urls:
                    _respond({"video": None, "error": None, "rejected": False,
                              "deleted": [], "failed": []})
                    continue
                deleted_urls = []
                failed_urls = []
                err_msg = None
                try:
                    if context is None:
                        context, launch_note, launch_error = _launch_context(
                            playwright, bool(req_settings.get("useSystemChromeProfile", True)))
                        if context is None:
                            _respond({"video": None, "rejected": False,
                                      "deleted": [], "failed": urls,
                                      "error": "Could not launch a browser. If Chrome "
                                               "is currently open with your default "
                                               "profile, close it first. Details: "
                                               f"{launch_error}"})
                            continue
                        _log(f"worker: browser ready for chat cleanup ({launch_note})")
                    needs_ultra_account = bool(req_settings.get("requireUltra", True)) and any(
                        not re.search(r"/u/\d+/app/", u) for u in urls
                    )
                    page, prepare_error = _prepare_page(
                        context, needs_ultra_account, page, chat_mode="fresh")
                    if prepare_error:
                        _respond({"video": None, "rejected": False,
                                  "deleted": [], "failed": urls,
                                  "error": prepare_error})
                        continue
                    for target_url in urls:
                        try:
                            if page is None or page.is_closed():
                                page = context.pages[0] if context.pages else context.new_page()
                            if _delete_current_chat(page, target_url):
                                _remove_cached_chat(target_url)
                                deleted_urls.append(target_url)
                            else:
                                failed_urls.append(target_url)
                        except Exception as exc:  # noqa: BLE001
                            _log(f"worker: deleteConversations failed for {target_url}: {exc}")
                            failed_urls.append(target_url)
                            if _is_target_closed_error(exc):
                                err_msg = str(exc)
                                for rem in urls:
                                    if rem not in deleted_urls and rem not in failed_urls:
                                        failed_urls.append(rem)
                                try:
                                    if context is not None:
                                        context.close()
                                except Exception:
                                    pass
                                context = None
                                page = None
                                break
                except Exception as exc:  # noqa: BLE001
                    _log(f"worker: deleteConversations error: {exc}")
                    err_msg = str(exc)
                    for rem in urls:
                        if rem not in deleted_urls and rem not in failed_urls:
                            failed_urls.append(rem)
                _respond({"video": None, "error": err_msg, "rejected": False,
                          "deleted": deleted_urls, "failed": failed_urls})
                continue
            try:
                prompt, output_dir, images, settings = _read_request(request)
            except ValueError as exc:
                _respond({"video": None, "rejected": False, "error": str(exc)})
                continue

            chat_mode = request.get("chatMode") or settings.get("chatMode", "fresh")
            message_action = request.get("messageAction") or settings.get("messageAction", "new")
            conversation_url = request.get("conversationUrl") or settings.get("conversationUrl")
            if chat_mode == "conversation":
                saved = _saved_submission(output_dir)
                if (saved.get("promptSubmitted") and saved.get("conversationUrl")
                        and (not conversation_url or conversation_url == saved["conversationUrl"])):
                    conversation_url = saved["conversationUrl"]
                    message_action = "edit_last"

            _log(f"worker: request output={output_dir}, images={images}, "
                 f"chatMode={chat_mode}, action={message_action}, "
                 f"prompt={prompt[:150]!r}")
            try:
                if context is None:
                    # First request pays the launch + login + Ultra cost;
                    # every later one reuses this very browser and tab.
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
                    context, bool(settings.get("requireUltra", True)), page,
                    chat_mode=chat_mode, conversation_url=conversation_url)
                if prepare_error:
                    _respond({"video": None, "rejected": False, "error": prepare_error})
                    continue
                result, page = _generate_once(context, page, prompt, images,
                                              output_dir, settings,
                                              chat_mode=chat_mode,
                                              message_action=message_action)
                # Some failures come back as a normal result rather than an
                # exception (a download that could not be saved because the
                # browser died mid-way, say). The browser is just as dead —
                # drop it here too, otherwise the NEXT request is wasted
                # discovering the corpse itself.
                browser_lost = (result.get("browserLost", False)
                                or bool(result.get("error")
                                        and _is_target_closed_error(result["error"])))
                if browser_lost:
                    result["browserLost"] = True
                # A completed download may have been recovered from disk
                # after Chrome died. Keep that success; relaunch for the next
                # job without spending a request discovering the dead page.
                if browser_lost or page is None or page.is_closed():
                    try:
                        context.close()
                    except Exception:
                        pass
                    _log("worker: browser closed; next request will relaunch")
                    context = None
                    page = None
                elif result.get("error") and page and not page.is_closed():
                    # Open a fresh tab BEFORE closing the dirty page: closing the sole
                    # open tab in Chrome on Linux quits the entire browser process.
                    replacement = None
                    try:
                        if context is not None:
                            replacement = context.new_page()
                    except Exception:
                        replacement = None
                    try:
                        page.close()
                    except Exception:
                        pass
                    page = replacement
                _respond(result)
            except Exception as exc:  # noqa: BLE001 - one bad request must not
                # kill the worker: report it and stay alive for the next one.
                _log(f"worker: request failed: {exc}")
                if _is_target_closed_error(exc):
                    # The browser itself is gone — drop it so the NEXT
                    # request relaunches cleanly instead of failing forever.
                    # Closing it explicitly matters: a half-dead Chrome still
                    # holding the profile's SingletonLock would make every
                    # relaunch fail with "Opening in existing browser session".
                    try:
                        if context is not None:
                            context.close()
                    except Exception:
                        pass
                    _log("worker: browser lost, will relaunch on the next request")
                    context = None
                    page = None
                _respond({"video": None, "rejected": False, "error": str(exc),
                          "browserLost": _is_target_closed_error(exc)})
    finally:
        _log("worker: stdin closed, shutting the browser down")
        try:
            if page is not None and not page.is_closed():
                page_url = page.url if isinstance(getattr(page, "url", None), str) else ""
                if _extract_chat_id(page_url):
                    _delete_current_chat(page, page_url)
        except Exception:
            pass
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
        prompt, output_dir, image_paths, settings = _read_request({
            "promptFile": prompt_path, "outputDir": output_dir,
            "settings": settings, "images": sys.argv[4:]})
    except ValueError as exc:
        _emit(error=str(exc))
        return 1

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
                      rejected=result.get("rejected", False),
                      retryable=result.get("retryable", True),
                      quotaExceeded=result.get("quotaExceeded", False))
                return 0 if result["video"] else 1
            finally:
                context.close()
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())
