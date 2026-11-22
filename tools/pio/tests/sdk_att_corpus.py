#!/usr/bin/env python3
import pathlib,re,subprocess,json,hashlib,time,os
T=time.monotonic()
OUT=pathlib.Path(os.environ['LITCH_PROOF_OUTPUT'])
REPO=pathlib.Path(os.environ['LITCH_PROOF_REPO'])
ROOT=pathlib.Path(os.environ['LITCH_PROOF_WITNESS'])
SDK=pathlib.Path(os.environ['LITCH_PROOF_SDK'])
import runpy
middleware=runpy.run_path(os.environ['LITCH_PROOF_MIDDLEWARE'])
def extract(text,name):
 m=re.search(r'^(?:static )?(?:int|void|bool)\n'+re.escape(name)+r'\(',text,re.M)
 assert m,name
 opening=text.index('{',m.end()); depth=0
 # Selected SDK bodies have no braces inside string/comment literals.
 for p in range(opening,len(text)):
  if text[p]=='{':depth+=1
  elif text[p]=='}':
   depth-=1
   if depth==0:return text[m.start():p+1]+'\n'
 raise AssertionError(name)
pins={}
observed={}
for file in ['ble_att.c','ble_att_svr.c']:
 orig=(SDK/'src'/file).read_bytes()
 transformed=middleware['ObserveAttSource'](file,orig).decode()
 observed[file]=transformed
 pins[str(SDK/'src'/file)]=hashlib.sha256(orig).hexdigest()
 pins['observed:'+file]=hashlib.sha256(transformed.encode()).hexdigest()
for path in [pathlib.Path(os.environ['LITCH_PROOF_MIDDLEWARE']), ROOT/'include/modules/sdk_att_witness.hpp', ROOT/'src/sdk_att_witness.cpp', SDK/'include/host/ble_hs.h', SDK/'include/host/ble_att.h']:
 pins[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
constants='\n'.join(line for name in ['ble_hs.h','ble_att.h'] for line in (SDK/'include/host'/name).read_text().splitlines() if re.match(r'#define BLE_(?:HS|ATT)_\w+\s+(?:0x[0-9a-fA-F]+|[0-9]+)\s*$',line))
base=(pathlib.Path(os.environ['LITCH_PROOF_HARNESS'])).read_text()
for mode in ['original','transformed']:
 svr=(SDK/'src/ble_att_svr.c').read_text() if mode=='original' else observed['ble_att_svr.c']
 att=(SDK/'src/ble_att.c').read_text() if mode=='original' else observed['ble_att.c']
 functions='\n'.join(extract(svr,n) for n in ['ble_att_svr_pullup_req_base','ble_att_svr_pkt','ble_att_svr_write','ble_att_svr_write_handle','ble_att_svr_tx_error_rsp','ble_att_svr_tx_rsp','ble_att_svr_build_write_rsp','ble_att_svr_rx_write'])
 functions+='\n'+'\n'.join(extract(att,n) for n in ['ble_att_send_outstanding_after_response','ble_att_is_response_op','ble_att_rx_extended'])
 source=constants+'\n'+base.replace('/* EXTRACTED SDK FUNCTIONS */',functions)
 (OUT/f'{mode}.c').write_text(source)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-g','-O1','-fsanitize=address,undefined',f'-DTRANSFORMED={int(mode=="transformed")}','-I'+str(ROOT/'include'),'-c',str(OUT/f'{mode}.c'),'-o',str(OUT/f'{mode}.o')],check=True)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(ROOT/'src/sdk_att_witness.cpp'),str(OUT/f'{mode}.o'),'-o',str(OUT/mode)],check=True)
results={}
for case in ['success','consume','allocation_null','access_error','normal_tx_error','error_reply_tx_error','fallback_allocation_null','housekeeping','prepare_null']:
 results[case]={}
 for mode in ['original','transformed']:
  p=subprocess.run([str(OUT/mode),case],check=True,text=True,capture_output=True)
  (OUT/f'{mode}-{case}.txt').write_text(p.stdout)
  results[case][mode]=p.stdout
  if mode=='original': effects=p.stdout.split('WITNESS')[0]
  else: assert p.stdout.split('WITNESS')[0]==effects,(case,'SDK behavioral drift')

elapsed=time.monotonic()-T
(OUT/'pins.json').write_text(json.dumps(pins,indent=2)+'\n')
(OUT/'results.json').write_text(json.dumps({'elapsedSeconds':elapsed,'results':results},indent=2)+'\n')
print(f'PASS {len(results)} scenarios x original/transformed; effects equal; ASan+UBSan; {elapsed:.3f}s')
for case in results:print(case,results[case]['transformed'].split('WITNESS')[-1].strip())
