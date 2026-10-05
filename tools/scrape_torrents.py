#!/usr/bin/env python3
"""Collect Switch-marked releases from the enabled MoviesAndSeries sources."""
import argparse
import datetime as dt
import json
from pathlib import Path
import re
import time
from build_index import info_hash, write_atomic, read_json, text
from movie_sources import SOURCES, fetch_page

def convert(results, source='ThePirateBay'):
    if not isinstance(results,list): raise ValueError('Provider did not return an array')
    rows,seen=[],set()
    for item in results:
        if not isinstance(item,dict): continue
        title=item.get('title',item.get('name',''))
        if not isinstance(title,str) or not re.search(r'\b(?:switch|NSP|NSZ|XCI|XCZ)\b',title,re.I): continue
        magnet=item.get('magnet')
        if magnet is None: magnet='magnet:?xt=urn:btih:'+str(item.get('info_hash',''))
        digest=info_hash(magnet)
        if not digest or digest=='0'*40 or digest in seen: continue
        seen.add(digest)
        rows.append({'title':text(title,1024),'magnet':magnet,'size':text(str(item.get('size','')),100),'source':source,'sources':[source]})
    return rows

def merge(rows):
    unique={}
    for row in rows:
        digest=info_hash(row['magnet'])
        if digest not in unique: unique[digest]=dict(row)
        else: unique[digest]['sources']=sorted(set(unique[digest]['sources']+row['sources']))
    return list(unique.values())

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--query',action='append')
    p.add_argument('--config',default='data/scraper-config.json')
    p.add_argument('--output',default='data/provider-releases.json')
    args=p.parse_args()
    config=read_json(args.config) if Path(args.config).exists() else {}
    queries=args.query or config.get('queries',['Nintendo Switch'])
    sources=config.get('sources',list(SOURCES)); pages=int(config.get('maxPagesPerQuery',3))
    if not sources or any(s not in SOURCES for s in sources): raise ValueError('Invalid source selection')
    if not 1<=pages<=10 or not 1<=len(queries)<=100: raise ValueError('Scraper configuration exceeds limits')
    old=read_json(args.output) if Path(args.output).exists() else []
    collected,report=[],{}
    for source in sources:
        rows=[]; failed=False
        for query in queries:
            for page in range(1,2 if source=='ThePirateBay' else pages+1):
                try:
                    hits=fetch_page(source,query,page)
                    rows.extend(convert(hits,source))
                    if not hits: break
                except Exception:
                    failed=True; break
                finally: time.sleep(0.5)
            if failed: break
        previous=[r for r in old if source in r.get('sources',[r.get('source')])]
        used_previous=bool(previous) and (failed or not rows)
        fresh=bool(rows)
        if failed or not rows:
            for row in previous:
                kept=dict(row);kept['sources']=[source];kept['source']=source;rows.append(kept)
        collected.extend(rows)
        report[source]={'status':'failed' if failed else 'ok' if fresh else 'no_matches','rows':len(rows),'usedPrevious':used_previous}
    print(json.dumps(report,indent=2))
    if all(r['status']=='failed' for r in report.values()): raise SystemExit('All providers failed; previous files preserved')
    rows=merge(collected)
    if not rows: raise SystemExit('No valid Switch results; previous files preserved')
    write_atomic(args.output,json.dumps(rows,ensure_ascii=False).encode())
    write_atomic(Path(args.output).with_name('scraper-status.json'),json.dumps({'generatedAt':dt.datetime.now(dt.timezone.utc).isoformat(),'providers':report,'uniqueReleases':len(rows)},indent=2).encode())

if __name__=='__main__': main()
