/* SPDX-License-Identifier: MIT
 * Host port-job regressions; no firmware forwarding qualification is implied. */
#include "ultra_sender_job.h"
static int cu_read(void *ctx,uint32_t off,uint8_t *out,uint32_t n)
{uint32_t i;(void)ctx;for(i=0;i<n;++i)out[i]=(uint8_t)((off+i)*37u);return 0;}
static void cu_image(upd_descriptor_t *d,upd_target_t *t,ultra_update_layout_t *l,int v407,uint32_t length)
{
    uint32_t off,crc=0xffffffffu;uint8_t b[32];
    memset(d,0,sizeof *d);memset(t,0,sizeof *t);memset(l,0,sizeof *l);
    t->bytes[0]=3;t->bytes[1]=v407?4:1;t->bytes[2]=v407?5:1;t->bytes[3]=v407?4:1;
    t->bytes[4]=3;t->bytes[5]=v407?3:0;t->bytes[6]=201;t->bytes[7]=17;
    ultra_put32(t->bytes+8,v407?0x08005000:0x08002100);ultra_put32(t->bytes+12,v407?503808:57088);
    memcpy(d->bytes,"QDU6",4);d->bytes[4]=2;d->bytes[6]=1;d->bytes[7]=255;
    memcpy(d->bytes+8,t->bytes,7);d->bytes[15]=3;ultra_put32(d->bytes+16,0x000700ff);
    for(off=0;off<length;off+=32){uint32_t n=length-off;if(n>32)n=32;cu_read(NULL,off,b,n);crc=upd_crc32_words(crc,b,n/4);}
    ultra_put32(d->bytes+20,length);ultra_put32(d->bytes+24,crc);
    memcpy(d->bytes+28,t->bytes+8,4);memcpy(d->bytes+32,t->bytes+8,4);
    d->bytes[36]=2;d->bytes[37]=3;d->bytes[38]=1;
    ultra_put32(d->bytes+60,upd_crc32_words(0xffffffffu,d->bytes,15));
    l->metadata_base=v407?0x08004000:0x08002000;l->metadata_bytes=v407?4096:256;
    l->bootloader_bytes=v407?16384:8192;l->boot_version=3;l->geometry[0]=v407?0x8c:0x88;
    l->geometry[1]=v407?30:10;l->geometry[2]=v407?100:20;l->entry_10ms=10;
}
static void cu_setup(ultra_sender_t *line,ultra_sender_board_t *boards,uint16_t count,uint8_t profile)
{
    static ultra_sender_item_t item;ultra_sender_config_t c;
    ultra_sender_config_init(&c,count,0);c.light_capacity=0;c.clear_unused=0;
    CHECK(!ultra_timing_profile(&c.timing,profile,9600000,500),"update profile");
    CHECK(!ultra_sender_init(line,&c,boards,&item,1),"update line init");
}
typedef char cu_port_limits[ULTRA_PORT_DIFF_POWER_MAX_BOARDS==8u && ULTRA_PORT_D2A_PIN_MAX_BOARDS==50u?1:-1];
typedef char cu_blind_passes[ULTRA_UPDATE_BLIND_MIN_PASSES==3u?1:-1];
/* One spare record above the largest port type: board limit + 1 is refused. */
enum { CU_MAX=ULTRA_PORT_D2A_PIN_MAX_BOARDS+1 };
static ultra_update_node_t cu_nodes[CU_MAX];static ultra_update_result_t cu_results[CU_MAX];
static ultra_update_participant_t cu_participants[CU_MAX];static ultra_sender_board_t cu_boards[CU_MAX];
static void cu_inventory(const upd_target_t *t,const ultra_update_layout_t *l)
{
    unsigned i;memset(cu_nodes,0,sizeof cu_nodes);
    for(i=0;i<CU_MAX;++i){cu_nodes[i].trusted=cu_nodes[i].reachable=1;cu_nodes[i].firmware=1;
        cu_nodes[i].participant.target=*t;cu_nodes[i].participant.layout=*l;cu_nodes[i].participant.has_return=1;
        ultra_put32(cu_nodes[i].participant.uid,i+1);cu_participants[i]=cu_nodes[i].participant;}
}
/* Group delivery and confirmation at the port type's limit; job admission at
 * the limit; one more board (inventory or line) is refused before any frame. */
static void cu_port_limit(uint8_t type,unsigned max,ultra_update_artifact_t *a)
{
    ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_sender_t line;ultra_update_t u;
    uint8_t f[48],answer[32],data[24]={0};ultra_identity_t id={0};unsigned i;int64_t at=1000000;
    CHECK(ultra_port_max_boards(type)==max,"port type %u limit %u",type,max);
    cu_setup(&line,cu_boards,1,0);
    CHECK(ultra_update_begin_group(&u,&line,&a->descriptor,cu_participants,(uint16_t)(max+1),type,33,3,at,cu_read,NULL)==
          ULTRA_UPDATE_TOO_MANY_BOARDS && !line.update_active,"type %u: %u participant group refused",type,max+1);
    CHECK(!ultra_update_begin_group(&u,&line,&a->descriptor,cu_participants,(uint16_t)max,type,33,3,at,cu_read,NULL),
          "type %u: %u participant group",type,max);
    for(i=0;i<600 && u.state!=ULTRA_UPDATE_CONFIRM;++i){at=u.due_us;CHECK(ultra_update_next(&u,at,f)>=0,"group next");}
    CHECK(u.transfer_complete && u.confirmations==max,"independent delivery completion at bound");
    id.parts=5;id.target=cu_participants[0].target;id.firmware=ultra_get32(a->descriptor.bytes+16);
    data[0]=1;data[1]=3;ultra_put32(data+4,id.firmware);ultra_put32(data+8,512);
    ultra_put32(data+12,u.plan.body_crc);ultra_put32(data+16,u.plan.descriptor_crc);
    for(i=0;i<max;++i){memcpy(id.uid,cu_participants[i].uid,12);memset(answer,0,32);
        answer[0]=0xa5;answer[1]=5;ultra_put16(answer+4,ultra_crc16(id.uid,12));memcpy(answer+6,data,24);ultra_slot_seal(answer);
        CHECK(!ultra_update_confirm(&u,&id,answer),"full-bound confirmation %u",i);
        if(!i)CHECK(!ultra_update_confirm(&u,&id,answer) && u.confirmations==max-1,"duplicate confirmation is idempotent");
    }
    CHECK(u.state==ULTRA_UPDATE_DONE && !line.update_active,"type %u: all %u confirmations",type,max);
    o.passes=3;o.first_session=255;o.port_type=type;a->authenticated=1;a->read=cu_read;
    cu_setup(&line,cu_boards,(uint16_t)(max+1),0);
    CHECK(line.n==max+1,"wide line build (ULTRA_SENDER_MAX_BOARDS >= 51)");
    if(line.n==max+1){memset(cu_results,0xee,sizeof cu_results);
        CHECK(ultra_update_begin(&u,&line,&a->descriptor,&cu_participants[0].target,&cu_participants[0].layout,
              cu_participants[0].uid,type,33,3,2,at,cu_read,NULL)==ULTRA_UPDATE_TOO_MANY_BOARDS &&
              !line.update_active,"type %u: single-target begin refuses %u-board line",type,max+1);
        cu_setup(&line,cu_boards,(uint16_t)(max+1),0);
        CHECK(ultra_update_begin_mode(&u,&line,&a->descriptor,&cu_participants[0].target,&cu_participants[0].layout,
              cu_participants[0].uid,type,33,3,2,ULTRA_UPDATE_SELECTED,at,cu_read,NULL)==ULTRA_UPDATE_TOO_MANY_BOARDS &&
              !line.update_active,"type %u: selected begin refuses %u-board line",type,max+1);
        cu_setup(&line,cu_boards,(uint16_t)(max+1),0);
        CHECK(ultra_update_begin_group(&u,&line,&a->descriptor,cu_participants,(uint16_t)max,type,33,3,at,cu_read,NULL)==
              ULTRA_UPDATE_TOO_MANY_BOARDS,"type %u: group on a %u-board line refused",type,max+1);
        CHECK(ultra_update_job_begin(&j,&line,cu_nodes,(uint16_t)(max+1),a,1,cu_results,&o,at)==ULTRA_UPDATE_TOO_MANY_BOARDS &&
              !line.update_active && cu_results[0].result==0xee,"type %u: %u-board port refused",type,max+1);
        CHECK(ultra_update_job_begin(&j,&line,cu_nodes,(uint16_t)max,a,1,cu_results,&o,at)==ULTRA_UPDATE_TOO_MANY_BOARDS &&
              !line.update_active,"type %u: %u-board line refused for a %u-node inventory",type,max+1,max);}
    cu_setup(&line,cu_boards,(uint16_t)max,type==ULTRA_PORT_D2A_PIN?3:0);
    CHECK(line.n==max,"line at the limit");
    if(line.n==max){
        CHECK(!ultra_update_begin(&u,&line,&a->descriptor,&cu_participants[0].target,&cu_participants[0].layout,
              cu_participants[0].uid,type,33,3,2,at,cu_read,NULL),"type %u: single-target begin at bound",type);
        cu_setup(&line,cu_boards,(uint16_t)max,type==ULTRA_PORT_D2A_PIN?3:0);
        /* Selected mode needs STATUS, independently of the physical bound. */
        {upd_descriptor_t d;upd_target_t t;ultra_update_layout_t l;
            cu_image(&d,&t,&l,1,512);
            CHECK(!ultra_update_begin_mode(&u,&line,&d,&t,&l,cu_participants[0].uid,type,33,3,2,
                  ULTRA_UPDATE_SELECTED,at,cu_read,NULL),"type %u: selected begin at bound",type);}
        cu_setup(&line,cu_boards,(uint16_t)max,type==ULTRA_PORT_D2A_PIN?3:0);
    }
    if(line.n==max)CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,(uint16_t)max,a,1,cu_results,&o,at) && line.update_active,
          "type %u: %u-board port inventory",type,max);
}
/* a Dig2Analog-Plus-5ch pin may not run faster than its speed table for
 * the board count, at every update entry point; Diff-Power is unchanged. */
