/* SPDX-License-Identifier: MIT
 * The recorded design limits as compiled, and where the sender refuses: one
 * line per fact for test-vectors/check_rules.py, which holds the recorded
 * numbers. This program asserts nothing itself; the facts go to argv[1]. Built
 * with ULTRA_SENDER_MAX_BOARDS >= 52 so one line can exceed every port type. */
#include <stdio.h>
#include <string.h>
#include "ultra_sender_update.h"
#include "ultra_sender_job.h"
#include "ultra_transport.h"

#define PROBE_BOARDS 52u
static ultra_sender_board_t boards[PROBE_BOARDS];
static ultra_update_node_t nodes[PROBE_BOARDS];
static ultra_update_result_t results[PROBE_BOARDS];
static ultra_update_job_t port_job;
static ultra_sender_t line;
static FILE *out;

#define CONST(name) fprintf(out,"const %s %lu\n", #name, (unsigned long)(name))
static void constants(void)
{
    CONST(ULTRA_PORT_DIFF_POWER_MAX_BOARDS); CONST(ULTRA_PORT_D2A_PIN_MAX_BOARDS);
    CONST(ULTRA_UPDATE_MAX_BOARDS);
    CONST(ULTRA_D2A_800_MAX_BOARDS); CONST(ULTRA_D2A_600_MAX_BOARDS);
    CONST(ULTRA_CHAIN_GUARANTEE_HOP_NS); CONST(ULTRA_CHAIN_FORWARD_NS); CONST(ULTRA_CHAIN_HOP_DISTORTION_NS);
    CONST(ULTRA_CHAIN_CABLE_DISTORTION_NS); CONST(ULTRA_CHAIN_CABLE_DELAY_NS); CONST(ULTRA_CHAIN_USED_NS);
    CONST(ULTRA_HOP_DELAY_NS); CONST(ULTRA_LINE_TX_TOL_NS); CONST(ULTRA_FRAME_GAP_US);
    CONST(ULTRA_D2A_HOP_NS); CONST(ULTRA_D2A_GATE_REOPEN_NS); CONST(ULTRA_D2A_LOW_RESIDUAL_NS);
    CONST(ULTRA_RATE_ERROR_PPM); CONST(ULTRA_RATE_LOCK_LOSS_MS); CONST(ULTRA_RW_LOCK_MS);
    CONST(ULTRA_RW_PITCH_SCALED_US); CONST(ULTRA_RW_PITCH_FIXED_US);
    CONST(ULTRA_RW_RELEASE_MIN_US); CONST(ULTRA_RW_RELEASE_US); CONST(ULTRA_RW_REARM_US); CONST(ULTRA_DP_RELEASE_MAX_US);
    CONST(ULTRA_DP_TRIES); CONST(ULTRA_DP_RETRY_S);
    CONST(ULTRA_UPDATE_BLIND_MIN_PASSES); CONST(ULTRA_UPDATE_QUIET_MS); CONST(ULTRA_UPDATE_ACK_COPIES);
    CONST(ULTRA_UPDATE_END_COPIES); CONST(ULTRA_UPDATE_CONFIRM_COPIES); CONST(ULTRA_UPDATE_CONTROL_PACE_US);
    CONST(UPD_TRAIN_MIN_MS); CONST(UPD_IDLE_CLOSE_MS); CONST(UPD_FORWARD_QUIET_US);
    CONST(UPD_TRANSIT_ERASE_MS); CONST(UPD_DECLARED_MARGIN_MS); CONST(UPD_TRANSIT_HOLD_MAX_MS);
    CONST(UPD_OPERATION_DEADLINE_FACTOR); CONST(UPD_DISPATCH_MS); CONST(UPD_IDLE_TIMEOUT_MS);
    CONST(UPD_ENTRY_MIN_MS); CONST(ULTRA_SENDER_MAX_BOARDS);
}
static int setup(uint16_t count, uint8_t profile)
{
    static ultra_sender_item_t item;ultra_sender_config_t c;
    ultra_sender_config_init(&c,count,0);c.light_capacity=0;c.clear_unused=0;
    if(ultra_timing_profile(&c.timing,profile,9600000,500))return -1;
    return ultra_sender_init(&line,&c,boards,&item,1);
}
/* job_begin for `count` inventoried nodes on a line of `count` boards at the
 * port's table profile (so only the board limit can refuse first). */
static int job(uint8_t type, uint16_t count)
{
    ultra_update_job_options_t o;uint8_t p=ULTRA_PROFILE_800;
    memset(&o,0,sizeof o);o.passes=3;o.first_session=1;o.port_type=type;
    ultra_port_line_profile(type,count,&p);if(p==255u)p=ULTRA_PROFILE_200;
    if(setup(count,p))return 1000;
    memset(nodes,0,sizeof nodes);
    return ultra_update_job_begin(&port_job,&line,nodes,count,NULL,0,results,&o,1000000);
}
/* job_begin for one inventoried node on a line of `count` boards */
static int job_line(uint8_t type, uint16_t count)
{
    ultra_update_job_options_t o;
    memset(&o,0,sizeof o);o.passes=3;o.first_session=1;o.port_type=type;
    if(setup(count,ULTRA_PROFILE_200))return 1000;
    memset(nodes,0,sizeof nodes);
    return ultra_update_job_begin(&port_job,&line,nodes,1,NULL,0,results,&o,1000000);
}
/* job_begin for `count` inventoried nodes on a line of one board less: only
 * the inventory count can refuse as too many */
