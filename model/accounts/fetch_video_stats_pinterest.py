#!/usr/bin/env python3
"""Fetches the stats of one Pinterest pin.

Two sources, selected by argv[2]:

- "public": headless Chromium on the pin page; the save/reaction/comment
  counts are read from the JSON embedded in the page's initial state.
  Pinterest does not publicly expose a view count, so "views" is null.
  Saves are reported in the "shares" field (a save is Pinterest's
  re-share primitive).
- "analytics": the pin-stats overlay shown to the pin's owner
  (impressions, saves, pin clicks). Reuses the SAME persistent browser
  profile as fetch_stats_pinterest.py (~/.cache/quantumsocial/
  pinterest-profile), so one interactive Pinterest login serves both
  scripts; first run opens a VISIBLE window and waits up to 5 minutes
  for the login. Labels are matched in English and French; anything not
  found is reported as null.

Prints a single JSON line to stdout:
    {"views": <int|null>, "likes": <int|null>, "comments": <int|null>,
     "shares": <int|null>, "impressions": <int|null>,
     "watch_time_seconds": <int|null>, "avg_watch_percent": <float|null>,
     "follows": <int|null>, "error": <string|null>}

Usage: fetch_video_stats_pinterest.py <pin-url> <public|analytics>
"""
import json
import os
import re
import sys
import time

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)

# Shared with fetch_stats_pinterest.py so one login serves both.
PROFILE_DIR = os.path.expanduser("~/.cache/quantumsocial/pinterest-profile")

LOGGED_OUT_MARKERS = ("Log in", "Se connecter")
LOGIN_WAIT_TIMEOUT_S = 300
LOGIN_POLL_INTERVAL_S = 2

# Owner-only pin-stats overlay labels (English / French) -> output field.
ANALYTICS_LABELS = {
    "impressions": ("Impressions", "impressions"),
    "shares": ("Saves", "Enregistrements", "enregistrements"),
    "views": ("Pin clicks", "Clics sur l'Épingle", "clics"),
}


def _emit(error=None, **stats):
    result = {"views": None, "likes": None, "comments": None, "shares": None,
              "impressions": None, "watch_time_seconds": None,
              "avg_watch_percent": None, "follows": None, "error": error}
    result.update(stats)
    print(json.dumps(result))


def _parse_human_number(text):
    """Parses formats like "91.6k", "91,6 k", "1,234" into an approximate int."""
    if not text:
        return None
    text = text.replace("\xa0", " ").strip()
    m = re.search(r"([\d][\d.,\s]*)\s*([kKmM])?", text)
    if not m:
        return None
    number_part, suffix = m.group(1), (m.group(2) or "").upper()
    if suffix:
        normalized = number_part.strip().replace(" ", "").replace(",", ".")
        try:
            value = float(normalized)
        except ValueError:
            return None
        scale = {"K": 1_000, "M": 1_000_000}[suffix]
        return int(round(value * scale))
    digits = re.sub(r"[.,\s]", "", number_part)
    return int(digits) if digits.isdigit() else None


def _embedded_count(html, key):
    m = re.search(r'"%s":\s*(\d+)' % key, html)
    return int(m.group(1)) if m else None


def _labeled_number(text, labels):
    for label in labels:
        m = re.search(r"([\d][\d.,\s]*[kKmM]?)\s*\n?\s*" + re.escape(label), text)
        if not m:
            m = re.search(re.escape(label) + r"\s*\n?\s*([\d][\d.,\s]*[kKmM]?)", text)
        if m:
            return _parse_human_number(m.group(1))
    return None


def _fetch_public(playwright, url):
    browser = playwright.chromium.launch(headless=True)
    try:
        context = browser.new_context(user_agent=USER_AGENT)
        page = context.new_page()
        page.goto(url, wait_until="domcontentloaded", timeout=30000)
        page.wait_for_timeout(2500)
        html = page.content()
    finally:
        browser.close()

    # Total reactions when present; plain "like" reactions as fallback.
    likes = _embedded_count(html, "total_reactions")
    if likes is None:
        m = re.search(r'"reaction_counts":\s*\{"1":\s*(\d+)', html)
        likes = int(m.group(1)) if m else None
    saves_m = re.search(r'"aggregated_stats":\s*\{[^}]*"saves":\s*(\d+)', html)
    _emit(
        likes=likes,
        comments=_embedded_count(html, "comment_count"),
        shares=int(saves_m.group(1)) if saves_m else _embedded_count(html, "repin_count"),
    )
    return 0


def _fetch_analytics(playwright, url):
    os.makedirs(PROFILE_DIR, exist_ok=True)
    context = playwright.chromium.launch_persistent_context(
        PROFILE_DIR, headless=False, user_agent=USER_AGENT,
        viewport={"width": 1280, "height": 900})
    try:
        page = context.pages[0] if context.pages else context.new_page()
        page.goto(url, wait_until="domcontentloaded", timeout=30000)
        page.wait_for_timeout(2500)
        text = page.locator("body").inner_text()

        if any(marker in text for marker in LOGGED_OUT_MARKERS):
            # Wait passively for a human to log in — never reload while
            # waiting, that would wipe whatever is typed in the login form.
            deadline = time.time() + LOGIN_WAIT_TIMEOUT_S
            while (time.time() < deadline
                   and any(marker in text for marker in LOGGED_OUT_MARKERS)):
                time.sleep(LOGIN_POLL_INTERVAL_S)
                try:
                    text = page.locator("body").inner_text()
                except Exception:
                    pass
            if any(marker in text for marker in LOGGED_OUT_MARKERS):
                _emit(error="Timed out waiting for Pinterest login.")
                return 1
            page.goto(url, wait_until="domcontentloaded", timeout=30000)
            page.wait_for_timeout(2500)
            text = page.locator("body").inner_text()

        # The stats overlay only exists on the owner's own pins; try to
        # expand it in case it is collapsed behind a "See more stats" link.
        for see_more in ("See more stats", "Voir plus de statistiques"):
            try:
                page.get_by_text(see_more).first.click(timeout=2000)
                page.wait_for_timeout(1500)
                text = page.locator("body").inner_text()
                break
            except Exception:
                pass
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1
    finally:
        context.close()

    stats = {field: _labeled_number(text, labels)
             for field, labels in ANALYTICS_LABELS.items()}
    if all(value is None for value in stats.values()):
        _emit(error="No pin stats found — is the logged-in Pinterest account "
                    "the owner of this pin?")
        return 1
    _emit(**{k: v for k, v in stats.items() if v is not None})
    return 0


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("public", "analytics"):
        _emit(error="usage: fetch_video_stats_pinterest.py <url> <public|analytics>")
        return 1

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        _emit(error="Playwright is not installed "
                    "(pip install playwright && playwright install chromium)")
        return 1

    try:
        with sync_playwright() as p:
            if sys.argv[2] == "public":
                return _fetch_public(p, sys.argv[1])
            return _fetch_analytics(p, sys.argv[1])
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())
