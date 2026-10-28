/* SPDX-License-Identifier: MIT
 * A receiving application's one-frame capture, driven by the sender's real frame times.
 *
 * The board's app captures one frame at a time: a frame whose first edge comes before the app has handled the frame
 * before it (that frame's end plus a processing time T) is dropped whole. The frames that get through go into the
 * receiver firmware's own decision code (core/upd_app.c, built from --firmware as test_sim builds the receive path):
 * transparency at the frame ends, the ANNOUNCE halves, evaluate() and its TAKE.
 *
 *   test_mailbox [body_bytes]      (default 49636: a 0.7.3 Dig2Analog-Plus-5ch body)
 *
 * Built twice by run_tests.py: with the library's update source (every decision frame >= ULTRA_UPDATE_CONTROL_PACE_US +
 * the port gap after the frame before it: the app takes the image at the entry ANNOUNCE for every T in 0.6 / 2 / 5 ms),
 * and as the control (MAILBOX_UNPACED=1) with a build-folder copy of that source whose pace steps add 0u: the schedule
 * before the pace, whose
 * ANNOUNCE half 0 starts one port gap after a captured PING and is dropped. That build must NOT take the image on an
 * unmarked line (the control: the model can fail, as the bench did). Both builds: an image that needs a newer
 * bootloader than the board's is refused at the ANNOUNCE (NACK), never taken. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef MAILBOX_UNPACED
#define MAILBOX_UNPACED 0
#endif
#include "ultra_sender_update.h"
#include "ultra_sender_update.c"   /* the control (MAILBOX_UNPACED): run_tests.py's copy, first on the include path */
#include "upd_app.h"

static unsigned failures, checks;
#define CHECK(c,...) do{++checks;if(!(c)){++failures;printf("FAIL %s:%d: ",__FILE__,__LINE__);printf(__VA_ARGS__);printf("\n");}}while(0)

static int mb_read(void *ctx,uint32_t off,uint8_t *out,uint32_t n)
{uint32_t i;(void)ctx;for(i=0;i<n;++i)out[i]=(uint8_t)((off+i)*37u);return 0;}
/* a V203 image (layout 1) for target t, min_bl = the bootloader it needs */
static void mb_image(upd_descriptor_t *d,upd_target_t *t,ultra_update_layout_t *l,uint32_t length,uint8_t min_bl)
{
    uint32_t off,crc=0xffffffffu;uint8_t b[32];
    memset(d,0,sizeof *d);memset(t,0,sizeof *t);memset(l,0,sizeof *l);
    t->bytes[0]=1;t->bytes[1]=1;t->bytes[2]=1;t->bytes[3]=1;t->bytes[4]=3;t->bytes[5]=2;t->bytes[6]=2;t->bytes[7]=4;
    ultra_put32(t->bytes+8,0x08002100);ultra_put32(t->bytes+12,57088);
    memcpy(d->bytes,"QDU6",4);d->bytes[4]=2;d->bytes[6]=4;d->bytes[7]=4;
    memcpy(d->bytes+8,t->bytes,7);d->bytes[15]=min_bl;ultra_put32(d->bytes+16,0x00070302);
    for(off=0;off<length;off+=32){uint32_t n=length-off;if(n>32)n=32;mb_read(NULL,off,b,n);crc=upd_crc32_words(crc,b,n/4);}
    ultra_put32(d->bytes+20,length);ultra_put32(d->bytes+24,crc);
    memcpy(d->bytes+28,t->bytes+8,4);memcpy(d->bytes+32,t->bytes+8,4);
    d->bytes[36]=2;d->bytes[37]=3;d->bytes[38]=1;
    ultra_put32(d->bytes+60,upd_crc32_words(0xffffffffu,d->bytes,15));
    l->metadata_base=0x08002000;l->metadata_bytes=256;l->bootloader_bytes=8192;l->boot_version=4;
    l->geometry[0]=0x88;l->geometry[1]=10;l->geometry[2]=20;l->entry_10ms=10;
}

