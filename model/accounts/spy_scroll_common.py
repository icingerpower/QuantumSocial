#!/usr/bin/env python3
"""Shared Playwright helper for the "Scroll & save" (spy) social-media workers.

Each platform script calls ``run_spy_worker(...)`` with:
  - ``platform_id``: e.g. "pinterest", "tiktok", "youtube", "instagram"
  - ``build_search_url(query)``: turns a plain search query into a search URL
  - ``extract_js``: JavaScript snippet executed in the page that returns a list
    of dicts:
      [{"url": str, "title": str, "media_url": str, "author": str,
        "views": int|None, "likes": int|None, "comments": int|None,
        "shares": int|None}, ...]
  - ``upgrade_media_url(url)`` (optional): upgrades thumbnail URLs to high-res
  - ``install_response_listener(page, cache)`` (optional): passive XHR listener
  - ``enrich_batch(items, context, cache)`` (optional): enriches stats/titles
  - ``score_post(item)`` (optional): returns engagement score for filtering/sorting

Human-in-the-loop rule:
  Whenever a login screen/modal, active user input, OAuth popup, consent wall,
  or CAPTCHA / human-verification challenge is detected, scrolling and timeouts
  PAUSE completely without reloading or stealing focus until a human resolves
  the issue in the visible Chrome window (or closes the window to save & exit).

CLI contract (invoked by AbstractSocialAccount::runSpyScript):
    spy_scroll_<platform>.py <query_or_url> <download_dir> [max_posts] [auto|manual] [min_score]

Stdout contract (single JSON line):
    {"posts": [...], "browsed_count": int, "saved_count": int,
     "rejected_count": int, "min_score": int, "error": null}
"""

from __future__ import annotations

from concurrent.futures import Future, ThreadPoolExecutor
import datetime
import email.utils
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from typing import Any, Callable, Optional

USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)

LOG_FILE = os.path.expanduser("~/.cache/quantumsocial/spy_scroll.log")


def log(platform_id: str, msg: str) -> None:
    try:
        os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(f"[{ts}] [{platform_id}] {msg}\n")
    except Exception:
        pass


def canonical_post_url(raw_url: str) -> str:
    text = (raw_url or "").strip()
    if not text:
        return ""
    try:
        parsed = urllib.parse.urlparse(text)
    except Exception:
        return ""
    scheme = (parsed.scheme or "").lower()
    if scheme not in ("http", "https"):
        return text
    host = (parsed.netloc or "").lower()
    if re.match(r"^(?:[a-z0-9-]+\.)?pinterest\.[a-z.]+$", host):
        host = "www.pinterest.com"
    elif host in ("tiktok.com", "m.tiktok.com"):
        host = "www.tiktok.com"
    elif host in ("instagram.com", "m.instagram.com"):
        host = "www.instagram.com"
    elif host in ("youtube.com", "m.youtube.com"):
        host = "www.youtube.com"

    query_pairs = [
        (k, v)
        for (k, v) in urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
        if not k.startswith("utm_") and k not in ("igsh", "_t", "_r")
    ]
    clean_query = urllib.parse.urlencode(query_pairs)
    path = parsed.path or ""
    while len(path) > 1 and path.endswith("/"):
        path = path[:-1]
    return urllib.parse.urlunparse(("https", host, path, "", clean_query, ""))


def _load_spy_context() -> tuple[set[str], set[str], set[str], set[str]]:
    """Returns (excluded_urls, saved_urls, rejected_urls, favorite_handles)."""
    excluded: set[str] = set()
    saved: set[str] = set()
    rejected: set[str] = set()
    fav_handles: set[str] = set()
    exclude_path = os.environ.get("QS_SPY_EXCLUDE_FILE", "").strip()
    if not exclude_path or not os.path.isfile(exclude_path):
        return (excluded, saved, rejected, fav_handles)
    try:
        with open(exclude_path, encoding="utf-8") as f:
            payload = json.load(f)
        if isinstance(payload, list):
            for raw in payload:
                norm = canonical_post_url(str(raw or ""))
                if norm:
                    excluded.add(norm)
                    saved.add(norm)
        elif isinstance(payload, dict):
            for raw in payload.get("saved") or []:
                norm = canonical_post_url(str(raw or ""))
                if norm:
                    saved.add(norm)
                    excluded.add(norm)
            for raw in payload.get("rejected") or []:
                norm = canonical_post_url(str(raw or ""))
                if norm:
                    rejected.add(norm)
                    excluded.add(norm)
            for raw in payload.get("excluded") or []:
                norm = canonical_post_url(str(raw or ""))
                if norm:
                    excluded.add(norm)
            for raw_h in payload.get("favorite_handles") or []:
                h = str(raw_h or "").strip()
                if h:
                    fav_handles.add(h if h.startswith("@") else f"@{h}")
    except Exception:
        pass
    return (excluded, saved, rejected, fav_handles)


def _load_excluded_urls() -> set[str]:
    return _load_spy_context()[0]


def format_compact_metric(val: Optional[int]) -> str:
    if val is None or val < 0:
        return ""
    if val >= 1_000_000:
        return f"{val / 1_000_000.0:.1f}M"
    if val >= 1_000:
        return f"{val / 1_000.0:.1f}K"
    return str(val)


def format_post_stats_hint(item: dict, platform_id: str) -> str:
    is_ad = bool(item.get("is_ad") or item.get("is_promoted"))
    views = parse_compact_count(item.get("views"))
    likes = parse_compact_count(item.get("likes"))
    shares = parse_compact_count(item.get("shares"))
    comments = parse_compact_count(item.get("comments"))

    parts: list[str] = []
    if is_ad:
        parts.append("ads")
    if platform_id == "pinterest":
        if shares is not None and shares >= 0:
            parts.append(f"{format_compact_metric(shares)} saves")
        elif likes is not None and likes >= 0:
            parts.append(f"{format_compact_metric(likes)} likes")
    elif platform_id == "instagram":
        if likes is not None and likes >= 0:
            parts.append(f"{format_compact_metric(likes)} likes")
        elif views is not None and views >= 0:
            parts.append(f"{format_compact_metric(views)} views")
        elif comments is not None and comments > 0:
            parts.append(f"{format_compact_metric(comments)} comments")
    else:
        if views is not None and views >= 0:
            parts.append(f"{format_compact_metric(views)} views")
        elif likes is not None and likes >= 0:
            parts.append(f"{format_compact_metric(likes)} likes")
    return " · ".join(parts)


def default_home_url(platform_id: str) -> str:
    if platform_id == "pinterest":
        return "https://www.pinterest.com/"
    if platform_id == "tiktok":
        return "https://www.tiktok.com/"
    if platform_id == "instagram":
        return "https://www.instagram.com/explore/"
    if platform_id == "youtube":
        return "https://www.youtube.com/"
    return "about:blank"


def extract_profile_handle_from_url(raw_url: str, platform_id: str) -> str:
    text = (raw_url or "").strip()
    if not text:
        return ""
    try:
        parsed = urllib.parse.urlparse(text)
    except Exception:
        return ""
    host = (parsed.netloc or "").lower()
    segs = [s for s in (parsed.path or "").split("/") if s]
    if not segs:
        return ""
    seg0 = segs[0]
    if "tiktok." in host or platform_id == "tiktok":
        if seg0.startswith("@") and len(seg0) > 1 and len(segs) == 1:
            return seg0
        return ""
    if "pinterest." in host or platform_id == "pinterest":
        reserved = {"pin", "search", "ideas", "today", "explore", "login", "settings", "business", "_"}
        clean = seg0.lstrip("@")
        if clean and clean.lower() not in reserved:
            return f"@{clean}"
        return ""
    if "instagram." in host or platform_id == "instagram":
        reserved = {"p", "reel", "reels", "explore", "stories", "accounts", "direct", "tv"}
        clean = seg0.lstrip("@")
        if clean and clean.lower() not in reserved and len(segs) == 1:
            return f"@{clean}"
        return ""
    if "youtube." in host or platform_id == "youtube":
        if seg0.startswith("@") and len(seg0) > 1:
            return seg0
        if seg0 in ("c", "user", "channel") and len(segs) >= 2 and segs[1]:
            return f"@{segs[1]}"
    return ""


def parse_compact_count(raw: Optional[Any]) -> Optional[int]:
    if raw is None or isinstance(raw, bool):
        return None
    if isinstance(raw, (int, float)):
        return int(raw) if raw >= 0 else None
    text = str(raw).strip().replace("\u00a0", " ")
    if not text:
        return None
    # Strip trailing unit words such as "views", "vues", "likes", "saves"
    text = re.sub(
        r"\s*(?:views|view|vues|vue|likes|like|saves|save|enregistrements|spectateurs).*$",
        "",
        text,
        flags=re.IGNORECASE,
    ).strip()
    text = text.replace(" ", "")
    if not text:
        return None
    m = re.match(r"^([0-9]+(?:[.,][0-9]+)?)\s*([KkMmBb])?$", text)
    if not m:
        return None
    num_str, suffix = m.group(1), m.group(2)
    multipliers = {"k": 1_000, "m": 1_000_000, "b": 1_000_000_000}
    if suffix:
        value = float(num_str.replace(",", ".")) * multipliers[suffix.lower()]
        return int(value)
    digits = re.sub(r"[.,]", "", num_str)
    return int(digits) if digits.isdigit() else None


def normalize_publish_date(raw: Optional[Any]) -> str:
    """Normalizes timestamps (Unix epoch, ISO-8601, RFC-2822, YYYY-MM-DD, or
    relative strings like '2 weeks ago') into a 'YYYY-MM-DD' date string."""
    if raw is None or isinstance(raw, bool):
        return ""
    if isinstance(raw, (int, float)):
        ts = float(raw)
        if ts > 1e12:
            ts /= 1000.0
        if 1_200_000_000 <= ts <= 2_500_000_000:
            return datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).strftime("%Y-%m-%d")
        return ""

    text = str(raw).strip()
    if not text:
        return ""
    if text.isdigit():
        return normalize_publish_date(int(text))

    # Direct YYYY-M-D or YYYY-MM-DD prefix (covers ISO-8601 "2026-10-01T08:32:17Z")
    m_iso = re.match(r"^(\d{4})-(\d{1,2})-(\d{1,2})(?:[T\s]|$)", text)
    if m_iso:
        try:
            dt_iso = datetime.date(int(m_iso.group(1)), int(m_iso.group(2)), int(m_iso.group(3)))
            return dt_iso.strftime("%Y-%m-%d")
        except ValueError:
            pass

    # TikTok current-year month-day badge ("3-2", "11-18")
    m_md = re.match(r"^(\d{1,2})-(\d{1,2})$", text)
    if m_md:
        today = datetime.datetime.now(datetime.timezone.utc).date()
        month, day = int(m_md.group(1)), int(m_md.group(2))
        for year in (today.year, today.year - 1):
            try:
                cand_date = datetime.date(year, month, day)
                if cand_date <= today + datetime.timedelta(days=1):
                    return cand_date.strftime("%Y-%m-%d")
            except ValueError:
                continue

    # RFC-2822 (Pinterest Relay createdAt: "Thu, 01 Oct 2026 08:32:17 +0000")
    try:
        dt = email.utils.parsedate_to_datetime(text)
        if dt is not None:
            return dt.astimezone(datetime.timezone.utc).strftime("%Y-%m-%d")
    except Exception:
        pass

    # Relative age ("3 days ago", "2w ago", "5h ago", "1y ago", French "il y a 2 semaines")
    lower = text.lower()
    rel = re.search(
        r"(\d+)\s*(hour|hr|heure|day|jour|week|semaine|month|mois|year|an)",
        lower,
    )
    if rel:
        qty = int(rel.group(1))
        unit = rel.group(2)
        days = 0
        if unit.startswith(("day", "jour")):
            days = qty
        elif unit.startswith(("week", "semaine")):
            days = qty * 7
        elif unit.startswith(("month", "mois")):
            days = qty * 30
        elif unit.startswith(("year", "an")):
            days = qty * 365
        approx = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=days)
        return approx.strftime("%Y-%m-%d")

    rel_compact = re.match(r"^(\d+)\s*([smhdwyj])(?:\s*ago)?$", lower)
    if rel_compact:
        qty = int(rel_compact.group(1))
        unit_ch = rel_compact.group(2)
        days = 0
        if unit_ch in ("d", "j"):
            days = qty
        elif unit_ch == "w":
            days = qty * 7
        elif unit_ch == "y":
            days = qty * 365
        approx = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=days)
        return approx.strftime("%Y-%m-%d")

    return ""


def age_days_from_iso(published_at: str) -> Optional[int]:
    if not published_at or len(published_at) < 10:
        return None
    try:
        pub_date = datetime.date.fromisoformat(published_at[:10])
        today = datetime.datetime.now(datetime.timezone.utc).date()
        return max(0, (today - pub_date).days)
    except Exception:
        return None


