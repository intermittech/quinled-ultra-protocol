#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate deterministic wire-3 skeleton fixtures from independent codecs."""
import json
from pathlib import Path
from copy import deepcopy
import struct
import re
from codec import encode, decode, crc16
import p1_cases

HERE=Path(__file__).resolve().parent
vectors=[]
UID=bytes(range(12))
def add(name,kind,fields,description):
    vectors.append(dict(name=name,kind=kind,description=description,fields=fields,hex=encode(kind,fields).hex()))
def slot(name,t,target,seq,payload,flags=0):
    add(name,'slot',dict(type=t,target=target,seq=seq,flags=flags,payload=payload.ljust(27,b'\0').hex()),name.replace('_',' '))
def answer(name,page,data,seq=0x42,index=0,flags=0):
    add(name,'answer',dict(page=page,index=index,flags=flags,seq=seq,uid16=crc16(UID),data=data.ljust(24,b'\0').hex()),name.replace('_',' '))
def update(name,opcode,data,index=0):
    add(name,'update',dict(opcode=opcode,session=7,index=index,flags=0,data=data.ljust(32,b'\0').hex(),time=0xabcdef,**{'pass':0}),name.replace('_',' '))

add('header_normal','header',dict(flags=0,time=0xabcdef),'Normal wire VERSION 3 trailer')
add('header_return','header',dict(flags=1,time=0xabcdef),'Return-window trailer')
slot('slot_set_target19',2,19,0x42,struct.pack('>11H',0x8000,*([0x1234]*5),*([0xffff]*5))+bytes.fromhex('0003e8fffe'),0x60)
slot('slot_set_target239',2,239,0x43,bytes(27),0x20)
slot('slot_config_channel19',5,19,0x44,bytes((12,0x09,0xc4)))
slot('slot_config_domain1',5,255,0x45,bytes((33,0x05,0xdc)))
slot('slot_config_output1_source2',5,1,0x46,bytes((77,0,3)))
slot('slot_config_input',5,255,0x47,bytes((21,0x07,0xd0)))
slot('slot_light_def_base15',1,19,0x48,bytes.fromhex('12345003e800')+bytes(10)+bytes.fromhex('ffff'*5)+bytes((15,)))
fx=bytes((1,))+struct.pack('>i',-2)+bytes((128,64,0xff))+bytes.fromhex('ff000000ff000000ff')+bytes((255,))+struct.pack('>HHH',250,0,1200)+bytes((0x10,1,2))
slot('slot_pixel_fx_output1_all',11,0x1f,0x49,fx)
for name,page,part,bank,kind in [('outputs_bank0',1,0,0,0),('outputs_bank3',1,0,3,0),('acks_bank3',2,0,3,0),('cal_pwm_bank3',6,0,3,0),('cal_pixel_output1',6,1,1,1),('descriptor_part10',15,10,0,0),('identity_target',4,2,0,0),('pixel_output1',16,1,1,0),('pixel_fx_output1',17,0,1,0),('limits_domain1',18,0,0,1)]:
    slot('poll_'+name,7,255,0x42,bytes((0,1,page,0,part,kind,0,bank)))
