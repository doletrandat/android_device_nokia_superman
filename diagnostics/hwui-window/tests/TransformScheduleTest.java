package org.superman.diagnostics.hwui;
public final class TransformScheduleTest {
 public static void main(String[] args) {
  int cases=0;
  for(int stage=0;stage<4;stage++) for(int frame=0;frame<96;frame++) for(int index=0;index<8;index++) {
   float[] p=TransformSchedule.pose(stage,frame,index,360,284);
   for(float v:p) if(!Float.isFinite(v))throw new AssertionError("finite");
   if(Math.abs(p[0])>116 || Math.abs(p[1])>137 || p[2]<.77 || p[2]>1.001 || p[3]<.599 || p[3]>1.001 || Math.abs(p[4])>8.001 || p[5]<-.001 || p[5]>128.001)throw new AssertionError("bounds");
   if(stage==3 && frame==95 && (p[0]!=0 || p[1]!=0 || p[2]!=1 || p[3]!=1 || p[4]!=0 || p[5]!=0))throw new AssertionError("identity endpoint");
   if(stage==0 && frame==95 && p[5]!=64)throw new AssertionError("scroll endpoint");
   if(stage==1 && frame==95 && (Math.abs(p[0])!=32 || p[2]!=.875f))throw new AssertionError("scale endpoint");
   cases++;
  }
  for(int[] bad:new int[][]{{-1,0,0,360,284},{4,0,0,360,284},{0,-1,0,360,284},{0,96,0,360,284},{0,0,8,360,284},{0,0,0,0,284}}) {
   try{TransformSchedule.pose(bad[0],bad[1],bad[2],bad[3],bad[4]);throw new AssertionError("argument accepted");}catch(IllegalArgumentException good){}
  }
  System.out.println("PASS schedule cases="+cases+" invalid inputs=6");
 }
}