static void cu_line_speed(ultra_update_artifact_t *a)
{
    static const uint16_t boards[]={20,21,34,35,50};static const uint8_t table[]={0,1,1,3,3};
    ultra_sender_t line;ultra_update_t u;ultra_update_job_t j;ultra_update_job_options_t o={0};
    unsigned b,p;int64_t at=1000000;int want,rc;
    o.passes=3;o.first_session=1;
    for(b=0;b<5;++b)for(p=0;p<4;++p){uint8_t got=255;
        CHECK(ultra_port_line_profile(ULTRA_PORT_D2A_PIN,boards[b],&got)<=ULTRA_PORT_PROFILE_BOARD_COUNT && got==table[b],
              "table profile for %u boards",boards[b]);
        want=p<table[b]?ULTRA_UPDATE_LINE_TOO_FAST:0;
        o.port_type=ULTRA_PORT_D2A_PIN;cu_setup(&line,cu_boards,boards[b],(uint8_t)p);
        memset(cu_results,0xee,sizeof cu_results);memset(&j,0xee,sizeof j);
        rc=ultra_update_job_begin(&j,&line,cu_nodes,boards[b],a,1,cu_results,&o,at);
        CHECK(rc==want && (want?!line.update_active && j.state==0xee && cu_results[0].result==0xee:line.update_active),
              "job: %u D2A boards at profile %u -> %d (want %d)",boards[b],p,rc,want);
        cu_setup(&line,cu_boards,boards[b],(uint8_t)p);memset(&u,0xee,sizeof u);
        rc=ultra_update_begin(&u,&line,&a->descriptor,&cu_participants[0].target,&cu_participants[0].layout,
              cu_participants[0].uid,ULTRA_PORT_D2A_PIN,33,3,2,at,cu_read,NULL);
        CHECK(rc==want && (want?!line.update_active && u.state==0xee:line.update_active),
              "begin: %u D2A boards at profile %u -> %d",boards[b],p,rc);
        cu_setup(&line,cu_boards,boards[b],(uint8_t)p);
        rc=ultra_update_begin_group(&u,&line,&a->descriptor,cu_participants,boards[b],ULTRA_PORT_D2A_PIN,33,3,at,cu_read,NULL);
        CHECK(rc==want,"group: %u D2A boards at profile %u -> %d",boards[b],p,rc);
        CHECK(ultra_port_line_too_fast(ULTRA_PORT_D2A_PIN,&line)==(want!=0),"too-fast helper");
    }
    /* Diff-Power: 800 kHz admitted for every board count up to eight. */
    o.port_type=ULTRA_PORT_DIFF_POWER;
    for(b=1;b<=ULTRA_PORT_DIFF_POWER_MAX_BOARDS;++b){cu_setup(&line,cu_boards,(uint16_t)b,0);
        CHECK(!ultra_port_line_too_fast(ULTRA_PORT_DIFF_POWER,&line) &&
              !ultra_update_job_begin(&j,&line,cu_nodes,(uint16_t)b,a,1,cu_results,&o,at),"Diff-Power %u boards at 800 kHz",b);}
}
static void cu_pass_min(ultra_update_artifact_t *a)
{
    ultra_sender_t line;ultra_update_t u;ultra_update_job_t j;ultra_update_job_options_t o={0};
    unsigned passes;int64_t at=1000000;
    o.port_type=ULTRA_PORT_DIFF_POWER;o.first_session=1;
    for(passes=0;passes<3;++passes){
        cu_setup(&line,cu_boards,1,0);memset(&u,0xee,sizeof u);
        CHECK(ultra_update_begin(&u,&line,&a->descriptor,&cu_nodes[0].participant.target,&cu_nodes[0].participant.layout,
              NULL,o.port_type,1,(uint8_t)passes,2,at,cu_read,NULL)==ULTRA_UPDATE_TOO_FEW_PASSES &&
              !line.update_active && u.state==0xee,"begin: %u passes explicitly refused before mutation",passes);
        cu_setup(&line,cu_boards,1,0);memset(&u,0xee,sizeof u);
        CHECK(ultra_update_begin_mode(&u,&line,&a->descriptor,&cu_nodes[0].participant.target,&cu_nodes[0].participant.layout,
              NULL,o.port_type,1,(uint8_t)passes,2,ULTRA_UPDATE_BLIND,at,cu_read,NULL)==ULTRA_UPDATE_TOO_FEW_PASSES &&
              !line.update_active && u.state==0xee,"begin_mode: %u passes explicitly refused before mutation",passes);
        cu_setup(&line,cu_boards,1,0);memset(&u,0xee,sizeof u);
        CHECK(ultra_update_begin_group(&u,&line,&a->descriptor,cu_participants,1,o.port_type,1,(uint8_t)passes,at,cu_read,NULL)==
              ULTRA_UPDATE_TOO_FEW_PASSES && !line.update_active && u.state==0xee,"group: %u passes explicitly refused",passes);
        cu_setup(&line,cu_boards,1,0);o.passes=(uint8_t)passes;memset(cu_results,0xee,sizeof cu_results);memset(&j,0xee,sizeof j);
        CHECK(ultra_update_job_begin(&j,&line,cu_nodes,1,a,1,cu_results,&o,at)==ULTRA_UPDATE_TOO_FEW_PASSES &&
              !line.update_active && j.state==0xee && cu_results[0].result==0xee,"job: %u passes explicitly refused",passes);
    }
}
static void test_chain_update_bound(void)
{
    ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_artifact_t a={0};
    ultra_sender_t line;ultra_update_t u;upd_target_t t;ultra_update_layout_t l;ultra_update_plan_t plan;
    uint8_t f[48];unsigned i;int64_t at;
    enum { N=ULTRA_PORT_DIFF_POWER_MAX_BOARDS };
    printf("update storage (host ABI): transfer %u B, job %u B, node %u B, result %u B; zero heap\n",
        (unsigned)sizeof u,(unsigned)sizeof j,(unsigned)sizeof cu_nodes[0],(unsigned)sizeof cu_results[0]);
    cu_image(&a.descriptor,&t,&l,1,503808);
    CHECK(!ultra_update_plan(&plan,&a.descriptor,&t,&l,500,cu_read,NULL) &&
          plan.erase_units==124 && plan.lead_ms==24900 && plan.pause_ms==61,"full V407 erase cursor");
    cu_image(&a.descriptor,&t,&l,1,63488);
    CHECK(!ultra_update_plan(&plan,&a.descriptor,&t,&l,500,cu_read,NULL) &&
          plan.erase_units==17 && plan.lead_ms==3500,"actual image erase cursor");
    /* a legal full-slot lead (200 ms erase bound) stays inside the
     * transit hold; a trusted layout whose lead exceeds that hold is refused. */
    cu_image(&a.descriptor,&t,&l,1,503808);l.geometry[2]=200;
    CHECK(!ultra_update_plan(&plan,&a.descriptor,&t,&l,500,cu_read,NULL) && plan.lead_ms==49700 &&
          upd_transit_hold_ms(a.descriptor.bytes)==63340,"49.7 s full-slot lead inside the 63.34 s transit hold");
    l.geometry[2]=255;
    CHECK(!ultra_update_plan(&plan,&a.descriptor,&t,&l,500,cu_read,NULL) && plan.lead_ms==63340,
          "255 ms erase bound: lead equals the transit hold");
    cu_image(&a.descriptor,&t,&l,0,516);l.geometry[2]=255;l.metadata_base=0x08001f00;l.metadata_bytes=512;
    CHECK(ultra_update_plan(&plan,&a.descriptor,&t,&l,500,cu_read,NULL)==UPD_ERROR_BOUNDS,
          "lead above the transit hold (5 units, hold counts 4) refused");
    l.geometry[2]=200;
    CHECK(!ultra_update_plan(&plan,&a.descriptor,&t,&l,500,cu_read,NULL) && plan.erase_units==5 &&
          plan.lead_ms==2100 && plan.lead_ms<=upd_transit_hold_ms(a.descriptor.bytes),"same layout inside the hold accepted");
    cu_image(&a.descriptor,&t,&l,0,512);cu_inventory(&t,&l);
    a.authenticated=1;a.read=cu_read;cu_pass_min(&a);cu_line_speed(&a);
    cu_port_limit(ULTRA_PORT_DIFF_POWER,ULTRA_PORT_DIFF_POWER_MAX_BOARDS,&a);
    cu_port_limit(ULTRA_PORT_D2A_PIN,ULTRA_PORT_D2A_PIN_MAX_BOARDS,&a);
    /* No default: a port without a known type is refused. */
    cu_setup(&line,cu_boards,1,0);o.passes=3;o.first_session=255;at=1000000;
    CHECK(ultra_port_max_boards(0)==0 && ultra_port_max_boards(3)==0,"unknown port types");
    CHECK(ultra_update_job_begin(&j,&line,cu_nodes,1,&a,1,cu_results,&o,at)==-1 && !line.update_active,"job without port type");
    CHECK(ultra_update_begin_group(&u,&line,&a.descriptor,cu_participants,1,0,33,3,at,cu_read,NULL)==-1,"group without port type");
    for(i=0;i<2;++i){uint8_t type=i?255:0;
        CHECK(ultra_update_begin(&u,&line,&a.descriptor,&t,&l,cu_participants[0].uid,type,33,3,2,at,cu_read,NULL)==-1 &&
              !line.update_active,"single-target begin refuses unknown type %u",type);
        cu_setup(&line,cu_boards,1,0);
        CHECK(ultra_update_begin_mode(&u,&line,&a.descriptor,&t,&l,cu_participants[0].uid,type,33,3,2,
              ULTRA_UPDATE_BLIND,at,cu_read,NULL)==-1 && !line.update_active,"begin_mode refuses unknown type %u",type);
        cu_setup(&line,cu_boards,1,0);
    }
    o.port_type=ULTRA_PORT_DIFF_POWER;
    cu_setup(&line,cu_boards,N,0);
    cu_nodes[N-1].participant.layout.entry_10ms=255;cu_nodes[0].blackout_ms=300;
    at=4294900000LL;
    CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,N,&a,1,cu_results,&o,at),"eight-board port inventory");
    CHECK(j.entry_ms==2850,"whole-port entry and independent maximum blackout");
    for(i=0;i<400 && j.transfer.state!=ULTRA_UPDATE_DATA;++i){at=j.due_us;
        CHECK(ultra_update_job_next_frame(&j,at,f,sizeof f)>=0,"port pre-entry");}
    CHECK(j.transfer.state==ULTRA_UPDATE_DATA,"reached reserved erase wait");
    ultra_update_job_abort(&j);
    CHECK(!ultra_update_job_next(&j,j.due_us-1,f) && line.update_active,"abort cannot cross erase deadline");
    at=j.due_us;CHECK(ultra_update_job_next(&j,at,f)==48 && f[0]==UPD_ABORT && line.update_active,"abort at busy boundary");
    CHECK(j.state==ULTRA_JOB_ABORTING && j.due_us>=j.transfer.poll_end_us+2100000 &&
          j.due_us==j.transfer.transit_until_us+2100000 && line.hold_us==j.due_us &&
          j.transfer.transit_until_us>0,"abandoned ANNOUNCE: transit hold + 2100 ms quiet");
    CHECK(!ultra_update_job_next(&j,j.due_us-1,f) && line.update_active,"abort quiet keeps lease");
    CHECK(!ultra_update_job_next(&j,j.due_us,f) && !line.update_active && j.state==ULTRA_JOB_DONE,"abort releases after quiet");
    for(i=0;i<N;++i)CHECK(cu_results[i].result==ULTRA_JOB_FAILED && !cu_results[i].transfer_complete,"abort result %u",i);
    /* One-way boards have no discoverable UID: index identifies their results. */
    cu_inventory(&t,&l);o.port_type=ULTRA_PORT_D2A_PIN;
    for(i=0;i<50;++i){cu_nodes[i].participant.has_return=0;memset(cu_nodes[i].participant.uid,0,12);}
    cu_setup(&line,cu_boards,50,3);
    CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,50,&a,1,cu_results,&o,at) && line.update_active,
          "50 UID-less one-way boards admitted");
    if(line.update_active){uint8_t wide[50*32+8];unsigned steps;
        for(steps=0;steps<3000 && j.state!=ULTRA_JOB_DONE;++steps){at=j.due_us;
            CHECK(ultra_update_job_next_frame(&j,at,wide,sizeof wide)>=0,"UID-less group delivery");}
        CHECK(j.state==ULTRA_JOB_DONE && !line.update_active && j.sessions==1,"one blind UID-less subtransfer");
        for(i=0;i<50;++i)CHECK(cu_results[i].result==ULTRA_JOB_SENT_UNVERIFIED &&
            cu_results[i].reason==ULTRA_JOB_NO_RETURN && cu_results[i].transfer_complete,"UID-less result index %u",i);
    }
    cu_setup(&line,cu_boards,2,0);cu_nodes[0].participant.uid[0]=cu_nodes[1].participant.uid[0]=7;
    memset(cu_results,0xee,sizeof cu_results);
    CHECK(ultra_update_job_begin(&j,&line,cu_nodes,2,&a,1,cu_results,&o,at)==-UPD_ERROR_TARGET &&
          !line.update_active && cu_results[0].result==0xee,"duplicate nonzero one-way UIDs refused");
    memset(cu_nodes[0].participant.uid,0,12);memset(cu_nodes[1].participant.uid,0,12);
    cu_nodes[0].participant.has_return=1;
    CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,2,&a,1,cu_results,&o,at),"UID-less one-way node exempt beside return node");
    cu_setup(&line,cu_boards,2,0);cu_nodes[1].participant.has_return=1;
    CHECK(ultra_update_job_begin(&j,&line,cu_nodes,2,&a,1,cu_results,&o,at)==-UPD_ERROR_TARGET &&
          !line.update_active,"duplicate return-channel zero UIDs refused");
}
/* ABORT after an ANNOUNCE with no DATA/END since waits that ANNOUNCE's
 * transit hold (upd_transit_hold_ms) + 2100 ms from its physical end; once DATA
 * went out, 2100 ms after the ABORT. */
