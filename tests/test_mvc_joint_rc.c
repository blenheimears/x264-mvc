/* GPL-2.0-or-later. Small deterministic combined-CPB/RD model test. */
#include "mvc/joint_rc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int write_stats( const char *path, int dep )
{
    FILE *f=fopen(path,"wb");
    if(!f) return -1;
    fputs("#options: synthetic mvc joint planning fixture\n",f);
    for(int i=0;i<12;i++)
    {
        /* A difficult cut at pair 5: the two eyes look very similar, so the
         * dependent cost stays low. Another cut at 9 needs both eyes. */
        int t=dep ? (i==9 ? 13000 : 180) : (i==5 ? 20000 : i==9 ? 15000 : 850);
        fprintf(f,"in:%d out:%d type:P dur:2 cpbdur:2 q:25.0 aq:25.0 tex:%d mv:90 misc:35;\n",i,i,t);
    }
    return fclose(f);
}
int main( void )
{
    const char *b="mvc_joint_base_test.log", *d="mvc_joint_dep_test.log";
    x264_mvc_joint_plan_t plan={0};
    x264_mvc_hrd_t hrd;
    int bad=1;
    if(write_stats(b,0) || write_stats(d,1)) goto done;
    if(x264_mvc_joint_plan(&plan,b,d,1000,500,24.,3000,4000)) goto done;
    if(plan.frames!=12 || plan.bitrate[0]<=plan.bitrate[1] || plan.bitrate[1]>500) goto done;
    int changed=0;
    for(int i=0;i<plan.frames;i++)
        if(!isfinite(plan.qscale[0][i]) || !isfinite(plan.qscale[1][i]) ||
           plan.qscale[0][i]<=0 || plan.qscale[1][i]<=0) goto done;
        else if(fabs(plan.qscale[0][i]-1.)>.005 || fabs(plan.qscale[1][i]-1.)>.005) changed++;
    /* Predictive headroom must be reserved *before* a difficult sequence;
     * ordinary greedy frame-by-frame buffering cannot see this burst. */
    double demand[] = { 7000, 7000, 7000, 18000, 18000, 18000, 5000 };
    double headroom[7] = {0};
    if( x264_mvc_predictive_limit(demand,headroom,7,4000.,26000.) ||
        !(headroom[0] > 10000.) || !(demand[1] < 7000.) ||
        !(demand[3] < 18000.) ) goto done;
    for(int j=0;j<7;j++)
        if(!isfinite(demand[j]) || demand[j]<0 || headroom[j]<0) goto done;
    if(!plan.future_headroom) goto done;
    if(changed<2 || x264_mvc_hrd_init(&hrd,100,10,24.) ||
       x264_mvc_hrd_commit(&hrd,50) || !x264_mvc_hrd_commit(&hrd,50000)) goto done;
    bad=0;
done:
    x264_mvc_joint_plan_clean(&plan);
    remove(b);remove(d);
    if(bad) fputs("MVC joint RD/CPB regression failed\n",stderr);
    return bad;
}
