#!/usr/bin/env python3
"""Scans a TikTok profile URL for published videos and their statistics.

Strategy:
1. Fetches https://www.tiktok.com/embed/@<handle> and parses the embedded
   __FRONTITY_CONNECT_STATE__ JSON, which publicly exposes:
   - userInfo (followerCount, heartCount, uniqueId, id)
   - videoList (recent public videos with id, desc, playCount)
   without requiring a login or triggering the main profile page's empty
   /api/post/item_list bot-block.
2. For each known QuantumSocial short code (passed via argv[2], QS_SHORT_CODES,
   or discovered in the active working directory's videos.json), queries
   https://www.tiktok.com/embed/tag/qsXXXX so older videos beyond the 10-video
   creator embed window are also discovered when their authorUniqueId matches
   <handle>.
3. For every discovered video ID, fetches the public video page
   https://www.tiktok.com/@<handle>/video/<id> and extracts the full
   itemStruct statistics from __UNIVERSAL_DATA_FOR_REHYDRATION__:
   playCount (views), diggCount (likes), commentCount (comments),
   shareCount + collectCount (shares/saves), createTime (publish_date), and
   desc (caption).
4. Logs every step and diagnostic detail to ~/.cache/quantumsocial/tiktok_stats.log
   (and QS_LOG_FILE if set).

Prints a single JSON line to stdout:
    {"followers": <int|null>, "views": <int|null>, "likes": <int|null>,
     "posts": [{"url": <str>, "short_code": <str|null>, "caption": <str|null>,
                "publish_date": <int|str|null>, "source": "public",
                "views": <int|null>, "likes": <int|null>,
                "comments": <int|null>, "shares": <int|null>}],
     "error": <str|null>}

Usage: fetch_account_videos_tiktok.py <profile-url> [comma-separated-short-codes]
"""
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)

LOG_DIR = os.path.expanduser("~/.cache/quantumsocial")
DEFAULT_LOG_PATH = os.path.join(LOG_DIR, "tiktok_stats.log")

SHORT_CODE_RE = re.compile(
    r"(?:#\s*qs[-_]?|\bqs-)([0-9a-hjkmnp-tv-z]{4,})\b", re.IGNORECASE
)


def _log(message):
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S ")
    line = timestamp + message + "\n"
    paths = [DEFAULT_LOG_PATH]
    extra_log = os.environ.get("QS_LOG_FILE", "").strip()
    if extra_log and extra_log not in paths:
        paths.append(extra_log)
    for path in paths:
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "a", encoding="utf-8") as handle:
                handle.write(line)
        except OSError:
            pass


def _emit(followers=None, views=None, likes=None, posts=None, error=None):
    payload = {
        "followers": followers,
        "views": views,
        "likes": likes,
        "posts": posts or [],
        "error": error,
    }
    _log(
        f"EMIT followers={followers} views={views} likes={likes} "
        f"posts={len(payload['posts'])} error={error!r}"
    )
    print(json.dumps(payload))


def _extract_short_code(text):
    if not text:
        return None
    m = SHORT_CODE_RE.search(text)
    return ("QS-" + m.group(1).upper()) if m else None


def _to_int(value):
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return int(value)
    if isinstance(value, str) and value.strip().isdigit():
        return int(value.strip())
    return None


def _http_get(url, timeout=15):
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            "Accept-Language": "en-US,en;q=0.9",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="ignore")