static void test_chain_update_abandon(void)
{
    ultra_sender_t line;ultra_sender_board_t board;ultra_update_t u;upd_descriptor_t d;
    upd_target_t t;ultra_update_layout_t l;uint8_t uid[12]={1},f[48];unsigned k,data;int64_t at,announce=0;
    cu_image(&d,&t,&l,1,503808);l.geometry[2]=200;cu_setup(&line,&board,1,0);at=1000000;
    CHECK(!ultra_update_begin(&u,&line,&d,&t,&l,uid,ULTRA_PORT_DIFF_POWER,5,3,2,at,cu_read,NULL) && u.plan.lead_ms==49700,
          "abandon: full layout-4 slot, 49.7 s lead");
    for(k=0;k<800 && u.state!=ULTRA_UPDATE_DATA;++k){at=u.due_us;
        CHECK(ultra_update_next(&u,at,f)>=0,"abandon next");
        if(f[0]==UPD_ANNOUNCE)announce=u.poll_end_us;}
    CHECK(u.state==ULTRA_UPDATE_DATA && announce && u.transit_until_us==announce+63340000 &&
          u.due_us==announce+49700000,"boot pair sent, erase lead reserved");
    at=u.due_us;CHECK(ultra_update_abort(&u,at,f)==48 && f[0]==UPD_ABORT,"abandon: ABORT at the lead's end");
    CHECK(u.due_us==announce+(63340+2100)*1000LL && line.hold_us==u.due_us,"abandon: 65.44 s from the boot pair");
    CHECK(!ultra_update_next(&u,u.due_us-1,f) && line.update_active,"abandon: lease kept inside the hold");
    CHECK(!ultra_update_next(&u,u.due_us,f) && !line.update_active,"abandon: lease released after hold + quiet");
    /* After DATA the hold is over: the ABORT's own 2100 ms quiet. */
    cu_image(&d,&t,&l,0,516);cu_setup(&line,&board,1,0);at=1000000;data=0;
    CHECK(!ultra_update_begin(&u,&line,&d,&t,&l,uid,ULTRA_PORT_DIFF_POWER,6,3,2,at,cu_read,NULL),"post-DATA abort begin");
    for(k=0;k<800 && data<3;++k){at=u.due_us;CHECK(ultra_update_next(&u,at,f)>=0,"post-DATA next");
        if(f[0]==UPD_DATA)++data;}
    CHECK(data==3 && !u.transit_until_us,"DATA ends the transit hold");
    at=u.due_us;CHECK(ultra_update_abort(&u,at,f)==48 && f[0]==UPD_ABORT && u.due_us==u.poll_end_us+2100000,
          "post-DATA ABORT: 2100 ms quiet");
}
/* an ordinary frame sent during verification keeps its duration plus the
 * port's processing LOW before ABORT (was 1 us LOW after its physical end). */
static void test_chain_update_abort_gap(void)
{
    ultra_sender_t line;ultra_sender_board_t board;ultra_update_job_t j;ultra_update_job_options_t o={0};
    ultra_update_artifact_t a={0};ultra_update_node_t n;ultra_update_result_t r;uint8_t f[48];
    int64_t at=1000000,end,free_us;unsigned k;
    memset(&n,0,sizeof n);cu_image(&a.descriptor,&n.participant.target,&n.participant.layout,0,516);
    a.authenticated=1;a.read=cu_read;n.trusted=n.reachable=n.participant.has_return=1;n.firmware=1;
    n.participant.uid[0]=1;cu_setup(&line,&board,1,0);o.passes=3;o.first_session=1;o.port_type=ULTRA_PORT_DIFF_POWER;
    CHECK(!ultra_update_job_begin(&j,&line,&n,1,&a,1,&r,&o,at),"abort-gap job");
    for(k=0;k<1000 && j.state!=ULTRA_JOB_VERIFY;++k){at=j.due_us;CHECK(ultra_update_job_next_frame(&j,at,f,sizeof f)>=0,"abort-gap next");}
    CHECK(j.state==ULTRA_JOB_VERIFY,"abort-gap verify");
    CHECK(ultra_sender_next_frame(&line,at,f)==40,"verification DARK");
    end=ultra_wire_end_us(&line.timing,f,40,at);free_us=at+(int64_t)line.frame_us+j.gap_us;
    CHECK(free_us>=end+(int64_t)j.gap_us,"frame plus processing LOW");
    ultra_update_job_abort(&j);
    CHECK(!ultra_update_job_next_frame(&j,end+1,f,sizeof f) && j.transfer.state!=ULTRA_UPDATE_ABORTED,"no ABORT inside the gap");
    CHECK(j.due_us==free_us,"abort retries at the frame's end plus gap");
    CHECK(!ultra_update_job_next_frame(&j,free_us-1,f,sizeof f),"still inside the gap");
    CHECK(ultra_update_job_next_frame(&j,free_us,f,sizeof f)==48 && f[0]==UPD_ABORT &&
          j.transfer.transport.frame_start_us-end>=(int64_t)j.gap_us,"ABORT after the processing LOW");
}
/* entry/reboot waits are bound + port processing gap from the
 * physical frame end, never max(bound, gap). */
