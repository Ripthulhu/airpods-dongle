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
    d=b-a-4;assert d%2==0 and -(1<<24)<=d<(1<<24)
    v=d&0x1ffffff;s=v>>24
    return struct.pack('<HH',0xf000|(s<<10)|(v>>12&0x3ff),0x9000|((1^(v>>23&1)^s)<<13)|((1^(v>>22&1)^s)<<11)|(v>>1&0x7ff))

REGS=(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,
      UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11,
      UC_ARM_REG_R12,UC_ARM_REG_SP,UC_ARM_REG_LR)
REPORT,STACK,BB=0x211000,0x21f000,0x210000
STAT=syms['sco_credit_vendor_stats']

def run(host=60,previous=0,packet=120,quarter=False,feature=0,mode=2,cpu=1,
        aggregate=0,group=0,mapping=True,unsafe=False,lookup=True,patched=True,
        flags=0x28000000,direct_lr=None):
    conn=0x2066cc+0x150*group;prod=0x2063e4+40*group;cons=0x20636c+40*group;pipe=conn+0x5c
    u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS);u.mem_map(0,0x100000);u.mem_write(0,rom)
    u.mem_write(0xd0200,vendor);u.mem_write(base,bytes(blob));u.mem_map(0x200000,0x30000)
    if patched:u.mem_write(0x37b04,branch(0x37b04,syms['sco_credit_vendor_thunk']&~1))
    def w(a,v):u.mem_write(a,struct.pack('<I',v))
    w(BB+0x50,cons);w(cons+0x14,pipe);w(prod+0x14,pipe)
    w(pipe+0x18,prod);w(pipe+0x1c,cons);w(prod,0x662e1 if not unsafe else 0x66295)
    u.mem_write(prod+0x24,b'\x01');u.mem_write(pipe+0x4c,struct.pack('<H',packet))
    w(conn+0xec,host);w(conn+0xf0,previous);w(conn+0xe4,group<<18)
    u.mem_write(conn+0xd0,struct.pack('<H',0x1000 if lookup else 0))
    u.mem_write(conn+0xc3,bytes([3 if quarter else 0]));u.mem_write(conn+0xd2,struct.pack('<H',6))
    u.mem_write(0x202e84+2*group,struct.pack('<H',6 if mapping else 7))
    w(0x2038d0,mode);u.mem_write(0x202e7d,bytes([cpu]));w(0x202dbc,REPORT)
    u.mem_write(REPORT,bytes([aggregate,0])+bytes(6))
    # Existing pending aggregate belongs to the same handle if nonzero.
    if aggregate:u.mem_write(REPORT,bytes([aggregate,1])+struct.pack('<HH',6,aggregate))
    for index,reg in enumerate(REGS):u.reg_write(reg,0x12340000+index*0x101)
    u.reg_write(UC_ARM_REG_R0,conn);u.reg_write(UC_ARM_REG_R4,BB)
    u.reg_write(UC_ARM_REG_SP,STACK);u.reg_write(UC_ARM_REG_LR,0x90001)
    u.reg_write(UC_ARM_REG_APSR,flags)
    if direct_lr is not None:
        u.reg_write(UC_ARM_REG_R0,6);u.reg_write(UC_ARM_REG_R1,1);u.reg_write(UC_ARM_REG_R2,1)
        u.reg_write(UC_ARM_REG_R5,conn);u.reg_write(UC_ARM_REG_R6,host)
        u.reg_write(UC_ARM_REG_R7,packet);u.reg_write(UC_ARM_REG_R8,previous)
        u.reg_write(UC_ARM_REG_LR,direct_lr)
    calls=[];events=[];lookup_args=[];depth=[0]
    stops={0xd1b60,0xd1c02,0x90000}
    if direct_lr is not None:stops.add(direct_lr & ~1)
    def code(u,a,size,_):
        depth[0]=max(depth[0],STACK-u.reg_read(UC_ARM_REG_SP))
        if a in stops:u.emu_stop()
        elif a==0x5ab94:
            u.reg_write(UC_ARM_REG_R0,feature);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
        elif a==0xf4a6:
            lookup_args.append([u.reg_read(UC_ARM_REG_R0),u.reg_read(UC_ARM_REG_R1)])
        elif a==0x37b08:calls.append([u.reg_read(r) for r in REGS[:3]])
        elif a==0x26522:
            events.append(u.reg_read(UC_ARM_REG_R0));u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
    def writes(u,access,a,size,value,_):
        assert (STACK-256<=a<STACK or conn+0xf0<=a<conn+0xf4 or
                REPORT<=a<REPORT+8 or STAT<=a<STAT+40),hex(a)
    u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_WRITE,writes)
    u.emu_start(0x37b05 if direct_lr is not None else 0xd1b0b,0,count=2000)
    assert u.reg_read(UC_ARM_REG_PC) in stops
    assert u.reg_read(UC_ARM_REG_SP)==STACK
    assert u.reg_read(UC_ARM_REG_APSR)&0x08000000==flags&0x08000000
    return dict(residual=struct.unpack('<I',u.mem_read(conn+0xf0,4))[0],calls=calls,
                aggregate=u.mem_read(REPORT,1)[0],per_handle=struct.unpack('<H',u.mem_read(REPORT+4,2))[0],
                stats=list(struct.unpack('<10I',u.mem_read(STAT,40))),events=events,
                lookup=lookup_args,stack=depth[0])

