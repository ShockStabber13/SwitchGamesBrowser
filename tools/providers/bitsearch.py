"""tools/providers/bitsearch.py"""

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


class BitSearchProvider:
    name = "Bitsearch"

    def _fetch(self, url):
        req = urllib.request.Request(url, headers=HEADERS)
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.read().decode("utf-8", errors="replace")

    def _fetch_page(self, query, page):
        q = urllib.parse.quote(query)
        url = f"https://bitsearch.to/search?q={q}&page={page}"
        html = self._fetch(url)
        soup = BeautifulSoup(html, "html.parser")

        card_selector = 'div[class*="bg-white"][class*="rounded-lg"][class*="shadow-sm"]'
        cards = soup.select(card_selector)

        results = []
        for card in cards:
            title_a = card.select_one('h3[class*="text-base"] a')
            if not title_a:
                continue
            title = title_a.get_text(strip=True)
            if not title:
                continue

            magnet = None
            container = card.select_one('div[class*="sm:hidden"][class*="mt-4"]')
            if container:
                for a in container.find_all("a", href=True):
                    if a["href"].startswith("magnet:"):
                        magnet = a["href"]
                        break

            # Fallback: search entire card for a magnet link
            if not magnet:
                for a in card.find_all("a", href=True):
                    if a["href"].startswith("magnet:"):
                        magnet = a["href"]
                        break

            if not magnet:
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


PROVIDER = BitSearchProvider()