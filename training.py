"""Generate a deterministic workload for compiler instrumentation during the image build."""
import csv
import json
import random
import sys
from pathlib import Path

root = Path(sys.argv[1])
randomizer = random.Random(8873)
for folder in ('events', 'clicks', 'links'):
    (root / folder).mkdir(parents=True)
(root / 'manifest.json').write_text(json.dumps(dict(window_start='2027-03-01T00:00:00Z', window_end='2027-04-01T00:00:00Z')))

def write(name, header, rows):
    with (root / name).open('w') as stream:
        print(header,file=stream)
        for row in rows:
            print(','.join('"'+str(v)+'"' if randomizer.randrange(5)==0 else str(v) for v in row), file=stream)

def user():
    return f'u{randomizer.randrange(18000):08x}'

def event_rows():
    for i in range(120000):
        ident = f'e{i:08x}'
        person = user()
        day = randomizer.randrange(14)+1
        hour = randomizer.randrange(23)
        clock = f'2027-03-{day:02d}T{hour:02d}:07:13'
        ingest = clock+'Z'
        time = randomizer.choice([str((1803859200+(day-1)*86400+hour*3600+433)*1000),clock+'Z',clock+'+03:00'])
        row = [ident,person,time,ingest,randomizer.choice(['view','view','view','cart','purchase']),f'p{i%129}',i%60000+1]
        yield row
        if i % 13 == 0:
            yield row
        if i % 97 == 0:
            yield ['bad',person,'bad time',ingest,'bad','p1','']

write('events/part.csv','event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents',event_rows())
write('links/part.csv','user_id_a,user_id_b',((user(),user()) for _ in range(2200)))
write('products.csv','product_id,category',((f'p{i}',f'cat{i%11}') for i in range(129)))
write('campaigns.csv','campaign_id,channel',((f'c{i}','search') for i in range(83)))
write('clicks/part.csv','click_id,user_id,campaign_id,ts',((f'k{i:08x}',user(),f'c{i%83}',f'2027-03-{1+i%13:02d}T09:12:21Z') for i in range(28000)))