def default_score_post(post: dict) -> int:
    views = post.get("views")
    likes = post.get("likes")
    shares = post.get("shares")
    comments = post.get("comments")
    engagement = (
        (likes if isinstance(likes, int) and likes > 0 else 0)
        + (shares if isinstance(shares, int) and shares > 0 else 0)
        + (comments if isinstance(comments, int) and comments > 0 else 0)
    )
    if engagement > 0:
        return engagement
    if isinstance(views, int) and views >= 0:
        return views
    return 0


def guess_extension(media_url: str, content_type: str = "") -> str:
    ct = (content_type or "").lower()
    if "video/mp4" in ct:
        return ".mp4"
    if "video/webm" in ct:
        return ".webm"
    if "image/png" in ct:
        return ".png"
    if "image/webp" in ct:
        return ".webp"
    if "image/jpeg" in ct or "image/jpg" in ct:
        return ".jpg"
    parsed = urllib.parse.urlparse(media_url)
    ext = os.path.splitext(parsed.path)[1].lower()
    if ext in (".jpg", ".jpeg", ".png", ".webp", ".mp4", ".mov", ".webm"):
        return ".jpg" if ext == ".jpeg" else ext
    return ".jpg"


def _write_qt_compatible_media(
    body: bytes,
    dest_prefix: str,
    ext: str,
    platform_id: str,
) -> str:
    """Writes media bytes to disk, converting WebP/AVIF images to JPEG so Qt 6
    installations without the optional ``libqwebp`` imageformat plugin can
    always display them natively."""
    is_webp = (
        ext == ".webp"
        or (len(body) >= 12 and body[:4] == b"RIFF" and body[8:12] == b"WEBP")
    )
    is_avif = len(body) >= 16 and body[4:12] in (b"ftypavif", b"ftypavis", b"ftypmif1")

    if is_webp or is_avif:
        jpg_dest = dest_prefix + ".jpg"
        try:
            from PIL import Image  # type: ignore

            with Image.open(io.BytesIO(body)) as img:
                if img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info):
                    rgba = img.convert("RGBA")
                    bg = Image.new("RGB", rgba.size, (255, 255, 255))
                    bg.paste(rgba, mask=rgba.split()[3])
                    bg.save(jpg_dest, format="JPEG", quality=92)
                else:
                    img.convert("RGB").save(jpg_dest, format="JPEG", quality=92)
            if os.path.isfile(jpg_dest) and os.path.getsize(jpg_dest) > 128:
                return jpg_dest
        except Exception as exc:
            log(platform_id, f"Pillow WebP/AVIF->JPEG conversion fallback ({exc})")

        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg:
            raw_tmp = dest_prefix + (".avif" if is_avif else ".webp")
            try:
                with open(raw_tmp, "wb") as f:
                    f.write(body)
                subprocess.run(
                    [ffmpeg, "-y", "-loglevel", "error", "-i", raw_tmp, "-frames:v", "1", jpg_dest],
                    check=False,
                    timeout=10,
                )
                if os.path.isfile(jpg_dest) and os.path.getsize(jpg_dest) > 128:
                    return jpg_dest
            except Exception as exc:
                log(platform_id, f"ffmpeg WebP/AVIF->JPEG conversion failed ({exc})")
            finally:
                try:
                    if os.path.isfile(raw_tmp):
                        os.remove(raw_tmp)
                except OSError:
                    pass

    dest = dest_prefix + ext
    with open(dest, "wb") as f:
        f.write(body)
    return dest


def download_media_file(
    context,
    media_url: str,
    referer_url: str,
    download_dir: str,
    index: int,
    platform_id: str,
    cookie_header: str = "",
    fallback_media_url: str = "",
) -> str:
    if not media_url or not download_dir:
        return ""
    os.makedirs(download_dir, exist_ok=True)
    digest = hashlib.sha1(media_url.encode("utf-8")).hexdigest()[:12]
    dest_prefix = os.path.join(download_dir, f"{platform_id}_{index:03d}_{digest}")

    try:
        if media_url.startswith("file://"):
            local_src = urllib.parse.unquote(urllib.parse.urlparse(media_url).path)
            if os.path.isfile(local_src):
                ext = guess_extension(local_src)
                with open(local_src, "rb") as sf:
                    return _write_qt_compatible_media(sf.read(), dest_prefix, ext, platform_id)
            return ""

        if os.path.isfile(media_url):
            ext = guess_extension(media_url)
            with open(media_url, "rb") as sf:
                return _write_qt_compatible_media(sf.read(), dest_prefix, ext, platform_id)
            return ""

        accept_hdr = "video/mp4,image/jpeg,image/png,*/*;q=0.8"

        # Try Playwright's authenticated request context first when available.
        if context is not None and media_url.startswith(("http://", "https://")):
            try:
                resp = context.request.get(
                    media_url,
                    headers={
                        "Referer": referer_url,
                        "User-Agent": USER_AGENT,
                        "Accept": accept_hdr,
                    },
                    timeout=20_000,
                )
                if resp.ok:
                    body = resp.body()
                    if body and len(body) > 128:
                        ct = resp.headers.get("content-type", "")
                        ext = guess_extension(media_url, ct)
                        return _write_qt_compatible_media(body, dest_prefix, ext, platform_id)
            except Exception as exc:
                log(platform_id, f"context.request.get failed for {media_url}: {exc}")

        if media_url.startswith(("http://", "https://")):
            headers = {
                "User-Agent": USER_AGENT,
                "Referer": referer_url,
                "Accept": accept_hdr,
            }
            if cookie_header:
                headers["Cookie"] = cookie_header
            req = urllib.request.Request(media_url, headers=headers)
            with urllib.request.urlopen(req, timeout=20) as resp:
                body = resp.read()
                if body and len(body) > 128:
                    ct = resp.headers.get("Content-Type", "")
                    ext = guess_extension(media_url, ct)
                    return _write_qt_compatible_media(body, dest_prefix, ext, platform_id)
    except Exception as exc:
        log(platform_id, f"download_media_file failed for {media_url}: {exc}")

    if fallback_media_url and fallback_media_url != media_url:
        return download_media_file(
            context,
            fallback_media_url,
            referer_url,
            download_dir,
            index,
            platform_id,
            cookie_header=cookie_header,
            fallback_media_url="",
        )
    return ""


DETECT_HUMAN_GATE_JS = r"""
([checkEmptyLoggedOut]) => {
    if (window.__qs_manual_pause === true) {
        return { kind: 'login', reason: 'Manual pause active', manual: true };
    }

    function isVisibleWithMinSize(el, minW, minH) {
        if (!el) return false;
        const style = window.getComputedStyle(el);
        if (!style || style.display === 'none' || style.visibility === 'hidden' || parseFloat(style.opacity) === 0) {
            return false;
        }
        const rect = el.getBoundingClientRect();
        if (rect.width < minW || rect.height < minH) return false;
        return rect.bottom > 0 && rect.right > 0
            && rect.top < window.innerHeight && rect.left < window.innerWidth;
    }

    function isSearchInput(el) {
        if (!el || el.tagName !== 'INPUT') return false;
        const type = (el.getAttribute('type') || 'text').toLowerCase();
        if (type === 'search') return true;
        const role = (el.getAttribute('role') || '').toLowerCase();
        if (role === 'searchbox') return true;
        if (el.name === 'q') return true;
        const attrs = [
            el.name,
            el.id,
            el.getAttribute('placeholder'),
            el.getAttribute('aria-label'),
            el.getAttribute('data-test-id'),
            el.getAttribute('data-e2e')
        ].map(v => (v || '').toLowerCase()).join(' ');
        return /\b(search|recherche|rechercher|search_query|searchbox)\b/.test(attrs);
    }

    const feedPostSelector = [
        'a[href*="/pin/"]',
        'a[href*="/video/"]',
        'a[href*="/photo/"]',
        'a[href*="/p/"]',
        'a[href*="/reel/"]',
        'a[href*="/shorts/"]',
        'a[href*="watch?v="]'
    ].join(', ');

    // 1. Explicit CAPTCHA / human-verification containers
    const captchaSelectors = [
        '#captcha-verify-container-main-page',
        '#captcha_container',
        '.captcha-verify-container',
        '[id*="captcha-verify" i]',
        '[class*="captcha-verify" i]',
        '[class*="secsdk-captcha" i]',
        '[id*="secsdk-captcha" i]',
        '#tiktok-verify-ele',
        '#px-captcha',
        '.cf-turnstile',
        '.h-captcha',
        '.g-recaptcha'
    ];
    for (const sel of captchaSelectors) {
        const nodes = document.querySelectorAll(sel);
        for (const el of nodes) {
            if (el.closest && el.closest('.grecaptcha-badge')) continue;
            if (isVisibleWithMinSize(el, 160, 55)) {
                return { kind: 'captcha', reason: 'CAPTCHA challenge visible' };
            }
        }
    }

    // 2. Visible CAPTCHA iframes (excluding passive reCAPTCHA v3 badge)
    const iframes = document.querySelectorAll('iframe');
    for (const frame of iframes) {
        if (frame.closest && frame.closest('.grecaptcha-badge')) continue;
        const style = window.getComputedStyle(frame);
        if (style && style.pointerEvents === 'none') continue;
        if (!isVisibleWithMinSize(frame, 180, 55)) continue;
        const meta = [
            frame.getAttribute('src'),
            frame.id,
            frame.name,
            frame.getAttribute('title')
        ].map(v => (v || '').toLowerCase()).join(' ');
        if (/recaptcha\/(api2|enterprise)\/bframe|hcaptcha\.com|challenges\.cloudflare\.com|turnstile|arkoselabs|funcaptcha|secsdk|captcha-verify|px-captcha|tiktok.*verify/.test(meta)) {
            return { kind: 'captcha', reason: 'CAPTCHA iframe visible' };
        }
        if (meta.includes('captcha') && isVisibleWithMinSize(frame, 200, 65)) {
            return { kind: 'captcha', reason: 'Verification iframe visible' };
        }
    }

    // 3. User actively typing / focusing a form field (never scroll away while typing)
    const active = document.activeElement;
    if (active && (!active.closest || (!active.closest('#__qs_spy_badge') && !active.closest('#__qs_picker_host')))) {
        const tag = (active.tagName || '').toUpperCase();
        if (tag === 'TEXTAREA' && isVisibleWithMinSize(active, 40, 16)) {
            return { kind: 'login', reason: 'Text field focused by user' };
        }
        if (tag === 'INPUT' && isVisibleWithMinSize(active, 40, 16) && !isSearchInput(active)) {
            const inputType = (active.getAttribute('type') || 'text').toLowerCase();
            if (!['hidden', 'checkbox', 'radio', 'submit', 'button', 'file', 'range', 'color'].includes(inputType)) {
                return { kind: 'login', reason: 'Input field focused by user' };
            }
        }
        if (active.isContentEditable && isVisibleWithMinSize(active, 40, 16)) {
            return { kind: 'login', reason: 'Editable field focused by user' };
        }
    }

    // 4. Visible login / signup / password / OTP inputs anywhere in the viewport
    const inputs = document.querySelectorAll('input');
    for (const inp of inputs) {
        if (isSearchInput(inp)) continue;
        if (!isVisibleWithMinSize(inp, 60, 16)) continue;
        const type = (inp.getAttribute('type') || 'text').toLowerCase();
        if (['hidden', 'checkbox', 'radio', 'submit', 'button', 'file', 'range', 'color'].includes(type)) {
            continue;
        }
        if (type === 'password' || type === 'email' || type === 'tel') {
            return { kind: 'login', reason: `Login ${type} field visible` };
        }
        const ac = (inp.getAttribute('autocomplete') || '').toLowerCase();
        if (/\b(username|current-password|new-password|one-time-code|email|tel)\b/.test(ac)) {
            return { kind: 'login', reason: 'Login autocomplete field visible' };
        }
        const nameId = [(inp.name || ''), (inp.id || '')].map(v => v.toLowerCase().trim());
        if (nameId.some(v => ['username', 'email', 'password', 'pass', 'passwd', 'login', 'phone', 'identifier', 'verificationcode', 'otp', 'passcode'].includes(v))) {
            return { kind: 'login', reason: 'Login input visible' };
        }
        const testId = (inp.getAttribute('data-test-id') || '').toLowerCase();
        if (testId === 'emailinputfield' || testId === 'passwordinputfield') {
            return { kind: 'login', reason: 'Pinterest login input visible' };
        }
        const labelHint = [
            inp.getAttribute('placeholder'),
            inp.getAttribute('aria-label')
        ].map(v => (v || '').toLowerCase()).join(' ');
        if (/password|mot de passe|e-mail|email|username|nom d'utilisateur|phone number|numéro de téléphone|verification code|code de vérification|enter code|entrez le code/.test(labelHint)) {
            return { kind: 'login', reason: 'Login prompt field visible' };
        }
    }

    // 5. Known platform login / signup / consent modals (even before inputs are revealed)
    const loginModalSelectors = [
        '#loginContainer',
        '#login-modal',
        '[data-e2e="login-modal"]',
        '[data-test-id="fullPageSignupModal"]',
        '[data-test-id="giftWrap"]',
        '[data-test-id="simple-signup-modal"]',
        '[data-test-id="signup-default-modal"]',
        '[data-test-id="login-modal"]',
        '[data-test-id="register-modal"]',
        '[data-test-id="persona-signup-modal"]',
        '[data-test-id="unauth-home-page"]',
        'ytd-consent-bump-v2-lightbox'
    ];
    for (const sel of loginModalSelectors) {
        const nodes = document.querySelectorAll(sel);
        for (const el of nodes) {
            if (isVisibleWithMinSize(el, 200, 120)) {
                return { kind: 'login', reason: 'Platform login/signup modal visible' };
            }
        }
    }

    const captchaTextRe = /verify to continue|vérifier pour continuer|drag the puzzle|fit the puzzle|drag the slider|slide to verify|faites glisser|select 2 objects|select two objects|sélectionnez 2 objets|confirm you are human|verify you are human|confirmez que vous êtes humain|security verification|vérification de sécurité|press & hold|press and hold to confirm/i;

    const loginDialogRe = /log in to tiktok|sign up for tiktok|connecte-toi à tiktok|connexion à tiktok|log in to instagram|see more on instagram|sign up to see|connectez-vous à instagram|welcome to pinterest|log in to see more|sign up to get your ideas|bienvenue sur pinterest|connectez-vous pour voir|sign in to youtube|before you continue to youtube|avant de continuer sur youtube|use phone \/ email \/ username|utiliser un téléphone|use qr code/i;

    // 6. Generic modal dialogs & full-screen fixed overlays
    const dialogs = Array.from(document.querySelectorAll('[role="dialog"], dialog, [aria-modal="true"]'));
    for (const dlg of dialogs) {
        if (!isVisibleWithMinSize(dlg, 200, 120)) continue;
        if (dlg.querySelector(feedPostSelector)) continue;
        const txt = (dlg.innerText || '').slice(0, 2500);
        if (captchaTextRe.test(txt)) {
            return { kind: 'captcha', reason: 'Verification dialog visible' };
        }
        if (loginDialogRe.test(txt)) {
            return { kind: 'login', reason: 'Login dialog visible' };
        }
        const lower = txt.toLowerCase();
        if ((/\blog in\b|\bsign in\b|\bse connecter\b|\bconnexion\b/.test(lower))
            && (/continue with google|continuer avec google|continue with facebook|forgot password|mot de passe oublié/.test(lower))) {
            return { kind: 'login', reason: 'Sign-in chooser dialog visible' };
        }
    }

    // Check large fixed blocking overlays without role="dialog"
    const fixedCandidates = document.querySelectorAll('div, section, aside');
    for (const el of fixedCandidates) {
        if (el.id === '__qs_spy_badge' || el.id === '__qs_picker_host') continue;
        const style = window.getComputedStyle(el);
        if (!style || style.position !== 'fixed') continue;
        if (!isVisibleWithMinSize(el, window.innerWidth * 0.5, window.innerHeight * 0.35)) continue;
        if (el.querySelector(feedPostSelector)) continue;
        const rawText = (el.innerText || '').trim();
        if (!rawText || rawText.length > 2500) continue;
        if (captchaTextRe.test(rawText)) {
            return { kind: 'captcha', reason: 'Blocking verification overlay visible' };
        }
        if (loginDialogRe.test(rawText)) {
            return { kind: 'login', reason: 'Blocking login overlay visible' };
        }
    }

    // 7. Page-level challenge or signed-out empty wall (when 0 feed posts exist)
    const hasAnyFeedPosts = Boolean(document.querySelector(feedPostSelector));
    if (!hasAnyFeedPosts && document.body) {
        const bodyText = (document.body.innerText || '').slice(0, 4000);
        if (captchaTextRe.test(bodyText)) {
            return { kind: 'captcha', reason: 'Verification challenge page' };
        }
        if (checkEmptyLoggedOut) {
            if (loginDialogRe.test(bodyText)
                || /\b(log in|sign in|se connecter|connexion)\b/i.test(bodyText)) {
                return { kind: 'login', reason: 'No posts visible while signed out — waiting for login' };
            }
        }
    }

    return null;
}
"""


