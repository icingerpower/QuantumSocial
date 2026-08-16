#!/usr/bin/env python3
"""Fetches the public subscriber count of a YouTube channel.

Launches a headless Chromium browser via Playwright, navigates to the
channel's "about" page, reads the subscriber count embedded in the
page's initial state, then closes the browser. Prints a single JSON
line to stdout:

    {"followers": <int|null>, "views": <int|null>, "likes": <int|null>, "error": <string|null>}

YouTube redirects EU traffic to a cookie-consent interstitial that hides
the whole page; a pre-set CONSENT cookie skips it. The channel's total
view count is not reliably present in the initial page payload (would
need extra interaction to reveal), so "views" is always null here.

Usage: fetch_stats_youtube.py <url>
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


def _emit(followers=None, views=None, likes=None, error=None):
    print(json.dumps({"followers": followers, "views": views, "likes": likes, "error": error}))


def main():
    if len(sys.argv) != 2:
        _emit(error="usage: fetch_stats_youtube.py <url>")
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

    m = re.search(r'"subscriberCountText":\{.*?"simpleText":"([^"]+)"', html)
    _emit(followers=_parse_human_number(m.group(1)) if m else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