cases=0
for host in (0,4,60,120,255):
    for previous in (0,1,59,60,120,15240,0xfffffffe):
        for packet in (60,120,240):
            for quarter,feature in ((False,0),(True,0),(True,1)):
                for aggregate in (0,250,254):
                    result=run(host,previous,packet,quarter,feature,aggregate=aggregate)
                    original=run(host,previous,packet,quarter,feature,aggregate=aggregate,patched=False)
                    unit=host//4 if quarter and feature else host
                    total=(previous+packet)&0xffffffff
                    old_remaining=(total-unit)&0xffffffff
                    can_call=host!=0 and total>=unit
                    corrected=1+old_remaining//60
                    eligible=(can_call and host==60 and packet==120 and previous+packet<=0xffffffff
                              and unit==60 and corrected<=255 and aggregate+corrected<=255)
                    if eligible:
                        assert result['residual']==old_remaining%60
                        assert result['calls']==[[6,corrected,1]]
                        assert result['aggregate']==aggregate+corrected
                        assert result['per_handle']==aggregate+corrected
                        assert result['stats'][2]==corrected-1
                        assert result['stats'][3]==0xd1b61
                    else:
                        for key in ('residual','calls','aggregate','per_handle','events'):
                            assert result[key]==original[key],(key,host,previous,packet,quarter,feature,aggregate,result,original)
                    cases+=1
for kw in ({'mode':0},{'cpu':0},{'mapping':False},{'unsafe':True},{'lookup':False}):
    result=run(**kw);original=run(**kw,patched=False)
    for key in ('residual','calls','aggregate','per_handle','events'):assert result[key]==original[key]
    cases+=1
for group in (0,1,2):
    for flags in (0,0x08000000,0xf8000000):
        result=run(group=group,flags=flags)
        assert result['calls']==[[6,2,1]] and result['residual']==0
        cases+=1
for lr in (0x90001,0x3bce7,0x3a9c5,0x3917f):
    result=run(direct_lr=lr);original=run(direct_lr=lr,patched=False)
    for key in ('residual','calls','aggregate','per_handle','events'):assert result[key]==original[key]
    assert result['stats']==[0]*10 and not result['lookup']
    cases+=1
normal=run();assert normal['calls']==[[6,2,1]] and normal['stats'][2]==1
proof=dict(cases=cases,base=base,length=len(blob),sha256=hashlib.sha256(blob).hexdigest(),
           symbols=syms,hook=dict(address=0x37b04,expected=rom[0x37b04:0x37b08].hex(),
                                 patch=branch(0x37b04,syms['sco_credit_vendor_thunk']&~1).hex()),
           normal=normal,actual='Unmodified signed vendor d1b0a..d1b60, actual native f4a6(handle,2) lookup and actual native37b04 report writes',
           stubbed=['5ab94 feature query','26522 event scheduling observer'],
           limit='Only fixed60-byte CVSD/native120 blocks with inferred actual nativeunit60. Other callers/states use original report.')
(ROOT/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');print(json.dumps(proof,indent=2))
