#!/usr/bin/env python3
"""Fetches the stats of one TikTok video.

Two sources, selected by argv[2]:

- "public": headless Chromium on the video page; play/digg/comment/share
  counts are read from the JSON embedded in the page's initial state.
- "analytics": TikTok Studio's per-video analytics page, which is only
  visible to the video's owner. Uses a persistent browser profile at
  ~/.cache/quantumsocial/tiktok-profile — first run opens a VISIBLE
  window and waits (up to 5 minutes) for a human to log in, the session
  is remembered afterwards (same approach as fetch_stats_pinterest.py).
  The dashboard is label-based and localized, so numbers are matched
  next to a small table of known English/French labels; anything not
  found is simply reported as null.

Prints a single JSON line to stdout:
    {"views": <int|null>, "likes": <int|null>, "comments": <int|null>,
     "shares": <int|null>, "impressions": <int|null>,
     "watch_time_seconds": <int|null>, "avg_watch_percent": <float|null>,
     "follows": <int|null>, "error": <string|null>}

Usage: fetch_video_stats_tiktok.py <video-url> <public|analytics>
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

PROFILE_DIR = os.path.expanduser("~/.cache/quantumsocial/tiktok-profile")

LOGGED_OUT_MARKERS = ("Log in", "Se connecter")
LOGIN_WAIT_TIMEOUT_S = 300
LOGIN_POLL_INTERVAL_S = 2

# Analytics dashboard labels (English / French) -> output field.
ANALYTICS_LABELS = {
    "views": ("Video views", "Vues de la vid"),
    "likes": ("Likes", "J'aime", "J’aime"),
    "comments": ("Comments", "Commentaires"),
    "shares": ("Shares", "Partages"),
    "follows": ("New followers", "Nouveaux abonn"),
    "avg_watch_percent": ("Watched full video", "vidéo en entier"),
}


def _emit(error=None, **stats):
    result = {"views": None, "likes": None, "comments": None, "shares": None,
              "impressions": None, "watch_time_seconds": None,
              "avg_watch_percent": None, "follows": None, "error": error}
    result.update(stats)
    print(json.dumps(result))


def _parse_human_number(text):
    """Parses formats like "58.2K", "1,13 M", "1,234" into an approximate int."""
    if not text:
        return None
    text = text.replace("\xa0", " ").strip()
    m = re.search(r"([\d][\d.,\s]*)\s*([kKmMbB])?", text)
    if not m:
        return None
    number_part, suffix = m.group(1), (m.group(2) or "").upper()
    if suffix:
        normalized = number_part.strip().replace(" ", "").replace(",", ".")
        try:
            value = float(normalized)
        except ValueError:
            return None
        scale = {"K": 1_000, "M": 1_000_000, "B": 1_000_000_000}[suffix]
        return int(round(value * scale))
    digits = re.sub(r"[.,\s]", "", number_part)
    return int(digits) if digits.isdigit() else None


def _embedded_count(html, key):
    """Reads "key":123 or "key":"123" from the page's embedded JSON."""
    m = re.search(r'"%s":\s*"?(\d+)"?' % key, html)
    return int(m.group(1)) if m else None


def _labeled_number(text, labels):
    """Finds a number directly before or after one of the given labels."""
    for label in labels:
        m = re.search(r"([\d][\d.,\s]*[kKmM]?%?)\s*\n?\s*" + re.escape(label), text)
        if not m:
            m = re.search(re.escape(label) + r"\s*\n?\s*([\d][\d.,\s]*[kKmM]?%?)", text)
        if m:
            return m.group(1)
    return None


def _fetch_public(playwright, url):
    browser = playwright.chromium.launch(headless=True)
    try:
        context = browser.new_context(user_agent=USER_AGENT)
        page = context.new_page()
        page.goto(url, wait_until="domcontentloaded", timeout=30000)
        page.wait_for_timeout(3000)
        html = page.content()
    finally:
        browser.close()

    _emit(
        views=_embedded_count(html, "playCount"),
        likes=_embedded_count(html, "diggCount"),
        comments=_embedded_count(html, "commentCount"),
        shares=_embedded_count(html, "shareCount"),
    )
    return 0


def _fetch_analytics(playwright, url):
    video_id_m = re.search(r"/video/(\d+)", url)
    if not video_id_m:
        _emit(error="Could not extract the video id from the URL "
                    "(expected .../video/<digits>).")
        return 1
    analytics_url = ("https://www.tiktok.com/tiktokstudio/analytics/video/"
                     + video_id_m.group(1))

    os.makedirs(PROFILE_DIR, exist_ok=True)
    context = playwright.chromium.launch_persistent_context(
        PROFILE_DIR, headless=False, user_agent=USER_AGENT,
        viewport={"width": 1280, "height": 900})
    try:
        page = context.pages[0] if context.pages else context.new_page()
        page.goto(analytics_url, wait_until="domcontentloaded", timeout=30000)
        page.wait_for_timeout(3000)
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
                _emit(error="Timed out waiting for TikTok login.")
                return 1
            # Logged in now — the login flow may have navigated away.
            page.goto(analytics_url, wait_until="domcontentloaded", timeout=30000)
            page.wait_for_timeout(3000)
            text = page.locator("body").inner_text()
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1
    finally:
        context.close()

    stats = {}
    for field, labels in ANALYTICS_LABELS.items():
        raw = _labeled_number(text, labels)
        if raw is None:
            continue
        if field == "avg_watch_percent":
            m = re.search(r"([\d.,]+)\s*%", raw)
            if m:
                stats[field] = float(m.group(1).replace(",", "."))
        else:
            stats[field] = _parse_human_number(raw)
    _emit(**stats)
    return 0


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("public", "analytics"):
        _emit(error="usage: fetch_video_stats_tiktok.py <url> <public|analytics>")
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
