# SPDX-License-Identifier: MIT
"""Independent host-only reference codecs for wire-3 skeleton vectors."""
import struct

KNOWN_PREFIX = {1:16,2:13,3:12,4:31,5:8,6:12,7:14,8:1}
NORMAL_VERSION = 3
UPD_HDR_VERSION = 3  # Frozen with ABI 3; never inherited from NORMAL_VERSION.


def build_header(version, flags, time):
    d=struct.pack('>HBI',0x5ad2,version<<4|flags,time)
    return d+bytes((crc8(d),))


def parse_header(version, d):
    if len(d)!=8 or d[:2]!=b'\x5a\xd2' or d[2]>>4!=version or crc8(d[:7])!=d[7]:
        raise ValueError('header')
    return dict(flags=d[2]&15,time=int.from_bytes(d[3:7],'big'))


def descriptor_prefixes(blob):
    """Host semantic view: skip unknown types and appended fields, retain raw elsewhere."""
    return [(r['type'], bytes.fromhex(r['value'])[:KNOWN_PREFIX[r['type']]])
            for r in decode('descriptor', blob)['records'] if r['type'] in KNOWN_PREFIX]


def erase_cursor(metadata, load, length, slot_size, erase_unit, program_unit=256):
    """Host-only bounds oracle for complete minimum-unit erase coverage."""
    if not all(isinstance(x, int) and 0 <= x <= 0xffffffff for x in
               (metadata, load, length, slot_size, erase_unit, program_unit)):
        raise ValueError('u32 bounds')
    if not erase_unit or erase_unit & (erase_unit-1) or not program_unit or program_unit & (program_unit-1):
        raise ValueError('geometry')
    if not length or length % 4 or length > slot_size or slot_size > 0xffffffff-load:
        raise ValueError('body bounds')
    if metadata % erase_unit or load % erase_unit or metadata >= load:
        raise ValueError('metadata/load alignment')
    for unit in (erase_unit, program_unit):
        if (length+unit-1)//unit*unit > slot_size:
            raise ValueError('rounded operation outside slot')
    end=load+(length+erase_unit-1)//erase_unit*erase_unit
    return list(range(metadata,end,erase_unit))


def validate_descriptor(blob):
    """Host format/capability checks; no product lookup or firmware-side parser."""
    f=decode('descriptor',blob)
    def require(ok, why):
        if not ok: raise ValueError(why)
    require(1<=f['board']<=254 and f['lights']<=240,'identity/count')
    groups={t:[] for t in KNOWN_PREFIX};keys=set();previous=(-1,-1)
    for r in f['records']:
        t=r['type'];v=bytes.fromhex(r['value'])
        if t not in groups: continue
        ident=0 if t in (7,8) else v[0]>>4 if t==2 else v[0]
        key=(t,ident)
        require(key not in keys and key>=previous,'duplicate/order')
        keys.add(key);previous=key;groups[t].append(v)
    require(len(groups[8])==1,'product name')
    name=groups[8][0].decode('utf-8')
    require(len(groups[8][0])<=63 and all(ord(c)>=32 and ord(c)!=127 for c in name),'name text')
    links={v[0]:v for v in groups[1]};domains={v[0]>>4:v for v in groups[2]}
    for v in links.values():
        require(v[0]<16 and v[15]==0,'link bounds')
        if v[11]==2:
            require(v[10] in (1,5) and not any(v[1:10]) and not any(v[12:]),'raw ingress fields')
            continue
        require(int.from_bytes(v[12:14],'big')>=32,'link MTU')
        require(not (v[1]&8 and v[1]&6),'chain end forwards or relays')
        if v[10] in (2,3,4):
            require(v[11]==0 and v[14] in (0,2) and not any(v[2:10]) and not v[1]&62,'local physical fields')
            require(bool(v[1]&1)==(v[14]==2),'local return')
            require(len(v)==16 or v[16]==0,'local scheduling reserved byte')
        elif v[10] in (1,5):
            require(v[11]==1 and v[14] in (0,1) and int.from_bytes(v[6:8],'big')>=500,'pulse profile')
            require(bool(v[1]&1)==bool(v[2])==(v[14]==1),'scheduled return')
        if v[11]==1 and len(v)>16:
            require(v[16]&1 and not v[16]&240,'adaptive profile mask')
        if f['board']==1:
            require(v[10]==5 and v[1]&4 and not v[1]&11 and v[14]==0 and v[2]==0 and v[8]==0,
                    'Dig2Analog always forward, no return/relay/chain-end')
    if domains:
        require(0 in domains and domains[0][0]==15 and domains[0][1]==1,'root supply')
    for ident,v in domains.items():
        require(ident<15 and (ident==0 or v[0]&15!=15),'domain identity')
        seen=set();cur=ident
        while cur!=15:
            require(cur in domains and cur not in seen,'domain reference/cycle')
            seen.add(cur);cur=domains[cur][0]&15
    count=0
    for v in groups[3]:
        require(v[0]==count and v[0]%5==0 and 1<=v[1]<=5 and v[2] in domains,'PWM bank')
        count+=v[1]
    require(count<=240 and (not count or f['lights']>0),'PWM count')
    for i,v in enumerate(groups[4]):
        require(v[0]==i and i<15 and v[1] in domains and v[30] in links,'pixel references')
        flags=v[3];source=v[23];mask=int.from_bytes(v[24:26],'big')
        require(bool(flags&1)==bool(source&2)==bool(mask),'pixel streaming')
        require(all(j in links for j in range(16) if mask&(1<<j)),'ingress')
        ctrl=links[v[30]]
        require(ctrl[11]!=2,'raw ingress is not control')
        require(not flags&16 or ctrl[1]&16,'marker')
        require(not flags&4 or ctrl[1]&12,'chain')
        require(not flags&32 or flags&1,'packet stream')
        require(not (mask&(1<<v[30])) or flags&32 or ctrl[1]&16,'raw shared stream')
        require(not int.from_bytes(v[16:18],'big')&0xff80,'chip mask')
        for off,bpp in ((6,3),(8,4),(10,5)):
            require(int.from_bytes(v[off:off+2],'big')*bpp<=int.from_bytes(v[4:6],'big'),'frame capacity')
        cur=v[1]
        while cur!=15:
            d=domains[cur];rating=int.from_bytes(d[6:8],'big')
            require(not (source&2 and d[1] in (2,3,4) and rating not in (0,65535)) or d[10]&4,'converter protection')
            cur=d[0]&15
    for v in groups[5]: require(v[2] in domains,'sense domain')
    require(bool(groups[7])==bool(groups[4]),'pixel resources')
    if groups[7]:
        v=groups[7][0]
        require(int.from_bytes(v[2:4],'big')<=int.from_bytes(v[:2],'big') and v[12]<=len(groups[4]),'resource pool')
    return f


