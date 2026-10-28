/* SPDX-License-Identifier: MIT */
#include <string.h>
#include "ultra_sender_job.h"

static int uidless(const ultra_update_participant_t *n)
{
    static const uint8_t zero[12]={0};
    return !n->has_return && !memcmp(n->uid,zero,sizeof zero);
}
static int compatible(const ultra_update_artifact_t *a,const ultra_update_node_t *n)
{
    return upd_desc_check(a->descriptor.bytes,&n->participant.target,n->participant.layout.boot_version)==0;
}
static int duplicate(const ultra_update_artifact_t *a,const ultra_update_artifact_t *b)
{
    return !memcmp(a->descriptor.bytes,b->descriptor.bytes,64);
}
/* Same tuple and variant with overlapping revision ranges: some receiver
 * revision accepts both, whatever its features or boot version. */
static int overlapping(const ultra_update_artifact_t *a,const ultra_update_artifact_t *b)
{
    const uint8_t *x=a->descriptor.bytes,*y=b->descriptor.bytes;
    return !duplicate(a,b) && !memcmp(x+8,y+8,5) && x[14]==y[14] && x[6]<=y[7] && y[6]<=x[7];
}
/* Overlaps are refused at begin, so the first compatible artifact is the only
 * one (or an identical duplicate). */
static int choice(const ultra_update_node_t *n,const ultra_update_artifact_t *a,uint16_t count)
{
    unsigned i;
    for(i=0;i<count;++i)if(compatible(a+i,n))return (int)i;
    return -1;
}
static uint32_t version(const ultra_update_artifact_t *a){return ultra_get32(a->descriptor.bytes+16);}
/* A known (trusted) board with a return path that doesn't report back. It may
 * sit dark in its recovery bootloader, or run with a broken return path; a
 * port update sends it nothing. Boards refuse an equal or older version unless
 * the ANNOUNCE carries FORCE, which the sender never sets. */
