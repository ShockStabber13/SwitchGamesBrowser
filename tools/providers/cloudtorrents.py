"""tools/providers/cloudtorrents.py"""

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


class CloudTorrentsProvider:
    name = "CloudTorrents"

    def _fetch(self, url):
        req = urllib.request.Request(url, headers=HEADERS)
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.read().decode("utf-8", errors="replace")

    def _fetch_page(self, query, offset):
        q = urllib.parse.quote(query)
        url = f"https://cloudtorrents.com/search?offset={offset}&query={q}"
        html = self._fetch(url)
        soup = BeautifulSoup(html, "html.parser")

        elements = soup.select(".torrent-name")
        results = []

        for el in elements:
            magnet = None
            for a in el.find_all("a", href=True):
                if a["href"].startswith("magnet:"):
                    magnet = a["href"]
                    break

            title_div = el.select_one(".torrent-title")
            anchor = title_div.find("a") if title_div else None
            bold = anchor.find("b") if anchor else None
            title = bold.get_text(strip=True) if bold else (anchor.get_text(strip=True) if anchor else "")

            if not magnet or not title:
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
        offset = 0
        page_count = 0
        empty_page_count = 0

        while page_count < MAX_PAGES:
            try:
                results = self._fetch_page(query, offset)
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

            offset += 25
            page_count += 1

        return all_results

PROVIDER = CloudTorrentsProvider()