def _discover_known_short_codes():
    codes = []
    raw_sources = []
    if len(sys.argv) >= 3:
        raw_sources.append(sys.argv[2])
    if os.environ.get("QS_SHORT_CODES"):
        raw_sources.append(os.environ["QS_SHORT_CODES"])

    for raw in raw_sources:
        for token in re.split(r"[,;\s]+", raw.strip()):
            sc = _extract_short_code(token)
            if sc and sc not in codes:
                codes.append(sc)

    if codes:
        return codes

    # Fallback when run standalone from CLI: check QuantumSocial.conf for
    # the active working directory's videos.json.
    conf_path = os.path.expanduser("~/.config/GRDF/QuantumSocial.conf")
    try:
        if os.path.isfile(conf_path):
            with open(conf_path, encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if line.startswith("recents-="):
                        first_dir = line.split("=", 1)[1].strip().split(",")[0].strip()
                        videos_json = os.path.join(first_dir, "videos.json")
                        if os.path.isfile(videos_json):
                            with open(videos_json, encoding="utf-8") as vf:
                                data = json.load(vf)
                            for v in data.get("videos", []):
                                sc = _extract_short_code(v.get("shortCode", ""))
                                if sc and sc not in codes:
                                    codes.append(sc)
    except Exception as exc:
        _log(f"fallback short-code discovery failed: {exc}")

    return codes


def _parse_frontity_state(html):
    m = re.search(
        r'<script[^>]*id="__FRONTITY_CONNECT_STATE__"[^>]*>(.*?)</script>',
        html,
        re.DOTALL,
    )
    if not m:
        return None
    try:
        return json.loads(m.group(1))
    except Exception as exc:
        _log(f"failed to parse __FRONTITY_CONNECT_STATE__: {exc}")
        return None


def _fetch_creator_embed(handle):
    url = f"https://www.tiktok.com/embed/@{handle}"
    _log(f"fetching creator embed: {url}")
    html = _http_get(url, timeout=15)
    state = _parse_frontity_state(html)
    if not state:
        return None, None, []
    data_map = state.get("source", {}).get("data", {})
    if not data_map:
        return None, None, []
    entry = next(iter(data_map.values()))
    user_info = entry.get("userInfo") or {}
    followers = _to_int(user_info.get("followerCount"))
    likes = _to_int(user_info.get("heartCount"))
    video_list = entry.get("videoList") or entry.get("videoData") or []
    _log(
        f"creator embed @{handle}: followers={followers} likes={likes} "
        f"videoList={len(video_list)}"
    )
    return followers, likes, video_list


def _fetch_tag_embed(tag_name, handle):
    url = f"https://www.tiktok.com/embed/tag/{tag_name}"
    try:
        html = _http_get(url, timeout=10)
    except urllib.error.HTTPError as exc:
        if exc.code != 400 and exc.code != 404:
            _log(f"tag embed {tag_name} HTTP {exc.code}")
        return []
    except Exception as exc:
        _log(f"tag embed {tag_name} error: {exc}")
        return []

    state = _parse_frontity_state(html)
    if not state:
        return []
    data_map = state.get("source", {}).get("data", {})
    if not data_map:
        return []
    entry = next(iter(data_map.values()))
    vlist = entry.get("videoList") or entry.get("videoData") or []
    matched = []
    for item in vlist:
        if not isinstance(item, dict):
            continue
        author = (item.get("authorUniqueId") or "").strip().lower()
        if not handle or author == handle.lower():
            matched.append(item)
    if matched:
        _log(f"tag embed #{tag_name}: matched {len(matched)} video(s) for @{handle}")
    return matched


def _fetch_video_detail(handle, video_id, fallback_item=None):
    url = f"https://www.tiktok.com/@{handle}/video/{video_id}"
    fallback_desc = (fallback_item or {}).get("desc") or ""
    fallback_views = _to_int((fallback_item or {}).get("playCount"))
    try:
        html = _http_get(url, timeout=15)
        m = re.search(
            r'<script[^>]+id="__UNIVERSAL_DATA_FOR_REHYDRATION__"[^>]*>(.*?)</script>',
            html,
            re.DOTALL,
        )
        if m:
            data = json.loads(m.group(1))
            item = (
                data.get("__DEFAULT_SCOPE__", {})
                .get("webapp.video-detail", {})
                .get("itemInfo", {})
                .get("itemStruct")
            )
            if isinstance(item, dict):
                caption = item.get("desc") or fallback_desc
                stats = item.get("stats") or item.get("statsV2") or {}
                views = _to_int(stats.get("playCount"))
                if views is None:
                    views = fallback_views
                likes = _to_int(stats.get("diggCount"))
                comments = _to_int(stats.get("commentCount"))
                shares_only = _to_int(stats.get("shareCount"))
                collects = _to_int(stats.get("collectCount"))
                if shares_only is not None or collects is not None:
                    shares = (shares_only or 0) + (collects or 0)
                else:
                    shares = None
                short_code = _extract_short_code(caption)
                _log(
                    f"video {video_id}: code={short_code} views={views} "
                    f"likes={likes} comments={comments} shares={shares}"
                )
                return {
                    "url": url,
                    "short_code": short_code,
                    "caption": caption,
                    "publish_date": _to_int(item.get("createTime")),
                    "source": "public",
                    "views": views,
                    "likes": likes,
                    "comments": comments,
                    "shares": shares,
                }
    except Exception as exc:
        _log(f"video detail {video_id} failed: {exc}")

    return {
        "url": url,
        "short_code": _extract_short_code(fallback_desc),
        "caption": fallback_desc,
        "publish_date": None,
        "source": "public",
        "views": fallback_views,
        "likes": None,
        "comments": None,
        "shares": None,
    }


def main():
    if len(sys.argv) < 2:
        _emit(error="usage: fetch_account_videos_tiktok.py <profile-url> [short-codes]")
        return 1
    profile_url = sys.argv[1].strip()
    _log(f"=== start fetch_account_videos_tiktok: url={profile_url} ===")

    handle_m = re.search(r"tiktok\.com/@([^/?#]+)", profile_url)
    if not handle_m:
        _emit(error=f"Could not extract TikTok handle (@username) from URL: {profile_url}")
        return 1
    handle = handle_m.group(1).strip()

    followers = None
    likes = None
    items_by_id = {}

    try:
        followers, likes, embed_videos = _fetch_creator_embed(handle)
        for item in embed_videos:
            vid = str(item.get("id") or "").strip()
            if vid.isdigit():
                items_by_id[vid] = item
    except Exception as exc:
        _log(f"creator embed failed for @{handle}: {exc}")

    # Check known short codes via /embed/tag/qsXXXX to find any videos beyond
    # the 10-video creator embed list.
    known_codes = _discover_known_short_codes()
    already_found_codes = {
        _extract_short_code(item.get("desc") or "")
        for item in items_by_id.values()
    }
    missing_tags = []
    for sc in known_codes:
        if sc not in already_found_codes and sc.startswith("QS-"):
            missing_tags.append((sc, "qs" + sc[3:].lower()))

    if missing_tags:
        _log(f"checking {len(missing_tags)} tag embed endpoint(s) for @{handle}")
        with ThreadPoolExecutor(max_workers=6) as pool:
            futures = {
                pool.submit(_fetch_tag_embed, tag_name, handle): (sc, tag_name)
                for sc, tag_name in missing_tags
            }
            for fut in as_completed(futures):
                sc, _ = futures[fut]
                for item in fut.result():
                    vid = str(item.get("id") or "").strip()
                    if vid.isdigit() and vid not in items_by_id:
                        if not item.get("desc"):
                            item["desc"] = f"#{sc}"
                        items_by_id[vid] = item

    # Fetch full per-video stats for every discovered video in parallel.
    posts = []
    if items_by_id:
        _log(f"fetching per-video details for {len(items_by_id)} video(s)")
        with ThreadPoolExecutor(max_workers=6) as pool:
            futures = {
                pool.submit(_fetch_video_detail, handle, vid, item): vid
                for vid, item in items_by_id.items()
            }
            for fut in as_completed(futures):
                post = fut.result()
                if post:
                    posts.append(post)

    # Sort posts newest-first by video ID.
    posts.sort(key=lambda p: p.get("url", ""), reverse=True)

    # Compute total views across discovered videos if available.
    view_values = [p["views"] for p in posts if isinstance(p.get("views"), int) and p["views"] >= 0]
    total_views = sum(view_values) if view_values else None

    if followers is None and likes is None and not posts:
        _emit(
            error=(
                f"Could not retrieve TikTok statistics for @{handle}. "
                f"See log: {DEFAULT_LOG_PATH}"
            )
        )
        return 1

    _emit(followers=followers, views=total_views, likes=likes, posts=posts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
