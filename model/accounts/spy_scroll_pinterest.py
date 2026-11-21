#!/usr/bin/env python3
"""Scrolls a Pinterest search or board/feed URL and saves candidate Pin posts + images + stats.

Usage:
    spy_scroll_pinterest.py <query_or_url> <download_dir> [max_posts] [auto|manual] [min_saves_and_likes]
"""

from concurrent.futures import ThreadPoolExecutor
import json
import re
import sys
from typing import Any
import urllib.parse
import urllib.request

from spy_scroll_common import USER_AGENT, download_media_file, log, normalize_publish_date, run_spy_worker

RELAY_SCRIPT_RE = re.compile(
    r'window\.__PWS_RELAY_REGISTER_COMPLETED_REQUEST__\("[^"]*",\s*(\{.*?\})\);?</script>',
    re.DOTALL,
)
DATE_PUBLISHED_RE = re.compile(r'"datePublished"\s*:\s*"([^"]+)"')
PIN_ID_RE = re.compile(r"/pin/(\d{6,24})")


def build_pinterest_search_url(query: str) -> str:
    return "https://www.pinterest.com/search/pins/?q=" + urllib.parse.quote(query.strip())


def upgrade_pinterest_media_url(url: str) -> str:
    if not url:
        return url
    return re.sub(
        r"(https?://i\.pinimg\.com/)(?:236x|474x|564x|170x)/",
        r"\g<1>736x/",
        url,
    )


def clean_pinterest_title(raw: str, fallback_pin_id: str = "") -> str:
    text = (raw or "").strip()
    text = re.sub(r"\s*\bpin\s+page\s*$", "", text, flags=re.IGNORECASE).strip()
    text = re.sub(r"^Pin\s+by\s+[^:]+:\s*", "", text, flags=re.IGNORECASE).strip()
    text = re.sub(r"^Untitled\b.*$", "", text, flags=re.IGNORECASE).strip()
    if "|" in text:
        first_part = text.split("|", 1)[0].strip()
        if len(first_part) >= 10:
            text = first_part
    if not text and fallback_pin_id:
        return f"Pinterest Pin {fallback_pin_id}"
    return text[:200]


def _pick_mp4_from_video_list(vlist: Any) -> str:
    if not isinstance(vlist, dict):
        return ""
    for pref in ("V_720P", "V_EXP7", "V_EXP6", "V_EXP5", "V_EXP4", "V_EXP3", "V_HLSV3_MOBILE"):
        slot = vlist.get(pref)
        if isinstance(slot, dict):
            u = str(slot.get("url") or "").strip()
            if u.lower().split("?", 1)[0].endswith(".mp4"):
                return u
    for slot in vlist.values():
        if isinstance(slot, dict):
            u = str(slot.get("url") or "").strip()
            if u.lower().split("?", 1)[0].endswith(".mp4"):
                return u
    return ""


def _extract_pinterest_video_url(d: dict) -> str:
    for vk in ("videos", "video"):
        vobj = d.get(vk)
        if isinstance(vobj, dict):
            found = _pick_mp4_from_video_list(vobj.get("video_list") or vobj.get("videoList") or vobj)
            if found:
                return found
    found = _pick_mp4_from_video_list(d.get("video_list") or d.get("videoList"))
    if found:
        return found
    for sk in ("story_pin_data", "storyPinData"):
        spd = d.get(sk)
        if isinstance(spd, dict):
            pages = spd.get("pages")
            if isinstance(pages, list):
                for pg in pages:
                    if not isinstance(pg, dict):
                        continue
                    for blk in pg.get("blocks") or []:
                        if not isinstance(blk, dict):
                            continue
                        vobj = blk.get("video") or blk.get("videos")
                        if isinstance(vobj, dict):
                            found = _pick_mp4_from_video_list(
                                vobj.get("video_list") or vobj.get("videoList") or vobj
                            )
                            if found:
                                return found
    return ""