def local_packet(endpoint, kind, txid, payload):
    if not 0<=endpoint<255 or kind not in (0,1,2,3) or len(payload)!={0:0,1:32,2:32,3:48}[kind]:
        raise ValueError('local packet fields')
    if kind in (1,2): decode('slot' if kind==1 else 'answer',payload)
    return seal(b'\xa6\x31'+struct.pack('>BBHH',endpoint,kind,txid,len(payload))+payload)


def parse_local_packet(data):
    if len(data)<10 or data[:2]!=b'\xa6\x31': raise ValueError('packet magic/length')
    endpoint,kind,txid,n=struct.unpack('>BBHH',data[2:8])
    checked(data,n+10)
    if local_packet(endpoint,kind,txid,data[8:-2])!=data: raise ValueError('packet fields')
    return endpoint,kind,txid,data[8:-2]


def local_start_us(data, accepted_us):
    """New kind-1 command's logical start; dedup/capability admission is external."""
    _,kind,_,slot=parse_local_packet(data)
    if kind!=1 or not 0<=accepted_us<=0x7fffffffffffffff: raise ValueError('local acceptance')
    t=slot[0]&31
    ms=int.from_bytes(slot[28:30],'big',signed=True) if t==2 else (
        int.from_bytes(slot[4:8],'big',signed=True) if t==11 else 0)
    result=accepted_us+1000*ms
    if result>0x7fffffffffffffff: raise ValueError('local start overflow')
    return result


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8): c = ((c << 1) ^ (7 if c & 128 else 0)) & 255
    return c


def crc16(data):
    c = 65535
    for b in data:
        c ^= b << 8
        for _ in range(8): c = ((c << 1) ^ (0x1021 if c & 32768 else 0)) & 65535
    return c


def crc32(data):
    if len(data) % 4: raise ValueError('CRC32 requires whole words')
    c = 0xffffffff
    for (word,) in struct.iter_unpack('<I', data):
        c ^= word
        for _ in range(32): c = ((c << 1) ^ (0x04c11db7 if c & 0x80000000 else 0)) & 0xffffffff
    return c


def seal(data): return data + struct.pack('>H', crc16(data))
def checked(data, length):
    if len(data) != length or crc16(data[:-2]) != int.from_bytes(data[-2:], 'big'):
        raise ValueError('length/CRC16')


