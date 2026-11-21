#!/usr/bin/env python3
"""Scrolls an Instagram explore/keyword/profile page and saves candidate posts + images.

Usage:
    spy_scroll_instagram.py <query_or_url> <download_dir> [max_posts] [auto|manual]
"""

import sys
import urllib.parse

from spy_scroll_common import run_spy_worker


def build_instagram_search_url(query: str) -> str:
    cleaned = query.strip()
    if not cleaned:
        return "https://www.instagram.com/explore/"
    return (
        "https://www.instagram.com/explore/search/keyword/?q="
        + urllib.parse.quote(cleaned)
    )


EXTRACT_INSTAGRAM_JS = r"""
() => {
    const results = [];
    const seen = new Set();
    const anchors = Array.from(document.querySelectorAll('a[href*="/p/"], a[href*="/reel/"]'));
    for (const a of anchors) {
        let href = a.href || a.getAttribute('href') || '';
        const m = href.match(/\/(p|reel)\/([0-9A-Za-z_-]+)/);
        if (!m) continue;
        const canonical = 'https://www.instagram.com/' + m[1] + '/' + m[2] + '/';
        if (seen.has(canonical)) continue;

        const img = a.querySelector('img');
        const mediaUrl = img ? (img.currentSrc || img.src || img.getAttribute('src') || '') : '';
        let title = (img ? (img.getAttribute('alt') || '') : '').trim();
        if (!title) {
            title = 'Instagram ' + m[1] + ' ' + m[2];
        }

        const card = a.closest('article, div') || a;
        const isAd = (
            a.getAttribute('data-ad') === 'true'
            || card.getAttribute('data-ad') === 'true'
            || /\b(Sponsored|Sponsorisé)\b/i.test(card.innerText || '')
        );

        const timeEl = card.querySelector('time[datetime]');
        const publishedAt = (
            a.getAttribute('data-created')
            || card.getAttribute('data-created')
            || (timeEl ? timeEl.getAttribute('datetime') : '')
            || ''
        );

        seen.add(canonical);
        results.push({
            url: canonical,
            title: title.slice(0, 200),
            media_url: mediaUrl,
            author: '',
            published_at: publishedAt,
            views: null,
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
            "instagram",
            build_instagram_search_url,
            EXTRACT_INSTAGRAM_JS,
        )
    )
