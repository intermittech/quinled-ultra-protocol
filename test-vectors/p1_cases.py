#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Complete command/page fixtures and independently calculated launch traces."""
import json
import struct
from fractions import Fraction
from pathlib import Path
from codec import encode, crc16, crc8

HERE=Path(__file__).resolve().parent

def add_cases(add,slot,answer,update):
    for flags in (0,32,64,96):
        slot(f'slot_set_flags_{flags}',2,239,255,struct.pack('>11H',1,*range(5),*range(5,10))+b'\0\0\1\x80\0',flags)
    for opcode in (3,4,10):slot(f'unknown_reserved_opcode_{opcode:02x}',opcode,0,1,bytes(27))
    slot('slot_nop',0,0,0,b'')
    for pattern in (0x55,0xaa,0,0xff):
        slot(f'slot_training_{pattern:02x}',0,0,0,bytes((pattern,))*27)
    slot('slot_light_opt_target239',9,239,12,bytes((3,))+struct.pack('>13H',1538,3704,3704,4000,0,3704,16384,3000,32768,2400,65535,1538,0))
    for mode in (0,1):slot(f'slot_scene_{mode}',8,239,20,bytes((mode,3))+struct.pack('>11H',100,*range(5),*range(5,10)))
    for action in (0,1,2,3,4,5,7):slot(f'slot_identify_{action}',6,255,action,bytes((action,5 if action==2 else 0)))
    for bank in (0,3):
        for page,name in ((6,'calibration'),(7,'cal_test'),(10,'health'),(13,'diag')):
            part=1 if page in (7,13) else 0
            slot(f'poll_{name}_bank{bank}',7,255,42,bytes((0,1,page,2,part,0,0,bank)))
            answer(f'answer_{name}_bank{bank}',page,bytes(range(24)))
    answer('identity_features_all_words',4,b''.join(struct.pack('>I',x) for x in (0x12345678,0x87654321,1,0x80000000,0xffffffff,0x55aa55aa)))
    for op,name in ((0x84,'abort'),(0x85,'ping')):update('update_'+name,op,b'')
    answer('update_status_committed',0x86,bytes((1,6,0,0,7,2))+
           struct.pack('>HIIII',0,503808,503808,0,0x12345678),seq=7)
    update('update_commit_ack',0x85,b'')
    add('update_idle_ping','update',dict(opcode=0x85,session=0,index=0,flags=0,
        data='00'*32,time=0xabcdef,**{'pass':0}),'Only session-zero idle PING can confirm an application')
    # All scoped settings, including highest legal target/output/domain.
    for target,ids in ((255,list(range(1,11))+list(range(21,25))+list(range(33,47))),(239,[11,12]),(14,list(range(64,78)))):
        for start in range(0,len(ids),9):
            values={1:8,2:1000,3:2,4:10,5:1,6:5,7:4,8:3,9:30,10:2,22:3,23:1,24:0x010a,
                    64:0,65:0,66:100,67:60,68:20,70:2,71:0,72:0,73:3,74:30,75:2,76:3,77:0}
            pairs=b''.join(bytes((i,))+struct.pack('>H',values.get(i,65000)) for i in ids[start:start+9])
            slot(f'slot_config_target{target}_page{start//9}',5,target,42+start,pairs)
    slot('slot_pixel_fx_output14_segment14',11,238,4,b'\x01'+bytes(26))
    slot('slot_pixel_fx_output14_all',11,239,5,b'\x01'+bytes(26))
    slot('pixel_abort',12,14,9,b'\x03')
    for label,ms in (('past',-32768),('now',0),('future',32767)):
        slot('l1_set_'+label,2,0,91,bytes(25)+struct.pack('>h',ms))
    for label,ms in (('past',-2),('now',0),('future',25)):
        slot('l1_pixel_fx_'+label,11,0,93,b'\x01'+struct.pack('>i',ms))
    from copy import deepcopy
    from format_cases import cases
    descriptor=deepcopy(next(r['descriptor'] for r in cases() if r['name']=='dual_pixel'))
    for r in descriptor['records']:
        if r['type']==4 and bytes.fromhex(r['value'])[0]==1:
            value=bytearray.fromhex(r['value']);value[2]=15;r['value']=value.hex()
    add('descriptor_pixel_fx_segments15','descriptor',descriptor,'Synthetic receiver with 15 segments on output 1')
    for name in ('unbounded','unknown_jitter','extended'):
        local=deepcopy(next(r['descriptor'] for r in cases() if r['name']=='local10'))
        for r in local['records']:
            if r['type']!=1:continue
            v=bytes.fromhex(r['value'])
            r['value']=(v[:16] if name=='unbounded' else v[:19]+b'\xff\xff' if name=='unknown_jitter' else v+b'\xaa\xbb\xcc').hex()
        add('descriptor_local_'+name,'descriptor',local,'Synthetic L1 scheduling bounds: '+name)
    slot('slot_pixel_fx_output1_segment14',11,0x1e,0x49,b'\x01'+bytes(26))
    slot('poll_acks_pixel_fx_never',7,255,0x42,bytes((0,1,2,0,11,5,2,0)))
    ack=bytearray(24)
    ack[1]=0x21;ack[4]=0x22;ack[16]=0x33
    never=sum((1<<(3*r)) | (1<<(3*r+2)) for r in range(6))
    never|=sum(1<<(3*r+1) for r in (2,3,4))
    ack[18:21]=never.to_bytes(3,'big')
    answer('answer_acks_pixel_fx_never',2,bytes(ack))
    for group in range(4):
        slot(f'poll_pixel_fx_output1_group{group}',7,255,0x42,bytes((0,1,17,0,0,group,0,1)))
        state=bytearray(24);state[:9]=struct.pack('>HBBBBHB',1,2,1,7,15,1500,1)
        if group==3:state[11]=1;state[15]=0x49
        state[20]=255
        answer(f'answer_pixel_fx_output1_group{group}',17,bytes(state))

