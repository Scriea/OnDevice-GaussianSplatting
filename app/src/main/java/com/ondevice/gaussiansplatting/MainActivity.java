package com.ondevice.gaussiansplatting;

import android.app.Activity;
import android.os.Bundle;
import android.view.Choreographer;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

public class MainActivity extends Activity {

    static {
        System.loadLibrary("gaussiansplatting");
    }

    private SurfaceView surfaceView;
    private boolean surfaceReady = false;

    private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            if (surfaceReady) {
                nativeRender();
            }
            Choreographer.getInstance().postFrameCallback(this);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        surfaceView = new SurfaceView(this);
        setContentView(surfaceView);

        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Surface surface = holder.getSurface();
                nativeOnSurfaceCreated(surface);
                surfaceReady = true;
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                nativeOnSurfaceChanged(width, height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                surfaceReady = false;
                nativeOnSurfaceDestroyed();
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        Choreographer.getInstance().postFrameCallback(frameCallback);
    }

    @Override
    protected void onPause() {
        Choreographer.getInstance().removeFrameCallback(frameCallback);
        super.onPause();
    }

    public native void nativeOnSurfaceCreated(Surface surface);
    public native void nativeOnSurfaceChanged(int width, int height);
    public native void nativeOnSurfaceDestroyed();
    public native void nativeRender();
}