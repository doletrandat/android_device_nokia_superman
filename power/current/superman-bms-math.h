/* SPDX-License-Identifier: GPL-2.0-only */
#include "superman-bms-profile.h"
static s64 gauge_signed36(u64 raw)
{
 raw&=(1ULL<<36)-1;
 return raw&(1ULL<<35)?(s64)(raw-(1ULL<<36)):(s64)raw;
}
static int gauge_charge_uah(s64 cc,unsigned gain,unsigned offset,unsigned rsense,s64 *uah)
{
 s64 uv,pvh;
 if(cc<-(1LL<<35)||cc>=(1LL<<35)||gain>65535||gain<=offset||gain-offset<1000||gain-offset>10000||rsense<1000||rsense>50000)return -ERANGE;
 uv=div_s64(cc*542535,100000);
 uv=div_s64(uv*3291,gain-offset);
 pvh=div_s64(uv*56*100000,32764*3600)*10;
 *uah=div_s64(pvh,rsense);return 0;
}
static int gauge_ocv_uv(unsigned raw,int ref625,int ref1250,int *uv)
{
 if(raw<=0x6000||raw>=65535||ref625<=0x6000||ref1250>=65535||ref1250-ref625<1000||ref1250-ref625>20000)return -ERANGE;
 *uv=3*(625000+div_s64(((s64)raw-ref625)*625000,ref1250-ref625));
 return *uv>=2500000&&*uv<=4500000?0:-ERANGE;
}
/* Percent in thousandths, interpolated across both measured temperature columns. */
static int gauge_ocv_pc(int uv,int temp,int *pc)
{
 unsigned hi=0,lo,i;int a,b,last=0;
 if(temp<gauge_temperatures[0]||temp>gauge_temperatures[ARRAY_SIZE(gauge_temperatures)-1]||uv<2500000||uv>4500000)return -ERANGE;
 while(gauge_temperatures[hi]<temp)hi++;
 lo=(gauge_temperatures[hi]==temp)?hi:hi-1;
 for(i=0;i<ARRAY_SIZE(gauge_profile);i++) {
  a=gauge_profile[i][lo+1]*1000;b=gauge_profile[i][hi+1]*1000;
  if(lo!=hi)a+=div_s64((s64)(b-a)*(temp-gauge_temperatures[lo]),gauge_temperatures[hi]-gauge_temperatures[lo]);
  if(i&&a>=last)return -EINVAL;
  if(uv>=a) {
   if(!i){*pc=100000;return 0;}
   *pc=gauge_profile[i][0]*1000+div_s64((s64)(uv-a)*(gauge_profile[i-1][0]-gauge_profile[i][0])*1000,last-a);return 0;
  }
  last=a;
 }
 *pc=0;return 0;
}