def shared_header(rows):
    out=['/* SPDX-License-Identifier: MIT */','/* Generated from committed vectors.json; firmware may include this header. */',
         '#ifndef ULTRA_SHARED_VECTORS_H','#define ULTRA_SHARED_VECTORS_H','#include <stdint.h>',
         'typedef struct { const char *name,*kind; const uint8_t *bytes; uint16_t size; } ultra_shared_vector_t;']
    for i,v in enumerate(rows):out.append('static const uint8_t ultra_vector_%d[]={%s};'%(i,','.join('0x'+v['hex'][j:j+2] for j in range(0,len(v['hex']),2))))
    out.append('static const ultra_shared_vector_t ultra_shared_vectors[]={')
    for i,v in enumerate(rows):out.append('{"%s","%s",ultra_vector_%d,sizeof ultra_vector_%d},'%(v['name'],v['kind'],i,i))
    out+=['};','#define ULTRA_SHARED_VECTOR_COUNT (sizeof ultra_shared_vectors / sizeof ultra_shared_vectors[0])','#endif']
    return '\n'.join(out)+'\n'

TIME_WRAP=4194304000   # header TIME = host us modulo 2^22 x 1000 (ULTRA_TIME_WRAP)

def traces():
    out=[]
    tables={9600000:((12,3,9),(16,4,12),(24,6,18),(48,12,36)),
            40000000:((50,12,38),(67,17,50),(100,25,75),(200,50,150)),
            80000000:((100,25,75),(133,33,100),(200,50,150),(400,100,300))}
    slot=encode('slot',dict(type=0,target=0,flags=0,seq=0,payload='00'*27))
    for hz,rows in tables.items():
        for profile,(period,zero,one) in enumerate(rows):
            for start in (0,1000000,TIME_WRAP-16,0xfffffff0):   # (TIME's wrap, and the host's 32-bit one)
                prefix=319*period
                lo=(prefix+zero)*1000000//hz
                hi=((prefix+one)*1000000+hz-1)//hz
                candidates=[]
                for delta in range(lo,hi+1):
                    frame=slot+encode('header',dict(flags=0,time=(start+delta)%TIME_WRAP))
                    ticks=prefix+(one if frame[-1]&1 else zero)
                    candidates.append((abs(delta*hz-ticks*1000000),delta,frame,ticks))
                _,delta,frame,ticks=min(candidates)
                update_prefix=383*period
                update_candidates=[]
                for d in range((update_prefix+zero)*1000000//hz,
                               ((update_prefix+one)*1000000+hz-1)//hz+1):
                    update=encode('update',dict(opcode=0x85,session=7,index=0,flags=0,data='00'*32,time=(start+d)%TIME_WRAP,**{'pass':0}))
                    edge=update_prefix+(one if update[-1]&1 else zero)
                    update_candidates.append((abs(d*hz-edge*1000000),d,update,edge))
                _,update_delta,update_frame,update_ticks=min(update_candidates)
                out.append(dict(profile=profile,clock_hz=hz,period_ticks=period,zero_ticks=zero,one_ticks=one,
                                start_us=start,time_us=(start+delta)%TIME_WRAP,frame_hex=frame.hex(),
                                falling_edge_ticks=ticks,reset_ticks=hz//2000,
                                actual_rate_num=hz,actual_rate_den=period,
                                time_error_ns_num=abs(delta*hz-ticks*1000000)*1000,time_error_ns_den=hz,
                                update_frame_hex=update_frame.hex(),update_time_us=(start+update_delta)%TIME_WRAP,
                                update_edge_ticks=update_ticks))
    return dict(format=1,traces=out)

def trace_header(gold):
    out=['/* SPDX-License-Identifier: MIT */','#ifndef ULTRA_GOLDEN_TRACES_H','#define ULTRA_GOLDEN_TRACES_H','#include <stdint.h>',
         'typedef struct { unsigned profile,clock_hz,period,zero,one; uint32_t start,time; uint8_t frame[40]; uint32_t edge_ticks; uint8_t update[48]; uint32_t update_time,update_edge; } ultra_trace_t;',
         'static const ultra_trace_t ultra_golden_traces[]={']
    for t in gold['traces']:
        out.append('{%du,%du,%du,%du,%du,%du,%du,{%s},%du,{%s},%du,%du},'%(t['profile'],t['clock_hz'],t['period_ticks'],t['zero_ticks'],t['one_ticks'],t['start_us'],t['time_us'],','.join('0x'+t['frame_hex'][i:i+2] for i in range(0,80,2)),t['falling_edge_ticks'],','.join('0x'+t['update_frame_hex'][i:i+2] for i in range(0,96,2)),t['update_time_us'],t['update_edge_ticks']))
    return '\n'.join(out+['};','#endif'])+'\n'

def write(rows):
    (HERE/'shared_vectors.h').write_text(shared_header(rows),encoding='utf-8',newline='\n')
    gold=traces()
    (HERE/'golden_traces.json').write_text(json.dumps(gold,indent=2)+'\n',encoding='utf-8',newline='\n')
    (HERE/'golden_traces.h').write_text(trace_header(gold),encoding='utf-8',newline='\n')

def local_header(rows):
    out=['/* SPDX-License-Identifier: MIT */','/* Committed L1 packet/acceptance scheduling goldens. */',
         '#ifndef ULTRA_LOCAL_VECTORS_H','#define ULTRA_LOCAL_VECTORS_H','#include <stdint.h>',
         'typedef struct { uint16_t size; uint8_t packet[58]; int timed; int64_t accepted_us,start_us; } ultra_local_vector_t;',
         'static const ultra_local_vector_t ultra_local_vectors[]={']
    for r in rows:
        packet=bytes.fromhex(r['hex'])
        out.append('{%du,{%s},%d,INT64_C(%d),INT64_C(%d)},'%(len(packet),
            ','.join('0x%02x'%b for b in packet),int('accepted_us' in r),r.get('accepted_us',0),r.get('start_us',0)))
    return '\n'.join(out+['};','#endif'])+'\n'

def check(rows):
    if (HERE/'shared_vectors.h').read_text()!=shared_header(rows):raise AssertionError('shared C fixture drift')
    gold=json.loads((HERE/'golden_traces.json').read_text())
    if gold!=traces() or (HERE/'golden_traces.h').read_text()!=trace_header(gold):raise AssertionError('launch golden drift')
    for t in gold['traces']:
        if Fraction(t['time_error_ns_num'],t['time_error_ns_den'])>2000:raise AssertionError('TIME edge budget')
    print(f"PASS: {len(gold['traces'])} exact-clock profile/TIME traces and C shared-vector export")
    local=json.loads((HERE/'local_vectors.json').read_text())
    if (HERE/'local_vectors.h').read_text()!=local_header(local):raise AssertionError('L1 C fixture drift')
