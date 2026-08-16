#!/usr/bin/env python3
"""Fetches the public follower count of a Pinterest profile, plus the
owner-only "monthly views" stat when logged in as that profile's owner.

Follower count is read from the page's embedded JSON and needs no login.
"Monthly views" is only shown by Pinterest to the profile's own owner
while logged in — never to anonymous visitors or to other logged-in
accounts (confirmed empirically: absent from the anonymous page in every
form checked — embedded JSON, visible text, after dismissing the cookie
banner, headed and headless). To read it, this script keeps a persistent
browser profile at ~/.cache/quantumsocial/pinterest-profile:

- Not logged into Pinterest at all: opens a VISIBLE browser window and
  waits (up to 5 minutes) for a human to log in. The session is then
  remembered for next time.
- Already logged in as this profile's owner: fetches everything and
  closes automatically within a few seconds — no window lingers.
- Logged in as a *different* Pinterest account (not this profile's
  owner): does NOT wait — that can never satisfy the owner check, so it
  just returns the followers count with views left null.

Prints a single JSON line to stdout:
    {"followers": <int|null>, "views": <int|null>, "likes": <int|null>, "error": <string|null>}

Usage: fetch_stats_pinterest.py <url>
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

PROFILE_DIR = os.path.expanduser("~/.cache/quantumsocial/pinterest-profile")

OWNER_MARKERS = ("Edit profile", "Modifier le profil")
LOGGED_OUT_MARKERS = ("Log in", "Se connecter")
LOGIN_WAIT_TIMEOUT_S = 300
LOGIN_POLL_INTERVAL_S = 2


def _emit(followers=None, views=None, likes=None, error=None):
    print(json.dumps({"followers": followers, "views": views, "likes": likes, "error": error}))


def _has_owner_access(text):
    return any(marker in text for marker in OWNER_MARKERS)


def _is_logged_out(text):
    return any(marker in text for marker in LOGGED_OUT_MARKERS)


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


def _extract_views(text):
    m = re.search(r"([\d][\d.,\s]*[kKmM]?)\s*(?:monthly views|vues mensuelles)",
                  text, re.IGNORECASE)
    return _parse_human_number(m.group(1)) if m else None


def main():
    if len(sys.argv) != 2:
        _emit(error="usage: fetch_stats_pinterest.py <url>")
        return 1
    url = sys.argv[1]

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        _emit(error="Playwright is not installed "
                    "(pip install playwright && playwright install chromium)")
        return 1

    os.makedirs(PROFILE_DIR, exist_ok=True)

    try:
        with sync_playwright() as p:
            context = p.chromium.launch_persistent_context(
                PROFILE_DIR, headless=False, user_agent=USER_AGENT,
                viewport={"width": 1280, "height": 900})
            try:
                page = context.pages[0] if context.pages else context.new_page()
                page.goto(url, wait_until="domcontentloaded", timeout=30000)
                page.wait_for_timeout(2500)
                html = page.content()
                text = page.locator("body").inner_text()

                if _is_logged_out(text) and not _has_owner_access(text):
                    # No Pinterest session at all: keep the window open and
                    # wait for a human to log in. We deliberately never
                    # navigate/reload the page while waiting — doing so
                    # would wipe out whatever is being typed into the login
                    # form. Just passively re-read the page text.
                    deadline = time.time() + LOGIN_WAIT_TIMEOUT_S
                    while time.time() < deadline and _is_logged_out(text):
                        time.sleep(LOGIN_POLL_INTERVAL_S)
                        try:
                            text = page.locator("body").inner_text()
                        except Exception:
                            pass

                    if _is_logged_out(text):
                        # Login never happened; still return the follower
                        # count below (it never needed a login), just skip
                        # the owner-only "monthly views" stat.
                        login_timed_out = True
                    else:
                        # Logged in now. Pinterest's login flow may have
                        # navigated away from the profile (e.g. to the home
                        # feed) — go back to it once to read final stats.
                        try:
                            page.goto(url, wait_until="domcontentloaded", timeout=30000)
                            page.wait_for_timeout(1500)
                        except Exception:
                            pass
                        html = page.content()
                        text = page.locator("body").inner_text()
                        login_timed_out = False
                else:
                    login_timed_out = False
            finally:
                context.close()
    except Exception as exc:  # noqa: BLE001 - report any failure to the caller
        _emit(error=str(exc))
        return 1

    followers_m = re.search(r'"follower_count":\s*(\d+)', html)
    views = _extract_views(text) if _has_owner_access(text) else None
    _emit(followers=int(followers_m.group(1)) if followers_m else None, views=views,
          error="Timed out waiting for Pinterest login (got followers, but not monthly views)."
          if login_timed_out else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
