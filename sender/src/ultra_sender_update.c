/* SPDX-License-Identifier: MIT */
#include <string.h>
#include "ultra_sender_update.h"

int ultra_update_status_decode(ultra_update_status_t *o,const uint8_t a[32],uint8_t session,
                               const uint8_t uid[12],uint32_t len,uint32_t crc)
{
    ultra_update_status_t s;const uint8_t *v;unsigned i,pop=0,contiguous=0;uint32_t block,remaining,prefix;
    if(!o || !a || !uid || ultra_answer_check(a) || a[1]!=0x86u || (a[2]>>4) || a[3]!=session ||
       ultra_get16(a+4)!=ultra_crc16(uid,12))return -1;
    v=a+6;s.version=v[0];s.state=v[1];s.error=v[2];s.credit=v[3];s.session=v[4];s.flags=v[5];
    s.chunk_mask=(uint16_t)ultra_get16(v+6);s.accepted=ultra_get32(v+8);s.committed=ultra_get32(v+12);
    s.retry_ms=ultra_get32(v+16);s.image_crc=ultra_get32(v+20);
    if(s.version!=1u || s.state>8u || s.error>10u || s.credit>8u || s.session!=session || s.chunk_mask>255u ||
       s.accepted>len || s.committed>s.accepted || s.image_crc!=crc ||
       (s.committed!=len && s.committed%256u) ||
       (s.state==UPD_STATE_BUSY && (s.credit || !s.retry_ms)) ||
       ((s.state==UPD_STATE_FAILED || s.state==UPD_STATE_RESTART_REQUIRED) && s.credit) ||
       (s.state==UPD_STATE_COMMITTED && (s.committed!=len || s.error)))return -1;
    block=s.committed/256u*256u;remaining=len-block;if(remaining>256u)remaining=256;
    for(i=0;i<8u;++i)if(s.chunk_mask&(1u<<i)){
        if(i*32u>=remaining)return -1;
        ++pop;
    }
    for(i=0;i<8u && s.chunk_mask&(1u<<i);++i)++contiguous;
    prefix=contiguous*32u;if(prefix>remaining)prefix=remaining;
    if(s.committed<len && (s.accepted!=block+prefix || s.credit>8u-pop))return -1;
    *o=s;return 0;
}
int ultra_update_plan_metadata(ultra_update_plan_t *out,const upd_descriptor_t *desc,const upd_target_t *target,
                       const ultra_update_layout_t *l,uint32_t gap)
{
    ultra_update_plan_t p;uint32_t base,size,erase,program,rounded;int rc;
    if(!out || !desc || !target || !l || gap>1000000u)return UPD_ERROR_BOUNDS;
    rc=upd_desc_check(desc->bytes,target,l->boot_version);if(rc)return rc;
    if(l->entry_10ms<UPD_ENTRY_MIN_MS/10u || !l->geometry[1] || !l->geometry[2] || l->geometry[0]>>4!=8u)return UPD_ERROR_BOUNDS;
    erase=(uint32_t)1u<<(l->geometry[0]&15u);program=256;
    if(erase<program)return UPD_ERROR_BOUNDS;
    base=ultra_get32(target->bytes+8);size=ultra_get32(target->bytes+12);
    memset(&p,0,sizeof p);p.length=ultra_get32(desc->bytes+20);p.body_crc=ultra_get32(desc->bytes+24);
    p.descriptor_crc=ultra_get32(desc->bytes+60);p.firmware=ultra_get32(desc->bytes+16);
    if(!l->bootloader_bytes || l->metadata_bytes<256u || l->metadata_base%erase || base%erase ||
       l->metadata_base>base || l->metadata_bytes!=base-l->metadata_base ||
       p.length>UINT32_MAX-(program-1u))return UPD_ERROR_BOUNDS;
    rounded=(p.length+program-1u)/program*program;if(rounded>size)return UPD_ERROR_BOUNDS;
    if(p.length>UINT32_MAX-base || base+p.length>UINT32_MAX-(erase-1u))return UPD_ERROR_BOUNDS;
    rounded=(base+p.length+erase-1u)/erase*erase;
    if(rounded<base || rounded-base>size)return UPD_ERROR_BOUNDS;
    p.erase_units=(rounded-l->metadata_base)/erase;
    p.status=(target->bytes[5]&UPD_FEATURE_STATUS)!=0;
    {uint64_t lead=(uint64_t)p.erase_units*UPD_OPERATION_DEADLINE_FACTOR*l->geometry[2]+UPD_DECLARED_MARGIN_MS;
        /* Transit forwarders hold only upd_transit_hold_ms() (timing.md). */
        if(lead>upd_transit_hold_ms(desc->bytes))return UPD_ERROR_BOUNDS;
        p.lead_ms=(uint32_t)lead;}
    p.pause_ms=UPD_OPERATION_DEADLINE_FACTOR*l->geometry[1]+(gap+999u)/1000u;
    p.finish_ms=UPD_DISPATCH_MS+UPD_OPERATION_DEADLINE_FACTOR*l->geometry[1];p.entry_ms=(uint32_t)l->entry_10ms*10u;
    *out=p;return 0;
}
int ultra_update_plan(ultra_update_plan_t *out,const upd_descriptor_t *desc,const upd_target_t *target,
                       const ultra_update_layout_t *l,uint32_t gap,ultra_update_read_t read,void *context)
{
    ultra_update_plan_t p;uint32_t off,crc=0xffffffffu;uint8_t block[32];int rc;
    if(!read)return UPD_ERROR_BOUNDS;
    rc=ultra_update_plan_metadata(&p,desc,target,l,gap);if(rc)return rc;
    for(off=0;off<p.length;off+=32u){uint32_t n=p.length-off;if(n>32u)n=32;
        if(read(context,off,block,n))return UPD_ERROR_CRC;
        crc=upd_crc32_words(crc,block,n/4u);
    }
    if(crc!=p.body_crc)return UPD_ERROR_CRC;
    *out=p;return 0;
}
static int fail(ultra_update_t *u,uint8_t e)
{
    u->state=ULTRA_UPDATE_FAILED;u->error=e;
    /* Keep lease/busy deadline: failure is not permission to transmit. */
    return -1;
}
static int emit(ultra_update_t *u,uint8_t *f,uint8_t op,uint16_t index,uint8_t pass,
                const uint8_t *data,int64_t now)
{
    const ultra_timing_t *t=&u->line->timing;
    uint32_t lo,hi,k,best=0,base;uint64_t prefix=383u*(uint64_t)t->bit_ticks,err=UINT64_MAX;
    memset(&u->transport,0,sizeof u->transport);u->transport.marker=u->line->digital;
    u->transport.handback_required=u->line->digital;
    if(u->line->digital){u->transport.lead_low_us=u->gap_us;u->transport.marker_high_us=20;
        u->transport.marker_low_us=10;now+=(int64_t)u->gap_us+30;}
    u->transport.frame_start_us=now;base=ultra_time_wire((uint64_t)now);
    lo=(uint32_t)((prefix+t->zero_ticks)*1000000u/t->clock_hz);
    hi=(uint32_t)(((prefix+t->one_ticks)*1000000u+t->clock_hz-1u)/t->clock_hz);
    for(k=lo;k<=hi;++k){uint64_t a,b,e;
        upd_build_frame(f,op,u->session,index,pass,0,data,ultra_time_add(base,k));
        a=(prefix+((f[47]&1u)?t->one_ticks:t->zero_ticks))*1000000u;b=(uint64_t)k*t->clock_hz;
        e=a>b?a-b:b-a;if(e<err){err=e;best=k;}
    }
    upd_build_frame(f,op,u->session,index,pass,0,data,ultra_time_add(base,best));
    u->line->last_start_us=now;u->line->last_time_us=now+best;
    u->poll_end_us=ultra_wire_end_us(t,f,48,now);
    u->transport.frame_end_us=u->poll_end_us;u->transport.tail_low_us=u->gap_us;
    if(op==UPD_STATUS){u->transport.release_us=80;u->transport.tail_low_us=0;
        u->transport.listen_us=u->line->digital?900u+ultra_rw_slot_us(u->rate):ultra_rw_window_us(1,u->rate);}
    u->due_us=u->poll_end_us+u->gap_us;u->line->hold_us=u->due_us;
    return 48;
}
static int64_t return_wait(const ultra_update_t *u)
{
    return u->line->digital?900u+ultra_rw_slot_us(u->rate)+20u+u->gap_us:
        ultra_rw_next_us(1,u->rate,u->gap_us);
}
static void reserve(ultra_update_t *u,uint64_t ms)
{
    int64_t until=u->poll_end_us+(int64_t)ms*1000;
    if(until>u->due_us)u->due_us=until;
    u->line->hold_us=u->due_us;
}
/* Receiver bounds (entry/reboot + blackout, commit, acknowledgement,
 * confirmation write) run from the physical frame end and add the port's
 * processing LOW on top, never max(bound, gap). */
