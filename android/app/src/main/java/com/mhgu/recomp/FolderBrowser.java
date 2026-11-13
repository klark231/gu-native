package com.mhgu.recomp;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.Window;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The app's own folder browser, in the game's style, for every screen that asks for a folder (the game's, the save
 * folder): Android's picker (HyperOS on a POCO F6, seen twice) showed the storage's root empty and refused it, and with
 * access to all files the folders can be listed here. Android's picker stays as the second way (systemPicker), and
 * pathOf turns its answer into a plain path.
 */
final class FolderBrowser {
    /** What the screen looks for: a folder that holds it gets a ✓. */
    interface Mark { boolean holds(File dir); }
    /** The line under the path for the folder shown. */
    interface Hint { String text(File dir, boolean holds); }
    interface Use { void folder(File dir); }

    private FolderBrowser() {}

    /**
     * The storage roots: the phone's own and each SD card's (from the app's folders on them). `writable` (the save
     * folder): an SD card only when the game can write in it (AppHome.probe). Android 9 and 10 never let an app write on
     * a removable card outside its own folder there, so every folder of it would only be refused once chosen.
     */
    static List<File> roots(Context c, boolean writable) {
        List<File> out = new ArrayList<>();
        out.add(Environment.getExternalStorageDirectory());
        File[] mine = c.getExternalFilesDirs(null);
        if (mine != null)
            for (File f : mine) {
                if (f == null) continue;
                String p = f.getAbsolutePath();
                int cut = p.indexOf("/Android/data/");
                if (cut <= 0) continue;
                File root = new File(p.substring(0, cut));
                if (!root.equals(out.get(0)) && root.isDirectory() && (!writable || AppHome.probe(root) == 0)) out.add(root);
            }
        return out;
    }