static int job_nodes(uint8_t type, uint16_t count)
{
    ultra_update_job_options_t o;
    memset(&o,0,sizeof o);o.passes=3;o.first_session=1;o.port_type=type;
    if(setup(count>1u?(uint16_t)(count-1u):1u,ULTRA_PROFILE_200))return 1000;
    memset(nodes,0,sizeof nodes);
    return ultra_update_job_begin(&port_job,&line,nodes,count,NULL,0,results,&o,1000000);
}
/* job_recover of node 0 for `count` inventoried nodes on a line of `count`
 * boards at the port's table profile: the whole port sets the board limit */
static int recover(uint8_t type, uint16_t count)
{
    ultra_update_job_options_t o;uint8_t p=ULTRA_PROFILE_800;
    memset(&o,0,sizeof o);o.passes=3;o.first_session=1;o.port_type=type;
    ultra_port_line_profile(type,count,&p);if(p==255u)p=ULTRA_PROFILE_200;
    if(setup(count,p))return 1000;
    memset(nodes,0,sizeof nodes);
    return ultra_update_job_recover(&port_job,&line,nodes,count,0,NULL,0,results,&o,1000000);
}
int main(int argc, char **argv)
{
    static const uint8_t types[4]={ULTRA_PORT_DIFF_POWER,ULTRA_PORT_D2A_PIN,0,3};
    unsigned t,n,p;
    if(argc<2 || !(out=fopen(argv[1],"w"))){printf("usage: rules_probe OUT\n");return 2;}
    constants();
    fprintf(out,"type diff_power %u\ntype d2a_pin %u\n",(unsigned)ULTRA_PORT_DIFF_POWER,(unsigned)ULTRA_PORT_D2A_PIN);
    fprintf(out,"rc too_many %d\nrc too_fast %d\n",ULTRA_UPDATE_TOO_MANY_BOARDS,ULTRA_UPDATE_LINE_TOO_FAST);
    fprintf(out,"why default %d\nwhy board_count %d\nwhy too_many %d\nwhy no_boards %d\nwhy unknown_type %d\n",
           ULTRA_PORT_PROFILE_DEFAULT,ULTRA_PORT_PROFILE_BOARD_COUNT,ULTRA_PORT_PROFILE_TOO_MANY,
           ULTRA_PORT_PROFILE_NO_BOARDS,ULTRA_PORT_PROFILE_UNKNOWN_TYPE);
    for(t=0;t<4;++t){
        fprintf(out,"max %u %u\n",types[t],(unsigned)ultra_port_max_boards(types[t]));
        for(n=0;n<=PROBE_BOARDS;++n){uint8_t got=0;
            unsigned why=ultra_port_line_profile(types[t],(uint16_t)n,&got);
            fprintf(out,"profile %u %u %u %u\n",types[t],n,why,(unsigned)got);}
    }
    for(t=0;t<2;++t)for(n=1;n<=PROBE_BOARDS;++n){
        fprintf(out,"job %u %u %d\n",types[t],n,job(types[t],(uint16_t)n));
        fprintf(out,"job_line %u %u %d\n",types[t],n,job_line(types[t],(uint16_t)n));
        fprintf(out,"job_nodes %u %u %d\n",types[t],n,job_nodes(types[t],(uint16_t)n));
        fprintf(out,"recover %u %u %d\n",types[t],n,recover(types[t],(uint16_t)n));
    }
    for(n=0;n<=PROBE_BOARDS;++n){
        fprintf(out,"path diff_power %u %u\n",n,(unsigned)ultra_line_path_ns((uint8_t)n,0));
        fprintf(out,"path d2a_pin %u %u\n",n,(unsigned)ultra_d2a_path_ns((uint8_t)n,0));
        for(p=0;p<ULTRA_PROFILE_COUNT;++p)fprintf(out,"admits %u %u %d\n",p,n,ultra_d2a_admits((uint8_t)p,(uint8_t)n,0));
    }
    for(p=0;p<ULTRA_PROFILE_COUNT;++p){const ultra_line_profile_t *x=ultra_line_profile((uint8_t)p);
        fprintf(out,"line_profile %u %u %u %u %u %u %u\n",p,x->bit_samples,x->zero_samples,x->one_samples,
               x->threshold_36mhz,x->threshold_50mhz,x->admission_ns);}
    for(n=0;n<3;++n)fprintf(out,"pitch %u %lu\n",n,(unsigned long)ultra_rw_slot_us((uint8_t)n));
    fprintf(out,"end\n");
    if(fclose(out)){printf("rules_probe: writing %s failed\n",argv[1]);return 2;}
    printf("rules_probe: facts in %s\n",argv[1]);
    return 0;
}
