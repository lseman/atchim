import os,sqlite3,subprocess,tempfile,time,statistics,json,sys
from pathlib import Path
with tempfile.TemporaryDirectory() as tmp:
 db=tmp+'/history.db';env=dict(os.environ,ATCHIM_DB=db,ATCHIM_DAEMON='0',ATCHIM_RECORD='1',ATCHIM_IGNORE='')
 candidate=str(Path('build/atchim').resolve()); baseline=sys.argv[1] if len(sys.argv)>1 else '/tmp/atchim-before-cpp23'
 subprocess.run([candidate,'log','--','seed'],env=env,check=True)
 with sqlite3.connect(db) as conn:
  conn.executemany('INSERT INTO HISTORY(COMMAND,HOST) VALUES(?,?)',(('git status '+str(i),'local') for i in range(100000)))
  conn.execute("INSERT INTO HISTORY(COMMAND,HOST) VALUES('rare target', 'local')")
  conn.executemany('INSERT INTO HISTORY(COMMAND,HOST) VALUES(?,?)',(('echo '+str(i),'local') for i in range(100000)))
 samples={}
 for label,args in [('suggest-missing',['suggest','missing-prefix']),('suggest-rare',['suggest','rare ']),('history-5000',['history','-n','5000']),('log',['log','--','benchmark'])]:
  values={baseline:[],candidate:[]}
  for i in range(23):
   for binary in ([baseline,candidate] if i%2==0 else [candidate,baseline]):
    start=time.perf_counter();subprocess.run([binary,*args],env=env,check=True,stdout=subprocess.DEVNULL);elapsed=(time.perf_counter()-start)*1000
    if i>=3:values[binary].append(elapsed)
  samples[label]={('before' if k==baseline else 'after'):{'median_ms':round(statistics.median(v),3),'min_ms':round(min(v),3),'max_ms':round(max(v),3)} for k,v in values.items()}
 print(json.dumps(samples,indent=2))
