#!/usr/bin/env python3
"""Fetches the public stats of one YouTube video (watch or Shorts URL).

Launches a headless Chromium browser via Playwright, navigates to the
watch page, and reads the counts from the JSON embedded in the page's
initial state. Only the "public" source is implemented — YouTube Studio
analytics would need its own dedicated script (the dashboard is a heavy
dynamic app); the platform class does not declare Analytics support
until that exists.

YouTube redirects EU traffic to a cookie-consent interstitial that hides
the whole page; a pre-set CONSENT cookie skips it (same trick as
fetch_stats_youtube.py). The comment count is lazy-loaded, so it is
often absent from the initial payload — reported as null in that case.

Prints a single JSON line to stdout:
    {"views": <int|null>, "likes": <int|null>, "comments": <int|null>,
     "shares": <int|null>, "impressions": <int|null>,
     "watch_time_seconds": <int|null>, "avg_watch_percent": <float|null>,
     "follows": <int|null>, "error": <string|null>}

Usage: fetch_video_stats_youtube.py <video-url> <public|analytics>
"""
import json
import re
import sys

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)

# Skips the EU cookie-consent interstitial that otherwise replaces the page.
CONSENT_COOKIES = [
    {"name": "CONSENT", "value": "YES+1", "domain": ".youtube.com", "path": "/"},
    {"name": "SOCS", "value": "CAI", "domain": ".youtube.com", "path": "/"},
]


def _emit(error=None, **stats):
    result = {"views": None, "likes": None, "comments": None, "shares": None,
              "impressions": None, "watch_time_seconds": None,
              "avg_watch_percent": None, "follows": None, "error": error}
    result.update(stats)
    print(json.dumps(result))


def _first_int(html, patterns):
    for pattern in patterns:
        m = re.search(pattern, html)
        if m:
            digits = re.sub(r"[^\d]", "", m.group(1))
            if digits:
                return int(digits)
    return None


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("public", "analytics"):
        _emit(error="usage: fetch_video_stats_youtube.py <url> <public|analytics>")
        return 1
    if sys.argv[2] == "analytics":
        _emit(error="YouTube Studio analytics fetching is not implemented yet.")
        return 1
    url = sys.argv[1]

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        _emit(error="Playwright is not installed "
                    "(pip install playwright && playwright install chromium)")
        return 1

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            try:
                context = browser.new_context(user_agent=USER_AGENT)
                context.add_cookies(CONSENT_COOKIES)
                page = context.new_page()
                page.goto(url, wait_until="domcontentloaded", timeout=30000)
                page.wait_for_timeout(3000)
                html = page.content()
            finally:
                browser.close()
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1

    # The like count has changed shape across YouTube frontend versions;
    # try the known embeddings from newest to oldest.
    _emit(
        views=_first_int(html, [r'"viewCount":\s*"(\d+)"']),
        likes=_first_int(html, [
            r'"likeCount":\s*"?(\d+)"?',
            r'"defaultText":\{"accessibility":\{"accessibilityData":'
            r'\{"label":"([\d,.\s]+) ',
        ]),
        comments=_first_int(html, [r'"commentCount":\s*\{?"?(?:simpleText"?:\s*")?(\d+)']),
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