static int silent(const ultra_update_node_t *n){return n->participant.has_return && !n->reachable;}
static uint8_t missing_reason(const ultra_update_node_t *n,const ultra_update_artifact_t *a,uint16_t count)
{
    unsigned i;
    for(i=0;i<count;++i)if(!memcmp(n->participant.target.bytes,a[i].descriptor.bytes+8,5) &&
        n->participant.target.bytes[6]==a[i].descriptor.bytes[14])return ULTRA_JOB_INCOMPATIBLE;
    return ULTRA_JOB_MISSING_ARTIFACT;
}
/* target<0: "update all receivers on this port"; else ultra_update_job_recover(). */
static int plan(ultra_update_job_t *j,ultra_sender_t *line,
    const ultra_update_node_t *nodes,uint16_t count,int target,const ultra_update_artifact_t *artifacts,uint16_t ac,
    ultra_update_result_t *results,const ultra_update_job_options_t *options,int64_t now)
{
    ultra_update_job_t v;ultra_update_plan_t p;unsigned i,k,max;uint32_t blackout=0;int a,rc,ra=-1;
    if(!j || !line || !nodes || !results || !options || !count || target>=(int)count)return -1;
    max=ultra_port_max_boards(options->port_type);
    if(!max)return -1;
    if(count>max || line->n>max)return ULTRA_UPDATE_TOO_MANY_BOARDS;
    if(ultra_port_line_too_fast(options->port_type,line))return ULTRA_UPDATE_LINE_TOO_FAST;
    if(options->mode==ULTRA_UPDATE_BLIND && options->passes<ULTRA_UPDATE_BLIND_MIN_PASSES)return ULTRA_UPDATE_TOO_FEW_PASSES;
    if(count!=line->n || ac>ULTRA_UPDATE_ARTIFACT_MAX || (ac && !artifacts) ||
       line->update_active || line->tx_active || line->profile_training || now<line->hold_us ||
       (line->has_sent && now<line->last_start_us+line->frame_us) ||
       options->mode>ULTRA_UPDATE_SELECTED || !options->passes || options->passes>15 || options->rate>=3 ||
       !options->first_session || (line->digital && count!=1) ||
       (options->deadline_us && options->deadline_us<=now))return -1;
    memset(&v,0,sizeof v);v.options=*options;
    if(!v.options.verify_timeout_ms)v.options.verify_timeout_ms=10000;
    if(!v.options.family_order[0] && !v.options.family_order[1]){
        v.options.family_order[0]=UPD_MCU_V407;v.options.family_order[1]=UPD_MCU_V203;
    }
    if(!((v.options.family_order[0]==4 && v.options.family_order[1]==1) ||
         (v.options.family_order[0]==1 && v.options.family_order[1]==4)))return -1;
    v.entry_ms=UPD_ENTRY_MIN_MS;v.gap_us=ultra_timing_gap_us(&line->timing);
    for(i=0;i<count;++i){const ultra_update_node_t *n=nodes+i;uint32_t entry;
        if(!n->trusted || n->participant.layout.entry_10ms<UPD_ENTRY_MIN_MS/10u || n->min_gap_us>1000000u ||
           n->blackout_ms>UINT32_MAX-2550u)return -UPD_ERROR_BOUNDS;
        /* UID-less one-way boards are never selected or confirmed; their
         * result belongs to the inventory index. Nonzero duplicates refuse. */
        for(k=0;k<i;++k)if(!uidless(&n->participant) && !uidless(&nodes[k].participant) &&
            !memcmp(n->participant.uid,nodes[k].participant.uid,12))return -UPD_ERROR_TARGET;
        entry=(uint32_t)n->participant.layout.entry_10ms*10u;
        if(entry>v.entry_ms)v.entry_ms=entry;
        if(n->blackout_ms>blackout)blackout=n->blackout_ms;
        if(n->min_gap_us>v.gap_us)v.gap_us=n->min_gap_us;
    }
    if(v.gap_us<ULTRA_FRAME_GAP_US)v.gap_us=ULTRA_FRAME_GAP_US;
    v.entry_ms+=blackout;
    /* Preflight every authenticated descriptor. Prepare/CRC-check bodies only
     * when their subtransfer starts, so an ESP32 needs one streamed source. */
    for(i=0;i<ac;++i){int matched=0;
        if(!artifacts[i].authenticated || !artifacts[i].read)return -UPD_ERROR_CRC;
        for(k=0;k<count;++k)if(compatible(artifacts+i,nodes+k)){
            rc=ultra_update_plan_metadata(&p,&artifacts[i].descriptor,&nodes[k].participant.target,
                &nodes[k].participant.layout,v.gap_us);
            if(rc)return -rc;
            matched=1;
        }
        /* A release may contain targets absent from this port. Still validate
         * descriptor using the descriptor's own minimal tuple;
         * no geometry/erase authority is inferred from that tuple. */
        if(!matched){uint32_t len=ultra_get32(artifacts[i].descriptor.bytes+20);
            upd_target_t t;memset(&t,0,sizeof t);memcpy(t.bytes,artifacts[i].descriptor.bytes+8,7);
            t.bytes[7]=artifacts[i].descriptor.bytes[6];memcpy(t.bytes+8,artifacts[i].descriptor.bytes+28,4);
            ultra_put32(t.bytes+12,len);
            if(upd_desc_check(artifacts[i].descriptor.bytes,&t,255))return -UPD_ERROR_TARGET;
        }
    }
    for(i=0;i<ac;++i)for(k=0;k<i;++k)if(overlapping(artifacts+i,artifacts+k))return ULTRA_UPDATE_OVERLAPPING_ARTIFACTS;
    /* Count selected UID transfers separately: each requires a fresh session. */
    k=0;
    for(i=0;i<count;++i){a=choice(nodes+i,artifacts,ac);
        if(a>=0 && nodes[i].firmware<ultra_get32(artifacts[a].descriptor.bytes+16) &&
           nodes[i].reachable && nodes[i].participant.has_return &&
           (nodes[i].participant.target.bytes[5]&UPD_FEATURE_STATUS) &&
           options->mode==ULTRA_UPDATE_SELECTED)++k;
    }
    if(k>255)return -UPD_ERROR_BOUNDS;
    /* Recover: one silent known board, with an artifact newer than its version. */
    if(target>=0){ra=choice(nodes+target,artifacts,ac);
        if(!silent(nodes+target) || ra<0 || nodes[target].firmware>=version(artifacts+ra))return ULTRA_UPDATE_NOT_RECOVERABLE;
    }
    /* A known board on the port that doesn't report back means something is
     * wrong: the port update doesn't run. Name every silent node. */
    for(i=0,k=0;target<0 && i<count;++i)k+=(unsigned)silent(nodes+i);
    if(k){
        for(i=0;i<count;++i){ultra_update_result_t *r=results+i;const ultra_update_node_t *n=nodes+i;
            memset(r,0,sizeof *r);r->before=r->observed=n->firmware;r->evidence_us=n->evidence_us;
            r->artifact=ULTRA_UPDATE_NO_ARTIFACT;a=choice(n,artifacts,ac);
            if(a>=0){r->artifact=(uint16_t)a;r->expected=version(artifacts+a);}
            r->result=silent(n)?ULTRA_JOB_UNREACHABLE:ULTRA_JOB_SKIPPED;
            r->reason=silent(n)?ULTRA_JOB_NOT_ANSWERING:ULTRA_JOB_PORT_BLOCKED;
        }
        return ULTRA_UPDATE_SILENT_BOARDS;
    }
    for(i=0;i<count;++i){ultra_update_result_t *r=results+i;const ultra_update_node_t *n=nodes+i;
        memset(r,0,sizeof *r);r->before=n->firmware;r->observed=n->firmware;r->evidence_us=n->evidence_us;
        r->artifact=ULTRA_UPDATE_NO_ARTIFACT;a=choice(n,artifacts,ac);
        if(a<0){r->result=ULTRA_JOB_SKIPPED;r->reason=missing_reason(n,artifacts,ac);continue;}
        r->artifact=(uint16_t)a;r->expected=version(artifacts+a);
        if(target>=0 && a!=ra){r->result=ULTRA_JOB_SKIPPED;r->reason=ULTRA_JOB_OTHER_TARGET;}
        else if((int)i==target)r->recovery=1;
        else if(n->firmware==r->expected)r->result=ULTRA_JOB_CURRENT;
        else if(n->firmware>r->expected){r->result=ULTRA_JOB_SKIPPED;r->reason=ULTRA_JOB_NEWER;}
        else if(target<0 && options->mode==ULTRA_UPDATE_SELECTED &&
            (!(n->participant.target.bytes[5]&UPD_FEATURE_STATUS) || !n->participant.has_return)){
            r->result=ULTRA_JOB_SKIPPED;r->reason=ULTRA_JOB_INCOMPATIBLE;
        }
        r->planned=r->result==ULTRA_JOB_PENDING;
    }
    v.line=line;v.nodes=nodes;v.node_count=count;v.artifacts=artifacts;v.artifact_count=ac;v.results=results;
    v.state=ULTRA_JOB_READY;v.started_us=v.due_us=now;
    line->update_active=1;*j=v;return 0;
}
int ultra_update_job_begin(ultra_update_job_t *j,ultra_sender_t *line,
    const ultra_update_node_t *nodes,uint16_t count,const ultra_update_artifact_t *artifacts,uint16_t ac,
    ultra_update_result_t *results,const ultra_update_job_options_t *options,int64_t now)
{return plan(j,line,nodes,count,-1,artifacts,ac,results,options,now);}
int ultra_update_job_recover(ultra_update_job_t *j,ultra_sender_t *line,
    const ultra_update_node_t *nodes,uint16_t count,uint16_t node,const ultra_update_artifact_t *artifacts,uint16_t ac,
    ultra_update_result_t *results,const ultra_update_job_options_t *options,int64_t now)
{return plan(j,line,nodes,count,node,artifacts,ac,results,options,now);}
static void release_artifact(ultra_update_job_t *j)
{
    if(j->artifact_prepared){const ultra_update_artifact_t *a=j->artifacts+j->active_artifact;
        if(a->release)a->release(a->context);
        j->artifact_prepared=0;}
}
/* A recovery job's artifact goes out blind whatever the options' mode. */
static int blind(const ultra_update_job_t *j,unsigned a)
{
    unsigned i;
    if(j->options.mode==ULTRA_UPDATE_BLIND)return 1;
    for(i=0;i<j->node_count;++i)if(j->results[i].recovery && j->results[i].artifact==a)return 1;
    return 0;
}
static uint8_t passes(const ultra_update_job_t *j,unsigned a)
{
    return blind(j,a) && j->options.passes<ULTRA_UPDATE_BLIND_MIN_PASSES?(uint8_t)ULTRA_UPDATE_BLIND_MIN_PASSES:j->options.passes;
}
static int start(ultra_update_job_t *j,int64_t now)
{
    unsigned a,i;ultra_update_plan_t slow,p;int rc,group;
    for(;j->family<2;++j->family,j->cursor=0)for(a=j->cursor;a<j->artifact_count;++a){
        if(j->artifacts[a].descriptor.bytes[9]!=j->options.family_order[j->family])continue;
        for(i=0;i<j->node_count;++i)if(j->results[i].result==ULTRA_JOB_PENDING &&
                !j->results[i].transfer_complete && j->results[i].artifact==a)break;
        if(i==j->node_count)continue;
        if(j->sessions==255)return -1;
        j->selected_node=(uint16_t)i;j->active_artifact=(uint16_t)a;
        if(!j->artifact_prepared)j->artifact_prepared=1;
        if(j->artifacts[a].prepare){rc=j->artifacts[a].prepare(j->artifacts[a].context);
            if(rc>0){j->due_us=now+10000;return 2;}if(rc<0)return rc;}
        j->line->update_active=0;group=blind(j,a);
        rc=ultra_update_begin_mode(&j->transfer,j->line,&j->artifacts[a].descriptor,
            &j->nodes[i].participant.target,&j->nodes[i].participant.layout,j->nodes[i].participant.uid,
            j->options.port_type,
            (uint8_t)(1u+((unsigned)j->options.first_session-1u+j->sessions)%255u),passes(j,a),
            j->options.rate,group?ULTRA_UPDATE_BLIND:ULTRA_UPDATE_SELECTED,now,j->artifacts[a].read,j->artifacts[a].context);
        j->line->update_active=1;if(rc){j->begin_code=rc;return rc;}
        j->cursor=(uint16_t)(group?a+1:a);
        ++j->sessions;slow=j->transfer.plan;
        /* Blind: pace for every node that accepts this image on the wire (the
         * receiver's own target/revision/feature/boot check), planned or not. */
        for(i=0;i<j->node_count;++i)if(group?compatible(j->artifacts+a,j->nodes+i):i==j->selected_node){
            rc=ultra_update_plan_metadata(&p,&j->artifacts[a].descriptor,&j->nodes[i].participant.target,
                &j->nodes[i].participant.layout,j->gap_us);
            if(rc){j->begin_code=-rc;return -rc;}
            if(p.lead_ms>slow.lead_ms)slow.lead_ms=p.lead_ms;
            if(p.pause_ms>slow.pause_ms)slow.pause_ms=p.pause_ms;
            if(p.finish_ms>slow.finish_ms)slow.finish_ms=p.finish_ms;
        }
        slow.entry_ms=j->entry_ms;j->transfer.plan=slow;j->transfer.gap_us=j->gap_us;
        j->state=ULTRA_JOB_TRANSFER;return 1;
    }
    j->state=ULTRA_JOB_DONE;j->line->update_active=0;return 0;
}
static int active(const ultra_update_job_t *j,unsigned i)
{
    return j->results[i].artifact==j->active_artifact && (blind(j,j->active_artifact) || i==j->selected_node);
}
static void failed_results(ultra_update_job_t *j,uint8_t reason,int64_t now)
{
    unsigned i;
    for(i=0;i<j->node_count;++i)if(j->results[i].result==ULTRA_JOB_PENDING){
        j->results[i].result=ULTRA_JOB_FAILED;j->results[i].reason=reason;j->results[i].evidence_us=now;}
}
int ultra_update_job_verification_done(ultra_update_job_t *j,int64_t now)
{
    unsigned i;
    if(!j || j->state!=ULTRA_JOB_VERIFY || now<j->verify_started_us || j->line->tx_active || now<j->line->hold_us)return -1;
    for(i=0;i<j->node_count;++i)if(active(j,i) && j->results[i].result==ULTRA_JOB_PENDING){
        j->results[i].result=j->nodes[i].participant.has_return?ULTRA_JOB_UNREACHABLE:ULTRA_JOB_SENT_UNVERIFIED;
        j->results[i].reason=!j->nodes[i].participant.has_return?ULTRA_JOB_NO_RETURN:
            j->results[i].recovery?ULTRA_JOB_RECOVERY_SENT:ULTRA_JOB_NO_REPLY;
        j->results[i].evidence_us=now;}
    release_artifact(j);j->state=ULTRA_JOB_READY;j->line->update_active=1;j->due_us=now;return 0;
}
int ultra_update_job_next_frame(ultra_update_job_t *j,int64_t now,uint8_t *f,uint32_t capacity)
{
    unsigned i;int rc;
    if(!j || !f || !j->line || capacity<48u)return -1;
    if(j->state==ULTRA_JOB_DONE || now<j->due_us)return 0;
    if(j->state==ULTRA_JOB_ABORTING){
        if(now<j->due_us)return 0;
        ultra_update_next_frame(&j->transfer,now,f,capacity);release_artifact(j);j->state=ULTRA_JOB_DONE;return 0;
    }
    if(j->options.deadline_us && now>=j->options.deadline_us)j->abort_requested=1;
    if(j->abort_requested){
        if(j->state==ULTRA_JOB_READY){release_artifact(j);j->state=ULTRA_JOB_DONE;j->line->update_active=0;}
        else{rc=ultra_update_abort(&j->transfer,now,f);j->due_us=j->transfer.due_us;
            if(!rc){if(j->line->hold_us>j->due_us)j->due_us=j->line->hold_us;
                if(j->due_us<=now)j->due_us=now+10000;
                return 0;}
            j->state=ULTRA_JOB_ABORTING;}
        failed_results(j,j->error?ULTRA_JOB_TRANSPORT:ULTRA_JOB_ABORT,now);
        return j->transfer.state==ULTRA_UPDATE_ABORTED?48:0;
    }
    if(j->state==ULTRA_JOB_VERIFY){
        for(i=0;i<j->node_count;++i)if(active(j,i) && j->results[i].result==ULTRA_JOB_PENDING)break;
        if(i==j->node_count || now>=j->verify_started_us+(int64_t)j->options.verify_timeout_ms*1000){
            if(ultra_update_job_verification_done(j,now)){j->due_us=j->line->hold_us;
                if(j->due_us<=now)j->due_us=now+10000;
                return 0;}
        }else{j->due_us=now+10000;return 0;}
    }
    if(j->state==ULTRA_JOB_READY){rc=start(j,now);
        if(rc<0 && j->begin_code){
            for(i=0;i<j->node_count;++i)if(active(j,i) && j->results[i].result==ULTRA_JOB_PENDING){
                j->results[i].result=ULTRA_JOB_FAILED;j->results[i].reason=ULTRA_JOB_REFUSED;j->results[i].evidence_us=now;}
            failed_results(j,ULTRA_JOB_ABORT,now);release_artifact(j);j->state=ULTRA_JOB_DONE;
            j->error=rc<=-2 && rc>=-UPD_ERROR_UNSUPPORTED?(uint8_t)-rc:0u;j->line->update_active=0;return rc;}
        if(rc<0){failed_results(j,ULTRA_JOB_TRANSPORT,now);release_artifact(j);j->state=ULTRA_JOB_DONE;
            j->error=UPD_ERROR_CRC;j->line->update_active=0;return rc;}
        if(rc==0 || rc==2)return 0;
    }
    rc=ultra_update_next_frame(&j->transfer,now,f,capacity);j->due_us=j->transfer.due_us;
    if(rc<0){j->error=j->transfer.error;j->abort_requested=1;return rc;}
    if(j->transfer.state==ULTRA_UPDATE_CONFIRM && j->state==ULTRA_JOB_TRANSFER && now>=j->transfer.due_us){
        for(i=0;i<j->node_count;++i)if(j->results[i].result==ULTRA_JOB_PENDING && active(j,i)){
            j->results[i].transfer_complete=1;
            if(!j->nodes[i].participant.has_return){j->results[i].result=ULTRA_JOB_SENT_UNVERIFIED;
                j->results[i].reason=ULTRA_JOB_NO_RETURN;j->results[i].evidence_us=now;}
        }
        j->state=ULTRA_JOB_VERIFY;j->verify_started_us=now;
    }
    return rc;
}
int ultra_update_job_next(ultra_update_job_t *j,int64_t now,uint8_t f[48])
{return ultra_update_job_next_frame(j,now,f,48);}
int ultra_update_job_answer(ultra_update_job_t *j,const uint8_t a[32],int64_t now)
{
    int rc;
    if(!j || j->state!=ULTRA_JOB_TRANSFER)return -1;
    rc=ultra_update_answer(&j->transfer,a,now);j->due_us=j->transfer.due_us;
    if(j->line->hold_us>j->due_us)j->due_us=j->line->hold_us;
    if(j->transfer.state==ULTRA_UPDATE_FAILED){j->error=j->transfer.error;j->abort_requested=1;}
    return rc;
}
void ultra_update_job_abort(ultra_update_job_t *j){if(j)j->abort_requested=1;}
int ultra_update_job_confirm(ultra_update_job_t *j,uint16_t node,uint8_t expected_answer_index,const ultra_identity_t *id,
                            const uint8_t a[32],int64_t evidence)
{
    ultra_update_result_t *r;const uint8_t *v;const ultra_update_artifact_t *artifact;unsigned i;
    if(!j || !id || !a || node>=j->node_count || j->state!=ULTRA_JOB_VERIFY ||
       !active(j,node) || evidence<=j->verify_started_us || evidence<=j->nodes[node].evidence_us ||
       id->parts!=7 || !j->nodes[node].participant.has_return ||
       ultra_answer_check(a) || a[1]!=ULTRA_PAGE_FW_UPDATE || (a[2]>>4)!=expected_answer_index ||
       ultra_get16(a+4)!=ultra_crc16(id->uid,12))return -1;
    r=j->results+node;if(r->result!=ULTRA_JOB_PENDING || !r->transfer_complete)return -1;
    v=a+6;if(v[0]!=1 || (v[1]&~7u) || v[2]>96 || v[3])return -1;
    for(i=20;i<24;++i)if(v[i])return -1;
    r->observed=id->firmware;r->evidence_us=evidence;artifact=j->artifacts+r->artifact;
    if(memcmp(id->uid,j->nodes[node].participant.uid,12) ||
       memcmp(id->target.bytes,j->nodes[node].participant.target.bytes,16) ||
       id->firmware!=r->expected || (v[1]&3u)!=3u || ultra_get32(v+4)!=r->expected ||
       ultra_get32(v+8)!=ultra_get32(artifact->descriptor.bytes+20) ||
       ultra_get32(v+12)!=ultra_get32(artifact->descriptor.bytes+24) ||
       ultra_get32(v+16)!=ultra_get32(artifact->descriptor.bytes+60)){
        r->result=ULTRA_JOB_FAILED;r->reason=ULTRA_JOB_MISMATCH;return 1;
    }
    r->result=ULTRA_JOB_VERIFIED;r->reason=ULTRA_JOB_REASON_NONE;return 0;
}
int ultra_update_job_progress(const ultra_update_job_t *j,int64_t now,ultra_update_job_progress_t *p)
{
    unsigned i,k;uint64_t remaining=0;uint32_t total=0;int in_transfer;
    if(!j || !p || !j->line || now<j->started_us)return -1;
    memset(p,0,sizeof *p);p->artifact=ULTRA_UPDATE_NO_ARTIFACT;
    p->elapsed_ms=(uint64_t)(now-j->started_us)/1000;
    /* Count planned distinct blind artifacts, or selected UID transfers. */
    for(i=0;i<j->node_count;++i){const ultra_update_result_t *r=j->results+i;
        if(!r->planned)continue;
        if(blind(j,r->artifact)){
            for(k=0;k<i;++k)if(j->results[k].artifact==r->artifact &&
                j->results[k].planned)break;
            if(k<i)continue;
        }
        ++p->subtransfers;
        if(!r->transfer_complete && r->result==ULTRA_JOB_PENDING){ultra_update_plan_t plan;
            if(!ultra_update_plan_metadata(&plan,&j->artifacts[r->artifact].descriptor,
                &j->nodes[i].participant.target,&j->nodes[i].participant.layout,j->gap_us)){
                uint64_t chunks=(plan.length+31u)/32u,blocks=(plan.length+255u)/256u;
                uint8_t n=passes(j,r->artifact);
                remaining+=plan.lead_ms+(uint64_t)blocks*plan.pause_ms*n+
                    chunks*ultra_frame_max_us(&j->line->timing,48)*n/1000u+
                    7000u+2u*(uint64_t)j->entry_ms+(1u+ULTRA_UPDATE_CONFIRM_COPIES)*(uint64_t)plan.finish_ms+j->options.verify_timeout_ms;
            }
        }
    }
    p->subtransfer=j->sessions;p->job_state=j->state;p->phase=j->transfer.state;p->passes=j->options.passes;
    in_transfer=j->state==ULTRA_JOB_TRANSFER || j->state==ULTRA_JOB_VERIFY || j->state==ULTRA_JOB_ABORTING;
    if(in_transfer){p->artifact=j->active_artifact;p->passes=j->transfer.passes;p->family=j->artifacts[p->artifact].descriptor.bytes[9];
        p->pass=j->transfer.pass<j->transfer.passes?(uint8_t)(j->transfer.pass+1u):j->transfer.passes;
        p->transfer_complete=j->transfer.transfer_complete;
        total=j->transfer.plan.length;p->bytes_sent=j->transfer.mode==ULTRA_UPDATE_SELECTED?j->transfer.committed:j->transfer.offset;
        if(p->bytes_sent>total)p->bytes_sent=total;
    }
    else if(j->artifact_prepared){p->artifact=j->active_artifact;p->passes=passes(j,p->artifact);
        p->family=j->artifacts[p->artifact].descriptor.bytes[9];p->subtransfer=(uint16_t)(j->sessions+1u);
        total=ultra_get32(j->artifacts[p->artifact].descriptor.bytes+20);}
    p->bytes_total=total;p->estimated_remaining_ms=j->state==ULTRA_JOB_DONE?0:remaining;
    return 0;
}
