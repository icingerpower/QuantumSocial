#!/usr/bin/env python3
"""Scrolls a YouTube search or Shorts/channel page and saves candidate videos + thumbnails + view counts.

Usage:
    spy_scroll_youtube.py <query_or_url> <download_dir> [max_posts] [auto|manual] [min_views]
"""

import sys
import urllib.parse

from spy_scroll_common import run_spy_worker


def build_youtube_search_url(query: str) -> str:
    return "https://www.youtube.com/results?search_query=" + urllib.parse.quote(query.strip())


def score_youtube_post(post: dict) -> int:
    views = post.get("views")
    if isinstance(views, int) and views >= 0:
        return views
    likes = post.get("likes")
    return likes if isinstance(likes, int) and likes > 0 else 0


EXTRACT_YOUTUBE_JS = r"""
() => {
    const results = [];
    const seen = new Set();
    const anchors = Array.from(document.querySelectorAll('a[href*="/shorts/"], a[href*="watch?v="]'));
    for (const a of anchors) {
        let href = a.href || a.getAttribute('href') || '';
        if (!href) continue;
        let videoId = '';
        let canonical = '';
        const shortsMatch = href.match(/\/shorts\/([0-9A-Za-z_-]{6,})/);
        const watchMatch = href.match(/[?&]v=([0-9A-Za-z_-]{6,})/);
        if (shortsMatch) {
            videoId = shortsMatch[1];
            canonical = 'https://www.youtube.com/shorts/' + videoId;
        } else if (watchMatch) {
            videoId = watchMatch[1];
            canonical = 'https://www.youtube.com/watch?v=' + videoId;
        } else {
            continue;
        }
        if (seen.has(canonical)) continue;

        const card = a.closest('ytd-video-renderer, ytd-reel-item-renderer, ytd-rich-item-renderer, ytm-shorts-lockup-view-model, article, div') || a;
        const img = card.querySelector('img') || a.querySelector('img');
        let mediaUrl = img ? (img.currentSrc || img.src || img.getAttribute('src') || '') : '';
        if (!mediaUrl && videoId) {
            mediaUrl = 'https://i.ytimg.com/vi/' + videoId + '/hqdefault.jpg';
        }

        let title = (a.getAttribute('title') || '').trim();
        if (!title) {
            const titleEl = card.querySelector('#video-title, h3, [id*="title"]');
            if (titleEl) title = (titleEl.innerText || '').trim();
        }
        const ariaLabel = (a.getAttribute('aria-label') || '').trim();
        if (!title && ariaLabel) {
            title = ariaLabel;
        }
        if (!title) title = canonical;

        let views = a.getAttribute('data-views') || card.getAttribute('data-views') || null;
        if (!views) {
            const metaSpans = Array.from(card.querySelectorAll('#metadata-line span, .inline-metadata-item, span'));
            for (const sp of metaSpans) {
                const t = (sp.innerText || '').trim();
                if (/^[0-9][0-9.,\s]*[KkMmBb]?\s*(views|view|vues|vue|spectateurs)\b/i.test(t)) {
                    views = t;
                    break;
                }
            }
        }
        if (!views && ariaLabel) {
            const vm = ariaLabel.match(/([0-9][0-9.,\s]*[KkMmBb]?)\s*(?:views|vues)\b/i);
            if (vm) views = vm[1];
        }

        let author = '';
        const ch = card.querySelector('ytd-channel-name a, #channel-name a, #text.ytd-channel-name');
        if (ch && ch.innerText) {
            author = ch.innerText.trim().split('\n')[0].trim();
        }

        const isAd = (
            a.getAttribute('data-ad') === 'true'
            || card.getAttribute('data-ad') === 'true'
            || Boolean(card.closest('ytd-ad-slot-renderer, ytd-in-feed-ad-layout-renderer'))
            || Boolean(card.querySelector('[aria-label*="Sponsored" i], .badge-style-type-ad'))
        );

        let publishedAt = a.getAttribute('data-created') || card.getAttribute('data-created') || '';
        if (!publishedAt) {
            const metaSpans = Array.from(card.querySelectorAll('#metadata-line span, .inline-metadata-item, span'));
            for (const sp of metaSpans) {
                const t = (sp.innerText || '').trim();
                if (/(\bago\b|^il y a\b)/i.test(t) && /\d/.test(t)) {
                    publishedAt = t;
                    break;
                }
            }
        }

        seen.add(canonical);
        results.push({
            url: canonical,
            title: title.slice(0, 200),
            media_url: mediaUrl,
            author: author,
            published_at: publishedAt,
            views: views,
            likes: null,
            comments: null,
            shares: null,
            is_ad: isAd
        });
    }
    return results;
}
"""


if __name__ == "__main__":
    sys.exit(
        run_spy_worker(
            "youtube",
            build_youtube_search_url,
            EXTRACT_YOUTUBE_JS,
            score_post=score_youtube_post,
        )
    )
