"""Fresh-process 60M release benchmark. The only inputs are generated public data."""
from pathlib import Path
import hashlib,json,os,random,shutil,statistics,subprocess,sys,tempfile,time
from check_output import connection,digest_table,SCHEMAS

a=Path(__file__).resolve().parent;tmp=Path(os.environ.get('RUNNER_TEMP','/tmp'))
seed=int(sys.argv[1]);plan=json.loads((a/'manifest.json').read_text());expected=plan['seeds'][str(seed)]
binset=tmp/'shopping-bin';exe=binset/'generate'
def sha(p):
 with p.open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
for arm, expected_hash in plan['releases'].items():
 binary=a/'bin'/arm
 assert sha(binary)==expected_hash,arm
 shutil.copy(binary,binset/arm);(binset/arm).chmod(0o755)
print('Generating fresh public dataset',seed,flush=True)
with tempfile.TemporaryDirectory(prefix='shopping-public-',dir=tmp) as t:
 work=Path(t);ds=work/'input';ds.mkdir()
 subprocess.run([exe,'--seed',str(seed),'--events','60000000','--clicks','15000000','--threads','2','--output',ds],check=True)
 digest=hashlib.sha256()
 for p in sorted(ds.rglob('*'),key=lambda x:x.relative_to(ds).as_posix()):
  if p.is_file():digest.update((p.relative_to(ds).as_posix()+'\0'+sha(p)+'\n').encode())
 assert digest.hexdigest()==expected['input_sha256'],'generator input differs'
 names=list(plan['releases']);rng=random.Random(seed);rng.shuffle(names)
 # Standard runners can lack AVX-512. Run the old release only where its ISA is available.
 flags=Path('/proc/cpuinfo').read_text().split('flags',1)[1].splitlines()[0].split()
 if not all(f in flags for f in ('avx512f','avx512bw','avx512vl','avx512dq','avx512cd','avx512_vnni')):
  portable=a/'bin/82fced9-avx2'
  assert sha(portable)=='d52298f34fc72a6f508d0385d262488dff72cddaa3f3639e3557f8e31c1445ac'
  shutil.copy(portable,binset/'82fced9');(binset/'82fced9').chmod(0o755)
  print('82fced9 uses its matching-source GCC15 x86-64-v3 Haswell+AES profile build on this CPU',flush=True)
 count=len(names);order=list(range(count));rng.shuffle(order)
 shifts=[(k+1)//2 if k%2 else (-k//2)%count for k in range(count)]
 rounds=[('warm',names)]+[(i,[names[(r+j)%count] for j in shifts])for i,r in enumerate(order)]
 all_samples=[]
 cpu=max(1,min(8,len(os.sched_getaffinity(0))))
 print('SETUP '+json.dumps({'seed':seed,'cpus':sorted(os.sched_getaffinity(0)),'workers':cpu,'executables':{n:sha(binset/n)for n in names}}),flush=True)
 env=os.environ|{'NCPU':str(cpu),'LD_PRELOAD':'/usr/lib/x86_64-linux-gnu/libjemalloc.so.2','MALLOC_CONF':'thp:always,narenas:1'}
 with connection() as db:
  db.execute('set threads=2')
  for block,seq in rounds:
   # Time the complete block before SQL verification. Each measured process
   # gets an unused directory and produces all required result files.
   samples=[]
   for pos,n in enumerate(seq):
    out=work/f'out-{pos}';out.mkdir()
    tick=time.monotonic();proc=subprocess.run([binset/n,ds,out],env=env,capture_output=True)
    elapsed=time.monotonic()-tick
    print("TIMED "+json.dumps(dict(seed=seed,arm=n,block=block,seconds=elapsed)),flush=True)
    if proc.returncode:raise RuntimeError((n,proc.returncode,proc.stderr[-800:]))
    samples.append(dict(block=block,pos=pos,arm=n,seconds=elapsed))
   for r in samples:
    out=work/f"out-{r['pos']}";tables={t:digest_table(db,out,t)for t in SCHEMAS};assert tables==expected['tables'],(r,tables)
    print('CHECKED '+json.dumps(r|{'seed':seed,'tables':tables}),flush=True)
    if block!='warm':all_samples.append(r)
    shutil.rmtree(out)


 print('SUMMARY '+json.dumps({n:statistics.median(r['seconds'] for r in all_samples if r['arm']==n) for n in names}),flush=True)