/* the board: a one-frame capture in front of the app's update code */
typedef struct {
    int64_t free_us;uint32_t T,ms;upd_app_t app;upd_bl_info_t bl;
    unsigned captured,dropped,announces_dropped,take,rejected,same,pair;
} mb_board_t;
static void mb_board(mb_board_t *b,const upd_target_t *t,uint8_t bl_version,uint32_t T)
{
    upd_app_info_t info;
    memset(b,0,sizeof *b);memcpy(b->bl.magic,"QDB2",4);b->bl.bl_version=bl_version;b->bl.target=*t;
    b->bl.geometry[0]=0x88;b->bl.geometry[1]=10;b->bl.geometry[2]=20;b->bl.entry_10ms=10;
    memset(&info,0,sizeof info);info.hw_id=1;info.hw_rev=4;info.bl_version=bl_version;info.desc_ok=1;info.confirmed=1;
    info.st_next=1;info.boot=&b->bl;info.version=0x000702ffu;   /* it runs 0.7.2 */
    upd_app_init(&b->app,&info);b->app.startup_ok=1;b->T=T;b->free_us=0;
}
static void mb_action(mb_board_t *b,uint32_t a)
{
    if(a==UPD_APP_TAKE && !b->take)b->take=b->pair;
    if(a==UPD_APP_SHOW_REJECTED)++b->rejected;
    if(a==UPD_APP_SHOW_SAME)++b->same;
}
/* one frame on the wire, first edge at `edge`, last at `end` (us) */
static void mb_frame(mb_board_t *b,const uint8_t *f,uint32_t n,int64_t edge,int64_t end)
{
    uint32_t a=UPD_APP_NONE,now;
    if(b->take)return;                                /* it reset into its bootloader */
    while((int64_t)(b->ms+1u)*1000<=edge){++b->ms;mb_action(b,upd_app_poll(&b->app,b->ms));}
    if(edge<b->free_us){++b->dropped;b->announces_dropped+=n==48u && f[0]==UPD_ANNOUNCE;return;}
    b->free_us=end+b->T;++b->captured;now=(uint32_t)(end/1000);
    if(now>b->ms)b->ms=now;                           /* the app's clock: it handles a frame after the frame's end */
    if(n==48u && f[0]==UPD_ANNOUNCE && ultra_get16(f+2)==0u)++b->pair;
    if(n==48u && upd_app_transparent(&b->app))a=upd_app_frame(&b->app,f,now);
    upd_app_frame_end(&b->app,1,(uint8_t)(f[n-6u]&0x0Fu),now);
    mb_action(b,a);
}
static void mb_setup(ultra_sender_t *line,ultra_sender_board_t *boards,uint8_t profile)
{
    static ultra_sender_item_t item;ultra_sender_config_t c;
    ultra_sender_config_init(&c,1,0);c.light_capacity=0;c.clear_unused=0;
    CHECK(!ultra_timing_profile(&c.timing,profile,9600000,500),"profile %u",profile);
    CHECK(!ultra_sender_init(line,&c,boards,&item,1),"line init");
}

#define MB_T 3
static const uint32_t mb_T[MB_T]={600,2000,5000};
typedef struct {uint8_t port,marker;const char *name;} mb_port_t;
static const mb_port_t mb_ports[]={{ULTRA_PORT_D2A_PIN,0,"Dig2Analog-Plus-5ch pin"},{ULTRA_PORT_DIFF_POWER,0,"Diff-Power"},
                                   {ULTRA_PORT_DIFF_POWER,1,"marker port"}};
static const uint8_t mb_profiles[]={0,1,3};

/* one blind job into one board per T; returns the frames sent */
static unsigned mb_job(const mb_port_t *p,uint8_t profile,uint32_t length,uint8_t min_bl,uint8_t board_bl,mb_board_t b[MB_T])
{
    ultra_sender_t line;ultra_sender_board_t board;ultra_update_t u;upd_descriptor_t d;upd_target_t t;
    ultra_update_layout_t l;uint8_t f[64];unsigned k,i,frames=0;int rc;int64_t at=5000000,edge;
    mb_image(&d,&t,&l,length,min_bl);mb_setup(&line,&board,profile);line.digital=p->marker;
    for(i=0;i<MB_T;++i)mb_board(b+i,&t,board_bl,mb_T[i]);
    rc=ultra_update_begin(&u,&line,&d,&t,&l,NULL,p->port,7,3,2,at,mb_read,NULL);
    CHECK(!rc,"%s profile %u: begin (%d)",p->name,profile,rc);if(rc)return 0;
    for(k=0;k<400000u && u.state!=ULTRA_UPDATE_CONFIRM;++k){at=u.due_us;
        rc=ultra_update_next_frame(&u,at,f,sizeof f);CHECK(rc>=0,"next (%d)",rc);if(rc<0)break;
        if(!rc)continue;
        ++frames;edge=u.transport.frame_start_us-(int64_t)(u.transport.marker_high_us+u.transport.marker_low_us);
        for(i=0;i<MB_T;++i)mb_frame(b+i,f,(uint32_t)rc,edge,u.transport.frame_end_us);
    }
    CHECK(u.transfer_complete,"%s profile %u: the job completes",p->name,profile);
    return frames;
}

