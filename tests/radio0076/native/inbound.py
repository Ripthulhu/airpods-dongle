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
P=0x210000;BLOCK=0x210100;CONS=0x210240;RING=0x212000;SP=0x228000;STOP=0x88000
EP=0x2063e4;CON=0x2066cc
REGS=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11,UC_ARM_REG_R12,UC_ARM_REG_LR]
class Test:
 def __init__(self,ready=False,packetlen=60):
  self.u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
  self.u.mem_map(0,0x90000);self.u.mem_write(0,rom)
  self.u.mem_map(0xd0000,0x10000);self.u.mem_write(0xd0200,signed)
  self.u.mem_map(0x200000,0x30000)
  for a,b in sections:self.u.mem_write(a,b)
  self.u.reg_write(UC_ARM_REG_XPSR,0x1000000)
  self.u.reg_write(UC_ARM_REG_MSP,SP)
  self.u.reg_write(UC_ARM_REG_SP,SP)
  self.packetlen=packetlen;self.credits=[];self.submitted=[];self.copies=[];self.stops=[];self.parser_test=False
  self.resets=[];self.forbid_rx_write=False
  self.w8(0x202e7d,1);self.w32(0x202e80,0x662e1)
  self.w16(CON+0xd0,0x1000);self.w16(CON+0xd2,6);self.w32(CON+0xe4,0)
  self.w16(0x202e84,6);self.w16(0x202e86,0xfff);self.w16(0x202e88,0xfff)
  self.w32(EP,0x662e1);self.w32(EP+0x14,P);self.w8(EP+0x24,1)
  self.w32(P+0x18,EP);self.w32(P+0x1c,CONS)
  self.w16(P+0x4c,packetlen);self.w16(P+0x4e,packetlen if ready else 0);self.w16(P+0x50,packetlen)
  self.queue(P+8,[BLOCK] if ready else []);self.queue(P+0x10,[] if ready else [BLOCK])
  self.w8(0x2039ae,3)
  self.u.mem_write(BLOCK+0x10,bytes([0xa5])*packetlen)
  self.u.mem_write(RING-4,bytes([0x5a])*4104)
  self.u.hook_add(UC_HOOK_CODE,self.on_code)
  self.u.hook_add(UC_HOOK_MEM_WRITE,self.on_write)
 def w8(self,a,v):self.u.mem_write(a,bytes([v]))
 def w16(self,a,v):self.u.mem_write(a,struct.pack('<H',v))
 def w32(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def r16(self,a):return struct.unpack('<H',self.u.mem_read(a,2))[0]
 def r32(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
 def queue(self,a,items):
  rows=[a]+items
  for i,b in enumerate(rows):self.w32(b,rows[(i+1)%len(rows)]);self.w32(b+4,rows[(i-1)%len(rows)])
 def on_write(self,u,access,a,n,value,_):
  assert not (0xd0200<=a<0xd3000),(hex(a),n)
  if self.forbid_rx_write:assert a+n<=symbols['rx'] or a>=symbols['rx']+258,(hex(a),n)
 def on_code(self,u,a,n,_):
  if a in self.stops:u.emu_stop();return
  if self.parser_test and a==0x5f940:
   # Wake/clock handshake only; the native parser and index commit still execute.
   u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR));return
  if a==(symbols['sco_bridge_reset']&~1):
   self.resets.append(dict(primask=u.reg_read(UC_ARM_REG_PRIMASK),lr=u.reg_read(UC_ARM_REG_LR)))
  if a==0x37b04:
   self.credits.append([u.reg_read(r) for r in REGS[:3]])
   u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
  elif a==0x390aa:self.submitted.append([u.reg_read(r) for r in REGS[:3]])
  elif a==0x662e0:self.copies.append([u.reg_read(r) for r in REGS[:3]])
  elif a in (0x5ed6,0x9736,0x1017e):raise AssertionError(f'Unexpected fatal/log helper {a:x}')
 def call(self,address,*args,stops=None):
  for r,v in zip(REGS[:4],args):self.u.reg_write(r,v)
  self.u.reg_write(UC_ARM_REG_SP,SP);self.u.reg_write(UC_ARM_REG_LR,STOP|1)
  if len(args)>4:self.w32(SP,args[4])
  self.stops=stops or [STOP]
  self.u.emu_start(address|1,0,count=50000)
  assert self.u.reg_read(UC_ARM_REG_PC) in self.stops,hex(self.u.reg_read(UC_ARM_REG_PC))
  assert bytes(self.u.mem_read(0xd0200,len(signed)))==signed
  return self.u.reg_read(UC_ARM_REG_R0)
 def process(self,data,offset=0,length=None,wrapped=None):
  if length is None:length=len(data)
  split=min(len(data),4096-offset)
  self.u.mem_write(RING+offset,data[:split])
  if len(data)>split:self.u.mem_write(RING,data[split:])
  if wrapped is None:wrapped=max(0,length-(4096-offset))
  return self.call(symbols['sco_inbound_process'],RING+offset,length,wrapped,RING,RING+4095)
 def count(self,name):
  names=['frames','malformed','invalid_handle','inactive','unsafe_pipe','cpu_mode_mismatch','native_submissions','native_bytes','unavailable','busy_retries','init_success','init_rejected']
  return self.r32(symbols['sco_inbound_stats']+names.index(name)*4)
 def consume_credit(self,size):
  # Actual baseband accumulation sequence; only downstream credit/event helper is stubbed.
  self.u.reg_write(UC_ARM_REG_R4,0x214000);self.u.reg_write(UC_ARM_REG_R5,CON)
  self.w32(0x214000+0x50,EP);self.w16(P+0x4c,size)
  self.call(0x3a95c,EP,stops=[0x3aa6e])

