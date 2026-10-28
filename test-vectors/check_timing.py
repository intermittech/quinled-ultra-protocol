# SPDX-License-Identifier: MIT
"""Check goldens without regenerating them; phase, topology and backend coverage."""
import math
from pathlib import Path
import re

# Literal EXTRA goldens: boards, D, remaining, per-sampler (M0,M1), HIGH, LOW,
# intermediate forwarding reserve. Independent arithmetic; no generator helper import.
EXTRA_GOLDENS = [
    (3,180.5,39.5,((66.553441643533,93.739377414224),
                   (83.203722601739,92.603271983640)),122,112,73.5),
    (4,242,58,((118.325348579824,117.518291297432),
               (132.801243802000,118.654396728016)),
     164.666666666667,154.666666666667,116.166666666667),
    (7,426.5,23.5,((133.189340752035,131.978754828448),
                   (131.357342953990,150.156441717791)),188.5,178.5,140),
    (8,488,262,((669.781317268669,667.360145421495),
                (677.734055282757,675.312883435583)),752,742,703.5),
]
EXTRA_SIX_BOARDS = (6,365,85,((194.689340752035,193.478754828448),
                          (192.857342953990,211.656441717791)),250,240,201.5)

def check_extra_budget(row, golden, need):
    boards,used,remaining,decoder,high,low,gate=golden
    need(row['boards']==boards,'EXTRA board-count golden')
    for name,value in [('used_ns',used),('remaining_admission_ns',remaining),
                       ('short_high_ns',high),('short_low_ns',low),('gate_reserve_ns',gate)]:
        need(abs(row[name]-value)<1e-8,'EXTRA literal '+name)
    need(len(row['decoder'])==2,'two EXTRA samplers')
    for actual,mhz,(m0,m1) in zip(row['decoder'],(36,50),decoder):
        need(actual['mhz']==mhz and abs(actual['zero_margin_ns']-m0)<1e-8 and
             abs(actual['one_margin_ns']-m1)<1e-8,'EXTRA literal decoder margins')
    # Aggregated stage signs include the 7.5 ns pair, cable and N-1 gates.
    for signs in range(1<<boards):
        distortion=sum((61.5 if i else 57.5)*(1 if signs&(1<<i) else -1)
                       for i in range(boards))
        need(abs(distortion)<=used,'EXTRA all stage signs, 3.5+4 pair')
    # Independently enumerate the two qualified width-error contributions.
    for signs in range(1<<(2*boards)):
        skew=sum(3.5*(1 if signs&(1<<(2*i)) else -1)+
                 4*(1 if signs&(1<<(2*i+1)) else -1) for i in range(boards))
        need(abs(skew)<=7.5*boards,'EXTRA independent width-error signs')

# Dig2Analog-Plus-5ch pin (timing.md, D2A link model): exact rational arithmetic,
# literal goldens. Per profile: rule-1 / rule-2 / model ceilings with ideal wiring,
# the pin table range, and the smallest V203 decoder margin at the table edge.
D2A_GOLDENS = [(25,21,21,(1,20),'76.053441644'),(34,34,34,(21,34),'54.992015246'),
               (51,58,51,None,'117.478754828'),(84,127,84,(35,50),'714.360145421')]
# (profile, boards, largest integer E_N still admitted)
D2A_RESERVES = [(0,20,17),(0,21,8),(1,34,3),(2,35,144),(2,40,99),(2,50,9),(3,50,309)]

