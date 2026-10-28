# SPDX-License-Identifier: MIT
"""Host-only reference validation and paging for power JSON v3 / setup S2."""
import hashlib
import json
from pathlib import Path
from jsonschema import Draft202012Validator

DOCS=Path(__file__).resolve().parents[1]/'docs/schemas'
SCHEMAS={'P3':'power-v3.schema.json','S2':'setup-s2.schema.json'}
PAGE_HEADER=87  # tag:sha256:part4:count4:length8:
CHUNK=255-PAGE_HEADER


def require(ok, message):
    if not ok: raise ValueError(message)


def unique(rows, key):
    values=[key(r) for r in rows]
    require(len(values)==len(set(values)), 'duplicate identity')


def validate(kind, value):
    schema=json.loads((DOCS/SCHEMAS[kind]).read_text())
    errors=list(Draft202012Validator(schema).iter_errors(value))
    require(not errors, errors[0].message if errors else '')
    if kind=='P3':
        domains={d['id']:d for d in value['dom']}
        unique(value['dom'],lambda d:d['id'])
        require(not domains or 0 in domains and domains[0]['parent']==15,'input root missing')
        for d in domains.values():
            seen=set();node=d
            while node['id']!=0:
                require(node['id'] not in seen and node['parent'] in domains,'domain cycle/reference')
                seen.add(node['id']);node=domains[node['parent']]
        unique(value['out'],lambda o:(o['kind'],o['o']))
        require(value['dom']==sorted(value['dom'],key=lambda d:d['id']),'domain order')
        require(value['out']==sorted(value['out'],key=lambda o:(o['kind'],o['o'])),'output order')
        for o in value['out']:
            require(o['dom'] in domains,'output domain absent')
            require(len(o['c_ma'])==(5 if o['kind']=='pixel' else o['n']),'coefficient count')
            require((o['o']<=14 if o['kind']=='pixel' else o['o']%5==0 and o['n']<=5 and o['o']+o['n']<=240),'output index/range')
    else:
        unique(value['boards'],lambda b:(b['port'],b['position']))
        require(value['boards']==sorted(value['boards'],key=lambda b:(b['port'],b['position'])),'board order')
        unique([b for b in value['boards'] if b['uid'] is not None],lambda b:b['uid'])
        for b in value['boards']:
            target=bytes.fromhex(b['target'])
            require(1<=target[0]<=254,'invalid target board')
            for field,key in [('domains','id'),('channels','i'),('outputs','o')]:
                unique(b[field],lambda r:r[key])
                require(b[field]==sorted(b[field],key=lambda r:r[key]),'indexed array order')
            domains={d['id'] for d in b['domains']}
            require(not domains or 0 in domains,'root limit absent')
            require(all(x['dom'] in domains for x in b['channels']+b['outputs']),'domain reference')
            require(sorted(x['i'] for x in b['channels'])==list(range(len(b['channels']))),'channel indices')
            require(sorted(x['o'] for x in b['outputs'])==list(range(len(b['outputs']))),'output indices')
            unique(b['config'],lambda c:(c['target'],c['id']))
            require(b['config']==sorted(b['config'],key=lambda c:(c['target'],c['id'])),'CONFIG order')
            for c in b['config']:
                scope='channel' if 11<=c['id']<=20 else 'output' if 64<=c['id']<=127 else 'board'
                require(c['target']==255 if scope=='board' else c['target']<len(b['channels'] if scope=='channel' else b['outputs']), 'CONFIG scope/target')
                expected=None
                if c['id']==21:
                    require(0 in domains,'CONFIG input domain absent')
                    expected=next(d['lim_ma'] for d in b['domains'] if d['id']==0)
                if 33<=c['id']<=46:
                    matches=[d for d in b['domains'] if d['id']==c['id']-32]
                    require(len(matches)==1,'CONFIG domain absent');expected=matches[0]['lim_ma']
                if c['id']==12: expected=b['channels'][c['target']]['lim_ma']
                if c['id'] in (66,69,77): expected=b['outputs'][c['target']][{66:'n',69:'lim_ma',77:'source'}[c['id']]]
                require(expected is None or expected==c['value'],'CONFIG/applied snapshot disagreement')
    return value


def canonical(kind, value):
    validate(kind,value)
    return json.dumps(value,ensure_ascii=True,sort_keys=True,separators=(',',':'),allow_nan=False)


def pages(kind, value):
    body=canonical(kind,value)
    count=(len(body)+CHUNK-1)//CHUNK
    require(0<count<=65535 and len(body)<=0xffffffff,'paging size')
    digest=hashlib.sha256(body.encode('ascii')).hexdigest()
    return [f'{kind}:{digest}:{i:04x}:{count:04x}:{len(body):08x}:'+body[i*CHUNK:(i+1)*CHUNK] for i in range(count)]


def reassemble(kind, parts):
    require(bool(parts),'empty transfer')
    chunks={};identity=None
    for part in parts:
        require(isinstance(part,str) and part.isascii() and PAGE_HEADER<len(part)<=255,'page length/ASCII')
        fields=part.split(':',5)
        require(len(fields)==6 and fields[0]==kind,'page type/header')
        tag,digest,index,count,length,chunk=fields
        require(len(digest)==64 and len(index)==4 and len(count)==4 and len(length)==8,'header widths')
        require(all(c in '0123456789abcdef' for c in digest+index+count+length),'hex header')
        i,n,size=int(index,16),int(count,16),int(length,16)
        require(0<=i<n<=65535 and n==(size+CHUNK-1)//CHUNK,'page indices/count')
        require(len(chunk)==min(CHUNK,size-i*CHUNK),'chunk length')
        key=(digest,n,size)
        require(identity is None or identity==key,'mixed transfer')
        identity=key
        require(i not in chunks or chunks[i]==chunk,'conflicting duplicate')
        chunks[i]=chunk
    digest,n,size=identity
    require(len(chunks)==n,'missing page')
    body=''.join(chunks[i] for i in range(n))
    require(len(body)==size and hashlib.sha256(body.encode('ascii')).hexdigest()==digest,'length/digest')
    def no_duplicates(pairs):
        result={}
        for k,v in pairs:
            require(k not in result,'duplicate JSON key');result[k]=v
        return result
    value=json.loads(body,object_pairs_hook=no_duplicates)
    require(canonical(kind,value)==body,'noncanonical payload')
    return value
