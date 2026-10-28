# SPDX-License-Identifier: MIT
"""Synthetic encoding cases, never production board definitions or firmware inputs."""
import struct
from codec import encode

def record(t, data): return dict(type=t, value=data.hex())
def link(port=0, kind=1):
    pulse=kind in (1,5)
    return record(1, struct.pack('>BBBHBHBBBBHBB', port, 1 if not pulse else 39,
                  4 if pulse else 0, 300 if pulse else 0, 24 if pulse else 0,
                  500 if pulse else 0, 80 if pulse else 0, 7 if pulse else 0,
                  kind, int(pulse), 48, 1 if pulse else 2, 0) +
                  (bytes((15,)) if pulse else struct.pack('>BHH',0,40,50)))
def domain(i=0, parent=15, kind=1):
    return record(2, struct.pack('>BBHHHHBBB', i<<4|parent,kind,500,4800,7000,10000,8,0,100))
def bank(first,count):
    return record(3, struct.pack('>BBBBHHBHB',first,count,0,0,1800,6000,8,250,0))
def pixel(i,port):
    data=bytes((i,0,4,35))+struct.pack('>9H',6000,2000,1500,1200,1250,400,127,2000,60)
    data+=bytes((8,3))+struct.pack('>HHHB',1<<port,500,500,port)
    return record(4,data)
def cases():
    result=[]
    for name,channels,kinds,pixels in [('nano2',2,[1],0),('partial7',7,[1],0),
            ('local10',10,[2,3,4],0),('analog20',20,[1],0),('dual_pixel',0,[2,3],2),
            ('single_pixel',0,[1],1)]:
        records=[link(i,k) for i,k in enumerate(kinds)]+[domain()]
        records += [bank(i,min(5,channels-i)) for i in range(0,channels,5)]
        records += [pixel(i,min(i,len(kinds)-1)) for i in range(pixels)]
        if pixels: records += [record(7,struct.pack('>HHIIBB',12000,12000,200000,60000,pixels,1))]
        records += [record(8,('Format test '+name).encode())]
        descriptor=dict(board=254,lights=channels,flags=1,variant=201,records=records)
        target=dict(board=254,mcu=4,package=5,layout=4,abi=3,features=3,variant=201,
                    revision=0,slot_base=0x08005000,slot_size=503808)
        blob=encode('descriptor',descriptor)
        result.append(dict(name=name,descriptor=descriptor,target=target,descriptor_hex=blob.hex(),
                           target_hex=encode('target',target).hex(),parts=(len(blob)+23)//24,
                           descriptor_crc=int.from_bytes(blob[4:6],'big'),geometry_hex='8c1e64',entry_10ms=10))
    return result