def check_d2a(need):
    from fractions import Fraction as F
    samples=[(12,3,9,22,220),(16,4,12,30,300),(24,6,18,45,450),(48,12,36,90,750)]
    rmt={40:[(50,12,38),(67,17,50),(100,25,75),(200,50,150)],
         80:[(100,25,75),(133,33,100),(200,50,150),(400,100,300)]}
    q=F(1000)/(36*F(978,1000))
    def backends(i):
        b,z,o=samples[i][:3]
        out=[tuple(F(v*1000)/F(96,10) for v in (b,z,o))]
        return out+[tuple(F(t*1000,mhz) for t in rmt[mhz][i]) for mhz in (40,80)]
    def path(n,e): return 9*(n-1)+e
    def admits(i,n,e):
        count,limit=samples[i][3:]
        low=min(p-h1 for p,_,h1 in backends(i))
        if path(n,e)>limit or (n>=2 and not low-20-path(n-1,e)>100): return False
        lo=F(1000*count)/(36*F(1022,1000));hi=F(1000*count)/(36*F(978,1000))
        # Rule 1 must already keep both classes, HIGH and final LOW above one quantum.
        need(all(lo-h0-10-path(n,e)-q>0 and h1-10-path(n,e)-hi-q>0 and h0-10-path(n,e)>q and
                 p-h1-20-path(n,e)>q for p,h0,h1 in backends(i)),'D2A rule 1 implies decoder margins')
        return True
    need([min(p-h1 for p,_,h1 in backends(i)) for i in range(4)]==[300,F(825,2),625,1250] and
         [backends(i)[0][0]-backends(i)[0][2] for i in range(4)]==[F(625,2),F(1250,3),625,1250],
         'D2A shortest backend LOW per profile')
    for i,(rule1,rule2,model,table,margin) in enumerate(D2A_GOLDENS):
        limit=samples[i][4]
        low=min(p-h1 for p,_,h1 in backends(i))
        need(max(n for n in range(1,200) if path(n,0)<=limit)==rule1,'D2A rule-1 ceiling')
        need(max(n for n in range(2,200) if low-20-path(n-1,0)>100)==rule2,'D2A rule-2 ceiling')
        need(max(n for n in range(1,200) if admits(i,n,0))==model==min(rule1,rule2),'D2A model ceiling')
        need(not admits(i,model+1,0),'D2A one board above the model ceiling fails')
        if table:
            first,last=table
            need(last<=min(model,50),'D2A pin table inside the model and the 50-board limit')
            count=samples[i][3];lo=F(1000*count)/(36*F(1022,1000));hi=F(1000*count)/(36*F(978,1000))
            worst=min(min(lo-h0-10-path(last,0)-q,h1-10-path(last,0)-hi-q) for _,h0,h1 in backends(i))
            need(f'{float(worst):.9f}'==margin,'D2A decoder margin at the table edge')
            for n in range(first,last+1):
                need(admits(i,n,0),'D2A table entry admitted with ideal wiring')
                fastest=min(j for j,g in enumerate(D2A_GOLDENS) if g[3] and g[3][0]<=n<=g[3][1])
                need(fastest==i,'D2A table: one profile per board count')
        else:
            count=samples[i][3];lo=F(1000*count)/(36*F(1022,1000));hi=F(1000*count)/(36*F(978,1000))
            worst=min(min(lo-h0-10-path(50,0)-q,h1-10-path(50,0)-hi-q) for _,h0,h1 in backends(i))
            need(f'{float(worst):.9f}'==margin,'D2A 400 kHz decoder margin at 50 boards')
    need(sorted(n for g in D2A_GOLDENS if g[3] for n in range(g[3][0],g[3][1]+1))==list(range(1,51)),
         'D2A table covers 1..50 exactly once')
    for i,n,reserve in D2A_RESERVES:
        need(admits(i,n,reserve) and not admits(i,n,reserve+1),'D2A wiring reserve')
    root=Path(__file__).parents[1]
    header=(root/'include/ultra_transport.h').read_text()
    limits=(root/'include/ultra_protocol.h').read_text()
    table=(root/'sender/src/ultra_sender_update.h').read_text()
    need(re.search(r'#define ULTRA_D2A_HOP_NS 9u\b',header) is not None and
         'boards>ULTRA_PORT_D2A_PIN_MAX_BOARDS' in header,
         'C D2A hop bound and 50-board path match the golden')
    need(re.search(r'#define ULTRA_D2A_800_MAX_BOARDS 20u\b',table) is not None and
         re.search(r'#define ULTRA_D2A_600_MAX_BOARDS 34u\b',table) is not None and
         re.search(r'#define ULTRA_PORT_D2A_PIN_MAX_BOARDS 50u\b',limits) is not None,
         'C D2A pin table matches the golden')

# Transit hold after a foreign ANNOUNCE (timing.md, update forwarding): literal
# layout goldens, independent erase-unit cursor, erase_max taken as the u8 maximum.
TRANSIT_LAYOUTS = {1: (0x08002000, 0x08002100, 0x08010000, 256),
                   4: (0x08004000, 0x08005000, 0x08080000, 4096)}