def update_status_badge(
    page,
    saved_count: int,
    max_posts: int,
    browsed_count: int,
    rejected_count: int,
    min_score: int,
    mode: str,
    paused_kind: Optional[str] = None,
    manual_pause: bool = False,
    last_badge_state: Optional[list] = None,
) -> None:
    desired_state = [
        saved_count,
        max_posts,
        browsed_count,
        rejected_count,
        min_score,
        mode,
        paused_kind or "",
        bool(manual_pause),
    ]
    if last_badge_state is not None and last_badge_state == desired_state:
        return
    try:
        page.evaluate(
            """([savedCount, maxPosts, browsedCount, rejectedCount, minScore, mode, pausedKind, manualPause]) => {
                if (!document.body) return;
                let badge = document.getElementById('__qs_spy_badge');
                if (!badge) {
                    badge = document.createElement('div');
                    badge.id = '__qs_spy_badge';
                    badge.style.cssText = [
                        'position: fixed',
                        'bottom: 18px',
                        'right: 18px',
                        'z-index: 2147483647',
                        'padding: 10px 14px',
                        'border-radius: 8px',
                        'font: 600 13px/1.35 system-ui, -apple-system, sans-serif',
                        'box-shadow: 0 6px 20px rgba(0, 0, 0, 0.35)',
                        'pointer-events: none',
                        'user-select: none',
                        'display: flex',
                        'align-items: center',
                        'gap: 10px',
                        'max-width: min(640px, calc(100vw - 36px))'
                    ].join(';');

                    const label = document.createElement('span');
                    label.id = '__qs_spy_badge_text';
                    badge.appendChild(label);

                    const btn = document.createElement('button');
                    btn.id = '__qs_spy_badge_btn';
                    btn.type = 'button';
                    btn.style.cssText = [
                        'pointer-events: auto',
                        'cursor: pointer',
                        'border: 1px solid rgba(255, 255, 255, 0.35)',
                        'border-radius: 5px',
                        'padding: 3px 8px',
                        'font: 600 12px/1.2 system-ui, -apple-system, sans-serif',
                        'background: rgba(255, 255, 255, 0.14)',
                        'color: #ffffff',
                        'white-space: nowrap'
                    ].join(';');
                    btn.addEventListener('click', (ev) => {
                        ev.preventDefault();
                        ev.stopPropagation();
                        window.__qs_manual_pause = !window.__qs_manual_pause;
                    });
                    badge.appendChild(btn);
                    document.body.appendChild(badge);
                }

                const label = document.getElementById('__qs_spy_badge_text');
                const btn = document.getElementById('__qs_spy_badge_btn');
                const rejPct = browsedCount > 0 ? Math.round((100 * rejectedCount) / browsedCount) : 0;
                const progressPart = (browsedCount > 0 && (minScore > 0 || rejectedCount > 0))
                    ? `Saved ${savedCount}/${maxPosts} · Browsed ${browsedCount} · Rejected ${rejectedCount} (${rejPct}%)`
                    : `${savedCount} / ${maxPosts} posts`;

                if (pausedKind) {
                    badge.style.background = 'rgba(146, 64, 14, 0.95)';
                    badge.style.color = '#fffbeb';
                    badge.style.border = '1px solid rgba(251, 191, 36, 0.75)';
                    if (manualPause) {
                        label.textContent = `QuantumSocial Spy: PAUSED (${progressPart}) — Log in or browse freely, then click Resume`;
                    } else if (pausedKind === 'captcha') {
                        label.textContent = `QuantumSocial Spy: PAUSED (${progressPart}) — Solve CAPTCHA / verification to continue`;
                    } else {
                        label.textContent = `QuantumSocial Spy: PAUSED (${progressPart}) — Complete login in Chrome (resumes automatically)`;
                    }
                    if (btn) {
                        btn.style.display = manualPause ? 'inline-block' : 'none';
                        btn.textContent = 'Resume';
                    }
                } else {
                    badge.style.background = 'rgba(20, 24, 33, 0.92)';
                    badge.style.color = '#f5f7fa';
                    badge.style.border = '1px solid rgba(255, 255, 255, 0.2)';
                    const hint = mode === 'manual'
                        ? 'close window when done'
                        : 'close anytime to save';
                    label.textContent = `QuantumSocial Spy: ${progressPart} (${hint})`;
                    if (btn) {
                        btn.style.display = mode === 'auto' ? 'inline-block' : 'none';
                        btn.textContent = 'Pause to log in';
                    }
                }
            }""",
            desired_state,
        )
        if last_badge_state is not None:
            last_badge_state[:] = desired_state
    except Exception:
        if last_badge_state is not None:
            last_badge_state.clear()


EXTRACT_OPEN_POST_FALLBACK_JS = r"""
() => {
    const href = window.location.href || '';
    const postRe = /(\/pin\/[0-9A-Za-z_-]+|\/@[^/?#]+\/(?:video|photo)\/\d{6,24}|\/(?:p|reel)\/[0-9A-Za-z_-]+|\/shorts\/[0-9A-Za-z_-]{6,}|[?&]v=[0-9A-Za-z_-]{6,})/;
    if (!postRe.test(href)) return null;

    const ogTitle = document.querySelector('meta[property="og:title"], meta[name="twitter:title"]');
    const ogImg = document.querySelector('meta[property="og:image"], meta[name="twitter:image"]');
    const vid = document.querySelector('video');
    let mediaUrl = '';
    if (vid) {
        const srcEl = vid.querySelector('source[src]');
        const vsrc = vid.currentSrc || vid.getAttribute('src') || (srcEl ? srcEl.getAttribute('src') : '') || '';
        if (vsrc && !vsrc.startsWith('blob:') && !vsrc.startsWith('data:')) {
            mediaUrl = vsrc;
        } else if (vid.getAttribute('poster')) {
            mediaUrl = vid.getAttribute('poster') || '';
        }
    }
    if (!mediaUrl && ogImg && ogImg.getAttribute('content')) {
        mediaUrl = ogImg.getAttribute('content') || '';
    }
    if (!mediaUrl) {
        const shortsM = href.match(/\/shorts\/([0-9A-Za-z_-]{6,})/);
        const watchM = href.match(/[?&]v=([0-9A-Za-z_-]{6,})/);
        const ytId = shortsM ? shortsM[1] : (watchM ? watchM[1] : '');
        if (ytId) mediaUrl = 'https://i.ytimg.com/vi/' + ytId + '/hqdefault.jpg';
    }

    let title = (ogTitle ? (ogTitle.getAttribute('content') || '') : '') || document.title || href;
    title = title.replace(/\s*[-|•]\s*(TikTok|Pinterest|Instagram|YouTube).*$/i, '').trim();

    let author = document.body ? (document.body.getAttribute('data-author') || '') : '';
    if (!author) {
        const tm = href.match(/tiktok\.com\/(@[^/?#]+)/i);
        if (tm) author = tm[1];
    }
    if (!author) {
        const authorLink = document.querySelector('a[href^="/@"], [data-e2e="browse-username"], ytd-channel-name a');
        if (authorLink) {
            const txt = (authorLink.innerText || authorLink.getAttribute('href') || '').trim().replace(/^\//, '');
            if (txt) author = txt.startsWith('@') ? txt.split('/')[0] : ('@' + txt.split(/\s+/)[0]);
        }
    }

    return {
        url: href,
        title: (title || href).slice(0, 200),
        media_url: mediaUrl,
        author: author,
        published_at: document.body ? (document.body.getAttribute('data-created') || '') : '',
        views: document.body ? document.body.getAttribute('data-views') : null,
        likes: document.body ? document.body.getAttribute('data-likes') : null,
        comments: document.body ? document.body.getAttribute('data-comments') : null,
        shares: document.body ? (document.body.getAttribute('data-saves') || document.body.getAttribute('data-shares')) : null,
        is_ad: false
    };
}
"""


