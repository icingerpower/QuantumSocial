#!/usr/bin/env python3
"""Scrolls a TikTok search, hashtag, or profile page and saves candidate posts + thumbnails.

Usage:
    spy_scroll_tiktok.py <query_or_url> <download_dir> [max_posts] [auto|manual]
"""

import sys
import urllib.parse

from spy_scroll_common import run_spy_worker


def build_tiktok_search_url(query: str) -> str:
    return "https://www.tiktok.com/search?q=" + urllib.parse.quote(query.strip())


EXTRACT_TIKTOK_JS = r"""
() => {
    const results = [];
    const seen = new Set();
    const anchors = Array.from(document.querySelectorAll('a[href*="/video/"], a[href*="/photo/"]'));
    for (const a of anchors) {
        let href = a.getAttribute('href') || a.href || '';
        if (!href) continue;
        if (href.startsWith('/')) {
            href = 'https://www.tiktok.com' + href;
        }
        href = href.replace(/^file:\/\/\/@/, 'https://www.tiktok.com/@');
        href = href.split('?')[0];
        if (!/\/(video|photo)\/\d+/.test(href)) continue;
        if (seen.has(href)) continue;

        const card = a.closest('[data-e2e*="search"], [data-e2e*="user-post-item"], article, div') || a;
        const img = a.querySelector('img') || card.querySelector('img');
        const mediaUrl = img ? (img.currentSrc || img.src || img.getAttribute('src') || '') : '';

        let title = (img ? (img.getAttribute('alt') || '') : '').trim();
        if (!title) {
            const desc = card.querySelector('[data-e2e*="desc"], [data-e2e*="card-desc"]');
            if (desc) title = (desc.innerText || '').trim();
        }
        if (!title) {
            title = (a.getAttribute('title') || a.innerText || '').trim();
        }

        const authorMatch = href.match(/tiktok\.com\/(@[^/]+)/);
        const author = authorMatch ? authorMatch[1] : '';

        let views = null;
        const viewEl = card.querySelector('[data-e2e="video-views"], strong[data-e2e="video-views"]');
        if (viewEl && viewEl.innerText) {
            views = viewEl.innerText.trim();
        }

        const isAd = (
            a.getAttribute('data-ad') === 'true'
            || card.getAttribute('data-ad') === 'true'
            || Boolean(card.querySelector('[data-e2e*="ad-tag" i], [class*="SpanAdBadge" i]'))
            || /\b(Sponsored|Sponsorisé)\b/i.test(card.innerText || '')
        );

        let publishedAt = a.getAttribute('data-created') || card.getAttribute('data-created') || '';
        if (!publishedAt) {
            const agoEl = card.querySelector('[data-e2e*="ago" i]');
            if (agoEl && agoEl.innerText) {
                publishedAt = agoEl.innerText.trim();
            }
        }
        if (!publishedAt) {
            const idMatch = href.match(/\/(?:video|photo)\/(\d{18,20})/);
            if (idMatch) {
                try {
                    const unixSec = Number(BigInt(idMatch[1]) >> 32n);
                    if (unixSec >= 1450000000 && unixSec <= 2500000000) {
                        publishedAt = String(unixSec);
                    }
                } catch (_) {}
            }
        }

        seen.add(href);
        results.push({
            url: href,
            title: (title || href).slice(0, 200),
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


def score_tiktok_post(post: dict) -> int:
    views = post.get("views")
    if isinstance(views, int) and views >= 0:
        return views
    likes = post.get("likes")
    return likes if isinstance(likes, int) and likes > 0 else 0


if __name__ == "__main__":
    sys.exit(
        run_spy_worker(
            "tiktok",
            build_tiktok_search_url,
            EXTRACT_TIKTOK_JS,
            score_post=score_tiktok_post,
        )
    )
