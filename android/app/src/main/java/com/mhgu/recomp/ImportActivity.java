package com.mhgu.recomp;

import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Ajustes -> Trucos / Mods: lo que el juego lee de las carpetas cheats/, mods/ y dlc/ de la app (Android/data, que en
 * Android 11 o más no se puede abrir con un explorador de archivos), copiado desde donde el jugador lo tenga:
 *   - trucos: archivos .txt en el formato de Atmosphere (runtime/cheats.c), a cheats/;
 *   - un mod: una carpeta con romfs/ o exefs/ (o los archivos de nativeNX), o un .zip de ella, a mods/<nombre>/
 *     (runtime/mods.c; su orden y si está activo, en la página Mods);
 *   - DLC: cada carpeta con un DLC.dlcp, a dlc/<id del título o número>/ (runtime/services/dlc.c).
 * Se lee al arrancar el juego: al cerrar, si algo cambió, se reinicia (como DriverActivity). La app ya tiene acceso a todos
 * los archivos (lo pide la pantalla de la carpeta del juego), así que el explorador es propio, con el estilo del juego.
 */
public class ImportActivity extends Activity {
    public static final String KIND = "kind";
    public static final int CHEATS = 0, MOD = 1, DLC = 2;
    private static final String TAG = "MHGU";
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120), LIT = Color.rgb(96, 62, 24);
    private static final Pattern TITLE_ID = Pattern.compile("(?i)\\b(0100770008dd[9a-f][0-9a-f]{3})\\b");

    private int kind;
    private boolean es, changed, busy;
    private TextView status;
    private LinearLayout list;
    private Button add, closeButton;
    private Dialog browser;

    private File home() { return AppHome.home(this); }
    private String sub() { return kind == CHEATS ? "cheats" : kind == MOD ? "mods" : "dlc"; }

    /** The game's folder (GameFolderActivity's choice), or null. */
    private File gameFolder() {
        try (FileInputStream in = new FileInputStream(new File(home(), "game_folder.txt"))) {
            byte[] b = new byte[4096];
            int n = in.read(b);
            String path = n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
            return path.isEmpty() ? null : new File(path);
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Where they go: the game folder's cheats/, mods/ or dlc/ (the game reads them there, and a player can put them
     * there by hand with any file manager); the app's own folder if the game folder can't be written (an SD card).
     */
    private File target() {
        File game = gameFolder();
        if (game != null) {
            File in = new File(game, sub());
            if ((in.isDirectory() || in.mkdirs()) && in.canWrite()) return in;
        }
        return new File(home(), sub());
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView text(String s, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(Zh.t(s));
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private GradientDrawable plate(int fill) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(2), EDGE);
        d.setCornerRadius(dp(6));
        return d;
    }

    private Button button(String s, boolean strong, int fill) {
        Button b = new Button(this);
        b.setText(Zh.t(s));
        b.setAllCaps(false);
        b.setTextColor(strong ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        if (strong) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(fill));
        b.setPadding(dp(14), 0, dp(14), 0);
        return b;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        es = "es".equals(Locale.getDefault().getLanguage());
        kind = getIntent().getIntExtra(KIND, CHEATS);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(14), dp(24), dp(16));
        box.setBackground(plate(WINE));
        String[] titles = es ? new String[] {"TRUCOS", "MODS", "DLC"} : new String[] {"CHEATS", "MODS", "DLC"};
        TextView title = text(titles[kind], 20, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        String[] bodies = es ? new String[] {
            "Archivos .txt de trucos en el formato de Atmosphere, para la versión 1.4.0 (su id de compilación es "
                + "FB08F1D20FD1204F). Después se activan uno por uno en la página Trucos. Los que cambian el código del juego "
                + "funcionan, pero esa parte del juego corre más lenta.",
            "Una carpeta de mod con romfs o exefs adentro, o un .zip de esa carpeta. Después se ordenan y se activan en la "
                + "página Mods. Los parches del código (exefs) funcionan, pero esa parte del juego corre más lenta.",
            "Tus propios DLC del juego: la carpeta de cada uno tiene un archivo DLC.dlcp. Elige una carpeta y se copian "
                + "todos los que haya adentro. Después, en el juego: menú principal, Descarga. Es experimental."}
            : new String[] {
            "Cheat .txt files in Atmosphere's format, for version 1.4.0 (its build id is FB08F1D20FD1204F). Then turn them on "
                + "one by one on the Cheats page. Those that change the game's code work, but that part of the game runs slower.",
            "A mod's folder with romfs or exefs in it, or a .zip of that folder. Then order and turn them on on the Mods page. "
                + "Code patches (exefs) work, but that part of the game runs slower.",
            "Your own DLC of the game: each one's folder holds a DLC.dlcp file. Choose a folder and every one in it is copied. "
                + "Then, in the game: main menu, Download. It is experimental."};
        TextView body = text(bodies[kind], 14, CREAM, false);
        body.setPadding(0, dp(10), 0, dp(6));
        body.setLineSpacing(dp(1), 1);
        box.addView(body);
        // (where they go, which is also where they can be put by hand)
        TextView where = text((es ? "También puedes copiarlos tú a esta carpeta del teléfono: "
                                  : "You can also copy them yourself into this folder on the phone: ") + target().getAbsolutePath(),
                13, GOLD, false);
        where.setPadding(0, 0, 0, dp(10));
        box.addView(where);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list);
        status = text("", 14, GOLD, true);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        box.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(12), 0, 0);
        String[] adds = es ? new String[] {"Agregar trucos", "Agregar un mod", "Agregar DLC"}
                           : new String[] {"Add cheats", "Add a mod", "Add DLC"};
        add = button(adds[kind], true, BRONZE);
        add.setOnClickListener(v -> openBrowser(null));
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(190), dp(46));
        size.setMargins(dp(8), 0, dp(8), 0);
        row.addView(add, size);
        closeButton = button(es ? "Cerrar" : "Close", false, BRONZE);
        closeButton.setOnClickListener(v -> {
            if (busy) return;
            if (changed) restart();
            else finish();
        });
        row.addView(closeButton, new LinearLayout.LayoutParams(size));
        box.addView(row);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(PANEL);
        scroll.setFillViewport(true);
        LinearLayout page = new LinearLayout(this);
        page.setGravity(Gravity.CENTER);
        page.setPadding(dp(32), dp(10), dp(32), dp(10));
        int room = getResources().getDisplayMetrics().widthPixels - dp(64);
        page.addView(box, new LinearLayout.LayoutParams(Math.min(dp(640), room), LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(page);
        setContentView(scroll);
        refresh();
    }

    private void say(String s, boolean bad) {
        status.setText(Zh.t(s));
        status.setTextColor(bad ? ALERT : GOLD);
    }

    /** What is installed: each with its Remove. */
    private void refresh() {
        list.removeAllViews();
        File[] all = target().listFiles(f -> !f.getName().startsWith("_") && !f.getName().startsWith(".")
                && (kind == CHEATS ? f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".txt") : f.isDirectory()));
        if (all == null || all.length == 0) {
            TextView none = text(es ? "(nada instalado todavía)" : "(nothing installed yet)", 13, CREAM, false);
            none.setGravity(Gravity.CENTER);
            list.addView(none);
            return;
        }
        Arrays.sort(all);
        for (File f : all) {
            LinearLayout row = new LinearLayout(this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            TextView name = text("◆  " + f.getName(), 14, CREAM, false);
            name.setSingleLine(true);
            row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            Button remove = button(es ? "Quitar" : "Remove", false, BRONZE);
            remove.setOnClickListener(v -> {
                if (busy) return;
                delete(f);
                changed();
                refresh();
            });
            row.addView(remove, new LinearLayout.LayoutParams(dp(92), dp(42)));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, dp(3), 0, dp(3));
            list.addView(row, lp);
        }
    }

    private void changed() {
        changed = true;
        closeButton.setText(Zh.t(es ? "Reiniciar el juego" : "Restart the game"));
    }

    /** Back to the game through PhoenixActivity: these are read at the start. */
    private void restart() {
        Intent intent = new Intent(this, PhoenixActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(PhoenixActivity.EXTRA_PID, Process.myPid());
        startActivity(intent);
        finishAffinity();
    }

    private static void delete(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) delete(k);
        f.delete();
    }

    // ---- the browser (folders, and the files this kind takes)
    private List<File> roots() {
        List<File> out = new ArrayList<>();
        out.add(Environment.getExternalStorageDirectory());
        File[] mine = getExternalFilesDirs(null);
        if (mine != null)
            for (File f : mine) {
                if (f == null) continue;
                String p = f.getAbsolutePath();
                int cut = p.indexOf("/Android/data/");
                if (cut <= 0) continue;
                File root = new File(p.substring(0, cut));
                if (!root.equals(out.get(0)) && root.isDirectory()) out.add(root);
            }
        return out;
    }

    private boolean takes(File f) {
        String n = f.getName().toLowerCase(Locale.ROOT);
        return kind == CHEATS ? n.endsWith(".txt") : kind == MOD ? n.endsWith(".zip") : n.equals("dlc.dlcp");
    }

    private LinearLayout.LayoutParams rowParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(42));
        lp.setMargins(0, dp(3), 0, dp(3));
        return lp;
    }

    private void openBrowser(File at) {
        if (busy) return;
        if (browser != null) browser.dismiss();
        List<File> roots = roots();
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(plate(PANEL));
        box.setPadding(dp(12), dp(12), dp(12), dp(12));
        String[] heads = es ? new String[] {"ELIGE UN ARCHIVO DE TRUCOS", "ELIGE LA CARPETA DEL MOD O SU .ZIP", "ELIGE LA CARPETA DE TUS DLC"}
                            : new String[] {"CHOOSE A CHEAT FILE", "CHOOSE THE MOD'S FOLDER OR ITS .ZIP", "CHOOSE YOUR DLC'S FOLDER"};
        TextView title = text(heads[kind], 16, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        LinearLayout tabs = new LinearLayout(this);
        tabs.setGravity(Gravity.CENTER);
        File dir = at != null ? at : roots.get(0);
        File root = roots.get(0);
        for (File r : roots) if (dir.getAbsolutePath().startsWith(r.getAbsolutePath())) root = r;
        for (int i = 0; i < roots.size(); i++) {
            File r = roots.get(i);
            Button t = button(i == 0 ? (es ? "Memoria interna" : "Phone storage") : (es ? "Tarjeta SD" : "SD card"), r.equals(root), r.equals(root) ? LIT : BRONZE);
            t.setOnClickListener(v -> openBrowser(r));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(40));
            lp.setMargins(dp(4), dp(6), dp(4), dp(4));
            tabs.addView(t, lp);
        }
        box.addView(tabs);
        String shown = dir.getAbsolutePath().substring(root.getAbsolutePath().length());
        TextView where = text(shown.isEmpty() ? "/" : shown, 13, CREAM, false);
        where.setPadding(dp(4), dp(2), dp(4), dp(6));
        box.addView(where);
        LinearLayout items = new LinearLayout(this);
        items.setOrientation(LinearLayout.VERTICAL);
        final File here = dir;
        if (!dir.equals(root)) {
            Button up = button("⬑  " + (es ? "Subir" : "Up"), false, BRONZE);
            up.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            up.setOnClickListener(v -> openBrowser(here.getParentFile()));
            items.addView(up, rowParams());
        }
        File[] kids = dir.listFiles(f -> !f.getName().startsWith(".") && (f.isDirectory() || takes(f)));
        if (kids != null) {
            Arrays.sort(kids, (a, b) -> a.isDirectory() != b.isDirectory() ? (a.isDirectory() ? -1 : 1)
                                                                           : a.getName().compareToIgnoreCase(b.getName()));
            for (File k : kids) {
                boolean folder = k.isDirectory();
                Button b = button((folder ? "▸  " : "◆  ") + k.getName(), !folder, folder ? BRONZE : LIT);
                b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                b.setOnClickListener(v -> {
                    if (folder) { openBrowser(k); return; }
                    browser.dismiss();
                    start(k);
                });
                items.addView(b, rowParams());
            }
        }
        if (kids == null || kids.length == 0) {
            TextView none = text(es ? "(vacío)" : "(empty)", 13, CREAM, false);
            none.setPadding(dp(6), dp(8), dp(6), dp(8));
            items.addView(none);
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(items);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(8), 0, 0);
        if (kind != CHEATS) {  // (a folder: the mod itself, or where the DLC are)
            Button use = button(es ? "Usar esta carpeta" : "Use this folder", true, BRONZE);
            use.setOnClickListener(v -> { browser.dismiss(); start(here); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
            lp.setMargins(dp(5), 0, dp(5), 0);
            row.addView(use, lp);
        }
        Button cancel = button(es ? "Cancelar" : "Cancel", false, BRONZE);
        cancel.setOnClickListener(v -> browser.dismiss());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
        lp.setMargins(dp(5), 0, dp(5), 0);
        row.addView(cancel, lp);
        box.addView(row);
        browser = new Dialog(this);
        browser.requestWindowFeature(Window.FEATURE_NO_TITLE);
        browser.setContentView(box);
        Window w = browser.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(640), (int) (m.widthPixels * .92f)), (int) (m.heightPixels * .92f));
        }
        browser.show();
    }

    // ---- the copy (a background thread)
    private long copied, total;

    private void start(File from) {
        busy = true;
        add.setEnabled(false);
        say(es ? "Copiando…" : "Copying…", false);
        new Thread(() -> {
            String done, error = null;
            try {
                done = kind == CHEATS ? importCheats(from) : kind == MOD ? importMod(from) : importDlc(from);
            } catch (Exception e) {
                Log.e(TAG, "import: " + e);
                done = null;
                error = e.getMessage();
            }
            final String ok = done, why = error;
            runOnUiThread(() -> {
                busy = false;
                add.setEnabled(true);
                if (ok != null) {
                    changed();
                    say((es ? "Listo: " : "Done: ") + ok + (es ? ". Se aplica al reiniciar el juego." : ". It applies when the game restarts."), false);
                } else {
                    say((es ? "No se pudo: " : "It couldn't be done: ") + why, true);
                }
                refresh();
            });
        }).start();
    }

    private void progress() {
        long c = copied, t = total;
        runOnUiThread(() -> say((es ? "Copiando… " : "Copying… ") + (t > 0 ? (c * 100 / t) + " %" : (c >> 20) + " MB"), false));
    }

    private void copyFile(File from, File to) throws IOException {
        to.getParentFile().mkdirs();
        File tmp = new File(to.getPath() + ".part");
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(tmp)) {
            copyStream(in, out);
        }
        to.delete();
        if (!tmp.renameTo(to)) throw new IOException("can't write " + to.getName());
    }

    private long lastTick;
    private void copyStream(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
            copied += n;
            long now = System.currentTimeMillis();
            if (now - lastTick > 400) { lastTick = now; progress(); }
        }
    }

    private static long sizeOf(File f) {
        if (f.isFile()) return f.length();
        long s = 0;
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) s += sizeOf(k);
        return s;
    }

    private void copyTree(File from, File to) throws IOException {
        if (from.isFile()) { copyFile(from, to); return; }
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("can't make " + to.getName());
        File[] kids = from.listFiles();
        if (kids != null) for (File k : kids) copyTree(k, new File(to, k.getName()));
    }

    private static String safeName(String s) {
        String n = s.replaceAll("[^A-Za-z0-9 ._()\\[\\]-]+", "_").trim();
        return n.isEmpty() || n.startsWith(".") || n.startsWith("_") ? "x" + n : n;
    }

    private String importCheats(File txt) throws IOException {
        String name = safeName(txt.getName());
        if (!name.toLowerCase(Locale.ROOT).endsWith(".txt")) name += ".txt";
        total = txt.length();
        copyFile(txt, new File(target(), name));
        // (each one starts off, to be turned on on the Cheats page: the game's list of those off, cheats/_off.txt in the
        // app's folder, gets "<file>\t<name>" for each [name] of the file; {master} codes always run)
        StringBuilder off = new StringBuilder();
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(new FileInputStream(txt), "UTF-8"))) {
            for (String l; (l = r.readLine()) != null; ) {
                l = l.trim();
                if (l.length() > 2 && l.startsWith("[") && l.endsWith("]")) off.append(name).append('\t').append(l, 1, l.length() - 1).append('\n');
            }
        }
        File list = new File(home(), "cheats/_off.txt");
        list.getParentFile().mkdirs();
        try (OutputStream o = new FileOutputStream(list, true)) {
            o.write(("\n" + off).getBytes("UTF-8"));
        }
        return name;
    }

    private static boolean hasModParts(File d, int depth) {
        if (depth > 3 || !d.isDirectory()) return false;
        File[] kids = d.listFiles(File::isDirectory);
        if (kids == null) return false;
        for (File k : kids) {
            String n = k.getName().toLowerCase(Locale.ROOT);
            if (n.equals("romfs") || n.equals("exefs") || n.equals("nativenx") || hasModParts(k, depth + 1)) return true;
        }
        return false;
    }

    private String importMod(File from) throws IOException {
        String base = from.getName();
        if (from.isFile()) {  // a .zip: unpacked into mods/<its name>/
            String name = safeName(base.replaceAll("(?i)\\.zip$", ""));
            File to = new File(target(), name), tmp = new File(target(), ".new");
            delete(tmp);
            total = from.length();
            String canon = tmp.getCanonicalPath() + File.separator;
            try (ZipInputStream z = new ZipInputStream(new FileInputStream(from))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    File out = new File(tmp, e.getName());
                    if (!out.getCanonicalPath().startsWith(canon)) continue;  // (nothing outside the folder)
                    if (e.isDirectory()) { out.mkdirs(); continue; }
                    out.getParentFile().mkdirs();
                    try (OutputStream o = new FileOutputStream(out)) { copyStream(z, o); }
                }
            }
            if (!hasModParts(tmp, 0)) { delete(tmp); throw new IOException(es ? "el .zip no trae romfs ni exefs" : "the .zip has no romfs or exefs"); }
            delete(to);
            if (!tmp.renameTo(to)) throw new IOException("can't write " + name);
            return name;
        }
        if (!hasModParts(from, 0))
            throw new IOException(es ? "esa carpeta no tiene romfs ni exefs adentro" : "that folder has no romfs or exefs in it");
        String name = safeName(base);
        File to = new File(target(), name), tmp = new File(target(), ".new");
        delete(tmp);
        total = sizeOf(from);
        copyTree(from, tmp);
        delete(to);
        if (!tmp.renameTo(to)) throw new IOException("can't write " + name);
        return name;
    }

    private static void findDlc(File d, int depth, List<File> out) {
        if (depth > 6 || out.size() >= 256) return;
        File[] kids = d.listFiles();
        if (kids == null) return;
        for (File k : kids)
            if (k.isFile() && k.getName().equalsIgnoreCase("DLC.dlcp")) out.add(d);
        for (File k : kids)
            if (k.isDirectory() && !k.getName().startsWith(".")) findDlc(k, depth + 1, out);
    }

    private String importDlc(File from) throws IOException {
        File start = from.isFile() ? from.getParentFile() : from;
        List<File> found = new ArrayList<>();
        findDlc(start, 0, found);
        if (found.isEmpty()) throw new IOException(es ? "no hay ningún DLC.dlcp en esa carpeta" : "there is no DLC.dlcp in that folder");
        total = 0;
        for (File d : found) total += sizeOf(d);
        int n = 0, next = 1;
        target().mkdirs();
        for (File d : found) {
            String id = null;  // (the DLC's title id, if its path says it: the index the game knows it by)
            for (File p = d; p != null && id == null; p = p.getParentFile()) {
                Matcher m = TITLE_ID.matcher(p.getName());
                if (m.find()) id = m.group(1).toUpperCase(Locale.ROOT);
            }
            String name = id;
            if (name == null) {
                while (new File(target(), String.format(Locale.ROOT, "%03d", next)).exists()) next++;
                name = String.format(Locale.ROOT, "%03d", next++);
            }
            File to = new File(target(), name);
            delete(to);
            copyTree(d, to);
            n++;
        }
        return n + (es ? (n == 1 ? " DLC copiado" : " DLC copiados") : " DLC copied");
    }
}