int main(int argc,char **argv)
{
    uint32_t length=argc>1?(uint32_t)strtoul(argv[1],NULL,0):49636u;unsigned pi,ri,i,frames,takes=0,controls=0;
    mb_board_t b[MB_T],r[MB_T];int paced=!MAILBOX_UNPACED;
    printf("%s, body %u B\n",paced?"paced sender (the library's source)":"UNPACED control (the pace steps add 0)",
           (unsigned)length);
    if(paced){
        /* the guard's control: an image that needs only bootloader 3 IS taken by a bootloader-3 board (a board whose
         * bootloader cannot finish an update must never be offered one: the guard is the image's min_bl alone) */
        mb_job(mb_ports,0,length,3,3,r);
        for(i=0;i<MB_T;++i)CHECK(r[i].take==1u,"T %u: a bootloader-3 board takes a min-bl-3 image (the guard's control)",
                                 (unsigned)r[i].T);
        printf("guard control: a min-bl-3 image is taken by %u of %u bootloader-3 boards\n",
               (unsigned)((r[0].take==1u)+(r[1].take==1u)+(r[2].take==1u)),(unsigned)MB_T);
    }
    for(pi=0;pi<sizeof mb_ports/sizeof mb_ports[0];++pi)for(ri=0;ri<sizeof mb_profiles;++ri){
        const mb_port_t *p=mb_ports+pi;uint8_t profile=mb_profiles[ri];
        if(p->port==ULTRA_PORT_D2A_PIN && profile==1u)continue;   /* (one board: 800 kHz; 200 kHz recovery) */
        frames=mb_job(p,profile,length,4,4,b);                    /* bootloader 4 board, min_bl 4 image */
        mb_job(p,profile,length,4,3,r);                           /* bootloader 3 board, the same image */
        for(i=0;i<MB_T;++i){
            printf("%-24s profile %u T %4u us: %u frames, %u captured, %u dropped (%u ANNOUNCE), %s; bootloader-3 board: %s\n",
                   p->name,profile,(unsigned)b[i].T,frames,b[i].captured,b[i].dropped,b[i].announces_dropped,
                   b[i].take?(b[i].take==1u?"TAKE at the entry ANNOUNCE":"TAKE at the boot ANNOUNCE"):"no TAKE",
                   r[i].take?"TAKE":r[i].rejected?"refused (NACK)":"never decided");
            /* every board, both builds: an image that needs bootloader 4 is never taken by a bootloader-3 board */
            CHECK(!r[i].take && r[i].app.takes==0u,"%s profile %u T %u: a bootloader-3 board took a min-bl-4 image",
                  p->name,profile,(unsigned)r[i].T);
            if(paced){
                CHECK(b[i].take==1u && b[i].app.takes==1u && b[i].app.sessions_seen==1u,
                      "%s profile %u T %u: the paced sender's entry ANNOUNCE is taken",p->name,profile,(unsigned)b[i].T);
                CHECK(b[i].announces_dropped==0u,"%s profile %u T %u: no ANNOUNCE half dropped",p->name,profile,
                      (unsigned)b[i].T);
                CHECK(r[i].rejected>=1u && r[i].app.sessions_seen>=1u,
                      "%s profile %u T %u: the bootloader-3 board decided (NACK)",p->name,profile,(unsigned)r[i].T);
                takes+=b[i].take==1u;
            }else if(!p->marker){
                /* the control (the bench, 2026-10-30): half 0 starts one port gap after a captured PING */
                CHECK(!b[i].take && b[i].app.sessions_seen==0u,"%s profile %u T %u: the unpaced sender must not be taken",
                      p->name,profile,(unsigned)b[i].T);
                controls+=!b[i].take;
            }
        }
    }
    if(paced)printf("paced: %u of %u boards took the image at the entry ANNOUNCE; no bootloader-3 board took it\n",takes,
                    (unsigned)(8u*MB_T));
    else printf("control: %u of %u unmarked boards never took the unpaced sender's image\n",controls,(unsigned)(5u*MB_T));
    printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
