package org.llcast;

import android.app.Activity;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.OsConstants;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.List;

/** Normal mode: hardware MediaCodec, or dav1d AV1 plus GLES, into this SurfaceView. No root. */
public final class ReceiveActivity extends Activity implements SurfaceHolder.Callback, Native.VideoSize {
    static final String HEVC = "hevc", HEVC_LOW_LATENCY = "hevcLowLatency";
    static final String AVC = "avc", AVC_LOW_LATENCY = "avcLowLatency";
    static final String AV1 = "av1", AV1_LOW_LATENCY = "av1LowLatency";
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final long[] status = new long[5];
    private FrameLayout screen;
    private SurfaceView video;
    private View waiting;
    private TextView unsupported, address, port, moved;
    private long receiver;
    private int videoWidth, videoHeight;
    private boolean failed;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        screen = new FrameLayout(this);
        screen.setBackgroundColor(Color.BLACK);
        video = new SurfaceView(this);
        video.getHolder().addCallback(this);
        screen.addView(video, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER));
        screen.addView(waitingScreen(), new FrameLayout.LayoutParams(-1, -1));
        showAddress(MainActivity.PORT);
        screen.addOnLayoutChangeListener((view, l, t, r, b, ol, ot, or, ob) -> place());
        setContentView(screen);
        fullScreen();
    }

    private void fullScreen() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        Surface surface = holder.getSurface();
        if (Build.VERSION.SDK_INT >= 30) surface.setFrameRate(60f, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        // The usual port, or the next free one of the following nine; the overlay says which.
        int[] error = new int[1];
        int port = MainActivity.PORT;
        for (; port < MainActivity.PORT + 10; ++port) {
            receiver = Native.receiverOpen(surface, port,
                getIntent().getStringExtra(HEVC), getIntent().getBooleanExtra(HEVC_LOW_LATENCY, false),
                getIntent().getStringExtra(AVC), getIntent().getBooleanExtra(AVC_LOW_LATENCY, false),
            getIntent().getStringExtra(AV1), getIntent().getBooleanExtra(AV1_LOW_LATENCY, false), this, error);
            if (receiver != 0 || error[0] != OsConstants.EADDRINUSE) break;
        }
        if (receiver != 0) showAddress(port);
        if (receiver == 0) {
            boolean busy = error[0] == OsConstants.EADDRINUSE;
            Ui.problem(this, busy ? getString(R.string.port_busy, MainActivity.PORT) : getString(R.string.receive_failed),
                getString(busy ? R.string.port_busy_advice : R.string.receive_failed_advice), this::finish);
            return;
        }
        handler.post(this::poll);
    }

    /** Centered: what to enter on the sender, large enough to read across a room. Scrolls on a
     *  short landscape phone instead of clipping. */
    private View waitingScreen() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER);
        int pad = Ui.dp(this, 24);
        column.setPadding(pad, pad, pad, pad);
        ImageView icon = new ImageView(this);
        icon.setImageResource(R.drawable.ic_receive);
        column.addView(icon, new LinearLayout.LayoutParams(Ui.dp(this, 64), Ui.dp(this, 64)));
        TextView title = centered(Ui.bold(this, getString(R.string.overlay_waiting), 30));
        title.setPadding(0, Ui.dp(this, 12), 0, Ui.dp(this, 24));
        column.addView(title);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER);
        panel.setBackground(Ui.panel(this));
        panel.setPadding(Ui.dp(this, 48), Ui.dp(this, 20), Ui.dp(this, 48), Ui.dp(this, 24));
        panel.addView(centered(Ui.muted(this, getString(R.string.overlay_enter), 18)));
        address = centered(Ui.bold(this, "", 44));
        panel.addView(address);
        port = centered(Ui.text(this, "", 24));
        port.setTextColor(Ui.ACCENT);
        panel.addView(port);
        column.addView(panel);
        moved = centered(Ui.muted(this, "", 16));
        moved.setPadding(0, Ui.dp(this, 16), 0, 0);
        column.addView(moved);
        TextView refresh = Ui.refreshWarning(this, 16);
        if (refresh != null) {
            centered(refresh).setPadding(0, Ui.dp(this, 16), 0, 0);
            column.addView(refresh);
        }
        unsupported = centered(Ui.text(this, "", 18));
        unsupported.setTextColor(Color.rgb(255, 168, 155));
        unsupported.setPadding(0, Ui.dp(this, 16), 0, 0);
        column.addView(unsupported);
        TextView stop = centered(Ui.muted(this, getString(R.string.overlay_stop), 16));
        stop.setPadding(0, Ui.dp(this, 32), 0, 0);
        column.addView(stop);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Ui.BACKGROUND);
        scroll.addView(column, new ScrollView.LayoutParams(-1, -1));
        waiting = scroll;
        return scroll;
    }

    private static TextView centered(TextView view) {
        view.setGravity(Gravity.CENTER);
        return view;
    }

    private void showAddress(int port) {
        List<String> addresses = Ui.addresses();
        address.setText(addresses.isEmpty() ? "—" : String.join("\n", addresses));
        this.port.setText(getString(R.string.overlay_port, port));
        boolean usual = port == MainActivity.PORT;
        moved.setText(usual ? "" : getString(R.string.overlay_port_moved, MainActivity.PORT, port));
        moved.setVisibility(usual ? View.GONE : View.VISIBLE);
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override public void surfaceDestroyed(SurfaceHolder holder) { close(); }

    private void close() {
        handler.removeCallbacksAndMessages(null);
        if (receiver != 0) Native.receiverClose(receiver);
        receiver = 0;
    }

    private void poll() {
        if (receiver == 0) return;
        Native.receiverStatus(receiver, status);
        boolean showing = status[0] != 0 && status[2] > 0;
        waiting.setVisibility(showing ? View.GONE : View.VISIBLE);
        // LLS_CODEC_HEVC 1, LLS_CODEC_H264 2, LLS_CODEC_AV1 3
        String codec = status[4] == 1 ? "HEVC" : status[4] == 2 ? "H.264" : status[4] == 3 ? "AV1" : null;
        unsupported.setText(codec == null ? "" : getString(R.string.overlay_unsupported, codec));
        if (status[3] != 0 && !failed) {
            failed = true;
            close();
            Ui.problem(this, getString(R.string.decoder_failed), getString(R.string.decoder_failed_advice), this::finish);
            return;
        }
        handler.postDelayed(this::poll, 250);
    }

    /** From the decoder's output thread. */
    @Override public void onVideoSize(int width, int height) {
        runOnUiThread(() -> {
            videoWidth = width;
            videoHeight = height;
            place();
        });
    }

    /** As on the box and the Mac: pixel for pixel when the picture fits and is within 10% of
     *  the screen in both dimensions; otherwise fitted with its aspect ratio, never cropped. */
    private void place() {
        int w = videoWidth, h = videoHeight, W = screen.getWidth(), H = screen.getHeight();
        if (w <= 0 || h <= 0 || W <= 0 || H <= 0) return;
        boolean near = w <= W && h <= H && w * 10 >= W * 9 && h * 10 >= H * 9;
        if (!near) {
            double scale = Math.min((double) W / w, (double) H / h);
            w = (int) Math.round(w * scale);
            h = (int) Math.round(h * scale);
        }
        FrameLayout.LayoutParams layout = (FrameLayout.LayoutParams) video.getLayoutParams();
        if (layout.width == w && layout.height == h) return;
        video.setLayoutParams(new FrameLayout.LayoutParams(w, h, Gravity.CENTER));
    }

    @Override public void onStop() {
        super.onStop();
        // Receiving only while visible; a new start comes from the Receive page.
        close();
        finish();
    }
}