out=struct.pack('>10H',1,2,3,4,5,100,200,300,400,500)+bytes((0,0x1f,0,1))
answer('answer_outputs_bank0',1,out)
answer('answer_outputs_bank3',1,out)
answer('answer_unsupported_bank',255,bytes(24))
answer('answer_limits_domain1',18,bytes((0,1,1,52))+struct.pack('>6H',2000,1500,1500,800,500,0)+bytes((6,0))+struct.pack('>H',1500)+bytes(4))
cfg=bytes((0,3))+struct.pack('>HHH',0,600,60)+bytes((3,))+struct.pack('>HHH',2000,1800,0x1234)+bytes((0,))+bytes(8)
answer('answer_pixel_output1',16,cfg)
answer('answer_diag_part2',13,bytes((0x0d,1,0,7)))
answer('answer_pixel_part0',16,bytes((2,2,0x19,1))+struct.pack('>6H',100,2,0,90,89,1)+bytes.fromhex('000140')+struct.pack('>H',3)+bytes(3))
config_records=bytes.fromhex('000c0bb8014d0003ff1507d0ff2105dc')
status=bytearray(24);status[21:23]=struct.pack('>H',crc16(config_records))
answer('answer_status_config_crc_targets',0,bytes(status))
current=bytearray(24);current[17]=130
answer('answer_lifetime_65a',12,bytes(current))
current=bytearray(24);current[0]=(32<<2)|1;current[5]=130
answer('answer_diag_trip_64a_dip_65a',13,bytes(current))
current=bytearray(24);current[19:21]=bytes.fromhex('ffff')
answer('answer_power_peak_saturated',8,bytes(current))
current=bytearray(24);current[17:19]=struct.pack('>H',2250);current[19:21]=struct.pack('>H',412)
answer('answer_energy_supply_z',11,bytes(current))
current=bytearray(24);current[16:18]=struct.pack('>h',32500)
answer('answer_health_dark_65a',10,bytes(current))
current=bytearray(24);current[7:9]=struct.pack('>h',32500)
answer('answer_pixel_cal_idle_65a',6,bytes(current))
slot('slot_config_pixel_coefficient_65a',5,0,0x50,bytes((68,))+struct.pack('>H',65000))

from format_cases import cases
fixtures=cases()
for row in fixtures:
    name=row['name']; blob=bytes.fromhex(row['descriptor_hex']); target=bytes.fromhex(row['target_hex'])
    add('descriptor_'+name,'descriptor',decode('descriptor',blob),'Synthetic format example, no production identity or ratings')
    add('target_'+name,'target',decode('target',target),'Immutable target tuple')
    add('boot_info_'+name,'boot_info',dict(version=3,target=target.hex(),geometry=row['geometry_hex'],entry_10ms=row['entry_10ms'],build_id='30313233343536'),'Protected QDB2 info block')
    identity0=UID+struct.pack('>I',0x000700ff)+bytes((3,target[0],target[7],3))+struct.pack('>HH',len(blob),int.from_bytes(blob[4:6],'big'))
    answer('identity_part0_'+name,4,identity0)
    answer('identity_part2_'+name,4,target+bytes.fromhex(row['geometry_hex'])+bytes((2,1,row['entry_10ms'],0,0)))
    answer('descriptor_last_'+name,15,blob[(row['parts']-1)*24:])

large=deepcopy(decode('descriptor',bytes.fromhex(fixtures[-2]['descriptor_hex'])))
large['records'].append(dict(type=200,value=bytes(160).hex()))
add('descriptor_over255_unknown_tlv','descriptor',large,'u16 length beyond 255 and host skip of unknown type 200')
extended=deepcopy(large);extended['records'][0]['value']+='aabbcc'
add('descriptor_known_extended','descriptor',extended,'Known LINK prefix plus appended tail must remain usable')
sense=dict(board=254,lights=0,flags=0,variant=0,records=[
    dict(type=2,value=struct.pack('>BB4H3B',15,1,500,4800,10000,0,8,3,0).hex()),
    dict(type=5,value=struct.pack('>BBBBBbH',0,1,0,255,1,0,16500).hex()),
    dict(type=8,value=b'Synthetic sense'.hex())])
add('descriptor_sense_10a_board','descriptor',sense,'10 A board can declare 16.5 A ADC full scale without changing transport')

image=dict(record_format=2,wire=3,descriptor_format=1,rev_min=0,rev_max=255,board=254,mcu=4,package=5,layout=4,abi=3,features=3,variant=201,min_bl=3,firmware=0x000700ff,length=503808,body_crc=0x12345678,load=0x08005000,entry=0x08005002,build_id=bytes(range(16)).hex())
add('image_v407_range','image',image,'64-byte image descriptor, full u8 revision range and >64 KiB body')
future=deepcopy(image);future.update(record_format=3,wire=4,descriptor_format=2)
add('image_future_app_formats','image',future,'ABI 3 accepts future app-owned record, wire and descriptor formats')
d=encode('image',image)
answer('answer_fw_update_confirmed',5,bytes((1,7,1,0))+struct.pack('>III',image['firmware'],image['length'],image['body_crc'])+d[60:64]+bytes(4))
update('update_announce0',0x81,d[:32]);update('update_announce1',0x81,d[32:],1)
update('update_data_index65535',0x82,bytes(range(32)),65535)
update('update_end',0x83,struct.pack('>II',0x12345678,503808))
update('update_select',0x87,UID+bytes.fromhex(fixtures[-2]['target_hex']))
update('update_status_poll',0x86,UID+bytes((0,1,2)))
answer('update_status_busy',0x86,bytes((1,2,0,0,7,2))+struct.pack('>HIIII',0,256,256,200,0x12345678),seq=7)
answer('update_status_ready',0x86,bytes((1,3,0,8,7,2))+struct.pack('>HIIII',0,256,256,0,0x12345678),seq=7)

