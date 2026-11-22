#!/usr/bin/env python3
"""Scrolls a TikTok search, hashtag, or profile page and saves candidate posts + videos/covers.

Usage:
    spy_scroll_tiktok.py <query_or_url> <download_dir> [max_posts] [auto|manual] [min_views] [max_days_old]
"""

import json
import re
import sys
from typing import Any
import urllib.parse

from spy_scroll_common import (
    log,
    normalize_publish_date,
    parse_compact_count,
    run_spy_worker,
)

TIKTOK_POST_ID_RE = re.compile(r"/(?:video|photo)/(\d{6,24})")


def build_tiktok_search_url(query: str) -> str:
    return "https://www.tiktok.com/search?q=" + urllib.parse.quote(query.strip())


def _first_http_url(val: Any) -> str:
    if isinstance(val, str):
        u = val.strip()
        return u if u.startswith(("http://", "https://")) else ""
    if isinstance(val, dict):
        for key in ("UrlList", "urlList", "url_list"):
            lst = val.get(key)
            if isinstance(lst, list):
                for entry in lst:
                    if isinstance(entry, str) and entry.strip().startswith(("http://", "https://")):
                        return entry.strip()
        for key in ("url", "uri", "Url"):
            u = val.get(key)
            if isinstance(u, str) and u.strip().startswith(("http://", "https://")):
                return u.strip()
    if isinstance(val, list):
        for entry in val:
            found = _first_http_url(entry)
            if found:
                return found
    return ""


def _extract_tiktok_video_and_cover(d: dict) -> tuple[str, str]:
    image_post = d.get("imagePost")
    is_slideshow = isinstance(image_post, dict) and bool(image_post.get("images"))

    cover_url = ""
    if is_slideshow and isinstance(image_post, dict):
        images = image_post.get("images") or []
        if isinstance(images, list) and images and isinstance(images[0], dict):
            cover_url = _first_http_url(
                images[0].get("imageURL") or images[0].get("imageUrl") or images[0]
            )
        if not cover_url:
            cover_obj = image_post.get("cover")
            if isinstance(cover_obj, dict):
                cover_url = _first_http_url(cover_obj.get("imageURL") or cover_obj)

    video = d.get("video")
    video_url = ""
    if isinstance(video, dict):
        if not cover_url:
            for ck in ("originCover", "cover", "dynamicCover"):
                u = _first_http_url(video.get(ck))
                if u:
                    cover_url = u
                    break
            if not cover_url and isinstance(video.get("zoomCover"), dict):
                for zk in ("720", "480", "960", "240"):
                    u = _first_http_url(video["zoomCover"].get(zk))
                    if u:
                        cover_url = u
                        break

        if not is_slideshow:
            # Prefer H.264 MP4 streams so Qt Multimedia & ffmpeg decode universally without HEVC issues.
            h264_candidates: list[tuple[int, str]] = []
            any_candidates: list[tuple[int, str]] = []
            bitrate_info = video.get("bitrateInfo")
            if isinstance(bitrate_info, list):
                for bi in bitrate_info:
                    if not isinstance(bi, dict):
                        continue
                    u = _first_http_url(bi.get("PlayAddr") or bi.get("playAddr"))
                    if not u:
                        continue
                    br = int(bi.get("Bitrate") or bi.get("bitrate") or 0)
                    codec = str(bi.get("CodecType") or bi.get("codecType") or "").lower()
                    any_candidates.append((br, u))
                    if codec.startswith("h264") or "avc" in codec:
                        h264_candidates.append((br, u))

            top_codec = str(video.get("codecType") or "").lower()
            top_play = _first_http_url(video.get("playAddr") or video.get("PlayAddr"))
            if top_play:
                top_br = int(video.get("bitrate") or 0)
                any_candidates.append((top_br, top_play))
                if top_codec.startswith("h264") or "avc" in top_codec or not top_codec:
                    h264_candidates.append((top_br + 1, top_play))

            dl_addr = _first_http_url(video.get("downloadAddr"))
            if dl_addr:
                any_candidates.append((0, dl_addr))

            if h264_candidates:
                h264_candidates.sort(key=lambda pair: pair[0], reverse=True)
                video_url = h264_candidates[0][1]
            elif any_candidates:
                any_candidates.sort(key=lambda pair: pair[0], reverse=True)
                video_url = any_candidates[0][1]

    return (video_url, cover_url)


