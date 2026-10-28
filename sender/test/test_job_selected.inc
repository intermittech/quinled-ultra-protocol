/* SPDX-License-Identifier: MIT */
static int job_bad_read(void *ctx,uint32_t off,uint8_t *out,uint32_t n)
{int rc=job_read(ctx,off,out,n);if(!rc && !off && n)out[0]^=1;return rc;}
static void job_lifecycle_edges(void)
{
    ultra_update_job_t j;ultra_update_job_options_t o={0};ultra_update_node_t n[2];
    ultra_update_artifact_t a={0};ultra_update_result_t r[2];job_source_t source={0};
    ultra_identity_t id;uint8_t f[72],answer[32],data[24];unsigned k,verified=0;uint8_t ended=0;int rc;
    memset(n,0,sizeof n);update_image(&a.descriptor,&n[0].participant.target,&n[0].participant.layout,1);
    n[0].trusted=n[0].reachable=n[0].participant.has_return=1;n[0].firmware=1;n[0].participant.uid[0]=1;
    n[1]=n[0];n[1].participant.uid[0]=2;a.authenticated=1;a.read=read_body;
    o.mode=ULTRA_UPDATE_SELECTED;o.first_session=254;o.passes=1;o.rate=2;o.verify_timeout_ms=50;o.port_type=ULTRA_PORT_DIFF_POWER;
    job_setup(2);CHECK(!ultra_update_job_begin(&j,&line,n,2,&a,1,r,&o,now));
    for(k=0;k<2000 && j.state!=ULTRA_JOB_DONE;++k){now=j.due_us;
        rc=ultra_update_job_next_frame(&j,now,f,sizeof f);CHECK(rc>=0);
        if(rc==48 && f[0]==UPD_SELECT)CHECK(!memcmp(f+6,n[j.selected_node].participant.uid,12));
        if(rc==48 && f[0]==UPD_END)ended=j.transfer.session;
        if(rc==48 && f[0]==UPD_STATUS){uint32_t committed=j.transfer.offset/256u*256u;
            memset(data,0,sizeof data);data[0]=1;
            data[1]=(uint8_t)(ended==j.transfer.session?UPD_STATE_COMMITTED:UPD_STATE_READY);
            data[3]=committed<512?8:0;data[4]=j.transfer.session;
            ultra_put32(data+8,committed);ultra_put32(data+12,committed);ultra_put32(data+20,j.transfer.plan.body_crc);
            envelope(answer,0x86,j.transfer.session,j.transfer.uid,data);
            CHECK(!ultra_update_job_answer(&j,answer,j.transfer.poll_end_us+2400));
            CHECK(j.due_us==j.transfer.due_us);
        }
        if(j.state==ULTRA_JOB_VERIFY && r[j.selected_node].result==ULTRA_JOB_PENDING){
            memset(&id,0,sizeof id);id.parts=7;id.target=n[j.selected_node].participant.target;
            memcpy(id.uid,n[j.selected_node].participant.uid,12);id.firmware=r[j.selected_node].expected;
            memset(data,0,sizeof data);data[0]=1;data[1]=3;ultra_put32(data+4,id.firmware);ultra_put32(data+8,512);
            ultra_put32(data+12,j.transfer.plan.body_crc);ultra_put32(data+16,j.transfer.plan.descriptor_crc);
            envelope(answer,5,3,id.uid,data);
            CHECK(!ultra_update_job_confirm(&j,j.selected_node,0,&id,answer,now+1));++verified;
        }
    }
    CHECK(j.state==ULTRA_JOB_DONE && j.sessions==2 && verified==2 && !line.update_active);
    CHECK(r[0].result==ULTRA_JOB_VERIFIED && r[1].result==ULTRA_JOB_VERIFIED);
    o.mode=ULTRA_UPDATE_BLIND;o.passes=ULTRA_UPDATE_BLIND_MIN_PASSES;job_setup(1);o.deadline_us=now+1;
    a.context=&source;a.prepare=job_prepare;a.release=job_release;a.read=job_read;
    CHECK(!ultra_update_job_begin(&j,&line,n,1,&a,1,r,&o,now));
    CHECK(!ultra_update_job_next_frame(&j,now,f,sizeof f) && source.prepares==1 && !source.reads);
    CHECK(!ultra_update_job_next_frame(&j,j.due_us,f,sizeof f) && j.state==ULTRA_JOB_DONE && !line.update_active);
    CHECK(r[0].result==ULTRA_JOB_FAILED && r[0].reason==ULTRA_JOB_ABORT && source.releases==1);
    memset(&source,0,sizeof source);o.deadline_us=0;a.read=job_bad_read;job_setup(1);
    CHECK(!ultra_update_job_begin(&j,&line,n,1,&a,1,r,&o,now));
    CHECK(!ultra_update_job_next_frame(&j,now,f,sizeof f));
    CHECK(ultra_update_job_next_frame(&j,j.due_us,f,sizeof f)<0 && j.state==ULTRA_JOB_DONE && !line.update_active);
    CHECK(r[0].result==ULTRA_JOB_FAILED && !r[0].transfer_complete && source.releases==1);
}
