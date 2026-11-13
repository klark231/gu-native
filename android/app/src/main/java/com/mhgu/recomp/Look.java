package com.mhgu.recomp;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.Locale;

/**
 * The app's screens in the game's style (the colours and shapes of GameFolderActivity, SaveImportActivity and the rest):
 * wine-dark plates with a bronze edge, gold titles, cream text. notice() is the game-styled notice the new screens use
 * (the save folder, the official-build and update notices): it never comes out wider than the screen, and its buttons
 * stack when there are more than two, so a long label is never cut.
 */
final class Look {
    static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    static final int ALERT = Color.rgb(255, 150, 120);

    private Look() {}

    static boolean es() { return "es".equals(Locale.getDefault().getLanguage()); }

    static int dp(Context c, int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static TextView text(Context c, CharSequence s, int sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    static GradientDrawable plate(Context c, int fill, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(c, 2), EDGE);
        d.setCornerRadius(dp(c, radius));
        return d;
    }

    static Button button(Context c, String s, boolean main) {
        Button b = new Button(c);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(main ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        if (main) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(c, BRONZE, 6));
        b.setPadding(dp(c, 18), 0, dp(c, 18), 0);
        return b;
    }

    /** One button of a notice: its label, whether it is the main one (gold), and what it does (the notice closes first). */
    static final class Choice {
        final String label;
        final boolean main;
        final Runnable action;
        Choice(String label, boolean main, Runnable action) { this.label = label; this.main = main; this.action = action; }
    }

    /**
     * A notice over `a`: a gold title, the text (it scrolls on a short screen), then the choices. The system's Back
     * presses the last one; a touch outside does nothing. Shown at once; the dialog is returned.
     */
    static Dialog notice(Activity a, String title, CharSequence body, Choice... choices) {
        Dialog d = new Dialog(a);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        d.setCancelable(false);
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(plate(a, WINE, 8));
        box.setPadding(dp(a, 22), dp(a, 14), dp(a, 22), dp(a, 16));
        TextView head = text(a, title, 18, GOLD, true);
        head.setGravity(Gravity.CENTER);
        box.addView(head);
        View rule = new View(a);
        rule.setBackgroundColor(EDGE);
        LinearLayout.LayoutParams ruleSize = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(a, 1));
        ruleSize.setMargins(0, dp(a, 8), 0, dp(a, 10));
        box.addView(rule, ruleSize);
        TextView t = text(a, body, 15, CREAM, false);
        t.setLineSpacing(dp(a, 2), 1);
        ScrollView scroll = new ScrollView(a);
        scroll.addView(t);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        LinearLayout row = new LinearLayout(a);
        boolean stacked = choices.length > 2;
        row.setOrientation(stacked ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(a, 12), 0, 0);
        Button first = null;
        for (Choice ch : choices) {
            Button b = button(a, ch.label, ch.main);
            b.setOnClickListener(v -> {
                d.dismiss();
                if (ch.action != null) ch.action.run();
            });
            LinearLayout.LayoutParams lp = stacked
                    ? new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(a, 44))
                    : new LinearLayout.LayoutParams(0, dp(a, 46), 1);
            lp.setMargins(stacked ? 0 : dp(a, 5), dp(a, stacked ? 3 : 0), stacked ? 0 : dp(a, 5), dp(a, stacked ? 3 : 0));
            row.addView(b, lp);
            if (first == null) first = b;
        }
        box.addView(row);
        final Choice last = choices.length > 0 ? choices[choices.length - 1] : null;
        d.setOnKeyListener((dlg, key, event) -> {  // (Back, or a pad's B: the last choice, "Cancel" / "Later" / "Exit")
            if (key != KeyEvent.KEYCODE_BACK && key != KeyEvent.KEYCODE_BUTTON_B && key != KeyEvent.KEYCODE_ESCAPE) return false;
            if (event.getAction() == KeyEvent.ACTION_UP && last != null) {
                d.dismiss();
                if (last.action != null) last.action.run();
            }
            return true;
        });
        d.setContentView(box);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = a.getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(a, 600), (int) (m.widthPixels * .92f)), android.view.WindowManager.LayoutParams.WRAP_CONTENT);
        }
        // (a box taller than the screen: the window's height is at most the screen's, and the text shrinks and scrolls
        // while the buttons stay: the weighted ScrollView takes what is left)
        d.show();
        if (first != null) first.requestFocus();  // (a pad: A presses the first)
        return d;
    }
}
