/* SPDX-License-Identifier: MIT */
#include <string.h>
#include "ultra_sender_profile.h"
#include "ultra_sender_internal.h"

int ultra_link_stats_decode(ultra_link_stats_t *out,uint8_t part,const uint8_t a[32],uint32_t boot,int64_t now)
{
    ultra_link_stats_t s;const uint8_t *v;
    if(!out || !a || part>1u || ultra_answer_check(a) || a[1]!=ULTRA_PAGE_LINK_STATS)return -1;
    v=a+6;
    if(!part) {
        memset(&s,0,sizeof s);s.port=v[0];s.phy=v[1];s.flags=v[2];s.quantum_ns=v[3];
        s.zero_min=(uint16_t)ultra_get16(v+4);s.zero_max=(uint16_t)ultra_get16(v+6);s.zero_mean=(uint16_t)ultra_get16(v+8);
        s.one_min=(uint16_t)ultra_get16(v+10);s.one_max=(uint16_t)ultra_get16(v+12);s.one_mean=(uint16_t)ultra_get16(v+14);
        s.zeros=(uint16_t)ultra_get16(v+16);s.ones=(uint16_t)ultra_get16(v+18);s.invalid=(uint16_t)ultra_get16(v+20);
        s.epoch=(uint16_t)ultra_get16(v+22);s.profile=255;s.boot=boot;s.captured_ms=now;s.parts=1;
        if(s.port>15u || s.phy!=1u || s.zero_min>s.zero_mean || s.zero_mean>s.zero_max ||
           s.one_min>s.one_mean || s.one_mean>s.one_max)return -1;
    } else {
        s=*out;
        if(s.parts!=1u || s.boot!=boot || now<s.captured_ms || now-s.captured_ms>2000 ||
           s.port!=v[0] || s.flags!=v[2] || s.epoch!=ultra_get16(v+4) || v[22] || v[23])return -1;
        s.profile=v[1];s.supported=v[3];s.period_min=(uint16_t)ultra_get16(v+6);s.period_max=(uint16_t)ultra_get16(v+8);
        s.low_min=(uint16_t)ultra_get16(v+10);s.low_max=(uint16_t)ultra_get16(v+12);s.frames=(uint16_t)ultra_get16(v+14);
        s.crc_bad=(uint16_t)ultra_get16(v+16);s.missing=(uint16_t)ultra_get16(v+18);s.scale_ppm=(uint16_t)ultra_get16(v+20);
        if(s.period_min>s.period_max || s.low_min>s.low_max || (s.profile>3u && s.profile!=255u))return -1;
        s.parts=3;
    }
    s.rate_request=(uint8_t)((s.flags&ULTRA_LS_RATE_REQUEST)!=0);
    *out=s;return 0;
}
int ultra_link_stats_margin(const ultra_link_stats_t *s,uint32_t boot,int64_t now,uint16_t tmin,uint16_t tmax,int32_t *out)
{
    uint32_t max,scale,uncertainty;
    if(!s || !out || s->parts!=3u || s->boot!=boot || now<s->captured_ms || now-s->captured_ms>2000 ||
       (s->flags&7u)!=3u || s->profile>3u || !(s->supported&(1u<<s->profile)) ||
       !s->quantum_ns || s->zeros<256u || s->ones<256u || s->scale_ppm>200u || !s->frames || tmin>tmax)return -1;
    max=s->period_max;if(s->low_max>max)max=s->low_max;if(s->one_max>max)max=s->one_max;if(s->zero_max>max)max=s->zero_max;
    scale=(max*s->scale_ppm+999999u)/1000000u;uncertainty=s->quantum_ns+scale;
    *out=ultra_line_margin_ns(s->zero_max,s->one_min,s->zero_min<s->one_min?s->zero_min:s->one_min,
                             s->low_min,tmin,tmax,(uint16_t)uncertainty);
    return 0;
}

int ultra_profile_allowed(uint8_t p, const ultra_profile_evidence_t *e)
{
    if (!e || !ultra_line_allowed(p,e->inventory_complete,e->all_ultra,e->common_mask,e->path_ns)) return 0;
    return (e->qualified_mask & e->observed_mask & e->gate_mask & (1u<<p)) != 0;
}
int ultra_profile_trainable(uint8_t p,const ultra_profile_evidence_t *e)
{
    return e && e->inventory_complete && e->all_ultra &&
        ultra_line_allowed(p,1,1,e->common_mask,e->path_ns) &&
        (e->qualified_mask&e->gate_mask&(1u<<p));
}