def _extract_tiktok_item_metadata(d: dict) -> dict:
    meta: dict[str, Any] = {}

    if (
        d.get("isAd") is True
        or d.get("is_ad") is True
        or d.get("adInfo") is not None
        or d.get("adAuthorization") is True
    ):
        meta["is_ad"] = True

    views: int | None = None
    likes: int | None = None
    comments: int | None = None
    shares_only: int | None = None
    collects_only: int | None = None

    for st_key in ("stats", "statsV2", "statistics"):
        st = d.get(st_key)
        if not isinstance(st, dict):
            continue
        v = parse_compact_count(st.get("playCount") if "playCount" in st else st.get("play_count"))
        if v is not None:
            views = max(views or 0, v)
        lk = parse_compact_count(st.get("diggCount") if "diggCount" in st else st.get("digg_count"))
        if lk is not None:
            likes = max(likes or 0, lk)
        cm = parse_compact_count(st.get("commentCount") if "commentCount" in st else st.get("comment_count"))
        if cm is not None:
            comments = max(comments or 0, cm)
        sh = parse_compact_count(st.get("shareCount") if "shareCount" in st else st.get("share_count"))
        if sh is not None:
            shares_only = max(shares_only or 0, sh)
        cl = parse_compact_count(st.get("collectCount") if "collectCount" in st else st.get("collect_count"))
        if cl is not None:
            collects_only = max(collects_only or 0, cl)

    if views is not None:
        meta["views"] = views
    if likes is not None:
        meta["likes"] = likes
    if comments is not None:
        meta["comments"] = comments
    if shares_only is not None or collects_only is not None:
        meta["shares"] = (shares_only or 0) + (collects_only or 0)

    author_obj = d.get("author")
    if isinstance(author_obj, dict):
        uid = str(author_obj.get("uniqueId") or author_obj.get("unique_id") or "").strip()
        if uid:
            meta["author"] = uid if uid.startswith("@") else f"@{uid}"
    elif isinstance(author_obj, str) and author_obj.strip():
        uid = author_obj.strip()
        meta["author"] = uid if uid.startswith("@") else f"@{uid}"

    raw_ct = d.get("createTime") or d.get("create_time")
    if raw_ct:
        norm_dt = normalize_publish_date(raw_ct)
        if norm_dt:
            meta["published_at"] = norm_dt

    desc = str(d.get("desc") or d.get("contents_desc") or "").strip()
    if desc:
        meta["title"] = desc[:200]

    video_url, cover_url = _extract_tiktok_video_and_cover(d)
    if video_url:
        meta["video_url"] = video_url
    if cover_url:
        meta["cover_url"] = cover_url

    return meta


def _merge_tiktok_meta(cache: dict, post_id: str, meta: dict) -> None:
    if not post_id or not meta:
        return
    cur = cache.setdefault(post_id, {})
    if meta.get("is_ad"):
        cur["is_ad"] = True
    for num_key in ("views", "likes", "comments", "shares"):
        val = meta.get(num_key)
        if isinstance(val, int):
            cur[num_key] = max(cur.get(num_key, 0), val) if isinstance(cur.get(num_key), int) else val
    for str_key in ("video_url", "cover_url", "title", "author", "published_at"):
        if meta.get(str_key) and not cur.get(str_key):
            cur[str_key] = meta[str_key]


def _walk_tiktok_json(obj: Any, cache: dict) -> None:
    if isinstance(obj, dict):
        # Wrapper shape used by /api/search/general/full/: {"type": 1, "item": {...}}
        raw_id = obj.get("id") or obj.get("aweme_id")
        if isinstance(raw_id, (str, int)):
            pid = str(raw_id).strip()
            if pid.isdigit() and len(pid) >= 10:
                if any(
                    k in obj
                    for k in ("stats", "statsV2", "statistics", "video", "imagePost", "createTime")
                ):
                    _merge_tiktok_meta(cache, pid, _extract_tiktok_item_metadata(obj))
        for val in obj.values():
            if isinstance(val, (dict, list)):
                _walk_tiktok_json(val, cache)
    elif isinstance(obj, list):
        for entry in obj:
            if isinstance(entry, (dict, list)):
                _walk_tiktok_json(entry, cache)


def install_tiktok_response_listener(page, metadata_cache: dict) -> None:
    def on_response(resp) -> None:
        url = resp.url or ""
        if "tiktok.com" not in url or "/api/" not in url:
            return
        if not any(
            tok in url
            for tok in (
                "/search/",
                "/item_list",
                "/post/",
                "/recommend/",
                "/challenge/",
                "/music/",
                "/item/detail",
            )
        ):
            return
        try:
            data = resp.json()
            _walk_tiktok_json(data, metadata_cache)
        except Exception:
            pass

    page.on("response", on_response)