    /** A picked folder as a plain path (null: a provider with no real folder behind it, Drive and the like). */
    static File pathOf(Uri tree) {
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            String authority = tree.getAuthority();
            if ("com.android.providers.downloads.documents".equals(authority) && id.startsWith("raw:"))
                return new File(id.substring(4));
            if (!"com.android.externalstorage.documents".equals(authority)) return null;
            int colon = id.indexOf(':');
            if (colon < 0) return null;
            String volume = id.substring(0, colon), rest = id.substring(colon + 1);
            File base = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory() : new File("/storage", volume);
            return rest.isEmpty() ? base : new File(base, rest);
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Opens the browser at `at` (null: the phone's storage). `use` gets the folder shown when "Use this folder" is
     * pressed; `system` (may be null) opens Android's picker instead. `writable`: the game will write in the folder
     * chosen (the save folder), so only the storage it can write on is offered (roots).
     */
    static Dialog open(Activity a, File at, String title, Mark mark, Hint hint, Use use, Runnable system, boolean writable) {
        boolean es = Look.es();
        List<File> roots = roots(a, writable);
        int pad = Look.dp(a, 12);
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(Look.plate(a, Look.PANEL, 8));
        box.setPadding(pad, pad, pad, pad);
        TextView head = Look.text(a, title, 17, Look.GOLD, true);
        head.setGravity(Gravity.CENTER);
        box.addView(head);
        Dialog d = new Dialog(a);
        // the roots, as tabs
        LinearLayout tabs = new LinearLayout(a);
        tabs.setGravity(Gravity.CENTER);
        File dir = at != null && at.isDirectory() ? at : roots.get(0);
        File root = roots.get(0);
        for (File r : roots) if (dir.getAbsolutePath().startsWith(r.getAbsolutePath())) root = r;
        if (!dir.getAbsolutePath().startsWith(root.getAbsolutePath())) dir = root;  // (on a card not offered: the phone's)
        for (int i = 0; i < roots.size(); i++) {
            File r = roots.get(i);
            Button t = Look.button(a, i == 0 ? (es ? "Memoria interna" : "Phone storage")
                                             : (es ? "Tarjeta SD" : "SD card") + (roots.size() > 2 ? " " + i : ""), r.equals(root));
            t.setOnClickListener(v -> { d.dismiss(); open(a, r, title, mark, hint, use, system, writable); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, Look.dp(a, 40));
            lp.setMargins(Look.dp(a, 4), Look.dp(a, 6), Look.dp(a, 4), Look.dp(a, 4));
            tabs.addView(t, lp);
        }
        box.addView(tabs);
        String shown = dir.getAbsolutePath().substring(root.getAbsolutePath().length());
        TextView where = Look.text(a, shown.isEmpty() ? "/" : shown, 13, Look.CREAM, false);
        where.setPadding(Look.dp(a, 4), Look.dp(a, 2), Look.dp(a, 4), Look.dp(a, 4));
        box.addView(where);
        boolean holds = mark.holds(dir);
        TextView line = Look.text(a, hint.text(dir, holds), 13, holds ? Look.GOLD : Look.CREAM, holds);
        line.setPadding(Look.dp(a, 4), 0, Look.dp(a, 4), Look.dp(a, 6));
        box.addView(line);
        // the folders here
        LinearLayout list = new LinearLayout(a);
        list.setOrientation(LinearLayout.VERTICAL);
        final File here = dir;
        if (!dir.equals(root)) {
            Button up = Look.button(a, "⬑  " + (es ? "Subir" : "Up"), false);
            up.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            up.setOnClickListener(v -> { d.dismiss(); open(a, here.getParentFile(), title, mark, hint, use, system, writable); });
            list.addView(up, rowParams(a));
        }
        File[] kids = dir.listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (kids != null) {
            Arrays.sort(kids, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
            for (File k : kids) {
                boolean has = mark.holds(k);
                Button b = Look.button(a, (has ? "✓  " : "▸  ") + k.getName(), has);
                b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                b.setOnClickListener(v -> { d.dismiss(); open(a, k, title, mark, hint, use, system, writable); });
                list.addView(b, rowParams(a));
            }
        }
        if (kids == null || kids.length == 0) {
            TextView none = Look.text(a, es ? "(no hay carpetas)" : "(no folders)", 13, Look.CREAM, false);
            none.setPadding(Look.dp(a, 6), Look.dp(a, 8), Look.dp(a, 6), Look.dp(a, 8));
            list.addView(none);
        }
        ScrollView scroll = new ScrollView(a);
        scroll.addView(list);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        // use it, Android's picker, cancel
        LinearLayout row = new LinearLayout(a);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, Look.dp(a, 8), 0, 0);
        Button ok = Look.button(a, es ? "Usar esta carpeta" : "Use this folder", true);
        ok.setOnClickListener(v -> { d.dismiss(); use.folder(here); });
        row.addView(ok, buttonParams(a));
        if (system != null) {
            Button other = Look.button(a, es ? "Selector de Android" : "Android's picker", false);
            other.setOnClickListener(v -> { d.dismiss(); system.run(); });
            row.addView(other, buttonParams(a));
        }
        Button cancel = Look.button(a, es ? "Cancelar" : "Cancel", false);
        cancel.setOnClickListener(v -> d.dismiss());
        row.addView(cancel, buttonParams(a));
        box.addView(row);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        d.setContentView(box);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = a.getResources().getDisplayMetrics();
            w.setLayout(Math.min(Look.dp(a, 640), (int) (m.widthPixels * .92f)), (int) (m.heightPixels * .92f));
        }
        d.show();
        ok.requestFocus();
        return d;
    }

    private static LinearLayout.LayoutParams rowParams(Context c) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Look.dp(c, 42));
        lp.setMargins(0, Look.dp(c, 3), 0, Look.dp(c, 3));
        return lp;
    }

    private static LinearLayout.LayoutParams buttonParams(Context c) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, Look.dp(c, 44));
        lp.setMargins(Look.dp(c, 5), 0, Look.dp(c, 5), 0);
        return lp;
    }
}
