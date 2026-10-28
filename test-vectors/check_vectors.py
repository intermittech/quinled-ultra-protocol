#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Round trips, independent arithmetic and negative format/encoding checks."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import sys
import re
import math
import hashlib
import codec
from codec import command as admit_command, encode, decode, crc8, crc16, crc32, descriptor_prefixes, erase_cursor, KNOWN_PREFIX
from host_formats import canonical, pages, reassemble, validate

HERE=Path(__file__).resolve().parent
def need(value, message):
    if not value: raise AssertionError(message)
def rejects(call, name):
    try: call()
    except (ValueError, AssertionError): return
    raise AssertionError('accepted invalid '+name)

def check_uid16(rows):
    poll_uids={bytes.fromhex(r['fields']['data'])[:12] for r in rows
               if r['kind']=='update' and r['fields']['opcode']==0x86}
    for row in rows:
        fields=row['fields']
        if 'uid16' not in fields: continue
        uid=None
        if 'uid' in fields:
            uid=bytes.fromhex(fields['uid'])
        elif row['name'].startswith('identity_part0_'):
            uid=bytes.fromhex(fields['data'])[:12]
        elif row['kind']=='answer' and fields['page']==0x86:
            need(len(poll_uids)==1,'unambiguous update status UID')
            uid=next(iter(poll_uids))
        if uid is not None:
            need(len(uid)==12 and fields['uid16']==crc16(uid),'UID16 '+row['name'])

