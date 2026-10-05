"""tools/providers/limetorrents.py"""

import re
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from bs4 import BeautifulSoup

MAGNET_HASH_RE = re.compile(r"xt=urn:btih:([0-9a-fA-F]{40}|[A-Za-z2-7]{32})", re.IGNORECASE)

MAX_PAGES = 10

HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/114.0.0.0 Safari/537.36",
    "Accept-Language": "en-US,en;q=0.9",
}


class LimeTorrentsProvider:
    name = "LimeTorrents"

    def _fetch(self, url):
        req = urllib.request.Request(url, headers=HEADERS)
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.read().decode("utf-8", errors="replace")

    def _fetch_search_hits(self, query, page):
        q = urllib.parse.quote(query)
        url = f"https://www.limetorrents.fun/search/all/{q}//{page}/"
        html = self._fetch(url)
        soup = BeautifulSoup(html, "html.parser")

        rows = soup.select("table.table2 tr")
        hits = []

        for tr in rows:
            container = tr.select_one("td.tdleft div.tt-name")
            if not container:
                continue

            links = container.find_all("a", href=True)
            if len(links) < 2:
                continue

            a2 = links[1]
            title = a2.get_text(strip=True)
            if not title:
                continue

            details_url = a2["href"]
            if not details_url.startswith("http"):
                details_url = "https://www.limetorrents.fun" + details_url

            hits.append((title, details_url))

        return hits

    def _fetch_magnet(self, details_url):
        try:
            html = self._fetch(details_url)
        except Exception:
            return None

        soup = BeautifulSoup(html, "html.parser")
        for a in soup.find_all("a", href=True):
            href = a["href"].strip()
            if href.lower().startswith("magnet:"):
                return href
        return None

    def search(self, query):
        all_results = []
        page = 1
        empty_page_count = 0

        while page <= MAX_PAGES:
            try:
                hits = self._fetch_search_hits(query, page)
            except Exception:
                break

            if not hits:
                empty_page_count += 1
                if empty_page_count >= 3:
                    break
                time.sleep(0.5)
            else:
                empty_page_count = 0

                with ThreadPoolExecutor(max_workers=8) as pool:
                    futures = {pool.submit(self._fetch_magnet, url): title for title, url in hits}
                    for future in as_completed(futures):
                        title = futures[future]
                        magnet = future.result()
                        if not magnet:
                            continue
                        match = MAGNET_HASH_RE.search(magnet)
                        info_hash = match.group(1).lower() if match else ""
                        if not info_hash:
                            continue

                        all_results.append({
                            "title": title,
                            "magnet": magnet,
                            "infoHash": info_hash,
                            "source": self.name,
                        })

            page += 1

        return all_results
 

PROVIDER = LimeTorrentsProvider()