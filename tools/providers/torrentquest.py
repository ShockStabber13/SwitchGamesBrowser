"""tools/providers/torrentquest.py"""

import re
import time
import urllib.parse
import urllib.request
from bs4 import BeautifulSoup

MAGNET_HASH_RE = re.compile(r"xt=urn:btih:([0-9a-fA-F]{40}|[A-Za-z2-7]{32})", re.IGNORECASE)

MAX_PAGES = 10

HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/114.0.0.0 Safari/537.36",
    "Accept-Language": "en-US,en;q=0.9",
}


class TorrentQuestProvider:
    name = "TorrentQuest"

    def _fetch(self, url):
        req = urllib.request.Request(url, headers=HEADERS)
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.read().decode("utf-8", errors="replace")

    def _build_url(self, query, page):
        trimmed = query.strip()
        first_char = trimmed[0].lower() if trimmed else ""
        q_lower = urllib.parse.quote(query.lower())
        return f"https://torrentquest.com/{first_char}/{q_lower}/{page}/"

    def _fetch_page(self, query, page):
        url = self._build_url(query, page)
        html = self._fetch(url)
        soup = BeautifulSoup(html, "html.parser")

        rows = soup.select("table.download tbody tr")
        results = []

        for tr in rows:
            link_a = tr.select_one("td.m a[href]")
            if not link_a:
                continue
            magnet = link_a["href"].strip()

            title_a = tr.select_one("td.n a")
            if not title_a:
                continue
            title = title_a.get_text(strip=True)
            if not title:
                continue

            match = MAGNET_HASH_RE.search(magnet)
            info_hash = match.group(1).lower() if match else ""
            if not info_hash:
                continue

            results.append({
                "title": title,
                "magnet": magnet,
                "infoHash": info_hash,
                "source": self.name,
            })

        return results

    def search(self, query):
        all_results = []
        page = 1
        empty_page_count = 0

        while page <= MAX_PAGES:
            try:
                results = self._fetch_page(query, page)
            except Exception:
                break

            if not results:
                empty_page_count += 1
                if empty_page_count >= 3:
                    break
                time.sleep(0.5)
            else:
                empty_page_count = 0
                all_results.extend(results)

            page += 1

        return all_results

PROVIDER = TorrentQuestProvider()