INSTALL_HUMAN_PICKER_JS = r"""
([platformId, initialState]) => {
    if (window.__qs_picker_installed) {
        if (window.__qs_picker_receive_state && initialState) {
            window.__qs_picker_receive_state(initialState);
        }
        return;
    }
    window.__qs_picker_installed = true;

    const POST_SELECTOR = [
        'a[href*="/pin/"]',
        'a[href*="/video/"]',
        'a[href*="/photo/"]',
        'a[href*="/p/"]',
        'a[href*="/reel/"]',
        'a[href*="/shorts/"]',
        'a[href*="watch?v="]'
    ].join(', ');

    const state = {
        picked: [],
        pickedSet: new Set(),
        savedKnownSet: new Set(),
        rejectedKnownSet: new Set(),
        starredSet: new Set(),
        liveStats: {},
        trayOpen: false
    };

    function canonicalizeUrl(rawUrl) {
        let text = String(rawUrl || '').trim();
        if (!text) return '';
        let u;
        try {
            u = new URL(text, window.location.href);
        } catch (_) {
            return '';
        }
        if (u.protocol === 'file:') {
            const p = u.pathname || '';
            if (/^\/pin\/[0-9A-Za-z_-]+/.test(p)) {
                u = new URL('https://www.pinterest.com' + p);
            } else if (/^\/@[^/]+\/(?:video|photo)\/\d+/.test(p)) {
                u = new URL('https://www.tiktok.com' + p);
            } else if (/^\/(?:p|reel)\/[0-9A-Za-z_-]+/.test(p)) {
                u = new URL('https://www.instagram.com' + p);
            } else if (/^\/shorts\/[0-9A-Za-z_-]+/.test(p)) {
                u = new URL('https://www.youtube.com' + p);
            } else {
                return text;
            }
        }
        if (u.protocol !== 'http:' && u.protocol !== 'https:') return '';

        let host = u.hostname.toLowerCase();
        if (/^(?:[a-z0-9-]+\.)?pinterest\.[a-z.]+$/.test(host)) host = 'www.pinterest.com';
        else if (host === 'tiktok.com' || host === 'm.tiktok.com') host = 'www.tiktok.com';
        else if (host === 'instagram.com' || host === 'm.instagram.com') host = 'www.instagram.com';
        else if (host === 'youtube.com' || host === 'm.youtube.com') host = 'www.youtube.com';

        let path = u.pathname || '';
        if (host === 'www.pinterest.com') {
            const pm = path.match(/\/pin\/([0-9A-Za-z_-]+)/);
            if (pm) path = '/pin/' + pm[1];
        } else if (host === 'www.instagram.com') {
            const im = path.match(/\/(p|reel)\/([0-9A-Za-z_-]+)/);
            if (im) path = '/' + im[1] + '/' + im[2];
        } else if (host === 'www.youtube.com') {
            const sm = path.match(/\/shorts\/([0-9A-Za-z_-]{6,})/);
            if (sm) {
                return 'https://www.youtube.com/shorts/' + sm[1];
            }
            const vid = u.searchParams.get('v');
            if (vid) {
                return 'https://www.youtube.com/watch?v=' + vid;
            }
        } else if (host === 'www.tiktok.com') {
            u.search = '';
        }

        const params = new URLSearchParams(u.search);
        for (const k of Array.from(params.keys())) {
            if (k.startsWith('utm_') || k === 'igsh' || k === '_t' || k === '_r') {
                params.delete(k);
            }
        }
        while (path.length > 1 && path.endsWith('/')) {
            path = path.slice(0, -1);
        }
        const qs = host === 'www.pinterest.com' || host === 'www.instagram.com' || host === 'www.tiktok.com'
            ? ''
            : params.toString();
        return 'https://' + host + path + (qs ? '?' + qs : '');
    }

    function applyIncomingState(incoming) {
        if (!incoming || typeof incoming !== 'object') return;
        if (Array.isArray(incoming.picked)) {
            state.picked = incoming.picked;
            state.pickedSet = new Set(incoming.picked.map(it => it.url));
        }
        if (Array.isArray(incoming.saved_known)) {
            state.savedKnownSet = new Set(incoming.saved_known);
        }
        if (Array.isArray(incoming.rejected_known)) {
            state.rejectedKnownSet = new Set(incoming.rejected_known);
        }
        if (Array.isArray(incoming.starred)) {
            state.starredSet = new Set(incoming.starred.map(s => String(s).toLowerCase()));
        }
        if (incoming.live_stats && typeof incoming.live_stats === 'object') {
            Object.assign(state.liveStats, incoming.live_stats);
        }
        renderAll();
    }
    window.__qs_picker_receive_state = applyIncomingState;

    function formatCompactNum(raw) {
        if (raw === null || raw === undefined || raw === '') return '';
        const s = String(raw).trim();
        if (!s) return '';
        if (/[KkMmBb]$/.test(s)) return s.toUpperCase();
        const n = Number(s.replace(/[,\s]/g, ''));
        if (!Number.isFinite(n) || n < 0) return '';
        if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
        if (n >= 1000) return (n / 1000).toFixed(1) + 'K';
        return String(Math.round(n));
    }

    function findOuterCard(anchor) {
        const explicit = anchor.closest(
            '[data-test-id="pin"], [data-test-id="pinWrapper"], [class*="DivItemContainer"], '
            + '[data-e2e="user-post-item"], [data-e2e="search_video-item"], '
            + 'ytd-video-renderer, ytd-reel-item-renderer, ytd-rich-item-renderer, ytm-shorts-lockup-view-model, article'
        );
        if (explicit) return explicit;
        let cur = anchor;
        for (let i = 0; i < 4 && cur.parentElement && cur.parentElement !== document.body; i++) {
            const p = cur.parentElement;
            if (p.querySelectorAll(POST_SELECTOR).length > 2) break;
            cur = p;
        }
        return cur;
    }

    function getCardRect(anchor, card) {
        const media = anchor.querySelector('img, video') || (card ? card.querySelector('img, video') : null);
        const candidates = [media, card, anchor].filter(Boolean);
        let best = null;
        for (const el of candidates) {
            const r = el.getBoundingClientRect();
            if (r.width >= 48 && r.height >= 48) {
                if (!best || (r.width * r.height) > (best.width * best.height)) {
                    // Avoid full-width row containers when media thumbnail is narrower
                    if (best && r.width > window.innerWidth * 0.75) continue;
                    best = r;
                }
            }
        }
        return best || anchor.getBoundingClientRect();
    }

    function extractCardHint(anchor, card, canonicalUrl) {
        const img = anchor.querySelector('img') || (card ? card.querySelector('img') : null);
        const vid = anchor.querySelector('video') || (card ? card.querySelector('video') : null);
        const dataVideo = anchor.getAttribute('data-video-url') || (card ? card.getAttribute('data-video-url') : '') || '';
        let mediaUrl = dataVideo;
        if (!mediaUrl && vid) {
            const srcEl = vid.querySelector('source[src]');
            const vs = vid.currentSrc || vid.getAttribute('src') || (srcEl ? srcEl.getAttribute('src') : '') || '';
            if (vs && !vs.startsWith('blob:') && !vs.startsWith('data:')) mediaUrl = vs;
        }
        let imgUrl = img ? (img.currentSrc || img.src || img.getAttribute('src') || '') : '';
        if (imgUrl.startsWith('blob:') || imgUrl.startsWith('data:')) imgUrl = '';
        if (!mediaUrl) mediaUrl = imgUrl;

        let title = (
            anchor.getAttribute('aria-label')
            || anchor.getAttribute('title')
            || (img ? img.getAttribute('alt') : '')
            || ''
        ).trim();
        if (!title && card) {
            const cap = card.querySelector('[data-e2e="search-card-video-caption"], #video-title, h3');
            if (cap && cap.innerText) title = cap.innerText.split('\n')[0].trim();
        }
        title = title.replace(/\s*\bpin\s+page\s*$/i, '').trim();

        let author = anchor.getAttribute('data-author') || (card ? card.getAttribute('data-author') : '') || '';
        if (!author) {
            const tm = canonicalUrl.match(/tiktok\.com\/(@[^/?#]+)/i);
            if (tm) author = tm[1];
        }
        if (!author && card) {
            const uEl = card.querySelector('[data-e2e="search-card-user-unique-id"], ytd-channel-name a');
            if (uEl && uEl.innerText) {
                const u = uEl.innerText.trim().split('\n')[0].trim();
                if (u) author = u.startsWith('@') ? u : ('@' + u.replace(/\s+/g, ''));
            }
        }

        const saves = anchor.getAttribute('data-saves') || (card ? card.getAttribute('data-saves') : null)
            || anchor.getAttribute('data-shares') || (card ? card.getAttribute('data-shares') : null);
        const likes = anchor.getAttribute('data-likes') || (card ? card.getAttribute('data-likes') : null);
        let views = anchor.getAttribute('data-views') || (card ? card.getAttribute('data-views') : null);
        if (!views && card) {
            const vEl = card.querySelector('[data-e2e="video-views"], strong[data-e2e="video-views"]');
            if (vEl && vEl.innerText) views = vEl.innerText.trim();
        }
        const comments = anchor.getAttribute('data-comments') || (card ? card.getAttribute('data-comments') : null);
        const created = anchor.getAttribute('data-created') || (card ? card.getAttribute('data-created') : '') || '';
        const isAd = (
            anchor.getAttribute('data-promoted') === 'true'
            || anchor.getAttribute('data-ad') === 'true'
            || (card && (card.getAttribute('data-promoted') === 'true' || card.getAttribute('data-ad') === 'true'))
        );

        let domStats = '';
        if (state.liveStats[canonicalUrl]) {
            domStats = state.liveStats[canonicalUrl];
        } else {
            const parts = [];
            if (isAd) parts.push('ads');
            if (saves !== null && saves !== '') {
                const fs = formatCompactNum(saves);
                if (fs) parts.push(fs + (platformId === 'pinterest' ? ' saves' : ' shares'));
            } else if (views !== null && views !== '') {
                const fv = formatCompactNum(views);
                if (fv) parts.push(fv + ' views');
            } else if (likes !== null && likes !== '') {
                const fl = formatCompactNum(likes);
                if (fl) parts.push(fl + ' likes');
            }
            domStats = parts.join(' · ');
        }

        return {
            url: canonicalUrl,
            title: (title || canonicalUrl).slice(0, 200),
            media_url: mediaUrl,
            fallback_media_url: dataVideo ? imgUrl : '',
            author: author,
            published_at: created,
            views: views,
            likes: likes,
            comments: comments,
            shares: saves,
            is_ad: Boolean(isAd),
            stats_hint: domStats
        };
    }

    function detectOpenPostUrl() {
        const href = window.location.href || '';
        if (!/(\/pin\/[0-9A-Za-z_-]+|\/@[^/?#]+\/(?:video|photo)\/\d{6,24}|\/(?:p|reel)\/[0-9A-Za-z_-]+|\/shorts\/[0-9A-Za-z_-]{6,}|[?&]v=[0-9A-Za-z_-]{6,})/.test(href)) {
            return '';
        }
        return canonicalizeUrl(href);
    }

    function detectPageCreatorHandle() {
        const href = window.location.href || '';
        let u;
        try { u = new URL(href); } catch (_) { return ''; }
        const host = u.hostname.toLowerCase();
        const segs = (u.pathname || '').split('/').filter(Boolean);
        if (u.protocol === 'file:') {
            const bodyAuth = document.body ? (document.body.getAttribute('data-profile-handle') || document.body.getAttribute('data-author') || '') : '';
            if (bodyAuth) return bodyAuth.startsWith('@') ? bodyAuth : ('@' + bodyAuth);
            return '';
        }
        if (segs.length === 0) return '';
        const s0 = segs[0];
        if (host.includes('tiktok.') && s0.startsWith('@') && s0.length > 1) return s0;
        if (host.includes('pinterest.')) {
            const res = new Set(['pin', 'search', 'ideas', 'today', 'explore', 'login', 'settings', 'business', '_']);
            const c = s0.replace(/^@/, '');
            if (c && !res.has(c.toLowerCase())) return '@' + c;
        }
        if (host.includes('instagram.')) {
            const res = new Set(['p', 'reel', 'reels', 'explore', 'stories', 'accounts', 'direct', 'tv']);
            const c = s0.replace(/^@/, '');
            if (c && !res.has(c.toLowerCase())) return '@' + c;
        }
        if (host.includes('youtube.')) {
            if (s0.startsWith('@') && s0.length > 1) return s0;
            if ((s0 === 'c' || s0 === 'user' || s0 === 'channel') && segs[1]) return '@' + segs[1];
        }
        return '';
    }

    let host = null;
    let shadow = null;
    let cardLayer = null;
    let hoverPill = null;
    let trayEl = null;
    let hudEl = null;
    let trayToggleBtn = null;
    let openPostBtn = null;
    let starCreatorBtn = null;
    let finishBtn = null;
    let hoveredTarget = null; // { url, rect, hint }

    function ensureDom() {
        if (host && host.isConnected) return true;
        const mount = document.body || document.documentElement;
        if (!mount) return false;

        host = document.createElement('div');
        host.id = '__qs_picker_host';
        host.style.cssText = 'position:fixed;inset:0;pointer-events:none;z-index:2147483646;font-family:system-ui,-apple-system,sans-serif;';
        shadow = host.attachShadow({ mode: 'open' });

        const style = document.createElement('style');
        style.textContent = [
            '* { box-sizing: border-box; font-family: system-ui, -apple-system, sans-serif; }',
            '.qs-card-ring { position: fixed; pointer-events: none; border: 3px solid #2ea043; border-radius: 10px; box-shadow: 0 0 0 1px rgba(0,0,0,0.45), inset 0 0 0 1px rgba(46,160,67,0.4); }',
            '.qs-badge { position: fixed; padding: 4px 9px; border-radius: 999px; font-size: 12px; font-weight: 700; line-height: 1.25; box-shadow: 0 3px 10px rgba(0,0,0,0.38); user-select: none; white-space: nowrap; }',
            '.qs-badge-picked { pointer-events: auto; cursor: pointer; background: #2ea043; color: #ffffff; border: 1px solid rgba(255,255,255,0.45); }',
            '.qs-badge-known { pointer-events: none; background: rgba(31, 111, 235, 0.88); color: #ffffff; border: 1px solid rgba(255,255,255,0.3); }',
            '.qs-badge-rejected { pointer-events: none; background: rgba(90, 96, 108, 0.88); color: #e6edf3; border: 1px solid rgba(255,255,255,0.22); }',
            '#qs-hover-pill { position: fixed; display: none; pointer-events: auto; cursor: pointer; padding: 6px 12px; border-radius: 999px; font-size: 12.5px; font-weight: 700; line-height: 1.25; color: #ffffff; background: rgba(17, 24, 39, 0.92); border: 1.5px solid #58a6ff; box-shadow: 0 4px 14px rgba(0,0,0,0.45); white-space: nowrap; transition: transform 0.08s ease; }',
            '#qs-hover-pill:hover { transform: scale(1.04); background: #1f6feb; border-color: #ffffff; }',
            '#qs-hover-pill.is-picked { background: #2ea043; border-color: #ffffff; }',
            '#qs-hud { position: fixed; bottom: 18px; right: 18px; pointer-events: auto; display: flex; align-items: center; gap: 8px; flex-wrap: wrap; padding: 9px 13px; border-radius: 10px; background: rgba(15, 20, 28, 0.95); color: #f0f6fc; border: 1px solid rgba(88, 166, 255, 0.45); box-shadow: 0 8px 26px rgba(0,0,0,0.48); max-width: min(780px, calc(100vw - 32px)); }',
            '.qs-hud-btn { cursor: pointer; border-radius: 6px; padding: 5px 10px; font-size: 12.5px; font-weight: 600; line-height: 1.25; border: 1px solid rgba(255,255,255,0.24); background: rgba(255,255,255,0.10); color: #f0f6fc; white-space: nowrap; }',
            '.qs-hud-btn:hover { background: rgba(255,255,255,0.20); }',
            '.qs-hud-primary { background: #2ea043; border-color: rgba(255,255,255,0.35); color: #ffffff; font-weight: 700; }',
            '.qs-hud-primary:hover { background: #3fb950; }',
            '.qs-hud-accent { background: rgba(31, 111, 235, 0.85); border-color: rgba(121, 192, 255, 0.5); color: #ffffff; }',
            '.qs-hud-starred { background: rgba(187, 128, 9, 0.90); border-color: rgba(251, 191, 36, 0.65); color: #fffbeb; }',
            '#qs-tray { position: fixed; bottom: 70px; right: 18px; pointer-events: auto; display: none; flex-direction: column; gap: 5px; width: min(380px, calc(100vw - 32px)); max-height: 260px; overflow-y: auto; padding: 10px; border-radius: 10px; background: rgba(15, 20, 28, 0.96); color: #f0f6fc; border: 1px solid rgba(88,166,255,0.4); box-shadow: 0 8px 24px rgba(0,0,0,0.45); }',
            '.qs-tray-row { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 4px 6px; border-radius: 5px; background: rgba(255,255,255,0.06); font-size: 12px; }',
            '.qs-tray-title { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; }',
            '.qs-tray-rm { cursor: pointer; border: none; border-radius: 4px; padding: 2px 6px; background: rgba(248,81,73,0.25); color: #ffa198; font-weight: 700; }'
        ].join('\n');
        shadow.appendChild(style);

        cardLayer = document.createElement('div');
        cardLayer.id = 'qs-card-overlays';
        shadow.appendChild(cardLayer);

        hoverPill = document.createElement('button');
        hoverPill.id = 'qs-hover-pill';
        hoverPill.type = 'button';
        hoverPill.addEventListener('click', (ev) => {
            ev.preventDefault();
            ev.stopPropagation();
            if (hoveredTarget && hoveredTarget.url) {
                invokeAction({ action: 'toggle_pick', url: hoveredTarget.url, card_hint: hoveredTarget.hint });
            }
        });
        shadow.appendChild(hoverPill);

        trayEl = document.createElement('div');
        trayEl.id = 'qs-tray';
        shadow.appendChild(trayEl);

        hudEl = document.createElement('div');
        hudEl.id = 'qs-hud';

        const brand = document.createElement('span');
        brand.style.cssText = 'font-size:12.5px;font-weight:700;color:#79c0ff;margin-right:2px;';
        brand.textContent = 'QuantumSocial Pick';
        hudEl.appendChild(brand);

        trayToggleBtn = document.createElement('button');
        trayToggleBtn.id = 'qs-tray-toggle';
        trayToggleBtn.className = 'qs-hud-btn';
        trayToggleBtn.type = 'button';
        trayToggleBtn.title = 'Click to view or remove picked posts (Alt+Click any card to pick fast)';
        trayToggleBtn.addEventListener('click', (ev) => {
            ev.preventDefault();
            ev.stopPropagation();
            state.trayOpen = !state.trayOpen;
            renderTray();
        });
        hudEl.appendChild(trayToggleBtn);

        openPostBtn = document.createElement('button');
        openPostBtn.id = 'qs-pick-open-btn';
        openPostBtn.className = 'qs-hud-btn qs-hud-accent';
        openPostBtn.type = 'button';
        openPostBtn.style.display = 'none';
        openPostBtn.addEventListener('click', (ev) => {
            ev.preventDefault();
            ev.stopPropagation();
            const openUrl = detectOpenPostUrl();
            if (openUrl) {
                invokeAction({ action: 'toggle_pick', url: openUrl, card_hint: { url: openUrl, title: document.title || openUrl } });
            }
        });
        hudEl.appendChild(openPostBtn);

        starCreatorBtn = document.createElement('button');
        starCreatorBtn.id = 'qs-star-creator-btn';
        starCreatorBtn.className = 'qs-hud-btn';
        starCreatorBtn.type = 'button';
        starCreatorBtn.style.display = 'none';
        starCreatorBtn.addEventListener('click', (ev) => {
            ev.preventDefault();
            ev.stopPropagation();
            const h = starCreatorBtn.getAttribute('data-handle') || '';
            if (h) {
                invokeAction({ action: 'toggle_star', handle: h, profile_url: window.location.href || '' });
            }
        });
        hudEl.appendChild(starCreatorBtn);

        finishBtn = document.createElement('button');
        finishBtn.id = 'qs-finish-btn';
        finishBtn.className = 'qs-hud-btn qs-hud-primary';
        finishBtn.type = 'button';
        finishBtn.addEventListener('click', (ev) => {
            ev.preventDefault();
            ev.stopPropagation();
            finishBtn.textContent = 'Saving…';
            invokeAction({ action: 'finish' });
        });
        hudEl.appendChild(finishBtn);

        shadow.appendChild(hudEl);
        mount.appendChild(host);
        return true;
    }

    function invokeAction(payload) {
        if (typeof window.__qs_py_action !== 'function') return;
        Promise.resolve(window.__qs_py_action(payload)).then((nextState) => {
            if (nextState) applyIncomingState(nextState);
        }).catch(() => {});
    }

    function renderTray() {
        if (!trayEl) return;
        while (trayEl.firstChild) trayEl.removeChild(trayEl.firstChild);
        if (!state.trayOpen || state.picked.length === 0) {
            trayEl.style.display = 'none';
            return;
        }
        trayEl.style.display = 'flex';
        for (const item of state.picked) {
            const row = document.createElement('div');
            row.className = 'qs-tray-row';
            const lbl = document.createElement('span');
            lbl.className = 'qs-tray-title';
            lbl.textContent = (item.author ? (item.author + ' — ') : '') + (item.title || item.url);
            row.appendChild(lbl);
            const rm = document.createElement('button');
            rm.className = 'qs-tray-rm';
            rm.type = 'button';
            rm.textContent = '✕';
            rm.addEventListener('click', (ev) => {
                ev.preventDefault();
                ev.stopPropagation();
                invokeAction({ action: 'toggle_pick', url: item.url, card_hint: item });
            });
            row.appendChild(rm);
            trayEl.appendChild(row);
        }
    }

    function renderHoverPill() {
        if (!hoverPill) return;
        if (!hoveredTarget || !hoveredTarget.url) {
            hoverPill.style.display = 'none';
            return;
        }
        const r = hoveredTarget.rect;
        if (!r || r.width < 40 || r.height < 40 || r.bottom < 20 || r.top > window.innerHeight - 20) {
            hoverPill.style.display = 'none';
            return;
        }
        const isPicked = state.pickedSet.has(hoveredTarget.url);
        const statsHint = state.liveStats[hoveredTarget.url] || (hoveredTarget.hint && hoveredTarget.hint.stats_hint) || '';
        hoverPill.className = isPicked ? 'is-picked' : '';
        if (isPicked) {
            hoverPill.textContent = statsHint ? ('✓ Picked · ' + statsHint) : '✓ Picked';
        } else {
            hoverPill.textContent = statsHint ? ('+ Pick · ' + statsHint) : '+ Pick';
        }
        hoverPill.setAttribute('data-url', hoveredTarget.url);
        hoverPill.style.display = 'block';
        const topPx = Math.max(8, Math.min(window.innerHeight - 44, Math.round(r.top + 8)));
        const rightPx = Math.max(8, Math.round(window.innerWidth - r.right + 8));
        hoverPill.style.top = topPx + 'px';
        hoverPill.style.right = rightPx + 'px';
    }

    function renderCardOverlays() {
        if (!cardLayer) return;
        while (cardLayer.firstChild) cardLayer.removeChild(cardLayer.firstChild);

        const seenUrls = new Set();
        const anchors = document.querySelectorAll(POST_SELECTOR);
        for (const a of anchors) {
            const rawHref = a.getAttribute('href') || a.href || '';
            const url = canonicalizeUrl(rawHref);
            if (!url || seenUrls.has(url)) continue;

            const isPicked = state.pickedSet.has(url);
            const isKnownSaved = !isPicked && state.savedKnownSet.has(url);
            const isKnownRejected = !isPicked && !isKnownSaved && state.rejectedKnownSet.has(url);
            if (!isPicked && !isKnownSaved && !isKnownRejected) continue;

            const card = findOuterCard(a);
            const rect = getCardRect(a, card);
            if (rect.width < 40 || rect.height < 40) continue;
            if (rect.bottom < 0 || rect.top > window.innerHeight || rect.right < 0 || rect.left > window.innerWidth) continue;

            seenUrls.add(url);
            if (isPicked) {
                const ring = document.createElement('div');
                ring.className = 'qs-card-ring';
                ring.style.left = Math.round(rect.left) + 'px';
                ring.style.top = Math.round(rect.top) + 'px';
                ring.style.width = Math.round(rect.width) + 'px';
                ring.style.height = Math.round(rect.height) + 'px';
                cardLayer.appendChild(ring);

                if (!hoveredTarget || hoveredTarget.url !== url) {
                    const pill = document.createElement('button');
                    pill.type = 'button';
                    pill.className = 'qs-badge qs-badge-picked';
                    pill.textContent = '✓ Picked';
                    pill.style.top = Math.max(6, Math.round(rect.top + 8)) + 'px';
                    pill.style.left = Math.max(6, Math.round(rect.left + 8)) + 'px';
                    pill.addEventListener('click', (ev) => {
                        ev.preventDefault();
                        ev.stopPropagation();
                        invokeAction({ action: 'toggle_pick', url: url, card_hint: { url: url } });
                    });
                    cardLayer.appendChild(pill);
                }
            } else {
                const badge = document.createElement('span');
                badge.className = isKnownSaved ? 'qs-badge qs-badge-known' : 'qs-badge qs-badge-rejected';
                badge.textContent = isKnownSaved ? 'In library' : 'Rejected';
                badge.style.top = Math.max(6, Math.round(rect.top + 8)) + 'px';
                badge.style.left = Math.max(6, Math.round(rect.left + 8)) + 'px';
                cardLayer.appendChild(badge);
            }
        }
    }

    function renderHud() {
        if (!hudEl) return;
        const n = state.picked.length;
        trayToggleBtn.textContent = 'Picked: ' + n + (n > 0 ? ' ▾' : '');
        finishBtn.textContent = '✓ Save & Finish (' + n + ')';

        const openUrl = detectOpenPostUrl();
        if (openUrl) {
            const isOpenPicked = state.pickedSet.has(openUrl);
            const openHint = state.liveStats[openUrl] || '';
            openPostBtn.style.display = 'inline-block';
            openPostBtn.className = isOpenPicked ? 'qs-hud-btn qs-hud-primary' : 'qs-hud-btn qs-hud-accent';
            openPostBtn.textContent = isOpenPicked
                ? '✓ Open post picked'
                : ('+ Pick open post' + (openHint ? (' · ' + openHint) : ''));
        } else {
            openPostBtn.style.display = 'none';
        }

        let creator = detectPageCreatorHandle();
        if (!creator && hoveredTarget && hoveredTarget.hint && hoveredTarget.hint.author) {
            creator = hoveredTarget.hint.author;
        }
        if (creator) {
            const norm = creator.startsWith('@') ? creator : ('@' + creator);
            const isStarred = state.starredSet.has(norm.toLowerCase());
            starCreatorBtn.style.display = 'inline-block';
            starCreatorBtn.setAttribute('data-handle', norm);
            starCreatorBtn.className = isStarred ? 'qs-hud-btn qs-hud-starred' : 'qs-hud-btn';
            starCreatorBtn.textContent = isStarred ? ('★ ' + norm) : ('☆ Star ' + norm);
        } else {
            starCreatorBtn.style.display = 'none';
        }
    }

    function renderAll() {
        if (!ensureDom()) return;
        if (hoveredTarget && hoveredTarget.anchor && hoveredTarget.anchor.isConnected) {
            hoveredTarget.rect = getCardRect(hoveredTarget.anchor, hoveredTarget.card);
        }
        renderCardOverlays();
        renderHoverPill();
        renderHud();
        renderTray();
    }

    function resolveAnchorAtPoint(x, y) {
        const els = document.elementsFromPoint(x, y);
        for (const el of els) {
            if (el === host) continue;
            const directA = el.closest ? el.closest(POST_SELECTOR) : null;
            if (directA) return directA;
        }
        for (const el of els) {
            if (el === host || !el.closest) continue;
            const card = el.closest(
                '[data-test-id="pin"], [data-test-id="pinWrapper"], [class*="DivItemContainer"], '
                + '[data-e2e="user-post-item"], [data-e2e="search_video-item"], '
                + 'ytd-video-renderer, ytd-reel-item-renderer, ytd-rich-item-renderer, ytm-shorts-lockup-view-model, article'
            );
            if (card) {
                const innerA = card.querySelector(POST_SELECTOR);
                if (innerA) return innerA;
            }
        }
        return null;
    }

    window.addEventListener('mousemove', (ev) => {
        if (!ensureDom()) return;
        const path = ev.composedPath ? ev.composedPath() : [];
        if (path.includes(hoverPill) || path.includes(hudEl) || path.includes(trayEl)) {
            return;
        }
        const a = resolveAnchorAtPoint(ev.clientX, ev.clientY);
        if (!a) {
            if (hoveredTarget !== null) {
                hoveredTarget = null;
                renderHoverPill();
                renderCardOverlays();
                renderHud();
            }
            return;
        }
        const rawHref = a.getAttribute('href') || a.href || '';
        const url = canonicalizeUrl(rawHref);
        if (!url) return;
        const card = findOuterCard(a);
        const rect = getCardRect(a, card);
        const hint = extractCardHint(a, card, url);
        hoveredTarget = { url, anchor: a, card, rect, hint };
        renderHoverPill();
        renderCardOverlays();
        renderHud();
    }, { passive: true });

    window.addEventListener('click', (ev) => {
        if (!ev.altKey) return;
        const path = ev.composedPath ? ev.composedPath() : [];
        if (path.includes(host)) return;
        const a = resolveAnchorAtPoint(ev.clientX, ev.clientY);
        if (!a) return;
        const rawHref = a.getAttribute('href') || a.href || '';
        const url = canonicalizeUrl(rawHref);
        if (!url) return;
        ev.preventDefault();
        ev.stopPropagation();
        const card = findOuterCard(a);
        const hint = extractCardHint(a, card, url);
        invokeAction({ action: 'toggle_pick', url, card_hint: hint });
    }, true);

    window.addEventListener('scroll', () => renderAll(), { passive: true, capture: true });
    window.addEventListener('resize', () => renderAll(), { passive: true });
    setInterval(() => renderAll(), 400);

    if (initialState) {
        applyIncomingState(initialState);
    } else {
        renderAll();
        invokeAction({ action: 'sync' });
    }
}
"""


