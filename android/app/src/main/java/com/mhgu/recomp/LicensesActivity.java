package com.mhgu.recomp;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.Arrays;
import java.util.Locale;

/** Ajustes -> Licencias: los textos de assets/licenses, con los colores de las ventanas del juego. */
public class LicensesActivity extends Activity {
    private static final int WINE = Color.rgb(38, 18, 14), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        boolean es = "es".equals(Locale.getDefault().getLanguage());
        int pad = dp(16);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(WINE);
        page.setPadding(pad * 2, pad, pad * 2, pad);

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = new TextView(this);
        title.setText(Zh.t(es ? "LICENCIAS" : "LICENSES"));
        title.setTextColor(GOLD);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        top.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button close = new Button(this);
        close.setText(Zh.t(es ? "Cerrar" : "Close"));
        close.setAllCaps(false);
        close.setTextColor(CREAM);
        GradientDrawable face = new GradientDrawable();
        face.setColor(BRONZE);
        face.setStroke(dp(2), EDGE);
        face.setCornerRadius(dp(6));
        close.setBackground(face);
        close.setOnClickListener((View v) -> finish());
        top.addView(close);
        page.addView(top);

        TextView body = new TextView(this);
        body.setText(allTexts());
        body.setTextColor(CREAM);
        body.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        body.setTypeface(Typeface.MONOSPACE);
        ScrollView scroll = new ScrollView(this);
        scroll.setPadding(0, pad, 0, 0);
        scroll.addView(body);
        page.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(page);
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private String allTexts() {
        StringBuilder all = new StringBuilder();
        try {
            String[] names = getAssets().list("licenses");
            Arrays.sort(names);
            for (String name : names) {
                all.append("======== ").append(name).append(" ========\n\n");
                try (InputStream in = getAssets().open("licenses/" + name)) {
                    ByteArrayOutputStream text = new ByteArrayOutputStream();
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) text.write(buf, 0, n);
                    all.append(text.toString("UTF-8")).append("\n\n");
                }
            }
        } catch (Exception e) {
            all.append(e);
        }
        return all.toString();
    }
}
