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


def _load_excluded_urls() -> set[str]:
    excluded: set[str] = set()
    exclude_path = os.environ.get("QS_SPY_EXCLUDE_FILE", "").strip()
    if not exclude_path or not os.path.isfile(exclude_path):
        return excluded
    try:
        with open(exclude_path, encoding="utf-8") as f:
            items = json.load(f)
        if isinstance(items, list):
            for raw in items:
                norm = canonical_post_url(str(raw or ""))
                if norm:
                    excluded.add(norm)
    except Exception:
        pass
    return excluded


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
    if (active && (!active.closest || !active.closest('#__qs_spy_badge'))) {
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
        if (el.id === '__qs_spy_badge') continue;
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
            "[max_posts] [auto|manual] [min_score] [max_days_old]"
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
    if mode not in ("auto", "manual"):
        mode = "auto"
    try:
        min_score = max(0, int(sys.argv[5])) if len(sys.argv) >= 6 else 0
    except ValueError:
        min_score = 0
    try:
        max_days_old = max(0, int(sys.argv[6])) if len(sys.argv) >= 7 else 0
    except ValueError:
        max_days_old = 0
    result["min_score"] = min_score
    result["max_days_old"] = max_days_old

    if not query_or_url:
        result["error"] = "Empty search query or URL."
        print(json.dumps(result), flush=True)
        return 0

    if query_or_url.startswith(("http://", "https://", "file://")):
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
    excluded_urls = _load_excluded_urls()
    skipped_known_urls: set[str] = set()
    metadata_cache: dict[str, dict] = {}
    browsed_urls: set[str] = set()
    rejected_urls: set[str] = set()
    collected_by_url: dict[str, dict] = {}
    badge_state_cache: list = []

    log(
        platform_id,
        f"Starting spy scroll: target={target_url} max_posts={max_posts} "
        f"mode={mode} min_score={min_score} max_days_old={max_days_old} "
        f"excluded={len(excluded_urls)} headless={headless}",
    )

    try:
        with sync_playwright() as p:
            context = _launch_browser_context(p, profile_dir, headless, platform_id)
            try:
                context.add_init_script(
                    "Object.defineProperty(navigator, 'webdriver', {get: () => undefined});"
                )
                page = context.pages[0] if context.pages else context.new_page()
                if install_response_listener is not None:
                    try:
                        install_response_listener(page, metadata_cache)
                    except Exception as exc:
                        log(platform_id, f"install_response_listener warning: {exc}")

                try:
                    page.goto(target_url, wait_until="domcontentloaded", timeout=45_000)
                except PlaywrightTimeoutError:
                    log(platform_id, "goto timed out on domcontentloaded, continuing")

                try:
                    page.wait_for_timeout(1800 if not headless else 250)
                except PlaywrightError:
                    pass

                def refresh_badge(paused_kind: Optional[str] = None, manual_pause: bool = False) -> None:
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

                alive, _ = wait_for_human_if_needed(check_empty_logged_out=True)
                if alive:
                    harvest_current_page()
                    refresh_badge()

                if alive and mode == "auto":
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
                                        if (el.id === '__qs_spy_badge') return false;
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

                elif alive:
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
            result["browsed_count"] = len(browsed_urls)
            result["saved_count"] = len(posts_list)
            result["rejected_count"] = len(rejected_urls)
            log(
                platform_id,
                f"Finished spy scroll: browsed={len(browsed_urls)} "
                f"saved={len(posts_list)} rejected={len(rejected_urls)} min_score={min_score}",
            )

    except Exception as exc:
        log(platform_id, f"Unhandled exception: {exc}")
        if collected_by_url:
            posts_list = list(collected_by_url.values())[:max_posts]
            for post in posts_list:
                post.pop("_score", None)
                post.pop("is_promoted", None)
                post.pop("fallback_media_url", None)
            result["posts"] = posts_list
            result["browsed_count"] = len(browsed_urls)
            result["saved_count"] = len(posts_list)
            result["rejected_count"] = len(rejected_urls)
        else:
            result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if temp_profile_dir and os.path.isdir(temp_profile_dir):
            shutil.rmtree(temp_profile_dir, ignore_errors=True)

    print(json.dumps(result), flush=True)
    return 0