def _resolve_active_page(context, current_page):
    """Returns an open primary page, or None if the user closed all browser tabs."""
    try:
        if current_page is not None and not current_page.is_closed():
            return current_page
        open_pages = [p for p in context.pages if not p.is_closed()]
        return open_pages[0] if open_pages else None
    except Exception:
        return None


def _is_navigation_error(exc: Exception) -> bool:
    msg = str(exc).lower()
    return any(
        token in msg
        for token in (
            "execution context was destroyed",
            "cannot find context with specified id",
            "frame was detached",
            "navigating",
        )
    )


def _classify_url_gate(url: str) -> Optional[tuple[str, str]]:
    if not url:
        return None
    parsed = urllib.parse.urlparse(url)
    scheme = (parsed.scheme or "").lower()
    host = (parsed.netloc or "").lower()
    path = (parsed.path or "").lower()
    query = (parsed.query or "").lower()
    full_lower = url.lower()

    if scheme in ("http", "https"):
        if (
            "google.com/sorry" in full_lower
            or "/captcha" in path
            or "captcha" in query
            or "/challenge" in path
            or "/checkpoint" in path
            or "tiktok.com/verify" in full_lower
            or "secsdk" in full_lower
        ):
            return ("captcha", f"Verification URL ({host}{path})")

        if (
            "accounts.google.com" in host
            or "consent.youtube.com" in host
            or "consent.google.com" in host
            or "appleid.apple.com" in host
            or ("facebook.com" in host and "/login" in path)
            or "/accounts/login" in path
            or "/accounts/onetap" in path
            or bool(
                re.search(
                    r"(^|/)(login|signin|sign-in|authenticate|oauth|passport)(/|$)",
                    path,
                )
            )
        ):
            return ("login", f"Login URL ({host}{path})")
    elif scheme == "file":
        base = os.path.basename(path)
        if "captcha" in base:
            return ("captcha", f"Local CAPTCHA fixture ({base})")
        if base in ("login.html", "signin.html"):
            return ("login", f"Local login fixture ({base})")
    return None