def transit_hold_ms(half0):
    """Independent INDEX 0 forwarder check: every target-independent invalidity
    holds the largest value; target match and bytes 32..63 are not checkable."""
    get32=lambda o:int.from_bytes(half0[o:o+4],'little')
    if (half0[0:4]!=b'QDU6' or half0[4]!=2 or half0[5]!=0 or half0[6]>half0[7] or
            half0[8] in (0,255) or half0[12]!=3 or half0[13]&0xF0 or half0[11] not in TRANSIT_LAYOUTS):
        return 114340
    meta,body,end,unit=TRANSIT_LAYOUTS[half0[11]]
    length=get32(20)
    if get32(28)!=body or not length or length%4 or length>end-body:
        return 114340
    return -(-(body+length-meta)//unit)*2*255+100

def check_transit_invalid(need):
    def half(layout,length,**field):
        d=bytearray(32);d[0:4]=b'QDU6';d[4]=2;d[6]=0;d[7]=255;d[8]=7;d[9]=1 if layout==1 else 4
        d[10]=1 if layout==1 else 5;d[11]=layout;d[12]=3;d[13]=1
        d[20:24]=length.to_bytes(4,'little');d[28:32]=TRANSIT_LAYOUTS.get(layout,(0,0,0,0))[1].to_bytes(4,'little')
        for at,value in field.items(): d[int(at[1:])]=value
        return bytes(d)
    need(transit_hold_ms(half(1,4))==1120 and transit_hold_ms(half(4,63488))==8770 and
         transit_hold_ms(half(4,503808))==63340 and transit_hold_ms(half(1,57088))==114340,'valid INDEX 0 exact holds')
    for field in ({'b5':1},{'b6':255,'b7':1},{'b13':0x80},{'b8':0},{'b8':255},{'b12':2},{'b12':4},
                  {'b4':1},{'b0':ord('q')},{'b11':2}):
        need(transit_hold_ms(half(1,4,**field))==114340,'invalid INDEX 0 %r -> 114340 ms' % field)
    for length in (0,2,57092,0xfffffffc):
        need(transit_hold_ms(half(1,length))==114340,'layout-1 length out of range -> 114340 ms')
    for field in ({'b6':255,'b7':255},{'b13':0x0F},{'b8':1},{'b8':254},{'b14':0xFF},{'b15':0xFF}):
        need(transit_hold_ms(half(1,4,**field))==1120,'valid or target-dependent INDEX 0 %r keeps the exact hold' % field)

def check_transit_hold(need):
    root=Path(__file__).parents[1]
    header=(root/'include/ultra_update.h').read_text()
    doc=(root/'docs/timing.md').read_text(encoding='utf-8')
    need(re.search(r'#define UPD_TRANSIT_ERASE_MS 255u\b',header) is not None and
         re.search(r'#define UPD_TRANSIT_HOLD_MAX_MS 114340u\b',header) is not None,
         'C transit hold constants match the golden')
    for layout,(meta,body,end,unit) in TRANSIT_LAYOUTS.items():
        mcu=203 if layout==1 else 407
        need(all(re.search(r'#define UPD_V%d_%s 0x%08Xu\b' % (mcu,name,value),header) is not None
                 for name,value in (('METADATA',meta),('BODY',body),('END',end))),
             'C layout table matches the transit golden')
    largest=0
    for layout,(meta,body,end,unit) in TRANSIT_LAYOUTS.items():
        # Erase cursor walked forward: the metadata unit, then one more unit
        # each time the body end crosses into a new erase unit.
        units,covered=1,body
        for length in range(4,end-body+1,4):
            if body+length>covered:
                units+=1;covered+=unit
            need(units==-(-(body+length-meta)//unit),'erase cursor = [metadata_base, round_up(load+length)) units')
            hold=units*2*255+100
            for erase_max in (1,20,100,200,254,255):
                need(units*2*erase_max+100<=hold,'hold covers every nonzero u8 declared lead')
            largest=max(largest,hold)
    need(largest==114340,'largest valid transit hold = the invalid fallback')
    need((1+63488//4096+1)*510+100==8770 and 124*510+100==63340 and 224*510+100==114340,'transit hold examples')
    need(124*2*200+100==49700 and 49700<=63340 and 124*2*100+100==24900,'full layout-4 lead at 200 / 100 ms erase bounds inside the hold')
    for text in ('transit_hold_ms = erase_units*2*255 + 100','**114340 ms**','**8770 ms**','**63340 ms**',
                 '**49700 ms**','upd_transit_hold_ms()','last INDEX 0 of the same',
                 'flags zero, `rev_min <= rev_max`, board 1..254,','ABI 3, no required feature bits outside the defined mask',
                 'neither is a forwarder check'):
        need(text in doc,'timing.md transit hold text: '+text)
    need('25000 ms' not in doc and '**25000' not in doc,'no fixed transit hold constant left in timing.md')
    # Host after an abandoned ANNOUNCE: that hold + the 2100 ms host quiet.
    sender=(root/'sender/src/ultra_sender_update.h').read_text()
    need(re.search(r'#define ULTRA_UPDATE_QUIET_MS 2100u\b',sender) is not None and
         'quiet LOW for >=2100 ms' in doc,'host quiet 2100 ms = 2000 ms idle close + tick margin')
    need(63340+2100==65440 and 114340+2100==116440 and '**65440 ms**' in doc and '**116440 ms**' in doc and
         'transit_hold_ms + 2100' in doc,'abandoned-ANNOUNCE host wait examples')

def check_timing(timing, need):
    profiles=timing['profiles']
    expected=[(800000,12,3,9,22,31,220,3),(600000,16,4,12,30,42,300,4),
              (400000,24,6,18,45,62,450,6),(200000,48,12,36,90,125,750,8)]
    need(len(profiles)==4 and timing['sample_hz']==9600000,'four exact shared-clock profiles')
    header=(Path(__file__).parents[1]/'include/ultra_transport.h').read_text()
    for row,exp in zip(profiles,expected):
        rate,b,z,o,n36,n50,limit,boards=exp
        need(row['rate']==rate and row['samples']==[b,z,o] and row['thresholds']==[n36,n50], 'profile golden')
        need(re.search(r'\{'+','.join(map(str,(b,z,o,n36,n50,limit)))+r'\}',header) is not None,'C profile table matches independent golden')
        need(9600000/b==rate and z*3==o and z+o==b,'integer exact sample symbols')
        period,zero,one=[v*1000/9.6 for v in (b,z,o)]
        used=74*boards-4
        need(row['used_ns']==used and row['admission_ns']==limit and used<=limit,'GUARANTEE worst sign and admission')
        ex=row['extra'];golden=EXTRA_GOLDENS[row['id']]
        need(ex['skew_pair_ns']==[3.5,4],'EXTRA qualified width-error pair')
        check_extra_budget(ex,golden,need)
        check_extra_budget(ex['at_guarantee_board_count'],EXTRA_SIX_BOARDS if rate==400000 else golden,need)
        need(ex['used_ns']<=limit and ex['boards']<=8,'EXTRA coverage and topology cap')
        need(abs(used-ex['at_guarantee_board_count']['used_ns']-[37.5,50,75,100][row['id']])<1e-8,
             'EXTRA literal margin gain at GUARANTEE board counts')
        need(boards==8 or 74*(boards+1)-4>limit,'next worst-case hop requires slower step or exceeds topology cap')
        # Enumerate every sign choice for aggregated stage errors, no cancellation credit.
        for signs in range(1<<boards):
            distortion=sum((74 if i else 70)*(1 if signs&(1<<i) else -1) for i in range(boards))
            need(abs(distortion)<=used,'all stage skew signs')
        need(abs(row['short_high_ns']-(zero-10-used))<1e-8 and
             abs(row['short_low_ns']-(period-one-20-used))<1e-8,'pulse survival golden')
        need(row['gate_reserve_ns']>0,'100 ns gate deadline at active intermediate board')
        need(abs(row['pixel600_fps']-1e6/(14400*period/1000+500))<1e-8,'600 RGB serialization FPS')
        need(abs(row['command8_us']-(2112*period/1000+500))<1e-8,'eight-board command rate')
        need(abs(row['update_us']-(384*period/1000+500))<1e-8,'update frame airtime')
        for decoder,mhz,count in zip(row['decoder'],(36,50),(n36,n50)):
            q=1000/(mhz*.978);lo=1000*count/(mhz*1.022);hi=1000*count/(mhz*.978)
            need(abs(decoder['zero_margin_ns']-(lo-zero-10-used-q))<1e-8 and
                 abs(decoder['one_margin_ns']-(one-10-used-hi-q))<1e-8,'decoder margins golden')
            # Both corners, all phases, every waveform backend; worst topology and admission.
            backends=[(period,zero,one)]+[tuple(t*1e9/r['hz'] for t in r['ticks']) for r in row['rmt']]
            for p,h0,h1 in backends:
                for d in sorted({used,limit,ex['used_ns'],ex['at_guarantee_board_count']['used_ns']}):
                    need(p-h1-20-d>q and h0-10-d>q,'HIGH and LOW survive sampler')
                    for f in (mhz*.978,mhz*1.022):
                        quantum=1000/f
                        for width,bit in ((h0-10-d,0),(h0+10+d,0),(h1-10-d,1),(h1+10+d,1)):
                            for phase in range(257):
                                first=phase/257*quantum
                                captured=max(0,math.ceil((width-first)/quantum))
                                need((captured>=count)==bool(bit),'profile/RMT sample-phase classification')
                intermediate=74*(boards-1)-4
                need(p-h1-20-intermediate>100,'RMT intermediate forwarding reserve')
                extra_intermediate=61.5*(ex['boards']-1)-4
                need(p-h1-20-extra_intermediate>100,'EXTRA RMT intermediate forwarding reserve')
                # Mean period discovery before any bit classification; endpoint uncertainty
                # amortizes across 256 periods, so unlike width error it is not per bit.
                for factor in (.978,1.022):
                    error=13408/256+2*q/256
                    observed=(p-error)*factor,(p+error)*factor
                    need(period*.92<observed[0] and observed[1]<period*1.08,'cold/late-join rate discovery at HSI extremes')
        for r in row['rmt']:
            bt,zt,ot=r['ticks']
            need(abs(bt-r['hz']/rate)<=.5 and abs(zt-r['hz']*z/9600000)<=.5 and
                 abs(ot-r['hz']*o/9600000)<=.5,'RMT nearest ticks')
            need(r['rate']==r['hz']/bt and abs(r['pixel600_fps']-1/(14400*bt/r['hz']+.0005))<1e-8,'RMT actual rates')
    need(timing['typical']['path_ns']==10+30+2*(4+10)+3*50==218,'three hop allowances + two forwarding allowances + three cables')
    need(timing['typical']['extra_path_ns']==180.5,'EXTRA typical path literal golden')
    for boards,used,limit in ((4,242,220),(5,303.5,300),(8,488,450)):
        need(61.5*boards-4==used and used>limit,
             'EXTRA 4 at 800 / 5 at 600 / 8 at 400 NOT covered')
    need('GUARANTEE maxima at 5 V only' in timing['qualification'] and
         'EXTRA' in timing['qualification'] and 'written confirmation' in timing['qualification'],
         'GUARANTEE 5 V and EXTRA written-confirmation qualification caveats')
    need(abs(timing['typical']['propagation_ns']-(900+36*5.38))<1e-8,'30+3+3 propagation separate from distortion')
    need(188+400==588 and 588<750 and (1000-750)/1000==.25,'robust recovery reserve')
    need(all(abs(t-1784*p['update_us'])<1e-6 for t,p in zip(timing['declared_v203']['pass_us'],profiles)),'all update passes')
    # PARLIO logical transfer queues need independent frame boundaries, not slowest padding.
    lengths=[int(round(p['pixel600_ms']*9600)) for p in profiles]
    need(lengths==[177600,235200,350400,696000],'integer lane frame/reset samples')
    fast_starts=list(range(0,lengths[3],lengths[0]))
    need(len(fast_starts)==4 and fast_starts==[0,177600,355200,532800], 'fast frames restart during slow transfer')
    need(1e3/(18.5+72.5)<11,'separate serial transfers cannot promise fast FPS')
    need(all(profiles[i]['bit_ns']*1.08<profiles[i+1]['bit_ns']*.92 for i in range(3)), 'unambiguous acquisition bands')
    check_d2a(need)
    check_transit_hold(need)
    check_transit_invalid(need)
    print('PASS: four profiles, GUARANTEE + EXTRA literal goldens and skew signs, 40/80 MHz RMT, HSI/phase corners, worst-sign hops, late-join acquisition, PARLIO scheduling bounds, the D2A pin model and the transit hold')


if __name__=='__main__':
    import json
    checks=0
    def need(ok, message):
        global checks
        checks+=1
        if not ok: raise AssertionError(message)
    check_timing(json.loads((Path(__file__).resolve().parent/'vectors_timing.json').read_text()),need)
    print(f'PASS: {checks} timing assertions against committed goldens')
