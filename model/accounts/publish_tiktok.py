#!/usr/bin/env python3
"""Publishes one video or image carousel to TikTok Studio via real Chrome.

Usage:
    publish_tiktok.py <request-json-file>

Request JSON keys:
    accountUrl:             e.g. "https://www.tiktok.com/@pradize.high.heel"
    mediaPaths:             list of absolute file paths (1 video OR 1..35 images)
    caption:                post caption including hashtags and qsXXXX code
    shortCode:              e.g. "QS-0004"
    useSystemChromeProfile: bool (default true)
    dryRun:                 bool (default false; stops right before clicking Post)
    uploadUrlOverride:      optional URL (e.g. file:// offline test fixture)

Diagnostics:
    Streams human-readable "[STAGE] ..." lines to stderr in real time so the
    Publishing tab's live log console shows exact progress and failure causes,
    and writes forensic logs + screenshots to ~/.cache/quantumsocial/publish_logs/.

Stdout contract (single JSON line on exit):
    {
      "published": bool,
      "dry_run": bool,
      "post_url": str,
      "log_file": str,
      "screenshot": str,
      "fatal": bool,
      "error": str | null
    }
"""

from __future__ import annotations

import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
from typing import Any, Dict, List, Optional, Tuple

TIKTOK_UPLOAD_URL = "https://www.tiktok.com/tiktokstudio/upload"
SYSTEM_CHROME_PROFILE = os.path.expanduser("~/.config/google-chrome")
CACHE_ROOT = os.path.expanduser("~/.cache/quantumsocial")
LOG_DIR = os.environ.get("QS_PUBLISH_LOG_DIR") or os.path.join(CACHE_ROOT, "publish_logs")

HANDLE_RE = re.compile(r"/@([A-Za-z0-9._-]+)")

VIDEO_EXTENSIONS = {".mp4", ".mov", ".webm", ".mkv", ".avi"}
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp"}


