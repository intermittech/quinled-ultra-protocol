/* SPDX-License-Identifier: MIT
 * Shared binary fixtures through the actual C99 sender implementation.
 * This same file is compiled as C and C++ and is suitable for receiver CI. */
#include <stdio.h>
#include <string.h>
#include "ultra_sender_descriptor.h"
#include "ultra_sender_profile.h"
#include "ultra_sender_rc.h"
#include "ultra_sender_update.h"
#include "ultra_sender_job.h"
#include "../../test-vectors/shared_vectors.h"
#include "../../test-vectors/golden_traces.h"
#include "../../test-vectors/local_vectors.h"
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}}while(0)
static const ultra_shared_vector_t *find(const char *name)
{
    unsigned i;for(i=0;i<ULTRA_SHARED_VECTOR_COUNT;++i)if(!strcmp(name,ultra_shared_vectors[i].name))return &ultra_shared_vectors[i];
    return NULL;
}
static ultra_sender_t line;
static ultra_sender_board_t boards[8];
static ultra_sender_item_t items[512];
static uint8_t frame[264];
static int64_t now;
static void setup(unsigned lights)
{
    ultra_sender_config_t c;ultra_sender_config_init(&c,1,(uint8_t)lights);c.clear_unused=0;
    CHECK(!ultra_sender_init(&line,&c,boards,items,512));now=1000000;
    while(line.preamble_left){CHECK(ultra_sender_next_frame(&line,now,frame)==40);now+=line.frame_us;}
}
static void envelope(uint8_t *a,uint8_t page,uint8_t session,const uint8_t *uid,const uint8_t *data)
{
    memset(a,0,32);a[0]=0xa5;a[1]=page;a[3]=session;ultra_put16(a+4,ultra_crc16(uid,12));memcpy(a+6,data,24);ultra_slot_seal(a);
}
static void all_vectors(void)
{
    unsigned i,j;
    for(i=0;i<ULTRA_SHARED_VECTOR_COUNT;++i){
        const ultra_shared_vector_t *v=&ultra_shared_vectors[i];const uint8_t *b=v->bytes;uint8_t out[6144]={0};
        if(!strcmp(v->kind,"header")){
            uint32_t time;CHECK(!ultra_parse_header(b,&time));ultra_build_header_flags(out,time,ultra_header_flags(b));
        }else if(!strcmp(v->kind,"slot")){
            uint8_t type=b[0]&31u;CHECK(!ultra_slot_check(b));
            if(type==ULTRA_NOP){ultra_slot_training_nop(out,b[3]);}
            else if(type==ULTRA_SET){uint16_t a[5],c[5];for(j=0;j<5;++j){a[j]=(uint16_t)ultra_get16(b+5+2*j);c[j]=(uint16_t)ultra_get16(b+15+2*j);}
                ultra_slot_set(out,b[1],b[2],b[0]&96u,(uint16_t)ultra_get16(b+3),a,c,ultra_get24(b+25),(int16_t)ultra_get16(b+28));
            }else if(type==ULTRA_LIGHT_DEF){uint8_t roles[5];uint16_t min[5],max[5];
                for(j=0;j<5;++j){roles[j]=(uint8_t)((b[3+j/2]>>((j&1)?0:4))&15);min[j]=(uint16_t)ultra_get16(b+9+2*j);max[j]=(uint16_t)ultra_get16(b+19+2*j);}
                ultra_slot_light_def(out,b[1],b[2],roles,(uint16_t)ultra_get16(b+6),b[8],min,max,b[29]);
            }else if(type==ULTRA_CONFIG){uint8_t ids[9];uint16_t values[9];for(j=0;j<9;++j){ids[j]=b[3+3*j];values[j]=(uint16_t)ultra_get16(b+4+3*j);}
                ultra_slot_config(out,b[1],b[2],ids,values,9);
            }else if(type==ULTRA_PIXEL_FX){ultra_pixel_fx_t fx;CHECK(ultra_pixel_fx_parse(b,&fx)==b[1]);ultra_slot_pixel_fx(out,b[1]>>4,b[1]&15,b[2],&fx);
            }else if(type==ULTRA_POLL){ultra_slot_poll(out,b[2],b[3],b[4],b[5],b[6],b+7,23);CHECK(!ultra_poll_valid(out));
            }else if(type==ULTRA_PIXEL_DATA){CHECK(!ultra_slot_pixel_data(out,b[1],b[2],b[3],(uint16_t)ultra_get16(b+4),b+6));}
            else {ultra_slot_begin(out,type,b[1],b[0]&96,b[2]);memcpy(out+3,b+3,27);ultra_slot_seal(out);}
        }else if(!strcmp(v->kind,"answer")){
            uint8_t rx[34];rx[0]=0xa5;rx[1]=0xff;memcpy(rx+2,b,32);
            CHECK(!ultra_answer_check(b));CHECK(!ultra_rc_answer(rx,NULL,34,out));
        }else if(!strcmp(v->kind,"descriptor")){
            ultra_descriptor_t d;ultra_tlv_t tlv;uint16_t pos=11,dst=11;
            CHECK(!ultra_descriptor_decode(&d,b,v->size));memcpy(out,b,11);
            while(ultra_descriptor_next(&d,&pos,&tlv)>0){out[dst++]=tlv.type;out[dst++]=tlv.length;memcpy(out+dst,tlv.value,tlv.length);dst=(uint16_t)(dst+tlv.length);}
            CHECK(dst==v->size);ultra_put16(out+4,ultra_crc16(out+6,dst-6));
        }else if(!strcmp(v->kind,"target")){
            memcpy(out,b,8);ultra_put32(out+8,ultra_get32(b+8));ultra_put32(out+12,ultra_get32(b+12));
        }else if(!strcmp(v->kind,"boot_info")){
            upd_bl_info_t info;memcpy(&info,b,sizeof info);memcpy(out,info.magic,4);out[4]=info.bl_version;
            memcpy(out+5,info.target.bytes,16);memcpy(out+21,info.geometry,3);out[24]=info.entry_10ms;memcpy(out+25,info.build_id,7);
        }else if(!strcmp(v->kind,"image")){
            memcpy(out,b,60);ultra_put32(out+60,upd_crc32_words(0xffffffffu,out,15));
        }else if(!strcmp(v->kind,"update")){
            CHECK(!upd_check_frame(b));upd_build_frame(out,b[0],b[1],(uint16_t)ultra_get16(b+2),b[4],b[5],b+6,ultra_get32(b+43));
        }else CHECK(0);
        if(memcmp(out,b,v->size)){printf("vector %s differs\n",v->name);CHECK(0);}else CHECK(1);
    }
}
static void pixel_fx_acknowledgement(void)
{
    const uint8_t *ack=find("answer_acks_pixel_fx_never")->bytes+ULTRA_RSP_DATA;
    const uint8_t *state=find("answer_pixel_fx_output1_group3")->bytes+ULTRA_RSP_DATA;
    const uint8_t *poll=find("poll_pixel_fx_output1_group3")->bytes;
    unsigned row;uint32_t never=ultra_get24(ack+ULTRA_AK_NEVER);
    for(row=0;row<6;++row)CHECK(!ack[3*row] && (never&((uint32_t)1u<<(3*row))));
    CHECK(ack[1]==0x21 && ack[4]==0x22 && ack[16]==0x33);
    CHECK(poll[ULTRA_POLL_BANK]==1 && poll[8]==14u/4u);
    CHECK(state[ULTRA_PXG_SEGMENTS]==15 && state[ULTRA_PXG_SEQ+14u%4u]==0x49);
    CHECK(state[ULTRA_PXG_RUNNING+14u%4u]==1 && !state[ULTRA_PXG_SEQ+3] && !state[ULTRA_PXG_RUNNING+3]);
}
static void local_scheduling(void)
{
    unsigned i;ultra_descriptor_t d;ultra_desc_link_t l;int64_t start;
    const char *names[]={"descriptor_local10","descriptor_local_unbounded","descriptor_local_unknown_jitter","descriptor_local_extended"};
    for(i=0;i<4;++i){
        const ultra_shared_vector_t *v=find(names[i]);
        CHECK(!ultra_descriptor_decode(&d,v->bytes,v->size));
        CHECK(!ultra_descriptor_link(&d,0,&l));
        CHECK(l.local_latency_us==(i==1?65535:40));
        CHECK(l.local_jitter_us==((i==1 || i==2)?65535:50));
    }
    for(i=0;i<sizeof ultra_local_vectors/sizeof ultra_local_vectors[0];++i){
        const ultra_local_vector_t *v=ultra_local_vectors+i;
        CHECK(!ultra_local_check(v->packet,v->size));
        if(v->timed)CHECK(!ultra_local_start_us(v->packet,v->size,v->accepted_us,&start) && start==v->start_us);
    }
}
static void descriptors(void)
{
    const char *names[6]={"nano2","partial7","local10","analog20","dual_pixel","single_pixel"};unsigned i,j;
    for(i=0;i<6;++i){
        char name[80];const ultra_shared_vector_t *v;ultra_descriptor_t d;ultra_identity_t id={0};
        uint8_t blob[6144],part[24],bad[6144];ultra_descriptor_rx_t rx={blob,sizeof blob,0,0};
        snprintf(name,sizeof name,"descriptor_%s",names[i]);v=find(name);CHECK(v!=NULL);if(!v)continue;
        CHECK(!ultra_descriptor_decode(&d,v->bytes,v->size));
        for(j=0;j<(v->size+23u)/24u;++j){unsigned n=v->size-j*24u;if(n>24)n=24;
            memset(part,0,24);memcpy(part,v->bytes+24u*j,n);
            CHECK(ultra_descriptor_part(&rx,(uint8_t)j,part)==(j*24u+24u>=v->size));
        }
        CHECK(!memcmp(blob,v->bytes,v->size));
        CHECK(ultra_descriptor_part(&rx,(uint8_t)((v->size-1u)/24u),part)==1);
        snprintf(name,sizeof name,"identity_part0_%s",names[i]);CHECK(!ultra_identity_decode(&id,0,find(name)->bytes));
        snprintf(name,sizeof name,"identity_part2_%s",names[i]);CHECK(!ultra_identity_decode(&id,2,find(name)->bytes));
        CHECK(id.descriptor_length==d.length && id.descriptor_crc==d.crc && id.target.bytes[6]==d.variant);
        memcpy(bad,v->bytes,v->size);bad[v->size-1]^=1;CHECK(ultra_descriptor_decode(&d,bad,v->size)!=0);
    }
    {
        ultra_identity_t id={0};
        CHECK(!ultra_identity_decode(&id,1,find("identity_features_all_words")->bytes));
        CHECK(id.features[0]==0x12345678 && id.features[3]==0x80000000 && id.features[5]==0x55aa55aa);
    }
    {
        ultra_descriptor_t d;ultra_bank_view_t bank;ultra_desc_sense_t sense;uint16_t applied;uint32_t div;
        const ultra_shared_vector_t *v=find("descriptor_partial7");CHECK(!ultra_descriptor_decode(&d,v->bytes,v->size));
        CHECK(!ultra_descriptor_bank(&d,ULTRA_PAGE_OUTPUTS,0,0,1,&bank) && bank.channels==3);
        CHECK(ultra_descriptor_bank(&d,ULTRA_PAGE_OUTPUTS,0,0,2,&bank)!=0);
        CHECK(!ultra_descriptor_config(&d,6,12,60000,&applied) && applied<=60000);
        CHECK(ultra_descriptor_config(&d,7,12,100,&applied)!=0);
        v=find("descriptor_sense_10a_board");CHECK(!ultra_descriptor_decode(&d,v->bytes,v->size));
        CHECK(!ultra_descriptor_sense(&d,0,&sense) && ultra_sense_full_scale(&sense,&div)==16500 && div==1);
        CHECK(ultra_power_mw(65000,4800)==3120000);
        CHECK(ultra_lifetime_energy_allowance(65000,4800,0,UINT32_MAX)==3120);
        CHECK(!ultra_lifetime_energy_allowance(65535,4800,0,UINT32_MAX));
    }
}
static void scoped_sender(void)
{
    ultra_values_t values={0};ultra_light_def_t def;unsigned i;uint8_t seq=0;
    setup(240);ultra_light_def_init(&def);def.base_channel=235;def.roles[4]=1;
    CHECK(!ultra_sender_light_def(&line,0,239,&def));
    CHECK(!ultra_sender_set_at(&line,0,239,&values,100,96,now+5000));
    CHECK(!ultra_sender_config_target(&line,0,19,11,65000));
    CHECK(!ultra_sender_config_target(&line,0,19,12,5000));
    CHECK(!ultra_sender_pixel_config(&line,0,1,ULTRA_CFG_PX_SOURCE,2));
    for(i=0;i<24;++i){uint32_t n=ultra_sender_next_frame(&line,now,frame);now+=line.frame_us;
        if(n && (frame[0]&31)==ULTRA_SET)CHECK(frame[0]==0x62 && frame[1]==239 && !ultra_slot_check(frame));
        if(n && frame[0]==ULTRA_CONFIG && frame[1]==19){CHECK(frame[3]==11 && frame[6]==12);seq=frame[2];}
    }
    CHECK(seq!=0);CHECK(ultra_sender_config(&line,0,11,100)!=0);
    CHECK(ultra_sender_config_target(&line,0,255,77,1)!=0);
    CHECK(ultra_sender_config_target(&line,0,240,12,100)!=0);
    CHECK(ultra_sender_config_target(&line,0,19,12,65535)!=0);
    {
        uint8_t records[16]={19,11,0xfd,0xe8,19,12,0x13,0x88,1,77,0,2,255,21,0,0};
        /* Canonical target order is output 1, channel 19, board 255. */
        uint8_t canonical[12];memcpy(canonical,records+8,4);memcpy(canonical+4,records,8);
        CHECK(ultra_sender_config_crc(&line,0)==ultra_crc16(canonical,12));
    }
}
/* CHAIN_END excludes FORWARD and RETURN_RELAY only: a last board may still pass update sessions on (UPDATE_FWD) */
static void descriptor_chain_end(void)
{
    static const struct { uint8_t flags; int ok; } c[] = {
        {ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END | ULTRA_LINK_MARKER | ULTRA_LINK_UPDATE_FWD, 1},
        {ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END, 1},
        {ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END | ULTRA_LINK_FORWARD, 0},
        {ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END | ULTRA_LINK_RELAY, 0},
        {ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END | ULTRA_LINK_RELAY | ULTRA_LINK_FORWARD | ULTRA_LINK_UPDATE_FWD, 0},
    };
    const ultra_shared_vector_t *v = find("descriptor_single_pixel");
    ultra_descriptor_t d; ultra_tlv_t tlv; uint8_t b[512]; uint16_t pos = 11; unsigned i, off = 0;
    CHECK(v->size <= sizeof b);
    memcpy(b, v->bytes, v->size);
    CHECK(!ultra_descriptor_decode(&d, b, v->size));
    while (ultra_descriptor_next(&d, &pos, &tlv) > 0) if (tlv.type == 1u) { off = (unsigned)(tlv.value - b) + 1u; break; }
    CHECK(off != 0u);
    for (i = 0; i < sizeof c / sizeof c[0]; ++i) {
        b[off] = c[i].flags;
        ultra_put16(b + 4, ultra_crc16(b + 6, v->size - 6u));
        CHECK((ultra_descriptor_decode(&d, b, v->size) == 0) == c[i].ok);
    }
}
static void descriptor_admission(void)
{
    ultra_descriptor_t d;ultra_tlv_t tlv;uint8_t bad[6144];uint16_t pos,applied;unsigned type;
    const ultra_shared_vector_t *v=find("descriptor_dual_pixel");
    CHECK(!ultra_descriptor_decode(&d,v->bytes,v->size));
    setup(1);CHECK(!ultra_sender_descriptor(&line,0,&d));
    CHECK(!ultra_sender_pixel_config(&line,0,0,69,60000));
    CHECK(!ultra_descriptor_config(&d,0,69,60000,&applied) && applied<60000);
    CHECK(!ultra_sender_pixel_config(&line,0,0,64,0));
    CHECK(!ultra_sender_pixel_config(&line,0,0,66,65535));
    CHECK(ultra_sender_pixel_config(&line,0,14,69,100)!=0);
    for(type=1;type<=8;++type){
        memcpy(bad,v->bytes,v->size);pos=11;
        while(ultra_descriptor_next(&d,&pos,&tlv)>0)if(tlv.type==type){
            uint16_t off=(uint16_t)(tlv.value-v->bytes-1);
            if(tlv.length)bad[off]=(uint8_t)(tlv.length-1u);
            ultra_put16(bad+4,ultra_crc16(bad+6,v->size-6u));
            CHECK(ultra_descriptor_decode(&d,bad,v->size)!=0);
            CHECK(!ultra_descriptor_decode(&d,v->bytes,v->size));break;
        }
    }
}
static void policy(void)
{
    ultra_auto_t state;ultra_auto_input_t in;ultra_auto_result_t r;unsigned i;
    memset(&in,0,sizeof in);in.valid=1;in.idle=1;in.margin_ns=26;
    in.evidence.inventory_complete=1;in.evidence.all_ultra=1;in.evidence.common_mask=15;
    in.evidence.qualified_mask=in.evidence.observed_mask=in.evidence.gate_mask=15;
    in.evidence.path_ns=100;in.evidence.min_gap_us=500;ultra_auto_init(&state,0);
    for(i=1;i<=70;++i){in.now_ms=i*1000;r=ultra_auto_step(&state,&in);CHECK(!r.action && state.active==0);}
    in.margin_ns=19;for(i=0;i<3;++i){in.now_ms+=1000;r=ultra_auto_step(&state,&in);CHECK(i==2?r.action==ULTRA_AUTO_DOWN:r.action==0);}
    CHECK(state.active==1);in.profile=1;in.margin_ns=100;
    for(i=0;i<59;++i){in.now_ms+=1000;r=ultra_auto_step(&state,&in);CHECK(r.action==0);}
    in.now_ms+=1000;r=ultra_auto_step(&state,&in);CHECK(r.action==ULTRA_AUTO_PROBE && state.active==0);
    in.profile=0;for(i=0;i<60;++i){in.now_ms+=1000;r=ultra_auto_step(&state,&in);}
    CHECK(r.action==ULTRA_AUTO_UP && !state.probing && !state.warning);
    in.margin_ns=0;in.now_ms+=1000;r=ultra_auto_step(&state,&in);CHECK(r.stop_pixels && r.action==ULTRA_AUTO_RECOVER && state.active==3);
    in.profile=3;in.valid=0;in.now_ms+=1000;r=ultra_auto_step(&state,&in);CHECK(r.state==ULTRA_AUTO_UNKNOWN && !r.action);
    in.valid=1;in.margin_ns=100;in.evidence.all_ultra=0;state.mode=2;in.now_ms+=1000;r=ultra_auto_step(&state,&in);
    CHECK(r.state==ULTRA_AUTO_FAILED && !r.action);
    {
        ultra_profile_evidence_t e={1,1,15,15,15,15,218,500};
        setup(1);line.tx_active=1;
        CHECK(ultra_sender_profile(&line,1,40000000,&e,now)!=0);line.tx_active=0;
        line.update_active=1;CHECK(ultra_sender_profile(&line,1,40000000,&e,now)!=0);line.update_active=0;
        e.all_ultra=0;CHECK(ultra_sender_profile(&line,1,40000000,&e,now)!=0);e.all_ultra=1;
        CHECK(!ultra_sender_profile(&line,1,40000000,&e,now));
        CHECK(line.timing.bit_ticks==67 && line.preamble_left==6 && line.profile_epoch==1);
        CHECK(!ultra_sender_retime(&line,now,frame));
    }
}
static void acquisition_training(void)
{
    ultra_profile_evidence_t e={1,1,15,15,0,15,100,500};ultra_values_t value={0};unsigned i;
    static const char *names[4]={"slot_training_55","slot_training_aa","slot_training_00","slot_training_ff"};
    setup(1);CHECK(ultra_sender_profile(&line,3,40000000,&e,now)!=0);
    CHECK(!ultra_sender_profile_acquire(&line,3,40000000,&e,now) && line.profile_training);
    while(line.preamble_left){CHECK(ultra_sender_next_frame(&line,now,frame)==40);now+=line.frame_us;}
    CHECK(!ultra_sender_set_at(&line,0,0,&value,0,0,now));
    CHECK(!ultra_sender_next_frame(&line,now,frame) && ultra_sender_pending(&line));
    for(i=0;i<4;++i){CHECK(ultra_sender_training_frame(&line,(uint8_t)i,&e,now,frame)==40);
        CHECK(!memcmp(frame,find(names[i])->bytes,32));now+=200000;}
    CHECK(ultra_sender_profile_accept(&line,&e)!=0);
    e.observed_mask=8;CHECK(!ultra_sender_profile_accept(&line,&e) && !line.profile_training);
    CHECK(ultra_sender_next_frame(&line,now,frame)==40 && (frame[0]&31u)==ULTRA_SET);
    e.all_ultra=0;CHECK(!ultra_sender_training_frame(&line,0,&e,now+1000000,frame));
    {
        ultra_auto_t s;ultra_auto_input_t in;ultra_auto_result_t r;
        ultra_auto_init(&s,0);memset(&in,0,sizeof in);in.evidence=e;in.evidence.all_ultra=1;in.repeated_loss=1;in.now_ms=1000;
        r=ultra_auto_step(&s,&in);CHECK(r.action==ULTRA_AUTO_RECOVER && r.stop_pixels);
        ultra_auto_init(&s,0);s.probing=1;s.last_good=1;
        in.repeated_loss=0;in.valid=1;in.profile=0;in.margin_ns=10;in.update_active=1;in.now_ms=2000;
        r=ultra_auto_step(&s,&in);CHECK(!r.action && s.probing==2 && r.stop_pixels);
        in.valid=0;in.update_active=0;in.now_ms=3000;
        r=ultra_auto_step(&s,&in);CHECK(r.action==ULTRA_AUTO_REVERT && r.profile==1 && !s.probing);
    }
}
static void stats(void)
{
    ultra_link_stats_t s={0};int32_t margin;uint8_t bad[32];
    CHECK(!ultra_link_stats_decode(&s,0,find("answer_link_stats")->bytes,1,1000));
    CHECK(!ultra_link_stats_decode(&s,1,find("answer_link_context_0")->bytes,1,1100));
    CHECK(!s.rate_request);
    CHECK(!ultra_link_stats_margin(&s,1,1200,598,625,&margin));
    CHECK(ultra_link_stats_margin(&s,1,3101,598,625,&margin)!=0);
    CHECK(ultra_link_stats_margin(&s,2,1200,598,625,&margin)!=0);
    CHECK(!ultra_link_stats_decode(&s,0,find("answer_link_stats")->bytes,1,4000));
    memcpy(bad,find("answer_link_context_0")->bytes,32);bad[11]^=1;ultra_slot_seal(bad);
    CHECK(ultra_link_stats_decode(&s,1,bad,1,4100)!=0);
    CHECK(!ultra_link_stats_decode(&s,0,find("answer_link_stats_rate_request")->bytes,1,5000));
    CHECK(s.rate_request && (s.flags&ULTRA_LS_RATE_REQUEST));
    memcpy(bad,find("answer_link_context_0")->bytes,32);
    CHECK(ultra_link_stats_decode(&s,1,bad,1,5100)!=0); /* flags must match */
    bad[ULTRA_RSP_DATA+ULTRA_LS_FLAGS]|=ULTRA_LS_RATE_REQUEST;ultra_slot_seal(bad);
    CHECK(!ultra_link_stats_decode(&s,1,bad,1,5100));
    CHECK(s.parts==3 && s.rate_request && (s.flags&ULTRA_LS_RATE_REQUEST));
}
static void parlio(void)
{
    ultra_parlio_lane_t lanes[4]={{0}};ultra_parlio_group_t groups[4];uint8_t bytes[2]={0x80,0};
    uint32_t out[128],completed=0,done,k,seen[4]={0};unsigned i;uint32_t boundaries[4]={4992,5056,5184,5568};
    for(i=0;i<4;++i)CHECK(!ultra_parlio_begin(&lanes[i],(uint8_t)i,(uint8_t)i,bytes,2,9600000,500));
    CHECK(!ultra_parlio_groups(lanes,4,groups));for(i=0;i<4;++i)CHECK(groups[i].lanes==(1u<<i));
    for(k=0;k<5600;++k){ultra_parlio_encode(lanes,4,out,1,&done);completed|=done;
        if(k==0)CHECK(out[0]==15);if(k==3)CHECK((out[0]&1u)!=0);if(k==9)CHECK(!(out[0]&1u));
        for(i=0;i<4;++i)if(done&(1u<<i)){seen[i]=k+1;CHECK(seen[i]==boundaries[i]);}
        if(k==boundaries[0]-1u)CHECK(!ultra_parlio_begin(&lanes[0],0,0,bytes,2,9600000,500));
    }
    CHECK(completed==15 && lanes[0].active && !lanes[3].active);
    CHECK(ultra_parlio_begin(&lanes[3],3,3,bytes,2,9599999,500)!=0);
}
static void golden_traces(void)
{
    unsigned i,j;
    for(i=0;i<sizeof ultra_golden_traces/sizeof ultra_golden_traces[0];++i){
        const ultra_trace_t *g=&ultra_golden_traces[i];ultra_timing_t timing;ultra_sender_config_t c;
        ultra_pulse_t pulses[320];uint32_t pos=0;uint64_t ticks=0;
        CHECK(!ultra_timing_profile(&timing,(uint8_t)g->profile,g->clock_hz,500));
        CHECK(timing.bit_ticks==g->period && timing.zero_ticks==g->zero && timing.one_ticks==g->one);
        ultra_sender_config_init(&c,1,0);c.light_capacity=0;c.clear_unused=0;c.timing=timing;
        CHECK(!ultra_sender_init(&line,&c,boards,items,512));line.preamble_left=0;
        CHECK(ultra_sender_next_frame(&line,g->start,frame)==40);
        CHECK(!memcmp(frame,g->frame,40) && ultra_time_wire((uint64_t)line.last_time_us)==g->time);
        CHECK(ultra_encode_pulses(&timing,frame,40,&pos,pulses,320)==320);
        for(j=0;j<320;++j){ticks+=pulses[j].high_ticks;if(j<319)ticks+=pulses[j].low_ticks;}
        CHECK(ticks==g->edge_ticks);
        if(g->clock_hz!=9600000u){
            ultra_rmt_t r;uint32_t words[320];int done=0;
            CHECK(!ultra_rmt_init(&r,&timing,g->clock_hz));
            CHECK(ultra_rmt_encode_rw(&r,frame,40,0,words,320,&done)==320 && done);
            CHECK((words[319]>>16)==g->clock_hz/12500u);
        }
        {
            ultra_update_t u={0};uint8_t update[48];
            u.line=&line;u.session=7;u.state=ULTRA_UPDATE_TRAIN;u.train_until_us=(int64_t)g->start+2000000;
            CHECK(ultra_update_next(&u,g->start,update)==48);
            CHECK(!memcmp(update,g->update,48) && ultra_time_wire((uint64_t)line.last_time_us)==g->update_time);
            CHECK(ultra_wire_ns(&timing,update,48)==((uint64_t)g->update_edge*1000000000u+g->clock_hz/2u)/g->clock_hz);
        }
    }
}
static uint8_t body[512];
static int read_body(void *ctx,uint32_t offset,uint8_t *data,uint32_t n)
{(void)ctx;if(offset>sizeof body || n>sizeof body-offset)return -1;memcpy(data,body+offset,n);return 0;}
static int read_pattern(void *ctx,uint32_t offset,uint8_t *data,uint32_t n)
{uint32_t i;(void)ctx;for(i=0;i<n;++i)data[i]=(uint8_t)((offset+i)*37u);return 0;}
static void image_length(upd_descriptor_t *d,uint32_t length)
{
    uint32_t offset,crc=0xffffffffu;uint8_t b[32];
    for(offset=0;offset<length;offset+=32){uint32_t n=length-offset;if(n>32)n=32;read_pattern(NULL,offset,b,n);crc=upd_crc32_words(crc,b,n/4);}
    ultra_put32(d->bytes+20,length);ultra_put32(d->bytes+24,crc);
    ultra_put32(d->bytes+60,upd_crc32_words(0xffffffffu,d->bytes,15));
}
static void update_image(upd_descriptor_t *d,upd_target_t *t,ultra_update_layout_t *layout,int status)
{
    unsigned i;memset(t,0,sizeof *t);memset(d,0,sizeof *d);memset(layout,0,sizeof *layout);
    for(i=0;i<sizeof body;++i)body[i]=(uint8_t)(i*37);
    t->bytes[0]=254;t->bytes[1]=status?4:1;t->bytes[2]=status?5:1;t->bytes[3]=status?4:1;t->bytes[4]=3;t->bytes[5]=status?3:0;t->bytes[6]=201;t->bytes[7]=17;
    ultra_put32(t->bytes+8,status?0x08005000:0x08002100);ultra_put32(t->bytes+12,status?503808:57088);
    memcpy(d->bytes,"QDU6",4);d->bytes[4]=2;d->bytes[6]=1;d->bytes[7]=255;memcpy(d->bytes+8,t->bytes,7);d->bytes[15]=3;
    ultra_put32(d->bytes+16,0x000700ff);ultra_put32(d->bytes+20,sizeof body);ultra_put32(d->bytes+24,upd_crc32_words(0xffffffff,body,sizeof body/4));
    ultra_put32(d->bytes+28,ultra_get32(t->bytes+8));ultra_put32(d->bytes+32,ultra_get32(t->bytes+8));d->bytes[36]=2;d->bytes[37]=3;d->bytes[38]=1;
    ultra_put32(d->bytes+60,upd_crc32_words(0xffffffff,d->bytes,15));
    layout->metadata_base=status?0x08004000:0x08002000;layout->metadata_bytes=status?4096:256;
    layout->bootloader_bytes=status?16384:8192;layout->boot_version=3;layout->geometry[0]=status?0x8c:0x88;
    layout->geometry[1]=10;layout->geometry[2]=20;layout->entry_10ms=5;
}
static void updates(void)
{
    upd_descriptor_t image,bad;upd_target_t target;ultra_update_layout_t layout;ultra_update_plan_t plan;
    ultra_update_t u;uint8_t uid[12]={1},f[48],a[32],data[24]={0};unsigned i,announces=0,chunks=0;int rc;
    update_image(&image,&target,&layout,0);
    CHECK(!ultra_update_plan(&plan,&image,&target,&layout,500,read_body,NULL));
    CHECK(plan.erase_units==3 && plan.lead_ms==220 && plan.pause_ms==21);
    bad=image;bad.bytes[14]^=1;ultra_put32(bad.bytes+60,upd_crc32_words(0xffffffff,bad.bytes,15));
    CHECK(ultra_update_plan(&plan,&bad,&target,&layout,500,read_body,NULL)==UPD_ERROR_TARGET);
    setup(1);CHECK(ultra_update_begin(&u,&line,&bad,&target,&layout,uid,ULTRA_PORT_DIFF_POWER,42,3,2,now,read_body,NULL)!=0 && !line.update_active);
    CHECK(!ultra_update_begin(&u,&line,&image,&target,&layout,uid,ULTRA_PORT_DIFF_POWER,42,3,2,now,read_body,NULL));
    CHECK(ultra_update_next(&u,now,f)==48 && f[0]==UPD_PING && f[1]==42);
    for(i=0;i<600 && u.state!=ULTRA_UPDATE_CONFIRM;++i){
        now=u.due_us;rc=ultra_update_next(&u,now,f);
        if(rc==48){CHECK(!upd_check_frame(f));announces+=f[0]==UPD_ANNOUNCE;chunks+=f[0]==UPD_DATA;
            CHECK(!ultra_sender_next_frame(&line,now,frame));}
        else CHECK(rc==0 || rc==40);
    }
    CHECK(u.state==ULTRA_UPDATE_CONFIRM && announces==4 && chunks==48 && line.update_active==2);
    update_image(&image,&target,&layout,1);setup(1);
    CHECK(!ultra_update_begin_mode(&u,&line,&image,&target,&layout,uid,ULTRA_PORT_DIFF_POWER,43,3,2,ULTRA_UPDATE_SELECTED,now,read_body,NULL));
    for(i=0;i<300 && !u.poll_pending;++i){now=u.due_us;CHECK(ultra_update_next(&u,now,f)>=0);}
    CHECK(u.poll_pending && f[0]==UPD_STATUS && f[42]==0x39);
    data[0]=1;data[1]=UPD_STATE_BUSY;data[4]=43;ultra_put32(data+16,40);ultra_put32(data+20,u.plan.body_crc);
    envelope(a,0x86,43,uid,data);now=u.poll_end_us+1100;CHECK(!ultra_update_answer(&u,a,now));
    CHECK(u.due_us==u.poll_end_us+ultra_rw_next_us(1,2,500)+40000);
    CHECK(!ultra_update_next(&u,u.due_us-1,f));
    now=u.due_us;CHECK(ultra_update_next(&u,now,f)==48);
    data[1]=UPD_STATE_READY;data[3]=8;ultra_put32(data+16,0);envelope(a,0x86,43,uid,data);
    CHECK(!ultra_update_answer(&u,a,u.poll_end_us+1100));
    now=u.due_us;for(i=0;i<8;++i){CHECK(ultra_update_next(&u,now,f)==48 && f[0]==UPD_DATA);now=u.due_us;}
    CHECK(u.state==ULTRA_UPDATE_POLL);
    now=u.due_us;CHECK(ultra_update_next(&u,now,f)==48 && f[0]==UPD_STATUS);
    data[1]=UPD_STATE_FAILED;data[2]=UPD_ERROR_FLASH;data[3]=0;ultra_put16(data+6,255);ultra_put32(data+8,256);
    envelope(a,0x86,43,uid,data);
    CHECK(ultra_update_answer(&u,a,u.poll_end_us+2400)!=0 && u.state==ULTRA_UPDATE_FAILED && line.update_active);
    CHECK(!ultra_update_abort(&u,line.hold_us-1,f));CHECK(ultra_update_abort(&u,line.hold_us,f)==48 && f[0]==UPD_ABORT && line.update_active);
    update_image(&image,&target,&layout,0);image_length(&image,57088);
    CHECK(!ultra_update_plan(&plan,&image,&target,&layout,500,read_pattern,NULL) && plan.erase_units==224 && plan.lead_ms==9060);
    update_image(&image,&target,&layout,1);image_length(&image,503808);
    CHECK(!ultra_update_plan(&plan,&image,&target,&layout,500,read_pattern,NULL) && plan.erase_units==124);
    layout.bootloader_bytes=16384;target.bytes[5]=0;image.bytes[13]=0;
    ultra_put32(image.bytes+60,upd_crc32_words(0xffffffffu,image.bytes,15));
    CHECK(!ultra_update_plan(&plan,&image,&target,&layout,500,read_pattern,NULL));
    /* STATUS and boot size describe abilities, never choose the wire mode. */
    update_image(&image,&target,&layout,1);setup(1);
    CHECK(!ultra_update_begin_mode(&u,&line,&image,&target,&layout,NULL,ULTRA_PORT_DIFF_POWER,47,3,2,
                                  ULTRA_UPDATE_BLIND,now,read_body,NULL));
    for(i=0;i<400 && u.state!=ULTRA_UPDATE_CONFIRM;++i){now=u.due_us;rc=ultra_update_next(&u,now,f);
        CHECK(rc>=0);if(rc==48)CHECK(f[0]!=UPD_SELECT && f[0]!=UPD_STATUS);}
    CHECK(u.state==ULTRA_UPDATE_CONFIRM);
    setup(1);target.bytes[5]=0;image.bytes[13]=0;
    ultra_put32(image.bytes+60,upd_crc32_words(0xffffffffu,image.bytes,15));
    CHECK(ultra_update_begin_mode(&u,&line,&image,&target,&layout,uid,ULTRA_PORT_DIFF_POWER,48,1,2,
                                 ULTRA_UPDATE_SELECTED,now,read_body,NULL)!=0 && !line.update_active);
    {
        ultra_update_participant_t nodes[2];ultra_identity_t id={0};unsigned k;
        update_image(&image,&target,&layout,0);memset(nodes,0,sizeof nodes);
        for(k=0;k<2;++k){nodes[k].target=target;nodes[k].layout=layout;nodes[k].uid[0]=(uint8_t)(k+1);nodes[k].has_return=1;}
        nodes[1].target.bytes[7]=18;nodes[1].layout.geometry[1]=20;nodes[1].layout.geometry[2]=40;nodes[1].layout.entry_10ms=10;
        setup(1);CHECK(!ultra_update_begin_group(&u,&line,&image,nodes,2,ULTRA_PORT_DIFF_POWER,44,3,now,read_body,NULL));
        CHECK(u.plan.pause_ms==41 && u.plan.lead_ms==340 && u.plan.entry_ms==100);
        for(k=0;k<600 && u.state!=ULTRA_UPDATE_CONFIRM;++k){now=u.due_us;CHECK(ultra_update_next(&u,now,f)>=0);}
        CHECK(u.state==ULTRA_UPDATE_CONFIRM && u.confirmations==2);
        memset(data,0,24);data[0]=1;data[1]=3;ultra_put32(data+4,u.plan.firmware);ultra_put32(data+8,u.plan.length);
        ultra_put32(data+12,u.plan.body_crc);ultra_put32(data+16,u.plan.descriptor_crc);
        id.parts=5;id.firmware=u.plan.firmware;
        for(k=0;k<2;++k){memcpy(id.uid,nodes[k].uid,12);id.target=nodes[k].target;envelope(a,5,1,id.uid,data);CHECK(!ultra_update_confirm(&u,&id,a));
            CHECK(k==0?u.state==ULTRA_UPDATE_CONFIRM:u.state==ULTRA_UPDATE_DONE);}
    }
    {
        unsigned phase;
        update_image(&image,&target,&layout,1);setup(1);
        CHECK(!ultra_update_begin_mode(&u,&line,&image,&target,&layout,uid,ULTRA_PORT_DIFF_POWER,45,3,2,ULTRA_UPDATE_SELECTED,now,read_body,NULL));
        for(phase=0;phase<3;++phase){
            for(i=0;i<400 && !u.poll_pending;++i){now=u.due_us;CHECK(ultra_update_next(&u,now,f)>=0);}
            CHECK(u.poll_pending);memset(data,0,24);data[0]=1;data[1]=UPD_STATE_READY;data[3]=phase<2?8:0;data[4]=45;
            ultra_put32(data+8,phase*256u);ultra_put32(data+12,phase*256u);ultra_put32(data+20,u.plan.body_crc);
            envelope(a,0x86,45,uid,data);CHECK(!ultra_update_answer(&u,a,u.poll_end_us+2400));
        }
        CHECK(u.state==ULTRA_UPDATE_END);now=u.due_us;CHECK(ultra_update_next(&u,now,f)==48 && f[0]==UPD_END);
        now=u.due_us;CHECK(ultra_update_next(&u,now,f)==48 && f[0]==UPD_STATUS);
        data[1]=UPD_STATE_COMMITTED;envelope(a,0x86,45,uid,data);CHECK(!ultra_update_answer(&u,a,u.poll_end_us+2400));
        CHECK(u.state==ULTRA_UPDATE_ACK);
        for(i=0;i<3;++i){
            now=u.due_us;CHECK(ultra_update_next(&u,now-1,f)==0);
            CHECK(ultra_update_next(&u,now,f)==48 && f[0]==UPD_PING && f[1]==45);
            CHECK(u.due_us>=u.poll_end_us+(int64_t)u.plan.entry_ms*1000+u.gap_us);
            if(i<2)CHECK(u.due_us==u.poll_end_us+(int64_t)u.plan.entry_ms*1000+u.gap_us);
            else CHECK(u.due_us>=u.poll_end_us+2100000);
            CHECK(u.state==(i<2?ULTRA_UPDATE_ACK:ULTRA_UPDATE_QUIET));
        }
        for(i=0;i<200 && u.state!=ULTRA_UPDATE_CONFIRM;++i){now=u.due_us;CHECK(ultra_update_next(&u,now,f)>=0);}
        CHECK(u.state==ULTRA_UPDATE_CONFIRM && u.transfer_complete);
        CHECK(line.update_active==2 && line.preamble_left==6 && !ultra_sender_rc_ready(&line,u.due_us));
        setup(1);CHECK(!ultra_update_begin_mode(&u,&line,&image,&target,&layout,uid,ULTRA_PORT_DIFF_POWER,46,3,2,ULTRA_UPDATE_SELECTED,now,read_body,NULL));
        for(i=0;i<300 && !u.poll_pending;++i){now=u.due_us;CHECK(ultra_update_next(&u,now,f)>=0);}
        CHECK(!u.credit);now=u.progress_deadline_us+1;
        CHECK(ultra_update_next(&u,now,f)<0 && u.error==UPD_ERROR_TIMEOUT && line.update_active);
    }
}
static void job_setup(uint16_t count)
{
    ultra_sender_config_t c;ultra_sender_config_init(&c,count,0);c.light_capacity=0;c.clear_unused=0;
    CHECK(!ultra_sender_init(&line,&c,boards,items,512));now=1000000;
}
static void job_planning(void)
{
    ultra_update_job_t j;ultra_update_job_options_t o={0};
    ultra_update_node_t n[2];ultra_update_artifact_t a[3];ultra_update_result_t r[2];
    unsigned k;memset(n,0,sizeof n);memset(a,0,sizeof a);
    for(k=0;k<2;++k){update_image(&a[k].descriptor,&n[k].participant.target,&n[k].participant.layout,k==0);
        n[k].participant.uid[0]=(uint8_t)(k+1);n[k].participant.has_return=1;
        n[k].trusted=n[k].reachable=1;n[k].firmware=1;n[k].evidence_us=10;
        a[k].authenticated=1;a[k].read=read_body;}
    a[2]=a[0];n[1].participant.layout.entry_10ms=17;n[0].blackout_ms=90;n[1].min_gap_us=1700;
    o.passes=3;o.first_session=254;o.verify_timeout_ms=100;o.port_type=ULTRA_PORT_DIFF_POWER;
    o.deadline_us=1000000000;
    job_setup(2);CHECK(!ultra_update_job_begin(&j,&line,n,2,a,3,r,&o,now));
    CHECK(j.entry_ms==260 && j.gap_us==1700 && j.options.family_order[0]==4 && j.options.family_order[1]==1);
    CHECK(r[0].artifact==0 && r[1].artifact==1 && r[0].result==ULTRA_JOB_PENDING);
    job_setup(2);n[0].trusted=0;
    CHECK(ultra_update_job_begin(&j,&line,n,2,a,3,r,&o,now)!=0 && !line.update_active);n[0].trusted=1;
    job_setup(2);a[2].authenticated=0;
    CHECK(ultra_update_job_begin(&j,&line,n,2,a,3,r,&o,now)!=0 && !line.update_active);a[2].authenticated=1;
    n[0].participant.target.bytes[7]=0;
    job_setup(2);CHECK(!ultra_update_job_begin(&j,&line,n,2,a,3,r,&o,now));
    CHECK(r[0].result==ULTRA_JOB_SKIPPED && r[0].reason==ULTRA_JOB_INCOMPATIBLE);
    n[0].participant.target.bytes[7]=17;n[0].firmware=ultra_get32(a[0].descriptor.bytes+16);
    n[1].firmware=n[0].firmware+1;
    job_setup(2);CHECK(!ultra_update_job_begin(&j,&line,n,2,a,3,r,&o,now));
    CHECK(r[0].result==ULTRA_JOB_CURRENT && r[1].reason==ULTRA_JOB_NEWER);
    {   /* Diff-Power port: eight boards is the forward timing budget, a ninth refuses the port */
        ultra_update_node_t port[9];ultra_update_result_t pr[9];
        for(k=0;k<9;++k){port[k]=n[0];port[k].firmware=1;port[k].participant.uid[0]=(uint8_t)(k+1);}
        job_setup(8);CHECK(!ultra_update_job_begin(&j,&line,port,8,a,1,pr,&o,now));
        job_setup(8);memset(pr,0xee,sizeof pr);
        CHECK(ultra_update_job_begin(&j,&line,port,9,a,1,pr,&o,now)==ULTRA_UPDATE_TOO_MANY_BOARDS &&
              !line.update_active && pr[0].result==0xee);
    }
    {   /* overlapping revision ranges refuse the job; blind pacing covers every
         * node that accepts the image on the wire, planned for it or not */
        ultra_update_artifact_t v[2];ultra_update_node_t p[2];ultra_update_result_t pr[2];
        ultra_update_plan_t fast,slow;uint8_t f[256];
        for(k=0;k<2;++k){v[k]=a[1];p[k]=n[1];p[k].firmware=1;p[k].participant.uid[0]=(uint8_t)(k+1);}
        v[0].descriptor.bytes[6]=1;v[0].descriptor.bytes[7]=20;v[1].descriptor.bytes[6]=20;v[1].descriptor.bytes[7]=30;
        for(k=0;k<2;++k)ultra_put32(v[k].descriptor.bytes+60,upd_crc32_words(0xffffffffu,v[k].descriptor.bytes,15));
        p[0].participant.target.bytes[7]=10;p[1].participant.target.bytes[7]=20;
        p[1].participant.layout.geometry[1]=200;p[1].participant.layout.geometry[2]=200;
        job_setup(2);memset(pr,0xee,sizeof pr);
        CHECK(ultra_update_job_begin(&j,&line,p,2,v,2,pr,&o,now)==ULTRA_UPDATE_OVERLAPPING_ARTIFACTS &&
              !line.update_active && pr[0].result==0xee);
        v[1].descriptor.bytes[6]=21;
        ultra_put32(v[1].descriptor.bytes+60,upd_crc32_words(0xffffffffu,v[1].descriptor.bytes,15));
        p[1].firmware=ultra_get32(v[0].descriptor.bytes+16);   /* current: no subtransfer planned for it */
        job_setup(2);CHECK(!ultra_update_job_begin(&j,&line,p,2,v,2,pr,&o,now));
        CHECK(!ultra_update_plan_metadata(&fast,&v[0].descriptor,&p[0].participant.target,&p[0].participant.layout,j.gap_us));
        CHECK(!ultra_update_plan_metadata(&slow,&v[0].descriptor,&p[1].participant.target,&p[1].participant.layout,j.gap_us));
        CHECK(slow.lead_ms>fast.lead_ms && slow.pause_ms>fast.pause_ms);
        CHECK(pr[0].artifact==0 && pr[0].result==ULTRA_JOB_PENDING && pr[1].artifact==0 && pr[1].result==ULTRA_JOB_CURRENT);
        CHECK(ultra_update_job_next_frame(&j,now,f,sizeof f)>0 && j.state==ULTRA_JOB_TRANSFER && j.active_artifact==0);
        CHECK(j.transfer.plan.lead_ms==slow.lead_ms && j.transfer.plan.pause_ms==slow.pause_ms &&
              j.transfer.plan.finish_ms>=slow.finish_ms);
        ultra_update_job_abort(&j);
    }
}
static void update_schedule(void)
{
    unsigned marker,k,announces,pings,normal,ends,confirms,paced;upd_descriptor_t d;upd_target_t t;
    ultra_update_layout_t l;ultra_update_t u;uint8_t uid[12]={7},f[48];
    int64_t first,last,entry_end,boot_end,end,confirm,prev_end;int rc;
    for(marker=0;marker<2;++marker){
        update_image(&d,&t,&l,1);setup(0);line.digital=(uint8_t)marker;
        CHECK(!ultra_update_begin(&u,&line,&d,&t,&l,uid,ULTRA_PORT_DIFF_POWER,7,3,2,now,read_body,NULL));
        announces=pings=normal=ends=confirms=paced=0;first=last=entry_end=boot_end=end=confirm=prev_end=0;
        for(k=0;k<600 && u.state!=ULTRA_UPDATE_CONFIRM;++k){
            now=u.due_us;CHECK(!ultra_update_next(&u,now-1,f));rc=ultra_update_next(&u,now,f);CHECK(rc>=0);
            if(!rc)continue;
            CHECK(u.transport.frame_end_us==ultra_wire_end_us(&line.timing,f,(uint32_t)rc,u.transport.frame_start_us));
            CHECK(u.transport.marker==marker && u.transport.marker_high_us==(marker?20u:0u));
            CHECK(u.transport.marker_low_us==(marker?10u:0u));
            CHECK(u.transport.frame_start_us==now+(marker?530:0));
            if(rc==40){++normal;CHECK(end && u.transport.frame_start_us>=end+2100000);prev_end=u.poll_end_us;continue;}
            CHECK(!upd_check_frame(f) && f[0]!=UPD_SELECT && f[0]!=UPD_STATUS);
            /* a receiving app captures one frame at a time: every ANNOUNCE half and END copy starts >= the pace plus
             * the port gap after the frame before it (the last training PING, the other half, the copy before) */
            if(f[0]==UPD_ANNOUNCE || f[0]==UPD_END){
                CHECK(prev_end && now>=prev_end+(int64_t)u.gap_us+ULTRA_UPDATE_CONTROL_PACE_US);++paced;}
            if(f[0]==UPD_PING && f[1]){if(!first)first=u.poll_end_us;last=u.poll_end_us;++pings;}
            if(f[0]==UPD_ANNOUNCE){
                if(announces==0 || announces==2)CHECK(last-first>=1000000);
                CHECK(ultra_get16(f+2)==announces%2 && !memcmp(f+6,d.bytes+32*(announces%2),32));
                ++announces;
                if(announces==2){entry_end=u.poll_end_us;first=last=0;}
                if(announces==4)boot_end=u.poll_end_us;
            }
            if(announces==2 && f[0]==UPD_PING)CHECK(u.transport.frame_start_us>=entry_end+50000);
            if(f[0]==UPD_DATA)CHECK(u.transport.frame_start_us>=boot_end+(int64_t)u.plan.lead_ms*1000);
            if(f[0]==UPD_END){
                /* END has no feedback: ULTRA_UPDATE_END_COPIES copies, each followed by the port gap (the next copy is due
                 * exactly there); the waits after it count from the last copy. */
                if(++ends<ULTRA_UPDATE_END_COPIES)
                    CHECK(u.state==ULTRA_UPDATE_END &&
                          u.due_us==u.poll_end_us+(int64_t)u.gap_us+ULTRA_UPDATE_CONTROL_PACE_US);
                else CHECK(u.state==ULTRA_UPDATE_QUIET && u.due_us>=u.poll_end_us+2100000);
                end=u.poll_end_us;
            }
            if(f[0]==UPD_PING && !f[1]){
                /* the idle confirmation PING: ULTRA_UPDATE_CONFIRM_COPIES copies, each with its confirmation-write pause;
                 * the quiet LOW counts from the last one. */
                confirm=u.poll_end_us;CHECK(normal>=6);
                if(++confirms<ULTRA_UPDATE_CONFIRM_COPIES)
                    CHECK(u.state==ULTRA_UPDATE_REBOOT && u.due_us==u.poll_end_us+(int64_t)u.plan.finish_ms*1000+u.gap_us);
                else CHECK(u.state==ULTRA_UPDATE_CONFIRM_WAIT);
            }
            prev_end=u.poll_end_us;
        }
        CHECK(announces==4 && pings>=4 && confirm && u.transfer_complete);
        CHECK(ends==ULTRA_UPDATE_END_COPIES && confirms==ULTRA_UPDATE_CONFIRM_COPIES && paced==4u+ULTRA_UPDATE_END_COPIES);
        CHECK(now>=confirm+2100000 && line.hold_us>=confirm+2100000);
    }
}
typedef struct {unsigned prepares,releases,reads;int ready;} job_source_t;
static int job_prepare(void *ctx)
{job_source_t *s=(job_source_t *)ctx;++s->prepares;if(s->prepares==1)return 1;s->ready=1;return 0;}
static void job_release(void *ctx)
{job_source_t *s=(job_source_t *)ctx;++s->releases;s->ready=0;}
static int job_read(void *ctx,uint32_t off,uint8_t *out,uint32_t n)
{job_source_t *s=(job_source_t *)ctx;++s->reads;return s->ready?read_body(NULL,off,out,n):-1;}
static void selected_deadlines(void)
{
    upd_descriptor_t d;upd_target_t t;ultra_update_layout_t l;ultra_update_t u;
    uint8_t uid[12]={1},f[48],a[32],data[24];unsigned marker,profile,rate,k;int64_t wait,until;
    for(marker=0;marker<2;++marker)for(profile=0;profile<4;++profile)for(rate=0;rate<3;++rate){
        update_image(&d,&t,&l,1);l.geometry[1]=30;l.geometry[2]=100;job_setup(1);line.digital=(uint8_t)marker;
        CHECK(!ultra_timing_profile(&line.timing,(uint8_t)profile,9600000,500));now=4294900000LL;
        CHECK(!ultra_update_begin_mode(&u,&line,&d,&t,&l,uid,ULTRA_PORT_DIFF_POWER,71,1,(uint8_t)rate,ULTRA_UPDATE_SELECTED,now,read_body,NULL));
        for(k=0;k<400 && !u.poll_pending;++k){now=u.due_us;CHECK(ultra_update_next(&u,now,f)>0);}
        CHECK(u.poll_pending && f[0]==UPD_STATUS && u.transport.listen_us==(marker?900u:1000u)+ultra_rw_slot_us((uint8_t)rate));
        memset(data,0,sizeof data);data[0]=1;data[1]=UPD_STATE_BUSY;data[4]=71;
        ultra_put32(data+16,24900);ultra_put32(data+20,u.plan.body_crc);envelope(a,0x86,71,uid,data);
        CHECK(!ultra_update_answer(&u,a,u.poll_end_us+u.transport.listen_us-1));
        wait=(marker?900:1000)+ultra_rw_slot_us((uint8_t)rate)+(marker?520:580);
        CHECK(u.due_us==u.poll_end_us+wait+24900000 && u.progress_deadline_us>u.due_us);
        CHECK(!ultra_update_next(&u,u.due_us-1,f) && !ultra_update_abort(&u,u.due_us-1,f));
        now=u.due_us;CHECK(ultra_update_next(&u,now,f)==48 && f[0]==UPD_STATUS);
        until=u.due_us;CHECK(!ultra_update_next(&u,until-1,f));
        CHECK(ultra_update_next(&u,until,f)==48 && f[0]==UPD_STATUS && !u.credit);
        now=u.progress_deadline_us+1;CHECK(ultra_update_next(&u,now,f)<0 && u.error==UPD_ERROR_TIMEOUT);
        CHECK(ultra_update_abort(&u,now,f)==48 && line.update_active && u.state==ULTRA_UPDATE_ABORTED);
        CHECK(!ultra_update_next(&u,u.due_us-1,f) && line.update_active);
        CHECK(!ultra_update_next(&u,u.due_us,f) && !line.update_active);
    }
}
static void job_results(int reverse)
{
    ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_job_progress_t progress;
    ultra_update_node_t n[8];ultra_update_artifact_t a[4];ultra_update_result_t r[8];job_source_t source[4];
    uint8_t f[264],answer[32],data[24];ultra_identity_t id;unsigned i,k,order=0,expected[4]={0,2,1,3};int rc;
    memset(n,0,sizeof n);memset(a,0,sizeof a);memset(source,0,sizeof source);
    for(i=0;i<4;++i){
        update_image(&a[i].descriptor,&n[i].participant.target,&n[i].participant.layout,(i%2)==0);
        n[i].participant.target.bytes[0]=(uint8_t)(i+1);a[i].descriptor.bytes[8]=(uint8_t)(i+1);
        ultra_put32(a[i].descriptor.bytes+60,upd_crc32_words(0xffffffffu,a[i].descriptor.bytes,15));
        a[i].authenticated=1;a[i].read=job_read;a[i].prepare=job_prepare;a[i].release=job_release;a[i].context=source+i;
        n[i].trusted=n[i].reachable=n[i].participant.has_return=1;n[i].firmware=1;n[i].participant.uid[0]=(uint8_t)(i+1);
    }
    n[4]=n[0];n[4].participant.uid[0]=5;n[4].firmware=ultra_get32(a[0].descriptor.bytes+16);
    n[5]=n[1];n[5].participant.uid[0]=6;n[5].firmware=n[4].firmware+1;
    n[6]=n[0];n[6].participant.uid[0]=7;n[6].participant.target.bytes[0]=77;
    n[7]=n[1];n[7].participant.uid[0]=8;n[7].participant.target.bytes[7]=0;
    n[2].participant.has_return=n[2].reachable=0;n[7].blackout_ms=60;n[6].participant.layout.entry_10ms=25;
    o.passes=3;o.first_session=254;o.verify_timeout_ms=100;o.port_type=ULTRA_PORT_DIFF_POWER;
    if(reverse){o.family_order[0]=1;o.family_order[1]=4;expected[0]=1;expected[1]=3;expected[2]=0;expected[3]=2;}
    job_setup(8);now=4294900000LL;
    CHECK(!ultra_update_job_begin(&j,&line,n,8,a,4,r,&o,now));
    CHECK(j.entry_ms==310 && r[4].result==ULTRA_JOB_CURRENT && r[5].reason==ULTRA_JOB_NEWER);
    CHECK(r[6].reason==ULTRA_JOB_MISSING_ARTIFACT && r[7].reason==ULTRA_JOB_INCOMPATIBLE);
    for(i=0;i<4;++i)CHECK(!source[i].reads && !source[i].prepares);
    for(k=0;k<4000 && j.state!=ULTRA_JOB_DONE;++k){
        now=j.due_us;rc=ultra_update_job_next_frame(&j,now,f,sizeof f);CHECK(rc>=0);
        CHECK(!ultra_update_job_progress(&j,now,&progress));
        if(rc>0 && f[0]==UPD_ANNOUNCE && rc==48)CHECK(j.active_artifact==expected[order]);
        if(j.state==ULTRA_JOB_VERIFY){unsigned node=j.active_artifact;
            CHECK(order<4 && node==expected[order] && r[node].transfer_complete);
            CHECK(j.transfer.session==(uint8_t)(1u+(253u+order)%255u));
            memset(&id,0,sizeof id);id.parts=7;id.target=n[node].participant.target;
            memcpy(id.uid,n[node].participant.uid,12);id.firmware=r[node].expected;
            memset(data,0,sizeof data);data[0]=1;data[1]=3;ultra_put32(data+4,id.firmware);
            ultra_put32(data+8,512);ultra_put32(data+12,ultra_get32(a[node].descriptor.bytes+24));
            ultra_put32(data+16,ultra_get32(a[node].descriptor.bytes+60));envelope(answer,5,1,id.uid,data);
            if(node==0){uint8_t slot=(uint8_t)(reverse?3:0),forged[32];
                answer[2]=(uint8_t)((slot<<4)|(answer[2]&15u));
                ultra_put16(answer+30,ultra_crc16(answer,30));
                CHECK(ultra_update_job_confirm(&j,(uint16_t)node,slot,&id,answer,j.verify_started_us)<0);
                CHECK(ultra_update_job_confirm(&j,(uint16_t)node,(uint8_t)(slot^1u),&id,answer,j.verify_started_us+1)<0);
                CHECK(ultra_update_job_confirm(&j,(uint16_t)node,16,&id,answer,j.verify_started_us+1)<0);
                memcpy(forged,answer,sizeof forged);forged[30]^=1;
                CHECK(ultra_update_job_confirm(&j,(uint16_t)node,slot,&id,forged,j.verify_started_us+1)<0);
                CHECK(r[node].result==ULTRA_JOB_PENDING);
                CHECK(!ultra_update_job_confirm(&j,(uint16_t)node,slot,&id,answer,j.verify_started_us+1));
            }else if(node==1){id.uid[0]=99;envelope(answer,5,1,id.uid,data);
                CHECK(ultra_update_job_confirm(&j,1,0,&id,answer,j.verify_started_us+1)==1);
            }
            CHECK(!ultra_update_job_verification_done(&j,now+2));++order;
        }
    }
    CHECK(j.state==ULTRA_JOB_DONE && !line.update_active && order==4 && j.sessions==4);
    CHECK(r[0].result==ULTRA_JOB_VERIFIED && r[1].result==ULTRA_JOB_FAILED && r[1].reason==ULTRA_JOB_MISMATCH);
    CHECK(r[2].result==ULTRA_JOB_SENT_UNVERIFIED && r[2].observed==1 && r[3].result==ULTRA_JOB_UNREACHABLE);
    for(i=0;i<4;++i)CHECK(source[i].reads>0 && source[i].prepares==2 && source[i].releases==1);
}
#include "test_job_selected.inc"
/* Line profile per port type and board count: the D2A pin table, Diff-Power
 * unchanged at 800 up to its eight receivers, refusals with their reason. */