def _extract_pin_dict_metadata(d: dict) -> dict:
    meta: dict[str, Any] = {}
    if (
        d.get("isPromoted")
        or d.get("isThirdPartyAd")
        or d.get("is_promoted")
        or d.get("promotedIsRemovable")
        or d.get("adData") is not None
    ):
        meta["is_ad"] = True

    saves: int | None = None
    comments: int | None = None
    likes: int | None = None

    for agg_key in ("aggregatedPinData", "aggregated_pin_data"):
        agg = d.get(agg_key)
        if isinstance(agg, dict):
            for st_key in ("aggregatedStats", "aggregated_stats"):
                st = agg.get(st_key)
                if isinstance(st, dict) and isinstance(st.get("saves"), int):
                    saves = max(saves or 0, st["saves"])
            for ck in ("commentCount", "comment_count"):
                if isinstance(agg.get(ck), int):
                    comments = max(comments or 0, agg[ck])

    for rk in ("repinCount", "repin_count", "save_count"):
        if isinstance(d.get(rk), int):
            saves = max(saves or 0, d[rk])

    for lk in ("totalReactionCount", "total_reaction_count", "total_reactions"):
        if isinstance(d.get(lk), int):
            likes = max(likes or 0, d[lk])

    rc = d.get("reaction_counts") or d.get("reactionCounts")
    if isinstance(rc, dict):
        rc_sum = sum(v for v in rc.values() if isinstance(v, int) and v > 0)
        likes = max(likes or 0, rc_sum)

    for ck in ("commentCount", "comment_count"):
        if isinstance(d.get(ck), int):
            comments = max(comments or 0, d[ck])

    if saves is not None:
        meta["shares"] = saves
    if likes is not None:
        meta["likes"] = likes
    if comments is not None:
        meta["comments"] = comments

    for pk in ("pinner", "nativeCreator", "native_creator"):
        pinner = d.get(pk)
        if isinstance(pinner, dict):
            uname = str(pinner.get("username") or "").strip()
            if uname:
                meta["author"] = uname if uname.startswith("@") else f"@{uname}"
                break

    for dk in ("createdAt", "created_at", "datePublished"):
        raw_dt = d.get(dk)
        if raw_dt:
            norm_dt = normalize_publish_date(raw_dt)
            if norm_dt:
                meta["published_at"] = norm_dt
                break

    title_candidates: list[str] = []
    for tk in ("gridTitle", "grid_title", "closeupUnifiedTitle", "title", "description", "seoTitle"):
        val = d.get(tk)
        if isinstance(val, str) and val.strip():
            title_candidates.append(val.strip())
    pin_join = d.get("pinJoin")
    if isinstance(pin_join, dict):
        va = pin_join.get("visualAnnotation")
        if isinstance(va, list) and va and isinstance(va[0], str):
            title_candidates.append(va[0].strip())

    for cand in title_candidates:
        cleaned = clean_pinterest_title(cand)
        if cleaned:
            meta["title"] = cleaned
            break

    video_url = _extract_pinterest_video_url(d)
    if video_url:
        meta["video_url"] = video_url
        meta["media_url"] = video_url

    images = d.get("images")
    if isinstance(images, dict):
        for size_key in ("736x", "orig", "564x", "474x"):
            slot = images.get(size_key)
            if isinstance(slot, dict) and isinstance(slot.get("url"), str) and slot["url"]:
                if not meta.get("media_url"):
                    meta["media_url"] = slot["url"]
                break
    elif isinstance(d.get("images_736x"), dict) and d["images_736x"].get("url"):
        if not meta.get("media_url"):
            meta["media_url"] = str(d["images_736x"]["url"])
    elif isinstance(d.get("images_474x"), dict) and d["images_474x"].get("url"):
        if not meta.get("media_url"):
            meta["media_url"] = upgrade_pinterest_media_url(str(d["images_474x"]["url"]))

    return meta


def _merge_pin_meta(cache: dict, pin_id: str, meta: dict) -> None:
    if not pin_id or not meta:
        return
    cur = cache.setdefault(pin_id, {})
    if meta.get("is_ad") or meta.get("is_promoted"):
        cur["is_ad"] = True
    for num_key in ("shares", "likes", "comments"):
        val = meta.get(num_key)
        if isinstance(val, int):
            cur[num_key] = max(cur.get(num_key, 0), val) if isinstance(cur.get(num_key), int) else val
    if meta.get("video_url"):
        cur["video_url"] = meta["video_url"]
        cur["media_url"] = meta["video_url"]
    for str_key in ("title", "author", "media_url", "published_at"):
        if meta.get(str_key) and not cur.get(str_key):
            cur[str_key] = meta[str_key]