# Partial bank telemetry and opaque assembly coverage.
for name,count,bank in [('nano2',2,0),('partial7',7,1),('local10',10,1)]:
    n=min(5,count-5*bank);mask=(1<<n)-1
    data=struct.pack('>10H',*list(range(1,n+1)),*([0]*(5-n)),*([100]*n),*([0]*(5-n)))+bytes((mask,mask,0,1))
    answer('answer_outputs_'+name,1,data)
    slot('poll_outputs_'+name,7,255,0x42,bytes((0,1,1,0,0,0,0,bank)))
    ack=bytearray(24)
    never=sum(7<<(3*i) for i in range(n,5))
    ack[18:21]=never.to_bytes(3,'big')
    answer('answer_acks_'+name,2,bytes(ack))
stats=bytes((0,1,3,29))+struct.pack('>10H',300,325,313,920,950,938,512,512,0,7)
answer('answer_link_stats',19,stats)
slot('poll_link_stats',7,255,0x42,bytes((0,1,19,2,0,0,0,0)))
from timing_cases import fixtures as timing_fixtures
for profile in timing_fixtures()['profiles']:
    ident=profile['id'];period=round(profile['bit_ns'])
    context=bytes((0,ident,3,15))+struct.pack('>9H',7,period-10,period+10,
            round(profile['bit_ns']-profile['one_ns'])-10,
            round(profile['bit_ns']-profile['zero_ns'])+10,12,0,0,200)+bytes(2)
    answer('answer_link_context_'+str(ident),19,context)
slot('poll_link_context',7,255,0x42,bytes((0,1,19,2,0,0,1,0)))
pixels=bytes(range(30));pc=crc16(pixels)
slot('pixel_begin',12,0,9,bytes((0,0,0))+struct.pack('>HH',len(pixels),pc))
slot('pixel_chunk0',12,0,9,bytes((1,0,0))+pixels[:24])
slot('pixel_chunk1',12,0,9,bytes((1,0,24))+pixels[24:])
slot('pixel_commit',12,0,9,bytes((2,0,30)))
p1_cases.add_cases(add,slot,answer,update)
answer('answer_link_stats_rate_request',19,stats[:2]+bytes((stats[2]|0x10,))+stats[3:])
p1_cases.write(vectors)
from codec import local_packet
local=[]
for kind,name in [(1,'slot_config_channel19'),(2,'answer_outputs_bank0'),(3,'update_data_index65535')]:
    payload=bytes.fromhex(next(v['hex'] for v in vectors if v['name']==name))
    local.append(dict(kind=kind,endpoint=3,txid=42,payload=payload.hex(),hex=local_packet(3,kind,42,payload).hex()))
for v in vectors:
    if not v['name'].startswith('l1_'):continue
    payload=bytes.fromhex(v['hex']);t=payload[0]&31
    ms=int.from_bytes(payload[28:30] if t==2 else payload[4:8],'big',signed=True)
    accepted=3000000000000;start=accepted+ms*1000;due=max(accepted,start)
    txid=40+len(local)
    local.append(dict(name=v['name'],kind=1,endpoint=3,txid=txid,payload=payload.hex(),
        hex=local_packet(3,1,txid,payload).hex(),accepted_us=accepted,phase_ms=ms,
        start_us=start,due_us=due,local_latency_us=40,local_jitter_us=50,latest_output_us=due+50))