static int profile_change(ultra_sender_t *l,uint8_t p,uint32_t hz,
                           const ultra_profile_evidence_t *e,int64_t now,int training)
{
    ultra_timing_t t;
    uint32_t gap;
    if (!l || !l->n || !(training?ultra_profile_trainable(p,e):ultra_profile_allowed(p,e)) || l->tx_active || l->update_active ||
        now<l->hold_us || (l->has_sent && now<l->last_start_us+l->frame_us)) return ULTRA_ERR_ARG;
    gap=e->min_gap_us<500u?500u:e->min_gap_us;
    if (ultra_timing_profile(&t,p,hz,gap)) return ULTRA_ERR_ARG;
    l->timing=t;
    l->frame_us=ultra_frame_max_us(&t,ultra_frame_bytes(l->n));
    l->switch_us=ultra_frame_max_us(&t,ultra_switch_frame_bytes(l->n));
    l->last_bytes=0;                 /* retime cannot reuse a discarded waveform */
    ++l->profile_epoch;
    ultra_sender_resync(l,now);
    l->preamble_left=(uint16_t)ultra_preamble_frames(l->n);
    l->ready_frames=0;
    l->profile_training=(uint8_t)training;
    return ULTRA_OK;
}
int ultra_sender_profile(ultra_sender_t *l,uint8_t p,uint32_t hz,const ultra_profile_evidence_t *e,int64_t now)
{return profile_change(l,p,hz,e,now,0);}
int ultra_sender_profile_acquire(ultra_sender_t *l,uint8_t p,uint32_t hz,const ultra_profile_evidence_t *e,int64_t now)
{return profile_change(l,p,hz,e,now,1);}
int ultra_sender_profile_accept(ultra_sender_t *l,const ultra_profile_evidence_t *e)
{
    if(!l || !l->n || l->preamble_left || !ultra_profile_allowed(l->timing.profile,e))return -1;
    l->profile_training=0;return 0;
}
static void training_nop(uint8_t *slot,uint8_t pattern)
{
    uint32_t i;
    ultra_slot_begin(slot,ULTRA_NOP,0,0,0);
    for(i=3u;i<30u;++i)slot[i]=pattern;
    ultra_slot_seal(slot);
}
uint32_t ultra_sender_training_frame(ultra_sender_t *l,uint8_t phase,const ultra_profile_evidence_t *e,int64_t now,uint8_t *frame)
{
    static const uint8_t pattern[4]={0x55,0xaa,0,0xff};uint32_t i,n;
    if(!l || !l->n || !frame || phase>3u || !ultra_profile_trainable(l->timing.profile,e) ||
       l->tx_active || l->update_active==1u || l->preamble_left || l->off || !l->powered ||
       l->switch_frame<ULTRA_SWITCH_WS_FRAMES || now<l->hold_us ||
       (l->last_bytes && now<l->last_start_us+l->frame_us))return 0;
    for(i=0;i<l->n;++i){l->boards[i].f_start=ULTRA_NO_START;l->boards[i].f_floor=0;
        training_nop(frame+32u*i,pattern[phase]);}
    n=ultra_frame_bytes(l->n);l->has_sent=1;l->last_start_us=now;l->last_ends_preamble=0;l->last_notice=0;
    l->hdr_flags=0;l->kind=ULTRA_KIND_ULTRA;l->last_bytes=(uint16_t)n;++l->frames;
    ultra_sender_finish_(l,now,frame);
    if(l->ready_us==INT64_MIN)l->ready_us=ultra_wire_end_us(&l->timing,frame,n,now);
    if(l->ready_frames<65535u)++l->ready_frames;
    return n;
}