results=[]
def passed(name):results.append(name)
def packet(n=60,handle=6):return struct.pack('<HB',handle,n)+bytes((i*37+9)&255 for i in range(n))

t=Test();oldmask=t.u.reg_read(UC_ARM_REG_PRIMASK)
assert t.process(packet())==1
assert bytes(t.u.mem_read(BLOCK+0x10,60))==packet()[3:]
assert t.r16(P+0x4e)==60 and t.r32(CON+0xec)==60 and not t.credits
assert t.r32(EP)==0x662e1 and t.u.reg_read(UC_ARM_REG_PRIMASK)==oldmask
assert t.count('native_submissions')==1 and t.count('native_bytes')==60
passed('actual mapper, native390aa, native writeBytes and memory callback preserve payload; no early success credit')
t.consume_credit(30);assert not t.credits and t.r32(CON+0xf0)==30
t.consume_credit(30);assert t.credits==[[6,1,1]] and t.r32(CON+0xf0)==0
passed('actual baseband consumption issues credit only after complete host packet length')

for offset in (4095,4094,4093,4070,4034,4033):
 t=Test();assert t.process(packet(),offset)==1
 assert bytes(t.u.mem_read(BLOCK+0x10,60))==packet()[3:] and t.r32(CON+0xec)==60 and not t.credits
passed('SCO header and payload wrap at six ring positions without loss')

for data,length,wrapped in [(b'',0,0),(b'\x06\x00',2,0),(packet(0),3,0),(packet(),259,0),(packet(),63,64),(packet(),62,0),(packet(),64,0)]:
 t=Test();assert t.process(data,length=length,wrapped=wrapped)==1
 assert t.count('malformed')==1 and not t.submitted and not t.credits and t.r16(P+0x4e)==0
passed('zero-payload, short, oversized, inconsistent and partial HCI lengths are consumed without native access')

for handle in (7,0xfff,0x1006,0xffff):
 t=Test();assert t.process(packet(handle=handle))==1
 assert t.count('invalid_handle')==1 and not t.submitted and not t.credits
passed('unknown, sentinel, status/reserved-bit handles rejected by actual mapping')