class Logger:
    def __init__(self, short_code: str) -> None:
        os.makedirs(LOG_DIR, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        safe_code = re.sub(r"[^A-Za-z0-9_-]", "", short_code or "post")
        self.base_path = os.path.join(LOG_DIR, f"tiktok_{stamp}_{safe_code}")
        self.log_file = self.base_path + ".log"
        self.chrome_log_file = self.base_path + "_chrome.log"

    def emit_log(self, stage: str, message: str) -> None:
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        line = f"[{ts}] [{stage}] {message}"
        try:
            sys.stderr.write(line + "\n")
            sys.stderr.flush()
        except Exception:
            pass
        try:
            with open(self.log_file, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
        except OSError:
            pass

    def dump_page(self, page: Any, tag: str) -> str:
        snap_path = f"{self.base_path}_{tag}.png"
        txt_path = f"{self.base_path}_{tag}.txt"
        try:
            page.screenshot(path=snap_path, full_page=False)
            self.emit_log("DEBUG-DUMP", f"Saved screenshot: {snap_path}")
        except Exception as exc:
            snap_path = ""
            self.emit_log("DEBUG-DUMP", f"Screenshot failed ({tag}): {exc}")

        try:
            diag = page.evaluate(
                """() => ({
                    url: location.href,
                    title: document.title,
                    fileInputs: Array.from(document.querySelectorAll('input[type="file"]')).map(i => ({
                        accept: i.accept, multiple: i.multiple, id: i.id, className: i.className
                    })),
                    editors: Array.from(document.querySelectorAll('[contenteditable="true"], textarea')).map(e => ({
                        tag: e.tagName, e2e: e.getAttribute('data-e2e'), cls: e.className,
                        textLen: (e.innerText || e.value || '').length
                    })),
                    buttons: Array.from(document.querySelectorAll('button, [role="button"]')).slice(0, 50).map(b => ({
                        text: (b.innerText || '').trim().slice(0, 60),
                        e2e: b.getAttribute('data-e2e'),
                        disabled: b.disabled || b.getAttribute('aria-disabled') === 'true'
                    })).filter(b => b.text || b.e2e),
                    bodyText: (document.body ? document.body.innerText : '').slice(0, 8000)
                })"""
            )
            with open(txt_path, "w", encoding="utf-8") as fh:
                json.dump(diag, fh, indent=2, ensure_ascii=False)
            self.emit_log("DEBUG-DUMP", f"Saved DOM state dump: {txt_path}")
        except Exception as exc:
            self.emit_log("DEBUG-DUMP", f"DOM state dump failed ({tag}): {exc}")
        return snap_path


def _finish(
    published: bool = False,
    dry_run: bool = False,
    post_url: str = "",
    sound_title: str = "",
    video_text_applied: bool = False,
    video_text_style: str = "",
    log_file: str = "",
    screenshot: str = "",
    fatal: bool = False,
    error: Optional[str] = None,
) -> int:
    payload = {
        "published": published,
        "dry_run": dry_run,
        "post_url": post_url,
        "sound_title": sound_title,
        "video_text_applied": video_text_applied,
        "video_text_style": video_text_style,
        "log_file": log_file,
        "screenshot": screenshot,
        "fatal": fatal,
        "error": error,
    }
    print(json.dumps(payload, ensure_ascii=False), flush=True)
    return 0


_DANGLING_END_WORDS = {
    "a", "an", "the", "this", "these", "that", "those", "my", "your", "our", "her", "his", "its",
    "in", "on", "of", "to", "for", "with", "at", "by", "from", "into",
    "le", "la", "les", "un", "une", "des", "du", "de", "d", "ce", "ces", "cet", "cette",
    "mon", "ma", "mes", "ton", "ta", "tes", "son", "sa", "ses", "en", "au", "aux",
}
_CLAUSE_START_WORDS = {
    "in", "on", "with", "for", "when", "until", "while", "from", "into", "that", "but", "and", "or", "to", "before", "after",
    "pour", "avec", "dans", "sans", "quand", "mais", "ou", "sur", "chez", "avant", "après",
}


def _wrap_single_paragraph_strategically(paragraph: str, max_line_chars: int = 24) -> str:
    cleaned = re.sub(r"\s+", " ", (paragraph or "").strip())
    if not cleaned:
        return ""
    words = cleaned.split(" ")
    n = len(words)
    if len(cleaned) <= 18 or n <= 2:
        return cleaned

    if len(cleaned) <= 44:
        k_candidates = [2, 3]
    elif len(cleaned) <= 68:
        k_candidates = [3, 2, 4]
    else:
        k_candidates = [3, 4]
    k_candidates = [k for k in k_candidates if 1 <= k <= n] or [min(n, 3)]

    best_overall_cost = float("inf")
    best_overall_lines: List[str] = [cleaned]

    for k in k_candidates:
        ideal_len = len(cleaned) / float(k)
        # dp[(idx, used)] = (cost, lines_list)
        dp: Dict[Tuple[int, int], Tuple[float, List[str]]] = {(0, 0): (0.0, [])}
        for used in range(k):
            for i in range(n):
                state = dp.get((i, used))
                if state is None:
                    continue
                base_cost, prev_lines = state
                for j in range(i + 1, n - (k - used - 1) + 1):
                    if used == k - 1 and j != n:
                        continue
                    seg_words = words[i:j]
                    seg = " ".join(seg_words)
                    seg_len = len(seg)
                    is_last = (j == n)

                    cost = (seg_len - ideal_len) ** 2 * 2.5
                    if seg_len > max_line_chars:
                        cost += 900.0 + ((seg_len - max_line_chars) ** 2) * 38.0
                    if len(seg_words) == 1 and seg_len <= 6 and n >= 4:
                        cost += 450.0  # avoid awkward 1-word orphan lines

                    if not is_last:
                        last_w = seg_words[-1]
                        last_clean = re.sub(r"[^A-Za-zÀ-ÿ0-9']+", "", last_w).lower().split("'")[-1]
                        next_clean = re.sub(r"[^A-Za-zÀ-ÿ0-9]+", "", words[j]).lower()
                        if re.search(r"[,;:—\-?!.]$", last_w):
                            cost -= 85.0
                        if last_clean in _DANGLING_END_WORDS:
                            cost += 145.0
                        if next_clean in _CLAUSE_START_WORDS:
                            cost -= 35.0

                    total_cost = base_cost + cost
                    nxt = dp.get((j, used + 1))
                    if nxt is None or total_cost < nxt[0]:
                        dp[(j, used + 1)] = (total_cost, prev_lines + [seg])

        final_state = dp.get((n, k))
        if final_state is not None:
            # Slight preference for fewer lines when all lines fit comfortably
            k_penalty = (k - 2) * 30.0 if k >= 2 else 0.0
            if final_state[0] + k_penalty < best_overall_cost:
                best_overall_cost = final_state[0] + k_penalty
                best_overall_lines = final_state[1]

    return "\n".join(best_overall_lines)


def _wrap_tiktok_overlay_text(raw: str, max_line_chars: int = 24) -> str:
    text = (raw or "").strip()
    if not text:
        return ""
    if "\n" in text:
        explicit_lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
        out: List[str] = []
        for ln in explicit_lines:
            out.append(
                _wrap_single_paragraph_strategically(ln, max_line_chars)
                if len(ln) > max_line_chars
                else ln
            )
        return "\n".join(out)
    return _wrap_single_paragraph_strategically(text, max_line_chars)


def _probe_video_duration_secs(video_path: str) -> float:
    ffprobe_bin = shutil.which("ffprobe")
    if not ffprobe_bin or not os.path.isfile(video_path):
        return 0.0
    try:
        proc = subprocess.run(
            [
                ffprobe_bin,
                "-v", "error",
                "-show_entries", "format=duration",
                "-of", "default=noprint_wrappers=1:nokey=1",
                video_path,
            ],
            capture_output=True,
            text=True,
            timeout=5,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            return max(0.0, float(proc.stdout.strip()))
    except Exception:
        pass
    return 0.0


def _strip_video_audio_copy(video_path: str, logger: Logger) -> Optional[str]:
    """Creates a lossless audio-free (-an) temporary copy of video_path so original video sound is 100% removed."""
    ffmpeg_bin = shutil.which("ffmpeg")
    if not ffmpeg_bin:
        logger.emit_log("SOUND", "ffmpeg not found on PATH; relying on in-browser original-track mute.")
        return None
    ext = os.path.splitext(video_path)[1].lower() or ".mp4"
    fd, out_path = tempfile.mkstemp(prefix="qs-muted-", suffix=ext)
    os.close(fd)
    try:
        proc = subprocess.run(
            [ffmpeg_bin, "-y", "-loglevel", "error", "-i", video_path, "-c:v", "copy", "-an", out_path],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if proc.returncode == 0 and os.path.isfile(out_path) and os.path.getsize(out_path) > 0:
            logger.emit_log("SOUND", f"Stripped original video audio track cleanly (-an) -> {os.path.basename(out_path)}")
            return out_path
        logger.emit_log("SOUND", f"ffmpeg -an returned {proc.returncode}: {(proc.stderr or '').strip()[:160]}")
    except Exception as exc:
        logger.emit_log("SOUND", f"Could not strip video audio with ffmpeg: {exc}")
    try:
        os.remove(out_path)
    except OSError:
        pass
    return None


VALID_VIDEO_TEXT_STYLES = {
    "classic_black_pill",
    "classic_white_pill",
    "elegance_serif",
    "classic_clean_stroke",
}


def _normalize_video_text_style(style_id: str) -> str:
    s = (style_id or "").strip().lower()
    return s if s in VALID_VIDEO_TEXT_STYLES else "classic_black_pill"


def _pick_open_color_swatch(surface: Any, want_dark: bool) -> bool:
    """Clicks either the darkest (black) or brightest (white) swatch in an open PropSettingColorPicker popup."""
    try:
        return bool(
            surface.evaluate(
                """(wantDark) => {
                    const blocks = Array.from(document.querySelectorAll('[class*="PropSettingColorCard__presetItemBlock"]'));
                    let best = null;
                    let bestScore = wantDark ? 9999 : -1;
                    for (const b of blocks) {
                        const r = b.getBoundingClientRect();
                        if (r.width <= 0 || r.height <= 0) continue;
                        const bg = window.getComputedStyle(b).backgroundColor || '';
                        const m = bg.match(/rgba?\\((\\d+),\\s*(\\d+),\\s*(\\d+)/);
                        if (!m) continue;
                        const lum = parseInt(m[1], 10) + parseInt(m[2], 10) + parseInt(m[3], 10);
                        if (wantDark && lum < bestScore) {
                            bestScore = lum;
                            best = b;
                        } else if (!wantDark && lum > bestScore) {
                            bestScore = lum;
                            best = b;
                        }
                    }
                    if (!best) return false;
                    best.click();
                    return true;
                }""",
                want_dark,
            )
        )
    except Exception:
        return False


def _apply_video_text_inside_editor(
    surface: Any,
    page: Any,
    overlay_text: str,
    style_id: str,
    headless: bool,
    logger: Logger,
) -> Tuple[bool, str]:
    """Adds a native TikTok text track inside the open TikTok Studio video editor, styled and moved to the upper safe zone."""
    effective_style = _normalize_video_text_style(style_id)
    wrapped_text = _wrap_tiktok_overlay_text(overlay_text, max_line_chars=24)
    if not wrapped_text:
        return False, ""

    # Switch to the 'Texte / Text' tab inside the open editor if 'Ajouter du texte' isn't already visible
    add_btn_loc = surface.locator('[class*="AddTextPanel__addTextBasicButton"]')
    try:
        if not (add_btn_loc.count() > 0 and add_btn_loc.first.is_visible()):
            switched = bool(
                surface.evaluate(
                    """() => {
                        const menuItems = Array.from(document.querySelectorAll('[class*="EditorLeftSideMenu__menuItem"], button, [role="tab"]'));
                        const item = menuItems.find(el => {
                            const txt = (el.innerText || '').trim();
                            const r = el.getBoundingClientRect();
                            return /^(texte|text)$/i.test(txt) && r.width > 0 && r.height > 0 && r.x < 120;
                        });
                        if (!item) return false;
                        item.click();
                        return true;
                    }"""
                )
            )
            if switched:
                page.wait_for_timeout(1200 if not headless else 120)
    except Exception as exc:
        logger.emit_log("VIDEO-TEXT", f"Could not switch to 'Texte' editor tab: {exc}")

    # Wait for 'Ajouter du texte / Add text' button to become visible
    add_ready = False
    deadline = time.monotonic() + (8.0 if not headless else 2.0)
    while time.monotonic() < deadline:
        try:
            if add_btn_loc.count() > 0 and add_btn_loc.first.is_visible():
                add_ready = True
                break
        except Exception:
            pass
        page.wait_for_timeout(350 if not headless else 80)

    if not add_ready:
        logger.emit_log("VIDEO-TEXT", "TikTok 'AddTextPanel__addTextBasicButton' did not appear; skipping video text track.")
        return False, ""

    try:
        add_btn_loc.first.click(timeout=4000)
    except Exception as exc:
        logger.emit_log("VIDEO-TEXT", f"Could not click 'Ajouter du texte': {exc}")
        return False, ""

    # Wait for the right-panel PropSettingTextArea__input to become visible (confirming the new text track is active!)
    ta_loc = surface.locator('textarea[class*="PropSettingTextArea__input"]')
    ta_ready = False
    ta_deadline = time.monotonic() + (6.0 if not headless else 1.5)
    while time.monotonic() < ta_deadline:
        try:
            if ta_loc.count() > 0 and ta_loc.first.is_visible():
                ta_ready = True
                break
        except Exception:
            pass
        page.wait_for_timeout(250 if not headless else 60)

    if not ta_ready:
        logger.emit_log("VIDEO-TEXT", "Text property textarea did not mount after clicking 'Ajouter du texte'.")
        return False, ""

    # 1. FIRST fill PropSettingTextArea__input so the text box expands to its full horizontal multi-word width!
    #    (Never drag BEFORE filling, because when the box only holds 'Text' its side resize handles sit right
    #     next to the center and can pinch the box width down to 1 character!)
    try:
        ta = surface.locator('textarea[class*="PropSettingTextArea__input"]').first
        ta.click(timeout=3000)
        ta.fill(wrapped_text)
        page.wait_for_timeout(500 if not headless else 60)
    except Exception as exc:
        logger.emit_log("VIDEO-TEXT", f"Failed filling video overlay text input: {exc}")
        return False, ""

    # 2. Compute the true interior center of the expanded text box from .moveable-line edges
    #    (guaranteed far inside the box, well away from left/right/corner resize handles!)
    #    and drag smoothly up to 20% height of StageCanvas__screen (top safe zone above heels/legs).
    try:
        coords = surface.evaluate(
            """() => {
                const lines = Array.from(document.querySelectorAll('.moveable-control-box .moveable-line'))
                    .filter(el => {
                        const r = el.getBoundingClientRect();
                        return (r.width > 10 || r.height > 10) && !String(el.className || '').includes('rotation');
                    });
                const stage = document.querySelector('[class*="StageCanvas__screen"]');
                if (lines.length < 2 || !stage) return null;
                let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
                for (const l of lines) {
                    const r = l.getBoundingClientRect();
                    minX = Math.min(minX, r.left);
                    maxX = Math.max(maxX, r.right);
                    minY = Math.min(minY, r.top);
                    maxY = Math.max(maxY, r.bottom);
                }
                const boxW = maxX - minX;
                const boxH = maxY - minY;
                if (boxW < 45 || boxH < 14) return null;
                const sr = stage.getBoundingClientRect();
                if (sr.height < 120 || sr.width < 80) return null;
                return {
                    boxW: Math.round(boxW),
                    boxH: Math.round(boxH),
                    fromX: (minX + maxX) * 0.5,
                    fromY: (minY + maxY) * 0.5,
                    toX: sr.x + sr.width * 0.5,
                    toY: sr.y + sr.height * 0.20
                };
            }"""
        )
        if coords:
            page.mouse.move(coords["fromX"], coords["fromY"])
            page.mouse.down()
            page.mouse.move(coords["toX"], coords["toY"], steps=12)
            page.mouse.up()
            page.wait_for_timeout(350 if not headless else 50)
            logger.emit_log(
                "VIDEO-TEXT",
                f"Positioned horizontal text box ({coords['boxW']}x{coords['boxH']}px) into upper safe zone.",
            )
    except Exception as exc:
        logger.emit_log("VIDEO-TEXT", f"Note: could not reposition text box on stage ({exc}); keeping centered position.")

    # 3. Apply the chosen curated fashion-safe TikTok text style:
    #    - classic_black_pill:   Classic + Bold + White text + Black rounded pill ('Arrière-plan')
    #    - classic_white_pill:   Classic + Bold + Dark text  + White rounded pill ('Arrière-plan')
    #    - elegance_serif:       Elegance serif + Bold + White text + Stroke ('Trait') + Shadow ('Ombre')
    #    - classic_clean_stroke: Classic + Bold + White text + Clean dark Stroke ('Trait'), no box
    try:
        if effective_style == "elegance_serif":
            trig = surface.locator('[class*="PropSettingTextFontFamily__trigger"]')
            if trig.count() > 0 and trig.first.is_visible():
                trig.first.click(timeout=2500)
                page.wait_for_timeout(350 if not headless else 50)
                surface.evaluate(
                    """() => {
                        const opts = Array.from(document.querySelectorAll('[role="option"], [class*="Select__option"], [class*="Popup"] div'));
                        const el = opts.find(o => /^(elegance|serif|postcard)$/i.test((o.innerText || '').trim()));
                        if (el) el.click();
                    }"""
                )
                page.wait_for_timeout(250 if not headless else 40)
                page.keyboard.press("Escape")

        surface.evaluate(
            """() => {
                const fontStyleWrap = document.querySelector('[class*="PropSettingText__fontStyle"]');
                if (fontStyleWrap) {
                    const boldBtn = fontStyleWrap.querySelector('button, div[role="button"], span');
                    if (boldBtn) boldBtn.click();
                }
            }"""
        )
        page.wait_for_timeout(150 if not headless else 30)

        if effective_style == "classic_white_pill":
            fill_trig = surface.locator('[class*="PropSettingTextFontColor__wrap"] [class*="PropSettingColorPicker__trigger"]')
            if fill_trig.count() > 0 and fill_trig.first.is_visible():
                fill_trig.first.click(timeout=2500)
                page.wait_for_timeout(300 if not headless else 40)
                _pick_open_color_swatch(surface, want_dark=True)
                page.keyboard.press("Escape")
                page.wait_for_timeout(150 if not headless else 30)
            opened_bg = bool(
                surface.evaluate(
                    """() => {
                        const labelGroup = document.querySelector('[class*="PropSettingText__labelGroup"]');
                        if (!labelGroup) return false;
                        const addBtns = labelGroup.querySelectorAll('[class*="PropSettingColorPopup__addBtn"]');
                        if (addBtns.length >= 2) { addBtns[1].click(); return true; }
                        return false;
                    }"""
                )
            )
            if opened_bg:
                page.wait_for_timeout(300 if not headless else 40)
                bg_triggers = surface.locator('[class*="PropSettingText__labelGroup"] [class*="PropSettingColorPicker__trigger"]')
                if bg_triggers.count() >= 2 and bg_triggers.nth(1).is_visible():
                    bg_triggers.nth(1).click(timeout=2500)
                    page.wait_for_timeout(300 if not headless else 40)
                _pick_open_color_swatch(surface, want_dark=False)
                page.keyboard.press("Escape")
        elif effective_style == "elegance_serif":
            surface.evaluate(
                """() => {
                    const labelGroup = document.querySelector('[class*="PropSettingText__labelGroup"]');
                    if (!labelGroup) return;
                    const addBtns = labelGroup.querySelectorAll('[class*="PropSettingColorPopup__addBtn"]');
                    if (addBtns.length >= 1) addBtns[0].click();
                    if (addBtns.length >= 3) addBtns[2].click();
                }"""
            )
            page.wait_for_timeout(200 if not headless else 40)
            page.keyboard.press("Escape")
        elif effective_style == "classic_clean_stroke":
            surface.evaluate(
                """() => {
                    const labelGroup = document.querySelector('[class*="PropSettingText__labelGroup"]');
                    if (!labelGroup) return;
                    const addBtns = labelGroup.querySelectorAll('[class*="PropSettingColorPopup__addBtn"]');
                    if (addBtns.length >= 1) addBtns[0].click();
                }"""
            )
            page.wait_for_timeout(200 if not headless else 40)
            page.keyboard.press("Escape")
        else:
            surface.evaluate(
                """() => {
                    const labelGroup = document.querySelector('[class*="PropSettingText__labelGroup"]');
                    if (!labelGroup) return;
                    const addBtns = labelGroup.querySelectorAll('[class*="PropSettingColorPopup__addBtn"]');
                    if (addBtns.length >= 2) addBtns[1].click();
                    else if (addBtns.length >= 1) addBtns[0].click();
                }"""
            )
            page.wait_for_timeout(200 if not headless else 40)
            page.keyboard.press("Escape")
    except Exception as exc:
        logger.emit_log("VIDEO-TEXT", f"Note: style customization warning ({effective_style}): {exc}")

    # 4. Guardrail: verify the text box was NOT squished into a narrow vertical column before saving!
    try:
        squished = bool(
            surface.evaluate(
                """() => {
                    const lines = Array.from(document.querySelectorAll('.moveable-control-box .moveable-line'))
                        .filter(el => {
                            const r = el.getBoundingClientRect();
                            return (r.width > 5 || r.height > 5) && !String(el.className || '').includes('rotation');
                        });
                    if (lines.length < 2) return false;
                    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
                    for (const l of lines) {
                        const r = l.getBoundingClientRect();
                        minX = Math.min(minX, r.left);
                        maxX = Math.max(maxX, r.right);
                        minY = Math.min(minY, r.top);
                        maxY = Math.max(maxY, r.bottom);
                    }
                    const w = maxX - minX;
                    const h = maxY - minY;
                    return w < 45 && h > w * 2.0;
                }"""
            )
        )
        if squished:
            logger.emit_log("VIDEO-TEXT", "Detected narrow vertical text box anomaly — undoing text track rather than saving deformed text.")
            page.keyboard.press("Delete")
            return False, ""
    except Exception:
        pass

    single_line_log = wrapped_text.replace("\n", " / ")
    logger.emit_log(
        "VIDEO-TEXT",
        f"Added native TikTok video text track (style='{effective_style}') in upper safe zone: '{single_line_log}'",
    )
    return True, effective_style


def _apply_video_editor_sound_and_text(
    surface: Any,
    page: Any,
    mute_original: bool,
    use_suggested_sound: bool,
    video_overlay_text: str,
    video_text_style: str,
    headless: bool,
    logger: Logger,
) -> Tuple[str, bool, str]:
    """Opens TikTok Studio's video editor once, applies #1 suggested sound and/or native 'Aa Texte' overlay, and clicks Save once."""
    clean_overlay = (video_overlay_text or "").strip()
    if not use_suggested_sound and not clean_overlay:
        return "", False, ""

    target_btn_pattern = "^(sons|sounds)$" if use_suggested_sound else "^(texte|text)$"
    clicked_entry = False
    try:
        clicked_entry = bool(
            surface.evaluate(
                """(pat) => {
                    const re = new RegExp(pat, 'i');
                    const candidates = Array.from(document.querySelectorAll('button.editor-entrance, button'));
                    const btn = candidates.find(b => {
                        const txt = (b.innerText || '').trim();
                        const r = b.getBoundingClientRect();
                        return re.test(txt) && r.width > 0 && r.height > 0 && r.x > 250;
                    }) || candidates.find(b => b.classList.contains('editor-entrance') && re.test((b.innerText || '').trim()));
                    if (!btn) return false;
                    btn.scrollIntoView({ block: 'center' });
                    btn.click();
                    return true;
                }""",
                target_btn_pattern,
            )
        )
    except Exception as exc:
        logger.emit_log("SOUND", f"Could not click TikTok Studio video editor button: {exc}")
        return "", False, ""

    if not clicked_entry:
        logger.emit_log("SOUND", "No 'Sons / Texte' video editor button visible on this layout; skipping editor step.")
        return "", False, ""

    chosen_title = ""
    if use_suggested_sound:
        logger.emit_log("SOUND", "Opened TikTok Studio 'Sons / Sounds' panel; loading suggested sounds...")
        music_ready = False
        deadline = time.monotonic() + (20.0 if not headless else 4.0)
        while time.monotonic() < deadline:
            _dismiss_studio_popups(surface, page, logger)
            try:
                if surface.locator('[class*="MusicPanelMusicItem__wrap"]').count() > 0:
                    music_ready = True
                    break
            except Exception:
                pass
            page.wait_for_timeout(600 if not headless else 100)

        if music_ready:
            _dismiss_studio_popups(surface, page, logger)
            if mute_original:
                try:
                    muted_in_ui = surface.evaluate(
                        """() => {
                            const trackMuteBtns = Array.from(document.querySelectorAll('button')).filter(b => {
                                const r = b.getBoundingClientRect();
                                return r.width >= 20 && r.width <= 44 && r.x > 20 && r.x < 180 && r.y > 760;
                            });
                            if (trackMuteBtns.length === 1) {
                                trackMuteBtns[0].click();
                                return true;
                            }
                            return false;
                        }"""
                    )
                    if muted_in_ui:
                        logger.emit_log("SOUND", "Muted original video audio track in TikTok Studio timeline.")
                        page.wait_for_timeout(300 if not headless else 50)
                except Exception:
                    pass

            try:
                items = surface.locator('[class*="MusicPanelMusicItem__wrap"]')
                item_count = items.count()
                for idx in range(min(item_count, 5)):
                    item = items.nth(idx)
                    if not item.is_visible():
                        continue
                    raw_txt = (item.inner_text() or "").strip()
                    title_line = raw_txt.splitlines()[0].strip() if raw_txt else f"Suggested Sound #{idx + 1}"
                    item.hover(timeout=2500)
                    page.wait_for_timeout(350 if not headless else 50)
                    plus_btn = item.locator('button[class*="type-primary"], button').last
                    if plus_btn.count() > 0 and plus_btn.is_visible() and plus_btn.is_enabled():
                        logger.emit_log("SOUND", f"Selecting #{idx + 1} suggested TikTok sound: '{title_line}'...")
                        plus_btn.click(timeout=4000)
                        chosen_title = title_line
                        break
            except Exception as exc:
                logger.emit_log("SOUND", f"Failed to click '+' on suggested sound: {exc}")
        else:
            logger.emit_log("SOUND", "Suggested music list did not appear in time.")

    text_applied = False
    applied_style = ""
    if clean_overlay:
        page.wait_for_timeout(800 if not headless else 80)
        _dismiss_studio_popups(surface, page, logger)
        text_applied, applied_style = _apply_video_text_inside_editor(
            surface, page, clean_overlay, video_text_style, headless, logger
        )

    if not chosen_title and not text_applied:
        logger.emit_log("SOUND", "Neither sound nor text track changed; closing editor without saving.")
        try:
            surface.evaluate(
                """() => {
                    const cancelBtn = Array.from(document.querySelectorAll('button'))
                        .find(b => /^(annuler|cancel)$/i.test((b.innerText || '').trim()));
                    if (cancelBtn) cancelBtn.click();
                }"""
            )
        except Exception:
            pass
        return "", False, ""

    page.wait_for_timeout(2000 if not headless else 150)
    _dismiss_studio_popups(surface, page, logger)

    saved = False
    try:
        saved = bool(
            surface.evaluate(
                """() => {
                    const saveBtn = Array.from(document.querySelectorAll('button'))
                        .find(b => /^(enregistrer|save)$/i.test((b.innerText || '').trim()) && !b.disabled);
                    if (!saveBtn) return false;
                    saveBtn.click();
                    return true;
                }"""
            )
        )
    except Exception as exc:
        logger.emit_log("SOUND", f"Could not click 'Enregistrer / Save' in video editor: {exc}")

    if saved:
        close_deadline = time.monotonic() + (40.0 if not headless else 5.0)
        while time.monotonic() < close_deadline:
            try:
                if (
                    surface.locator('[class*="MusicPanelContainer__root"]').count() == 0
                    and surface.locator('[class*="AddTextPanel__addTextBasicButton"]').count() == 0
                ):
                    break
            except Exception:
                break
            page.wait_for_timeout(700 if not headless else 100)
        page.wait_for_timeout(1200 if not headless else 100)
        logger.emit_log(
            "SOUND",
            f"Saved TikTok Studio video edits (sound='{chosen_title or 'none'}', videoText={text_applied}, style='{applied_style or 'none'}').",
        )
        return chosen_title, text_applied, applied_style

    return "", False, ""


def _apply_suggested_tiktok_sound(
    surface: Any,
    page: Any,
    mute_original: bool,
    headless: bool,
    logger: Logger,
) -> str:
    sound_title, _, _ = _apply_video_editor_sound_and_text(
        surface, page, mute_original, True, "", "", headless, logger
    )
    return sound_title


def _apply_suggested_photo_sound(
    surface: Any,
    page: Any,
    headless: bool,
    logger: Logger,
) -> str:
    """Clicks '+ Ajouter un son / Add sound' on TikTok Photo Mode and attaches the #1 suggested TikTok sound."""
    clicked_add = False
    try:
        clicked_add = bool(
            surface.evaluate(
                """() => {
                    const btn = Array.from(document.querySelectorAll('button, [role="button"]'))
                        .find(b => {
                            const txt = (b.innerText || '').trim();
                            const r = b.getBoundingClientRect();
                            return /ajouter un son|add sound/i.test(txt) && r.width > 0 && r.height > 0 && r.x > 220;
                        });
                    if (!btn) return false;
                    btn.scrollIntoView({ block: 'center' });
                    btn.click();
                    return true;
                }"""
            )
        )
    except Exception as exc:
        logger.emit_log("SOUND", f"Could not click '+ Ajouter un son / Add sound' on Photo Mode: {exc}")
        return ""

    if not clicked_add:
        logger.emit_log("SOUND", "No '+ Ajouter un son / Add sound' button visible on Photo Mode; trying editor 'Sons' fallback...")
        return _apply_suggested_tiktok_sound(surface, page, False, headless, logger)

    logger.emit_log("SOUND", "Opened TikTok Photo Mode sound picker; loading #1 suggested sound...")
    picker_ready = False
    deadline = time.monotonic() + (18.0 if not headless else 4.0)
    while time.monotonic() < deadline:
        _dismiss_studio_popups(surface, page, logger)
        try:
            if surface.locator('[class*="MusicPickerView__addButton"], [class*="MusicPanelMusicItem__wrap"]').count() > 0:
                picker_ready = True
                break
        except Exception:
            pass
        page.wait_for_timeout(500 if not headless else 100)

    if not picker_ready:
        logger.emit_log("SOUND", "Photo Mode sound list did not load in time; continuing.")
        return ""

    chosen_title = ""
    try:
        picked = surface.evaluate(
            """() => {
                const useBtn = document.querySelector('[class*="MusicPickerView__addButton"]');
                if (useBtn && !useBtn.disabled) {
                    let row = useBtn.parentElement;
                    while (row && row.parentElement && (row.innerText || '').split('\\n').length < 2) {
                        row = row.parentElement;
                    }
                    const lines = ((row ? row.innerText : '') || '').split('\\n').map(s => s.trim()).filter(Boolean);
                    const title = lines.find(l => !/^(use|utiliser)$/i.test(l)) || 'Suggested TikTok Sound #1';
                    useBtn.click();
                    return title;
                }
                const wrap = document.querySelector('[class*="MusicPanelMusicItem__wrap"]');
                if (wrap) {
                    const title = ((wrap.innerText || '').split('\\n')[0] || 'Suggested TikTok Sound #1').trim();
                    const btns = wrap.querySelectorAll('button');
                    if (btns.length > 0) {
                        btns[btns.length - 1].click();
                        return title;
                    }
                }
                return '';
            }"""
        )
        if picked:
            chosen_title = str(picked).strip()
    except Exception as exc:
        logger.emit_log("SOUND", f"Could not click 'Use' on Photo Mode #1 sound: {exc}")

    if chosen_title:
        page.wait_for_timeout(1500 if not headless else 120)
        # Close MusicPickerView modal if it stayed open after clicking Use
        try:
            surface.evaluate(
                """() => {
                    const picker = document.querySelector('[class*="MusicPickerView__root"]');
                    if (picker) {
                        const hdrBtns = picker.querySelectorAll('[class*="MusicPickerView__header"] button, button[aria-label="Close"]');
                        if (hdrBtns.length > 0) hdrBtns[hdrBtns.length - 1].click();
                    }
                }"""
            )
        except Exception:
            pass
        page.wait_for_timeout(600 if not headless else 80)
        logger.emit_log("SOUND", f"Attached #1 suggested TikTok sound to Photo Slideshow: '{chosen_title}'.")
    return chosen_title


def _extract_expected_handle(account_url: str) -> str:
    m = HANDLE_RE.search(account_url or "")
    return m.group(1).lower() if m else ""


def _active_lock_pid(profile_dir: str) -> Optional[int]:
    lock_path = os.path.join(profile_dir, "SingletonLock")
    if not os.path.lexists(lock_path):
        return None
    try:
        target = os.readlink(lock_path)
        m = re.search(r"-([0-9]+)$", target)
        if m:
            pid = int(m.group(1))
            os.kill(pid, 0)
            return pid
    except OSError:
        return None
    return None


def _launch_browser(playwright: Any, expected_handle: str, use_system_profile: bool, headless: bool, logger: Logger):
    base_kwargs: dict[str, Any] = {
        "headless": headless,
        "viewport": {"width": 1380, "height": 920},
        "ignore_default_args": ["--enable-automation"],
        "args": [
            "--disable-blink-features=AutomationControlled",
            "--no-first-run",
            "--no-default-browser-check",
            "--enable-logging",
            f"--log-file={logger.chrome_log_file}",
        ],
    }

    if headless:
        tmp_dir = tempfile.mkdtemp(prefix="qs-tiktok-pub-test-")
        ctx = playwright.chromium.launch_persistent_context(tmp_dir, **base_kwargs)
        logger.emit_log("BROWSER", "Launched headless test browser context.")
        return ctx, tmp_dir

    safe_handle = re.sub(r"[^A-Za-z0-9._-]", "_", expected_handle or "default")
    dedicated_dir = os.path.join(CACHE_ROOT, f"publish-tiktok-{safe_handle}")
    legacy_tiktok_dir = os.path.join(CACHE_ROOT, "tiktok-profile")
    if not os.path.exists(dedicated_dir) and os.path.isdir(legacy_tiktok_dir) and _active_lock_pid(legacy_tiktok_dir) is None:
        try:
            os.rename(legacy_tiktok_dir, dedicated_dir)
            logger.emit_log("BROWSER", f"Migrated existing TikTok session to dedicated @{expected_handle or 'tiktok'} profile: {dedicated_dir}")
        except OSError:
            pass

    # If the user opened Chrome via the "Reconnect" button and left it open, wait politely for them to close it
    lock_pid = _active_lock_pid(dedicated_dir)
    if lock_pid is not None:
        logger.emit_log(
            "BROWSER",
            f"Your Reconnect Chrome window for @{expected_handle or 'tiktok'} (PID {lock_pid}) is still open — "
            "close that Chrome window when you're done logging in/switching accounts, and Auto-Publish will continue automatically...",
        )
        while _active_lock_pid(dedicated_dir) is not None:
            time.sleep(1.0)
        time.sleep(0.5)

    # Note: Chrome 136+ explicitly rejects DevTools automation on the default
    # ~/.config/google-chrome directory ("DevTools remote debugging requires a
    # non-default data directory"), so each TikTok @handle gets its own isolated
    # persistent Chrome profile directory that never mixes multiple accounts.
    attempts: list[tuple[str, Optional[str], str]] = [
        (dedicated_dir, "chrome", f"Google Chrome (dedicated @{expected_handle or 'tiktok'} profile: {dedicated_dir})"),
        (dedicated_dir, None, f"Playwright Chromium (dedicated @{expected_handle or 'tiktok'} profile: {dedicated_dir})"),
    ]

    errors: list[str] = []
    for user_dir, channel, label in attempts:
        kwargs = dict(base_kwargs)
        if channel:
            kwargs["channel"] = channel
        try:
            os.makedirs(user_dir, exist_ok=True)
            logger.emit_log("BROWSER", f"Launching {label}...")
            ctx = playwright.chromium.launch_persistent_context(user_dir, **kwargs)
            logger.emit_log("BROWSER", f"Launched {label} successfully.")
            return ctx, None
        except Exception as exc:
            logger.emit_log("BROWSER", f"Could not launch {label}: {exc}")
            errors.append(f"{label}: {exc}")

    raise RuntimeError("Failed to launch browser: " + " | ".join(errors))


DETECT_GATE_JS = r"""
() => {
    const url = (location.href || '').toLowerCase();
    if (url.includes('/login') || url.includes('passport.tiktok.com')) {
        return { kind: 'login', reason: 'TikTok login page (' + location.pathname + ')' };
    }
    if (url.includes('tiktok.com/verify') || url.includes('secsdk')) {
        return { kind: 'captcha', reason: 'TikTok verification URL' };
    }

    const vis = (el, minW = 120, minH = 40) => {
        if (!el) return false;
        const st = window.getComputedStyle(el);
        if (!st || st.display === 'none' || st.visibility === 'hidden' || parseFloat(st.opacity) < 0.05) return false;
        const r = el.getBoundingClientRect();
        return r.width >= minW && r.height >= minH;
    };

    const captchaSels = [
        '#captcha-verify-container-main-page',
        '.captcha_verify_container',
        '[id*="captcha-verify"]',
        '[class*="secsdk-captcha"]',
        'iframe[src*="captcha"]',
        'iframe[src*="secsdk"]'
    ];
    for (const sel of captchaSels) {
        for (const el of document.querySelectorAll(sel)) {
            if (vis(el, 140, 60)) {
                return { kind: 'captcha', reason: 'TikTok verification challenge visible (' + sel + ')' };
            }
        }
    }

    const loginSels = ['#loginContainer', '#login-modal', '[data-e2e="login-modal"]'];
    for (const sel of loginSels) {
        for (const el of document.querySelectorAll(sel)) {
            if (vis(el, 180, 100)) {
                return { kind: 'login', reason: 'TikTok login modal visible (' + sel + ')' };
            }
        }
    }

    const bodyTxt = (document.body ? document.body.innerText : '').slice(0, 3000);
    if (/connecte-toi à tiktok|log in to tiktok|utiliser un code qr|use qr code/i.test(bodyTxt)
        && !document.querySelector('input[type="file"]')) {
        return { kind: 'login', reason: 'TikTok login screen visible' };
    }
    return null;
}
"""


def _update_banner(page: Any, text: str, is_paused: bool = False) -> None:
    try:
        page.evaluate(
            """([msg, paused]) => {
                if (!document.body) return;
                let b = document.getElementById('__qs_pub_banner');
                if (!b) {
                    b = document.createElement('div');
                    b.id = '__qs_pub_banner';
                    b.style.cssText = 'position:fixed;top:10px;right:14px;z-index:2147483647;'
                        + 'padding:8px 14px;border-radius:8px;font:600 13px/1.4 system-ui,sans-serif;'
                        + 'box-shadow:0 4px 14px rgba(0,0,0,0.35);pointer-events:none;max-width:520px;';
                    document.body.appendChild(b);
                }
                b.style.background = paused ? 'rgba(146, 64, 14, 0.95)' : 'rgba(15, 23, 42, 0.92)';
                b.style.color = paused ? '#fef3c7' : '#f8fafc';
                b.style.border = paused ? '1px solid #f59e0b' : '1px solid rgba(148,163,184,0.4)';
                b.textContent = msg;
            }""",
            [text, is_paused],
        )
    except Exception:
        pass


def _wait_for_human_gate_if_needed(
    context: Any,
    page: Any,
    target_url: str,
    headless: bool,
    logger: Logger,
) -> tuple[Any, Optional[str]]:
    """Pauses passively while login or CAPTCHA is on screen. Never attempts to bypass or click CAPTCHAs."""
    try:
        gate = page.evaluate(DETECT_GATE_JS)
    except Exception:
        gate = None
    if not gate:
        return page, None

    kind = gate.get("kind", "login")
    reason = gate.get("reason", "User action required")
    if headless:
        return page, f"Blocked in headless mode by {kind}: {reason}"

    was_login = kind == "login"
    logger.emit_log(
        "HUMAN-GATE",
        f"PAUSED ({kind}): {reason}. Please complete it directly in the opened Chrome window "
        "(timeout is paused; closing the Chrome window cancels the job).",
    )
    _update_banner(
        page,
        f"QuantumSocial Auto-Publish PAUSED: {reason} — complete in Chrome to continue",
        is_paused=True,
    )

    clear_ticks = 0
    while True:
        time.sleep(1.0)
        open_pages = [p for p in context.pages if not p.is_closed()]
        if not open_pages:
            return None, "Chrome window was closed by user while waiting for login/verification."
        page = open_pages[0]
        try:
            cur_gate = page.evaluate(DETECT_GATE_JS)
        except Exception:
            clear_ticks = 0
            continue
        if cur_gate is None:
            clear_ticks += 1
            if clear_ticks >= 3:
                break
        else:
            clear_ticks = 0
            if cur_gate.get("kind") == "login":
                was_login = True

    logger.emit_log("HUMAN-GATE", "Login / verification cleared! Resuming TikTok Studio upload...")
    if was_login and target_url.startswith("http") and "tiktokstudio/upload" not in (page.url or ""):
        logger.emit_log("NAVIGATION", f"Returning to upload page: {target_url}")
        page.goto(target_url, wait_until="domcontentloaded", timeout=45000)
        page.wait_for_timeout(2500)
    return page, None


def _find_upload_surface(page: Any):
    """Returns the frame (main page or creator-center iframe) containing the upload UI."""
    frames = [page] + [f for f in page.frames if f != page.main_frame]
    for fr in frames:
        try:
            if fr.locator('input[type="file"]').count() > 0:
                return fr
        except Exception:
            continue
    return page


def _detect_logged_in_handles(page: Any) -> list[str]:
    try:
        handles = page.evaluate(
            """async () => {
                const found = new Set();
                for (const a of document.querySelectorAll('a[href*="/@"]')) {
                    const m = (a.getAttribute('href') || '').match(/\\/@([A-Za-z0-9._-]+)/);
                    if (m && m[1]) found.add(m[1].toLowerCase());
                }
                if (location.origin.includes('tiktok.com')) {
                    try {
                        const resp = await fetch('/passport/web/account/info/', { credentials: 'include' });
                        if (resp.ok) {
                            const j = await resp.json();
                            const u = j && j.data && (j.data.username || j.data.unique_id || '');
                            if (u) found.add(String(u).toLowerCase());
                        }
                    } catch (_) {}
                }
                return Array.from(found);
            }"""
        )
        return handles if isinstance(handles, list) else []
    except Exception:
        return []


def _dismiss_studio_popups(surface: Any, page: Any, logger: Logger) -> None:
    """Dismisses floating TikTok Studio Joyride tours, tooltips, & content-check modals without touching page buttons."""
    modal_btn_selector = (
        '#react-joyride-portal button, '
        '.react-joyride__tooltip button, '
        '[class*="joyride"] button, '
        '.TUXModal-overlay button, '
        '.TUXModal button, '
        '[role="dialog"] button, '
        '[role="alertdialog"] button, '
        '[data-floating-ui-portal] button'
    )
    accept_re = re.compile(
        r"^\s*(Activer|J'ai compris|Compris|Suivant|Terminer|Passer|Continuer|Fermer|Ignorer|"
        r"Turn on|Enable|Got it|Next|Skip|Done|Continue|OK|Close)\s*$",
        re.IGNORECASE,
    )
    for _ in range(8):
        clicked_any = False
        try:
            btns = surface.locator(modal_btn_selector)
            cnt = btns.count()
            for idx in range(cnt):
                b = btns.nth(idx)
                if not b.is_visible():
                    continue
                txt = (b.inner_text() or "").strip()
                aria = (b.get_attribute("aria-label") or "").strip().lower()
                d_action = (b.get_attribute("data-action") or "").strip().lower()
                d_testid = (b.get_attribute("data-test-id") or "").strip().lower()
                if (
                    accept_re.match(txt)
                    or aria in ("close", "fermer")
                    or d_action in ("close", "skip", "primary")
                    or d_testid in ("button-primary", "button-close", "button-skip")
                ):
                    logger.emit_log("UI", f"Dismissing TikTok Studio overlay/tour button: '{txt or d_action or aria}'")
                    b.click(timeout=2000)
                    page.wait_for_timeout(450)
                    clicked_any = True
                    break
        except Exception:
            pass
        if not clicked_any:
            break

    # If a react-joyride onboarding tour backdrop still lingers and intercepts clicks, remove that tour overlay
    try:
        removed_joyride = surface.evaluate(
            """() => {
                let removed = 0;
                for (const el of document.querySelectorAll('#react-joyride-portal, .react-joyride__overlay')) {
                    el.remove();
                    removed++;
                }
                return removed;
            }"""
        )
        if removed_joyride:
            logger.emit_log("UI", f"Cleared {removed_joyride} lingering react-joyride onboarding tour overlay(s).")
            page.wait_for_timeout(200)
    except Exception:
        pass


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "--reconnect":
        acc_url = sys.argv[2] if len(sys.argv) >= 3 else ""
        handle = _extract_expected_handle(acc_url)
        safe_handle = re.sub(r"[^A-Za-z0-9._-]", "_", handle or "default")
        dedicated_dir = os.path.join(CACHE_ROOT, f"publish-tiktok-{safe_handle}")
        os.makedirs(dedicated_dir, exist_ok=True)
        from playwright.sync_api import sync_playwright

        with sync_playwright() as pw:
            ctx = pw.chromium.launch_persistent_context(
                dedicated_dir,
                headless=False,
                viewport={"width": 1380, "height": 920},
                ignore_default_args=["--enable-automation"],
                args=["--disable-blink-features=AutomationControlled", "--no-first-run", "--no-default-browser-check"],
            )
            p = ctx.pages[0] if ctx.pages else ctx.new_page()
            p.goto(TIKTOK_UPLOAD_URL, wait_until="domcontentloaded", timeout=45000)
            while any(not pg.is_closed() for pg in ctx.pages):
                time.sleep(0.5)
            ctx.close()
        return 0

    if len(sys.argv) < 2 or not os.path.isfile(sys.argv[1]):
        return _finish(fatal=True, error="Usage: publish_tiktok.py <request-json-file>")

    try:
        with open(sys.argv[1], encoding="utf-8") as fh:
            req = json.load(fh)
    except Exception as exc:
        return _finish(fatal=True, error=f"Could not read request JSON: {exc}")

    account_url = str(req.get("accountUrl") or "").strip()
    media_paths = [str(p) for p in (req.get("mediaPaths") or []) if p]
    caption = str(req.get("caption") or "").strip()
    video_overlay_text = str(req.get("videoOverlayText") or "").strip()
    video_text_style = _normalize_video_text_style(str(req.get("videoTextStyle") or ""))
    short_code = str(req.get("shortCode") or "QS").strip()
    use_system_profile = bool(req.get("useSystemChromeProfile", True))
    mute_original_audio = bool(req.get("muteOriginalAudio", True))
    use_suggested_sound = bool(req.get("useSuggestedSound", True))
    dry_run = bool(req.get("dryRun", False))
    upload_url_override = str(req.get("uploadUrlOverride") or "").strip()

    logger = Logger(short_code)
    expected_handle = _extract_expected_handle(account_url)
    target_url = upload_url_override or TIKTOK_UPLOAD_URL
    headless = os.environ.get("QS_PUBLISH_HEADLESS", "0") == "1"

    logger.emit_log(
        "INIT",
        f"Starting TikTok publish for {short_code} -> target account @{expected_handle or 'unknown'} "
        f"({len(media_paths)} file(s), dryRun={dry_run})",
    )

    if not media_paths:
        logger.emit_log("ERROR", "No media files provided to publish.")
        return _finish(log_file=logger.log_file, fatal=False, error="No media files found for this creative.")

    for mp in media_paths:
        if not os.path.isfile(mp):
            logger.emit_log("ERROR", f"Media file does not exist on disk: {mp}")
            return _finish(log_file=logger.log_file, fatal=False, error=f"Missing media file: {mp}")

    exts = {os.path.splitext(p)[1].lower() for p in media_paths}
    is_video_post = any(e in VIDEO_EXTENSIONS for e in exts)
    is_image_post = all(e in IMAGE_EXTENSIONS for e in exts)
    if not is_video_post and not is_image_post:
        return _finish(
            log_file=logger.log_file,
            fatal=False,
            error=f"Unsupported media file extension(s): {sorted(exts)}",
        )
    if is_video_post and len(media_paths) > 1:
        media_paths = [media_paths[0]]

    # Safety guardrail: TikTok posts must be either a Video OR a multi-image Slideshow (>= 2 slides), never a lone static image!
    if is_image_post and len(media_paths) < 2 and not upload_url_override:
        err = (
            f"Refusing to publish {short_code}: TikTok requires either a Video or a multi-image Slideshow "
            f"(>= 2 slides with text + music), not a single static image ({os.path.basename(media_paths[0])})."
        )
        logger.emit_log("ERROR", err)
        return _finish(log_file=logger.log_file, fatal=False, error=err)

    # Safety guardrail: never publish a real TikTok post whose caption is only a bare qsXXXX code without description/#hashtags
    stripped_without_code = re.sub(r"(?:#\s*)?\bqs[-_]?[0-9a-z]+\b", "", caption, flags=re.IGNORECASE).strip()
    if not upload_url_override and (len(stripped_without_code) < 10 or "#" not in stripped_without_code):
        err = (
            f"Refusing to publish {short_code} with missing description or #hashtags "
            f"(caption was only: {caption!r}). Every TikTok post must include a real description + hashtags."
        )
        logger.emit_log("ERROR", err)
        return _finish(log_file=logger.log_file, fatal=False, error=err)

    muted_temp_video: Optional[str] = None
    if is_video_post and mute_original_audio:
        muted_temp_video = _strip_video_audio_copy(media_paths[0], logger)
        if muted_temp_video:
            media_paths = [muted_temp_video]

    logger.emit_log(
        "MEDIA",
        f"Post mode: {'VIDEO' if is_video_post else f'PHOTO CAROUSEL / IMAGE ({len(media_paths)} slide(s))'} — "
        + ", ".join(os.path.basename(p) for p in media_paths),
    )

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        err = "Playwright is not installed for python3 (run: python3 -m pip install playwright)."
        logger.emit_log("ERROR", err)
        return _finish(log_file=logger.log_file, fatal=True, error=err)

    temp_dir_to_clean: Optional[str] = None
    try:
        with sync_playwright() as pw:
            context, temp_dir_to_clean = _launch_browser(
                pw, expected_handle, use_system_profile, headless, logger
            )
            try:
                context.add_init_script(
                    "Object.defineProperty(navigator, 'webdriver', {get: () => undefined});"
                )
                page = context.pages[0] if context.pages else context.new_page()
                logger.emit_log("NAVIGATION", f"Opening {target_url} ...")
                page.goto(target_url, wait_until="domcontentloaded", timeout=45000)
                page.wait_for_timeout(2500 if not headless else 300)

                page, gate_err = _wait_for_human_gate_if_needed(
                    context, page, target_url, headless, logger
                )
                if gate_err or page is None:
                    logger.emit_log("ERROR", gate_err or "Browser closed.")
                    return _finish(log_file=logger.log_file, fatal=True, error=gate_err or "Browser closed.")

                _update_banner(page, f"QuantumSocial Auto-Publish: uploading {short_code}...")

                # Wait up to 25s for TikTok Studio's file input to mount
                surface = page
                found_input = False
                deadline_mount = time.monotonic() + (25.0 if not headless else 6.0)
                while time.monotonic() < deadline_mount:
                    page, gate_err = _wait_for_human_gate_if_needed(
                        context, page, target_url, headless, logger
                    )
                    if gate_err or page is None:
                        return _finish(log_file=logger.log_file, fatal=True, error=gate_err or "Browser closed.")
                    surface = _find_upload_surface(page)
                    if surface.locator('input[type="file"]').count() > 0:
                        found_input = True
                        break
                    page.wait_for_timeout(600 if not headless else 150)

                if not found_input:
                    snap = logger.dump_page(page, "no_file_input")
                    err = (
                        f"TikTok Studio upload input[type=file] did not appear at {page.url}. "
                        f"Check screenshot {snap} and log {logger.log_file}."
                    )
                    logger.emit_log("ERROR", err)
                    return _finish(log_file=logger.log_file, screenshot=snap, fatal=True, error=err)

                # Verify logged-in TikTok handle if visible on page
                visible_handles = _detect_logged_in_handles(page)
                if visible_handles:
                    logger.emit_log("ACCOUNT", f"Detected TikTok profile link(s) on page: {visible_handles}")
                    if expected_handle and expected_handle not in visible_handles:
                        snap = logger.dump_page(page, "account_mismatch")
                        err = (
                            f"Account mismatch safety stop: Chrome is logged into @{visible_handles[0]}, "
                            f"but job expects @{expected_handle}. Click 'Reconnect…' in the Publishing tab "
                            f"to switch/log into @{expected_handle} in Chrome and retry."
                        )
                        logger.emit_log("ACCOUNT-MISMATCH", err)
                        return _finish(log_file=logger.log_file, screenshot=snap, fatal=True, error=err)

                # Inspect file inputs and pick the best matching one for Video vs Photo
                file_inputs = surface.locator('input[type="file"]')
                input_count = file_inputs.count()
                chosen_idx = 0
                accept_summary: list[str] = []
                for idx in range(input_count):
                    acc = (file_inputs.nth(idx).get_attribute("accept") or "").lower()
                    mult = file_inputs.nth(idx).get_attribute("multiple")
                    accept_summary.append(f"#{idx}(accept='{acc}', multiple={mult is not None})")
                    if is_image_post and ("image" in acc or ".png" in acc or ".jpg" in acc or acc == ""):
                        chosen_idx = idx
                    elif is_video_post and ("video" in acc or ".mp4" in acc or acc == ""):
                        chosen_idx = idx
                logger.emit_log("UPLOAD", f"Found {input_count} file input(s): {', '.join(accept_summary)} -> using #{chosen_idx}")

                chosen_accept = (file_inputs.nth(chosen_idx).get_attribute("accept") or "").lower()
                if is_image_post and "video" in chosen_accept and "image" not in chosen_accept and ".jpg" not in chosen_accept and ".png" not in chosen_accept:
                    # Check if there is a Photo / Carousel mode button on the upload card first
                    photo_tab = surface.locator(
                        'button:has-text("Photo"), [role="tab"]:has-text("Photo"), button:has-text("Photos")'
                    )
                    if photo_tab.count() > 0 and photo_tab.first.is_visible():
                        logger.emit_log("UPLOAD", "Clicking 'Photo' tab to switch uploader to Photo Mode...")
                        photo_tab.first.click()
                        page.wait_for_timeout(1200 if not headless else 150)
                        file_inputs = surface.locator('input[type="file"]')

                if is_image_post:
                    # Prefer the file input with 'multiple' enabled so all carousel slides attach together
                    for idx in range(file_inputs.count()):
                        acc = (file_inputs.nth(idx).get_attribute("accept") or "").lower()
                        mult = file_inputs.nth(idx).get_attribute("multiple")
                        if mult is not None and ("image" in acc or ".png" in acc or ".jpg" in acc or acc == ""):
                            chosen_idx = idx
                            break

                logger.emit_log("UPLOAD", f"Attaching {len(media_paths)} file(s) to TikTok Studio uploader...")
                file_inputs.nth(chosen_idx).set_input_files(media_paths if len(media_paths) > 1 else media_paths[0])

                # Wait for caption editor + uploaded media processing
                logger.emit_log("PROCESSING", "Waiting for TikTok Studio to process uploaded media and enable caption editor...")
                editor_selector = (
                    '[data-e2e*="caption"] [contenteditable="true"], '
                    '.public-DraftEditor-content[contenteditable="true"], '
                    'div[contenteditable="true"], '
                    'textarea[name="caption"], textarea[data-e2e*="caption"]'
                )
                editor_ready = False
                proc_deadline = time.monotonic() + (120.0 if not headless else 10.0)
                while time.monotonic() < proc_deadline:
                    page, gate_err = _wait_for_human_gate_if_needed(
                        context, page, target_url, headless, logger
                    )
                    if gate_err or page is None:
                        return _finish(log_file=logger.log_file, fatal=True, error=gate_err or "Browser closed.")
                    surface = _find_upload_surface(page)
                    ed = surface.locator(editor_selector)
                    if ed.count() > 0 and ed.first.is_visible():
                        editor_ready = True
                        break
                    page.wait_for_timeout(1000 if not headless else 150)

                if not editor_ready:
                    snap = logger.dump_page(page, "no_caption_editor")
                    err = (
                        f"Caption editor did not appear after attaching media (TikTok Studio may have rejected the file format). "
                        f"See screenshot {snap}."
                    )
                    logger.emit_log("ERROR", err)
                    return _finish(log_file=logger.log_file, screenshot=snap, fatal=False, error=err)

                # Attach 1st suggested TikTok sound AND/OR native TikTok 'Aa Texte' video overlay BEFORE filling caption
                applied_sound_title = ""
                applied_video_text = False
                applied_video_text_style = ""
                if is_video_post and (use_suggested_sound or bool(video_overlay_text)):
                    _dismiss_studio_popups(surface, page, logger)
                    applied_sound_title, applied_video_text, applied_video_text_style = _apply_video_editor_sound_and_text(
                        surface,
                        page,
                        mute_original_audio,
                        use_suggested_sound,
                        video_overlay_text,
                        video_text_style,
                        headless,
                        logger,
                    )
                    surface = _find_upload_surface(page)
                elif is_image_post and use_suggested_sound:
                    _dismiss_studio_popups(surface, page, logger)
                    applied_sound_title = _apply_suggested_photo_sound(
                        surface, page, headless, logger
                    )
                    surface = _find_upload_surface(page)

                # On TikTok Photo Slideshow mode, also populate the 90-char Catchy Title input if present
                if is_image_post:
                    try:
                        title_inp = surface.locator(
                            'input[class*="titleInput"], input[placeholder*="titre" i], input[placeholder*="title" i]'
                        )
                        if title_inp.count() > 0 and title_inp.first.is_visible():
                            hook_headline = caption.split(" — ")[0].strip()
                            hook_headline = re.sub(r"(?:#\s*)[A-Za-z0-9_]+", "", hook_headline).strip()
                            hook_headline = re.sub(r"\bqs[-_]?[0-9a-z]+\b", "", hook_headline, flags=re.IGNORECASE).strip()[:85]
                            if hook_headline:
                                title_inp.first.fill(hook_headline)
                                logger.emit_log("CAPTION", f"Filled Photo Slideshow catchy title: '{hook_headline}'")
                    except Exception:
                        pass

                # Fill caption + hashtags + qsXXXX tracking code
                _dismiss_studio_popups(surface, page, logger)
                editor = surface.locator(editor_selector).first
                logger.emit_log("CAPTION", f"Filling caption ({len(caption)} chars): {caption[:140]}")

                try:
                    editor.click(timeout=5000)
                except Exception:
                    logger.emit_log("UI", "Overlay intercepted caption click — clearing overlays and retrying...")
                    _dismiss_studio_popups(surface, page, logger)
                    try:
                        editor.click(timeout=4000)
                    except Exception:
                        editor.click(force=True, timeout=3000)
                        editor.evaluate("el => el.focus()")
                page.wait_for_timeout(250 if not headless else 50)
                page.keyboard.press("Control+A")
                page.keyboard.press("Backspace")
                page.wait_for_timeout(150 if not headless else 30)

                tag_name = (editor.evaluate("el => el.tagName") or "").upper()
                if tag_name == "TEXTAREA" or tag_name == "INPUT":
                    editor.fill(caption)
                else:
                    # Insert text cleanly; collapse internal newlines to spaces if DraftJS splits blocks awkwardly
                    normalized_caption = re.sub(r"\s*\n+\s*", " — ", caption).strip()
                    page.keyboard.insert_text(normalized_caption)
                    page.wait_for_timeout(400 if not headless else 50)
                    # Dismiss any hashtag autocomplete popover so it cannot intercept the Post button
                    page.keyboard.press("Escape")

                page.wait_for_timeout(400 if not headless else 50)
                actual_caption = (editor.evaluate("el => (el.innerText || el.value || '')") or "").strip()
                logger.emit_log(
                    "CAPTION",
                    f"Caption editor verified ({len(actual_caption)} chars, hashtags={'#' in actual_caption}): "
                    f"{actual_caption[:160]}",
                )

                # Wait for the Post / Publier button to become enabled (video upload 100% finished)
                post_btn_js = r"""
                () => {
                    const e2eBtn = document.querySelector('[data-e2e="post_video_button"]');
                    if (e2eBtn) {
                        const dis = e2eBtn.disabled || e2eBtn.getAttribute('aria-disabled') === 'true';
                        return { found: true, enabled: !dis, label: (e2eBtn.innerText || 'Post').trim() };
                    }
                    const labels = /^(post|publier|publish)$/i;
                    for (const b of document.querySelectorAll('button, [role="button"]')) {
                        const txt = (b.innerText || '').trim();
                        if (labels.test(txt)) {
                            const dis = b.disabled || b.getAttribute('aria-disabled') === 'true';
                            return { found: true, enabled: !dis, label: txt };
                        }
                    }
                    return { found: false, enabled: false, label: '' };
                }
                """
                post_ready = False
                btn_label = "Post"
                upload_deadline = time.monotonic() + (180.0 if not headless else 10.0)
                last_wait_log = 0.0
                while time.monotonic() < upload_deadline:
                    page, gate_err = _wait_for_human_gate_if_needed(
                        context, page, target_url, headless, logger
                    )
                    if gate_err or page is None:
                        return _finish(log_file=logger.log_file, fatal=True, error=gate_err or "Browser closed.")
                    surface = _find_upload_surface(page)
                    st = surface.evaluate(post_btn_js)
                    if st.get("found") and st.get("enabled"):
                        post_ready = True
                        btn_label = st.get("label") or "Post"
                        break
                    now_m = time.monotonic()
                    if now_m - last_wait_log >= 8.0:
                        last_wait_log = now_m
                        logger.emit_log(
                            "PROCESSING",
                            f"Waiting for media upload/checks to finish (button found={st.get('found')}, enabled={st.get('enabled')})...",
                        )
                    page.wait_for_timeout(1000 if not headless else 150)

                if not post_ready:
                    snap = logger.dump_page(page, "post_button_disabled")
                    err = f"Post/Publier button never became enabled before timeout. Check {snap}."
                    logger.emit_log("ERROR", err)
                    return _finish(log_file=logger.log_file, screenshot=snap, fatal=False, error=err)

                ready_snap = logger.dump_page(page, "ready_to_post")
                if dry_run:
                    logger.emit_log(
                        "DRY-RUN",
                        f"Dry-run succeeded! Media uploaded, caption filled, and '{btn_label}' button is enabled. "
                        f"Stopped before clicking '{btn_label}'. Screenshot: {ready_snap}",
                    )
                    return _finish(
                        published=False,
                        dry_run=True,
                        sound_title=applied_sound_title,
                        video_text_applied=applied_video_text,
                        video_text_style=applied_video_text_style,
                        log_file=logger.log_file,
                        screenshot=ready_snap,
                        error=None,
                    )

                _dismiss_studio_popups(surface, page, logger)
                logger.emit_log("SUBMIT", f"Clicking '{btn_label}' button...")
                e2e_loc = surface.locator('[data-e2e="post_video_button"]')
                if e2e_loc.count() > 0 and e2e_loc.first.is_visible():
                    e2e_loc.first.scroll_into_view_if_needed()
                    try:
                        e2e_loc.first.click(timeout=6000)
                    except Exception:
                        _dismiss_studio_popups(surface, page, logger)
                        e2e_loc.first.click(force=True, timeout=4000)
                else:
                    btn_loc = surface.locator('button, [role="button"]').filter(
                        has_text=re.compile(r"^\s*(Post|Publier|Publish)\s*$", re.IGNORECASE)
                    )
                    btn_loc.first.scroll_into_view_if_needed()
                    try:
                        btn_loc.first.click(timeout=6000)
                    except Exception:
                        _dismiss_studio_popups(surface, page, logger)
                        btn_loc.first.click(force=True, timeout=4000)

                # Wait for confirmation (redirect to /tiktokstudio/content or confirmation dialog/toast)
                confirmed = False
                confirm_deadline = time.monotonic() + (60.0 if not headless else 8.0)
                confirm_re = re.compile(
                    r"manage your posts|gérer tes publications|gérer vos publications|"
                    r"video is being uploaded|photo is being uploaded|vidéo a été publiée|"
                    r"published|publiée|upload another|téléverser une autre|view profile|voir le profil",
                    re.IGNORECASE,
                )
                post_anyway_re = re.compile(
                    r"^\s*(Publier maintenant|Continuer à publier|Publier quand même|"
                    r"Post now|Continue to post|Post anyway)\s*$",
                    re.IGNORECASE,
                )
                while time.monotonic() < confirm_deadline:
                    page, gate_err = _wait_for_human_gate_if_needed(
                        context, page, target_url, headless, logger
                    )
                    if gate_err or page is None:
                        return _finish(log_file=logger.log_file, fatal=True, error=gate_err or "Browser closed.")
                    cur_url = (page.url or "").lower()
                    if "/tiktokstudio/content" in cur_url or "published=1" in cur_url:
                        confirmed = True
                        break
                    # Handle "Content check still in progress -> Post now / Continuer à publier" modal
                    try:
                        modal_btns = surface.locator('.TUXModal button, [role="dialog"] button, [data-floating-ui-portal] button')
                        for mb_i in range(modal_btns.count()):
                            mb = modal_btns.nth(mb_i)
                            if mb.is_visible():
                                mb_txt = (mb.inner_text() or "").strip()
                                if post_anyway_re.match(mb_txt):
                                    logger.emit_log("SUBMIT", f"Confirming post modal button: '{mb_txt}'")
                                    mb.click(timeout=2000)
                                    page.wait_for_timeout(600)
                                    break
                    except Exception:
                        pass
                    try:
                        btxt = (surface.locator("body").inner_text() or "")[:5000]
                        if confirm_re.search(btxt):
                            confirmed = True
                            break
                    except Exception:
                        pass
                    page.wait_for_timeout(1000 if not headless else 150)

                done_snap = logger.dump_page(page, "after_submit")
                if not confirmed:
                    err = (
                        f"Clicked '{btn_label}', but TikTok Studio did not show publish confirmation within 60s "
                        f"(current URL: {page.url}). Inspect screenshot {done_snap} before retrying."
                    )
                    logger.emit_log("ERROR", err)
                    return _finish(
                        published=False,
                        log_file=logger.log_file,
                        screenshot=done_snap,
                        fatal=True,
                        error=err,
                    )

                fallback_post_url = account_url or f"https://www.tiktok.com/@{expected_handle}"
                logger.emit_log(
                    "SUCCESS",
                    f"Published {short_code} to TikTok (@{expected_handle or 'account'})! "
                    f"Screenshot: {done_snap}",
                )
                placeholder_url = f"{fallback_post_url}#{short_code}" if fallback_post_url else ""
                return _finish(
                    published=True,
                    dry_run=False,
                    post_url=placeholder_url,
                    sound_title=applied_sound_title,
                    video_text_applied=applied_video_text,
                    video_text_style=applied_video_text_style,
                    log_file=logger.log_file,
                    screenshot=done_snap,
                    fatal=False,
                    error=None,
                )
            finally:
                try:
                    context.close()
                except Exception:
                    pass
    except Exception as exc:
        logger.emit_log("FATAL", f"Unhandled exception: {exc}")
        return _finish(log_file=logger.log_file, fatal=True, error=str(exc))
    finally:
        if muted_temp_video and os.path.isfile(muted_temp_video):
            try:
                os.remove(muted_temp_video)
            except OSError:
                pass
        if temp_dir_to_clean and os.path.isdir(temp_dir_to_clean):
            shutil.rmtree(temp_dir_to_clean, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