void ultra_auto_init(ultra_auto_t *s, int64_t now)
{
    memset(s,0,sizeof *s); s->mode=255; s->changed_ms=now;
    s->last_window_ms=INT64_MIN; s->state=ULTRA_AUTO_UNKNOWN;
}
static void reason(ultra_auto_t *s,uint8_t r,int64_t now)
{
    s->reason=r; s->reason_ms=now; s->warning=1;
}
static void change(ultra_auto_t *s,ultra_auto_result_t *r,uint8_t p,uint8_t action,int64_t now)
{
    s->active=p; s->changed_ms=now; s->bad=0; r->profile=p; r->action=action;
}
ultra_auto_result_t ultra_auto_step(ultra_auto_t *s,const ultra_auto_input_t *in)
{
    ultra_auto_result_t r={0,0,0,0,0};
    uint8_t bad, hard;
    r.profile=s->active;
    if(s->active && (!in->evidence.inventory_complete || !in->evidence.all_ultra ||
       !(in->evidence.common_mask&(1u<<s->active)))){
        r.stop_pixels=1;reason(s,ULTRA_AUTO_REASON_ADMISSION,in->now_ms);s->state=ULTRA_AUTO_FAILED;goto done;
    }
    if(s->probing==2u){
        r.stop_pixels=1;
        if(!in->transaction_active && !in->update_active){
            s->probing=0;s->good=0;change(s,&r,s->last_good,ULTRA_AUTO_REVERT,in->now_ms);
        }
        goto done;
    }
    /* Explicit repeated training-frame loss is distinct from an absent report.
     * It can request DARK acquisition on a previously qualified Ultra-only port. */
    if(in->repeated_loss && !s->probing){
        r.stop_pixels=1;reason(s,ULTRA_AUTO_REASON_ERRORS,in->now_ms);s->state=ULTRA_AUTO_RECOVERING;
        if(s->mode==255u && !in->transaction_active && !in->update_active && ultra_profile_trainable(3,&in->evidence))
            change(s,&r,3,ULTRA_AUTO_RECOVER,in->now_ms);
        goto done;
    }
    if (s->mode!=255u && s->mode>3u) {
        reason(s,ULTRA_AUTO_REASON_OVERRIDE,in->now_ms); s->state=ULTRA_AUTO_FAILED; goto done;
    }
    if(s->mode<4u && s->mode!=s->active){
        if(in->transaction_active || in->update_active)goto done;
        if(ultra_profile_allowed(s->mode,&in->evidence)){
            s->reason=ULTRA_AUTO_REASON_OVERRIDE;s->reason_ms=in->now_ms;
            change(s,&r,s->mode,ULTRA_AUTO_MANUAL,in->now_ms);
        }else{reason(s,ULTRA_AUTO_REASON_ADMISSION,in->now_ms);s->state=ULTRA_AUTO_FAILED;}
        goto done;
    }
    if (!in->valid || in->profile!=s->active ||
        (s->last_window_ms!=INT64_MIN && in->now_ms-s->last_window_ms<1000)) {
        if (!in->valid) {
            reason(s,ULTRA_AUTO_REASON_MISSING,in->now_ms); s->state=ULTRA_AUTO_UNKNOWN;
            s->bad=0; s->good=0;
        }
        goto done;
    }
    if (s->last_window_ms!=INT64_MIN && in->now_ms-s->last_window_ms>2000) { s->bad=0; s->good=0; }
    s->last_window_ms=in->now_ms;
    bad=(uint8_t)(in->margin_ns<20 || in->crc_bad || in->invalid_runs || in->missing_frames);
    hard=(uint8_t)(in->margin_ns<=0 || in->repeated_loss);
    if (bad) reason(s,in->margin_ns<20?ULTRA_AUTO_REASON_MARGIN:ULTRA_AUTO_REASON_ERRORS,in->now_ms);
    s->state=bad?ULTRA_AUTO_WARNING:ULTRA_AUTO_OK;
    if (hard) r.stop_pixels=1;
    if (s->probing) {
        if (bad || in->margin_ns<60 || !in->idle || in->transaction_active || in->update_active) {
            s->good=0; s->probing=0; reason(s,ULTRA_AUTO_REASON_PROBE,in->now_ms);
            if (!in->transaction_active && !in->update_active)
                change(s,&r,s->last_good,ULTRA_AUTO_REVERT,in->now_ms);
            else { s->probing=2; r.stop_pixels=1; }
        } else if (++s->good>=60u) {
            s->probing=0; s->last_good=s->active; s->warning=0; r.action=ULTRA_AUTO_UP;
        }
        goto done;
    }
    if (in->transaction_active || in->update_active) goto done;
    if (s->mode<4u) {
        if (s->mode!=s->active) {
            if (ultra_profile_allowed(s->mode,&in->evidence))
                change(s,&r,s->mode,ULTRA_AUTO_MANUAL,in->now_ms);
            else { reason(s,ULTRA_AUTO_REASON_ADMISSION,in->now_ms); s->state=ULTRA_AUTO_FAILED; }
        }
        goto done;
    }
    if (hard) {
        s->state=ULTRA_AUTO_RECOVERING;
        if (ultra_profile_trainable(3,&in->evidence)) change(s,&r,3,ULTRA_AUTO_RECOVER,in->now_ms);
        else { reason(s,ULTRA_AUTO_REASON_ADMISSION,in->now_ms); s->state=ULTRA_AUTO_FAILED; }
        goto done;
    }
    if (bad) { if(s->bad<3u) ++s->bad; } else s->bad=0;
    if (s->bad>=3u && s->active<3u) {
        if (ultra_profile_trainable((uint8_t)(s->active+1u),&in->evidence)) {
            change(s,&r,(uint8_t)(s->active+1u),ULTRA_AUTO_DOWN,in->now_ms); s->last_good=s->active;
            r.stop_pixels=1;
        } else { reason(s,ULTRA_AUTO_REASON_ADMISSION,in->now_ms); s->state=ULTRA_AUTO_FAILED; }
    } else if (!bad && s->active && in->idle && in->now_ms-s->changed_ms>=60000 &&
               ultra_profile_trainable((uint8_t)(s->active-1u),&in->evidence)) {
        s->last_good=s->active; s->probing=1; s->good=0;
        change(s,&r,(uint8_t)(s->active-1u),ULTRA_AUTO_PROBE,in->now_ms);
        r.stop_pixels=1;
    }
done:
    if (s->warning && s->state==ULTRA_AUTO_OK) s->state=ULTRA_AUTO_WARNING;
    r.state=s->state; r.reason=s->reason; return r;
}

