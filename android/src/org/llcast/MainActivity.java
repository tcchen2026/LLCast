package org.llcast;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.media.MediaCodecInfo;
import android.content.pm.PackageManager;
import android.media.projection.MediaProjectionManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** As on the Mac: first choose Send or Receive, then that role's page with Back. Every control is
 *  a large focusable frame, so a TV remote's arrows and OK reach everything; touch works the same. */
public final class MainActivity extends Activity {
    static final int PORT = 47800;
    private enum Page { CHOOSER, SEND, RECEIVE }
    private static final int CAPTURE = 1;
    /** Profiles (user decision), as on every sender: Low 1280×720 at 30 fps, Medium 1920×1080 at
     *  60 fps, then Custom. */
    private static final int[][] PROFILES = {{1280, 720, 30}, {1920, 1080, 60}};
    private static final int CUSTOM = 2;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private SharedPreferences settings;
    private Page page = Page.CHOOSER;
    private EditText host;
    private TextView sendStatus;
    private Button start;
    private int profile, codec;
    private boolean autoBudget;
    private EditText customWidth, customHeight, customFps, budget;
    private Button budgetAuto;
    /** The Codec choice, in button order: Auto (HEVC, else H.264; never AV1), then each codec. */
    private static final int[] CODECS = {0, Codecs.LLS_AV1, Codecs.LLS_HEVC, Codecs.LLS_H264};
    private static final String[] CODEC_NAMES = {"Auto", "AV1", "HEVC", "H.264"};
    private Intent pending; // the checked stream, waiting for capture consent

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        settings = getSharedPreferences("llcast", MODE_PRIVATE);
        showChooser();
    }

    @Override public void onBackPressed() {
        if (page == Page.CHOOSER) super.onBackPressed();
        else showChooser();
    }

    /** Rotation and window resizes keep the page, and what was typed on it. */
    @Override public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        if (page == Page.SEND) { saveSend(); showSend(); }
        else if (page == Page.RECEIVE) showReceive();
        else showChooser();
    }

    private boolean wide() {
        return getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
    }

    /** A centred column no wider than 760 dp, so tablets and TVs don't stretch it. */
    private LinearLayout column() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        int width = getResources().getDisplayMetrics().widthPixels;
        int side = Math.max(Ui.dp(this, wide() ? 48 : 24), (width - Ui.dp(this, 760)) / 2);
        column.setPadding(side, Ui.dp(this, 27), side, Ui.dp(this, 27));
        return column;
    }

    private void show(LinearLayout content) {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Ui.BACKGROUND);
        scroll.setFillViewport(true);
        scroll.addView(content);
        setContentView(scroll);
    }

    private LinearLayout.LayoutParams spaced(int width, int height, float weight) {
        LinearLayout.LayoutParams place = new LinearLayout.LayoutParams(width, height, weight);
        int gap = Ui.dp(this, 8);
        place.setMargins(gap, gap, gap, gap);
        return place;
    }

    private void showChooser() {
        page = Page.CHOOSER;
        handler.removeCallbacksAndMessages(null);
        LinearLayout screen = column();
        screen.addView(Ui.bold(this, getString(R.string.app_name), 32));
        TextView intro = Ui.text(this, getString(R.string.chooser_intro), 18);
        intro.setPadding(0, Ui.dp(this, 4), 0, Ui.dp(this, 24));
        screen.addView(intro);
        LinearLayout cards = new LinearLayout(this);
        cards.setOrientation(wide() ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        LinearLayout send = Ui.card(this, R.drawable.ic_send, getString(R.string.role_send), null);
        LinearLayout receive = Ui.card(this, R.drawable.ic_receive, getString(R.string.role_receive), null);
        send.setOnClickListener(view -> showSend());
        receive.setOnClickListener(view -> showReceive());
        for (LinearLayout card : new LinearLayout[]{send, receive})
            cards.addView(card, wide() ? spaced(0, Ui.dp(this, 200), 1)
                                       : spaced(ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 170), 0));
        screen.addView(cards);
        show(screen);
        // The role used last has the focus, so OK picks it again.
        ("send".equals(settings.getString("role", "receive")) ? send : receive).requestFocus();
    }

    /** A role page: Back, then the title. */
    private LinearLayout rolePage(Page shown, String role, int title) {
        page = shown;
        settings.edit().putString("role", role).apply();
        LinearLayout screen = column();
        Button back = Ui.button(this, getString(R.string.back));
        back.setOnClickListener(view -> showChooser());
        screen.addView(back, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView heading = Ui.bold(this, getString(title), 28);
        heading.setPadding(0, Ui.dp(this, 16), 0, Ui.dp(this, 12));
        screen.addView(heading);
        return screen;
    }

    private TextView label(int text) {
        TextView label = Ui.text(this, getString(text), 16);
        label.setTextColor(Color.rgb(200, 214, 230));
        label.setPadding(Ui.dp(this, 8), Ui.dp(this, 12), 0, 0);
        return label;
    }

    private void saveSend() {
        if (host == null) return;
        settings.edit().putString("host", host.getText().toString().trim()).putInt("profile", profile)
            .putInt("codec", codec).putBoolean("autoBudget", autoBudget)
            .putString("customWidth", customWidth.getText().toString())
            .putString("customHeight", customHeight.getText().toString())
            .putString("customFps", customFps.getText().toString()).apply();
        if (!autoBudget) settings.edit().putString("budget", budget.getText().toString().trim()).apply();
    }

    private static int number(EditText field) {
        try { return Integer.parseInt(field.getText().toString().trim()); } catch (NumberFormatException e) { return 0; }
    }

    /** Width, height and fps; Custom's may be invalid (checked at Start). */
    private int[] chosenProfile() {
        return profile == CUSTOM ? new int[]{number(customWidth), number(customHeight), number(customFps)} : PROFILES[profile];
    }

    private static boolean validProfile(int[] p) {
        return p[0] >= 160 && p[0] <= 7680 && p[0] % 2 == 0 && p[1] >= 160 && p[1] <= 7680 && p[1] % 2 == 0 &&
            p[2] >= 1 && p[2] <= 240;
    }

    /** A typed budget in bit/s (e.g. "12.5" Mbps), or 0 outside 0.5 to 200 Mbps. */
    private static int typedBudget(String text) {
        try {
            double mbps = Double.parseDouble(text.trim().replace(',', '.'));
            return mbps >= 0.5 && mbps <= 200 ? (int) Math.round(mbps * 1e6) : 0;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    static String mbps(int bps) {
        return bps % 1000000 == 0 ? String.valueOf(bps / 1000000)
            : String.format(java.util.Locale.ROOT, "%.1f", bps / 1e6);
    }

    /** Auto shows its value in the box, for the chosen codec (Auto counts as HEVC). */
    private void showBudget() {
        budgetAuto.setActivated(autoBudget);
        budget.setEnabled(!autoBudget);
        budget.setFocusable(!autoBudget);
        if (!autoBudget) return;
        int[] p = chosenProfile();
        int bps = validProfile(p) ? Native.autoBudget(p[0], p[1], p[2], CODECS[codec]) : 0;
        budget.setText(bps > 0 ? mbps(bps) : "");
    }

    /** Low and Medium show their size; only Custom can be edited. */
    private void showProfile() {
        for (EditText field : new EditText[]{customWidth, customHeight, customFps}) {
            field.setEnabled(profile == CUSTOM);
            field.setFocusable(profile == CUSTOM);
        }
        if (profile != CUSTOM) {
            customWidth.setText(String.valueOf(PROFILES[profile][0]));
            customHeight.setText(String.valueOf(PROFILES[profile][1]));
            customFps.setText(String.valueOf(PROFILES[profile][2]));
        } else {
            customWidth.setText(settings.getString("customWidth", "1920"));
            customHeight.setText(settings.getString("customHeight", "1080"));
            customFps.setText(settings.getString("customFps", "60"));
        }
        showBudget();
    }

    private EditText numberField(int columns) {
        EditText field = Ui.field(this);
        field.setInputType(InputType.TYPE_CLASS_NUMBER);
        field.setEms(columns);
        field.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(android.text.Editable s) { if (budgetAuto != null) showBudget(); }
        });
        return field;
    }

    private TextView inline(String text) {
        TextView view = Ui.text(this, text, 18);
        view.setPadding(Ui.dp(this, 6), 0, Ui.dp(this, 6), 0);
        return view;
    }

    private void showSend() {
        LinearLayout screen = rolePage(Page.SEND, "send", R.string.send_title);
        screen.addView(label(R.string.send_receiver));
        host = Ui.field(this);
        host.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        host.setHint(R.string.send_receiver_hint);
        host.setText(settings.getString("host", ""));
        screen.addView(host, spaced(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0));
        TextView typed = label(R.string.send_receiver_detail);
        typed.setTextSize(14);
        screen.addView(typed);
        // Side-by-side choices instead of drop-downs: left/right and OK on a remote.
        // User decision, every sender: profile, codec, then the bandwidth budget.
        screen.addView(label(R.string.send_profile));
        // Before profiles, "quality" was 0 for 720p and 1 for 1080p.
        profile = settings.getInt("profile", settings.getInt("quality", 1) == 0 ? 0 : 1);
        LinearLayout choices = new LinearLayout(this);
        Button[] options = {Ui.button(this, getString(R.string.profile_low)),
                            Ui.button(this, getString(R.string.profile_medium)),
                            Ui.button(this, getString(R.string.profile_custom))};
        for (int i = 0; i < options.length; ++i) {
            final int choice = i;
            options[i].setActivated(i == profile);
            options[i].setOnClickListener(view -> {
                profile = choice;
                for (int j = 0; j < options.length; ++j) options[j].setActivated(j == choice);
                showProfile();
            });
            choices.addView(options[i], spaced(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        }
        screen.addView(choices);
        LinearLayout size = new LinearLayout(this);
        size.setGravity(Gravity.CENTER_VERTICAL);
        customWidth = numberField(4);
        customHeight = numberField(4);
        customFps = numberField(3);
        size.addView(customWidth);
        size.addView(inline("×"));
        size.addView(customHeight);
        size.addView(inline(getString(R.string.send_at)));
        size.addView(customFps);
        size.addView(inline(getString(R.string.send_fps)));
        screen.addView(size, spaced(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0));
        screen.addView(label(R.string.send_codec));
        codec = settings.getInt("codec", 0);
        LinearLayout codecRow = new LinearLayout(this);
        Button[] codecButtons = new Button[CODEC_NAMES.length];
        for (int i = 0; i < codecButtons.length; ++i) {
            final int choice = i;
            codecButtons[i] = Ui.button(this, CODEC_NAMES[i]);
            codecButtons[i].setMinWidth(0);
            codecButtons[i].setActivated(i == codec);
            codecButtons[i].setOnClickListener(view -> {
                codec = choice;
                for (int j = 0; j < codecButtons.length; ++j) codecButtons[j].setActivated(j == choice);
                showBudget();
            });
            codecRow.addView(codecButtons[i], spaced(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        }
        screen.addView(codecRow);
        screen.addView(label(R.string.send_budget));
        LinearLayout budgetRow = new LinearLayout(this);
        budgetRow.setGravity(Gravity.CENTER_VERTICAL);
        budgetAuto = Ui.button(this, getString(R.string.budget_auto));
        budgetAuto.setMinWidth(0);
        budget = Ui.field(this);
        budget.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
        budget.setEms(4);
        autoBudget = settings.getBoolean("autoBudget", true);
        if (!autoBudget) budget.setText(settings.getString("budget", "20"));
        budgetAuto.setOnClickListener(view -> {
            autoBudget = !autoBudget;
            if (!autoBudget) budget.setText(settings.getString("budget", budget.getText().toString()));
            showBudget();
        });
        budgetRow.addView(budgetAuto, spaced(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0));
        budgetRow.addView(budget);
        budgetRow.addView(inline(getString(R.string.budget_mbps)));
        screen.addView(budgetRow);
        showProfile();
        sendStatus = Ui.text(this, getString(R.string.send_ready), 16);
        sendStatus.setPadding(Ui.dp(this, 8), Ui.dp(this, 16), 0, Ui.dp(this, 4));
        screen.addView(sendStatus);
        start = Ui.button(this, getString(R.string.start));
        // Copy: the status, reception and the doctor's hint, for a message or a report.
        Button copy = Ui.button(this, getString(R.string.copy));
        copy.setOnClickListener(view -> {
            android.content.ClipboardManager clipboard = getSystemService(android.content.ClipboardManager.class);
            if (clipboard != null)
                clipboard.setPrimaryClip(android.content.ClipData.newPlainText("LLCast", sendStatus.getText()));
        });
        LinearLayout buttons = new LinearLayout(this);
        buttons.addView(copy, spaced(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0));
        buttons.addView(new android.view.View(this), new LinearLayout.LayoutParams(0, 0, 1));
        buttons.addView(start, spaced(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0));
        screen.addView(buttons, spaced(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0));
        start.setOnClickListener(view -> {
            if (SendService.running != null) {
                startService(new Intent(this, SendService.class).setAction(SendService.STOP));
                return;
            }
            saveSend();
            String[] target = Ui.splitAddress(host.getText().toString().trim(), PORT);
            String name = target[0];
            int number = parsePort(target[1]);
            int[] chosen = chosenProfile();
            int typedBps = autoBudget ? 0 : typedBudget(budget.getText().toString());
            if (name.isEmpty())
                Ui.problem(this, getString(R.string.address_missing), getString(R.string.address_missing_advice), null);
            else if (number == 0)
                Ui.problem(this, getString(R.string.port_invalid), getString(R.string.port_invalid_advice), null);
            else if (!validProfile(chosen))
                Ui.problem(this, getString(R.string.custom_invalid), getString(R.string.custom_invalid_advice), null);
            else if (!autoBudget && typedBps == 0)
                Ui.problem(this, getString(R.string.budget_invalid), getString(R.string.budget_invalid_advice), null);
            else
                beginSending(name, number, chosen, typedBps, CODECS[codec]);
        });
        show(screen);
        (host.getText().toString().trim().isEmpty() ? host : start).requestFocus();
        handler.removeCallbacksAndMessages(null);
        handler.post(this::pollSending);
        // Once: the sending notification carries Stop. Sending works without it.
        if (android.os.Build.VERSION.SDK_INT >= 33 && !settings.getBoolean("askedNotifications", false) &&
            checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED) {
            settings.edit().putBoolean("askedNotifications", true).apply();
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 0);
        }
    }

    /** The Send page's status line and Start/Stop, once a second. */
    private void pollSending() {
        if (page != Page.SEND) return;
        SendService service = SendService.running;
        start.setText(service != null ? R.string.stop : R.string.start);
        if (service != null) sendStatus.setText(service.describe());
        String[] stopped = SendService.problem;
        if (stopped != null) {
            SendService.problem = null;
            sendStatus.setText(stopped[0] + ".");
            Ui.problem(this, stopped[0], stopped[1], null);
        }
        handler.postDelayed(this::pollSending, 1000);
    }

    private static int parsePort(String text) {
        try {
            int value = Integer.parseInt(text);
            return value >= 1 && value <= 65535 ? value : 0;
        } catch (NumberFormatException invalid) {
            return 0;
        }
    }

    /** Prechecks off the UI thread (encoders, the receiver and its codecs), then capture consent.
     *  typedBudget 0: Auto, for the codec the receiver and this device agree on. */
    private void beginSending(String name, int number, int[] m, int typedBudget, int wanted) {
        start.setEnabled(false);
        sendStatus.setText(R.string.send_checking);
        android.util.DisplayMetrics screen = new android.util.DisplayMetrics();
        getSystemService(android.hardware.display.DisplayManager.class)
            .getDisplay(android.view.Display.DEFAULT_DISPLAY).getRealMetrics(screen);
        String label = getString(R.string.profile_label, m[0], m[1], m[2]);
        new Thread(() -> {
            // What this device encodes in hardware at this size and rate, by LLS_CODEC_* number.
            Codecs.Encoder[] encoders = new Codecs.Encoder[4];
            encoders[Codecs.LLS_HEVC] = Codecs.encoder(Codecs.HEVC, screen.widthPixels, screen.heightPixels, m[0], m[1], m[2]);
            encoders[Codecs.LLS_H264] = Codecs.encoder(Codecs.AVC, screen.widthPixels, screen.heightPixels, m[0], m[1], m[2]);
            encoders[Codecs.LLS_AV1] = Codecs.encoder(Codecs.AV1, screen.widthPixels, screen.heightPixels, m[0], m[1], m[2]);
            long here = 0;
            for (int c = 1; c < encoders.length; ++c) if (encoders[c] != null) here |= 1L << c;
            String[] problem = null;
            Intent stream = null;
            if (here == 0) {
                boolean hardware = Codecs.hasEncoder(Codecs.HEVC) || Codecs.hasEncoder(Codecs.AVC) || Codecs.hasEncoder(Codecs.AV1);
                problem = hardware
                    ? new String[]{getString(R.string.encoder_profile_unavailable, label), getString(R.string.encoder_profile_advice)}
                    : new String[]{getString(R.string.no_encoder), getString(R.string.no_encoder_advice, label)};
            } else if (wanted != 0 && (here & (1L << wanted)) == 0) {
                String type = wanted == Codecs.LLS_AV1 ? Codecs.AV1 : wanted == Codecs.LLS_H264 ? Codecs.AVC : Codecs.HEVC;
                problem = Codecs.hasEncoder(type)
                    ? new String[]{getString(R.string.codec_profile_unavailable, codecName(wanted), label),
                                   getString(R.string.encoder_profile_advice)}
                    : new String[]{getString(R.string.codec_not_here, codecName(wanted)),
                                   getString(R.string.codec_not_here_advice, codecList(here))};
            } else {
                long[] probe = new long[4];
                Native.probe(name, number, probe);
                problem = probeProblem((int) probe[0], name, number);
                long there = probe[2];
                int chosen = pick(wanted, here, there);
                if (problem == null && chosen == 0) {
                    problem = wanted != 0
                        ? new String[]{getString(R.string.codec_not_there, codecName(wanted)),
                                       getString(R.string.codec_not_there_advice, codecList(there))}
                        : new String[]{getString(R.string.codecs_mismatch),
                                       getString(R.string.codecs_mismatch_advice, codecList(here), codecList(there))};
                }
                if (problem == null) {
                    String type = chosen == Codecs.LLS_AV1 ? Codecs.AV1 : chosen == Codecs.LLS_H264 ? Codecs.AVC : Codecs.HEVC;
                    int bps = typedBudget != 0 ? typedBudget : Native.autoBudget(m[0], m[1], m[2], chosen);
                    stream = new Intent(this, SendService.class)
                        .putExtra(SendService.HOST, name).putExtra(SendService.PORT, number)
                        .putExtra(SendService.CODEC, chosen).putExtra(SendService.TYPE, type)
                        .putExtra(SendService.WIDTH, m[0]).putExtra(SendService.HEIGHT, m[1])
                        .putExtra(SendService.FPS, m[2]).putExtra(SendService.BITRATE, bps)
                        .putExtra(SendService.LABEL, label + " · " + getString(R.string.budget_label, mbps(bps)));
                }
            }
            String[] found = problem;
            Intent checked = stream;
            runOnUiThread(() -> {
                start.setEnabled(true);
                if (found != null) {
                    sendStatus.setText(found[0] + ".");
                    Ui.problem(this, found[0], found[1], null);
                    return;
                }
                pending = checked;
                startActivityForResult(getSystemService(MediaProjectionManager.class).createScreenCaptureIntent(), CAPTURE);
            });
        }, "llcast-checks").start();
    }

    /** lls_codec_pick: Auto is HEVC, else H.264, never AV1; a chosen codec only if both sides have it. */
    private static int pick(int wanted, long here, long there) {
        long both = here & there;
        if (wanted != 0) return (both & (1L << wanted)) != 0 ? wanted : 0;
        if ((both & (1L << Codecs.LLS_HEVC)) != 0) return Codecs.LLS_HEVC;
        if ((both & (1L << Codecs.LLS_H264)) != 0) return Codecs.LLS_H264;
        return 0;
    }

    static String codecName(int codec) {
        return codec == Codecs.LLS_AV1 ? "AV1" : codec == Codecs.LLS_H264 ? "H.264" : "HEVC";
    }

    private static String codecList(long set) {
        StringBuilder text = new StringBuilder();
        for (int c : new int[]{Codecs.LLS_HEVC, Codecs.LLS_H264, Codecs.LLS_AV1})
            if ((set & (1L << c)) != 0) text.append(text.length() > 0 ? ", " : "").append(codecName(c));
        return text.length() > 0 ? text.toString() : "nothing";
    }

    /** The receiver probe's result in words, as on Windows and the Mac; null when it answered. */
    private String[] probeProblem(int result, String name, int number) {
        switch (result) {
            case 0: return null;
            case 2: return new String[]{getString(R.string.receiver_refused), getString(R.string.receiver_refused_advice, name)};
            case 3: return new String[]{getString(R.string.receiver_unreachable), getString(R.string.receiver_unreachable_advice, name)};
            case 4: return new String[]{getString(R.string.receiver_unknown), getString(R.string.receiver_unknown_advice, name)};
            default: return new String[]{getString(R.string.receiver_silent), getString(R.string.receiver_silent_advice, name, number)};
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != CAPTURE || pending == null) return;
        Intent stream = pending;
        pending = null;
        if (result != RESULT_OK || data == null) {
            if (sendStatus != null) sendStatus.setText(R.string.capture_denied);
            return;
        }
        startForegroundService(stream.putExtra(SendService.RESULT, result).putExtra(SendService.DATA, data));
    }

    private void showReceive() {
        LinearLayout screen = rolePage(Page.RECEIVE, "receive", R.string.receive_title);
        // No address here: it would read as already listening. The waiting screen shows it.
        TextView hint = Ui.text(this, getString(R.string.receive_choose), 20);
        hint.setPadding(0, Ui.dp(this, 4), 0, Ui.dp(this, 16));
        screen.addView(hint);
        TextView refresh = Ui.refreshWarning(this, 17);
        if (refresh != null) {
            refresh.setPadding(0, 0, 0, Ui.dp(this, 16));
            screen.addView(refresh);
        }
        boolean box = android.os.Process.is64Bit();
        LinearLayout normal = Ui.card(this, 0, getString(R.string.mode_normal), getString(R.string.mode_normal_detail));
        LinearLayout extreme = Ui.card(this, 0, getString(R.string.mode_extreme),
            getString(box ? R.string.mode_extreme_detail : R.string.mode_extreme_unavailable));
        // Unavailable stays focusable so a remote can reach and read it.
        if (!box) extreme.setAlpha(0.5f);
        normal.setOnClickListener(view -> receiveNormal());
        extreme.setOnClickListener(view -> {
            if (!box) {
                Ui.problem(this, getString(R.string.mode_extreme), getString(R.string.mode_extreme_unavailable), null);
                return;
            }
            settings.edit().putString("receiveMode", "extreme").apply();
            startActivity(new Intent(this, BoxActivity.class));
        });
        for (LinearLayout card : new LinearLayout[]{normal, extreme})
            screen.addView(card, spaced(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, 0));
        show(screen);
        String mode = settings.getString("receiveMode", Codecs.rockchip() ? "extreme" : "normal");
        ("extreme".equals(mode) && box ? extreme : normal).requestFocus();
    }

    private void receiveNormal() {
        settings.edit().putString("receiveMode", "normal").apply();
        if (Ui.addresses().isEmpty()) {
            Ui.problem(this, getString(R.string.no_network), getString(R.string.no_network_advice), null);
            return;
        }
        MediaCodecInfo hevc = Codecs.hardware(Codecs.HEVC), avc = Codecs.hardware(Codecs.AVC), av1 = Codecs.hardware(Codecs.AV1);
        if (hevc == null && avc == null && av1 == null && !Native.softwareAv1()) {
            Ui.problem(this, getString(R.string.no_decoder), getString(R.string.no_decoder_advice), null);
            return;
        }
        Intent receive = new Intent(this, ReceiveActivity.class);
        if (hevc != null) receive.putExtra(ReceiveActivity.HEVC, hevc.getName())
            .putExtra(ReceiveActivity.HEVC_LOW_LATENCY, Codecs.lowLatency(hevc, Codecs.HEVC));
        if (avc != null) receive.putExtra(ReceiveActivity.AVC, avc.getName())
            .putExtra(ReceiveActivity.AVC_LOW_LATENCY, Codecs.lowLatency(avc, Codecs.AVC));
        if (av1 != null) receive.putExtra(ReceiveActivity.AV1, av1.getName())
            .putExtra(ReceiveActivity.AV1_LOW_LATENCY, Codecs.lowLatency(av1, Codecs.AV1));
        startActivity(receive);
    }
}