def encode(kind, f):
    if kind == 'header':
        return build_header(NORMAL_VERSION,f['flags'],f['time'])
    if kind == 'slot':
        payload = bytes.fromhex(f['payload'])
        if len(payload) != 27: raise ValueError('payload length')
        return seal(bytes((f['type'] | f['flags'], f['target'], f['seq'])) + payload)
    if kind == 'answer':
        data = bytes.fromhex(f['data'])
        if len(data) != 24: raise ValueError('answer data length')
        return seal(struct.pack('>BBBBH', 0xa5, f['page'], f['index'] << 4 | f['flags'], f['seq'], f['uid16']) + data)
    if kind == 'descriptor':
        body = bytes((f['board'], f['lights'], len(f['records']), f['flags'], f.get('variant',0)))
        for record in f['records']:
            v = bytes.fromhex(record['value'])
            body += bytes((record['type'], len(v))) + v
        if len(body) + 6 > 6144: raise ValueError('descriptor too long')
        return bytes((1, 11)) + struct.pack('>HH', len(body)+6, crc16(body)) + body
    if kind == 'target':
        return struct.pack('>BBBBBBBBII', *(f[k] for k in ('board','mcu','package','layout','abi','features','variant','revision','slot_base','slot_size')))
    if kind == 'image':
        d = b'QDU6' + bytes((2, 0, f['rev_min'], f['rev_max']))
        d += struct.pack('>BBBBBBBB', *(f[k] for k in ('board','mcu','package','layout','abi','features','variant','min_bl')))
        d += struct.pack('>5I', *(f[k] for k in ('firmware','length','body_crc','load','entry')))
        d += bytes((f['record_format'],f['wire'],f['descriptor_format'],0)) + bytes.fromhex(f['build_id']) + bytes(4)
        return d + struct.pack('>I', crc32(d))
    if kind == 'update':
        d = struct.pack('>BBHBB', *(f[k] for k in ('opcode','session','index','pass','flags'))) + bytes.fromhex(f['data'])
        if len(d) != 38: raise ValueError('update data length')
        return seal(d) + build_header(UPD_HDR_VERSION,9 if f['opcode']==0x86 else 8,f['time'])
    if kind == 'boot_info':
        return b'QDB2' + bytes((f['version'],)) + bytes.fromhex(f['target']+f['geometry']) + bytes((f['entry_10ms'],)) + bytes.fromhex(f['build_id'])
    raise ValueError('unknown kind')


def decode(kind, d):
    if kind == 'header':
        return parse_header(NORMAL_VERSION,d)
    if kind == 'slot':
        checked(d,32)
        return dict(type=d[0]&0x9f,flags=d[0]&0x60,target=d[1],seq=d[2],payload=d[3:30].hex())
    if kind == 'answer':
        checked(d,32)
        if d[0]!=0xa5: raise ValueError('answer sync')
        return dict(page=d[1],index=d[2]>>4,flags=d[2]&15,seq=d[3],uid16=int.from_bytes(d[4:6],'big'),data=d[6:30].hex())
    if kind == 'descriptor':
        if len(d)<11 or len(d)>6144 or d[:2]!=bytes((1,11)) or int.from_bytes(d[2:4],'big')!=len(d) or crc16(d[6:])!=int.from_bytes(d[4:6],'big'): raise ValueError('descriptor header')
        pos, records = 11, []
        lengths = KNOWN_PREFIX
        while pos<len(d):
            if pos+2>len(d): raise ValueError('truncated TLV prefix')
            t,n=d[pos:pos+2];pos+=2
            if pos+n>len(d) or t in lengths and n<lengths[t]: raise ValueError('TLV length')
            records.append(dict(type=t,value=d[pos:pos+n].hex()));pos+=n
        if len(records)!=d[8]: raise ValueError('record count')
        return dict(board=d[6],lights=d[7],flags=d[9],variant=d[10],records=records)
    if kind == 'target':
        return dict(zip(('board','mcu','package','layout','abi','features','variant','revision','slot_base','slot_size'),struct.unpack('>BBBBBBBBII',d)))
    if kind == 'image':
        if len(d)!=64 or d[:6]!=b'QDU6\x02\0' or d[39]!=0 or any(d[56:60]) or crc32(d[:60])!=int.from_bytes(d[60:],'big'): raise ValueError('image descriptor')
        f = dict(rev_min=d[6],rev_max=d[7])
        f.update(zip(('board','mcu','package','layout','abi','features','variant','min_bl'),struct.unpack('>BBBBBBBB',d[8:16])))
        f.update(zip(('firmware','length','body_crc','load','entry'),struct.unpack('>5I',d[16:36])))
        f.update(record_format=d[36],wire=d[37],descriptor_format=d[38])
        f['build_id']=d[40:56].hex()
        return f
    if kind == 'update':
        checked(d[:40],40)
        h=parse_header(UPD_HDR_VERSION,d[40:])
        f=dict(zip(('opcode','session','index','pass','flags'),struct.unpack('>BBHBB',d[:6])))
        if not 0x81<=f['opcode']<=0x87 or h['flags']!=(9 if f['opcode']==0x86 else 8): raise ValueError('update flags/type')
        f.update(data=d[6:38].hex(),time=h['time'])
        return f
    if kind == 'boot_info':
        if len(d)!=32 or d[:4]!=b'QDB2': raise ValueError('boot info')
        return dict(version=d[4],target=d[5:21].hex(),geometry=d[21:24].hex(),entry_10ms=d[24],build_id=d[25:].hex())
    raise ValueError('unknown kind')


def command(data):
    """Dispatch admission after container CRC validation; unknown commands ignored."""
    fields=decode('slot',data)
    if fields['type'] not in (0,1,2,5,6,7,8,9,11,12):
        raise ValueError('unknown command')
    return fields
