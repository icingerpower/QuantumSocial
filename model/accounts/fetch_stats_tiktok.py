#!/usr/bin/env python3
"""Fetches the public follower/like counts of a TikTok profile.

Launches a headless Chromium browser via Playwright, navigates to the
profile URL, reads the stats embedded in the page's initial state, then
closes the browser. Prints a single JSON line to stdout:

    {"followers": <int|null>, "views": <int|null>, "likes": <int|null>, "error": <string|null>}

TikTok does not publicly expose a total-views stat on a profile page
(its stats row is Following / Followers / Likes), so "views" is always
null here; "likes" is the total hearts received across all videos.

Usage: fetch_stats_tiktok.py <url>
"""
import json
import re
import sys

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)


def _emit(followers=None, views=None, likes=None, error=None):
    print(json.dumps({"followers": followers, "views": views, "likes": likes, "error": error}))


def main():
    if len(sys.argv) != 2:
        _emit(error="usage: fetch_stats_tiktok.py <url>")
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
                page = context.new_page()
                page.goto(url, wait_until="domcontentloaded", timeout=30000)
                page.wait_for_timeout(3000)
                html = page.content()
            finally:
                browser.close()
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1

    followers_m = re.search(r'"followerCount":(\d+)', html)
    likes_m = re.search(r'"heartCount":(\d+)', html)
    _emit(
        followers=int(followers_m.group(1)) if followers_m else None,
        likes=int(likes_m.group(1)) if likes_m else None,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