static void test_chain_update_bound_waits(void)
{
    ultra_sender_t line;ultra_sender_board_t board;ultra_update_t u;upd_descriptor_t d;
    upd_target_t t;ultra_update_layout_t l;uint8_t uid[12]={1},f[48];unsigned k;int64_t at=1000000;
    int entry=0,end=0;
    cu_image(&d,&t,&l,0,516);l.entry_10ms=255;cu_setup(&line,&board,1,0);
    CHECK(!ultra_update_begin(&u,&line,&d,&t,&l,uid,ULTRA_PORT_DIFF_POWER,9,3,2,at,cu_read,NULL),"bound-wait begin");
    CHECK(u.gap_us>=500 && u.plan.entry_ms==2550,"bound-wait plan");
    for(k=0;k<800 && u.state!=ULTRA_UPDATE_CONFIRM;++k){uint8_t before=u.state;at=u.due_us;
        CHECK(ultra_update_next(&u,at,f)>=0,"bound-wait next");
        if(before==ULTRA_UPDATE_ANNOUNCE && u.state==ULTRA_UPDATE_ENTRY){entry=1;
            CHECK(u.due_us==u.poll_end_us+2550000+u.gap_us,"entry wait = bound + gap");}
        if(u.state==ULTRA_UPDATE_QUIET && f[0]==UPD_END && before!=ULTRA_UPDATE_QUIET){end=1;
            CHECK(u.due_us==u.poll_end_us+((int64_t)u.plan.finish_ms+2550)*1000+u.gap_us,"reboot wait = commit + entry + gap");}
    }
    /* Shorter bounds: the 2100 ms host quiet after END and after the
     * session-zero PING sets the wait (2000 ms idle close + tick margin). */
    cu_image(&d,&t,&l,0,516);cu_setup(&line,&board,1,0);at=1000000;end=0;entry=0;
    CHECK(!ultra_update_begin(&u,&line,&d,&t,&l,uid,ULTRA_PORT_DIFF_POWER,9,3,2,at,cu_read,NULL),"quiet begin");
    for(k=0;k<800 && u.state!=ULTRA_UPDATE_CONFIRM;++k){at=u.due_us;
        CHECK(ultra_update_next(&u,at,f)>=0,"quiet next");
        if(f[0]==UPD_END && u.state==ULTRA_UPDATE_QUIET && !end){end=1;
            CHECK(u.due_us==u.poll_end_us+2100000 && line.hold_us==u.due_us,"END: 2100 ms quiet");}
        if(f[0]==UPD_PING && !f[1] && u.state==ULTRA_UPDATE_CONFIRM_WAIT){entry=1;
            CHECK(u.due_us==u.poll_end_us+2100000 && line.hold_us==u.due_us,"confirmation PING: 2100 ms quiet");}
    }
    CHECK(end && entry && u.state==ULTRA_UPDATE_CONFIRM,"quiet schedule complete");
    CHECK(entry && end && u.state==ULTRA_UPDATE_CONFIRM,"bound-wait schedule complete");
}
/* a subtransfer the sender refuses at begin keeps its code in the job;
 * its nodes end REFUSED, nodes not yet tried ABORT, never "transport". */
static void test_chain_update_refused(void)
{
    ultra_sender_t line;ultra_update_job_t j;ultra_update_job_options_t o={0};
    ultra_update_artifact_t a[2];upd_target_t t;ultra_update_layout_t l;uint8_t f[48];unsigned i;int64_t at=1000000;int rc;
    memset(a,0,sizeof a);
    cu_image(&a[1].descriptor,&t,&l,0,516);cu_inventory(&t,&l);
    cu_image(&a[0].descriptor,&t,&l,1,516);cu_nodes[0].participant.target=t;cu_nodes[0].participant.layout=l;
    a[0].descriptor.bytes[24]^=1;ultra_put32(a[0].descriptor.bytes+60,upd_crc32_words(0xffffffffu,a[0].descriptor.bytes,15));
    for(i=0;i<2;++i){a[i].authenticated=1;a[i].read=cu_read;}
    o.passes=3;o.first_session=1;o.port_type=ULTRA_PORT_DIFF_POWER;cu_setup(&line,cu_boards,2,0);
    CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,2,a,2,cu_results,&o,at) && !j.begin_code,"refused: job admitted");
    rc=ultra_update_job_next(&j,at,f);
    CHECK(rc==-UPD_ERROR_CRC && j.begin_code==-UPD_ERROR_CRC && j.error==UPD_ERROR_CRC && j.active_artifact==0 &&
          j.state==ULTRA_JOB_DONE && !line.update_active,"body CRC refused at begin: code kept (%d)",rc);
    CHECK(cu_results[0].result==ULTRA_JOB_FAILED && cu_results[0].reason==ULTRA_JOB_REFUSED &&
          cu_results[1].result==ULTRA_JOB_FAILED && cu_results[1].reason==ULTRA_JOB_ABORT,
          "refused node REFUSED, untried node ABORT");
    /* The pin retuned faster than its speed table after the job began. */
    a[0]=a[1];cu_inventory(&t,&l);for(i=0;i<21;++i){cu_nodes[i].participant.target=t;cu_nodes[i].participant.layout=l;}
    cu_image(&a[0].descriptor,&t,&l,1,516);a[0].authenticated=1;a[0].read=cu_read;
    o.port_type=ULTRA_PORT_D2A_PIN;cu_setup(&line,cu_boards,21,1);
    CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,21,a,1,cu_results,&o,at),"refused: 21-board pin admitted at profile 1");
    line.timing.profile=0;rc=ultra_update_job_next(&j,at,f);
    CHECK(rc==ULTRA_UPDATE_LINE_TOO_FAST && j.begin_code==ULTRA_UPDATE_LINE_TOO_FAST && !j.error &&
          j.state==ULTRA_JOB_DONE && !line.update_active,"line too fast refused at begin: code kept");
    for(i=0;i<21;++i)CHECK(cu_results[i].result==ULTRA_JOB_FAILED && cu_results[i].reason==ULTRA_JOB_REFUSED,
          "blind subtransfer node %u REFUSED",i);
}
/* fresh rediscovery of a node (IDENTITY parts 0/1/2 + FW_UPDATE) at
 * firmware, matching its artifact's length and CRCs, in answer slot 0. */
static int cu_confirm(ultra_update_job_t *j,uint16_t node,uint32_t firmware,int64_t evidence)
{
    ultra_identity_t id;uint8_t answer[32],data[24];
    const ultra_update_artifact_t *a=j->artifacts+j->results[node].artifact;
    memset(&id,0,sizeof id);id.parts=7;id.target=j->nodes[node].participant.target;
    memcpy(id.uid,j->nodes[node].participant.uid,12);id.firmware=firmware;
    memset(data,0,sizeof data);data[0]=1;data[1]=3;ultra_put32(data+4,firmware);
    ultra_put32(data+8,ultra_get32(a->descriptor.bytes+20));ultra_put32(data+12,ultra_get32(a->descriptor.bytes+24));
    ultra_put32(data+16,ultra_get32(a->descriptor.bytes+60));
    memset(answer,0,sizeof answer);answer[0]=0xa5;answer[1]=ULTRA_PAGE_FW_UPDATE;
    ultra_put16(answer+4,ultra_crc16(id.uid,12));memcpy(answer+6,data,24);ultra_slot_seal(answer);
    return ultra_update_job_confirm(j,node,0,&id,answer,evidence);
}
/* Port: node 0 a reachable V407, node 1 a V203 that is trusted (ledger) but
 * unreachable, at ledger version `dark`; artifacts V407 (0) and V203 (1). */
static void cu_dark_port(ultra_update_artifact_t a[2],uint32_t v407,uint32_t dark)
{
    upd_target_t t;ultra_update_layout_t l;unsigned i;
    memset(a,0,2*sizeof *a);
    cu_image(&a[0].descriptor,&t,&l,1,516);cu_inventory(&t,&l);
    cu_image(&a[1].descriptor,&t,&l,0,516);
    for(i=1;i<CU_MAX;++i){cu_nodes[i].participant.target=t;cu_nodes[i].participant.layout=l;}
    for(i=0;i<2;++i){a[i].authenticated=1;a[i].read=cu_read;}
    cu_nodes[0].firmware=v407;cu_nodes[1].firmware=dark;cu_nodes[1].reachable=0;
}
/* a reference receiver for the installed-version gate, fed the job's frames. Application stage: a complete
 * ANNOUNCE is checked against the board's target, then against the installed image (an equal firmware is SAME, an
 * older one REJECTED, unless the ANNOUNCE carries FORCE) before the board enters its bootloader; a revoked board
 * (no valid installed descriptor) sits in the bootloader already. Bootloader stage: the same two checks before
 * the revoke. Counters: same / rejected / refused (no change), revokes + programs (flash operations), installs. */
