/* SPDX-License-Identifier: MIT */
#include <string.h>
#include "ultra_sender_descriptor.h"

int ultra_descriptor_next(const ultra_descriptor_t *d,uint16_t *cursor,ultra_tlv_t *o)
{
    uint16_t p;
    if(!d || !cursor || !o || !d->bytes) return -1;
    p=*cursor;
    if(p==d->length) return 0;
    if(p<11u || p>d->length || (uint32_t)d->length-p<2u || d->bytes[p+1u]>d->length-p-2u) return -1;
    o->type=d->bytes[p]; o->length=d->bytes[p+1u]; o->value=d->bytes+p+2u;
    *cursor=(uint16_t)(p+2u+o->length); return 1;
}
static uint8_t instance(const ultra_tlv_t *r)
{
    return r->type==7u || r->type==8u ? 0u : r->type==2u ? (uint8_t)(r->value[0]>>4) : r->value[0];
}
int ultra_descriptor_find(const ultra_descriptor_t *d,uint8_t type,uint8_t id,ultra_tlv_t *o)
{
    ultra_tlv_t r; uint16_t p=11; int rc;
    while((rc=ultra_descriptor_next(d,&p,&r))>0)
        if(r.type==type && r.length && instance(&r)==id) { *o=r; return 0; }
    return -1;
}
int ultra_descriptor_link(const ultra_descriptor_t *d,uint8_t id,ultra_desc_link_t *o)
{
    ultra_tlv_t r; const uint8_t *v;
    if(!o || ultra_descriptor_find(d,1,id,&r) || r.length<16u) return -1;
    v=r.value; o->id=v[0];o->flags=v[1];o->rates=v[2];o->hop_ns=(uint16_t)ultra_get16(v+3);
    o->skew_ns=v[5];o->gap_us=(uint16_t)ultra_get16(v+6);o->burst_us=v[8];o->clock_flags=v[9];
    o->kind=v[10];o->phy=v[11];o->mtu=(uint16_t)ultra_get16(v+12);o->reply=v[14];
    o->profiles=r.length>16u?v[16]:1u;
    o->local_latency_us=o->local_jitter_us=65535u;
    if(!o->phy && (o->kind==2u || o->kind==3u || o->kind==4u) && r.length>=ULTRA_LINK_LOCAL_BYTES) {
        o->local_latency_us=(uint16_t)ultra_get16(v+ULTRA_LINK_LOCAL_LATENCY);
        o->local_jitter_us=(uint16_t)ultra_get16(v+ULTRA_LINK_LOCAL_JITTER);
    }
    return 0;
}
int ultra_descriptor_domain(const ultra_descriptor_t *d,uint8_t id,ultra_desc_domain_t *o)
{
    ultra_tlv_t r;const uint8_t *v;
    if(!o || ultra_descriptor_find(d,2,id,&r) || r.length<13u) return -1;
    v=r.value;o->id=(uint8_t)(v[0]>>4);o->parent=v[0]&15u;o->kind=v[1];
    o->vmin_10mv=(uint16_t)ultra_get16(v+2);o->vmax_10mv=(uint16_t)ultra_get16(v+4);
    o->rating_ma=(uint16_t)ultra_get16(v+6);o->trip_ma=(uint16_t)ultra_get16(v+8);
    o->enforcement=v[10];o->sense=v[11];o->efficiency=v[12];return 0;
}
int ultra_descriptor_pwm(const ultra_descriptor_t *d,uint8_t id,ultra_desc_pwm_t *o)
{
    ultra_tlv_t r;const uint8_t *v;
    if(!o || ultra_descriptor_find(d,3,id,&r) || r.length<12u) return -1;
    v=r.value;o->first=v[0];o->count=v[1];o->domain=v[2];o->flags=v[3];
    o->channel_ma=(uint16_t)ultra_get16(v+4);o->bank_ma=(uint16_t)ultra_get16(v+6);
    o->enforcement=v[8];o->cal_ms=(uint16_t)ultra_get16(v+9);return 0;
}
int ultra_descriptor_pixel(const ultra_descriptor_t *d,uint8_t id,ultra_desc_pixel_t *o)
{
    ultra_tlv_t r;const uint8_t *v;unsigned i;
    if(!o || ultra_descriptor_find(d,4,id,&r) || r.length<31u) return -1;
    v=r.value;o->output=v[0];o->domain=v[1];o->segments=v[2];o->flags=v[3];
    o->frame_bytes=(uint16_t)ultra_get16(v+4);
    for(i=0;i<3u;++i)o->max_pixels[i]=(uint16_t)ultra_get16(v+6u+2u*i);
    o->bit_ns=(uint16_t)ultra_get16(v+12);o->delay_us=(uint16_t)ultra_get16(v+14);
    o->chips=(uint16_t)ultra_get16(v+16);o->rating_ma=(uint16_t)ultra_get16(v+18);
    o->fps=(uint16_t)ultra_get16(v+20);o->enforcement=v[22];o->sources=v[23];
    o->ingress_mask=(uint16_t)ultra_get16(v+24);o->cal_ms=(uint16_t)ultra_get16(v+26);
    o->latch_us=(uint16_t)ultra_get16(v+28);o->control=v[30];return 0;
}
int ultra_descriptor_sense(const ultra_descriptor_t *d,uint8_t id,ultra_desc_sense_t *o)
{
    ultra_tlv_t r; const uint8_t *v;
    if(!o || ultra_descriptor_find(d,5,id,&r) || r.length<8u)return -1;
    v=r.value;o->id=v[0];o->quantity=v[1];o->domain=v[2];o->target=v[3];o->flags=v[4];
    o->exponent=(int8_t)v[5];o->full_scale=(uint16_t)ultra_get16(v+6);return 0;
}
int ultra_descriptor_timing(const ultra_descriptor_t *d,uint8_t id,ultra_desc_timing_t *o)
{
    ultra_tlv_t r;const uint8_t *v;unsigned i;
    if(!o || ultra_descriptor_find(d,6,id,&r) || r.length<12u)return -1;
    v=r.value;o->port=v[0];o->qualified=v[1];o->certificate=(uint16_t)ultra_get16(v+2);o->quantum_ns=v[4];
    o->threshold_ns[0]=(uint16_t)ultra_get16(v+5);o->uart_error_ppm=(uint16_t)ultra_get16(v+7);
    o->isr_us=v[9];o->t0_us=v[10];o->uart_gap_us=v[11];o->profiles=r.length>=19u?v[12]:1u;
    for(i=1;i<4u;++i)o->threshold_ns[i]=r.length>=19u?(uint16_t)ultra_get16(v+11u+2u*i):0;
    return 0;
}
int ultra_descriptor_resources(const ultra_descriptor_t *d,ultra_desc_resources_t *o)
{
    ultra_tlv_t r;const uint8_t *v;
    if(!o || ultra_descriptor_find(d,7,0,&r) || r.length<14u)return -1;
    v=r.value;o->frame_pool_bytes=(uint16_t)ultra_get16(v);o->simultaneous_bytes=(uint16_t)ultra_get16(v+2);
    o->render_bytes_s=ultra_get32(v+4);o->stream_bytes_s=ultra_get32(v+8);
    o->active_outputs=v[12];o->calibration_jobs=v[13];return 0;
}
/* Strict UTF-8, no NUL/control, surrogate, overlong or out-of-range scalar. */
static int name_ok(const uint8_t *v,uint8_t n)
{
    unsigned i=0,j,k;uint32_t c,min;
    if(!n || n>63u)return 0;
    while(i<n) {
        c=v[i++];if(c<32u || c==127u)return 0;
        if(c<128u)continue;
        if(c>=194u && c<=223u){k=1;c&=31u;min=128;}
        else if(c>=224u && c<=239u){k=2;c&=15u;min=2048;}
        else if(c>=240u && c<=244u){k=3;c&=7u;min=65536;}
        else return 0;
        if(i+k>n)return 0;
        for(j=0;j<k;++j){if((v[i]&192u)!=128u)return 0;c=(c<<6)|(v[i++]&63u);}
        if(c<min || c>0x10ffffu || (c>=0xd800u && c<=0xdfffu) || (c>=128u && c<=159u))return 0;
    }
    return 1;
}
int ultra_descriptor_decode(ultra_descriptor_t *out,const uint8_t *b,uint16_t n)
{
    static const uint8_t prefix[9]={0,16,13,12,31,8,12,14,1};
    ultra_descriptor_t d;ultra_tlv_t r,tmp;uint16_t p=11,prev=0;
    unsigned count=0,names=0,resources=0;int rc;uint8_t partial=0,wide_timing=0,port_timing=0;
    if(!out || !b || n<11u || n>6144u || b[0]!=1u || b[1]!=11u || ultra_get16(b+2)!=n ||
       ultra_crc16(b+6,n-6u)!=ultra_get16(b+4) || !b[6] || b[6]==255u || b[7]>240u)return -1;
    memset(&d,0,sizeof d);d.bytes=b;d.length=n;d.crc=(uint16_t)ultra_get16(b+4);
    d.board=b[6];d.lights=b[7];d.records=b[8];d.flags=b[9];d.variant=b[10];
    while((rc=ultra_descriptor_next(&d,&p,&r))>0) {
        const uint8_t *v=r.value;uint16_t key;
        ++count;
        if(r.type>8u || !r.type)continue;
        if(r.length<prefix[r.type])return -1;
        key=(uint16_t)(((uint16_t)r.type<<8)|instance(&r));
        if(key<=prev)return -1;
        prev=key;
        if(r.type==1u) {
            ultra_desc_link_t l;if(ultra_descriptor_link(&d,v[0],&l))return -1;
            if(l.id>15u || v[15] || (l.flags&8u && l.flags&6u))return -1;   /* CHAIN_END: no FORWARD / RETURN_RELAY (UPDATE_FWD may stay) */
            if(l.phy==2u) {
                if((l.kind!=1u && l.kind!=5u) || l.flags || l.rates || l.hop_ns || l.skew_ns || l.gap_us || l.burst_us || l.clock_flags || l.mtu || l.reply)return -1;
            } else if(l.kind==2u || l.kind==3u || l.kind==4u) {
                if(l.phy || l.mtu<32u || (l.flags&62u) || l.rates || l.hop_ns || l.skew_ns || l.gap_us || l.burst_us || l.clock_flags ||
                   (l.reply!=0u && l.reply!=2u) || !!(l.flags&1u)!=(l.reply==2u) ||
                   (r.length>16u && v[16]))return -1;
            } else if(l.phy==1u) {
                if((l.kind!=1u && l.kind!=5u) || l.mtu<32u || l.gap_us<500u || l.reply>1u ||
                   !!(l.flags&1u)!=!!l.rates || !!l.rates!=(l.reply==1u) || l.rates&248u || l.burst_us>80u ||
                   (!l.rates && l.burst_us) || !(l.profiles&1u) || l.profiles&240u)return -1;
            } /* Unknown link kinds/PHY remain metadata, never usable. */
            if(d.board==1u && (l.kind!=5u || !(l.flags&4u) || l.flags&11u || l.reply || l.rates || l.burst_us))return -1;
        } else if(r.type==2u) {
            ultra_desc_domain_t dom;uint8_t cur=instance(&r),steps=0;
            if(cur>=15u)return -1;
            if(ultra_descriptor_domain(&d,cur,&dom) || (cur==0u?(dom.parent!=15u || dom.kind!=1u):dom.parent==15u))return -1;
            if((dom.vmin_10mv==65535u)!=(dom.vmax_10mv==65535u) || dom.vmin_10mv>dom.vmax_10mv || dom.efficiency>100u)return -1;
            while(cur!=15u){if(++steps>15u || ultra_descriptor_domain(&d,cur,&dom))return -1;cur=dom.parent;}
        } else if(r.type==3u) {
            if(partial || v[0]!=d.channels || v[0]%5u || !v[1] || v[1]>5u || (unsigned)d.channels+v[1]>240u ||
               ultra_descriptor_find(&d,2,v[2],&tmp) || v[11])return -1;
            d.channels=(uint8_t)(d.channels+v[1]);partial=v[1]<5u;
        } else if(r.type==4u) {
            ultra_desc_pixel_t px;ultra_desc_link_t ctrl;ultra_desc_domain_t dom;unsigned i;uint8_t cur;
            if(v[0]!=d.outputs || v[0]>14u || !v[2] || v[2]>15u || ultra_descriptor_pixel(&d,v[0],&px) ||
               ultra_descriptor_link(&d,px.control,&ctrl) || ctrl.phy==2u || ultra_descriptor_domain(&d,px.domain,&dom))return -1;
            if(!!(px.flags&1u)!=!!(px.sources&2u) || !!(px.sources&2u)!=!!px.ingress_mask ||
               (px.flags&16u && !(ctrl.flags&16u)) || (px.flags&4u && !(ctrl.flags&12u)) ||
               (px.flags&32u && !(px.flags&1u)) || px.chips&0xff80u)return -1;
            if((px.ingress_mask&((uint16_t)1u<<px.control)) && !(px.flags&32u) && !(ctrl.flags&16u))return -1;
            for(i=0;i<16u;++i)if((px.ingress_mask&((uint16_t)1u<<i)) && ultra_descriptor_find(&d,1,(uint8_t)i,&tmp))return -1;
            for(i=0;i<3u;++i)if((uint32_t)px.max_pixels[i]*(i+3u)>px.frame_bytes)return -1;
            cur=px.domain;
            for(i=0;cur!=15u && i<15u;++i) {
                if(ultra_descriptor_domain(&d,cur,&dom))return -1;
                if(px.sources&2u && dom.kind>=2u && dom.kind<=4u && dom.rating_ma && dom.rating_ma!=65535u && !(dom.enforcement&4u))return -1;
                cur=dom.parent;
            }
            if(cur!=15u)return -1;
            ++d.outputs;
        } else if(r.type==5u) {
            if(ultra_descriptor_find(&d,2,v[2],&tmp) || ((v[4]&24u)==24u) ||
               (d.flags&2u && v[1]==1u && v[4]&1u && !ultra_get16(v+6)))return -1;
        } else if(r.type==6u) {
            if(v[0]==255u)wide_timing=1;else{if(ultra_descriptor_find(&d,1,v[0],&tmp))return -1;port_timing=1;}
            if(wide_timing && port_timing)return -1;
            if(v[1]>1u || (!v[1] && ultra_get16(v+2)) ||
               (r.length>=19u && (!(v[12]&1u) || v[12]&240u)))return -1;
        } else if(r.type==7u) {
            ++resources;if(ultra_get16(v+2)>ultra_get16(v) || v[12]>d.outputs)return -1;
        } else {++names;if(!name_ok(v,r.length))return -1;}
    }
    if(rc<0 || count!=d.records || names!=1u || !!resources!=!!d.outputs || (d.channels && !d.lights))return -1;
    p=11;
    while(ultra_descriptor_next(&d,&p,&r)>0)if(r.type==1u && r.length>=16u && r.value[11]==1u){
        ultra_desc_timing_t tm;uint8_t profiles=r.length>16u?r.value[16]:1u;
        int have=ultra_descriptor_timing(&d,r.value[0],&tm)==0 || ultra_descriptor_timing(&d,255,&tm)==0;
        if(have && tm.profiles!=profiles)return -1;
        if(d.flags&2u && (!have || tm.qualified!=1u || !tm.certificate))return -1;
    }
    *out=d;return 0;
}
int ultra_descriptor_bank(const ultra_descriptor_t *d,uint8_t page,uint8_t part,uint8_t kind,uint8_t bank,ultra_bank_view_t *o)
{
    uint8_t n;
    if(!d || !o)return -1;
    o->channels=ultra_bank_mask(d->channels,bank);o->lights=ultra_bank_mask(d->lights,bank);o->outputs=ultra_bank_mask(d->outputs,bank);
    if(page==ULTRA_PAGE_PIXEL || page==ULTRA_PAGE_PIXEL_FX || (page==ULTRA_PAGE_CALIBRATION && kind==1u))return bank<d->outputs?0:-1;
    n=d->channels;
    if(page==ULTRA_PAGE_ACKS || page==ULTRA_PAGE_OUTPUTS) {if(d->lights>n)n=d->lights;}
    if(page==ULTRA_PAGE_ACKS && d->outputs>n)n=d->outputs;
    if(page==ULTRA_PAGE_DIAG && part!=1u)return -1;
    if(page!=ULTRA_PAGE_ACKS && page!=ULTRA_PAGE_OUTPUTS && page!=ULTRA_PAGE_HEALTH && page!=ULTRA_PAGE_CAL_TEST && page!=ULTRA_PAGE_CALIBRATION && page!=ULTRA_PAGE_DIAG)return -1;
    return bank<ultra_bank_count(n)?0:-1;
}
uint32_t ultra_descriptor_calibration_ms(const ultra_descriptor_t *d,uint8_t kind,uint8_t index)
{
    uint32_t ms=0;ultra_desc_pwm_t p;ultra_desc_pixel_t x;uint16_t i;
    if(kind==1u){if(ultra_descriptor_pixel(d,index,&x) || !(x.flags&ULTRA_PIXEL_CAL) || !x.cal_ms)return 0;ms=x.cal_ms;}
    else if(kind==0u){for(i=0;i<d->channels;i+=5u){if(ultra_descriptor_pwm(d,(uint8_t)i,&p) || !(p.flags&ULTRA_PWM_CAL) || !p.cal_ms)return 0;ms+=(uint32_t)p.cal_ms*p.count;}}
    else return 0;
    return 50u+(ms*3u+1u)/2u;
}
int ultra_outputs_decode(const ultra_descriptor_t *d,uint8_t bank,const uint8_t a[32],ultra_outputs_t *out)
{
    ultra_outputs_t o;const uint8_t *v;unsigned i;
    if(!out || !a || ultra_answer_check(a) || a[1]!=ULTRA_PAGE_OUTPUTS ||
       ultra_descriptor_bank(d,ULTRA_PAGE_OUTPUTS,0,0,bank,&o.present))return -1;
    v=a+6;
    for(i=0;i<5u;++i){o.level[i]=(uint16_t)ultra_get16(v+2u*i);o.input_ma[i]=(uint16_t)ultra_get16(v+10u+2u*i);
        if(!(o.present.channels&(1u<<i)) && (o.level[i] || o.input_ma[i]))return -1;
    }
    o.defined=v[21];o.scene=v[22];o.why=v[23];
    if(o.defined&~o.present.lights || o.scene&~o.present.channels)return -1;
    *out=o;return 0;
}
int ultra_sender_descriptor(ultra_sender_t *l,uint16_t board,const ultra_descriptor_t *d)
{
    ultra_sender_board_t *b;uint8_t lights;unsigned i;
    if(!l || board>=l->n || !d || !d->bytes)return -1;
    b=&l->boards[board];if(b->first!=65535u || b->cfg_dirty)return -1;
    for(i=0;i<ULTRA_SENDER_CFG_WORDS;++i)if(b->cfg_have[i])return -1;
    b->descriptor=d;b->capacity=d->lights;b->channels=d->channels;
    lights=b->lights>d->lights?d->lights:b->lights;
    if(ultra_sender_set_lights(l,board,lights))return -1;
    ultra_sender_resync(l,0);
    return 0;
}
int ultra_descriptor_config(const ultra_descriptor_t *d,uint8_t target,uint8_t id,uint16_t req,uint16_t *out)
{
    uint16_t rating=65535u;ultra_desc_domain_t dom;ultra_desc_pwm_t bank;ultra_desc_pixel_t px;
    if(!d || !out || !ultra_config_scope_ok(target,id))return -1;
    if(target==255u) {
        if(id==21u || (id>=33u && id<=46u)){
            if(ultra_descriptor_domain(d,id==21u?0u:(uint8_t)(id-32u),&dom))return -1;
            rating=dom.rating_ma;
        } else if(id==25u){
            uint16_t i;   /* the pulse-skip rate: 2000 / 1000 Hz, only on a board whose every PWM bank takes it */
            if(!d->channels || (req!=ULTRA_SKIP_HZ_DEFAULT && req!=ULTRA_SKIP_HZ_SLOW))return -1;
            for(i=0;i<d->channels;i+=5u)if(ultra_descriptor_pwm(d,(uint8_t)i,&bank) || !(bank.flags&ULTRA_PWM_SKIP_RATE))return -1;
            *out=req;return 0;
        } else {*out=req;return 0;}
    } else if(id==11u || id==12u) {
        if(target>=d->channels || ultra_descriptor_pwm(d,(uint8_t)(target/5u*5u),&bank))return -1;
        if(id==11u){*out=req;return req==65535u?-1:0;}rating=bank.channel_ma;
    } else {
        if(ultra_descriptor_pixel(d,target,&px))return -1;
        if(id==69u)rating=px.rating_ma;
        else if(id==77u){
            if(req>16u || (req==0u?!(px.sources&1u):!(px.sources&2u) || !(px.ingress_mask&((uint16_t)1u<<(req-1u)))))return -1;
            *out=req;return 0;
        } else if(id==64u){if(req>=16u || !(px.chips&((uint16_t)1u<<req)))return -1;*out=req;return 0;}
        else if(id==67u){*out=req>px.fps?px.fps:req;return 0;}
        else {*out=req;return 0;}
    }
    if(rating==65535u || req==65535u)return -1;
    *out=ultra_lower_limit(rating,req);return 0;
}
uint32_t ultra_power_mw(uint16_t ma,uint16_t v){return (uint32_t)((uint64_t)ma*v/100u);}
uint32_t ultra_lifetime_energy_allowance(uint16_t ma,uint16_t v,uint32_t dt,uint32_t remaining)
{
    uint64_t wh;
    if(ma==65535u || v==65535u)return 0;
    wh=((uint64_t)ma*v*((uint64_t)dt+10u)+999999u)/1000000u;
    return wh>remaining?remaining:(uint32_t)wh;
}
uint64_t ultra_sense_full_scale(const ultra_desc_sense_t *s,uint32_t *div)
{
    uint64_t n=s->full_scale;int e=s->exponent;*div=1;
    while(e>0){if(n>UINT64_MAX/10u)return UINT64_MAX;n*=10u;--e;}
    while(e<0){if(*div>UINT32_MAX/10u){*div=0;return n;}*div*=10u;++e;}
    return n;
}
int ultra_identity_decode(ultra_identity_t *o,uint8_t part,const uint8_t a[32])
{
    ultra_identity_t x;const uint8_t *v=a+6;unsigned i;
    if(!o || !a || part>2u || ultra_answer_check(a) || a[1]!=ULTRA_PAGE_IDENTITY)return -1;
    x=*o;
    if(!part){memcpy(x.uid,v,12);x.firmware=ultra_get32(v+12);x.boot_version=v[16];x.board=v[17];x.revision=v[18];x.wire=v[19];
        x.descriptor_length=(uint16_t)ultra_get16(v+20);x.descriptor_crc=(uint16_t)ultra_get16(v+22);
        if(x.wire!=3u || !x.board || x.board==255u || x.descriptor_length>6144u)return -1;
    }else if(part==1u){for(i=0;i<6u;++i)x.features[i]=ultra_get32(v+4u*i);}
    else{memcpy(x.target.bytes,v,16);memcpy(x.geometry,v+16,3);x.record_format=v[19];x.descriptor_format=v[20];x.entry_10ms=v[21];
        if(v[22] || v[23] || x.entry_10ms<5u)return -1;}
    x.parts|=(uint8_t)(1u<<part);
    if((x.parts&5u)==5u && (x.target.bytes[0]!=x.board || x.target.bytes[7]!=x.revision))return -1;
    *o=x;return 0;
}
int ultra_descriptor_part(ultra_descriptor_rx_t *r,uint8_t part,const uint8_t data[24])
{
    uint16_t offset=(uint16_t)(part*24u),take;unsigned i;
    if(!r || !data || !r->bytes || offset>r->received)return -1;
    if(!part && !r->received){
        if(data[0]!=1u || data[1]!=11u)return -1;
        r->length=(uint16_t)ultra_get16(data+2);
        if(r->length<11u || r->length>6144u || r->length>r->capacity){r->length=0;return -1;}
    }
    if(offset>=r->length)return -1;
    take=(uint16_t)(r->length-offset);if(take>24u)take=24;
    for(i=take;i<24u;++i)if(data[i])return -1;
    if(offset<r->received)return memcmp(r->bytes+offset,data,take)?-1:(r->received==r->length);
    memcpy(r->bytes+offset,data,take);r->received=(uint16_t)(offset+take);
    return r->received==r->length?1:0;
}
