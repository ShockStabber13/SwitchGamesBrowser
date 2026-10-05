import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from movie_sources import parse_bitsearch,parse_knaben,parse_lime_search,fetch_page
from scrape_torrents import merge
MAGNET='magnet:?xt=urn:btih:'+'1'*40
class HtmlProviderTests(unittest.TestCase):
    def test_bitsearch(self):
        html=f'<div class="bg-white rounded-lg"><h3><a href="/x">Demo &amp; Quest [NSP]</a></h3><a href="{MAGNET}">Magnet</a></div>'
        self.assertEqual(parse_bitsearch(html)[0]['title'],'Demo & Quest [NSP]')
    def test_knaben(self):
        html=f'<table class="table table-darker table-striped caption-top"><tr class="text-nowrap border-start"><td class="text-wrap w-100"><a href="{MAGNET}">Demo [NSZ]</a></td></tr></table>'
        self.assertEqual(parse_knaben(html)[0]['magnet'],MAGNET)
    def test_lime_two_requests(self):
        base='https://www.limetorrents.fun/search/all/Demo//1/'
        html='<table class="table2"><tr><td class="tdleft"><div class="tt-name"><a href="/category">Games</a><a href="/demo.html">Demo [XCI]</a></div></td></tr></table>'
        hits=parse_lime_search(html,base);self.assertEqual(hits[0][1],'https://www.limetorrents.fun/demo.html')
        def fetch(url):return f'<a href="{MAGNET}">Magnet</a>' if url.endswith('demo.html') else html
        self.assertEqual(fetch_page('LimeTorrents','Demo',1,fetch)[0]['magnet'],MAGNET)
    def test_lime_external_link_rejected(self):
        html='<table class="table2"><tr><td class="tdleft"><div class="tt-name"><a href="/cat">Cat</a><a href="https://other.invalid/demo">Demo</a></div></td></tr></table>'
        self.assertEqual(parse_lime_search(html,'https://www.limetorrents.fun/'),[])
    def test_duplicate_sources_preserved(self):
        rows=[{'magnet':MAGNET,'sources':[s],'source':s} for s in ['Knaben','Bitsearch']]
        merged=merge(rows);self.assertEqual(len(merged),1);self.assertEqual(merged[0]['sources'],['Bitsearch','Knaben'])
