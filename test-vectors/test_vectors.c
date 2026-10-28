/* SPDX-License-Identifier: MIT
 * Wire-3 specification harness: C codecs versus independently generated JSON.
 * Run through sender/test/run_tests.py --only vectors --skip-cross.
 * No sender, hub, firmware or bootloader implementation is linked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ultra_protocol.h"
#include "ultra_transport.h"
#include "local_vectors.h"
/* Compile update helpers with a future app version to detect accidental coupling.
 * Normal wire helpers above were compiled with version 3 for the golden fixtures. */
#undef ULTRA_VERSION
#define ULTRA_VERSION 4u
#include "ultra_update.h"
#undef ULTRA_VERSION
#define ULTRA_VERSION 3u

static char *json;
static unsigned checks, failures;
static void check(int ok,const char *name) { ++checks;if(!ok){++failures;printf("FAIL %s\n",name);} }
static int hexval(char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; }
static int vector(const char *name,uint8_t *out,unsigned cap)
{
    char key[128];const char *p;unsigned n=0;
    (void)snprintf(key,sizeof key,"\"name\": \"%s\"",name);
    p=strstr(json,key);if(!p)return -1;p=strstr(p,"\"hex\": \"");if(!p)return -1;p+=8;
    while(hexval(p[0])>=0&&hexval(p[1])>=0){if(n>=cap)return -1;out[n++]=(uint8_t)(hexval(p[0])*16+hexval(p[1]));p+=2;}
    return (int)n;
}
static void compare(const char *name,const uint8_t *data,unsigned n)
{
    uint8_t expected[512];int len=vector(name,expected,sizeof expected);
    check(len==(int)n&&memcmp(expected,data,n)==0,name);
}
static void image_seal(uint8_t *d) { ultra_put32(d+60,upd_crc32_words(0xFFFFFFFFu,d,15)); }
static void make_target(upd_target_t *t)
{
    memset(t,0,sizeof *t);t->bytes[0]=254;t->bytes[1]=4;t->bytes[2]=5;t->bytes[3]=4;t->bytes[4]=3;
    t->bytes[5]=3;t->bytes[6]=201;ultra_put32(t->bytes+8,0x08005000);ultra_put32(t->bytes+12,503808);
}
int main(int argc,char **argv)
{
    FILE *fp;long length;uint8_t s[48],data[32],d[64],bad[64],a[32];
    const uint8_t uid[12]={0,1,2,3,4,5,6,7,8,9,10,11};
    uint16_t values[5]={0x1234,0x1234,0x1234,0x1234,0x1234},full[5]={65535,65535,65535,65535,65535},zero[5]={0};
    uint8_t roles[5]={1,2,3,4,5},id;uint16_t value;uint32_t time,i;
    upd_target_t target;ultra_pixel_fx_t fx,decoded;
    if(argc!=2 || !(fp=fopen(argv[1],"rb")))return 2;
    (void)fseek(fp,0,SEEK_END);length=ftell(fp);rewind(fp);
    if(length<0){fclose(fp);return 2;}json=(char*)malloc((size_t)length+1u);if(!json){fclose(fp);return 2;}
    if(fread(json,1,(size_t)length,fp)!=(size_t)length){fclose(fp);free(json);return 2;}
    json[length]=0;fclose(fp);
    ultra_build_header(s,0xabcdef);compare("header_normal",s,8);
    check(ultra_parse_header(s,&time)==0&&time==0xabcdef,"header decode");
    s[2]=0x20;s[7]=ultra_crc8(s,7);check(ultra_parse_header(s,&time)!=0,"wrong wire version");
    ultra_build_header_flags(s,0xabcdef,ULTRA_HDR_RW);compare("header_return",s,8);
    ultra_slot_set(s,19,0x42,ULTRA_SET_FLAGS,0x8000,values,full,1000,-2);compare("slot_set_target19",s,32);
    check(s[0]==0x62&&s[1]==19&&ultra_get16(s+3)==0x8000&&ultra_get24(s+25)==1000,"SET decode full TARGET/flags");
    ultra_slot_set(s,239,0x43,ULTRA_SET_SMOOTH,0,zero,zero,0,0);compare("slot_set_target239",s,32);
    id=12;value=2500;ultra_slot_config(s,19,0x44,&id,&value,1);compare("slot_config_channel19",s,32);
    check(s[1]==19&&s[3]==12&&ultra_get16(s+4)==2500,"channel CONFIG decode");
    id=33;value=1500;ultra_slot_config(s,255,0x45,&id,&value,1);compare("slot_config_domain1",s,32);
    id=77;value=3;ultra_slot_config(s,1,0x46,&id,&value,1);compare("slot_config_output1_source2",s,32);
    id=21;value=2000;ultra_slot_config(s,255,0x47,&id,&value,1);compare("slot_config_input",s,32);
    ultra_slot_light_def(s,19,0x48,roles,1000,0,zero,full,15);compare("slot_light_def_base15",s,32);
    check(s[29]==15&&s[3]==0x12,"LIGHT_DEF base/roles decode");
    memset(&fx,0,sizeof fx);fx.effect=1;fx.start_ms=-2;fx.speed=128;fx.intensity=64;fx.palette=255;
    fx.c[0][0]=255;fx.c[1][1]=255;fx.c[2][2]=255;fx.brightness=255;fx.transition_ms=250;
    fx.seg_len=1200;fx.flags=0x10;fx.custom1=1;fx.custom2=2;
    ultra_slot_pixel_fx(s,1,15,0x49,&fx);compare("slot_pixel_fx_output1_all",s,32);
    check(ultra_pixel_fx_parse(s,&decoded)==0x1f&&decoded.start_ms==-2&&decoded.seg_len==1200,"PIXEL_FX decode");
    s[1]=0xff;check(ultra_pixel_fx_parse(s,&decoded)==-1,"reserved pixel output");
    {
        static const struct {const char *name;uint8_t page,part,bank,kind;} p[]={
            {"outputs_bank0",1,0,0,0},{"outputs_bank3",1,0,3,0},{"acks_bank3",2,0,3,0},
            {"cal_pwm_bank3",6,0,3,0},{"cal_pixel_output1",6,1,1,1},{"descriptor_part10",15,10,0,0},
            {"identity_target",4,2,0,0},{"pixel_output1",16,1,1,0},{"pixel_fx_output1",17,0,1,0},{"limits_domain1",18,0,0,1}};
        for(i=0;i<sizeof p/sizeof p[0];++i){char name[80];uint8_t args[4]={p[i].part,p[i].kind,0,p[i].bank};
            ultra_slot_poll(s,0x42,0,1,p[i].page,0,args,4);(void)snprintf(name,sizeof name,"poll_%s",p[i].name);compare(name,s,32);
            check(ultra_poll_valid(s)==0&&s[ULTRA_POLL_BANK]==p[i].bank,"POLL decode bank");}
        s[1]=15;check(ultra_poll_valid(s)!=0,"POLL rejects nibble board target");
    }
    memset(a,0,sizeof a);a[0]=ULTRA_RSP_SYNC;a[1]=ULTRA_PAGE_OUTPUTS;a[3]=0x42;ultra_put16(a+4,ultra_crc16(uid,sizeof uid));
    for(i=0;i<5;++i){ultra_put16(a+6+2*i,i+1);ultra_put16(a+16+2*i,100*(i+1));}
    a[26]=0;a[27]=31;a[29]=1;ultra_put16(a+30,ultra_crc16(a,30));
    compare("answer_outputs_bank0",a,32);compare("answer_outputs_bank3",a,32);
    check(ultra_answer_check(a)==0&&ultra_get16(a+ULTRA_RSP_DATA+ULTRA_OU_CURRENT+8)==500,"OUTPUTS decode");
    {
        static const uint8_t records[]={0,12,11,184,1,77,0,3,255,21,7,208,255,33,5,220};
        check(vector("answer_status_config_crc_targets",a,sizeof a)==32&&
              ultra_get16(a+6+ULTRA_ST_CONFIG_CRC)==ultra_config_crc(records,4),"STATUS CRC across channel/output/board targets");
        check(vector("answer_diag_part2",a,sizeof a)==32&&a[6+ULTRA_DG_DECLINE]==13&&a[7]==1&&a[8]==0&&a[9]==7,"DIAG part 2 decode");
        check(vector("answer_pixel_part0",a,sizeof a)==32&&ultra_get16(a+6+ULTRA_PXL_FRAMES)==100&&ultra_get24(a+6+ULTRA_PXL_BITS)==320,"PIXEL part 0 decode");
        check(vector("answer_lifetime_65a",a,sizeof a)==32&&a[6+ULTRA_LT_CURRENT_PEAK]*500u==65000u,"lifetime 65 A");
        check(vector("answer_diag_trip_64a_dip_65a",a,sizeof a)==32&&(a[6+ULTRA_DG_TRIP]>>2)*2000u==64000u&&a[6+ULTRA_DG_DIP_CURRENT]*500u==65000u,"DIAG current scales");
        check(vector("answer_power_peak_saturated",a,sizeof a)==32&&ultra_get16(a+6+ULTRA_PW_TRIP_MA)==65535u,"saturated short-circuit peak");
        check(vector("answer_energy_supply_z",a,sizeof a)==32&&ultra_get16(a+6+ULTRA_EN_PEAK_MA)==2250u&&
              ultra_get16(a+6+ULTRA_EN_SUPPLY_Z)==412u&&!a[6+21]&&!a[6+22]&&!a[6+23],"ENERGY supply impedance, mOhm");
        check(ULTRA_LIGHT_TYPES==11&&6u*ULTRA_LIGHT_TYPES*2u==132u,"analog sequence state unchanged");
        check(ULTRA_PROFILE_COUNT==4u&&ULTRA_PARLIO_SAMPLE_HZ==9600000u,"four profiles on one sample grid");
        check(vector("answer_acks_pixel_fx_never",a,sizeof a)==32&&
              (ultra_get24(a+6+ULTRA_AK_NEVER)&0x9249u)==0x9249u,
              "receiver reports NEVER for PIXEL_FX in all ACKS rows");
        check(!a[6]&&!a[9]&&!a[12]&&!a[15]&&!a[18]&&!a[21],"PIXEL_FX ACKS sequence bytes zero");
        check(vector("answer_pixel_fx_output1_group3",a,sizeof a)==32&&
              a[6+ULTRA_PXG_SEGMENTS]==15&&a[6+ULTRA_PXG_SEQ+2]==0x49&&
              a[6+ULTRA_PXG_RUNNING+2]==1&&!a[6+ULTRA_PXG_SEQ+3],"explicit segment 14 acknowledgement");
    }
    {uint8_t blob[512];unsigned j;
     static const char *names[]={"descriptor_nano2","descriptor_partial7","descriptor_local10","descriptor_analog20","descriptor_dual_pixel"};
     for(j=0;j<5;++j){int n=vector(names[j],blob,sizeof blob);unsigned pos=11,records=0;
       check(n>=11&&ultra_get16(blob+2)==(unsigned)n,"descriptor length");
       check(ultra_crc16(blob+6,(unsigned)n-6)==ultra_get16(blob+4),"descriptor CRC");
       while(pos<(unsigned)n){pos+=2u+blob[pos+1];++records;}
       check(pos==(unsigned)n&&records==blob[8]&&blob[6]==254&&blob[10]==201,"synthetic descriptor TLVs/opaque assembly");
     }}
    make_target(&target);compare("target_dual_pixel",target.bytes,16);
    check(ultra_get32(target.bytes+12)==503808&&sizeof(upd_bl_info_t)==32,"target size/bounds decode");
    memset(d,0,sizeof d);memcpy(d,"QDU6",4);d[4]=2;d[7]=255;memcpy(d+8,target.bytes,7);d[15]=3;
    ultra_put32(d+16,0x000700ff);ultra_put32(d+20,503808);ultra_put32(d+24,0x12345678);
    ultra_put32(d+28,0x08005000);ultra_put32(d+32,0x08005002);d[36]=2;d[37]=3;d[38]=1;
    for(i=0;i<16;++i)d[40+i]=(uint8_t)i;image_seal(d);compare("image_v407_range",d,64);
    memset(a,0,sizeof a);a[0]=ULTRA_RSP_SYNC;a[1]=ULTRA_PAGE_FW_UPDATE;a[3]=0x42;ultra_put16(a+4,ultra_crc16(uid,sizeof uid));
    a[6+ULTRA_FU_FORMAT]=ULTRA_FU_FORMAT_VERSION;
    a[6+ULTRA_FU_FLAGS]=ULTRA_FU_F_VERIFIED|ULTRA_FU_F_CONFIRMED|ULTRA_FU_F_TRIAL_SEEN;
    a[6+ULTRA_FU_TRIALS]=1;memcpy(a+6+ULTRA_FU_FW_VERSION,d+UPD_D_FW_VERSION,12);
    memcpy(a+6+ULTRA_FU_DESC_CRC,d+UPD_D_CRC,4);ultra_put16(a+30,ultra_crc16(a,30));
    compare("answer_fw_update_confirmed",a,32);
    check(ultra_answer_check(a)==0&&ultra_get32(a+6+ULTRA_FU_BODY_LEN)==503808&&
          ultra_get32(a+6+ULTRA_FU_DESC_CRC)==ultra_get32(d+UPD_D_CRC),"installed image confirmation decode");
    check(upd_desc_check(d,&target,3)==0,"accept revision 0");target.bytes[7]=255;check(upd_desc_check(d,&target,3)==0,"accept revision 255");target.bytes[7]=0;
    check(vector("image_future_app_formats",bad,sizeof bad)==64&&upd_desc_check(bad,&target,3)==0,"future app formats accepted by immutable bootloader");
    for(i=0;i<5;++i){memcpy(bad,d,64);bad[8+i]^=1;image_seal(bad);check(upd_desc_check(bad,&target,3)!=0,"reject target component before erase");}
    memcpy(bad,d,64);bad[13]=0x80;image_seal(bad);check(upd_desc_check(bad,&target,3)!=0,"required features");
    {upd_target_t extra=target;extra.bytes[UPD_T_FEATURES]=15;
     check(upd_desc_check(d,&extra,3)==0,"required feature subset remains valid");
     extra=target;extra.bytes[UPD_T_VARIANT]=8;memcpy(bad,d,64);bad[UPD_D_VARIANT]=8;image_seal(bad);
     check(upd_desc_check(bad,&extra,3)==0,"all opaque assembly values allowed when exact");
     bad[UPD_D_VARIANT]=9;image_seal(bad);check(upd_desc_check(bad,&extra,3)==UPD_ERROR_TARGET,"opaque assembly mismatch rejects");}
    memcpy(bad,d,64);bad[6]=1;image_seal(bad);check(upd_desc_check(bad,&target,3)!=0,"revision out of range");
    memcpy(bad,d,64);bad[6]=2;bad[7]=1;image_seal(bad);check(upd_desc_check(bad,&target,3)!=0,"inverted revision range");
    check(upd_desc_check(d,&target,2)!=0,"minimum bootloader within ABI");
    for(i=0;i<4;++i){static const uint32_t lengths[]={0,3,503812,0xfffffffc};memcpy(bad,d,64);ultra_put32(bad+20,lengths[i]);image_seal(bad);check(upd_desc_check(bad,&target,3)!=0,"length bounds");}
    for(i=0;i<4;++i){static const uint32_t entries[]={0x5000,0x08080000,0x08005001,0x08004ffe};memcpy(bad,d,64);ultra_put32(bad+32,entries[i]);image_seal(bad);check(upd_desc_check(bad,&target,3)!=0,"entry bounds/alignment/alias");}
    memcpy(bad,d,64);bad[39]=1;image_seal(bad);check(upd_desc_check(bad,&target,3)!=0,"reserved byte");
    memcpy(bad,d,64);bad[40]^=1;check(upd_desc_check(bad,&target,3)!=0,"descriptor CRC");
    {upd_target_t overflow=target;ultra_put32(overflow.bytes+8,0xffffff00);ultra_put32(overflow.bytes+12,512);
     memcpy(bad,d,64);ultra_put32(bad+20,256);ultra_put32(bad+28,0xffffff00);ultra_put32(bad+32,0xffffff00);image_seal(bad);
     check(upd_desc_check(bad,&overflow,3)!=0,"immutable slot addition overflow");}
    /* Transit hold (timing.md): exact erase units from INDEX 0, erase_max taken as 255. */
    check(upd_transit_hold_ms(d)==63340u&&upd_transit_hold_ms(d)>=124u*2u*200u+100u,
          "transit hold: full layout-4 slot 124 units, covers the 49.7 s full-slot lead");
    memcpy(bad,d,64);ultra_put32(bad+20,63488);check(upd_transit_hold_ms(bad)==8770u,"transit hold: 62 KiB layout-4 body, 17 units");
    memcpy(bad,d,64);ultra_put32(bad+20,4);check(upd_transit_hold_ms(bad)==1120u,"transit hold: smallest layout-4 body, 2 units");
    memcpy(bad,d,64);bad[8+UPD_T_LAYOUT]=UPD_LAYOUT_V203;ultra_put32(bad+28,UPD_V203_BODY);
    ultra_put32(bad+20,57088);check(upd_transit_hold_ms(bad)==UPD_TRANSIT_HOLD_MAX_MS,"transit hold: full layout-1 slot, 224 units");
    ultra_put32(bad+20,516);check(upd_transit_hold_ms(bad)==2140u,"transit hold: 516 B layout-1 body, 4 units");
    ultra_put32(bad+20,256);check(upd_transit_hold_ms(bad)==1120u,"transit hold: one layout-1 body unit plus metadata");
    {static const uint32_t lengths[]={0,2,57092,0xfffffffc};
     for(i=0;i<4;++i){ultra_put32(bad+20,lengths[i]);
         check(upd_transit_hold_ms(bad)==UPD_TRANSIT_HOLD_MAX_MS,"transit hold: layout-1 length out of range");}}
    {static const uint8_t at[]={0,3,4,8+UPD_T_LAYOUT,8+UPD_T_LAYOUT,28,31,20};
     static const uint8_t to[]={'q','7',3,2,0,0x09,0x04,0x08};
     for(i=0;i<8;++i){memcpy(bad,d,64);bad[at[i]]=to[i];
         check(upd_transit_hold_ms(bad)==UPD_TRANSIT_HOLD_MAX_MS,"transit hold: bad magic/version/layout/load/length -> largest");}}
    check(upd_transit_hold_ms(NULL)==UPD_TRANSIT_HOLD_MAX_MS&&UPD_TRANSIT_HOLD_MAX_MS==114340u,"transit hold: none -> 114340 ms");
    /* Every target-independent INDEX 0 invalidity of upd_desc_check() holds the
     * largest value; each case is resealed, so only the checked field is wrong. */
    {upd_target_t one=target;
     memcpy(bad,d,64);bad[8+UPD_T_LAYOUT]=UPD_LAYOUT_V203;ultra_put32(bad+20,4);
     ultra_put32(bad+28,UPD_V203_BODY);ultra_put32(bad+32,UPD_V203_BODY);image_seal(bad);
     one.bytes[UPD_T_LAYOUT]=UPD_LAYOUT_V203;ultra_put32(one.bytes+8,UPD_V203_BODY);ultra_put32(one.bytes+12,UPD_V203_END-UPD_V203_BODY);
     check(upd_desc_check(bad,&one,3)==0&&upd_transit_hold_ms(bad)==1120u,"transit hold: valid layout-1 length 4, 1120 ms");
     {static const uint8_t at[]={UPD_D_FLAGS,UPD_D_REV_MIN,UPD_D_FEATURES,UPD_D_TARGET+UPD_T_BOARD,
                                 UPD_D_TARGET+UPD_T_BOARD,UPD_D_TARGET+UPD_T_ABI,UPD_D_TARGET+UPD_T_ABI};
      static const uint8_t to[]={1,255,0x80,0,255,UPD_BOOT_ABI-1u,UPD_BOOT_ABI+1u};
      uint8_t rev[64];
      for(i=0;i<7;++i){upd_target_t match=one;memcpy(rev,bad,64);rev[at[i]]=to[i];
          if(at[i]==UPD_D_REV_MIN)rev[UPD_D_REV_MAX]=1;
          if(at[i]>=UPD_D_TARGET&&at[i]<UPD_D_TARGET+5u)match.bytes[at[i]-UPD_D_TARGET]=to[i];
          if(at[i]==UPD_D_FEATURES)match.bytes[UPD_T_FEATURES]=0xFF;
          image_seal(rev);
          check(upd_desc_check(rev,&match,3)!=0&&upd_transit_hold_ms(rev)==UPD_TRANSIT_HOLD_MAX_MS,
                "transit hold: flags/revision order/features/board/ABI invalid -> 114340 ms");}}}
    upd_build_frame(s,UPD_ANNOUNCE,7,0,0,0,d,0xabcdef);compare("update_announce0",s,48);
    upd_build_frame(s,UPD_ANNOUNCE,7,1,0,0,d+32,0xabcdef);compare("update_announce1",s,48);
    for(i=0;i<32;++i)data[i]=(uint8_t)i;
    upd_build_frame(s,UPD_DATA,7,65535,0,0,data,0xabcdef);compare("update_data_index65535",s,48);
    check(upd_check_frame(s)==0&&ultra_get16(s+2)*32u==2097120u,"u16 INDEX reach");
    s[42]=0x28;s[47]=ultra_crc8(s+40,7);check(upd_check_frame(s)!=0,"UPD version 2 rejected");
    memset(data,0,32);for(i=0;i<12;++i)data[i]=(uint8_t)i;data[13]=1;data[14]=2;
    upd_build_frame(s,UPD_STATUS,7,0,0,0,data,0xabcdef);compare("update_status_poll",s,48);
    check(upd_check_frame(s)==0&&s[42]==0x39,"UPD_STATUS RW header");
    s[0]=UPD_DATA;ultra_put16(s+38,ultra_crc16(s,38));check(upd_check_frame(s)!=0,"DATA cannot request RW");
    check(upd_erased16(0xe339)&&upd_erased16(0xffff)&&!upd_erased16(0),"erase patterns");
    for(i=0;i<3;++i){static const uint32_t pitch[]={500,835,1505};check(ultra_rw_slot_us((uint8_t)i)==pitch[i],"return pitch");}
    check(ultra_rw_window_us(16,2)==25080&&ultra_rw_slot_us(255)==0,"window and invalid-rate shift guard");
    check(ultra_rw_next_us(16,2,500)==25660,"return holdoff plus processing gap");
    check(UPD_REV_ENCODE(1u,1u)==UPD_REV_V1R1&&UPD_REV_ENCODE(0u,15u)<UPD_REV_V1R1,"PCB revision order");
    check(ULTRA_PORT_DIFF_POWER_MAX_BOARDS==8u&&ULTRA_PORT_D2A_PIN_MAX_BOARDS==50u&&ULTRA_CHAIN_USED_NS==588u,
          "per-port-type board limits, exact eight-hop topology");
    check(ultra_line_path_ns(3,150)==218&&ultra_line_path_ns(2,100)==144,"typical two/three-board worst case");
    check(ultra_line_path_ns(0,0)==65535&&ultra_line_path_ns(9,0)==65535&&
          ultra_line_path_ns(8,65535)==65535,"invalid topology/overflow fails closed");
    check(ultra_d2a_path_ns(1,0)==0&&ultra_d2a_path_ns(20,0)==171&&ultra_d2a_path_ns(50,0)==441&&
          ultra_d2a_path_ns(2,30)==39,"D2A path: N-1 gates of 9 ns plus wiring");
    check(ultra_d2a_path_ns(0,0)==65535&&ultra_d2a_path_ns(51,0)==65535&&
          ultra_d2a_path_ns(50,65535)==65535,"D2A path: 1..50 boards, overflow fails closed");
    {static const uint8_t ceiling[]={21,34,50,50};unsigned j,n,last;
     for(j=0;j<4;++j){for(last=0,n=1;n<=60;++n)if(ultra_d2a_admits((uint8_t)j,(uint8_t)n,0))last=n;
         check(last==ceiling[j],"D2A ideal-wiring ceiling per profile");
         for(n=1;n<=last;++n)check(ultra_d2a_admits((uint8_t)j,(uint8_t)n,0),"D2A admits every count up to its ceiling");}}
    {static const uint16_t reserve[][3]={{0,20,17},{0,21,8},{1,34,3},{2,35,144},{2,40,99},{2,50,9},{3,50,309}};unsigned j;
     for(j=0;j<7;++j)check(ultra_d2a_admits((uint8_t)reserve[j][0],(uint8_t)reserve[j][1],reserve[j][2])&&
         !ultra_d2a_admits((uint8_t)reserve[j][0],(uint8_t)reserve[j][1],(uint16_t)(reserve[j][2]+1u)),
         "D2A wiring reserve (forwarding LOW at 800, admission otherwise)");}
    check(ultra_d2a_admits(0,1,220)&&!ultra_d2a_admits(0,1,221)&&!ultra_d2a_admits(0,0,0)&&
          !ultra_d2a_admits(4,1,0),"D2A one board: admission only; invalid input fails closed");
    {static const uint16_t admissions[]={220,300,450,750};unsigned j,k;
     static const uint16_t ticks[2][4][3]={{{50,12,38},{67,17,50},{100,25,75},{200,50,150}},
                                         {{100,25,75},{133,33,100},{200,50,150},{400,100,300}}};
     for(j=0;j<4;++j){const ultra_line_profile_t *p=ultra_line_profile((uint8_t)j);
         check(p&&p->admission_ns==admissions[j],"profile admission table");
         check(ultra_line_allowed((uint8_t)j,1,1,15,admissions[j])&&
              !ultra_line_allowed((uint8_t)j,1,1,15,(uint16_t)(admissions[j]+1)),"admission boundary");
         check(!ultra_line_allowed((uint8_t)j,1,1,0,0),"common app/boot mask required");
         if(j){check(!ultra_line_allowed((uint8_t)j,0,1,15,0),"unknown inventory locks 800");
               check(!ultra_line_allowed((uint8_t)j,1,0,15,0),"non-Ultra device/pixel locks 800");}
         for(k=0;k<2;++k){ultra_line_rmt_t r;
             check(ultra_line_rmt((uint8_t)j,k?80000000u:40000000u,&r)==0&&
                   r.bit_ticks==ticks[k][j][0]&&r.zero_ticks==ticks[k][j][1]&&
                   r.one_ticks==ticks[k][j][2],"RMT actual ticks");}}
     {ultra_line_rmt_t r;check(!ultra_line_profile(4)&&
       ultra_line_rmt(4,40000000u,&r)<0&&ultra_line_rmt(0,20000000u,&r)<0&&
       ultra_line_rmt(0,40000000u,0)<0,"invalid profile/clock/output");}}
    check(ultra_line_margin_ns(541,709,84,74,597,625,30)==26,"signed conservative typical margin");
    check(ultra_line_margin_ns(600,650,20,10,597,625,30)==-33,"failed margin never wraps unsigned");
    check(ultra_line_candidate(0,255,1,0,2,0,60000)==0,"down hysteresis");
    check(ultra_line_candidate(0,255,1,0,3,0,0)==1,"down after three bad windows");
    check(ultra_line_candidate(0,255,1,1,0,0,0)==3,"hard fault requests recovery profile");
    check(ultra_line_candidate(0,255,0,1,3,60,60000)==0,"missing report does not authorize change");
    check(ultra_line_candidate(2,255,1,0,0,60,59999)==2&&
          ultra_line_candidate(2,255,1,0,0,59,60000)==2,"up dwell and candidate probe hysteresis");
    check(ultra_line_candidate(2,255,1,0,0,60,60000)==1,"qualified probe up one step");
    check(ultra_line_candidate(0,3,0,0,0,0,0)==3&&
          !ultra_line_allowed(3,1,0,15,0),"manual candidate still subject to inventory");
    check(ultra_line_candidate(3,255,1,1,3,0,0)==3&&
          ultra_line_candidate(2,9,1,0,3,0,0)==2,"slowest and invalid override bounds");
    check(ultra_bank_count(2)==1&&ultra_bank_mask(2,0)==3&&ultra_bank_mask(2,1)==0,"two channel partial bank");
    check(ultra_bank_count(7)==2&&ultra_bank_mask(7,1)==3,"seven channel partial final bank");
    check(ultra_bank_count(10)==2&&ultra_bank_mask(10,1)==31&&ultra_bank_mask(10,2)==0,"ten channel banks");
    check(ultra_lower_limit(1800,2000)==1800&&ultra_lower_limit(1800,1200)==1200&&ultra_lower_limit(1800,0)==1800,"only lower firmware ratings");
    check(ultra_lower_limit(65535,1200)==0&&ultra_lower_limit(0,1200)==0,"unknown or disabled never grants load");

    check(vector("answer_link_stats",a,sizeof a)==32&&ultra_get16(a+6+ULTRA_LS_ZERO_MAX)==325&&ultra_get16(a+6+ULTRA_LS_ONE_MIN)==920,"high-time statistics offsets");
    {
        unsigned j;int64_t start;
        for(j=0;j<sizeof ultra_local_vectors/sizeof ultra_local_vectors[0];++j){
            const ultra_local_vector_t *v=ultra_local_vectors+j;uint8_t packet[58];
            check(!ultra_local_check(v->packet,v->size),"L1 envelope and inner CRC");
            check(ultra_local_build(packet,sizeof packet,v->packet[2],v->packet[3],
                (uint16_t)ultra_get16(v->packet+4),v->packet+8,(uint16_t)ultra_get16(v->packet+6))==v->size&&
                !memcmp(packet,v->packet,v->size),"L1 committed packet golden");
            if(v->timed){
                start=123;check(!ultra_local_start_us(v->packet,v->size,v->accepted_us,&start)&&
                    start==v->start_us,"L1 signed acceptance-relative START/phase golden");
                check(ultra_local_start_us(v->packet,(uint16_t)(v->size-1),v->accepted_us,&start)<0,
                    "truncated packet cannot establish acceptance");
                check(ultra_local_start_us(v->packet,v->size,-1,&start)<0,"negative acceptance rejected");
                if(v->start_us>v->accepted_us){
                    start=123;check(ultra_local_start_us(v->packet,v->size,INT64_MAX,&start)<0&&
                        start==123,"L1 timestamp overflow leaves output untouched");
                }
                packet[38]^=1;ultra_put16(packet+40,ultra_crc16(packet,40));start=123;
                check(ultra_local_start_us(packet,v->size,v->accepted_us,&start)<0&&start==123,
                    "bad slot CRC despite intact envelope cannot schedule");
            }else if(v->packet[3]==1){
                check(!ultra_local_start_us(v->packet,v->size,1000000,&start)&&start==1000000,
                    "untimed L1 command starts at acceptance");
            }else check(ultra_local_start_us(v->packet,v->size,1000000,&start)<0,
                "non-slot L1 packet has no command START");
        }
    }
    {uint8_t packet[58],bytes[24]={0};unsigned k;uint16_t size;
     check(vector("slot_config_channel19",s,sizeof s)==32,"load local slot");
     size=ultra_local_build(packet,sizeof packet,3,1,42,s,32);
     check(size==42&&ultra_local_check(packet,size)==0&&memcmp(packet+8,s,32)==0,"local framing preserves command");
     check(ultra_get16(packet+40)==0x1135u,"independent local CRC golden");
     packet[40]^=1;check(ultra_local_check(packet,size)!=0,"local CRC rejection");
     check(ultra_local_build(packet,41,3,1,42,s,32)==0&&ultra_local_build(packet,58,255,1,42,s,32)==0,"local bounds");
     for(k=0;k<24;++k)bytes[k]=(uint8_t)k;
     check(ultra_slot_pixel_data(s,0,9,1,0,bytes)==0,"pixel fragment builder");compare("pixel_chunk0",s,32);
     check(ultra_slot_pixel_data(s,15,9,1,0,bytes)!=0,"pixel output bounds");}

    check(vector("answer_link_context_3",a,sizeof a)==32&&a[6+ULTRA_LC_PROFILE]==3&&
          ultra_get16(a+6+ULTRA_LC_EPOCH)==7&&ultra_get16(a+6+ULTRA_LC_PERIOD_MIN)==4990,
          "profile context report offsets");
    {
        static const uint8_t assemblies[]={0,1,201,255};
        upd_target_t variants[4];uint8_t image[64];unsigned j,k;
        for(j=0;j<4;++j){make_target(&variants[j]);variants[j].bytes[6]=assemblies[j];
            memcpy(image,d,64);memcpy(image+8,variants[j].bytes,7);image[6]=image[7]=variants[j].bytes[7];
            ultra_put32(image+20,256);image_seal(image);
            check(upd_desc_check(image,&variants[j],3)==0,"matching assembly accepts");
            for(k=0;k<j;++k)check(upd_desc_check(image,&variants[k],3)==UPD_ERROR_TARGET,"different opaque assembly rejects before erase");
        }
    }
    printf("%s: %u C encode/decode and rejection checks\n",failures?"FAIL":"PASS",checks);free(json);return failures?1:0;
}