def _walk_pinterest_json(obj: Any, cache: dict) -> None:
    if isinstance(obj, dict):
        raw_id = obj.get("entityId") or obj.get("id")
        if isinstance(raw_id, (str, int)):
            pid = str(raw_id).strip()
            if pid.isdigit() and len(pid) >= 6:
                if any(
                    k in obj
                    for k in (
                        "aggregatedPinData",
                        "aggregated_pin_data",
                        "repinCount",
                        "repin_count",
                        "totalReactionCount",
                        "total_reaction_count",
                        "pinner",
                        "seoTitle",
                        "gridTitle",
                        "createdAt",
                        "created_at",
                        "videos",
                        "video_list",
                        "story_pin_data",
                        "storyPinData",
                    )
                ):
                    _merge_pin_meta(cache, pid, _extract_pin_dict_metadata(obj))
        for val in obj.values():
            if isinstance(val, (dict, list)):
                _walk_pinterest_json(val, cache)
    elif isinstance(obj, list):
        for item in obj:
            if isinstance(item, (dict, list)):
                _walk_pinterest_json(item, cache)


def install_pinterest_response_listener(page, metadata_cache: dict) -> None:
    def on_response(resp) -> None:
        url = resp.url or ""
        if "pinterest." not in url:
            return
        if "Resource/get" not in url and "/graphql" not in url:
            return
        try:
            data = resp.json()
            _walk_pinterest_json(data, metadata_cache)
        except Exception:
            pass

    page.on("response", on_response)


def _fetch_public_pin_metadata(pin_id: str) -> dict:
    url = f"https://www.pinterest.com/pin/{pin_id}/"
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "text/html,application/xhtml+xml",
            "Accept-Language": "en-US,en;q=0.9",
        },
    )
    html = urllib.request.urlopen(req, timeout=12).read().decode("utf-8", errors="ignore")
    combined: dict[str, Any] = {}
    temp_cache: dict[str, dict] = {}
    for m in RELAY_SCRIPT_RE.finditer(html):
        try:
            payload = json.loads(m.group(1))
        except Exception:
            continue
        _walk_pinterest_json(payload, temp_cache)
    if pin_id in temp_cache:
        combined.update(temp_cache[pin_id])
    if not combined.get("published_at"):
        dm = DATE_PUBLISHED_RE.search(html)
        if dm:
            norm_dt = normalize_publish_date(dm.group(1))
            if norm_dt:
                combined["published_at"] = norm_dt
    return combined


def enrich_pinterest_batch(candidates: list[dict], _context, metadata_cache: dict) -> None:
    missing_ids: list[str] = []
    pin_ids_by_idx: dict[int, str] = {}

    for idx, cand in enumerate(candidates):
        m = PIN_ID_RE.search(cand.get("url") or "")
        if not m:
            continue
        pin_id = m.group(1)
        pin_ids_by_idx[idx] = pin_id

        # Offline local HTML fixtures supply their own file:// media or data-* attributes
        media_url = str(cand.get("media_url") or "")
        if media_url.startswith("file://"):
            continue
        if cand.get("shares") is not None or cand.get("likes") is not None:
            continue

        cached = metadata_cache.get(pin_id)
        if (
            not cached
            or (cached.get("shares") is None and cached.get("likes") is None)
            or not cached.get("published_at")
            or (cand.get("has_video") and not cached.get("video_url"))
        ):
            if pin_id not in missing_ids:
                missing_ids.append(pin_id)

    if missing_ids:
        def _safe_fetch(pid: str) -> tuple[str, dict]:
            try:
                return (pid, _fetch_public_pin_metadata(pid))
            except Exception as exc:
                log("pinterest", f"Pin metadata fetch failed for {pid}: {exc}")
                return (pid, {})

        with ThreadPoolExecutor(max_workers=6) as pool:
            for pid, fetched in pool.map(_safe_fetch, missing_ids):
                if fetched:
                    _merge_pin_meta(metadata_cache, pid, fetched)

    for idx, pin_id in pin_ids_by_idx.items():
        cand = candidates[idx]
        cached = metadata_cache.get(pin_id) or {}
        if cached.get("is_ad") or cached.get("is_promoted"):
            cand["is_ad"] = True
        for k in ("shares", "likes", "comments"):
            if cand.get(k) is None and isinstance(cached.get(k), int):
                cand[k] = cached[k]
        if cached.get("author") and not cand.get("author"):
            cand["author"] = cached["author"]
        if cached.get("published_at") and not cand.get("published_at"):
            cand["published_at"] = cached["published_at"]
        if cached.get("video_url"):
            cand["media_url"] = cached["video_url"]
        elif cached.get("media_url") and not cand.get("media_url"):
            cand["media_url"] = cached["media_url"]

        cleaned_existing = clean_pinterest_title(cand.get("title") or "")
        if cleaned_existing and not cleaned_existing.startswith("Pinterest Pin "):
            cand["title"] = cleaned_existing
        elif cached.get("title"):
            cand["title"] = cached["title"]
        else:
            cand["title"] = clean_pinterest_title(cand.get("title") or "", fallback_pin_id=pin_id)


