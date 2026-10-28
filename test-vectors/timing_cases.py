# SPDX-License-Identifier: MIT
"""Independent physical fixture arithmetic, not imported by public C helpers."""

PROFILES = [
    # rate, samples(bit,0,1), thresholds(36,50), admission, worst-length boards
    (800000, (12,3,9), (22,31), 220, 3),
    (600000, (16,4,12), (30,42), 300, 4),
    (400000, (24,6,18), (45,62), 450, 6),
    (200000, (48,12,36), (90,125), 750, 8),
]
# Explicit goldens, independently check C rounding against these.
RMT = {40000000: [(50,12,38),(67,17,50),(100,25,75),(200,50,150)],
       80000000: [(100,25,75),(133,33,100),(200,50,150),(400,100,300)]}
GUARANTEE_SKEW_NS = (10, 10)  # qualified width-error contributions; mixed-path guarantee
EXTRA_SKEW_NS = (3.5, 4)  # every stage meets the qualified-path timing bound
EXTRA_BOARDS = (3, 4, 7, 8)

def path(n, cables, skew_pair=GUARANTEE_SKEW_NS): return sum(skew_pair)*n+4*(n-1)+sum(cables)

def extra_budget(n, period, zero, one, thresholds, admission):
    used=path(n,[50]*n,EXTRA_SKEW_NS)
    decoder=[]
    for mhz,count in zip((36,50),thresholds):
        low=1000*count/(mhz*1.022);high=1000*count/(mhz*.978);q=1000/(mhz*.978)
        decoder.append(dict(mhz=mhz,zero_margin_ns=low-zero-10-used-q,
                            one_margin_ns=one-10-used-high-q))
    return dict(boards=n,used_ns=used,remaining_admission_ns=admission-used,
                decoder=decoder,short_high_ns=zero-10-used,
                short_low_ns=period-one-20-used,
                gate_reserve_ns=period-one-20-path(n-1,[50]*(n-1),EXTRA_SKEW_NS)-100)

def fixtures():
    profiles=[]
    for ident,(rate,samples,thresholds,admission,n) in enumerate(PROFILES):
        period,zero,one=[v*1000/9.6 for v in samples]
        used=path(n,[50]*n)
        row=dict(id=ident,rate=rate,samples=list(samples),thresholds=list(thresholds),
                 admission_ns=admission,boards=n,used_ns=used,bit_ns=period,
                 zero_ns=zero,one_ns=one,short_high_ns=zero-10-used,
                 short_low_ns=period-one-20-used,
                 gate_reserve_ns=period-one-20-path(n-1,[50]*(n-1))-100,
                 pixel600_ms=14400/rate*1000+.5,pixel600_fps=1/(14400/rate+.0005),
                 command8_us=2112/rate*1e6+500,update_us=384/rate*1e6+500,
                 decoder=[],rmt=[])
        for mhz,count in zip((36,50),thresholds):
            low=1000*count/(mhz*1.022);high=1000*count/(mhz*.978);q=1000/(mhz*.978)
            row['decoder'].append(dict(mhz=mhz,threshold=count,threshold_min_ns=low,
                threshold_max_ns=high,quantum_ns=q,zero_margin_ns=low-zero-10-used-q,
                one_margin_ns=one-10-used-high-q))
        for hz,table in RMT.items():
            ticks=table[ident]
            row['rmt'].append(dict(hz=hz,ticks=list(ticks),rate=hz/ticks[0],
                pixel600_fps=hz/(14400*ticks[0]+hz/2000)))
        row['extra']=dict(skew_pair_ns=list(EXTRA_SKEW_NS),
            **extra_budget(EXTRA_BOARDS[ident],period,zero,one,thresholds,admission),
            at_guarantee_board_count=extra_budget(n,period,zero,one,thresholds,admission))
        profiles.append(row)
    return dict(units='ns unless field suffix says otherwise',phy='adaptive',sample_hz=9600000,
        profiles=profiles,typical=dict(segments_m=[30,3,3],path_ns=218,
        propagation_ns=3*300+36*5.38,extra_path_ns=180.5),
        qualification='pending: GUARANTEE maxima at 5 V only; EXTRA 3.0..5.5 V MAX interpretation needs written confirmation; cable/protection and assemblies unqualified',
        rate_error_ppm=200,window_max_us=25080,start_bound_us=75.016,
        check_bound_us=56.90,restore_bound_us=77.04,next_frame16_us=[9580,14940,25660],
        rates=[dict(rate=r,pitch=(335<<r)+165,window16=1000+16*((335<<r)+165),
            latest_end=118+330*(1<<r)/.985,deadline=((335<<r)+165)-30) for r in range(3)],
        declared_v203=dict(erase_units=224,lead_in_ms=9060,program_pause_ms=21,
            frame_us=[p['update_us'] for p in profiles],
            pass_us=[1784*p['update_us'] for p in profiles]))