t=Test();t.w32(CON+0xe4,3<<18);assert t.process(packet())==1 and t.count('invalid_handle')==1 and not t.submitted
t=Test();t.w16(0x202e84,7);assert t.process(packet())==1 and t.count('invalid_handle')==1 and not t.submitted
passed('internal index and endpoint group handle agreement enforced')

for active in (0,2,3,7):
 t=Test();t.w8(EP+0x24,active);assert t.process(packet())==1
 assert t.count('inactive')==1 and not t.submitted and not t.credits
passed('inactive and unsupported format states drain without a retry loop')

for credits in (False,True):
 for invalid in ('pipe-null','uart-copy','bad-producer','bad-pipe','bad-count'):
  t=Test();t.w8(0x20449b,8 if credits else 0)
  if invalid=='pipe-null':t.w32(EP+0x14,0)
  if invalid=='uart-copy':t.w32(EP,0x66295)
  if invalid=='bad-producer':t.w32(P+0x18,EP+40)
  if invalid=='bad-pipe':t.w32(EP+0x14,0xdeadbeec)
  if invalid=='bad-count':t.w16(P+0x4e,61)
  assert t.process(packet())==1 and t.r32(CON+0xec)==0 and not t.copies
  assert t.credits==([[6,1,1]] if credits else [])
passed('unavailable native pipe and guarded rejects retain configured native drop-credit behavior')

t=Test(ready=True);assert t.process(packet())==1
assert bytes(t.u.mem_read(BLOCK+0x10,60))==packet()[3:] and t.r16(P+0x4e)==60 and not t.credits
passed('full pipe follows native discard-oldest then copy semantics')

t=Test();assert t.process(packet(20))==1 and t.r16(P+0x4e)==20 and t.r32(P+0x2c)==20
assert t.process(packet(40))==1 and t.r16(P+0x4e)==60 and t.r32(P+8)==BLOCK
assert bytes(t.u.mem_read(BLOCK+0x10,60))==packet(20)[3:]+packet(40)[3:] and not t.credits
passed('partial packet storage combines native writes across a packet block boundary')

t=Test(packetlen=120);assert t.process(packet())==1 and t.r16(P+0x4e)==60 and t.r32(P+0x2c)==60
assert t.process(packet())==1 and t.r16(P+0x4e)==120 and t.r32(P+8)==BLOCK
assert bytes(t.u.mem_read(BLOCK+0x10,120))==packet()[3:]*2 and t.r32(CON+0xec)==60 and not t.credits
t.consume_credit(120)
assert t.credits==[[6,1,1]] and t.r32(CON+0xf0)==60
passed('live120-byte pipe geometry stores two60-byte host frames; native credit fragment exposes residual accounting explicitly')

t=Test();t.w32(symbols['busy'],1);assert t.process(packet())==0 and t.count('busy_retries')==1 and not t.submitted
t=Test();t.w8(0x202e7d,0);assert t.process(packet())==1 and t.count('cpu_mode_mismatch')==1 and not t.submitted
passed('reentrancy retains frame; missing one-time CPU setup is counted and cannot invoke UART callbacks')

for selector in (0,1):
 t=Test();t.w8(EP+0x24,0);t.w8(0x202e7d,selector);t.w32(0x202e80,0x66295 if selector==0 else 0x662e1)
 assert t.call(symbols['sco_inbound_enable_cpu_mode'])==1
 assert bytes(t.u.mem_read(0x202e7d,1))==b'\1' and t.r32(0x202e80)==0x662e1
passed('actual38e8c idle initializer enables CPU mode from either legal starting selector')
t=Test();assert t.call(symbols['sco_inbound_enable_cpu_mode'])==0 and t.count('init_rejected')==1
passed('one-time setup refuses an already active SCO endpoint')