def detect_human_gate(
    context,
    page,
    check_empty_logged_out: bool = False,
) -> Optional[dict]:
    """Detects whether login, user input, OAuth popup, or CAPTCHA requires pausing.

    Returns ``{"kind": "login"|"captcha", "reason": str, "manual": bool}`` or ``None``.
    Never clicks, types, reloads, or attempts to solve challenges.
    """
    try:
        open_pages = [p for p in context.pages if not p.is_closed()]
    except Exception:
        return None

    # 1. Check secondary popup windows (e.g. "Continue with Google / Facebook / Apple")
    for extra in open_pages:
        if extra is page:
            continue
        try:
            extra_url = (extra.url or "").strip()
        except Exception:
            extra_url = ""
        lower_extra = extra_url.lower()
        if (
            not lower_extra
            or lower_extra == "about:blank"
            or _classify_url_gate(extra_url) is not None
            or any(
                tok in lower_extra
                for tok in (
                    "accounts.google.com",
                    "facebook.com",
                    "appleid.apple.com",
                    "oauth",
                    "auth",
                    "login",
                    "signin",
                    "captcha",
                    "verify",
                    "challenge",
                )
            )
        ):
            return {
                "kind": "login",
                "reason": "Sign-in / verification popup window is open",
                "manual": False,
            }

    # 2. Check main page URL
    try:
        current_url = page.url or ""
    except Exception:
        current_url = ""
    url_gate = _classify_url_gate(current_url)
    if url_gate is not None:
        return {"kind": url_gate[0], "reason": url_gate[1], "manual": False}

    # 3. Inspect visible DOM on the main page
    is_web = current_url.lower().startswith(("http://", "https://"))
    try:
        dom_gate = page.evaluate(
            DETECT_HUMAN_GATE_JS,
            [bool(check_empty_logged_out and is_web)],
        )
        if isinstance(dom_gate, dict) and dom_gate.get("kind"):
            return {
                "kind": str(dom_gate["kind"]),
                "reason": str(dom_gate.get("reason") or dom_gate["kind"]),
                "manual": bool(dom_gate.get("manual", False)),
            }
    except Exception as exc:
        if _is_navigation_error(exc):
            return {
                "kind": "login",
                "reason": "Page navigation in progress",
                "manual": False,
            }
    return None


def _should_restore_target_url(current_url: str, target_url: str) -> bool:
    """True when a login redirect dumped the browser onto a home/landing page
    instead of the requested search/feed ``target_url``."""
    if not current_url or not target_url:
        return False
    cur = urllib.parse.urlparse(current_url)
    tgt = urllib.parse.urlparse(target_url)
    if cur.scheme != tgt.scheme:
        return True
    cur_path = (cur.path or "").rstrip("/")
    tgt_path = (tgt.path or "").rstrip("/")
    if not tgt_path:
        return False
    return cur_path != tgt_path


def _launch_browser_context(playwright, profile_dir: str, headless: bool, platform_id: str):
    if headless:
        return playwright.chromium.launch_persistent_context(
            profile_dir,
            headless=True,
            user_agent=USER_AGENT,
            viewport={"width": 1366, "height": 900},
            locale="en-US",
            args=["--disable-blink-features=AutomationControlled"],
        )

    base_kwargs = {
        "headless": False,
        "viewport": {"width": 1366, "height": 900},
        "locale": "en-US",
        "ignore_default_args": ["--enable-automation"],
        "args": [
            "--disable-blink-features=AutomationControlled",
            "--no-first-run",
            "--no-default-browser-check",
        ],
    }
    for channel in ("chrome", None):
        kwargs = dict(base_kwargs)
        if channel:
            kwargs["channel"] = channel
        try:
            ctx = playwright.chromium.launch_persistent_context(profile_dir, **kwargs)
            log(platform_id, f"Launched persistent browser context (channel={channel or 'chromium'})")
            return ctx
        except Exception as exc:
            log(platform_id, f"Launch attempt (channel={channel or 'chromium'}) failed: {exc}")
            if channel is None:
                raise
    raise RuntimeError("Failed to launch browser context")


