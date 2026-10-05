"""tools/providers/the_pirate_bay.py"""

import json
import re
import time
import urllib.parse
import urllib.request

MAGNET_HASH_RE = re.compile(r"xt=urn:btih:([0-9a-fA-F]{40}|[A-Za-z2-7]{32})", re.IGNORECASE)

MAX_PAGES = 10


class ThePirateBayProvider:
    name = "ThePirateBay"

    def _fetch_page(self, query, page):
        url = "https://apibay.org/q.php?" + urllib.parse.urlencode({"q": query})
        if page > 1:
            url += "&limit=50&offset=" + str((page - 1) * 50)

        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "Mozilla/5.0",
                "Accept": "application/json",
            },
        )

        with urllib.request.urlopen(req, timeout=30) as response:
            body = response.read().decode("utf-8", errors="replace")

        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            return []

        if not isinstance(data, list):
            return []

        results = []
        for obj in data:
            title = obj.get("name")
            info_hash = obj.get("info_hash")
            if not title or not info_hash:
                continue

            info_hash = info_hash.lower()
            magnet = f"magnet:?xt=urn:btih:{info_hash}&dn={urllib.parse.quote(title)}"

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

PROVIDER = ThePirateBayProvider()