typedef struct {
    upd_target_t target;uint8_t boot_version,boot,desc_ok,receiving,pending_session,have,session,decided;
    uint32_t version,chunks;uint8_t half[2][32],desc[UPD_DESC_BYTES],body[576];
    unsigned same,rejected,refused,revokes,programs,installs;
} cu_rx_t;
static void cu_rx_init(cu_rx_t *r,const ultra_update_node_t *n,int installed)
{
    memset(r,0,sizeof *r);r->target=n->participant.target;r->boot_version=n->participant.layout.boot_version;
    r->desc_ok=(uint8_t)installed;r->boot=!installed;r->version=n->firmware;
}
static void cu_rx_announce(cu_rx_t *r,const uint8_t *f)
{
    uint32_t v;
    if(r->receiving || (r->decided && f[1]==r->session))return;
    memcpy(r->desc,r->half[0],32);memcpy(r->desc+32,r->half[1],32);r->session=f[1];
    if(upd_desc_check(r->desc,&r->target,r->boot_version)){++r->refused;r->decided=1;return;}
    v=ultra_get32(r->desc+UPD_D_FW_VERSION);
    if(r->desc_ok && v<=r->version && !(f[5]&UPD_F_FORCE)){
        if(v==r->version)++r->same;else ++r->rejected;
        if(r->boot)r->decided=1;      /* a bootloader that refused leaves the session decided */
        return;
    }
    if(!r->boot){r->boot=1;return;}   /* the application takes it: reboot into the bootloader, no flash operation */
    r->desc_ok=0;++r->revokes;r->receiving=1;r->chunks=0;memset(r->body,0,sizeof r->body);
}
static void cu_rx_frame(cu_rx_t *r,const uint8_t *f)
{
    uint32_t len,off,n,all;
    if(upd_check_frame(f))return;
    if(f[0]==UPD_ANNOUNCE){unsigned h=ultra_get16(f+2);
        if(h>1)return;
        if(f[1]!=r->pending_session)r->have=0;
        r->pending_session=f[1];memcpy(r->half[h],f+6,32);r->have=(uint8_t)(r->have|(1u<<h));
        if(r->have==3u){r->have=0;cu_rx_announce(r,f);}
        return;
    }
    if(!r->receiving || f[1]!=r->session)return;
    len=ultra_get32(r->desc+UPD_D_BODY_LEN);all=(1u<<((len+31u)/32u))-1u;
    if(f[0]==UPD_DATA){off=ultra_get16(f+2)*32u;
        if(off>=len || (r->chunks&(1u<<(off/32u))))return;
        n=len-off;if(n>32u)n=32u;memcpy(r->body+off,f+6,n);r->chunks|=1u<<(off/32u);++r->programs;
    } else if(f[0]==UPD_END){
        if(r->chunks==all && ultra_get32(f+6)==ultra_get32(r->desc+UPD_D_BODY_CRC) && ultra_get32(f+10)==len &&
           upd_crc32_words(0xffffffffu,r->body,len/4u)==ultra_get32(f+6)){
            r->desc_ok=1;r->version=ultra_get32(r->desc+UPD_D_FW_VERSION);r->receiving=0;r->boot=0;++r->installs;}
    }
}
static cu_rx_t *cu_rx_set;static unsigned cu_rx_n;   /* receivers every frame of cu_run is fed to (0: none) */
static unsigned cu_end_drop,cu_end_seen;             /* the first cu_end_drop END frames of a run never reach the receivers */
/* progress_n: frames sampled with the job in TRANSFER; progress_bad: those whose progress passes differ from the
 * running subtransfer's, or whose one-based pass is outside 1..passes. ends: END frames sent. */
typedef struct { unsigned sessions, selects, data, blind, progress_n, progress_bad, force, ends; } cu_run_t;
/* Drive the job to DONE; rediscover node `node` at `firmware` (0: never). */
static cu_run_t cu_run(ultra_update_job_t *j,int64_t *at,uint16_t node,uint32_t firmware)
{
    cu_run_t s={0,0,0,0,0,0,0,0};uint8_t f[CU_MAX*32+8];unsigned i,k;int rc;ultra_update_job_progress_t pr;
    cu_end_seen=0;
    for(k=0;k<20000 && j->state!=ULTRA_JOB_DONE;++k){*at=j->due_us;
        rc=ultra_update_job_next_frame(j,*at,f,sizeof f);CHECK(rc>=0,"recovery job next (%d)",rc);
        if(rc<0)break;
        if(rc==48 && f[0]==UPD_SELECT)++s.selects;
        if(rc==48 && f[0]==UPD_ANNOUNCE)s.force+=(f[5]&UPD_F_FORCE)!=0;
        if(rc==48){int lost=f[0]==UPD_END && cu_end_seen++<cu_end_drop;
            for(i=0;i<cu_rx_n && !lost;++i)cu_rx_frame(cu_rx_set+i,f);}
        if(rc==48 && f[0]==UPD_END)++s.ends;
        if(rc==48 && f[0]==UPD_DATA)++s.data;
        if(j->state==ULTRA_JOB_TRANSFER && j->sessions>s.sessions){s.sessions=j->sessions;
            s.blind+=j->transfer.mode==ULTRA_UPDATE_BLIND && j->transfer.passes>=ULTRA_UPDATE_BLIND_MIN_PASSES;}
        if(j->state==ULTRA_JOB_TRANSFER && !ultra_update_job_progress(j,*at,&pr)){++s.progress_n;
            s.progress_bad+=pr.passes!=j->transfer.passes || pr.pass<1 || pr.pass>pr.passes;}
        if(firmware && j->state==ULTRA_JOB_VERIFY && j->results[node].result==ULTRA_JOB_PENDING &&
           j->results[node].transfer_complete)
            CHECK(cu_confirm(j,node,cu_rx_n?cu_rx_set[node].version:firmware,*at+1)>=0,"rediscovery of node %u",node);
    }
    CHECK(j->state==ULTRA_JOB_DONE && !j->line->update_active,"recovery job done");
    return s;
}
/* "update all receivers on this port" doesn't run while a known board
 * on the port is silent (something is wrong: power, cable, a board left in its
 * recovery bootloader). The refusal names every silent node; nothing is sent. */
