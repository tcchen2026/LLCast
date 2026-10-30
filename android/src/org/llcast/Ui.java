package org.llcast;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** The pages' shared look: dark, large, and obvious focus for a TV remote. */
final class Ui {
    static final int BACKGROUND = Color.rgb(16, 28, 44);
    static final int ACCENT = Color.rgb(120, 190, 255);
    static final int WARNING = Color.rgb(255, 205, 120);
    private static final int MUTED = Color.rgb(170, 186, 206);
    private static final int CARD = Color.rgb(30, 48, 72);
    private static final int CARD_FOCUS = Color.rgb(44, 70, 104);
    private static final int EDGE_FOCUS = ACCENT;
    private static final int CHOSEN = Color.rgb(36, 92, 150);

    private Ui() {}

    static int dp(Context context, float value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    static TextView text(Context context, CharSequence value, float size) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.WHITE);
        return view;
    }

    static TextView bold(Context context, CharSequence value, float size) {
        TextView view = text(context, value, size);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        return view;
    }

    /** Secondary text: instructions around the thing to read. */
    static TextView muted(Context context, CharSequence value, float size) {
        TextView view = text(context, value, size);
        view.setTextColor(MUTED);
        return view;
    }

    /** A rounded card that is not a control. */
    static GradientDrawable panel(Context context) { return shape(context, CARD, CARD); }

    private static GradientDrawable shape(Context context, int fill, int edge) {
        GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dp(context, 16));
        shape.setColor(fill);
        shape.setStroke(dp(context, 3), edge);
        return shape;
    }

    /** Every control's frame: the same bright edge when the remote's focus is on it. A chosen
     *  (activated) option keeps an accent fill. */
    static StateListDrawable frame(Context context) {
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_pressed}, shape(context, CARD_FOCUS, EDGE_FOCUS));
        states.addState(new int[]{android.R.attr.state_focused}, shape(context, CARD_FOCUS, EDGE_FOCUS));
        states.addState(new int[]{android.R.attr.state_activated}, shape(context, CHOSEN, CHOSEN));
        states.addState(new int[]{}, shape(context, CARD, CARD));
        return states;
    }

    static Button button(Context context, CharSequence label) {
        Button button = new Button(context);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(18);
        button.setTextColor(Color.WHITE);
        button.setBackground(frame(context));
        button.setStateListAnimator(null);
        button.setMinWidth(dp(context, 140));
        button.setPadding(dp(context, 24), dp(context, 12), dp(context, 24), dp(context, 12));
        return button;
    }

    static EditText field(Context context) {
        EditText field = new EditText(context);
        field.setSingleLine(true);
        field.setTextSize(20);
        field.setTextColor(Color.WHITE);
        field.setHintTextColor(Color.rgb(140, 156, 176));
        field.setBackground(frame(context));
        int pad = dp(context, 14);
        field.setPadding(pad, pad, pad, pad);
        return field;
    }

    /** A focusable, clickable card: an optional icon, a title, an optional line of description. */
    static LinearLayout card(Context context, int icon, CharSequence title, CharSequence detail) {
        LinearLayout card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setGravity(icon != 0 ? Gravity.CENTER : Gravity.CENTER_VERTICAL | Gravity.START);
        int pad = dp(context, 20);
        card.setPadding(pad, pad, pad, pad);
        card.setBackground(frame(context));
        card.setFocusable(true);
        card.setClickable(true);
        if (icon != 0) {
            ImageView image = new ImageView(context);
            image.setImageResource(icon);
            card.addView(image, new LinearLayout.LayoutParams(dp(context, 64), dp(context, 64)));
        }
        TextView heading = bold(context, title, icon != 0 ? 22 : 21);
        if (icon != 0) heading.setPadding(0, dp(context, 10), 0, 0);
        card.addView(heading);
        if (detail != null) {
            TextView line = text(context, detail, 16);
            line.setTextColor(Color.rgb(200, 214, 230));
            line.setPadding(0, dp(context, 4), 0, 0);
            card.addView(line);
        }
        return card;
    }

    /** What is wrong and what to do about it, as on the Mac and Windows. */
    static void problem(Activity activity, CharSequence headline, CharSequence advice, Runnable dismissed) {
        if (activity.isFinishing()) return;
        new AlertDialog.Builder(activity).setTitle(headline).setMessage(advice)
            .setPositiveButton(R.string.ok, null)
            .setOnDismissListener(dialog -> { if (dismissed != null) dismissed.run(); })
            .show();
    }

    /** "host" or "host:port" (IPv6 as "[address]:port"), as receivers show it: {host, port text}.
     *  Without a port, the default; a bare IPv6 address has no port. */
    static String[] splitAddress(String text, int fallback) {
        if (text.startsWith("[")) {
            int close = text.indexOf(']');
            if (close < 0) return new String[]{text, ""};
            String rest = text.substring(close + 1);
            return new String[]{text.substring(1, close),
                rest.isEmpty() ? String.valueOf(fallback) : rest.startsWith(":") ? rest.substring(1) : ""};
        }
        int colon = text.indexOf(':');
        if (colon < 0 || colon != text.lastIndexOf(':')) return new String[]{text, String.valueOf(fallback)};
        return new String[]{text.substring(0, colon), text.substring(colon + 1)};
    }

    /** A warning when this screen's refresh rate isn't about 60 Hz, or null. LLCast keeps the
     *  screen's mode (user decision); at other rates some frames are dropped or held longer. */
    static TextView refreshWarning(Context context, float size) {
        android.view.Display display = context.getSystemService(android.hardware.display.DisplayManager.class)
            .getDisplay(android.view.Display.DEFAULT_DISPLAY);
        float hz = display == null ? 60 : display.getRefreshRate();
        if (hz >= 59.5f && hz <= 60.5f) return null;
        TextView warning = text(context, context.getString(R.string.refresh_warning, String.valueOf(Math.round(hz))), size);
        warning.setTextColor(WARNING);
        return warning;
    }

    /** This device's IPv4 addresses on active interfaces. */
    static List<String> addresses() {
        List<String> found = new ArrayList<>();
        try {
            for (NetworkInterface nic : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nic.isUp() || nic.isLoopback()) continue;
                for (InetAddress ip : Collections.list(nic.getInetAddresses()))
                    if (ip instanceof Inet4Address && !ip.isLoopbackAddress() && !ip.isLinkLocalAddress())
                        found.add(ip.getHostAddress());
            }
        } catch (java.net.SocketException ignored) { /* No interfaces: an empty list says it. */ }
        return found;
    }
}
