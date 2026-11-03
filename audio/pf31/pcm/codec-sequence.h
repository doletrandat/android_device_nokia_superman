/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUPERMAN_CODEC_SEQUENCE_H
#define SUPERMAN_CODEC_SEQUENCE_H
struct codec_step { unsigned short reg; unsigned char mask, value; unsigned short delay_us; };
struct codec_saved { unsigned short reg; unsigned char mask, value; };
#define CODEC_SAVED_MAX 40
static int codec_snapshot(struct regmap *map,const struct codec_step *steps,unsigned count,
                          struct codec_saved *saved,unsigned *saved_count)
{
 unsigned i,j,value;int ret;*saved_count=0;
 for(i=0;i<count;i++) {
  if(!steps[i].mask||(steps[i].reg<0x100&&steps[i].reg!=0x80&&steps[i].reg!=0x88&&steps[i].reg!=0&&steps[i].reg!=0xc1)||steps[i].reg>0x3ff)return -EINVAL;
  for(j=0;j<*saved_count;j++)if(saved[j].reg==steps[i].reg)break;
  if(j==*saved_count) {
   if(j==CODEC_SAVED_MAX)return -E2BIG;
   ret=regmap_read(map,steps[i].reg,&value);if(ret)return ret;
   saved[j]=(struct codec_saved){steps[i].reg,0,value};++*saved_count;
  }
  saved[j].mask|=steps[i].mask;
 }
 return 0;
}
static int codec_apply(struct regmap *map,const struct codec_step *steps,unsigned count)
{
 unsigned i,value;int ret;
 for(i=0;i<count;i++) {
  ret=regmap_update_bits(map,steps[i].reg,steps[i].mask,steps[i].value);if(ret)return ret;
  if(steps[i].delay_us)usleep_range(steps[i].delay_us,steps[i].delay_us+100);
  ret=regmap_read(map,steps[i].reg,&value);if(ret)return ret;
  if((value&steps[i].mask)!=(steps[i].value&steps[i].mask)) {
   pr_err("SUPERMAN_CODEC step=%u reg=%03x mask=%02x want=%02x read=%02x\n",i,steps[i].reg,steps[i].mask,steps[i].value,value);
   return -EIO;
  }
 }
 return 0;
}
static int codec_restore(struct regmap *map,const struct codec_saved *saved,unsigned count)
{
 unsigned value;int ret,first=0;
 while(count--) {
  ret=regmap_update_bits(map,saved[count].reg,saved[count].mask,saved[count].value);
  if(!ret)ret=regmap_read(map,saved[count].reg,&value);
  if(!ret&&(value&saved[count].mask)!=(saved[count].value&saved[count].mask))ret=-EIO;
  if(ret&&!first)first=ret;
 }
 return first;
}
#endif