static void cu_silent_refused(ultra_sender_t *line,uint16_t count,const ultra_update_artifact_t *a,
    const ultra_update_job_options_t *o,const char *what)
{
    ultra_update_job_t j;int64_t at=1000000;int rc;
    memset(cu_results,0xee,sizeof cu_results);memset(&j,0xee,sizeof j);
    rc=ultra_update_job_begin(&j,line,cu_nodes,count,a,2,cu_results,o,at);
    CHECK(rc==ULTRA_UPDATE_SILENT_BOARDS && !line->update_active && !line->has_sent && j.state==0xee,
          "silent %s: update all refused before any frame (%d)",what,rc);
}
static void test_chain_update_silent(void)
{
    ultra_sender_t line;ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_artifact_t a[2];
    uint32_t want;int64_t at=1000000;cu_run_t s;unsigned i,mode;
    cu_dark_port(a,0,1);want=ultra_get32(a[0].descriptor.bytes+16);
    o.passes=3;o.first_session=1;o.port_type=ULTRA_PORT_DIFF_POWER;o.verify_timeout_ms=50;
    /* A silent known V203 next to a current V407, in both job modes: refused, the V203 named. */
    for(mode=0;mode<2;++mode){
        cu_dark_port(a,want,1);o.mode=mode?ULTRA_UPDATE_SELECTED:ULTRA_UPDATE_BLIND;o.passes=mode?1:3;
        cu_setup(&line,cu_boards,2,0);cu_silent_refused(&line,2,a,&o,mode?"selected":"blind");
        CHECK(cu_results[1].result==ULTRA_JOB_UNREACHABLE && cu_results[1].reason==ULTRA_JOB_NOT_ANSWERING &&
              cu_results[1].artifact==1 && cu_results[1].before==1 && cu_results[1].observed==1 &&
              !cu_results[1].planned && !cu_results[1].recovery,"silent: the V203 is named as not answering");
        CHECK(cu_results[0].result==ULTRA_JOB_SKIPPED && cu_results[0].reason==ULTRA_JOB_PORT_BLOCKED &&
              cu_results[0].artifact==0 && cu_results[0].before==want && !cu_results[0].planned,
              "silent: the current V407 is skipped, port blocked");
    }
    o.mode=ULTRA_UPDATE_BLIND;o.passes=3;
    /* An equal or newer silent board blocks the port the same way; so does one alone on its port. */
    for(i=0;i<2;++i){cu_dark_port(a,want,want+i);cu_setup(&line,cu_boards,2,0);
        cu_silent_refused(&line,2,a,&o,i?"newer":"equal");
        CHECK(cu_results[1].reason==ULTRA_JOB_NOT_ANSWERING,"silent: %s version named",i?"newer":"equal");}
    cu_dark_port(a,0,1);cu_nodes[0]=cu_nodes[1];cu_setup(&line,cu_boards,1,0);cu_silent_refused(&line,1,a,&o,"alone");
    CHECK(cu_results[0].result==ULTRA_JOB_UNREACHABLE && cu_results[0].reason==ULTRA_JOB_NOT_ANSWERING,"silent: board alone named");
    /* Every silent node is named: nodes 1 and 3 of four; 0 and 2 answer. */
    cu_dark_port(a,0,1);cu_nodes[3].reachable=0;cu_setup(&line,cu_boards,4,0);cu_silent_refused(&line,4,a,&o,"two of four");
    for(i=0;i<4;++i)CHECK(cu_results[i].reason==(i&1u?ULTRA_JOB_NOT_ANSWERING:ULTRA_JOB_PORT_BLOCKED) &&
                          cu_results[i].result==(i&1u?ULTRA_JOB_UNREACHABLE:ULTRA_JOB_SKIPPED),"silent: node %u named right",i);
    /* An untrusted node still refuses the job as before, results untouched. */
    cu_dark_port(a,0,1);cu_nodes[1].trusted=0;cu_setup(&line,cu_boards,2,0);
    memset(cu_results,0xee,sizeof cu_results);memset(&j,0xee,sizeof j);
    CHECK(ultra_update_job_begin(&j,&line,cu_nodes,2,a,2,cu_results,&o,at)==-UPD_ERROR_BOUNDS && !line.update_active &&
          j.state==0xee && cu_results[1].result==0xee,"silent: an untrusted node refuses the job, results untouched");
    /* The port limits and the D2A speed table come first. */
    cu_dark_port(a,0,1);cu_setup(&line,cu_boards,ULTRA_PORT_DIFF_POWER_MAX_BOARDS+1,0);
    CHECK(ultra_update_job_begin(&j,&line,cu_nodes,ULTRA_PORT_DIFF_POWER_MAX_BOARDS+1,a,2,cu_results,&o,at)==
          ULTRA_UPDATE_TOO_MANY_BOARDS && !line.update_active,"silent: 9 Diff-Power nodes refused as too many");
    o.port_type=ULTRA_PORT_D2A_PIN;cu_setup(&line,cu_boards,CU_MAX,3);
    CHECK(ultra_update_job_begin(&j,&line,cu_nodes,CU_MAX,a,2,cu_results,&o,at)==ULTRA_UPDATE_TOO_MANY_BOARDS &&
          !line.update_active,"silent: 51 D2A nodes refused as too many");
    cu_setup(&line,cu_boards,ULTRA_D2A_800_MAX_BOARDS+1,ULTRA_PROFILE_800);
    CHECK(ultra_update_job_begin(&j,&line,cu_nodes,ULTRA_D2A_800_MAX_BOARDS+1,a,2,cu_results,&o,at)==
          ULTRA_UPDATE_LINE_TOO_FAST && !line.update_active,"silent: a D2A pin above its speed table refused as too fast");
    o.port_type=ULTRA_PORT_DIFF_POWER;
    /* A one-way board has no return path to be silent on: the job runs, it ends sent, unverified. */
    cu_dark_port(a,want,1);cu_nodes[1].participant.has_return=0;cu_setup(&line,cu_boards,2,0);
    CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,2,a,2,cu_results,&o,at),"silent: one-way node admitted");
    s=cu_run(&j,&at,1,0);
    CHECK(s.sessions==1 && s.blind==1 && cu_results[1].result==ULTRA_JOB_SENT_UNVERIFIED &&
          cu_results[1].reason==ULTRA_JOB_NO_RETURN,"silent: one-way node sent, unverified");
    /* Every board answering: the job runs and the V203 is verified. */
    cu_dark_port(a,want,1);cu_nodes[1].reachable=1;cu_setup(&line,cu_boards,2,0);
    CHECK(!ultra_update_job_begin(&j,&line,cu_nodes,2,a,2,cu_results,&o,at),"silent: answering port admitted");
    s=cu_run(&j,&at,1,want);
    CHECK(s.sessions==1 && cu_results[1].result==ULTRA_JOB_VERIFIED,"silent: answering V203 verified");
}
/* the receiver model's gate on one complete ANNOUNCE (the artifact's descriptor, two halves). */
static void cu_gate_announce(cu_rx_t *r,const upd_descriptor_t *d,uint8_t session,uint8_t flags)
{
    uint8_t f[48];unsigned h;
    for(h=0;h<2;++h){upd_build_frame(f,UPD_ANNOUNCE,session,(uint16_t)h,0,flags,d->bytes+32u*h,0);cu_rx_frame(r,f);}
}
static void test_chain_update_install_gate(void)
{
    ultra_update_artifact_t a[2];ultra_update_node_t n;cu_rx_t r;uint32_t want;unsigned k;
    cu_dark_port(a,0,1);want=ultra_get32(a[1].descriptor.bytes+16);n=cu_nodes[1];
    /* The application stage: an equal image is SAME, an older one REJECTED; nothing is revoked or written. */
    for(k=0;k<2;++k){n.firmware=want+k;cu_rx_init(&r,&n,1);cu_gate_announce(&r,&a[1].descriptor,5,0);
        CHECK(r.same==(k==0?1u:0u) && r.rejected==(k==1?1u:0u) && !r.boot && !r.revokes && !r.programs && r.desc_ok && r.version==want+k,
              "install gate: %s installed firmware skips the image",k?"newer":"equal");
        /* FORCE lifts the version gate only: the board enters its bootloader, and the second ANNOUNCE revokes. */
        cu_gate_announce(&r,&a[1].descriptor,6,UPD_F_FORCE);cu_gate_announce(&r,&a[1].descriptor,6,UPD_F_FORCE);
        CHECK(r.revokes==1 && r.receiving && !r.desc_ok,"install gate: FORCE revokes an %s installed firmware",k?"newer":"equal");}
    /* An older installed firmware is replaced; the bootloader of a revoked board applies it at once. */
    n.firmware=1;cu_rx_init(&r,&n,1);cu_gate_announce(&r,&a[1].descriptor,7,0);cu_gate_announce(&r,&a[1].descriptor,7,0);
    CHECK(r.boot && r.revokes==1 && r.receiving,"install gate: an older installed firmware is taken");
    cu_rx_init(&r,&n,0);cu_gate_announce(&r,&a[1].descriptor,8,0);
    CHECK(r.revokes==1 && r.receiving && !r.same && !r.rejected,"install gate: a revoked board applies it without FORCE");
    /* A bootloader refuses an equal or older image before any revoke, FORCE aside. */
    n.firmware=want;cu_rx_init(&r,&n,1);r.boot=1;cu_gate_announce(&r,&a[1].descriptor,9,0);
    CHECK(r.same==1 && !r.revokes && r.desc_ok && r.decided,"install gate: the bootloader refuses an equal image before the revoke");
    /* Another target never matches. */
    cu_rx_init(&r,&n,1);r.target=cu_nodes[0].participant.target;r.boot=1;cu_gate_announce(&r,&a[1].descriptor,10,UPD_F_FORCE);
    CHECK(r.refused==1 && !r.revokes,"install gate: a foreign target is refused, FORCE or not");
}
/* the explicit RECOVER action for one silent known board. Port: a current reachable V407 (node 0) and a dark
 * trusted V203 (node 1) at ledger version 1. Only the V203 artifact goes out, BLIND, 3 passes, in either job mode. */