def _harvest_ssr_hydration_scripts(context, metadata_cache: dict) -> None:
    checked_urls = metadata_cache.setdefault("__ssr_urls", set())
    try:
        pages = [p for p in context.pages if not p.is_closed()]
        for pg in pages:
            pg_url = (pg.url or "").strip()
            if not pg_url or pg_url in checked_urls:
                continue
            checked_urls.add(pg_url)
            blobs = pg.evaluate(
                """() => {
                    const out = [];
                    for (const id of ['__UNIVERSAL_DATA_FOR_REHYDRATION__', 'SIGI_STATE']) {
                        const el = document.getElementById(id);
                        if (el && el.textContent) out.push(el.textContent);
                    }
                    return out;
                }"""
            )
            for raw in blobs or []:
                try:
                    _walk_tiktok_json(json.loads(raw), metadata_cache)
                except Exception:
                    continue
    except Exception as exc:
        log("tiktok", f"SSR hydration check skipped: {exc}")


def enrich_tiktok_batch(candidates: list[dict], context, metadata_cache: dict) -> None:
    if context is not None:
        _harvest_ssr_hydration_scripts(context, metadata_cache)

    for cand in candidates:
        m = TIKTOK_POST_ID_RE.search(cand.get("url") or "")
        if not m:
            continue
        post_id = m.group(1)
        cached = metadata_cache.get(post_id)
        if not isinstance(cached, dict):
            continue

        if cached.get("is_ad"):
            cand["is_ad"] = True

        for k in ("views", "likes", "comments", "shares"):
            c_val = cached.get(k)
            if isinstance(c_val, int):
                existing = cand.get(k)
                cand[k] = max(existing, c_val) if isinstance(existing, int) else c_val

        if cached.get("author") and not cand.get("author"):
            cand["author"] = cached["author"]
        if cached.get("published_at"):
            cand["published_at"] = cached["published_at"]
        if cached.get("title"):
            cur_title = str(cand.get("title") or "").strip()
            if not cur_title or cur_title.startswith("http"):
                cand["title"] = cached["title"]

        existing_media = str(cand.get("media_url") or "").strip()
        if existing_media.startswith(("data:", "blob:")):
            existing_media = ""
            cand["media_url"] = ""
        if not existing_media.startswith("file://"):
            video_url = str(cached.get("video_url") or "").strip()
            cover_url = str(cached.get("cover_url") or "").strip()
            if video_url:
                cand["fallback_media_url"] = cover_url or existing_media
                cand["media_url"] = video_url
            elif cover_url:
                cand["fallback_media_url"] = existing_media
                cand["media_url"] = cover_url