def main():
    rows=json.loads((HERE/'vectors.json').read_text())['vectors']
    check_uid16(rows)
    old=deepcopy(rows)
    next(r for r in old if r['name']=='update_status_busy')['fields']['uid16']=0x1234
    rejects(lambda:check_uid16(old),'old update status UID16')
    need(crc16(bytes(range(12)))==0x7673,'fixture UID CRC known answer')
    print('PASS: UID16/UID consistency, update status poll binding and old-UID16 negative control')
    import p1_cases
    p1_cases.check(rows)
    names=set()
    for row in rows:
        need(row['name'] not in names,'duplicate vector name');names.add(row['name'])
        raw=bytes.fromhex(row['hex']);kind=row['kind'];fields=row['fields']
        need(encode(kind,fields)==raw,'encode '+row['name'])
        need(decode(kind,raw)==fields,'decode '+row['name'])
        need(encode(kind,decode(kind,raw))==raw,'roundtrip '+row['name'])
        if kind in ('header','slot','answer','descriptor','image','update'):
            bad=bytearray(raw);bad[-1]^=1
            rejects(lambda:decode(kind,bytes(bad)),row['name']+' CRC')
            rejects(lambda:decode(kind,raw[:-1]),row['name']+' truncation')
    byname={r['name']:r for r in rows}
    for pattern in (0x55,0xaa,0,0xff):
        raw=bytes.fromhex(byname[f'slot_training_{pattern:02x}']['hex'])
        need(raw[:3]==bytes(3) and raw[3:30]==bytes((pattern,))*27,'ignored NOP training bytes 3..29')
    need(byname['update_commit_ack']['fields']['session']==7 and
         byname['update_idle_ping']['fields']['session']==0,'commit ACK and idle confirmation sessions')
    committed=bytes.fromhex(byname['update_status_committed']['fields']['data'])
    need(committed[1]==6 and committed[3]==0 and committed[4]==7 and
         int.from_bytes(committed[12:16],'big')==503808,'held COMMITTED status')
    ack=bytes.fromhex(byname['answer_acks_pixel_fx_never']['fields']['data'])
    never=int.from_bytes(ack[18:21],'big')
    need(all(ack[3*r]==0 and never&(1<<(3*r)) for r in range(6)),
         'PIXEL_FX ACKS column NEVER for every receiver/board row')
    need([ack[1],ack[4],ack[16]]==[0x21,0x22,0x33], 'other ACKS columns remain independent')
    state=bytes.fromhex(byname['answer_pixel_fx_output1_group3']['fields']['data'])
    need(state[5]==15 and state[11]==1 and state[15]==0x49 and state[12]==state[16]==0,
         'segment 14 acknowledges in group 3 entry 2, absent segment 15 stays zero')
    codec.NORMAL_VERSION=4
    try:
        row=byname['update_status_poll']
        need(encode('update',row['fields']).hex()==row['hex'] and decode('update',bytes.fromhex(row['hex']))==row['fields'],'update trailer independent of normal version')
    finally:
        codec.NORMAL_VERSION=3
    need(byname['answer_outputs_bank0']['hex']==byname['answer_outputs_bank3']['hex'],'bank payload layout changed')
    need(bytes.fromhex(byname['slot_set_target19']['hex'])[:3]==bytes((0x62,19,0x42)),'SET flags/target placement')
    need(crc8(b'123456789')==0xf4 and crc16(b'123456789')==0x29b1,'CRC known answers')
    for opcode in (3,4,10):
        raw=bytes.fromhex(byname[f'unknown_reserved_opcode_{opcode:02x}']['hex'])
        need(decode('slot',raw)['type']==opcode,'reserved opcode fixture')
        rejects(lambda:admit_command(raw),'unknown reserved analog opcode')
    # Independent word feeding known answer: one zero word has MPEG-2 CRC C704DD7B.
    need(crc32(bytes(4))==0xc704dd7b,'word CRC known answer')
    timing=json.loads((HERE/'vectors_timing.json').read_text())
    for r in timing['rates']:
        rate=r['rate'];pitch=(335<<rate)+165
        end=80+10+(33*10*(2**rate))/0.985+8+5+15
        need(r['pitch']==pitch and r['window16']==1000+16*pitch,'pitch/window')
        need(abs(r['latest_end']-end)<1e-9 and pitch-30-end>=16.89,'end budget')
    need(50+15+2+25080*200/1e6+3<80,'start budget')
    need(2*977*0.0002/(1-0.0002)<0.40,'one-sided check rate interval')
    need(2*25080*0.0002/(1-0.0002)<10.04,'one-sided restore rate interval')
    need(abs(timing['check_bound_us']-(50+2+4+0.40+0.50))<1e-9 and timing['check_bound_us']<57,'check budget')
    need(abs(timing['restore_bound_us']-(50+10+4+10.04+3))<1e-9 and timing['restore_bound_us']<80,'restore budget')
    need(timing['next_frame16_us']==[1000+16*((335<<r)+165)+max(200,80+500) for r in range(3)],'holdoff plus processing LOW')
    need(timing['declared_v203']['program_pause_ms']==2*10+1,'program dispatch margin')
    header=(HERE.parent/'include/ultra_protocol.h').read_text()
    from check_timing import check_timing
    check_timing(timing,need)
    need(2*8*(300+538)/1000<15,'forward/return cable delay')
    need(592+8*15+(4000*.015*9.5+4000/16)/.985<2000,'250k return eye')
    need(224*2*20+100==timing['declared_v203']['lead_in_ms'],'declared pacing')
    # WS2915: all sample-count discontinuities, with calibrated frequency error
    # and a separately qualified 12 ns output-path allowance.
    for nominal in (36,50):
        low,high=nominal*.978,nominal*1.022
        boundaries={low,high}
        for scale,half_step in ((1.28,0),(.34,.5),(.65,.5)):
            for k in range(100):
                boundary=(k+half_step)/scale
                if low<boundary<high:
                    boundaries.update((boundary-1e-9,boundary,boundary+1e-9))
        for f in boundaries:
            period=math.ceil(1.28*f);zero=math.floor(.34*f+.5);one=math.floor(.65*f+.5)
            for samples,minimum,maximum in ((zero,220,380),(one,520,1000),(period-zero,580,1000),(period-one,580,1000),(period,1250,2000)):
                need(samples*1000/(f*1.0002)-12>=minimum and samples*1000/(f*.9998)+12<=maximum,'regenerated WS2915 HIGH/LOW/period')

    from format_cases import cases
    from codec import validate_descriptor,local_packet,parse_local_packet,local_start_us
    fixtures=cases()
    for row in fixtures:
        blob=bytes.fromhex(row['descriptor_hex']);f=validate_descriptor(blob)
        need(f['variant']==201 and f['board']==254,'opaque assembly, no product registry')
        need(encode('descriptor',f)==blob,'firmware-like const format')
    for name,n,banks in [('nano2',2,[2]),('partial7',7,[5,2]),('local10',10,[5,5])]:
        f=next(r['descriptor'] for r in fixtures if r['name']==name)
        actual=[bytes.fromhex(r['value'])[1] for r in f['records'] if r['type']==3]
        need(actual==banks and sum(actual)==n,'odd channel banks')
        tail=banks[-1];ack=bytes.fromhex(byname['answer_acks_'+name]['fields']['data'])
        need(not any(ack[:18]) and int.from_bytes(ack[18:21],'big')==sum(7<<(3*i) for i in range(tail,5)),'absent ACKS rows NEVER, not data')
        bad=deepcopy(f);pwms=[r for r in bad['records'] if r['type']==3];v=bytearray.fromhex(pwms[-1]['value']);v[0]=1;pwms[-1]['value']=v.hex()
        rejects(lambda:validate_descriptor(encode('descriptor',bad)),'misaligned/overlapping bank')
    local=next(r['descriptor'] for r in fixtures if r['name']=='local10')
    need({bytes.fromhex(r['value'])[10] for r in local['records'] if r['type']==1}=={2,3,4},'UART SPI I2C descriptors')
    need(all(bytes.fromhex(r['value'])[16:21]==bytes.fromhex('0000280032')
             for r in local['records'] if r['type']==1),'declared worst-load L1 latency/jitter')
    bad=deepcopy(local);v=bytearray.fromhex(bad['records'][0]['value']);v[1]|=4;bad['records'][0]['value']=v.hex()
    rejects(lambda:validate_descriptor(encode('descriptor',bad)),'local link cannot claim pulse forwarding')
    for row in fixtures:
        bad=deepcopy(row['descriptor']);bad['records'].append(deepcopy(bad['records'][-1]))
        rejects(lambda:validate_descriptor(encode('descriptor',bad)),'duplicate name')
    one_way=deepcopy(next(r['descriptor'] for r in fixtures if r['name']=='nano2'))
    one_way['board']=1
    link=bytearray.fromhex(one_way['records'][0]['value'])
    link[1]=4;link[2]=0;link[8]=0;link[10]=5;link[14]=0
    one_way['records'][0]['value']=link.hex()
    validate_descriptor(encode('descriptor',one_way))
    for flags in (0,1,2,5,6,7,8,12):
        bad=deepcopy(one_way);v=bytearray(link);v[1]=flags
        bad['records'][0]['value']=v.hex()
        rejects(lambda:validate_descriptor(encode('descriptor',bad)),'board 1 forward/no-return invariant')
    for mask in (0,2,16,255):
        bad=deepcopy(one_way);v=bytearray(link);v[16]=mask
        bad['records'][0]['value']=v.hex()
        rejects(lambda:validate_descriptor(encode('descriptor',bad)),'invalid profile mask')
    # No power port or sensing is required for a control-only local node.
    bare=deepcopy(local);bare['lights']=0;bare['records']=[r for r in bare['records'] if r['type'] in (1,8)]
    validate_descriptor(encode('descriptor',bare))
    # Preserve reference, streamed-converter and chip-range rejection coverage.
    digital=deepcopy(next(r['descriptor'] for r in fixtures if r['name']=='dual_pixel'))
    bad=deepcopy(digital);domain=next(r for r in bad['records'] if r['type']==2)
    value=bytearray.fromhex(domain['value']);value[0]=0;domain['value']=value.hex()
    rejects(lambda:validate_descriptor(encode('descriptor',bad)),'cyclic root domain')
    for index,mask in ((16,128),(24,128)):
        bad=deepcopy(digital);pixel=next(r for r in bad['records'] if r['type']==4)
        value=bytearray.fromhex(pixel['value']);value[index]|=mask;pixel['value']=value.hex()
        rejects(lambda:validate_descriptor(encode('descriptor',bad)),'reserved chip or nonexistent ingress')
    from format_cases import domain as make_domain
    bad=deepcopy(digital);extra=make_domain(1,0,2);bad['records'].insert(3,extra)
    for r in bad['records']:
        if r['type']==4:
            value=bytearray.fromhex(r['value']);value[1]=1;r['value']=value.hex()
    rejects(lambda:validate_descriptor(encode('descriptor',bad)),'streamed converter without hardware current limit')
    for packet in json.loads((HERE/'local_vectors.json').read_text()):
        raw=bytes.fromhex(packet['hex']);payload=bytes.fromhex(packet['payload'])
        need(local_packet(packet['endpoint'],packet['kind'],packet['txid'],payload)==raw,'local encode')
        need(parse_local_packet(raw)==(packet['endpoint'],packet['kind'],packet['txid'],payload),'local decode, identical command bytes')
        if 'accepted_us' in packet:
            need(local_start_us(raw,packet['accepted_us'])==packet['start_us'],'L1 acceptance-relative signed phase')
            need(packet['due_us']==max(packet['accepted_us'],packet['start_us']) and
                 packet['latest_output_us']==packet['due_us']+packet['local_jitter_us'],
                 'L1 physical latch lateness bound')
            corrupt=bytearray(raw);corrupt[38]^=1;corrupt[-2:]=crc16(corrupt[:-2]).to_bytes(2,'big')
            rejects(lambda:local_start_us(bytes(corrupt),packet['accepted_us']),'bad inner CRC despite valid envelope')
            rejects(lambda:local_start_us(raw,-1),'negative acceptance')
            if packet['phase_ms']>0:
                rejects(lambda:local_start_us(raw,0x7fffffffffffffff),'local time overflow')
        rejects(lambda:parse_local_packet(raw[:-1]),'local truncated')
        changed=bytearray(raw);changed[-1]^=1
        rejects(lambda:parse_local_packet(changed),'local CRC')
    # Packet image is encoded from recovered bytes, never sampled pulse replay.
    chunks=[bytes.fromhex(byname['pixel_chunk0']['fields']['payload'])[3:],bytes.fromhex(byname['pixel_chunk1']['fields']['payload'])[3:9]]
    pixel_bytes=b''.join(chunks)
    begin=bytes.fromhex(byname['pixel_begin']['fields']['payload'])
    need(len(pixel_bytes)==int.from_bytes(begin[3:5],'big') and crc16(pixel_bytes)==int.from_bytes(begin[5:7],'big'),'pixel packet full frame CRC')
    need(pixel_bytes==bytes(range(30)),'pixel regeneration input bytes')
    original=bytes.fromhex(byname['descriptor_over255_unknown_tlv']['hex'])
    extended=bytes.fromhex(byname['descriptor_known_extended']['hex'])
    need(descriptor_prefixes(original)==descriptor_prefixes(extended),'long known prefix retains semantics')
    for t,n in KNOWN_PREFIX.items():
        fields=dict(board=6,lights=0,flags=0,records=[dict(type=t,value=bytes(n-1).hex())])
        rejects(lambda:decode('descriptor',encode('descriptor',fields)),'short known record '+str(t))
        fields['records'][0]['value']=bytes(n+3).hex()
        need(len(descriptor_prefixes(encode('descriptor',fields))[0][1])==n,'skip appended known tail')
    sense_blob=bytes.fromhex(byname['descriptor_sense_10a_board']['hex'])
    need(validate_descriptor(sense_blob)['board']==254,'synthetic SENSE descriptor with valid DOMAIN/NAME')
    sense=next(value for kind,value in descriptor_prefixes(sense_blob) if kind==5)
    need(int.from_bytes(sense[6:8],'big')==16500,'sense full scale')
    records=bytes.fromhex('000c0bb8014d0003ff1507d0ff2105dc')
    need(int.from_bytes(bytes.fromhex(byname['answer_status_config_crc_targets']['fields']['data'])[21:23],'big')==crc16(records),'STATUS CRC several targets')
    need(130*500==65000 and 255*500>=65500 and 63*2000>=65500 and 32767*2>=65500,'all compact current ranges')
    need((65000*4800*10+999999)//1000000==3120,'LIFE_SYNC rated-power energy step')
    cursor=erase_cursor(0x08002000,0x08002100,57088,57088,256)
    need(len(cursor)==224 and cursor[0]==0x08002000 and cursor[-1]+256==0x08010000,'complete erase beyond 24 operations')
    need(all(b-a==256 for a,b in zip(cursor,cursor[1:])),'erase cursor without holes')
    need(len(erase_cursor(0x08004000,0x08005000,503808,503808,4096))==124,'V407 erase coverage')
    for args in [(0x08002000,0x08002100,57092,57088,256),(0x08002001,0x08002100,4,57088,256),(0xffffff00,0xffffff80,256,256,128),(0x08002000,0x08002100,4,4,256),(0x08002000,0x08002100,0,57088,256)]:
        rejects(lambda:erase_cursor(*args),'erase cursor bounds')
    host=json.loads((HERE/'host_vectors.json').read_text())['vectors']
    need(len(host)==4,'two host vectors per format')
    validate('P3',dict(v=3,dom=[],out=[]))
    local_setup=deepcopy(host[2]['value']);node=local_setup['boards'][0]
    node.update(domains=[],channels=[],outputs=[],config=[])
    validate('S2',local_setup)
    for row in host:
        kind,value,parts=row['kind'],row['value'],row['pages']
        body=canonical(kind,value)
        need(body==row['canonical'] and len(body)==row['length'],'canonical host payload')
        need(hashlib.sha256(body.encode('ascii')).hexdigest()==row['sha256'],'host payload digest')
        need(pages(kind,value)==parts and all(len(s)<=255 for s in parts),'host paging bytes/bound')
        need(reassemble(kind,parts[::-1]+parts[:1])==value,'host out-of-order and identical duplicate roundtrip')
        rejects(lambda:reassemble(kind,parts[:-1]),'missing page')
        rejects(lambda:reassemble(kind,[parts[0][:-1]]+parts[1:]),'truncated page')
        changed=parts[0][:-1]+('x' if parts[0][-1]!='x' else 'y')
        rejects(lambda:reassemble(kind,[changed]+parts[1:]),'corrupt page digest')
        rejects(lambda:reassemble(kind,parts+[changed]),'conflicting duplicate')
        rejects(lambda:reassemble(kind,[parts[0]+'x'*255]+parts[1:]),'overlong page')
        bad=deepcopy(value)
        if kind=='P3':bad['dom'][0]['ma']=65536
        else:bad['boards'][0]['domains'][0]['lim_ma']=65536
        rejects(lambda:validate(kind,bad),'current beyond u16')
    rejects(lambda:reassemble('P3',host[0]['pages']+host[1]['pages']),'mixed snapshots')
    bad=deepcopy(host[2]['value']);bad['schema']='S1'
    rejects(lambda:validate('S2',bad),'S1 import')
    bad=deepcopy(host[2]['value']);bad['boards'][0]['config'][0]['target']=255
    rejects(lambda:validate('S2',bad),'wrong CONFIG scope')
    bad=deepcopy(host[0]['value']);bad['dom'].append(deepcopy(bad['dom'][0]))
    rejects(lambda:validate('P3',bad),'duplicate domain')
    need(host[0]['value']['dom'][0]['mw']==65000*4800//100,'65 A power range')
    print(f'PASS: {len(rows)} encode/decode/re-encode vectors; CRC/truncation, adaptive timing, local links, partial banks, prefix compatibility, current ranges and erase coverage checks')
    print('PASS: 4 host schema/JSON/paging vectors and malformed transfer rejection checks')
    print('PASS: 9 L1 packets, including 6 acceptance-relative START/phase and lateness goldens')
    return 0

if __name__=='__main__': sys.exit(main())
