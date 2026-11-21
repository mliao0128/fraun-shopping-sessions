"""Generate a deterministic workload for compiler instrumentation during the image build."""
from datetime import datetime, timedelta, timezone
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
            print(','.join('"'+str(v)+'"' if randomizer.randrange(9)==0 else str(v) for v in row), file=stream)

def user():
    return randomizer.choice(users)

# Exercise the runtime decoder across numeric, UTC and offset forms. These
# rows exist only in the compiler instrumentation pass.
start = datetime(2027, 3, 1, tzinfo=timezone.utc)
epoch_ms = int(start.timestamp()) * 1000
alphabet="0123456789abcdefghijklmnopqrstuvwxyz"
def suffix():
    value=randomizer.getrandbits(40)
    chars=[]
    while value:
        value,digit=divmod(value,36)
        chars.append(alphabet[digit])
    return "".join(reversed(chars)) or "0"
users=["u"+suffix() for _ in range(52000)]
active=users[:6500]
def choose_user():
    return randomizer.choice(active if randomizer.random()<0.50 else users)


def stamp(second, form):
    if form == 0:
        return str(epoch_ms + second * 1000 + randomizer.randrange(1000))
    if form == 1:
        return (start + timedelta(seconds=second)).strftime('%Y-%m-%dT%H:%M:%SZ')
    offset = randomizer.choice([-540,-315,120,330,570])
    local = start + timedelta(seconds=second, minutes=offset)
    return local.strftime('%Y-%m-%dT%H:%M:%S') + f"{'+' if offset>=0 else '-'}{abs(offset)//60:02d}:{abs(offset)%60:02d}"

def event_rows():
    for i in range(650000):
        second = randomizer.randrange(14*86400)
        event_type = randomizer.choice(['view']*13+['cart']*4+['purchase'])
        arrival = stamp(second + randomizer.randrange(0,1200) + (86400 if i%71==0 else 0),1)
        row = ['e'+suffix(), choose_user(),
               stamp(second,randomizer.choice([0,0,0,1,1,1,1,1,2])), arrival,event_type,
               f'p{randomizer.randrange(30000)}',randomizer.randrange(1,100000) if event_type=='purchase' or i%5==0 else '']
        yield row
        if i%23==0:
            yield row
        if i%97==0:
            yield ['bad',row[1],'bad time',arrival,'bad','p1','']

write('events/part.csv','event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents',event_rows())
write('links/part.csv','user_id_a,user_id_b',((user(),user()) for _ in range(4500)))
write('products.csv','product_id,category',((f'p{i}',f'category_{i%27}') for i in range(30000)))
write('campaigns.csv','campaign_id,channel',((f'c_{i}','search') for i in range(1000)))
write('clicks/part.csv','click_id,user_id,campaign_id,ts',(('k'+suffix(),choose_user(),f'c_{i%1000}',stamp(randomizer.randrange(14*86400),randomizer.randrange(3))) for i in range(162500)))