static void test_chain_update_recover(void)
{
    ultra_sender_t line;ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_artifact_t a[2];
    uint32_t want;int64_t at=1000000;cu_run_t s;memset(&j,0,sizeof j);
    cu_dark_port(a,0,1);want=ultra_get32(a[0].descriptor.bytes+16);
    o.passes=3;o.first_session=1;o.port_type=ULTRA_PORT_DIFF_POWER;o.verify_timeout_ms=50;
    cu_dark_port(a,want,1);cu_setup(&line,cu_boards,2,0);
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,2,1,a,2,cu_results,&o,at),"recover: job admitted");
    CHECK(cu_results[0].result==ULTRA_JOB_SKIPPED && cu_results[0].reason==ULTRA_JOB_OTHER_TARGET && !cu_results[0].planned,
          "recover: the V407 (other artifact) is not sent");
    CHECK(cu_results[1].result==ULTRA_JOB_PENDING && cu_results[1].planned && cu_results[1].recovery &&
          cu_results[1].artifact==1 && cu_results[1].before==1,"recover: the dark V203 is the planned target");
    s=cu_run(&j,&at,1,want);
    CHECK(s.sessions==1 && s.blind==1 && !s.selects && !s.force && s.data>=3*17u,"recover: V203 artifact went out blind, 3 passes");
    CHECK(s.progress_n>100 && !s.progress_bad,"recover: progress passes = 3 (%u frames, %u wrong)",s.progress_n,s.progress_bad);
    CHECK(cu_results[1].transfer_complete && cu_results[1].result==ULTRA_JOB_VERIFIED && cu_results[1].observed==want,
          "recover: rediscovered with the expected firmware -> VERIFIED");
    CHECK(cu_results[0].result==ULTRA_JOB_SKIPPED && cu_results[0].reason==ULTRA_JOB_OTHER_TARGET,"recover: V407 untouched");
    /* Not rediscovered: UNREACHABLE, "recovery sent", version unchanged. */
    cu_dark_port(a,want,1);cu_setup(&line,cu_boards,2,0);
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,2,1,a,2,cu_results,&o,at),"recover: admitted again");
    s=cu_run(&j,&at,1,0);
    CHECK(s.sessions==1 && cu_results[1].transfer_complete && cu_results[1].result==ULTRA_JOB_UNREACHABLE &&
          cu_results[1].reason==ULTRA_JOB_RECOVERY_SENT && cu_results[1].observed==1 && cu_results[1].recovery,
          "recover: not rediscovered -> UNREACHABLE, recovery sent");
    /* Rediscovered with other firmware: FAILED with the observed version. */
    cu_dark_port(a,want,1);cu_setup(&line,cu_boards,2,0);
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,2,1,a,2,cu_results,&o,at),"recover: admitted a third time");
    s=cu_run(&j,&at,1,0x00060001u);
    CHECK(cu_results[1].result==ULTRA_JOB_FAILED && cu_results[1].reason==ULTRA_JOB_MISMATCH &&
          cu_results[1].observed==0x00060001u,"recover: rediscovered with other firmware -> FAILED, observed kept");
    /* A SELECTED context (1 pass configured): still BLIND, 3 passes, no SELECT; the estimate counts the blind
     * passes. An older reachable sibling of the same target applies the broadcast too and is planned with it. */
    cu_dark_port(a,want,1);cu_nodes[2]=cu_nodes[1];cu_nodes[2].reachable=1;cu_nodes[2].participant.uid[11]=3;
    o.mode=ULTRA_UPDATE_SELECTED;o.passes=1;cu_setup(&line,cu_boards,3,0);
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,3,1,a,2,cu_results,&o,at),"recover: selected context admitted");
    CHECK(cu_results[1].planned && cu_results[1].recovery && cu_results[2].planned && !cu_results[2].recovery,
          "recover: the target and its older reachable sibling are planned");
    {ultra_update_job_progress_t pr,bl;ultra_update_job_t b;ultra_sender_t l2;
        CHECK(!ultra_update_job_progress(&j,at,&pr) && pr.subtransfers==1,"recover: one blind subtransfer in a selected context");
        o.mode=ULTRA_UPDATE_BLIND;o.passes=3;cu_setup(&l2,cu_boards,3,0);
        CHECK(!ultra_update_job_recover(&b,&l2,cu_nodes,3,1,a,2,cu_results+CU_MAX-3,&o,at) &&
              !ultra_update_job_progress(&b,at,&bl) && bl.estimated_remaining_ms==pr.estimated_remaining_ms,
              "recover: selected estimate counts the blind passes");
        ultra_update_job_abort(&b);o.mode=ULTRA_UPDATE_SELECTED;o.passes=1;}
    s=cu_run(&j,&at,1,want);
    CHECK(s.sessions==1 && s.blind==1 && !s.selects,"recover: selected context sends it blind");
    CHECK(s.progress_n>100 && !s.progress_bad,"recover: selected context progress reports 3 passes, not the configured 1 "
          "(%u frames, %u wrong)",s.progress_n,s.progress_bad);
    CHECK(cu_results[1].result==ULTRA_JOB_VERIFIED && cu_results[2].transfer_complete,"recover: selected context verified");
    CHECK(cu_results[2].result==ULTRA_JOB_UNREACHABLE && cu_results[2].reason==ULTRA_JOB_NO_REPLY,
          "recover: a sibling that stays silent after it is NO_REPLY, not recovery sent");
    o.mode=ULTRA_UPDATE_BLIND;o.passes=3;
    /* A dark board alone on its port. */
    cu_dark_port(a,0,1);cu_nodes[0]=cu_nodes[1];cu_setup(&line,cu_boards,1,0);
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,1,0,a,2,cu_results,&o,at) && cu_results[0].recovery,
          "recover: dark board alone admitted");
    s=cu_run(&j,&at,0,want);
    CHECK(s.sessions==1 && s.blind==1 && cu_results[0].result==ULTRA_JOB_VERIFIED,"recover: dark board alone recovered");
}
/* recover a dark older V203 (revoked, or older but not revoked) next to an equal and a newer
 * sibling of the same target, in both job modes. The broadcast reaches every sibling; the live ones refuse it before
 * any flash operation (the sender never sets FORCE), and the dark one applies it. */
static void test_chain_update_mixed_versions(void)
{
    ultra_sender_t line;ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_artifact_t a[2];
    cu_rx_t rx[4];uint32_t want;int64_t at=1000000;cu_run_t s;unsigned mode,variant;memset(&j,0,sizeof j);
    for(mode=0;mode<2;++mode)for(variant=0;variant<2;++variant){
        const char *m=mode?"selected":"blind",*v=variant?"older, not revoked":"revoked";
        cu_dark_port(a,0,1);want=ultra_get32(a[0].descriptor.bytes+16);
        cu_nodes[0].firmware=want;cu_nodes[2].firmware=want;cu_nodes[3].firmware=want+1;   /* V407 current; equal; newer */
        cu_rx_init(rx+0,cu_nodes+0,1);cu_rx_init(rx+1,cu_nodes+1,variant);cu_rx_init(rx+2,cu_nodes+2,1);
        cu_rx_init(rx+3,cu_nodes+3,1);
        o.mode=mode?ULTRA_UPDATE_SELECTED:ULTRA_UPDATE_BLIND;o.passes=mode?1:3;o.first_session=1;
        o.port_type=ULTRA_PORT_DIFF_POWER;o.verify_timeout_ms=50;cu_setup(&line,cu_boards,4,0);
        CHECK(ultra_update_job_begin(&j,&line,cu_nodes,4,a,2,cu_results,&o,at)==ULTRA_UPDATE_SILENT_BOARDS &&
              cu_results[1].reason==ULTRA_JOB_NOT_ANSWERING,"mixed %s/%s: update all refused, the dark node named",m,v);
        cu_rx_set=rx;cu_rx_n=4;
        CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,4,1,a,2,cu_results,&o,at),"mixed %s/%s: recover admitted",m,v);
        CHECK(cu_results[1].recovery && cu_results[1].planned && !cu_results[2].recovery && !cu_results[3].recovery &&
              cu_results[2].result==ULTRA_JOB_CURRENT && cu_results[3].result==ULTRA_JOB_SKIPPED &&
              cu_results[3].reason==ULTRA_JOB_NEWER,"mixed %s/%s: only the dark node is planned",m,v);
        s=cu_run(&j,&at,1,want);cu_rx_n=0;
        CHECK(s.sessions==1 && s.blind==1 && !s.selects && !s.force && s.data>=3*17u,
              "mixed %s/%s: one blind subtransfer, no FORCE (%u sessions, %u FORCE)",m,v,s.sessions,s.force);
        CHECK(rx[1].installs==1 && rx[1].revokes==1 && rx[1].desc_ok && rx[1].version==want && rx[1].programs==17u,
              "mixed %s/%s: the dark node (%s) revoked once and recovered (%u revokes, %u installs)",m,v,v,rx[1].revokes,rx[1].installs);
        CHECK(cu_results[1].result==ULTRA_JOB_VERIFIED && cu_results[1].observed==want,"mixed %s/%s: the dark node is VERIFIED",m,v);
        CHECK(rx[2].same>=1 && !rx[2].rejected && !rx[2].revokes && !rx[2].programs && !rx[2].installs && rx[2].desc_ok &&
              rx[2].version==want,"mixed %s/%s: the equal sibling skips it, no flash operation",m,v);
        CHECK(rx[3].rejected>=1 && !rx[3].same && !rx[3].revokes && !rx[3].programs && !rx[3].installs && rx[3].desc_ok &&
              rx[3].version==want+1,"mixed %s/%s: the newer sibling refuses it, no flash operation",m,v);
        CHECK(rx[0].refused>=1 && !rx[0].revokes && !rx[0].programs && rx[0].version==want,
              "mixed %s/%s: the other target's board ignores it",m,v);
        CHECK(cu_results[2].result==ULTRA_JOB_CURRENT && cu_results[3].result==ULTRA_JOB_SKIPPED &&
              cu_results[0].reason==ULTRA_JOB_OTHER_TARGET,"mixed %s/%s: the live boards are not reported updated",m,v);
    }
}
/* A blind transfer's END has no feedback and no repeat from the receiver's side. The sender sends
 * ULTRA_UPDATE_END_COPIES copies: a receiver that misses the first ones still takes END (a repeat is a no-op once taken),
 * and only a transfer that loses EVERY copy leaves the board unfinished (control: the model can fail). The commit
 * wait counts from the last copy, so the job never verifies a board that is still committing. */