def score_pinterest_post(post: dict) -> int:
    saves = post.get("shares")
    likes = post.get("likes")
    comments = post.get("comments")
    return (
        (saves if isinstance(saves, int) and saves > 0 else 0)
        + (likes if isinstance(likes, int) and likes > 0 else 0)
        + (comments if isinstance(comments, int) and comments > 0 else 0)
    )


EXTRACT_PINTEREST_JS = r"""
() => {
    const results = [];
    const seen = new Set();
    const anchors = Array.from(document.querySelectorAll('a[href*="/pin/"]'));
    for (const a of anchors) {
        const rawHref = a.getAttribute('href') || '';
        const m = rawHref.match(/\/pin\/([0-9A-Za-z_-]+)/);
        if (!m) continue;
        const pinUrl = new URL('/pin/' + m[1] + '/', window.location.origin).href.replace(/^file:\/\/\/pin\//, 'https://www.pinterest.com/pin/');
        if (seen.has(pinUrl)) continue;

        const card = a.closest('[data-test-id="pin"], [data-test-id="pinWrapper"], [role="listitem"], article, div') || a;
        const img = a.querySelector('img') || card.querySelector('img');
        const video = a.querySelector('video') || card.querySelector('video');

        let mediaUrl = '';
        const dataVideoUrl = a.getAttribute('data-video-url') || card.getAttribute('data-video-url') || '';
        if (dataVideoUrl) {
            mediaUrl = dataVideoUrl;
        }
        if (!mediaUrl && video) {
            const sourceEl = video.querySelector('source[src]');
            const rawVidSrc = video.currentSrc || video.getAttribute('src') || (sourceEl ? sourceEl.getAttribute('src') : '') || '';
            if (rawVidSrc && !rawVidSrc.startsWith('blob:')) {
                mediaUrl = rawVidSrc;
            }
        }
        if (!mediaUrl && img) {
            const srcset = img.getAttribute('srcset') || '';
            if (srcset) {
                const candidates = srcset.split(',').map(s => s.trim().split(/\s+/)[0]).filter(Boolean);
                if (candidates.length > 0) {
                    mediaUrl = candidates[candidates.length - 1];
                }
            }
            if (!mediaUrl) {
                mediaUrl = img.currentSrc || img.src || img.getAttribute('src') || '';
            }
        }
        if (!mediaUrl && video) {
            mediaUrl = video.getAttribute('poster') || '';
        }
        const hasVideo = Boolean(
            dataVideoUrl
            || video
            || card.querySelector('[data-test-id*="video" i], [data-test-id="pinrep-video"]')
        );

        let title = (a.getAttribute('aria-label') || a.getAttribute('title') || (img ? img.getAttribute('alt') : '') || '').trim();
        title = title.replace(/\s*\bpin\s+page\s*$/i, '').replace(/^Pin\s+by\s+/i, '').trim();
        if (/^untitled$/i.test(title)) {
            title = '';
        }
        if (!title) {
            const firstLine = (card.innerText || '').split('\n').map(s => s.trim()).filter(Boolean)[0] || '';
            if (firstLine && !/^save$|^enregistrer$/i.test(firstLine)) {
                title = firstLine;
            }
        }

        const savesAttr = a.getAttribute('data-saves') || card.getAttribute('data-saves');
        const likesAttr = a.getAttribute('data-likes') || card.getAttribute('data-likes');
        const commentsAttr = a.getAttribute('data-comments') || card.getAttribute('data-comments');
        const authorAttr = a.getAttribute('data-author') || card.getAttribute('data-author') || '';
        const createdAttr = a.getAttribute('data-created') || card.getAttribute('data-created') || '';
        const promotedAttr = (
            a.getAttribute('data-promoted') === 'true'
            || card.getAttribute('data-promoted') === 'true'
            || a.getAttribute('data-ad') === 'true'
            || card.getAttribute('data-ad') === 'true'
            || Boolean(card.querySelector('[data-test-id*="promoted" i], [data-test-id*="oneTapPromotedPin" i]'))
            || /\b(Promoted\s+by|Sponsored|Sponsorisé|Annonce|Gesponsert|Promocionado)\b/i.test(card.innerText || '')
        );

        seen.add(pinUrl);
        results.push({
            url: pinUrl,
            title: title.slice(0, 200),
            media_url: mediaUrl,
            author: authorAttr,
            published_at: createdAttr,
            views: null,
            likes: likesAttr,
            comments: commentsAttr,
            shares: savesAttr,
            is_ad: promotedAttr,
            has_video: hasVideo
        });
    }
    return results;
}
"""