static void reserve_bound(ultra_update_t *u,uint64_t ms)
{
    int64_t until=u->poll_end_us+(int64_t)ms*1000+u->gap_us;
    if(until>u->due_us)u->due_us=until;
    u->line->hold_us=u->due_us;
}
static int ordinary(ultra_update_t *u,uint8_t *f,int64_t now,int dark)
{
    const ultra_timing_t *t=&u->line->timing;uint32_t i,k,best=0,base,n=ultra_frame_bytes(u->line->n),lo,hi;
    uint64_t prefix=(uint64_t)(n*8u-1u)*t->bit_ticks,err=UINT64_MAX;
    memset(&u->transport,0,sizeof u->transport);u->transport.marker=u->line->digital;
    u->transport.handback_required=u->line->digital;
    if(u->line->digital){u->transport.lead_low_us=u->gap_us;u->transport.marker_high_us=20;
        u->transport.marker_low_us=10;now+=(int64_t)u->gap_us+30;}
    memset(f,0,n);base=ultra_time_wire((uint64_t)now);
    if(!dark)for(i=0;i<u->line->n;++i)ultra_slot_seal(f+32u*i);
    lo=(uint32_t)((prefix+t->zero_ticks)*1000000u/t->clock_hz);
    hi=(uint32_t)(((prefix+t->one_ticks)*1000000u+t->clock_hz-1u)/t->clock_hz);
    for(k=lo;k<=hi;++k){uint64_t a,b,e;
        ultra_build_header(f+n-8,ultra_time_add(base,k));
        a=(prefix+((f[n-1]&1u)?t->one_ticks:t->zero_ticks))*1000000u;b=(uint64_t)k*t->clock_hz;
        e=a>b?a-b:b-a;if(e<err){err=e;best=k;}
    }
    ultra_build_header(f+n-8,ultra_time_add(base,best));
    u->line->last_start_us=now;u->line->last_time_us=now+best;
    u->poll_end_us=ultra_wire_end_us(t,f,n,now);
    u->transport.frame_start_us=now;u->transport.frame_end_us=u->poll_end_us;u->transport.tail_low_us=u->gap_us;
    u->due_us=u->poll_end_us+u->gap_us;u->line->hold_us=u->due_us;return (int)n;
}
int ultra_update_begin_mode(ultra_update_t *u,ultra_sender_t *line,const upd_descriptor_t *d,const upd_target_t *t,
                        const ultra_update_layout_t *l,const uint8_t uid[12],uint8_t port_type,uint8_t session,uint8_t passes,
                        uint8_t rate,uint8_t mode,int64_t now,ultra_update_read_t read,void *ctx)
{
    ultra_update_t v;unsigned max=ultra_port_max_boards(port_type);int rc;
    if(!u || !line || !max)return -1;
    if(line->n>max)return ULTRA_UPDATE_TOO_MANY_BOARDS;
    if(ultra_port_line_too_fast(port_type,line))return ULTRA_UPDATE_LINE_TOO_FAST;
    if(mode==ULTRA_UPDATE_BLIND && passes<ULTRA_UPDATE_BLIND_MIN_PASSES)return ULTRA_UPDATE_TOO_FEW_PASSES;
    if(!line->n || line->profile_training || line->update_active || line->tx_active || !session || !passes || passes>15u ||
       rate>=3u || mode>ULTRA_UPDATE_SELECTED || now<line->hold_us || (line->has_sent && now<line->last_start_us+line->frame_us))return -1;
    memset(&v,0,sizeof v);
    rc=ultra_update_plan(&v.plan,d,t,l,ultra_timing_gap_us(&line->timing),read,ctx);if(rc)return -rc;
    if(mode==ULTRA_UPDATE_SELECTED && (!v.plan.status || !uid))return -UPD_ERROR_UNSUPPORTED;
    v.line=line;v.read=read;v.context=ctx;v.descriptor=*d;v.target=*t;v.layout=*l;
    if(uid)memcpy(v.uid,uid,12);
    v.session=session;v.passes=passes;v.rate=rate;v.mode=mode;v.epoch=line->profile_epoch;
    v.due_us=now;v.state=ULTRA_UPDATE_PREAMBLE;v.gap_us=ultra_timing_gap_us(&line->timing);
    v.progress_deadline_us=now+(int64_t)UPD_IDLE_TIMEOUT_MS*1000;
    line->update_active=1;line->last_bytes=0;*u=v;return 0;
}
int ultra_update_begin(ultra_update_t *u,ultra_sender_t *line,const upd_descriptor_t *d,const upd_target_t *t,
                        const ultra_update_layout_t *l,const uint8_t uid[12],uint8_t port_type,uint8_t session,uint8_t passes,
                        uint8_t rate,int64_t now,ultra_update_read_t read,void *ctx)
{
    return ultra_update_begin_mode(u,line,d,t,l,uid,port_type,session,passes,rate,ULTRA_UPDATE_BLIND,now,read,ctx);
}
int ultra_update_next_frame(ultra_update_t *u,int64_t now,uint8_t *f,uint32_t capacity)
{
    uint8_t data[32]={0};int rc,first;
    if(!u || !f || !u->line)return -1;
    if(u->state==ULTRA_UPDATE_FAILED)return -1;
    if(u->state==ULTRA_UPDATE_ABORTED){if(now>=u->due_us && u->line->update_active){u->line->update_active=0;ultra_sender_resync(u->line,now);}return 0;}
    if(u->state>=ULTRA_UPDATE_CONFIRM)return 0;
    if(u->epoch!=u->line->profile_epoch)return fail(u,UPD_ERROR_CONFLICT);
    if(now<u->due_us || u->line->tx_active)return 0;
    if(!u->gap_us)u->gap_us=ultra_timing_gap_us(&u->line->timing);
    if(capacity<48u || ((u->state==ULTRA_UPDATE_DARK || u->state==ULTRA_UPDATE_NORMAL_TRAIN ||
        u->state==ULTRA_UPDATE_QUIET) && capacity<ultra_frame_bytes(u->line->n)))return fail(u,UPD_ERROR_BOUNDS);
    switch(u->state) {
    case ULTRA_UPDATE_PREAMBLE:case ULTRA_UPDATE_TRAIN:
        /* A gap of the idle close lets transparency lapse: the count restarts. */
        first=!u->train_until_us || (u->last_training_us && now-u->last_training_us>=(int64_t)UPD_IDLE_CLOSE_MS*1000);
        rc=emit(u,f,UPD_PING,0,0,0,now);u->last_training_us=u->poll_end_us;
        if(first)u->train_until_us=u->poll_end_us+(int64_t)UPD_TRAIN_MIN_MS*1000;
        if(u->poll_end_us>=u->train_until_us){
            u->state=u->state==ULTRA_UPDATE_PREAMBLE?
                (u->mode==ULTRA_UPDATE_SELECTED?ULTRA_UPDATE_SELECT:ULTRA_UPDATE_ANNOUNCE):ULTRA_UPDATE_BOOT_ANNOUNCE;
            u->train_until_us=0;
            u->due_us+=ULTRA_UPDATE_CONTROL_PACE_US;   /* the SELECT / ANNOUNCE after it is a decision frame */
        }else u->due_us+=10000;
        u->line->hold_us=u->due_us;return rc;
    case ULTRA_UPDATE_SELECT:case ULTRA_UPDATE_RESELECT:
        memcpy(data,u->uid,12);memcpy(data+12,u->target.bytes,16);
        rc=emit(u,f,UPD_SELECT,0,0,data,now);
        if(u->state==ULTRA_UPDATE_RESELECT){u->state=ULTRA_UPDATE_TRAIN;u->train_until_us=0;}
        else u->state=ULTRA_UPDATE_ANNOUNCE;
        u->due_us+=ULTRA_UPDATE_CONTROL_PACE_US;u->line->hold_us=u->due_us;
        return rc;
    case ULTRA_UPDATE_ANNOUNCE:case ULTRA_UPDATE_BOOT_ANNOUNCE:
        rc=emit(u,f,UPD_ANNOUNCE,u->half,0,u->descriptor.bytes+32u*u->half,now);
        /* Both halves restart the forwarders' hold from INDEX 0 (timing.md). */
        u->transit_until_us=u->poll_end_us+(int64_t)upd_transit_hold_ms(u->descriptor.bytes)*1000;
        if(u->half==0u){u->half=1;u->due_us+=ULTRA_UPDATE_CONTROL_PACE_US;u->line->hold_us=u->due_us;}
        else{u->half=0;
            if(u->state==ULTRA_UPDATE_ANNOUNCE){u->state=ULTRA_UPDATE_ENTRY;reserve_bound(u,u->plan.entry_ms);}
            else{u->state=(u->mode==ULTRA_UPDATE_SELECTED)?ULTRA_UPDATE_POLL:ULTRA_UPDATE_DATA;
                reserve(u,(u->mode==ULTRA_UPDATE_SELECTED)?0:u->plan.lead_ms);
                u->progress_deadline_us=u->due_us+(int64_t)UPD_IDLE_TIMEOUT_MS*1000;}
            u->line->hold_us=u->due_us;
        }return rc;
    case ULTRA_UPDATE_ENTRY:
        u->state=(u->mode==ULTRA_UPDATE_SELECTED)?ULTRA_UPDATE_RESELECT:ULTRA_UPDATE_TRAIN;u->train_until_us=0;
        return ultra_update_next_frame(u,now,f,capacity);
    case ULTRA_UPDATE_POLL:
        if(now>u->progress_deadline_us)return fail(u,UPD_ERROR_TIMEOUT);
        /* Lost poll: no DATA until readiness arrives; wait the declared worst
         * operation deadline before a new poll, never infer credit from silence. */
        memcpy(data,u->uid,12);data[13]=1;data[14]=u->rate;
        rc=emit(u,f,UPD_STATUS,0,0,data,now);u->poll_pending=1;
        u->due_us=u->poll_end_us+return_wait(u)+
            (int64_t)(u->layout.geometry[2]>u->layout.geometry[1]?u->layout.geometry[2]:u->layout.geometry[1])*2000;
        u->status_deadline_us=u->due_us;u->line->hold_us=u->due_us;return rc;
    case ULTRA_UPDATE_DATA: {
        uint32_t n,index=u->offset/32u;
        if(!(u->mode==ULTRA_UPDATE_SELECTED) && u->offset>=u->plan.length){u->state=ULTRA_UPDATE_END;return ultra_update_next_frame(u,now,f,capacity);}
        if(u->mode==ULTRA_UPDATE_SELECTED) {
            uint32_t block=u->committed/256u*256u;uint8_t k;
            for(k=0;k<8u;++k)if(!(u->mask&(1u<<k)) && block+32u*k<u->plan.length)break;
            if(k==8u || !u->credit){u->state=ULTRA_UPDATE_POLL;return ultra_update_next_frame(u,now,f,capacity);}
            u->offset=block+32u*k;index=u->offset/32u;u->mask|=(uint8_t)(1u<<k);--u->credit;
        }
        n=u->plan.length-u->offset;if(n>32u)n=32;
        if(u->read(u->context,u->offset,data,n))return fail(u,UPD_ERROR_CRC);
        rc=emit(u,f,UPD_DATA,(uint16_t)index,(uint8_t)((u->pass<<4)|u->passes),data,now);
        u->offset+=n;u->transit_until_us=0;
        if(u->mode==ULTRA_UPDATE_SELECTED){
            if(!u->credit || u->offset>=u->plan.length)u->state=ULTRA_UPDATE_POLL;
        } else if(u->offset%256u==0u || u->offset==u->plan.length){
            reserve(u,u->plan.pause_ms);
            if(u->offset==u->plan.length && ++u->pass<u->passes)u->offset=0;
        }
        u->line->hold_us=u->due_us;return rc;
    }
    case ULTRA_UPDATE_END:
        ultra_put32(data,u->plan.body_crc);ultra_put32(data+4,u->plan.length);
        rc=emit(u,f,UPD_END,0,0,data,now);u->transit_until_us=0;
        if(u->mode==ULTRA_UPDATE_SELECTED){u->state=ULTRA_UPDATE_POLL;reserve_bound(u,u->plan.finish_ms);}
        /* Blind: END has no feedback, so it goes out ULTRA_UPDATE_END_COPIES times, each followed by the port's
         * processing LOW (emit) and ULTRA_UPDATE_CONTROL_PACE_US (a receiver's capture mailbox is free again for
         * the next copy); a repeat is a no-op for a receiver that already took END. The commit and reboot waits
         * count from the last copy's physical end. */
        else if(++u->half<ULTRA_UPDATE_END_COPIES){u->due_us+=ULTRA_UPDATE_CONTROL_PACE_US;u->line->hold_us=u->due_us;return rc;}
        else{u->half=0;u->state=ULTRA_UPDATE_QUIET;
            reserve_bound(u,(uint64_t)u->plan.finish_ms+u->plan.entry_ms);reserve(u,ULTRA_UPDATE_QUIET_MS);}
        u->line->hold_us=u->due_us;return rc;
    case ULTRA_UPDATE_REBOOT:
        /* Idle PING confirmation write has its own conservative busy interval. A lost copy must not leave the
         * application unconfirmed: ULTRA_UPDATE_CONFIRM_COPIES copies, each with its own pause (a confirmed
         * application ignores a repeat). The quiet LOW counts from the last one. */
        {uint8_t session=u->session;u->session=0;rc=emit(u,f,UPD_PING,0,0,0,now);u->session=session;}
        reserve_bound(u,u->plan.finish_ms);
        if(++u->half<ULTRA_UPDATE_CONFIRM_COPIES){u->line->hold_us=u->due_us;return rc;}
        u->half=0;reserve(u,ULTRA_UPDATE_QUIET_MS);
        u->state=ULTRA_UPDATE_CONFIRM_WAIT;return rc;
    case ULTRA_UPDATE_QUIET:
        u->state=ULTRA_UPDATE_DARK;u->dark_left=ultra_preamble_frames(u->line->n);
        return ultra_update_next_frame(u,now,f,capacity);
    case ULTRA_UPDATE_DARK:
        rc=ordinary(u,f,now,1);
        if(!--u->dark_left){u->state=ULTRA_UPDATE_NORMAL_TRAIN;u->train_until_us=0;}
        return rc;
    case ULTRA_UPDATE_NORMAL_TRAIN:
        /* Rate lock before replies; a gap that unlocks receivers restarts it. */
        first=!u->train_until_us || now-u->last_training_us>=(int64_t)ULTRA_RATE_LOCK_LOSS_MS*1000;
        rc=ordinary(u,f,now,0);u->last_training_us=u->poll_end_us;
        if(first)u->train_until_us=u->poll_end_us+(int64_t)ULTRA_RW_LOCK_MS*1000;
        if(u->poll_end_us>=u->train_until_us)u->state=ULTRA_UPDATE_REBOOT;
        else u->due_us+=10000;
        u->line->hold_us=u->due_us;return rc;
    case ULTRA_UPDATE_CONFIRM_WAIT:
        u->state=ULTRA_UPDATE_CONFIRM;u->transfer_complete=1;u->line->update_active=2;
        ultra_sender_resync(u->line,now);return 0;
    case ULTRA_UPDATE_ACK:
        /* Selected-session keepalive acknowledges the observed commit. Repeat
         * across separate entry bounds; a lost first PING cannot shorten reboot. */
        rc=emit(u,f,UPD_PING,0,0,0,now);
        reserve_bound(u,u->plan.entry_ms);
        u->line->hold_us=u->due_us;
        if(++u->half>=ULTRA_UPDATE_ACK_COPIES){u->half=0;u->state=ULTRA_UPDATE_QUIET;reserve(u,ULTRA_UPDATE_QUIET_MS);}
        return rc;
    default:return 0;
    }
}
int ultra_update_next(ultra_update_t *u,int64_t now,uint8_t f[48])
{return ultra_update_next_frame(u,now,f,48);}
uint16_t ultra_port_max_boards(uint8_t port_type)
{
    return port_type==ULTRA_PORT_DIFF_POWER?ULTRA_PORT_DIFF_POWER_MAX_BOARDS:
           port_type==ULTRA_PORT_D2A_PIN?ULTRA_PORT_D2A_PIN_MAX_BOARDS:0u;
}
uint8_t ultra_port_line_profile(uint8_t port_type,uint16_t boards,uint8_t *profile)
{
    uint16_t max=ultra_port_max_boards(port_type);uint8_t p=255u,why;
    if(!max)why=ULTRA_PORT_PROFILE_UNKNOWN_TYPE;
    else if(!boards)why=ULTRA_PORT_PROFILE_NO_BOARDS;
    else if(boards>max)why=ULTRA_PORT_PROFILE_TOO_MANY;
    else if(port_type!=ULTRA_PORT_D2A_PIN || boards<=ULTRA_D2A_800_MAX_BOARDS){p=ULTRA_PROFILE_800;why=ULTRA_PORT_PROFILE_DEFAULT;}
    /* The pin table never picks 400 kHz (timing.md, D2A pin link model). */
    else{p=boards<=ULTRA_D2A_600_MAX_BOARDS?ULTRA_PROFILE_600:ULTRA_PROFILE_200;why=ULTRA_PORT_PROFILE_BOARD_COUNT;}
    if(profile)*profile=p;
    return why;
}
int ultra_port_line_too_fast(uint8_t port_type,const ultra_sender_t *line)
{
    uint8_t p;
    return port_type==ULTRA_PORT_D2A_PIN && line &&
        ultra_port_line_profile(port_type,line->n,&p)<=ULTRA_PORT_PROFILE_BOARD_COUNT && line->timing.profile<p;
}
int ultra_update_begin_group(ultra_update_t *u,ultra_sender_t *line,const upd_descriptor_t *image,
                              const ultra_update_participant_t *nodes,uint16_t count,uint8_t port_type,
                              uint8_t session,uint8_t passes,int64_t now,ultra_update_read_t read,void *ctx)
{
    ultra_update_plan_t slow,p;unsigned i,j,max=ultra_port_max_boards(port_type);int rc;
    if(!u || !line || !nodes || !count || !max)return -1;
    if(count>max || line->n>max)return ULTRA_UPDATE_TOO_MANY_BOARDS;
    if(ultra_port_line_too_fast(port_type,line))return ULTRA_UPDATE_LINE_TOO_FAST;
    if(passes<ULTRA_UPDATE_BLIND_MIN_PASSES)return ULTRA_UPDATE_TOO_FEW_PASSES;
    memset(&slow,0,sizeof slow);
    for(i=0;i<count;++i){
        rc=ultra_update_plan_metadata(&p,image,&nodes[i].target,&nodes[i].layout,
                              ultra_timing_gap_us(&line->timing));
        if(rc)return -rc;
        for(j=0;j<i;++j)if(nodes[i].has_return && nodes[j].has_return && !memcmp(nodes[i].uid,nodes[j].uid,12))return -UPD_ERROR_TARGET;
        if(!i)slow=p;
        if(p.lead_ms>slow.lead_ms)slow.lead_ms=p.lead_ms;
        if(p.pause_ms>slow.pause_ms)slow.pause_ms=p.pause_ms;
        if(p.finish_ms>slow.finish_ms)slow.finish_ms=p.finish_ms;
        if(p.entry_ms>slow.entry_ms)slow.entry_ms=p.entry_ms;
    }
    rc=ultra_update_begin(u,line,image,&nodes[0].target,&nodes[0].layout,nodes[0].uid,
                          port_type,session,passes,2,now,read,ctx);
    if(rc)return rc;
    u->plan=slow;u->participants=nodes;u->participant_count=(uint8_t)count;
    u->confirmations=(uint8_t)count;return 0;
}
int ultra_update_answer(ultra_update_t *u,const uint8_t a[32],int64_t now)
{
    ultra_update_status_t s;uint32_t wait;
    if(!u || !u->poll_pending || u->state!=ULTRA_UPDATE_POLL || now<u->poll_end_us ||
       now>u->poll_end_us+u->transport.listen_us ||
       ultra_update_status_decode(&s,a,u->session,u->uid,u->plan.length,u->plan.body_crc))return -1;
    if(s.committed<u->committed)return fail(u,UPD_ERROR_CONFLICT);
    u->poll_pending=0;u->committed=s.committed;u->mask=(uint8_t)s.chunk_mask;u->credit=s.credit;
    wait=(uint32_t)return_wait(u);
    u->due_us=u->poll_end_us+wait;
    if(s.state==UPD_STATE_FAILED || s.state==UPD_STATE_RESTART_REQUIRED){u->line->hold_us=u->status_deadline_us;return fail(u,s.error?s.error:UPD_ERROR_SESSION);}
    if(s.state==UPD_STATE_BUSY)u->due_us+=(int64_t)s.retry_ms*1000;
    else if(s.state==UPD_STATE_COMMITTED){u->state=ULTRA_UPDATE_ACK;u->half=0;}
    else if(s.committed==u->plan.length){u->offset=s.committed;u->state=ULTRA_UPDATE_END;}
    else if(s.credit && (s.state==UPD_STATE_READY || s.state==UPD_STATE_RECEIVE))u->state=ULTRA_UPDATE_DATA;
    u->progress_deadline_us=u->due_us+(int64_t)UPD_IDLE_TIMEOUT_MS*1000;
    u->line->hold_us=u->due_us;return 0;
}
int ultra_update_confirm(ultra_update_t *u,const ultra_identity_t *id,const uint8_t a[32])
{
    const uint8_t *v;unsigned member=0;
    if(!u || !id || !a || u->state!=ULTRA_UPDATE_CONFIRM || (id->parts&5u)!=5u ||
       id->firmware!=u->plan.firmware ||
       ultra_answer_check(a) || a[1]!=ULTRA_PAGE_FW_UPDATE || ultra_get16(a+4)!=ultra_crc16(id->uid,12))return -1;
    if(u->participants){
        for(member=0;member<u->participant_count;++member)
            if(u->participants[member].has_return && !memcmp(u->participants[member].uid,id->uid,12) &&
               !memcmp(u->participants[member].target.bytes,id->target.bytes,16))break;
        if(member==u->participant_count)return -1;
    }else if(memcmp(id->target.bytes,u->target.bytes,16) || memcmp(id->uid,u->uid,12))return -1;
    v=a+6;
    if(v[0]!=1u || (v[1]&3u)!=3u || ultra_get32(v+4)!=u->plan.firmware || ultra_get32(v+8)!=u->plan.length ||
       ultra_get32(v+12)!=u->plan.body_crc || ultra_get32(v+16)!=u->plan.descriptor_crc)return -1;
    if(u->participants){uint8_t bit=(uint8_t)(1u<<(member%8u));
        if(!(u->confirmed[member/8u]&bit)){u->confirmed[member/8u]|=bit;--u->confirmations;}
        if(u->confirmations)return 0;}
    u->state=ULTRA_UPDATE_DONE;u->line->update_active=0;return 0;
}
int ultra_update_abort(ultra_update_t *u,int64_t now,uint8_t f[48])
{
    int rc;int64_t free_us;
    if(!u || !u->line || !f || now<u->due_us || now<u->line->hold_us || u->line->tx_active)return 0;
    if(u->state==ULTRA_UPDATE_ABORTED || u->state==ULTRA_UPDATE_DONE)return 0;
    /* An ordinary frame sent after this transfer's last frame (verification
     * traffic) keeps its whole duration plus the port's processing LOW. */
    if(u->line->last_start_us>u->transport.frame_start_us){
        free_us=u->line->last_start_us+(int64_t)u->line->frame_us+u->gap_us;
        if(now<free_us){u->due_us=free_us;return 0;}
    }
    rc=emit(u,f,UPD_ABORT,0,0,0,now);u->state=ULTRA_UPDATE_ABORTED;
    reserve(u,ULTRA_UPDATE_QUIET_MS);
    /* Abandoned after an ANNOUNCE: a hop that missed this ABORT still holds
     * transparency, so wait its hold and the idle close out (timing.md). */
    if(u->transit_until_us){int64_t until=u->transit_until_us+(int64_t)ULTRA_UPDATE_QUIET_MS*1000;
        if(until>u->due_us)u->due_us=until;}
    u->line->hold_us=u->due_us;u->line->update_active=1;return rc;
}