(HERE/'local_vectors.json').write_text(json.dumps(local,indent=2)+'\n',encoding='utf-8',newline='\n')
(HERE/'local_vectors.h').write_text(p1_cases.local_header(local),encoding='utf-8',newline='\n')
(HERE/'vectors.json').write_text(json.dumps(dict(protocol='0.3.2-draft',wire=3,vectors=vectors),indent=2)+'\n',encoding='utf-8',newline='\n')
md=['# Wire-3 format vectors','', 'Generated by `gen_vectors.py`. All board examples are synthetic, ID 254.','']
for v in vectors: md += ['## '+v['name'],'',v['description'],'','```text',v['hex'],'```','']
(HERE/'vectors.md').write_text('\n'.join(md),encoding='utf-8',newline='\n')
timing=timing_fixtures()
(HERE/'vectors_timing.json').write_text(json.dumps(timing,indent=2)+'\n',encoding='utf-8',newline='\n')
print(f'Generated {len(vectors)} wire-3 vectors, {len(local)} local packets and four-profile timing bounds')

# These host documents have their own versioning and transport, not binary slots.
import hashlib
from host_formats import canonical, pages
def domain(i,parent,kind,limit,trip,enforce,v,ma,mw,quality,idle):
    return dict(id=i,parent=parent,kind=kind,lim_ma=limit,trip_ma=trip,enforce=enforce,v=v,ma=ma,mw=mw,meas=quality,idle_ma=idle)
def output(o,kind,dom,n,limit,coeff):
    return dict(o=o,kind=kind,dom=dom,n=n,c_ma=coeff,lim_ma=limit,model='configured',quality=6)
power65=dict(v=3,dom=[domain(0,15,1,65535,65535,1,4800,65000,3120000,5,0)],out=[output(0,'pwm',0,1,65000,[65000])])
power2=dict(v=3,dom=[domain(0,15,1,2250,3000,26,4800,633,30384,5,20),domain(1,0,2,2000,0,52,500,2000,10000,14,0),domain(2,0,2,3000,0,52,500,3000,15000,14,0)],out=[output(0,'pixel',1,600,2000,[600,600,600,0,0]),output(1,'pixel',2,600,3000,[1000,1000,1000,0,0])])
targets={r['name']:r for r in fixtures}
def setup_board(name):
    row=targets[name]
    return dict(uid='000102030405060708090a0b',port=0,position=0,target=row['target_hex'],desc_crc=row['descriptor_crc'],domains=[dict(id=0,lim_ma=2250)],channels=[],outputs=[],config=[])
analog=setup_board('analog20');analog['domains'].append(dict(id=1,lim_ma=3000))
analog['channels']=[dict(i=i,dom=1,lim_ma=3000 if i<10 else 5000) for i in range(20)]
analog['channels'][19]['lim_ma']=2500
analog['config']=[dict(target=19,id=12,value=2500),dict(target=255,id=21,value=2250)]
digital=setup_board('dual_pixel');digital['domains'] += [dict(id=1,lim_ma=2000),dict(id=2,lim_ma=3000)]
digital['outputs']=[dict(o=0,dom=1,n=600,source=0,lim_ma=3000),dict(o=1,dom=2,n=600,source=3,lim_ma=5000)]
digital['config']=[dict(target=0,id=77,value=0),dict(target=1,id=77,value=3)]
unknown=setup_board('single_pixel');unknown.update(uid='101112131415161718191a1b',position=1)
unknown['domains'].append(dict(id=1,lim_ma=65535))
unknown['outputs']=[dict(o=0,dom=1,n=600,source=0,lim_ma=65535)]
host=[]
for name,kind,value in [('power_65a','P3',power65),('power_two_outputs','P3',power2),('setup_analog20','S2',dict(schema='S2',boards=[analog])),('setup_dual_pixel','S2',dict(schema='S2',boards=[digital,unknown]))]:
    body=canonical(kind,value)
    host.append(dict(name=name,kind=kind,value=value,canonical=body,length=len(body),sha256=hashlib.sha256(body.encode('ascii')).hexdigest(),pages=pages(kind,value)))
(HERE/'host_vectors.json').write_text(json.dumps(dict(vectors=host),indent=2)+'\n',encoding='utf-8',newline='\n')
print('Generated 4 power JSON v3 / setup S2 vectors with <=255-character pages')