t=Test();t.call(0x38ad4)
assert bytes(t.u.mem_read(0x202e7d,1))==b'\1' and t.r32(0x202e80)==0x662e1
assert all(t.r16(0x202e84+2*i)==0xfff for i in range(3))
passed('actual endpoint reset38ad4 preserves the CPU selector and RAM discard callback')
for direction,format_value in ((0,1),(1,1),(0,2)):
 t=Test();endpoint=EP if direction==0 else 0x20645c
 # Actual146bc obtains the transport type through this guarded ROM literal.
 transport_pointer=struct.unpack_from('<I',rom,0x14ab8)[0]
 t.w32(transport_pointer,0x215000);t.w8(0x215000+0xb0,9)
 t.w8(endpoint+0x1e,direction)
 t.call(0x38da6,endpoint,format_value)
 assert t.r32(endpoint)==(0x662e1 if direction==0 else 0x662f5)
 assert bytes(t.u.mem_read(endpoint+0x24,1))==bytes([format_value])
passed('actual mode9 endpoint configuration selects RAM producer/consumer callbacks; format2 remains distinctly marked')

# Execute the complete native frame parser, patched type discriminator, adapter,
# and original aligned consumer-index commit. Only the wake/MMIO handshake is stubbed.
for out in (0,4,4028,4088,4092):
 t=Test();t.parser_test=True
 wire=packet();header=len(wire).to_bytes(3,'little')+b'\x03'
 allbytes=header+wire
 first=min(len(allbytes),4096-out)
 t.u.mem_write(RING+out,allbytes[:first])
 if len(allbytes)>first:t.u.mem_write(RING,allbytes[first:])
 write=((out+len(allbytes)+3)&~3)%4096
 t.w32(0x2038cc+0xc,RING);t.w32(0x2038cc+0x10,4096);t.w32(0x2038cc+0x14,4096)
 t.w32(RING+8192,write);t.w32(RING+8196,out)
 assert t.call(0x5fa0e,0x215000)==0
 assert t.r32(RING+8196)==write and t.count('frames')==1 and t.r32(CON+0xec)==60
 assert bytes(t.u.mem_read(BLOCK+0x10,60))==wire[3:] and not t.credits
 assert t.u.reg_read(UC_ARM_REG_SP)==SP
passed('whole native parser, thunk and native aligned ring commit consume exactly once at five wrap positions')

t=Test();t.parser_test=True;t.w32(0x2038cc+0xc,RING);t.w32(0x2038cc+0x10,4096);t.w32(0x2038cc+0x14,4096)
t.w32(RING+8192,68);t.w32(RING+8196,0);t.u.mem_write(RING,(63).to_bytes(3,'little')+b'\3'+packet())
t.w32(symbols['busy'],1);assert t.call(0x5fa0e,0x215000)==0
assert t.r32(RING+8196)==0 and t.count('busy_retries')==1 and not t.submitted
passed('reentrant adapter leaves native ring consumer index unchanged')

for flags in range(32):
 for hci_type in (0,1,2,4,7,255):
  states=[]
  for modified in (False,True):
   t=Test()
   if not modified:t.u.mem_write(0x5faa4,rom[0x5faa4:0x5faa8])
   for i,r in enumerate(REGS):t.u.reg_write(r,0x11110000+i)
   t.u.reg_write(UC_ARM_REG_R1,hci_type)
   t.u.reg_write(UC_ARM_REG_XPSR,0x01000000|(flags<<27))
   t.call(0x5faa4,stops=[0x5fab2,0x5faa8])
   states.append(([t.u.reg_read(r) for r in REGS],t.u.reg_read(UC_ARM_REG_APSR),t.u.reg_read(UC_ARM_REG_SP),t.u.reg_read(UC_ARM_REG_PC)))
  assert states[0]==states[1]
passed('all non-SCO type1 compare/branch register effects and32 NZCVQ patterns remain native')

