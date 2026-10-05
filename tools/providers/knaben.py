"""tools/providers/knaben.py"""

import re
import time
import urllib.parse
import urllib.request
from bs4 import BeautifulSoup

MAGNET_HASH_RE = re.compile(r"xt=urn:btih:([0-9a-fA-F]{40}|[A-Za-z2-7]{32})", re.IGNORECASE)

HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/114.0.0.0 Safari/537.36",
    "Accept-Language": "en-US,en;q=0.9",
}


class KnabenProvider:
    name = "Knaben"

    def _fetch(self, url):
        req = urllib.request.Request(url, headers=HEADERS)
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.read().decode("utf-8", errors="replace")

    def _build_url(self, query, page):
        q = urllib.parse.quote(query)
        return f"https://knaben.org/search/{q}/0/{page}/"

    def _fetch_page(self, query, page):
        url = self._build_url(query, page)
        html = self._fetch(url)
        soup = BeautifulSoup(html, "html.parser")

        row_selector = "table.table.table-darker.table-striped.caption-top tr.text-nowrap.border-start"
        rows = soup.select(row_selector)
        results = []

        for tr in rows:
            a = tr.select_one("td.text-wrap.w-100 a[href]")
            if not a:
                continue
            magnet = a["href"].strip()
            title = a.get_text(strip=True)
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

        while True:
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
PROVIDER = KnabenProvider()