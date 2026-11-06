from pathlib import Path
import json,struct,sys
from fixture_data import load_rom,production_adapter
from unicorn import *
from unicorn.arm_const import *
root=Path(sys.argv[1]);m,blob=production_adapter();sym=m['symbols'];rom=load_rom()
STOP=0x88000;SP=0x21f000;CTX=0x211000;MSG=0x212000;ALLOC=0x212100
class F:
 def __init__(self):
  self.u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);self.u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M3)
  self.u.mem_map(0,0x100000);self.u.mem_write(0,rom);self.u.mem_write(m['base'],blob);self.u.mem_map(0x200000,0x30000)
  self.depth=0;self.queued=[];self.alloc=True;self.minimum=SP
  self.u.hook_add(UC_HOOK_CODE,self.hook)
 def w(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def r(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
 def ret(self,v=0):self.u.reg_write(UC_ARM_REG_R0,v);self.u.reg_write(UC_ARM_REG_PC,self.u.reg_read(UC_ARM_REG_LR))
 def hook(self,u,a,n,arg):
  self.minimum=min(self.minimum,u.reg_read(UC_ARM_REG_SP))
  if a==STOP:u.emu_stop()
  elif a==0x45ee:self.depth+=1;self.ret(0)
  elif a==0x45f6:self.depth-=1;assert self.depth>=0;self.ret()
  elif a==0x1797a:assert u.reg_read(UC_ARM_REG_R0)==8;self.ret(ALLOC if self.alloc else 0)
  elif a==0x1495a:self.queued.append(u.reg_read(UC_ARM_REG_R0));self.ret()
 def call(self,n,*args):
  for r,v in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args):self.u.reg_write(r,v)
  self.u.reg_write(UC_ARM_REG_SP,SP);self.u.reg_write(UC_ARM_REG_LR,STOP|1)
  self.u.emu_start(sym[n]|1,STOP+2,count=10000);assert self.u.reg_read(UC_ARM_REG_PC)==STOP and self.depth==0
  return self.u.reg_read(UC_ARM_REG_R0)
state=sym['sco_bridge_state'];out=sym['sco_outbound_state'];ctxsym=sym['sco_out_transport_context'];proof=[]
f=F();f.w(state,4);f.w(out,MSG);f.w(out+16,63);f.call('sco_bridge_reset')
assert f.r(state)==5 and f.r(state+4)==1 and f.r(state+8)==1 and f.r(out)==f.r(out+16)==0 and f.r(out+52)==1
proof.append('Reset blocks new staging, advances epoch and cancels an owned staging copy without freeing metadata')
for packet,expected in [(bytes.fromhex('0e0401030c00'),0),(bytes.fromhex('0e0401030c01'),1),(bytes.fromhex('0e0401011000'),1),(bytes.fromhex('0e0301030c00'),1)]:
 f=F();f.w(state+4,1);f.w(MSG+4,0x7041);f.u.mem_write(MSG+8,packet);f.call('sco_bridge_observe_control',MSG);assert f.r(state+4)==expected
proof.append('Only successful exact HCI Reset CommandComplete clears the blocked state')
for nonempty,already,allocation,expected in [(False,0,True,0),(True,0,True,1),(True,1,True,0),(True,0,False,0)]:
 f=F();f.w(ctxsym,CTX);f.w(0x201f1c,CTX);head=CTX+0x10c;f.w(head,MSG if nonempty else 0);f.w(head+4,MSG if nonempty else 0);f.w(MSG,head);f.w(MSG+4,head);f.w(0x202f3c,already);f.alloc=allocation
 f.call('drop_notify');assert len(f.queued)==expected
 if expected:assert f.r(ALLOC+4)==0xc000 and f.r(0x202f3c)==1 and f.r(state+16)==1
 if not allocation:assert f.r(state+20)==1
proof.append('Actual native queue-empty predicate and C000 allocator schedule one bounded continuation, with duplicate/failure checks')
f=F();f.w(state+4,1);f.w(MSG+4,0x7074);assert f.call('sco_outbound_send',MSG,sym['sco_outbound_env'])==2
assert f.r(out+36)==1 and f.r(out)==0
proof.append('Reset-blocked outbound drops before touching any endpoint or pipe')
assert all(h['address']%4==0 for h in m['hooks'])
report={'groups':len(proof),'checks':proof,'hook_word_alignment':True,'maximum_glue_test_stack_bytes':SP-f.minimum,'limits':['Native allocation failure injection models a returning allocator; actual ROM1797a can fatal on pool exhaustion. Native OS queue and allocator endpoints modeled; native queue predicate/C000 builder execute from trusted ROM. Native pipe/ring ownership is covered by the separate adapter tests.']}
(root/'glue-proof.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
