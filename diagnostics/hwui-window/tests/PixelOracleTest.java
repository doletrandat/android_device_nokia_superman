package org.superman.diagnostics.hwui;
public class PixelOracleTest {
 public static void main(String[] args) {
  int checks=0;
  for(int shift=0;shift<32;shift+=8) for(int a=0;a<256;a++) for(int b=0;b<256;b++) {
   boolean want=Math.abs(a-b)<=2;
   if(PixelOracle.near(a<<shift,b<<shift,2)!=want) throw new AssertionError();
   checks++;
  }
  if(PixelOracle.near(0,0xff000000,2))throw new AssertionError("alpha");
  if(PixelOracle.near(0xff000000,0xffffffff,2))throw new AssertionError("missing patch");
  if(!PixelOracle.near(0xff2870b4,0xff2870b4,0))throw new AssertionError("same");
  for(int invalid:new int[]{-1,256}) {
   try {PixelOracle.near(0,0,invalid);throw new AssertionError("bounds");}
   catch(IllegalArgumentException good) {}
  }
  System.out.println("PASS comparisons="+checks+" plus alpha/missing/exact/tolerance bounds");
 }
}