static void port_line_profiles(void)
{
    static const struct { uint8_t type; uint16_t boards; uint8_t profile, why; } c[]={
        {ULTRA_PORT_D2A_PIN,1,0,ULTRA_PORT_PROFILE_DEFAULT},{ULTRA_PORT_D2A_PIN,20,0,ULTRA_PORT_PROFILE_DEFAULT},
        {ULTRA_PORT_D2A_PIN,21,1,ULTRA_PORT_PROFILE_BOARD_COUNT},{ULTRA_PORT_D2A_PIN,34,1,ULTRA_PORT_PROFILE_BOARD_COUNT},
        {ULTRA_PORT_D2A_PIN,35,3,ULTRA_PORT_PROFILE_BOARD_COUNT},{ULTRA_PORT_D2A_PIN,50,3,ULTRA_PORT_PROFILE_BOARD_COUNT},
        {ULTRA_PORT_D2A_PIN,51,255,ULTRA_PORT_PROFILE_TOO_MANY},{ULTRA_PORT_D2A_PIN,0,255,ULTRA_PORT_PROFILE_NO_BOARDS},
        {ULTRA_PORT_DIFF_POWER,1,0,ULTRA_PORT_PROFILE_DEFAULT},{ULTRA_PORT_DIFF_POWER,8,0,ULTRA_PORT_PROFILE_DEFAULT},
        {ULTRA_PORT_DIFF_POWER,9,255,ULTRA_PORT_PROFILE_TOO_MANY},{ULTRA_PORT_DIFF_POWER,0,255,ULTRA_PORT_PROFILE_NO_BOARDS},
        {0,1,255,ULTRA_PORT_PROFILE_UNKNOWN_TYPE},{3,1,255,ULTRA_PORT_PROFILE_UNKNOWN_TYPE},{255,8,255,ULTRA_PORT_PROFILE_UNKNOWN_TYPE}};
    unsigned i;uint16_t n;uint8_t p;
    for(i=0;i<sizeof c/sizeof c[0];++i){p=77;
        CHECK(ultra_port_line_profile(c[i].type,c[i].boards,&p)==c[i].why && p==c[i].profile);
        CHECK(ultra_port_line_profile(c[i].type,c[i].boards,NULL)==c[i].why);}
    /* Every D2A count: the table profile is admitted by the link model with ideal
     * wiring, 400 kHz is never picked, and 800 keeps a full hop of reserve. */
    for(n=1;n<=ULTRA_PORT_D2A_PIN_MAX_BOARDS;++n){
        CHECK(ultra_port_line_profile(ULTRA_PORT_D2A_PIN,n,&p)<=ULTRA_PORT_PROFILE_BOARD_COUNT && p!=2u &&
              ultra_d2a_admits(p,(uint8_t)n,0));
        CHECK((p==0u)==(n<=ULTRA_D2A_800_MAX_BOARDS) && (p==1u)==(n>ULTRA_D2A_800_MAX_BOARDS && n<=ULTRA_D2A_600_MAX_BOARDS));
        if(p==0u)CHECK(ultra_d2a_admits(0,(uint8_t)n,ULTRA_D2A_HOP_NS));
    }
    CHECK(ultra_d2a_admits(0,(uint8_t)(ULTRA_D2A_800_MAX_BOARDS+1u),0) && !ultra_d2a_admits(0,(uint8_t)(ULTRA_D2A_800_MAX_BOARDS+2u),0) &&
          !ultra_d2a_admits(1,(uint8_t)(ULTRA_D2A_600_MAX_BOARDS+1u),0));
    CHECK(ultra_d2a_path_ns((uint8_t)ULTRA_PORT_D2A_PIN_MAX_BOARDS,0)!=65535u && ultra_d2a_path_ns((uint8_t)(ULTRA_PORT_D2A_PIN_MAX_BOARDS+1u),0)==65535u);
    CHECK(ultra_line_path_ns((uint8_t)ULTRA_PORT_DIFF_POWER_MAX_BOARDS,0)!=65535u &&
          ultra_line_path_ns((uint8_t)(ULTRA_PORT_DIFF_POWER_MAX_BOARDS+1u),0)==65535u);
}
int main(void)
{
    CHECK(!ultra_command_known(find("unknown_reserved_opcode_03")->bytes[0]));
    CHECK(!ultra_command_known(find("unknown_reserved_opcode_04")->bytes[0]));
    CHECK(!ultra_command_known(find("unknown_reserved_opcode_0a")->bytes[0]));
    all_vectors();pixel_fx_acknowledgement();local_scheduling();descriptors();scoped_sender();descriptor_admission();descriptor_chain_end();policy();acquisition_training();stats();parlio();golden_traces();updates();job_planning();update_schedule();selected_deadlines();job_results(0);job_results(1);job_lifecycle_edges();port_line_profiles();
    printf("%u shared-vector/host-state checks, %u failures\n",checks,failures);return failures?1:0;
}