EXTRACT_TIKTOK_JS = r"""
() => {
    const results = [];
    const seen = new Set();
    const anchors = Array.from(document.querySelectorAll('a[href*="/video/"], a[href*="/photo/"]'));

    function extractPostId(href) {
        const m = (href || '').match(/\/(?:video|photo)\/(\d{6,24})/);
        return m ? m[1] : '';
    }

    function findOuterCard(anchor, postId) {
        const explicitCard = anchor.closest(
            '[class*="DivItemContainer"], [data-e2e="user-post-item"], [data-e2e="search_video-item"], article'
        );
        if (explicitCard) return explicitCard;
        let card = anchor;
        let cur = anchor.parentElement;
        for (let depth = 0; depth < 6 && cur && cur !== document.body; depth++) {
            if (cur.tagName === 'MAIN' || cur.tagName === 'SECTION') break;
            const innerLinks = cur.querySelectorAll('a[href*="/video/"], a[href*="/photo/"]');
            const distinctIds = new Set();
            for (const lnk of innerLinks) {
                const pid = extractPostId(lnk.getAttribute('href') || lnk.href || '');
                if (pid) distinctIds.add(pid);
                if (distinctIds.size > 1) break;
            }
            if (distinctIds.size > 1) break;
            card = cur;
            cur = cur.parentElement;
        }
        return card;
    }

    for (const a of anchors) {
        let href = a.getAttribute('href') || a.href || '';
        if (!href) continue;
        if (href.startsWith('/')) {
            href = 'https://www.tiktok.com' + href;
        }
        href = href.replace(/^file:\/\/\/@/, 'https://www.tiktok.com/@');
        href = href.split('?')[0];
        const postId = extractPostId(href);
        if (!postId) continue;
        if (seen.has(href)) continue;

        const card = findOuterCard(a, postId);
        const img = a.querySelector('img') || card.querySelector('img');
        const dataVideoUrl = a.getAttribute('data-video-url') || card.getAttribute('data-video-url') || '';
        let imgUrl = img ? (img.currentSrc || img.src || img.getAttribute('src') || '') : '';
        if (imgUrl.startsWith('data:') || imgUrl.startsWith('blob:')) {
            imgUrl = '';
        }
        const mediaUrl = dataVideoUrl || imgUrl;

        let title = '';
        const capEl = card.querySelector(
            '[data-e2e="search-card-video-caption"], [data-e2e="search-card-desc"], [data-e2e*="desc"]'
        );
        if (capEl && capEl.innerText) {
            title = capEl.innerText.split('\n')[0].trim();
        }
        if (!title && img) {
            title = (img.getAttribute('alt') || '').trim();
        }
        if (!title) {
            title = (a.getAttribute('title') || a.innerText || '').trim();
        }

        let author = a.getAttribute('data-author') || card.getAttribute('data-author') || '';
        if (!author) {
            const authorMatch = href.match(/tiktok\.com\/(@[^/?#]+)/);
            if (authorMatch) author = authorMatch[1];
        }
        if (!author) {
            const userEl = card.querySelector(
                '[data-e2e="search-card-user-unique-id"], [data-e2e="search-card-user-link"]'
            );
            if (userEl && userEl.innerText) {
                const rawUser = userEl.innerText.trim();
                if (rawUser) author = rawUser.startsWith('@') ? rawUser : ('@' + rawUser.replace(/\s+/g, ''));
            }
        }

        let views = a.getAttribute('data-views') || card.getAttribute('data-views') || null;
        if (!views) {
            const viewEl = card.querySelector('[data-e2e="video-views"], strong[data-e2e="video-views"]');
            if (viewEl && viewEl.innerText) {
                views = viewEl.innerText.trim();
            }
        }

        let likes = a.getAttribute('data-likes') || card.getAttribute('data-likes') || null;
        if (!likes) {
            const likeEl = card.querySelector(
                '[data-e2e="search-card-like-container"] strong, [data-e2e="like-count"], strong[data-e2e*="like"]'
            );
            if (likeEl && likeEl.innerText) {
                likes = likeEl.innerText.trim();
            }
        }

        const comments = a.getAttribute('data-comments') || card.getAttribute('data-comments') || null;
        const shares = a.getAttribute('data-shares') || card.getAttribute('data-shares') || null;

        const isAd = (
            a.getAttribute('data-ad') === 'true'
            || card.getAttribute('data-ad') === 'true'
            || Boolean(card.querySelector('[data-e2e*="ad-tag" i], [class*="SpanAdBadge" i]'))
            || /\b(Sponsored|Sponsorisé|Promoted)\b/i.test(card.innerText || '')
        );

        let publishedAt = a.getAttribute('data-created') || card.getAttribute('data-created') || '';
        if (!publishedAt) {
            const agoEl = card.querySelector('[data-e2e*="ago" i], [class*="SpanTimeTag" i]');
            if (agoEl && agoEl.innerText) {
                publishedAt = agoEl.innerText.trim();
            }
        }
        if (!publishedAt && postId.length >= 18 && postId.length <= 20) {
            try {
                const unixSec = Number(BigInt(postId) >> 32n);
                if (unixSec >= 1450000000 && unixSec <= 2500000000) {
                    publishedAt = String(unixSec);
                }
            } catch (_) {}
        }

        seen.add(href);
        results.push({
            url: href,
            title: (title || href).slice(0, 200),
            media_url: mediaUrl,
            fallback_media_url: dataVideoUrl ? imgUrl : '',
            author: author,
            published_at: publishedAt,
            views: views,
            likes: likes,
            comments: comments,
            shares: shares,
            is_ad: isAd
        });
    }
    return results;
}
"""


def score_tiktok_post(post: dict) -> int:
    views = post.get("views")
    if isinstance(views, int) and views > 0:
        return views
    likes = post.get("likes")
    comments = post.get("comments")
    shares = post.get("shares")
    engagement = (
        (likes if isinstance(likes, int) and likes > 0 else 0)
        + (comments if isinstance(comments, int) and comments > 0 else 0)
        + (shares if isinstance(shares, int) and shares > 0 else 0)
    )
    if engagement > 0:
        return engagement
    return views if isinstance(views, int) and views >= 0 else 0


if __name__ == "__main__":
    sys.exit(
        run_spy_worker(
            "tiktok",
            build_tiktok_search_url,
            EXTRACT_TIKTOK_JS,
            install_response_listener=install_tiktok_response_listener,
            enrich_batch=enrich_tiktok_batch,
            score_post=score_tiktok_post,
        )
    )