def run_spy_worker(
    platform_id: str,
    build_search_url: Callable[[str], str],
    extract_js: str,
    upgrade_media_url: Optional[Callable[[str], str]] = None,
    install_response_listener: Optional[Callable[[Any, dict], None]] = None,
    enrich_batch: Optional[Callable[[list[dict], Any, dict], None]] = None,
    score_post: Optional[Callable[[dict], int]] = None,
) -> int:
    result: dict = {
        "posts": [],
        "starred_influencers": [],
        "visited_profiles": [],
        "browsed_count": 0,
        "saved_count": 0,
        "rejected_count": 0,
        "min_score": 0,
        "max_days_old": 0,
        "error": None,
    }

    if len(sys.argv) < 3:
        result["error"] = (
            f"Usage: spy_scroll_{platform_id}.py <query_or_url> <download_dir> "
            "[max_posts] [auto|manual|pick] [min_score] [max_days_old]"
        )
        print(json.dumps(result), flush=True)
        return 0

    query_or_url = sys.argv[1].strip()
    download_dir = sys.argv[2].strip()
    try:
        max_posts = max(1, min(200, int(sys.argv[3]))) if len(sys.argv) >= 4 else 24
    except ValueError:
        max_posts = 24
    mode = sys.argv[4].strip().lower() if len(sys.argv) >= 5 else "auto"
    if mode not in ("auto", "manual", "pick"):
        mode = "auto"
    try:
        min_score = max(0, int(sys.argv[5])) if len(sys.argv) >= 6 else 0
    except ValueError:
        min_score = 0
    try:
        max_days_old = max(0, int(sys.argv[6])) if len(sys.argv) >= 7 else 0
    except ValueError:
        max_days_old = 0
    if mode == "pick":
        min_score = 0
        max_days_old = 0
        max_posts = max(max_posts, 500)
    result["min_score"] = min_score
    result["max_days_old"] = max_days_old

    if not query_or_url:
        if mode == "pick":
            target_url = default_home_url(platform_id)
        else:
            result["error"] = "Empty search query or URL."
            print(json.dumps(result), flush=True)
            return 0
    elif query_or_url.startswith(("http://", "https://", "file://")):
        target_url = query_or_url
    elif os.path.isfile(query_or_url):
        target_url = "file://" + os.path.abspath(query_or_url)
    else:
        target_url = build_search_url(query_or_url)

    try:
        from playwright.sync_api import (
            Error as PlaywrightError,
            TimeoutError as PlaywrightTimeoutError,
            sync_playwright,
        )
    except ImportError:
        result["error"] = (
            "Playwright is not installed for python3 "
            "(run: pip install playwright && playwright install chromium)."
        )
        print(json.dumps(result), flush=True)
        return 0

    headless = os.environ.get("QS_SPY_HEADLESS", "0") == "1"
    headless_gate_timeout_s = float(os.environ.get("QS_SPY_GATE_TIMEOUT_S", "12"))
    temp_profile_dir: Optional[str] = None
    if headless:
        temp_profile_dir = tempfile.mkdtemp(prefix=f"qs-spy-{platform_id}-")
        profile_dir = temp_profile_dir
    else:
        profile_dir = os.path.expanduser(
            f"~/.cache/quantumsocial/{platform_id}-profile"
        )
        os.makedirs(profile_dir, exist_ok=True)

    scorer = score_post or default_score_post
    excluded_urls, saved_known_urls, rejected_known_urls, initial_fav_handles = _load_spy_context()
    skipped_known_urls: set[str] = set()
    metadata_cache: dict[str, dict] = {}
    browsed_urls: set[str] = set()
    rejected_urls: set[str] = set()
    collected_by_url: dict[str, dict] = {}
    picked_order: list[str] = []
    live_stats_by_url: dict[str, str] = {}
    starred_handles: dict[str, str] = {h.lower(): h for h in initial_fav_handles}
    newly_starred_handles: list[str] = []
    visited_profiles: list[str] = []
    finish_requested = False
    badge_state_cache: list = []

    log(
        platform_id,
        f"Starting spy scroll: target={target_url} max_posts={max_posts} "
        f"mode={mode} min_score={min_score} max_days_old={max_days_old} "
        f"excluded={len(excluded_urls)} headless={headless}",
    )

    bg_pool: Optional[ThreadPoolExecutor] = ThreadPoolExecutor(max_workers=3) if mode == "pick" else None
    bg_futures: dict[str, Future] = {}

    try:
        with sync_playwright() as p:
            context = _launch_browser_context(p, profile_dir, headless, platform_id)
            try:
                context.add_init_script(
                    "Object.defineProperty(navigator, 'webdriver', {get: () => undefined});"
                )
                page = context.pages[0] if context.pages else context.new_page()
                if install_response_listener is not None:
                    def _attach_listener(pg) -> None:
                        try:
                            install_response_listener(pg, metadata_cache)
                        except Exception as exc:
                            log(platform_id, f"install_response_listener warning: {exc}")

                    _attach_listener(page)
                    context.on("page", _attach_listener)

                def _build_picker_state() -> dict:
                    picked_summaries = []
                    for u in picked_order:
                        rec = collected_by_url.get(u)
                        if rec is not None:
                            picked_summaries.append({
                                "url": u,
                                "title": rec.get("title") or u,
                                "author": rec.get("author") or "",
                            })
                    return {
                        "picked": picked_summaries,
                        "saved_known": list(saved_known_urls),
                        "rejected_known": list(rejected_known_urls),
                        "starred": list(starred_handles.values()),
                        "live_stats": dict(live_stats_by_url),
                    }

                if mode == "pick":
                    def _on_picker_action(_source, payload: Any) -> dict:
                        nonlocal finish_requested
                        if not isinstance(payload, dict):
                            return _build_picker_state()
                        act = str(payload.get("action") or "")
                        if act == "finish":
                            finish_requested = True
                            return _build_picker_state()
                        if act == "toggle_star":
                            raw_h = str(payload.get("handle") or "").strip()
                            if raw_h:
                                norm_h = raw_h if raw_h.startswith("@") else f"@{raw_h}"
                                key_h = norm_h.lower()
                                if key_h in starred_handles:
                                    starred_handles.pop(key_h, None)
                                    newly_starred_handles[:] = [
                                        x for x in newly_starred_handles if x.lower() != key_h
                                    ]
                                else:
                                    starred_handles[key_h] = norm_h
                                    if all(x.lower() != key_h for x in newly_starred_handles):
                                        newly_starred_handles.append(norm_h)
                                    if all(v.lower() != key_h for v in visited_profiles):
                                        visited_profiles.append(norm_h)
                            return _build_picker_state()
                        if act == "toggle_pick":
                            raw_url = str(payload.get("url") or "").strip()
                            post_url = canonical_post_url(raw_url)
                            if not post_url:
                                return _build_picker_state()
                            if post_url in collected_by_url:
                                collected_by_url.pop(post_url, None)
                                picked_order[:] = [u for u in picked_order if u != post_url]
                                fut = bg_futures.pop(post_url, None)
                                if fut is not None and fut.done():
                                    try:
                                        p_old = fut.result()
                                        if p_old and os.path.isfile(p_old):
                                            os.remove(p_old)
                                    except Exception:
                                        pass
                            else:
                                hint = payload.get("card_hint")
                                hint_d = hint if isinstance(hint, dict) else {}
                                media_url = str(hint_d.get("media_url") or "").strip()
                                if upgrade_media_url and media_url:
                                    media_url = upgrade_media_url(media_url)
                                cand = {
                                    "url": post_url,
                                    "title": str(hint_d.get("title") or "").strip(),
                                    "media_url": media_url,
                                    "fallback_media_url": str(hint_d.get("fallback_media_url") or "").strip(),
                                    "local_media": "",
                                    "author": str(hint_d.get("author") or "").strip(),
                                    "published_at": normalize_publish_date(hint_d.get("published_at")),
                                    "views": parse_compact_count(hint_d.get("views")),
                                    "likes": parse_compact_count(hint_d.get("likes")),
                                    "comments": parse_compact_count(hint_d.get("comments")),
                                    "shares": parse_compact_count(hint_d.get("shares")),
                                    "is_ad": bool(hint_d.get("is_ad")),
                                    "_needs_enrich": True,
                                }
                                collected_by_url[post_url] = cand
                                picked_order.append(post_url)
                                browsed_urls.add(post_url)
                            return _build_picker_state()
                        return _build_picker_state()

                    context.expose_binding("__qs_py_action", _on_picker_action)
                    init_state_json = json.dumps(_build_picker_state())
                    init_plat_json = json.dumps(platform_id)
                    context.add_init_script(
                        f"({INSTALL_HUMAN_PICKER_JS})([{init_plat_json}, {init_state_json}]);"
                    )

                try:
                    page.goto(target_url, wait_until="domcontentloaded", timeout=45_000)
                except PlaywrightTimeoutError:
                    log(platform_id, "goto timed out on domcontentloaded, continuing")

                try:
                    page.wait_for_timeout(1800 if not headless else 250)
                except PlaywrightError:
                    pass

                def refresh_badge(paused_kind: Optional[str] = None, manual_pause: bool = False) -> None:
                    if mode == "pick":
                        return
                    update_status_badge(
                        page,
                        len(collected_by_url),
                        max_posts,
                        len(browsed_urls),
                        len(rejected_urls),
                        min_score,
                        mode,
                        paused_kind=paused_kind,
                        manual_pause=manual_pause,
                        last_badge_state=badge_state_cache,
                    )

                cached_cookie_header = ""

                def harvest_current_page() -> int:
                    """Extracts visible posts, enriches stats, applies quality filter.

                    Returns the number of NEW unique candidate URLs browsed on this pass.
                    """
                    nonlocal page, cached_cookie_header
                    page = _resolve_active_page(context, page)
                    if page is None:
                        return 0
                    if not cached_cookie_header:
                        try:
                            cookies = context.cookies() or []
                            if cookies:
                                cached_cookie_header = "; ".join(
                                    f"{c.get('name')}={c.get('value')}"
                                    for c in cookies
                                    if c.get("name") and c.get("value") is not None
                                )
                        except Exception:
                            pass
                    try:
                        raw_items = page.evaluate(extract_js) or []
                    except PlaywrightError as exc:
                        if _is_navigation_error(exc):
                            return 0
                        raise

                    new_candidates: list[dict] = []
                    newly_skipped_known = 0
                    for item in raw_items:
                        if not isinstance(item, dict):
                            continue
                        post_url = canonical_post_url(str(item.get("url") or "").strip())
                        if not post_url:
                            continue
                        if post_url in excluded_urls:
                            if post_url not in skipped_known_urls:
                                skipped_known_urls.add(post_url)
                                newly_skipped_known += 1
                            continue
                        media_url = str(item.get("media_url") or "").strip()
                        if upgrade_media_url and media_url:
                            media_url = upgrade_media_url(media_url)
                        is_ad = bool(item.get("is_ad") or item.get("is_promoted", False))
                        normalized = {
                            "url": post_url,
                            "title": str(item.get("title") or "").strip(),
                            "media_url": media_url,
                            "fallback_media_url": str(item.get("fallback_media_url") or "").strip(),
                            "local_media": "",
                            "author": str(item.get("author") or "").strip(),
                            "published_at": normalize_publish_date(item.get("published_at")),
                            "views": parse_compact_count(item.get("views")),
                            "likes": parse_compact_count(item.get("likes")),
                            "comments": parse_compact_count(item.get("comments")),
                            "shares": parse_compact_count(item.get("shares")),
                            "is_ad": is_ad,
                        }

                        # Update already-saved records if richer fields arrive later
                        existing = collected_by_url.get(post_url)
                        if existing is not None:
                            if is_ad:
                                existing["is_ad"] = True
                            if not existing.get("title") and normalized["title"]:
                                existing["title"] = normalized["title"]
                            if not existing.get("media_url") and normalized["media_url"]:
                                existing["media_url"] = normalized["media_url"]
                            if not existing.get("author") and normalized["author"]:
                                existing["author"] = normalized["author"]
                            if not existing.get("published_at") and normalized["published_at"]:
                                existing["published_at"] = normalized["published_at"]
                            for k in ("views", "likes", "comments", "shares"):
                                if existing.get(k) is None and normalized.get(k) is not None:
                                    existing[k] = normalized[k]
                            continue

                        if post_url in browsed_urls:
                            continue
                        new_candidates.append(normalized)

                    if not new_candidates:
                        return newly_skipped_known

                    # Only evaluate as many new batches as needed until max_posts is filled.
                    if enrich_batch is not None:
                        try:
                            enrich_batch(new_candidates, context, metadata_cache)
                        except Exception as exc:
                            log(platform_id, f"enrich_batch warning: {exc}")

                    newly_browsed = 0
                    for cand in new_candidates:
                        if len(collected_by_url) >= max_posts:
                            break
                        url = cand["url"]
                        if url in browsed_urls:
                            continue
                        browsed_urls.add(url)
                        newly_browsed += 1

                        if upgrade_media_url and cand.get("media_url"):
                            cand["media_url"] = upgrade_media_url(cand["media_url"])

                        is_ad = bool(cand.get("is_ad") or cand.get("is_promoted"))
                        cand["is_ad"] = is_ad
                        cand.pop("is_promoted", None)

                        if max_days_old > 0:
                            age_days = age_days_from_iso(str(cand.get("published_at") or ""))
                            if age_days is None or age_days > max_days_old:
                                rejected_urls.add(url)
                                continue

                        post_score = scorer(cand)
                        cand["_score"] = max(post_score, min_score) if is_ad else post_score
                        if not is_ad and min_score > 0 and post_score < min_score:
                            rejected_urls.add(url)
                            continue

                        collected_by_url[url] = cand

                    return newly_browsed + newly_skipped_known

                def wait_for_human_if_needed(check_empty_logged_out: bool = False) -> tuple[bool, bool]:
                    """If login or CAPTCHA is active, pauses until a human solves it.

                    Returns ``(session_alive, did_pause)``.
                    """
                    nonlocal page
                    page = _resolve_active_page(context, page)
                    if page is None:
                        return (False, False)

                    gate = detect_human_gate(
                        context,
                        page,
                        check_empty_logged_out=check_empty_logged_out,
                    )
                    if gate is None:
                        return (True, False)

                    had_login_gate = gate["kind"] == "login" and not gate.get("manual", False)
                    log(
                        platform_id,
                        f"Pausing for human intervention ({gate['kind']}): {gate['reason']}",
                    )
                    refresh_badge(
                        paused_kind=gate["kind"],
                        manual_pause=gate.get("manual", False),
                    )

                    poll_ms = 120 if headless else 800
                    required_clear_ticks = 2 if headless else 3
                    clear_ticks = 0
                    started_wait = time.monotonic()

                    while True:
                        if headless and (time.monotonic() - started_wait) >= headless_gate_timeout_s:
                            log(
                                platform_id,
                                f"Headless human-gate timeout ({headless_gate_timeout_s}s) reached; stopping wait",
                            )
                            return (False, True)

                        try:
                            page.wait_for_timeout(poll_ms)
                        except PlaywrightError:
                            time.sleep(poll_ms / 1000.0)

                        page = _resolve_active_page(context, page)
                        if page is None:
                            return (False, True)

                        try:
                            ready_state = page.evaluate("() => document.readyState")
                        except PlaywrightError as exc:
                            if _is_navigation_error(exc):
                                clear_ticks = 0
                                continue
                            if _resolve_active_page(context, page) is None:
                                return (False, True)
                            clear_ticks = 0
                            continue

                        if ready_state == "loading":
                            clear_ticks = 0
                            continue

                        current_gate = detect_human_gate(
                            context,
                            page,
                            check_empty_logged_out=False,
                        )
                        if current_gate is not None:
                            clear_ticks = 0
                            if current_gate["kind"] == "login" and not current_gate.get("manual", False):
                                had_login_gate = True
                            refresh_badge(
                                paused_kind=current_gate["kind"],
                                manual_pause=current_gate.get("manual", False),
                            )
                            continue

                        clear_ticks += 1
                        if clear_ticks >= required_clear_ticks:
                            break

                    log(platform_id, "Human intervention completed; resuming spy worker")

                    if had_login_gate and _should_restore_target_url(page.url, target_url):
                        try:
                            page.wait_for_timeout(150 if headless else 700)
                        except PlaywrightError:
                            pass
                        if _should_restore_target_url(page.url, target_url):
                            log(
                                platform_id,
                                f"Login landed on {page.url}; returning to target {target_url}",
                            )
                            badge_state_cache.clear()
                            try:
                                page.goto(target_url, wait_until="domcontentloaded", timeout=45_000)
                                page.wait_for_timeout(250 if headless else 1600)
                            except PlaywrightError as exc:
                                log(platform_id, f"Post-login return goto warning: {exc}")

                            alive, _ = wait_for_human_if_needed(check_empty_logged_out=False)
                            if not alive:
                                return (False, True)

                    badge_state_cache.clear()
                    refresh_badge()
                    return (True, True)

                def sync_human_pick_session() -> None:
                    """Updates live hover stats on open pages, tracks visited profiles,
                    enriches newly picked items, and starts background media downloads."""
                    nonlocal page, cached_cookie_header
                    open_pages = []
                    try:
                        open_pages = [pg for pg in context.pages if not pg.is_closed()]
                    except Exception:
                        open_pages = []
                    if not open_pages:
                        return

                    if not cached_cookie_header:
                        try:
                            cookies = context.cookies() or []
                            if cookies:
                                cached_cookie_header = "; ".join(
                                    f"{c.get('name')}={c.get('value')}"
                                    for c in cookies
                                    if c.get("name") and c.get("value") is not None
                                )
                        except Exception:
                            pass

                    visible_by_url: dict[str, dict] = {}
                    for pg in open_pages:
                        try:
                            pg_url = (pg.url or "").strip()
                        except Exception:
                            pg_url = ""
                        if pg_url:
                            prof_h = extract_profile_handle_from_url(pg_url, platform_id)
                            if prof_h and all(v.lower() != prof_h.lower() for v in visited_profiles):
                                visited_profiles.append(prof_h)
                            elif pg_url.startswith("file://") and pg_url not in visited_profiles:
                                visited_profiles.append(pg_url)

                        try:
                            raw_items = pg.evaluate(extract_js) or []
                        except Exception:
                            raw_items = []
                        try:
                            open_item = pg.evaluate(EXTRACT_OPEN_POST_FALLBACK_JS)
                            if isinstance(open_item, dict) and open_item.get("url"):
                                raw_items = list(raw_items) + [open_item]
                        except Exception:
                            pass

                        for item in raw_items:
                            if not isinstance(item, dict):
                                continue
                            u = canonical_post_url(str(item.get("url") or "").strip())
                            if not u:
                                continue
                            browsed_urls.add(u)
                            visible_by_url[u] = item
                            hint = format_post_stats_hint(item, platform_id)
                            if hint:
                                live_stats_by_url[u] = hint

                    # Merge richer DOM fields + cached metadata into picked records
                    to_enrich: list[dict] = []
                    for u in list(picked_order):
                        rec = collected_by_url.get(u)
                        if rec is None:
                            continue
                        vis = visible_by_url.get(u)
                        if isinstance(vis, dict):
                            if not rec.get("title") or rec.get("title") == u:
                                t = str(vis.get("title") or "").strip()
                                if t:
                                    rec["title"] = t
                            if not rec.get("media_url"):
                                mu = str(vis.get("media_url") or "").strip()
                                if upgrade_media_url and mu:
                                    mu = upgrade_media_url(mu)
                                if mu:
                                    rec["media_url"] = mu
                            if not rec.get("author") and vis.get("author"):
                                rec["author"] = str(vis.get("author") or "").strip()
                            if not rec.get("published_at") and vis.get("published_at"):
                                rec["published_at"] = normalize_publish_date(vis.get("published_at"))
                            for k in ("views", "likes", "comments", "shares"):
                                if rec.get(k) is None and vis.get(k) is not None:
                                    rec[k] = parse_compact_count(vis.get(k))
                            if vis.get("is_ad") or vis.get("is_promoted"):
                                rec["is_ad"] = True

                        if rec.get("_needs_enrich"):
                            rec.pop("_needs_enrich", None)
                            to_enrich.append(rec)

                    if to_enrich and enrich_batch is not None:
                        try:
                            enrich_batch(to_enrich, context, metadata_cache)
                        except Exception as exc:
                            log(platform_id, f"pick enrich_batch warning: {exc}")
                        for rec in to_enrich:
                            if upgrade_media_url and rec.get("media_url"):
                                rec["media_url"] = upgrade_media_url(rec["media_url"])
                            hint = format_post_stats_hint(rec, platform_id)
                            if hint:
                                live_stats_by_url[rec["url"]] = hint

                    # Also surface metadata_cache stats for visible unpicked cards without network calls
                    if metadata_cache and visible_by_url:
                        for u, vis in visible_by_url.items():
                            if u in live_stats_by_url:
                                continue
                            m_id = re.search(r"/(?:pin|video|photo)/(\d{6,24})", u)
                            if m_id and isinstance(metadata_cache.get(m_id.group(1)), dict):
                                hint = format_post_stats_hint(metadata_cache[m_id.group(1)], platform_id)
                                if hint:
                                    live_stats_by_url[u] = hint

                    # Kick off thread-safe background downloads (context=None) for picked posts
                    if bg_pool is not None:
                        for idx_p, u in enumerate(list(picked_order), start=1):
                            rec = collected_by_url.get(u)
                            if rec is None or rec.get("local_media") or u in bg_futures:
                                continue
                            media_u = str(rec.get("media_url") or "").strip()
                            if not media_u:
                                continue
                            fb_u = str(rec.get("fallback_media_url") or "").strip()
                            bg_futures[u] = bg_pool.submit(
                                download_media_file,
                                None,
                                media_u,
                                u,
                                download_dir,
                                idx_p,
                                platform_id,
                                cached_cookie_header,
                                fb_u,
                            )

                    for u, fut in list(bg_futures.items()):
                        if fut.done():
                            bg_futures.pop(u, None)
                            rec = collected_by_url.get(u)
                            if rec is not None and not rec.get("local_media"):
                                try:
                                    dl_path = fut.result()
                                    if dl_path and os.path.isfile(dl_path):
                                        rec["local_media"] = dl_path
                                except Exception:
                                    pass

                    state_snapshot = _build_picker_state()
                    for pg in open_pages:
                        try:
                            pg.evaluate(INSTALL_HUMAN_PICKER_JS, [platform_id, state_snapshot])
                        except Exception:
                            pass

                if mode == "pick":
                    started_pick = time.monotonic()
                    sync_human_pick_session()
                    while not finish_requested:
                        if headless and (time.monotonic() - started_pick) >= headless_gate_timeout_s:
                            log(
                                platform_id,
                                f"Headless pick timeout ({headless_gate_timeout_s}s) reached; finishing",
                            )
                            break
                        page = _resolve_active_page(context, page)
                        if page is None:
                            break
                        sync_human_pick_session()
                        if finish_requested:
                            break
                        try:
                            page.wait_for_timeout(80 if headless else 300)
                        except PlaywrightError:
                            if _resolve_active_page(context, page) is None:
                                break
                    sync_human_pick_session()
                else:
                    alive, _ = wait_for_human_if_needed(check_empty_logged_out=True)
                    if alive:
                        harvest_current_page()
                        refresh_badge()

                if mode == "auto" and not finish_requested and _resolve_active_page(context, page) is not None:
                    idle_rounds = 0
                    max_idle_rounds = 2 if headless else 10
                    # Allow enough scroll steps even when reject rate is 70-85%
                    max_scroll_steps = max(12, max_posts * 8)
                    scroll_steps = 0

                    while scroll_steps < max_scroll_steps:
                        page = _resolve_active_page(context, page)
                        if page is None:
                            break

                        alive, paused = wait_for_human_if_needed(
                            check_empty_logged_out=(len(browsed_urls) == 0 and idle_rounds >= 1)
                        )
                        if not alive:
                            break
                        if paused:
                            idle_rounds = 0
                            harvest_current_page()

                        if len(collected_by_url) >= max_posts:
                            break

                        try:
                            page.evaluate(
                                """() => {
                                    const delta = Math.max(700, Math.floor(window.innerHeight * 0.85));
                                    const beforeY = window.scrollY;
                                    window.scrollBy(0, delta);
                                    if (Math.abs(window.scrollY - beforeY) >= 10) return;
                                    const scrollables = Array.from(
                                        document.querySelectorAll('main, [id*="main"], section, div')
                                    ).filter(el => {
                                        if (el.id === '__qs_spy_badge' || el.id === '__qs_picker_host') return false;
                                        const st = window.getComputedStyle(el);
                                        return (st.overflowY === 'auto' || st.overflowY === 'scroll')
                                            && el.scrollHeight > el.clientHeight + 80;
                                    });
                                    scrollables.sort(
                                        (a, b) => (b.clientWidth * b.clientHeight) - (a.clientWidth * a.clientHeight)
                                    );
                                    if (scrollables.length > 0) {
                                        scrollables[0].scrollBy(0, delta);
                                    }
                                }"""
                            )
                        except PlaywrightError as exc:
                            if _is_navigation_error(exc):
                                continue
                            if _resolve_active_page(context, page) is None:
                                break

                        scroll_steps += 1
                        try:
                            page.wait_for_timeout(200 if headless else 1400)
                        except PlaywrightError:
                            if _resolve_active_page(context, page) is None:
                                break

                        alive, paused = wait_for_human_if_needed(
                            check_empty_logged_out=(len(browsed_urls) == 0)
                        )
                        if not alive:
                            break
                        if paused:
                            idle_rounds = 0

                        newly_browsed = harvest_current_page()
                        refresh_badge()
                        if len(collected_by_url) >= max_posts:
                            alive, _ = wait_for_human_if_needed(check_empty_logged_out=False)
                            break

                        if newly_browsed == 0:
                            idle_rounds += 1
                            if idle_rounds >= max_idle_rounds:
                                break
                        else:
                            idle_rounds = 0

                elif mode == "manual" and _resolve_active_page(context, page) is not None:
                    ticks = 0
                    while True:
                        if headless and ticks >= 4:
                            break
                        page = _resolve_active_page(context, page)
                        if page is None:
                            break
                        alive, _ = wait_for_human_if_needed(check_empty_logged_out=False)
                        if not alive:
                            break
                        harvest_current_page()
                        refresh_badge()
                        if len(collected_by_url) >= max_posts:
                            alive, _ = wait_for_human_if_needed(check_empty_logged_out=False)
                            break
                        ticks += 1
                        try:
                            page.wait_for_timeout(150 if headless else 800)
                        except PlaywrightError:
                            if _resolve_active_page(context, page) is None:
                                break

            except PlaywrightError as exc:
                log(
                    platform_id,
                    f"Browser session ended ({exc}); saving {len(collected_by_url)} posts",
                )

            # Wait briefly for any in-flight background downloads in pick mode
            for u, fut in list(bg_futures.items()):
                try:
                    dl_path = fut.result(timeout=15)
                    rec = collected_by_url.get(u)
                    if rec is not None and dl_path and os.path.isfile(dl_path):
                        rec["local_media"] = dl_path
                except Exception:
                    pass

            if mode == "pick":
                posts_list = [
                    collected_by_url[u]
                    for u in picked_order
                    if u in collected_by_url
                ]
            else:
                # Sort saved posts by engagement score descending so top performers appear first
                posts_list = list(collected_by_url.values())[:max_posts]
                posts_list.sort(key=lambda item: int(item.get("_score") or 0), reverse=True)

            active_context = context if _resolve_active_page(context, None) is not None else None
            if active_context is not None:
                try:
                    cookies = active_context.cookies() or []
                    if cookies:
                        cached_cookie_header = "; ".join(
                            f"{c.get('name')}={c.get('value')}"
                            for c in cookies
                            if c.get("name") and c.get("value") is not None
                        )
                except Exception:
                    pass
            for idx, post in enumerate(posts_list, start=1):
                post.pop("_score", None)
                post.pop("_needs_enrich", None)
                post.pop("is_promoted", None)
                fallback_url = str(post.pop("fallback_media_url", "") or "").strip()
                media_url = post.get("media_url") or ""
                if media_url and not post.get("local_media"):
                    local_path = download_media_file(
                        active_context,
                        media_url,
                        post.get("url") or target_url,
                        download_dir,
                        idx,
                        platform_id,
                        cookie_header=cached_cookie_header,
                        fallback_media_url=fallback_url,
                    )
                    post["local_media"] = local_path

            try:
                context.close()
            except Exception:
                pass

            result["posts"] = posts_list
            result["starred_influencers"] = list(newly_starred_handles)
            result["visited_profiles"] = list(visited_profiles)
            result["browsed_count"] = max(len(browsed_urls), len(posts_list))
            result["saved_count"] = len(posts_list)
            result["rejected_count"] = len(rejected_urls)
            log(
                platform_id,
                f"Finished spy scroll (mode={mode}): browsed={result['browsed_count']} "
                f"saved={len(posts_list)} rejected={len(rejected_urls)} "
                f"starred={len(newly_starred_handles)}",
            )

    except Exception as exc:
        log(platform_id, f"Unhandled exception: {exc}")
        if collected_by_url:
            posts_list = list(collected_by_url.values())[:max_posts]
            for post in posts_list:
                post.pop("_score", None)
                post.pop("_needs_enrich", None)
                post.pop("is_promoted", None)
                post.pop("fallback_media_url", None)
            result["posts"] = posts_list
            result["starred_influencers"] = list(newly_starred_handles)
            result["visited_profiles"] = list(visited_profiles)
            result["browsed_count"] = max(len(browsed_urls), len(posts_list))
            result["saved_count"] = len(posts_list)
            result["rejected_count"] = len(rejected_urls)
        else:
            result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if bg_pool is not None:
            bg_pool.shutdown(wait=False)
        if temp_profile_dir and os.path.isdir(temp_profile_dir):
            shutil.rmtree(temp_profile_dir, ignore_errors=True)

    print(json.dumps(result), flush=True)
    return 0