int ultra_parlio_groups(const ultra_parlio_lane_t *lanes,uint8_t n,ultra_parlio_group_t g[4])
{
    uint8_t i; uint32_t used=0;
    if (!g || (!lanes && n) || n>32u) return -1;
    for(i=0;i<4u;++i) { g[i].profile=i; g[i].lanes=0; }
    for(i=0;i<n;++i) {
        uint32_t mask;
        if(lanes[i].lane>=32u || lanes[i].profile>=4u) return -1;
        mask=(uint32_t)1u<<lanes[i].lane;
        if(used&mask) return -1;
        used|=mask; g[lanes[i].profile].lanes|=mask;
    }
    return 0;
}
int ultra_parlio_begin(ultra_parlio_lane_t *l,uint8_t pin,uint8_t p,const uint8_t *b,
                       uint32_t size,uint32_t hz,uint32_t gap)
{
    if(!l || l->active || pin>=32u || p>=4u || !b || !size || size>32776u || hz!=9600000u ||
       gap<500u || gap>1000000u) return -1;
    l->bytes=b; l->length=size; l->sample=0; l->profile=p; l->lane=pin; l->active=1;
    l->reset_samples=(gap*48u+4u)/5u; return 0;
}
void ultra_parlio_encode(ultra_parlio_lane_t *lanes,uint8_t n,uint32_t *out,uint32_t count,uint32_t *done)
{
    uint32_t i; uint8_t j; *done=0;
    for(i=0;i<count;++i) {
        uint32_t word=0;
        for(j=0;j<n;++j) {
            ultra_parlio_lane_t *l=&lanes[j];
            const ultra_line_profile_t *p;
            uint32_t end,bit,phase,high;
            if(!l->active) continue;
            p=ultra_line_profile(l->profile); end=l->length*8u*p->bit_samples;
            if(l->sample<end) {
                bit=l->sample/p->bit_samples; phase=l->sample%p->bit_samples;
                high=(l->bytes[bit>>3]&(0x80u>>(bit&7u)))?p->one_samples:p->zero_samples;
                if(phase<high) word|=(uint32_t)1u<<l->lane;
            }
            if(++l->sample>=end+l->reset_samples) { l->active=0; *done|=(uint32_t)1u<<l->lane; }
        }
        out[i]=word;
    }
}