static void test_chain_update_end_lost(void)
{
    ultra_sender_t line;ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_artifact_t a[2];
    cu_rx_t rx[1];uint32_t want;int64_t at;cu_run_t s;unsigned lost;
    memset(&j,0,sizeof j);
    for(lost=0;lost<=ULTRA_UPDATE_END_COPIES;++lost){
        at=1000000;cu_dark_port(a,0,1);want=ultra_get32(a[0].descriptor.bytes+16);cu_nodes[0]=cu_nodes[1];
        cu_rx_init(rx,cu_nodes+0,0);                        /* a dark board in its recovery bootloader */
        o.mode=ULTRA_UPDATE_BLIND;o.passes=3;o.first_session=1;o.port_type=ULTRA_PORT_DIFF_POWER;o.verify_timeout_ms=50;
        cu_setup(&line,cu_boards,1,0);
        CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,1,0,a,2,cu_results,&o,at),"END lost %u: recover admitted",lost);
        cu_rx_set=rx;cu_rx_n=1;cu_end_drop=lost;
        s=cu_run(&j,&at,0,want);cu_rx_n=0;cu_end_drop=0;
        CHECK(s.sessions==1 && s.blind==1 && s.ends==ULTRA_UPDATE_END_COPIES,
              "END lost %u: the sender sent %u END copies in one session",lost,s.ends);
        if(lost<ULTRA_UPDATE_END_COPIES){
            CHECK(rx[0].installs==1 && rx[0].desc_ok && rx[0].version==want && !rx[0].receiving,
                  "END lost %u of %u: the board still took END (%u installs)",lost,ULTRA_UPDATE_END_COPIES,rx[0].installs);
            CHECK(cu_results[0].result==ULTRA_JOB_VERIFIED && cu_results[0].observed==want,
                  "END lost %u: the node is VERIFIED",lost);
        }else{
            CHECK(!rx[0].installs && !rx[0].desc_ok && rx[0].receiving,
                  "END lost on every copy: the board is left unfinished (control)");
            CHECK(cu_results[0].result==ULTRA_JOB_FAILED,"END lost on every copy: the node is not VERIFIED");
        }
    }
}
/* recover() refusals (nothing sent, results untouched) and the port's limits, which count every node. */
static void cu_recover_refused(ultra_sender_t *line,uint16_t count,uint16_t node,const ultra_update_artifact_t *a,
    uint16_t ac,const ultra_update_job_options_t *o,int want,const char *what)
{
    ultra_update_job_t j;int rc;
    memset(cu_results,0xee,sizeof cu_results);memset(&j,0xee,sizeof j);
    rc=ultra_update_job_recover(&j,line,cu_nodes,count,node,a,ac,cu_results,o,1000000);
    CHECK(rc==want && !line->update_active && !line->has_sent && j.state==0xee && cu_results[node%CU_MAX].result==0xee,
          "recover refused: %s (%d, want %d), nothing sent, results untouched",what,rc,want);
}
static void test_chain_update_recover_refused(void)
{
    ultra_sender_t line;ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_artifact_t a[2];
    uint32_t want;int64_t at=1000000;unsigned i;
    cu_dark_port(a,0,1);want=ultra_get32(a[0].descriptor.bytes+16);
    o.passes=3;o.first_session=1;o.port_type=ULTRA_PORT_DIFF_POWER;o.verify_timeout_ms=50;
    /* A reachable node: it answers, so "update all" is the action. */
    cu_dark_port(a,want,1);cu_setup(&line,cu_boards,2,0);
    cu_recover_refused(&line,2,0,a,2,&o,ULTRA_UPDATE_NOT_RECOVERABLE,"reachable V407");
    cu_nodes[1].reachable=1;cu_recover_refused(&line,2,1,a,2,&o,ULTRA_UPDATE_NOT_RECOVERABLE,"reachable V203");
    /* A one-way node has no return path to be silent on. */
    cu_dark_port(a,want,1);cu_nodes[1].participant.has_return=0;
    cu_recover_refused(&line,2,1,a,2,&o,ULTRA_UPDATE_NOT_RECOVERABLE,"one-way node");
    /* An equal or newer known version: boards skip it without FORCE, so it is never sent. */
    for(i=0;i<2;++i){cu_dark_port(a,want,want+i);
        cu_recover_refused(&line,2,1,a,2,&o,ULTRA_UPDATE_NOT_RECOVERABLE,i?"newer known version":"equal known version");}
    /* No artifact for the node's target. */
    cu_dark_port(a,want,1);cu_recover_refused(&line,2,1,a,1,&o,ULTRA_UPDATE_NOT_RECOVERABLE,"no artifact");
    /* An untrusted node (the target or another) refuses as job_begin does. */
    cu_dark_port(a,want,1);cu_nodes[1].trusted=0;cu_recover_refused(&line,2,1,a,2,&o,-UPD_ERROR_BOUNDS,"untrusted target");
    cu_dark_port(a,want,1);cu_nodes[0].trusted=0;cu_recover_refused(&line,2,1,a,2,&o,-UPD_ERROR_BOUNDS,"untrusted other node");
    cu_dark_port(a,want,1);cu_recover_refused(&line,2,2,a,2,&o,-1,"node index out of range");
    /* Blind needs its minimum passes. */
    o.passes=ULTRA_UPDATE_BLIND_MIN_PASSES-1u;cu_recover_refused(&line,2,1,a,2,&o,ULTRA_UPDATE_TOO_FEW_PASSES,"2 blind passes");
    o.passes=3;
    /* Port limits per port type count every node, and the D2A speed table applies to the whole pin. */
    cu_dark_port(a,0,1);cu_setup(&line,cu_boards,ULTRA_PORT_DIFF_POWER_MAX_BOARDS+1,0);
    cu_recover_refused(&line,ULTRA_PORT_DIFF_POWER_MAX_BOARDS+1,1,a,2,&o,ULTRA_UPDATE_TOO_MANY_BOARDS,"9 Diff-Power nodes");
    cu_setup(&line,cu_boards,ULTRA_PORT_DIFF_POWER_MAX_BOARDS,0);
    cu_recover_refused(&line,ULTRA_PORT_DIFF_POWER_MAX_BOARDS+1,1,a,2,&o,ULTRA_UPDATE_TOO_MANY_BOARDS,"8-board line, 9 nodes");
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,ULTRA_PORT_DIFF_POWER_MAX_BOARDS,1,a,2,cu_results,&o,at),
          "recover: 8 Diff-Power nodes admitted");
    ultra_update_job_abort(&j);
    o.port_type=ULTRA_PORT_D2A_PIN;cu_setup(&line,cu_boards,CU_MAX,3);
    cu_recover_refused(&line,CU_MAX,1,a,2,&o,ULTRA_UPDATE_TOO_MANY_BOARDS,"51 D2A nodes");
    cu_setup(&line,cu_boards,ULTRA_D2A_800_MAX_BOARDS+1,ULTRA_PROFILE_800);
    cu_recover_refused(&line,ULTRA_D2A_800_MAX_BOARDS+1,1,a,2,&o,ULTRA_UPDATE_LINE_TOO_FAST,"21 D2A boards at 800 kHz");
    cu_setup(&line,cu_boards,ULTRA_PORT_D2A_PIN_MAX_BOARDS,ULTRA_PROFILE_200);
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,ULTRA_PORT_D2A_PIN_MAX_BOARDS,1,a,2,cu_results,&o,at),
          "recover: 50 D2A nodes at 200 kHz admitted");
    ultra_update_job_abort(&j);
    o.port_type=ULTRA_PORT_DIFF_POWER;
    /* Another silent node on the port doesn't refuse it: each silent board is recovered on its own. Node 3 is a
     * silent older sibling of the same target: the broadcast reaches it too, so it is planned (not the target). */
    cu_dark_port(a,want,1);cu_nodes[3].reachable=0;cu_setup(&line,cu_boards,4,0);
    CHECK(!ultra_update_job_recover(&j,&line,cu_nodes,4,1,a,2,cu_results,&o,at) && cu_results[1].recovery &&
          cu_results[3].planned && !cu_results[3].recovery,"recover: a second silent node doesn't refuse it");
    {cu_run_t s=cu_run(&j,&at,1,want);
        CHECK(s.sessions==1 && cu_results[1].result==ULTRA_JOB_VERIFIED && cu_results[3].result==ULTRA_JOB_UNREACHABLE &&
              cu_results[3].reason==ULTRA_JOB_NO_REPLY,"recover: the second silent node is not reported recovered");}
}
static void cu_put64(uint8_t *b,int64_t value)
{ultra_put32(b,(uint32_t)((uint64_t)value>>32));ultra_put32(b+4,(uint32_t)value);}
static void test_chain_update_traces(const char *dir)
{
    ultra_sender_t line;ultra_sender_board_t board;ultra_update_t u;upd_descriptor_t d;
    upd_target_t t;ultra_update_layout_t l;uint8_t uid[12]={1},f[48],record[52];
    unsigned profile,marker,family,k;int rc;int64_t at;char path[1024];FILE *out;
    if(!dir)return;
    for(family=0;family<2;++family)for(profile=0;profile<4;++profile)for(marker=0;marker<2;++marker){
        cu_image(&d,&t,&l,(int)family,516);cu_setup(&line,&board,1,(uint8_t)profile);line.digital=(uint8_t)marker;
        at=(int64_t)ULTRA_TIME_WRAP-67296;   /* (across TIME's wrap: check_update_traces.py) */
        CHECK(!ultra_update_begin(&u,&line,&d,&t,&l,uid,ULTRA_PORT_DIFF_POWER,7,3,2,at,cu_read,NULL),"trace begin");
        (void)snprintf(path,sizeof path,"%s/update-%u-%u-%u.bin",dir,family,profile,marker);
        out=fopen(path,"wb");CHECK(out!=NULL,"open trace");if(!out)return;
        for(k=0;k<800 && u.state!=ULTRA_UPDATE_CONFIRM;++k){at=u.due_us;rc=ultra_update_next(&u,at,f);CHECK(rc>=0,"trace next");
            if(rc<=0)continue;
            cu_put64(record,at);ultra_put32(record+8,(uint32_t)rc);cu_put64(record+12,u.transport.frame_start_us);
            cu_put64(record+20,u.transport.frame_end_us);ultra_put32(record+28,u.transport.lead_low_us);
            ultra_put32(record+32,u.transport.marker_high_us);ultra_put32(record+36,u.transport.marker_low_us);
            ultra_put32(record+40,u.transport.tail_low_us);ultra_put32(record+44,u.transport.release_us);
            ultra_put32(record+48,u.transport.listen_us);
            CHECK(fwrite(record,1,sizeof record,out)==sizeof record && fwrite(f,1,(size_t)rc,out)==(size_t)rc,"write trace");
        }
        CHECK(!fclose(out) && u.transfer_complete,"complete trace");
    }
}
