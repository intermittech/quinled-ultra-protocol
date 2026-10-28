/* SPDX-License-Identifier: MIT
 * Independent integer arithmetic oracle for wire-3 launch timing. */
#include <stdio.h>
#include <string.h>
#include "ultra_sender.c"
#include "ultra_sender_bits.c"
static uint32_t rng=0x3952871u;
static uint32_t random32(void){rng=rng*1664525u+1013904223u;return rng;}
int main(void)
{
    uint32_t clocks[3]={9600000,40000000,80000000},i,c,p,bad=0,checks=0;
    uint16_t bits[3][4]={{12,16,24,48},{50,67,100,200},{100,133,200,400}};
    uint16_t zero[3][4]={{3,4,6,12},{12,17,25,50},{25,33,50,100}};
    uint16_t one[3][4]={{9,12,18,36},{38,50,75,150},{75,100,150,300}};
    for(i=0;i<200000u;++i){uint64_t a=((uint64_t)random32()<<32)|random32(),d=random32()|1u;
        bad+=ultra_udiv64_(a,d)!=a/d;++checks;}
    for(c=0;c<3u;++c)for(p=0;p<4u;++p){
        ultra_timing_t t;ultra_sender_t l;ultra_sender_config_t cfg;
        ultra_sender_board_t boards[8];ultra_sender_item_t items[24];uint8_t f[264];
        if(ultra_timing_profile(&t,(uint8_t)p,clocks[c],500))return 2;
        bad+=t.bit_ticks!=bits[c][p] || t.zero_ticks!=zero[c][p] || t.one_ticks!=one[c][p];++checks;
        ultra_sender_config_init(&cfg,8,1);cfg.timing=t;cfg.clear_unused=0;
        bad+=ultra_sender_init(&l,&cfg,boards,items,24)!=0;
        for(i=0;i<2000u;++i){
            uint64_t ticks,expected,actual;uint32_t n,k;
            int64_t now=(int64_t)i*200000u+0xffff0000LL;
            n=ultra_sender_next_frame(&l,now,f);if(!n)continue;
            ticks=0;
            for(k=0;k<n*8u;++k){uint32_t h=(f[k>>3]&(0x80u>>(k&7u)))?one[c][p]:zero[c][p];
                ticks+=h;if(k+1u<n*8u)ticks+=bits[c][p]-h;}
            expected=(ticks*1000000000u+clocks[c]/2u)/clocks[c];
            bad+=ultra_wire_ns(&t,f,n)!=expected;++checks;
            expected=ticks*1000000u;
            actual=(uint64_t)(l.last_time_us-now)*clocks[c];
            bad+=(actual>expected?actual-expected:expected-actual)>(uint64_t)2u*clocks[c];++checks;
        }
        ultra_sender_deinit(&l);
    }
    for(i=0;i<20000u;++i){uint8_t a[32],b[32],flags=(uint8_t)(random32()&96u);ultra_values_t v;
        uint32_t j,d=random32()&0xffffffu;v.m=(uint16_t)random32();
        for(j=0;j<5u;++j){v.a[j]=(uint16_t)random32();v.b[j]=(uint16_t)random32();}
        set_fill(a,239,66,flags,&v,d);ultra_slot_seal(a);
        ultra_slot_set(b,239,66,flags,v.m,v.a,v.b,d,0);bad+=memcmp(a,b,32)!=0;++checks;
    }
    printf("%u arithmetic/profile/TIME checks, %u failures\n",checks,bad);return bad?1:0;
}
