package org.llcast;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public final class BoxActivity extends Activity {
    // Only what the box itself must allow; the display and remote are checked when the receiver starts.
    private static final String[] KEYS = {"root", "decoder"};
    private static final int[] LABELS = {R.string.root, R.string.decoder};
    private static final int[] OK = {R.string.root_ok, R.string.decoder_ok};
    private static final int[] FAIL = {R.string.root_fail, R.string.decoder_fail};
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final TextView[] rows = new TextView[KEYS.length];
    private final boolean[] complete = new boolean[KEYS.length];
    private TextView status, address;
    private Button action;
    private volatile boolean destroyed;
    private boolean ready;
    private String failCheck;
    private String su;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        failCheck = getIntent().getStringExtra("llcast.fail"); // Forces failure only; never bypasses a check.
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setPadding(dp(40), dp(20), dp(40), dp(16));
        screen.setBackgroundColor(Color.rgb(16, 28, 44));
        TextView title = text(getString(R.string.app_name), 32);
        screen.addView(title);
        screen.addView(text(getString(R.string.intro), 18));
        ScrollView scroll = new ScrollView(this);
        LinearLayout checks = new LinearLayout(this);
        checks.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < rows.length; ++i) {
            rows[i] = text(getString(R.string.row, "…", getString(LABELS[i]), getString(R.string.checking)), 17);
            rows[i].setPadding(0, dp(4), 0, dp(4));
            checks.addView(rows[i]);
        }
        scroll.addView(checks);
        screen.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        address = text("", 19);
        screen.addView(address);
        TextView refresh = Ui.refreshWarning(this, 17); // the HDMI waiting card repeats it
        if (refresh != null) screen.addView(refresh);
        status = text(getString(R.string.root_prompt), 17);
        screen.addView(status);
        action = Ui.button(this, "");
        action.setText(R.string.checking);
        action.setEnabled(false);
        action.setMinWidth(dp(170));
        LinearLayout.LayoutParams button = new LinearLayout.LayoutParams(-2, -2);
        button.gravity = Gravity.END;
        screen.addView(action, button);
        action.setOnClickListener(view -> {
            if (!ready) { finish(); return; }
            action.setEnabled(false);
            action.setText(R.string.starting);
            status.setText(R.string.starting);
            worker.execute(this::startReceiver);
        });
        setContentView(screen);
        worker.execute(this::checks);
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private TextView text(String value, int size) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.WHITE);
        return view;
    }
    private void ui(Runnable task) { if (!destroyed) runOnUiThread(() -> { if (!destroyed) task.run(); }); }

    private boolean report(int index, boolean pass, int failure) { return report(index, pass, failure, null); }

    /** reason: what the box said, shown under a failure so it can be reported. */
    private boolean report(int index, boolean pass, int failure, String reason) {
        boolean forced = KEYS[index].equals(failCheck);
        boolean result = pass && !forced;
        complete[index] = true;
        ui(() -> {
            String detail = getString(result ? OK[index] : failure);
            if (!result && reason != null && !reason.isEmpty()) detail += "\n" + getString(R.string.box_said, reason);
            rows[index].setText(getString(R.string.row, result ? "✓" : "✗", getString(LABELS[index]), detail));
            rows[index].setTextColor(result ? Color.rgb(110, 235, 194) : Color.rgb(255, 168, 155));
            if (!result) rows[index].requestRectangleOnScreen(new android.graphics.Rect(0, 0,
                rows[index].getWidth(), rows[index].getHeight()));
        });
        return result;
    }

    private void checks() {
        boolean success = false;
        try {
            Result root = null;
            // Resolve only known system/root-manager locations, never app-writable PATH entries.
            for (String candidate : new String[]{"/system/bin/su", "/system_ext/bin/su", "/system/xbin/su", "/sbin/su", "/su/bin/su", "/debug_ramdisk/su"}) {
                try {
                    root = command(candidate, "id", 90);
                    su = candidate;
                    break; // A denial is not a reason to trigger five more permission prompts.
                } catch (java.io.IOException absent) { /* Try the next system su location. */ }
            }
            boolean rootTimeout = "root_timeout".equals(failCheck) || (root != null && root.timeout);
            if (!report(0, !rootTimeout && root != null && root.code == 0 && CheckProtocol.root(root.output),
                    rootTimeout ? R.string.root_timeout : FAIL[0], root == null ? null : firstLine(root.output))) return;
            Result probe = nativeCall("--probe", 15);
            String said = CheckProtocol.details(probe.output).get("decoder");
            if (!report(1, probe.code == 0 && CheckProtocol.passed(CheckProtocol.parse(probe.output), "decoder"), FAIL[1],
                    said != null ? said : probe.timeout ? getString(R.string.box_no_answer) : firstLine(probe.output))) return;
            List<String> addresses = Ui.addresses();
            ui(() -> address.setText(addresses.isEmpty() ? getString(R.string.receive_no_network)
                : getString(R.string.pc_address, String.join(" / ", addresses), MainActivity.PORT)));
            success = true;
        } catch (Exception error) {
            android.util.Log.e("LLCast", "Check failed", error);
            for (int i = 0; i < complete.length; ++i)
                if (!complete[i]) { report(i, false, R.string.unexpected_failure); break; }
        } finally {
            boolean allPassed = success;
            ui(() -> {
                for (int i = 0; i < complete.length; ++i)
                    if (!complete[i]) rows[i].setText(getString(R.string.row, "—", getString(LABELS[i]), getString(R.string.not_checked)));
                ready = allPassed;
                if (ready) {
                    // Nothing to choose: receiving starts as soon as every check passed.
                    action.setText(R.string.starting);
                    status.setText(R.string.starting);
                    worker.execute(this::startReceiver);
                    return;
                }
                action.setText(R.string.quit);
                action.setEnabled(true);
                action.requestFocus();
                status.setText(R.string.cannot_start);
            });
        }
    }

    private static String firstLine(String output) {
        for (String line : output.split("\n")) if (!line.trim().isEmpty()) return line.trim();
        return null;
    }

    private void startReceiver() {
        String reason = null;
        try {
            Result start = nativeCall("--start", 30);
            if (start.code == 0 && start.output.contains("SESSION\tPASS\t")) return;
            android.util.Log.e("LLCast", "Session start failed: " + start.output);
            reason = CheckProtocol.sessionFailure(start.output);
            if (reason == null) reason = firstLine(start.output);
        } catch (Exception error) { android.util.Log.e("LLCast", "Session start failed", error); }
        String said = reason;
        ui(() -> {
            ready = false;
            status.setText(said == null ? getString(R.string.start_failed)
                : getString(R.string.start_failed) + "\n" + getString(R.string.box_said, said));
            action.setText(R.string.quit);
            action.setEnabled(true);
            action.requestFocus();
        });
    }

    private Result nativeCall(String operation, int seconds) throws Exception {
        return command(su, CheckProtocol.nativeCommand(getApplicationInfo().nativeLibraryDir, operation), seconds);
    }

    private static final class Result {
        int code;
        boolean timeout;
        String output;
    }

    private static Result command(String executable, String fixedCommand, int seconds) throws Exception {
        Process process = new ProcessBuilder(executable, "-c", fixedCommand).redirectErrorStream(true).start();
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        Thread reader = new Thread(() -> {
            try (InputStream stream = process.getInputStream()) {
                byte[] buffer = new byte[2048];
                int n;
                while ((n = stream.read(buffer)) != -1)
                    if (output.size() < 65536) output.write(buffer, 0, Math.min(n, 65536 - output.size()));
            } catch (java.io.IOException closed) { /* Timeout or process exit. */ }
        }, "llcast-root-output");
        reader.setDaemon(true);
        reader.start();
        Result result = new Result();
        try {
            result.timeout = !process.waitFor(seconds, TimeUnit.SECONDS);
            if (result.timeout) process.destroyForcibly();
            result.code = result.timeout ? -1 : process.exitValue();
            reader.join(1000);
            result.output = output.toString(StandardCharsets.UTF_8.name());
            return result;
        } finally {
            if (process.isAlive()) process.destroyForcibly();
            process.getInputStream().close();
            process.getOutputStream().close();
        }
    }

    @Override public boolean onKeyUp(int key, KeyEvent event) {
        if (key == KeyEvent.KEYCODE_ESCAPE) { finish(); return true; }
        return super.onKeyUp(key, event);
    }
    @Override public void onDestroy() {
        destroyed = true;
        worker.shutdownNow();
        super.onDestroy();
    }
}