for offset in (0,4094,4095):
 for primask in (0,1):
  t=Test();data=b'\x03\x0c\x00';first=min(3,4096-offset)
  t.u.mem_write(RING+offset,data[:first])
  if first<3:t.u.mem_write(RING,data[first:])
  t.forbid_rx_write=True;t.u.reg_write(UC_ARM_REG_PRIMASK,primask)
  before=bytes(t.u.mem_read(symbols['rx'],258))
  t.call(symbols['sco_inbound_command'],RING+offset,3,3-first,RING,RING+4095)
  assert len(t.resets)==1 and t.resets[0]['primask']==1
  assert t.u.reg_read(UC_ARM_REG_PRIMASK)==primask
  assert bytes(t.u.mem_read(symbols['rx'],258))==before
passed('exact Reset command invokes cancellation once, including two header splits, without RX scratch use')

for data,length,wrapped in [(b'',0,0),(b'\x03\x0c',2,0),(b'\x03\x0c\x00\x00',4,0),(b'\x03\x0c\x01',3,0),(b'\x04\x0c\x00',3,0),(b'\x03\x04\x00',3,0),(b'\x03\x0c\x00',3,4),(b'\x03\x0c\x00',3,1)]:
 t=Test();t.u.mem_write(RING,data);t.forbid_rx_write=True
 t.call(symbols['sco_inbound_command'],RING,length,wrapped,RING,RING+4095)
 assert not t.resets
# All malformed pointer/span cases must reject before any invalid memory access.
for body,length,wrapped,base,last in [(0xdeadbeec,2,0,RING,RING+4095),(0xdeadbeec,3,0,RING,RING+4095),(RING,3,0,RING+3,RING),(RING,3,0,RING,RING+1)]:
 t=Test();t.forbid_rx_write=True
 t.call(symbols['sco_inbound_command'],body,length,wrapped,base,last)
 assert not t.resets
passed('non-Reset, malformed, parameter-bearing and invalid-span command headers have no cancellation effect')

# Real parser entry reaches root cancellation before the original command owner.
t=Test();t.parser_test=True;t.forbid_rx_write=True
t.w32(0x2038cc+0xc,RING);t.w32(0x2038cc+0x10,4096);t.w32(0x2038cc+0x14,4096)
t.u.mem_write(RING,b'\x03\x00\x00\x01\x03\x0c\x00')
t.w32(RING+8192,8);t.w32(RING+8196,0)
t.call(0x5fa0e,0x215000,stops=[0x5fab2])
assert len(t.resets)==1 and t.r32(RING+8196)==0
passed('actual command parser calls cancellation before native command allocation, dispatch or owner teardown')

# Teardown retains endpoint state1 in observed firmware. Empty handles and pipe
# metadata, rather than state alone, must make stale packets harmless.
t=Test();t.w16(CON+0xd0,0);t.w32(CON+0xe4,0);t.w16(0x202e84,0xfff)
t.w32(P+0x18,0);t.w32(P+0x1c,0);t.w16(P+0x50,0)
assert bytes(t.u.mem_read(EP+0x24,1))==b'\1' and t.r32(EP+0x14)==P
assert t.process(packet())==1 and t.count('invalid_handle')==1 and not t.submitted and not t.credits
passed('observed teardown state1 with retained pipe pointer cannot admit a stale handle')

report=dict(rom_fixture_sha256=hashlib.sha256(rom).hexdigest(),signed_0076_sha256=hashlib.sha256(signed).hexdigest(),cases=results,passed=len(results),native_entries_executed=['f4a6','390aa','39088','670d6','66576','66bca','662e0','663a0','38e8c','38ad4','38da6','5fa0e parser and original commit','3a95c..3a9c4'],credit_helper_stub='37b04 invocation captured; HCI event builder and live scheduler not emulated',reset_helper_stub='External root cancellation invocation observed before native command owner; root cancellation implementation not emulated here',scope='Actual compiled Thumb plus actual trusted ROM with synthetic sample bytes. No hardware. Signed .0076 region remains byte-identical. Root owns final link, publication, and live endpoint qualification.')
(ROOT/'inbound-proof.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
