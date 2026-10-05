"""Ports of the four ENABLED MoviesAndSeries provider adapters.
Standard-library HTML parser; no Android or Jsoup runtime needed.
"""
from html.parser import HTMLParser
import json
import urllib.parse
import urllib.request

SOURCES = ('ThePirateBay', 'Bitsearch', 'LimeTorrents', 'Knaben')

class Node:
    def __init__(self, tag='', attrs=None):
        self.tag, self.attrs, self.children = tag, dict(attrs or []), []
    def find(self, tag=None, classes=()):
        result = []
        for c in self.children:
            if isinstance(c, Node):
                if (tag is None or c.tag == tag) and set(classes).issubset(c.attrs.get('class', '').split()):
                    result.append(c)
                result.extend(c.find(tag, classes))
        return result
    def text(self):
        return ' '.join(''.join(c.text() if isinstance(c, Node) else c for c in self.children).split())

class Document(HTMLParser):
    VOID = {'area','base','br','col','embed','hr','img','input','link','meta','param','source','track','wbr'}
    def __init__(self, html):
        super().__init__(convert_charrefs=True)
        self.root = Node(); self.stack = [self.root]; self.feed(html)
    def handle_starttag(self, tag, attrs):
        node = Node(tag, attrs); self.stack[-1].children.append(node)
        if tag not in self.VOID: self.stack.append(node)
    def handle_startendtag(self, tag, attrs):
        self.stack[-1].children.append(Node(tag, attrs))
    def handle_endtag(self, tag):
        for i in range(len(self.stack)-1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]; break
    def handle_data(self, data):
        self.stack[-1].children.append(data)

def anchors(node):
    return [n for n in node.find('a') if n.attrs.get('href')]

def magnet_in(node):
    return next((n.attrs['href'].strip() for n in anchors(node) if n.attrs['href'].strip().lower().startswith('magnet:')), '')

def parse_bitsearch(html):
    result = []
    for card in Document(html).root.find('div', ('bg-white','rounded-lg')):
        headings = card.find('h3')
        titles = anchors(headings[0]) if headings else []
        magnet = magnet_in(card)
        if titles and magnet: result.append({'title': titles[0].text(), 'magnet': magnet})
    return result

def parse_knaben(html):
    result = []
    for table in Document(html).root.find('table', ('table','table-darker','table-striped','caption-top')):
        for row in table.find('tr', ('text-nowrap','border-start')):
            cells = row.find('td', ('text-wrap','w-100'))
            links = anchors(cells[0]) if cells else []
            if links:
                magnet = magnet_in(cells[0]) or magnet_in(row)
                if magnet: result.append({'title': links[0].text(), 'magnet': magnet})
    return result

def parse_lime_search(html, base):
    hits = []
    for table in Document(html).root.find('table', ('table2',)):
        for row in table.find('tr'):
            cells = row.find('td', ('tdleft',))
            names = cells[0].find('div', ('tt-name',)) if cells else []
            links = anchors(names[0]) if names else []
            if len(links) >= 2 and links[1].text():
                url = urllib.parse.urljoin(base, links[1].attrs['href'])
                if urllib.parse.urlsplit(url).scheme == 'https' and urllib.parse.urlsplit(url).hostname == urllib.parse.urlsplit(base).hostname:
                    hits.append((links[1].text(), url))
    return hits

def request(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != 'https': raise ValueError('HTTPS required')
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0 SwitchGamesBrowser/0.2', 'Accept-Language': 'en-US,en;q=0.9'})
    with urllib.request.urlopen(req, timeout=20) as response:
        if urllib.parse.urlsplit(response.url).scheme != 'https' or urllib.parse.urlsplit(response.url).hostname != parsed.hostname:
            raise ValueError('Provider redirected outside its configured HTTPS host')
        content = response.read(4*1024*1024+1)
        if len(content) > 4*1024*1024: raise ValueError('Provider page exceeds limit')
        return content.decode(response.headers.get_content_charset() or 'utf-8', errors='replace')

def fetch_page(source, query, page, fetch=request):
    q = urllib.parse.quote_plus(query)
    if source == 'ThePirateBay':
        if page > 1: return []
        items = json.loads(fetch('https://apibay.org/q.php?q=' + q))
        if not isinstance(items, list): raise ValueError('Unexpected APIBay response')
        return [{'title': i.get('name',''), 'magnet': 'magnet:?xt=urn:btih:' + str(i.get('info_hash','')) + '&dn=' + urllib.parse.quote(str(i.get('name',''))), 'size': str(i.get('size','')) + ' bytes'} for i in items if isinstance(i,dict)]
    if source == 'Bitsearch':
        return parse_bitsearch(fetch(f'https://bitsearch.to/search?q={q}&page={page}'))
    if source == 'Knaben':
        return parse_knaben(fetch(f'https://knaben.org/search/{q}/0/{page}/'))
    if source == 'LimeTorrents':
        base = f'https://www.limetorrents.fun/search/all/{q}//{page}/'
        result = []
        for title, url in parse_lime_search(fetch(base), base)[:40]:
            magnet = magnet_in(Document(fetch(url)).root)
            if magnet: result.append({'title':title,'magnet':magnet})
        return result
    raise ValueError('Unknown provider')
