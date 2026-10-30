package org.llcast;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.Display;

/** Sends the screen while LLCast is in the background: MediaProjection mirrors the display into the
 *  encoder's input surface. A notification with Stop stays while it runs. */
public final class SendService extends Service {
    static final String STOP = "org.llcast.STOP";
    static final String RESULT = "result", DATA = "data", HOST = "host", PORT = "port", CODEC = "codec",
        TYPE = "type", WIDTH = "modeWidth", HEIGHT = "modeHeight", FPS = "fps", BITRATE = "bitrate", LABEL = "label";
    private static final String CHANNEL = "sending";

    /** The running service, for the Send page's status line; null when stopped. */
    static volatile SendService running;
    /** Why the last stream stopped by itself (headline, advice), once, for the Send page. */
    static volatile String[] problem;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final long[] status = new long[18];
    private MediaProjection projection;
    private VirtualDisplay display;
    private DisplayManager displays;
    private DisplayManager.DisplayListener rotation;
    private long sender;
    private String host, type, encoder, label;
    private int port, codec, modeWidth, modeHeight, fps, bitrate, width, height;
    private long lastFrames, lastBytes, lastPoll;
    private String rate = "";
    private String reception;

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int id) {
        if (intent == null || STOP.equals(intent.getAction()) || running != null) {
            if (intent != null && STOP.equals(intent.getAction())) stopSelf();
            return START_NOT_STICKY;
        }
        host = intent.getStringExtra(HOST);
        port = intent.getIntExtra(PORT, MainActivity.PORT);
        codec = intent.getIntExtra(CODEC, Codecs.LLS_HEVC);
        type = intent.getStringExtra(TYPE);
        modeWidth = intent.getIntExtra(WIDTH, 1920);
        modeHeight = intent.getIntExtra(HEIGHT, 1080);
        fps = intent.getIntExtra(FPS, 60);
        bitrate = intent.getIntExtra(BITRATE, 20000000); // video + FEC budget
        label = intent.getStringExtra(LABEL);
        // The foreground service comes before the projection, as Android 14 requires.
        if (Build.VERSION.SDK_INT >= 29)
            startForeground(1, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        else
            startForeground(1, notification());
        MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
        projection = manager.getMediaProjection(intent.getIntExtra(RESULT, 0), intent.getParcelableExtra(DATA));
        if (projection == null) { fail(R.string.send_failed, R.string.send_failed_advice); return START_NOT_STICKY; }
        projection.registerCallback(new MediaProjection.Callback() {
            @Override public void onStop() { stopSelf(); }
        }, handler);
        displays = getSystemService(DisplayManager.class);
        if (!open()) { fail(R.string.send_failed, R.string.send_failed_advice); return START_NOT_STICKY; }
        DisplayMetrics metrics = metrics();
        display = projection.createVirtualDisplay("LLCast", width, height, metrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, Native.senderSurface(sender), null, handler);
        // Rotation: a new size means a new encoder and session; the virtual display only resizes
        // (Android 14 allows one virtual display per projection).
        rotation = new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int displayId) {}
            @Override public void onDisplayRemoved(int displayId) {}
            @Override public void onDisplayChanged(int displayId) { if (displayId == Display.DEFAULT_DISPLAY) resize(); }
        };
        displays.registerDisplayListener(rotation, handler);
        running = this;
        handler.post(this::watch);
        return START_NOT_STICKY;
    }

    private DisplayMetrics metrics() {
        DisplayMetrics metrics = new DisplayMetrics();
        displays.getDisplay(Display.DEFAULT_DISPLAY).getRealMetrics(metrics);
        return metrics;
    }

    /** The encoder and session for the screen's current size. */
    private boolean open() {
        DisplayMetrics metrics = metrics();
        Codecs.Encoder found = Codecs.encoder(type, metrics.widthPixels, metrics.heightPixels, modeWidth, modeHeight, fps);
        if (found == null) return false;
        encoder = found.name;
        width = found.width;
        height = found.height;
        sender = Native.senderOpen(host, port, codec, encoder, width, height, fps, bitrate);
        if (sender != 0) label = getString(R.string.profile_label, width, height, fps) + " · " +
            getString(R.string.budget_label, MainActivity.mbps(bitrate));
        return sender != 0;
    }

    private void resize() {
        DisplayMetrics metrics = metrics();
        Codecs.Encoder found = Codecs.encoder(type, metrics.widthPixels, metrics.heightPixels, modeWidth, modeHeight, fps);
        if (found == null || (found.width == width && found.height == height) || display == null) return;
        display.setSurface(null);
        Native.senderClose(sender);
        sender = 0;
        if (!open()) { fail(R.string.send_failed, R.string.send_failed_advice); return; }
        display.resize(width, height, metrics.densityDpi);
        display.setSurface(Native.senderSurface(sender));
    }

    private void watch() {
        if (sender == 0) return;
        Native.senderStatus(sender, status);
        long now = System.nanoTime();
        if (lastPoll != 0) {
            double seconds = (now - lastPoll) / 1e9;
            rate = getString(R.string.send_rate, Math.round((status[0] - lastFrames) / seconds),
                (status[1] - lastBytes) * 8 / seconds / 1e6,
                status[2] < 0 ? "—" : String.format(java.util.Locale.ROOT, "%.1f ms", status[2] / 1000.0));
        }
        reception = status[5] == 0 ? null : reception();
        lastPoll = now;
        lastFrames = status[0];
        lastBytes = status[1];
        if (status[3] != 0) { fail(R.string.send_failed, R.string.send_failed_advice); return; }
        if (status[4] != 0 && (status[4] & (1L << codec)) == 0) {
            problem = new String[]{getString(R.string.peer_codec, codecName()), getString(R.string.peer_codec_advice)};
            stopSelf();
            return;
        }
        handler.postDelayed(this::watch, 1000);
    }

    String codecName() { return MainActivity.codecName(codec); }

    /** Reception over the last 5 s, including clean-frame capture-to-display
     *  minimum and median, then the doctor's hint. */
    private String reception() {
        int[] levels = {R.string.reception_good, R.string.reception_fair, R.string.reception_poor};
        StringBuilder text = new StringBuilder(getString(R.string.send_reception,
            getString(levels[(int) Math.min(Math.max(status[5], 1), 3) - 1]), Math.round(status[6] / 100.0),
            status[7] / 100.0, status[11], status[8] / 100.0));
        if (status[10] > 0) text.append(getString(R.string.send_reception_repaired, status[10]));
        if (status[9] > 0) text.append(getString(status[17] != 0 ? R.string.send_reception_latency_estimated
            : R.string.send_reception_latency, status[16], status[9]));
        String budget = MainActivity.mbps((int) status[13] * 100000);
        long shown = Math.round(status[6] / 100.0), sent = Math.round(status[14] / 100.0);
        // LLS_HINT_*: 1 lower budget, 2 late, 3 receiver slow, 4 decode errors, 5 sender slow
        String hint = status[12] == 1 ? getString(R.string.hint_budget, budget)
            : status[12] == 2 ? getString(R.string.hint_late, budget)
            : status[12] == 3 ? getString(R.string.hint_receiver_slow, shown, sent)
            : status[12] == 4 ? getString(R.string.hint_decode_errors)
            : status[12] == 5 ? getString(R.string.hint_sender_slow, sent, status[15]) : null;
        if (hint != null) text.append('\n').append(hint);
        return text.toString();
    }

    /** "Sending to … · 1080p60 · HEVC · 60 fps · 28.1 Mbps · reply 3.2 ms", then the reception line. */
    String describe() {
        String sending = getString(R.string.send_status, host, label, codecName(), rate);
        return reception == null ? sending : sending + "\n" + reception;
    }

    private void fail(int headline, int advice) {
        problem = new String[]{getString(headline), getString(advice)};
        stopSelf();
    }

    private Notification notification() {
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.notify_channel),
            NotificationManager.IMPORTANCE_LOW));
        PendingIntent stop = PendingIntent.getService(this, 0, new Intent(this, SendService.class).setAction(STOP),
            PendingIntent.FLAG_IMMUTABLE);
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
            PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_send)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.notify_text, host))
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(new Notification.Action.Builder(null, getString(R.string.stop), stop).build())
            .build();
    }

    @Override public void onDestroy() {
        running = null;
        handler.removeCallbacksAndMessages(null);
        if (rotation != null) displays.unregisterDisplayListener(rotation);
        if (display != null) display.release();
        if (sender != 0) Native.senderClose(sender);
        sender = 0;
        if (projection != null) projection.stop();
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }
}