def _run_backfill_mode(urls: list[str]) -> int:
    pin_map: dict[str, str] = {}
    for u in urls:
        m = PIN_ID_RE.search(u)
        if m:
            pin_map[u] = m.group(1)

    def _safe(pid: str) -> tuple[str, dict]:
        try:
            return (pid, _fetch_public_pin_metadata(pid))
        except Exception as exc:
            log("pinterest", f"Backfill fetch failed for {pid}: {exc}")
            return (pid, {})

    unique_pids = list(dict.fromkeys(pin_map.values()))
    fetched_by_pid: dict[str, dict] = {}
    if unique_pids:
        with ThreadPoolExecutor(max_workers=6) as pool:
            for pid, meta in pool.map(_safe, unique_pids):
                if meta:
                    fetched_by_pid[pid] = meta

    out: dict[str, dict] = {}
    for u, pid in pin_map.items():
        if pid in fetched_by_pid:
            out[u] = fetched_by_pid[pid]
    print(json.dumps({"items": out}), flush=True)
    return 0


def _run_download_video_mode(pin_url: str, dest_dir: str) -> int:
    m = PIN_ID_RE.search(pin_url or "")
    if not m or not dest_dir:
        print(json.dumps({"video_path": ""}), flush=True)
        return 0
    pin_id = m.group(1)
    try:
        meta = _fetch_public_pin_metadata(pin_id)
    except Exception as exc:
        log("pinterest", f"Video lookup failed for {pin_id}: {exc}")
        meta = {}
    video_url = str(meta.get("video_url") or "").strip()
    if not video_url:
        print(json.dumps({"video_path": ""}), flush=True)
        return 0
    local_path = download_media_file(None, video_url, pin_url, dest_dir, 0, "pinterest_video")
    print(json.dumps({"video_path": local_path if local_path.lower().endswith(".mp4") else ""}), flush=True)
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--backfill":
        sys.exit(_run_backfill_mode(sys.argv[2:]))
    if len(sys.argv) >= 4 and sys.argv[1] == "--download-video":
        sys.exit(_run_download_video_mode(sys.argv[2], sys.argv[3]))
    sys.exit(
        run_spy_worker(
            "pinterest",
            build_pinterest_search_url,
            EXTRACT_PINTEREST_JS,
            upgrade_media_url=upgrade_pinterest_media_url,
            install_response_listener=install_pinterest_response_listener,
            enrich_batch=enrich_pinterest_batch,
            score_post=score_pinterest_post,
        )
    )
