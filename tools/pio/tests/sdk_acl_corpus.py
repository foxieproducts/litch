#!/usr/bin/env python3
from pathlib import Path
import re,runpy,subprocess,hashlib,json,time,os
T=time.monotonic();D=Path(os.environ['LITCH_ACL_PROOF_OUTPUT'])
P=Path(os.environ['LITCH_ACL_PROOF_REPO']);O=Path(os.environ['LITCH_ACL_PROOF_WITNESS'])
SDK=Path(os.environ['LITCH_ACL_PROOF_SDK'])
MEM=SDK/'src/mem.c'
m=runpy.run_path(os.environ['LITCH_ACL_PROOF_MIDDLEWARE'])
def extract(source,name):
 match=re.search(r'^(?:static )?(?:int|void|bool|struct os_mbuf \*)\n'+re.escape(name)+r'\(',source,re.M);assert match,name
 opening=source.index('{',match.end());depth=0
 for p in range(opening,len(source)):
  if source[p]=='{':depth+=1
  elif source[p]=='}':
   depth-=1
   if depth==0:return source[match.start():p+1]+'\n'
 raise AssertionError(name)
original=(SDK/'src/ble_hs_hci.c').read_bytes();observed=m['ObserveAttSource']('ble_hs_hci.c',original).decode()
constants='\n'.join(line for line in (SDK/'include/host/ble_hs.h').read_text().splitlines() if re.match(r'#define BLE_HS_\w+\s+(?:0x[0-9a-fA-F]+|[0-9]+)\s*$',line))
base=Path(os.environ['LITCH_ACL_PROOF_HARNESS']).read_text();pins={}
for path in [SDK/'src/ble_hs_hci.c',SDK/'src/ble_l2cap.c',SDK/'src/ble_hs.c',SDK/'include/host/ble_hs.h',MEM,O/'src/sdk_att_witness.cpp',O/'include/modules/sdk_att_witness.hpp',O/'tools/pio/harden_nimble_repeat_pairing.py']:
 pins[str(path)]=hashlib.sha256(path.read_bytes()).hexdigest()
for mode in ['original','transformed']:
 hci=original.decode() if mode=='original' else observed
 funcs='\n'.join(extract(hci,n) for n in ['ble_hs_hci_set_buf_sz','ble_hs_hci_add_avail_pkts','ble_hs_hci_frag_alloc','ble_hs_hci_frag_num_mbufs_free','ble_hs_hci_acl_hdr_prepend','ble_hs_hci_acl_tx_now','ble_hs_hci_acl_tx'])
 for path,n in [(MEM,'mem_split_frag'),(SDK/'src/ble_hs.c','ble_hs_tx_data'),(SDK/'src/ble_l2cap.c','ble_l2cap_tx')]:funcs+='\n'+extract(path.read_text(),n)
 source=constants+'\n'+base.replace('/* EXTRACTED SDK FUNCTIONS */',funcs);(D/f'{mode}.c').write_text(source)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-g','-O1','-fsanitize=address,undefined',f'-DTRANSFORMED={int(mode=="transformed")}','-I'+str(O/'include'),'-c',str(D/f'{mode}.c'),'-o',str(D/f'{mode}.o')],check=True)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-I'+str(O/'include'),str(O/'src/sdk_att_witness.cpp'),str(D/f'{mode}.o'),'-o',str(D/mode)],check=True)
results={}
for case in ['success','zero_credit','queue_blocked','fragment_null','header_null','partial_credit_exhausted','transport_error','full_handoff','credit_add','disconnect_credit_add','credit_overflow','null_now','null_queue']:
 results[case]={}
 for mode in ['original','transformed']:
  r=subprocess.run([str(D/mode),case],text=True,capture_output=True)
  (D/f'{mode}-{case}.txt').write_text(r.stdout+r.stderr)
  assert r.returncode==0,(case,mode,r.stdout,r.stderr)
  results[case][mode]=r.stdout
 assert results[case]['original'].split('WITNESS')[0]==results[case]['transformed'].split('WITNESS')[0],(case,'control behavior changed')
(D/'pins.json').write_text(json.dumps(pins,indent=2)+'\n')
(D/'results.json').write_text(json.dumps({'elapsedSeconds':time.monotonic()-T,'results':results},indent=2)+'\n')
print(f'PASS {len(results)} actual SDK C scenarios original/transformed ASan/UBSan effects identical; {time.monotonic()-T:.3f}s')
for case in results:print(case,results[case]['transformed'].split('WITNESS')[-1].strip())
