from pathlib import Path
import hashlib,json,struct,sys
from fixture_data import load_rom,load_vendor
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.arm_const import *
ROOT=Path(sys.argv[1]);root=ROOT
rom=load_rom();vendor=load_vendor();signed=vendor;ds=vendor
with (ROOT/'test-only.elf').open('rb') as f:
    elf=ELFFile(f);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name}
    syms=symbols
    allocated=[s for s in elf.iter_sections() if s['sh_flags']&2]
    sections=[(s['sh_addr'],s.data()) for s in allocated if s['sh_type']!='SHT_NOBITS']
    base=min(s['sh_addr'] for s in allocated)
    blob=bytearray(max(s['sh_addr']+s['sh_size'] for s in allocated)-base)
    for a,b in sections:blob[a-base:a-base+len(b)]=b
def branch(a,b):
 n=(b-a-4)&0x1ffffff;s=n>>24
 return struct.pack('<HH',0xf000|(s<<10)|(n>>12&0x3ff),0x9000|((1^(n>>23&1)^s)<<13)|((1^(n>>22&1)^s)<<11)|(n>>1&0x7ff))
P=0x210000;B=0x210100;PROD=0x210300;EP=0x20645c;MSG=0x21e000;MSG2=0x21e040;CTX=0x211000;SRC=0x211400
META=0x6ea61c;RING=0x6e961c;R=0x1000;STOP=0x88000;NOTIFY=0x88010
class Fixture:
 def __init__(self,packet=60,ready=True,writer=0,reader=0):
  self.u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M3);self.u.mem_map(0,0x100000);self.u.mem_write(0,rom);self.u.mem_write(0xd0200,ds)
  self.u.mem_map(0x200000,0x30000);self.u.mem_map(0x6e8000,0x3000)
  for a,b in sections:self.u.mem_write(a,b)
  self.u.mem_write(0x5fcb4,branch(0x5fcb4,0xd0348))
  assert rom[0x4411c:0x44120].hex()=='90421bd0'
  self.u.mem_write(0x4411c,branch(0x4411c,symbols['dispatch_7074_thunk']&~1))
  self.u.mem_write(0x5fd3c,branch(0x5fd3c,symbols['serializer_7074_thunk']&~1))
  self.reads=0;self.rearms=0;self.wakes=0;self.notifies=0;self.drop_notifies=0;self.lock_depth=0;self.it_boundary=False;self.it_resume=False;self.it_fixes=0;self.frees=[];self.queue=[];self.commit_failures=0;self.copy_short=None;self.trace=[]
  self.packet=packet;self.u.mem_write(RING,bytes([0xa5])*R)
  for a,v in [(0x2038cc,1),(0x2038d0,2),(0x2038d8,0x6861c),(0x2038dc,R),(0x2038e0,R),(META+8,writer),(META+12,reader),
              (P+0x18,PROD),(P+0x1c,EP),(PROD,0x662e1),(EP,0x662f5),(EP+8,NOTIFY|1),(EP+0x14,P),(EP+0x18,0),
              (P+0x3c,0),(P+0x40,packet if ready else 0),(0x201f1c,CTX)]:self.w(a,v)
  self.w16(EP+0x1c,60);self.w16(EP+0x1e,0x35);self.u.mem_write(EP+0x24,b'\x01')
  self.w16(P+0x4c,packet);self.w16(P+0x4e,packet if ready else 0);self.w16(P+0x50,packet)
  self.u.mem_write(0x204482,b'\x3c');self.u.mem_write(0x202e7c,b'\x3c')
  self.u.mem_write(0x2039ae,b'\x03');self.w16(0x202e84,0xfff);self.w16(0x202e86,0x23);self.w16(0x202e88,0xfff)
  self.q(P+8,[B] if ready else []);self.q(P+0x10,[] if ready else [B]);self.w(B+8,0x5000)
  self.u.mem_write(B+0x10,bytes(i&255 for i in range(packet)))
  for msg in (MSG,MSG2):self.w(msg+4,0x7074);self.u.mem_write(msg+9,struct.pack('<HB',0,1)) # stale advisory length1 must not truncate60.
  self.u.hook_add(UC_HOOK_CODE,self.hook)
  self.u.hook_add(UC_HOOK_MEM_READ,self.read_guard);self.u.hook_add(UC_HOOK_MEM_WRITE,self.write_guard)
 def w(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def w16(self,a,v):self.u.mem_write(a,struct.pack('<H',v))
 def r(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
 def r16(self,a):return struct.unpack('<H',self.u.mem_read(a,2))[0]
 def q(self,head,items):
  nodes=[head]+items
  for i,a in enumerate(nodes):self.w(a,nodes[(i+1)%len(nodes)]);self.w(a+4,nodes[(i-1)%len(nodes)])
 def ret(self,result=None):
  if result is not None:self.u.reg_write(UC_ARM_REG_R0,result)
  self.u.reg_write(UC_ARM_REG_PC,self.u.reg_read(UC_ARM_REG_LR))
 def hook(self,u,a,n,user):
  self.trace.append(a)
  if a==STOP:u.emu_stop()
  elif a==0x666aa:
   # TEST-ONLY: guarded ITTE/LDRH/TST/conditionalPOP architectural assist.
   # Unicorn mis-retains ITSTATE on the not-taken conditional POP. No firmware change.
   assert bytes(u.mem_read(a,10)).hex()=='1abfc18b11f0100f10bd'
   self.it_fixes+=1
   callback=u.reg_read(UC_ARM_REG_R1);flags=0
   cpsr=u.reg_read(UC_ARM_REG_CPSR)&~0x0600fc00
   if callback:
    flags=self.r16(u.reg_read(UC_ARM_REG_R0)+0x1e);u.reg_write(UC_ARM_REG_R1,flags)
    cpsr=(cpsr&~0xc0000000)|(0x40000000 if not flags&0x10 else 0)
   u.reg_write(UC_ARM_REG_CPSR,cpsr)
   if not callback or not flags&0x10:
    sp=u.reg_read(UC_ARM_REG_SP);u.reg_write(UC_ARM_REG_R4,self.r(sp));pc=self.r(sp+4)
    u.reg_write(UC_ARM_REG_SP,sp+8);u.reg_write(UC_ARM_REG_PC,pc)
   else:u.reg_write(UC_ARM_REG_PC,0x666b5)
  elif a==0x45ee:self.lock_depth+=1;self.ret(0)
  elif a==0x45f6:self.lock_depth-=1;assert self.lock_depth>=0;self.ret()
  elif a==0x5f940:self.ret(0)
  elif a==0x5f87a:self.wakes+=1;self.ret()
  elif a==NOTIFY:self.notifies+=1;self.ret()
  elif a==0x88020:assert self.lock_depth==0;self.drop_notifies+=1;self.ret()
  elif a==0x669fe:
   self.reads+=1
   if self.copy_short is not None:u.reg_write(UC_ARM_REG_R2,self.copy_short)
  elif a==0x66652:self.rearms+=1
  elif a==(symbols['sco_native_b2h_commit']&~1) and self.commit_failures:
   self.commit_failures-=1;self.ret(1)
  elif a==0x17ada:self.frees.append(u.reg_read(UC_ARM_REG_R0));self.ret()
  elif a==0x17ed4:
   assert u.reg_read(UC_ARM_REG_R0)==CTX+0xd8;self.ret(self.queue.pop(0) if self.queue else 0)
  elif a in (0x5ed6,0x1017e,0x9736):raise AssertionError('Unexpected fatal/log '+hex(a))
 def read_guard(self,u,kind,a,n,value,user):assert not 0x6861c<=a<0x6a62c,'RAW ring address dereferenced '+hex(a)
 def write_guard(self,u,kind,a,n,value,user):
  assert 0x200000<=a and a+n<=0x230000 or 0x6e8000<=a and a+n<=0x6eb000,(hex(a),n)
 def call(self,address,*args):
  for reg,value in zip([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3],args):self.u.reg_write(reg,value)
  self.u.reg_write(UC_ARM_REG_SP,0x21d000);self.u.reg_write(UC_ARM_REG_LR,STOP|1)
  try:self.u.emu_start(address|1,STOP+4,count=30000)
  except Exception:
   print('TRACE',[hex(a) for a in self.trace[-25:]],'PC',hex(self.u.reg_read(UC_ARM_REG_PC)),'CPSR',hex(self.u.reg_read(UC_ARM_REG_CPSR)),'BYTES',bytes(self.u.mem_read(self.u.reg_read(UC_ARM_REG_PC),8)).hex());raise
  assert self.u.reg_read(UC_ARM_REG_PC)==STOP and self.lock_depth==0,hex(self.u.reg_read(UC_ARM_REG_PC))
  return self.u.reg_read(UC_ARM_REG_R0)
 def send(self,msg=MSG):return self.call(symbols['sco_outbound_send'],msg,symbols['sco_outbound_env'])
 def state(self):return dict(readable=self.r16(P+0x4e),cap=self.r16(P+0x50),offset=self.r(P+0x28),current=self.r(P+0x20),consumed=self.r(P+0x3c),ready=self.r(P+8),free=self.r(P+0x10))
 def frame(self,writer,n):
  data=bytes(self.u.mem_read(RING,R));raw=bytes(data[(writer+i)%R] for i in range(4+n))
  assert raw[:4]==bytes([n&255,n>>8,0,3]);return raw[4:]
 def pending(self):return self.r(symbols['out_state'])
 def dispatch(self,msg=MSG):self.queue.append(msg);self.call(0x44104,CTX,1)

results={}
f=Fixture();assert f.send()==0
assert f.frame(0,63)==bytes.fromhex('23103c')+bytes(range(60));assert f.r(META+8)==68
assert f.reads==1 and f.wakes==1 and f.rearms==1 and f.state()['consumed']==60 and f.state()['free']==B
assert bytes(f.u.mem_read(RING+67,1))==b'\xa5' # Native alignment padding is deliberately untouched.
assert f.drop_notifies==0
results['native_read_flags_mapping_ring_gap_and_free_once']=f.state()
f=Fixture(writer=R-4,reader=256);assert f.send()==0;assert f.frame(R-4,63)==bytes.fromhex('23103c')+bytes(range(60));assert f.r(META+8)==64
results['header_end_payload_wrap']=f.r(META+8)
f=Fixture(writer=R-12,reader=256);assert f.send()==0;assert f.frame(R-12,63)==bytes.fromhex('23103c')+bytes(range(60));assert f.r(META+8)==56
results['split_payload_wrap']=f.r(META+8)
f=Fixture(reader=68);before=f.state();assert f.send()==1 and f.state()==before and f.reads==0 and not f.pending() and f.r(META+8)==0
assert f.drop_notifies==0
f.w(META+12,72);assert f.send()==0 and f.reads==1
results['0076_gap_full_preflight_then_retry_without_early_consumption']=f.state()
f=Fixture(packet=120);assert f.send()==0;assert f.state()['offset']==60 and f.state()['free']==P+0x10
assert f.send(MSG2)==0;assert f.state()['readable']==0 and f.state()['consumed']==120 and f.state()['free']==B
free_state=f.state();assert f.send()==2 and f.state()==free_state and f.reads==2
assert f.drop_notifies==1
assert f.frame(0,63)==bytes.fromhex('23103c')+bytes(range(60));assert f.frame(68,63)==bytes.fromhex('23103c')+bytes(range(60,120))
results['partial_read_remainder_duplicate_wakeup_no_double_block_return']=f.state()
f=Fixture(packet=120,ready=False);f.w(P+0x24,B);f.w(P+0x2c,60);f.w16(P+0x4e,60);f.w(P+0x40,60);f.w16(EP+0x1e,0x25)
before=f.state();assert f.send()==1 and f.reads==0 and f.state()==before and f.r16(EP+0x1e)&0x10 and f.notifies==0
f.u.mem_write(SRC,bytes(range(60,120)));assert f.call(0x66bca,P,SRC,60)==60 and f.notifies==1
assert f.send()==0 and f.frame(0,63)==bytes.fromhex('23103c')+bytes(range(60))
results['uncommitted_producer_partial_retained_next_commit_notifies']=f.state()
f=Fixture();f.commit_failures=1;assert f.send()==1 and f.pending()==MSG and f.reads==1 and f.rearms==1
assert f.drop_notifies==0
st=f.state();scratch=bytes(f.u.mem_read(symbols['out_scratch'],63));assert f.send(MSG2)==1 and f.state()==st
assert f.send()==0 and f.reads==1 and f.rearms==1 and not f.pending() and f.frame(0,63)==scratch and f.state()==st
results['unexpected_commit_retry_stages_once_and_busy_does_not_overwrite']=f.state()
f=Fixture();f.commit_failures=1;assert f.send()==1;st=f.state();f.w(symbols['out_epoch'],2);assert f.send()==2 and not f.pending() and f.state()==st and f.wakes==0 and f.drop_notifies==1
results['epoch_teardown_discards_staged_without_native_packet_return']=f.state()
f=Fixture();f.commit_failures=1;assert f.send()==1;st=f.state();f.call(symbols['sco_outbound_cancel'],symbols['sco_outbound_env']);assert not f.pending() and f.state()==st
assert f.drop_notifies==0
results['explicit_cancel_leaves_native_packet_ownership_alone']=f.state()
rejected=[]
for name,edit in [('internal_index3',lambda f:f.u.mem_write(MSG+9,b'\x03\x00')),('mapped_handleFFF',lambda f:f.w16(0x202e86,0xfff)),('inactive',lambda f:f.u.mem_write(EP+0x24,b'\0')),('UART_copy_callback',lambda f:f.w(EP,0x662a7)),('consumer_owner_lost',lambda f:f.w(P+0x1c,0)),('producer_owner_lost',lambda f:f.w(P+0x18,0)),('released_capacity0',lambda f:f.w16(P+0x50,0)),('wrong_external_base',lambda f:f.w(0x2038d8,0x68620)),('corrupt_ring_index',lambda f:f.w(META+8,1)),('mode0',lambda f:f.w(0x2038d0,0))]:
 f=Fixture();edit(f);before=f.state();assert f.send()==2 and f.state()==before and f.reads==0 and f.wakes==0 and f.drop_notifies==1,name;rejected.append(name)
results['stale_or_unsupported_no_sample_mutation']=rejected
f=Fixture();f.copy_short=20;assert f.send()==0 and f.frame(0,23)==bytes.fromhex('231014')+bytes(range(20)) and f.state()['offset']==20
results['actual_native_short_read_still_publishes_only_consumed_bytes']=f.state()
f=Fixture();f.dispatch();assert f.frees==[MSG] and f.reads==1 and f.wakes==1 and not f.r(CTX+0x10c)
assert f.r(symbols['sco_out_transport_context'])==CTX and f.drop_notifies==0
f.dispatch(MSG2);assert f.frees==[MSG,MSG2] and f.reads==1
assert f.drop_notifies==1
results['actual_dispatcher_and4406a_free_each_token_once']=list(map(hex,f.frees))
f=Fixture(reader=68);f.dispatch();assert not f.frees and f.r(CTX+0x10c)==MSG and f.reads==0
f.w(META+12,72);f.call(0x4406a,CTX);assert f.frees==[MSG] and not f.r(CTX+0x10c) and f.reads==1
results['actual4406a_requeues_full_then_frees_once']=list(map(hex,f.frees))
f=Fixture();f.w(symbols['out_reset_blocked'],1);before=f.state();assert f.send()==2 and f.state()==before and f.reads==0 and f.drop_notifies==1
f.w(symbols['out_reset_blocked'],0);assert f.send()==0 and f.reads==1 and f.drop_notifies==1
results['reset_blocked_drops_before_pipe_read_and_unblocked_resumes']=f.state()
f=Fixture();f.commit_failures=1;assert f.send()==1;before=f.state();f.w(symbols['out_reset_blocked'],1);f.w(EP+0x14,0xdead0000)
assert f.send()==2 and not f.pending() and f.state()==before and f.reads==1 and f.drop_notifies==1
results['blocked_pending_cleared_without_invalid_pipe_access']=f.state()
legacy=[]
for kind in (0x7041,0x7043,0x7084):
 f=Fixture();f.w(MSG+4,kind)
 if kind==0x7041:payload=bytes([0x0e,4,1,2,3,4]);f.u.mem_write(MSG+8,payload);wire=4
 elif kind==0x7043:payload=b'abc';f.u.mem_write(MSG+8,b'\x03'+payload);wire=7
 else:payload=bytes([0x23,0,2,0,0xaa,0xbb]);f.u.mem_write(MSG+8,payload);wire=2
 f.dispatch();data=bytes(f.u.mem_read(RING,4+len(payload)))
 assert data==bytes([len(payload),0,0,wire])+payload and f.frees==[MSG] and f.reads==0 and f.drop_notifies==0
 legacy.append(hex(kind))
results['ordinary7041_7043_7084_native_forwarding_unchanged']=legacy
cfg=[]
for kind in (0,0x7040,0x7041,0x7043,0x7074,0x7075,0x7084,0xc001,0xc002,0xc003,0xc004,0xffffffff):
 records=[]
 for patched in (False,True):
  f=Fixture()
  if not patched:f.u.mem_write(0x4411c,rom[0x4411c:0x44120])
  for reg,value in [(UC_ARM_REG_R0,kind),(UC_ARM_REG_R1,(kind-0xc001)&0xffffffff),(UC_ARM_REG_R2,0xc001),(UC_ARM_REG_R4,MSG),(UC_ARM_REG_R5,CTX),(UC_ARM_REG_SP,0x21d000)]:f.u.reg_write(reg,value)
  f.u.reg_write(UC_ARM_REG_APSR,0xa8000000)
  stops={0x44120,0x44158,0x4414e}
  def stop_cfg(u,a,n,user):
   if a in stops:u.emu_stop()
  handle=f.u.hook_add(UC_HOOK_CODE,stop_cfg);f.u.emu_start(0x4411d,0,count=100);f.u.hook_del(handle)
  records.append(([f.u.reg_read(r) for r in [UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_SP]],f.u.reg_read(UC_ARM_REG_PC),f.u.reg_read(UC_ARM_REG_APSR)))
 if kind==0x7074:assert records[1][1]==0x4414e and records[0][1]==0x44120 and records[0][0]==records[1][0]
 else:assert records[0]==records[1],(hex(kind),records)
 cfg.append(hex(kind))
results['aligned4411c_CMP_BEQ_replay_all_class_CFG_and_flags']=cfg
report={'passed':len(results),'rom_fixture_sha256':hashlib.sha256(rom).hexdigest(),'0076_TCA_sha256':hashlib.sha256(ds).hexdigest(),'test_elf_sha256':hashlib.sha256((root/'test-only.elf').read_bytes()).hexdigest(),'cases':results,'scope':'Compiled C/thunks and actual ROM669fe/66716/list/copy7baec/5f924/5fc54 instructions with signed0076capacityoverlay. Remaining66652 callback/rearm instructions execute, but TEST-ONLY guarded10-byte ITTE/LDRH/TST/conditionalPOP block666aa..666b2 has an explicit architectural semantic assist for Unicorn staleITSTATE bug. Synthetic bytes only. IRQ save/restore, power-ready, doorbell, notification/drop callbacks, RTOS dequeue and metadatafree stubbed. No hardware, audio capture or production addresses assigned.'}
(root/'test-results.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
