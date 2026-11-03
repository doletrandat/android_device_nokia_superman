package org.superman.diagnostics.hwui;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.graphics.*;
import android.util.Log;
import android.view.*;
import java.io.*;
import org.json.*;

/** Standalone, finite HWUI window experiment. Never selected by a product. */
public final class WindowTest extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final JSONArray samples = new JSONArray();
    private Scene scene;
    private String token;
    private File directory;
    private int frame, sample;
    private boolean stopped;
    private long start;
    private boolean transforms, readbacks = true;
    private boolean direct;
    private Surface ownSurface;
    private JSONObject cpuBefore;
    private int transformFrame;
    private final JSONArray frameTimes = new JSONArray();
    private final Choreographer.FrameCallback animation = this::animate;
    private static final int BG = 0xff203040;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        token = getIntent().getStringExtra("token");
        if (token == null || !token.matches("[0-9a-f]{16}")) { finish(); return; }
        String mode = getIntent().getStringExtra("mode");
        if (mode != null && !mode.equals("transforms")) { finish(); return; }
        transforms = "transforms".equals(mode);
        readbacks = getIntent().getBooleanExtra("readbacks", true);
        direct = getIntent().getBooleanExtra("direct", false);
        directory = new File(getFilesDir(), token);
        if (!directory.mkdir()) { finish(); return; } // No overwrite/reuse.
        start = SystemClock.elapsedRealtime();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        scene = new Scene();
        setContentView(scene);
        handler.postDelayed(() -> stop("deadline"), 55000);
        scene.postDelayed(transforms ? this::beginStage : this::step, 1000);
        Log.i("SupermanHWUI", "BEGIN token=" + token + " readback=after-settle only");
    }

    private void beginStage() {
        if (stopped) return;
        if (scene.getWidth() < 320 || scene.getHeight() < 600 ||
                scene.getWidth() > 1080 || scene.getHeight() > 1920 ||
                !scene.isHardwareAccelerated()) { stop("unsupported-window"); return; }
        transformFrame=0;
        scene.cells[0].phase=sample%3; scene.cells[0].invalidate();
        Log.i("SupermanHWUI","ANIMATION_BEGIN token="+token+" stage="+sample+" readbacks="+readbacks);
        Choreographer.getInstance().postFrameCallback(animation);
    }

    private void animate(long frameTimeNanos) {
        if (stopped) return;
        frameTimes.put(frameTimeNanos);
        for (Cell cell:scene.cells) {
            float[] p=TransformSchedule.pose(sample,transformFrame,cell.index,cell.getWidth(),cell.getHeight());
            cell.setTranslationX(p[0]); cell.setTranslationY(p[1]);
            cell.setScaleX(p[2]); cell.setScaleY(p[2]);
            cell.setAlpha(p[3]); cell.setRotation(p[4]);
            if (cell.index==0) scene.scrollTo(0,Math.round(p[5]));
        }
        if (++transformFrame == TransformSchedule.FRAMES) {
            Log.i("SupermanHWUI","ANIMATION_END token="+token+" stage="+sample);
            handler.postDelayed(this::capture,750);
        } else Choreographer.getInstance().postFrameCallback(animation);
    }

    private JSONObject geometry() throws JSONException {
        JSONArray children=new JSONArray();
        for (Cell cell:scene.cells) {
            float[] values=new float[9]; cell.getMatrix().getValues(values);
            JSONArray matrix=new JSONArray(); for(float value:values) matrix.put(value);
            children.put(new JSONObject().put("index",cell.index).put("drawCount",cell.drawCount)
                    .put("phase",cell.phase).put("recordedPhase",cell.lastPhase)
                    .put("left",cell.getLeft()).put("top",cell.getTop())
                    .put("width",cell.getWidth()).put("height",cell.getHeight())
                    .put("alpha",cell.getAlpha()).put("matrix",matrix));
        }
        return new JSONObject().put("stage",sample).put("scrollY",scene.getScrollY())
                .put("children",children).put("callbackCount",frameTimes.length());
    }

    private void nextSample() {
        if (++sample == 4) stop("complete");
        else handler.postDelayed(transforms ? this::beginStage : this::step,500);
    }

    private void step() {
        if (stopped) return;
        if (scene.getWidth() < 320 || scene.getHeight() < 600 ||
                scene.getWidth() > 1080 || scene.getHeight() > 1920 ||
                !scene.isHardwareAccelerated()) { stop("unsupported-window"); return; }
        // Samples 0/1 re-record every child; samples 2/3 retain seven child nodes.
        scene.cells[0].phase = (frame + sample) % 3;
        if (sample < 2) for (Cell cell : scene.cells) cell.invalidate();
        else scene.cells[0].invalidate();
        Log.i("SupermanHWUI", "DRAW token=" + token + " sample=" + sample +
                " frame=" + frame + " elapsed=" + (SystemClock.elapsedRealtime() - start));
        if (++frame % 12 == 0) handler.postDelayed(this::capture, 750);
        else handler.postDelayed(this::step, 150);
    }

    private void capture() {
        if (stopped) return;
        if (!readbacks) {
            try { samples.put(geometry().put("readback",false)); writeReport("running"); nextSample(); }
            catch(Exception e) { Log.e("SupermanHWUI","metadata",e); stop("metadata-error"); }
            return;
        }
        int[] pos = new int[2]; scene.getLocationInWindow(pos);
        final int w = scene.getWidth(), h = scene.getHeight(), current = sample;
        if (direct) {
            try {
                // Access only this diagnostic's own root; fail closed if hidden API access is denied.
                Object root=View.class.getDeclaredMethod("getViewRootImpl").invoke(getWindow().getDecorView());
                ownSurface=(Surface)root.getClass().getField("mSurface").get(root);
                cpuBefore=new JSONObject(DirectBuffer.capture(ownSurface,
                        new File(directory,"sample-"+current+"-cpu-before.raw").getPath()));
                cpuBefore.put("cropX",pos[0]).put("cropY",pos[1]);
                if(cpuBefore.getInt("rc")!=0) {
                    samples.put(new JSONObject().put("sample",current).put("cpuBefore",cpuBefore));
                    stop("direct-before-failed");return;
                }
            } catch(Throwable e) {
                Log.e("SupermanHWUI","direct access",e);stop("direct-access-failed");return;
            }
        }
        final Bitmap actual = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        final long requested = SystemClock.elapsedRealtime();
        Log.i("SupermanHWUI", "READBACK_BEGIN token=" + token + " sample=" + current);
        try {
            PixelCopy.request(getWindow(), new Rect(pos[0], pos[1], pos[0]+w, pos[1]+h),
                    actual, result -> {
                if (stopped) { actual.recycle(); return; }
                try {
                    JSONObject item = new JSONObject();
                    if (direct) {
                        JSONObject cpuAfter=new JSONObject(DirectBuffer.capture(ownSurface,
                                new File(directory,"sample-"+current+"-cpu-after.raw").getPath()));
                        item.put("cpuBefore",cpuBefore).put("cpuAfter",cpuAfter);
                        if(cpuAfter.getInt("rc")!=0) {
                            samples.put(item);actual.recycle();stop("direct-after-failed");return;
                        }
                    }
                    item.put("sample", current).put("fullChildInvalidation", !transforms && current < 2)
                        .put("geometry",geometry())
                        .put("pixelCopyResult", result).put("phase", scene.cells[0].phase)
                        .put("width", w).put("height", h)
                        .put("requestElapsedMs", requested-start)
                        .put("callbackElapsedMs", SystemClock.elapsedRealtime()-start);
                    JSONArray drawCounts = new JSONArray(), recordedPhases = new JSONArray();
                    boolean recorded = true;
                    for (Cell cell : scene.cells) {
                        drawCounts.put(cell.drawCount); recordedPhases.put(cell.lastPhase);
                        recorded &= cell.drawCount > 0 && cell.lastPhase == cell.phase;
                    }
                    item.put("childDrawCounts",drawCounts).put("recordedPhases",recordedPhases)
                        .put("expectedPhasesRecorded",recorded);
                    if (result == PixelCopy.SUCCESS) {
                        Bitmap reference = Bitmap.createBitmap(w,h,Bitmap.Config.ARGB_8888);
                        Canvas canvas = new Canvas(reference); canvas.drawColor(BG);
                        for (Cell cell : scene.cells) {
                            canvas.save();
                            canvas.translate(cell.getLeft()-scene.getScrollX(),cell.getTop()-scene.getScrollY());
                            canvas.concat(cell.getMatrix());
                            canvas.clipRect(0,0,cell.getWidth(),cell.getHeight());
                            int layer=canvas.saveLayerAlpha(0,0,cell.getWidth(),cell.getHeight(),Math.round(cell.getAlpha()*255));
                            paintCell(canvas, cell.getWidth(), cell.getHeight(), cell.index, cell.phase);
                            canvas.restoreToCount(layer);
                            canvas.restore();
                        }
                        int[] a = new int[w*h], b = new int[w*h];
                        actual.getPixels(a,0,w,0,0,w,h); reference.getPixels(b,0,w,0,0,w,h);
                        long exactDifferent=0, patchDifferent=0, patchPixels=0;
                        for (int i=0; i<a.length; i++) if (a[i]!=b[i]) exactDifferent++;
                        // Independent uniform-color oracle, away from rasterized edges/text.
                        for (Cell cell : scene.cells) {
                            if (transforms && current != 3) continue;
                            int expected = swatch(cell.index, cell.phase);
                            for (int y=cell.getTop()+16; y<cell.getTop()+32; y++)
                                for (int x=cell.getLeft()+16; x<cell.getLeft()+64; x++) {
                                    patchPixels++;
                                    if (!PixelOracle.near(a[y*w+x],expected,2)) patchDifferent++;
                                }
                        }
                        item.put("softwareReferenceDifferentPixels",exactDifferent)
                            .put("uniformPatchOracleApplicable",!transforms || current==3)
                            .put("uniformPatchPixels",patchPixels)
                            .put("uniformPatchMismatches",patchDifferent);
                        save(actual,"sample-"+current+"-window.png");
                        save(reference,"sample-"+current+"-software.png"); reference.recycle();
                    }
                    samples.put(item); writeReport("running");
                    Log.i("SupermanHWUI", "READBACK_END token="+token+" "+item.toString());
                    actual.recycle();
                    nextSample();
                } catch (Exception e) { actual.recycle(); Log.e("SupermanHWUI","capture",e); stop("capture-error"); }
            }, handler);
        } catch (Exception e) { actual.recycle(); Log.e("SupermanHWUI","request",e); stop("request-error"); }
    }

    private void save(Bitmap bitmap, String name) throws IOException {
        try (FileOutputStream out = new FileOutputStream(new File(directory,name))) {
            if (!bitmap.compress(Bitmap.CompressFormat.PNG,100,out)) throw new IOException("PNG");
            out.getFD().sync();
        }
    }
    private void writeReport(String status) throws Exception {
        JSONObject report = new JSONObject().put("token",token).put("status",status)
                .put("mode",transforms ? "transforms" : "baseline").put("readbacks",readbacks)
                .put("direct",direct)
                .put("callbackTimesNs",frameTimes)
                .put("elapsedMs",SystemClock.elapsedRealtime()-start).put("samples",samples)
                .put("scope","app-window PixelCopy; excludes SurfaceFlinger composition and physical scanout")
                .put("softwareDifference","informational: GPU/software AA and font rendering can differ");
        try (FileOutputStream out = new FileOutputStream(new File(directory,"report.json"))) {
            out.write(report.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8)); out.getFD().sync();
        }
    }
    private void stop(String reason) {
        if (stopped) return;
        stopped=true; handler.removeCallbacksAndMessages(null);
        Choreographer.getInstance().removeFrameCallback(animation);
        try { if (directory != null) writeReport(reason); } catch (Exception e) { Log.e("SupermanHWUI","report",e); }
        Log.i("SupermanHWUI","END token="+token+" reason="+reason); finish();
    }
    @Override protected void onPause() { super.onPause(); if (!stopped) stop("paused"); }

    private final class Scene extends ViewGroup {
        final Cell[] cells = new Cell[8];
        Scene() { super(WindowTest.this); setBackgroundColor(BG);
            for(int i=0;i<cells.length;i++) { cells[i]=new Cell(i); addView(cells[i]); }
        }
        @Override protected void onMeasure(int ws,int hs) {
            int w=MeasureSpec.getSize(ws),h=MeasureSpec.getSize(hs); setMeasuredDimension(w,h);
            for(Cell cell:cells) cell.measure(MeasureSpec.makeMeasureSpec(w/2,MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(h/4,MeasureSpec.EXACTLY));
        }
        @Override protected void onLayout(boolean changed,int l,int t,int r,int b) {
            for(Cell cell:cells) { int x=(cell.index%2)*getWidth()/2,y=(cell.index/2)*getHeight()/4;
                cell.layout(x,y,x+getWidth()/2,y+getHeight()/4); }
        }
    }
    private final class Cell extends View {
        final int index; int phase, drawCount, lastPhase = -1;
        Cell(int index) { super(WindowTest.this); this.index=index; }
        @Override protected void onDraw(Canvas canvas) {
            drawCount++; lastPhase=phase;
            paintCell(canvas,getWidth(),getHeight(),index,phase);
        }
    }
    private static int swatch(int index,int phase) { return Color.rgb(40+index*20,70+phase*50,180-index*12); }
    private static void paintCell(Canvas c,int w,int h,int index,int phase) {
        Paint p=new Paint(); c.drawColor(0xffeeeeee); p.setColor(swatch(index,phase));
        c.drawRect(8,8,80,40,p);
        p.setAntiAlias(true); p.setColor(0xff102030); p.setTextSize(22);
        c.drawText("Row "+index+" phase "+phase,12,74,p);
        c.drawText("Aa 019 glyphs",12,104,p);
        c.save(); Path clip=new Path(); clip.addRoundRect(new RectF(12,116,w-12,h-12),18,18,Path.Direction.CW);
        c.clipPath(clip); c.saveLayerAlpha(12,116,w-12,h-12,160);
        p.setColor(swatch(index,phase)); c.drawRect(8,110,w-8,h-8,p);
        p.setColor(0xff802040); c.drawCircle(w/2f,h-40,34,p); c.restore(); c.restore();
    }
}
