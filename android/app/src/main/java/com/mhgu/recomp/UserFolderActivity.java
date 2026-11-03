package com.mhgu.recomp;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Locale;

/**
 * The user folder's screen (see UserFolder): choose where settings, saves, mods, cheats and logs live, as Azahar's user
 * directory. Three ways in: the first start (EXTRA_FIRST: offered once), Settings -> User folder (the game is running: the
 * choice is only noted and applied at the next start, so that nothing is copied while the game still writes), and the
 * start after a choice (EXTRA_APPLY: copies the files with a progress, then goes on to GameFolderActivity).
 */
public class UserFolderActivity extends Activity {
    static final String EXTRA_FIRST = "first", EXTRA_APPLY = "apply";
    private static final int PICK = 1, PERM = 2;
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120);

    private boolean es, first, apply, waitingAccess, lost;
    private TextView status;
    private Button pick, useDefault, leave, retry;
    private volatile boolean working;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        es = "es".equals(Locale.getDefault().getLanguage());
        apply = getIntent().getBooleanExtra(EXTRA_APPLY, false);
        first = getIntent().getBooleanExtra(EXTRA_FIRST, false);
        lost = UserFolder.lost(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (apply) {
            buildApply();
            run();
        } else {
            buildPicker();
            if (lost) {
                status.setTextColor(ALERT);
                status.setText(es ? "No se puede usar la carpeta " + UserFolder.chosen(this) + " (¿se quitó la tarjeta o el permiso?). Elígela otra vez o usa la carpeta por defecto."
                                  : "The folder " + UserFolder.chosen(this) + " can't be used (was the card taken out, or the permission?). Choose it again or use the default.");
            }
        }
    }

    // ---- look
    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView text(String s, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private GradientDrawable plate(int fill, int r) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(2), EDGE);
        d.setCornerRadius(dp(r));
        return d;
    }

    private Button button(String s, boolean main) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(main ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        if (main) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(BRONZE, 6));
        b.setPadding(dp(18), 0, dp(18), 0);
        return b;
    }

    private LinearLayout.LayoutParams btnSize(int w) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, dp(46));
        p.setMargins(dp(8), 0, dp(8), 0);
        return p;
    }

    private LinearLayout shell(String title, String intro, LinearLayout.LayoutParams unused, LinearLayout[] pageOut) {
        int pad = dp(14);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad * 2, pad, pad * 2, pad);
        TextView t = text(title, 20, GOLD, true);
        t.setGravity(Gravity.CENTER);
        page.addView(t);
        TextView i = text(intro, 14, CREAM, false);
        i.setGravity(Gravity.CENTER);
        i.setPadding(0, dp(4), 0, dp(10));
        page.addView(i);
        pageOut[0] = page;
        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        LinearLayout whole = new LinearLayout(this);
        whole.setOrientation(LinearLayout.VERTICAL);
        whole.setBackgroundColor(WINE);
        whole.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        status = text("", 14, GOLD, false);
        status.setGravity(Gravity.CENTER);
        status.setPadding(pad * 2, dp(6), pad * 2, dp(6));
        whole.addView(status);
        whole.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(),
                    insets.getSystemWindowInsetBottom());
            return insets;
        });
        return whole;
    }

    private void buildPicker() {
        LinearLayout[] pg = new LinearLayout[1];
        LinearLayout whole = shell(es ? "CARPETA DEL USUARIO" : "USER FOLDER",
                es ? "Aquí van tus ajustes, partidas, mods, trucos y registros. Elige una carpeta que alcances con un administrador de archivos: así es fácil copiarla o reemplazar archivos."
                   : "Your settings, saves, mods, cheats and logs live here. Choose a folder you can reach with a file manager, so it is easy to back up or to replace files.",
                null, pg);
        LinearLayout page = pg[0];
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(plate(PANEL, 6));
        box.setPadding(dp(14), dp(10), dp(14), dp(10));
        box.addView(text(es ? "AHORA" : "NOW", 13, GOLD, true));
        TextView now = text(UserFolder.home(this).getAbsolutePath() + (UserFolder.chosen(this) == null ? (es ? "  (por defecto)" : "  (default)") : ""),
                14, CREAM, false);
        now.setPadding(0, dp(4), 0, dp(10));
        box.addView(now);
        box.addView(text(es ? "LA CARPETA CONTIENE" : "THE FOLDER HOLDS", 13, GOLD, true));
        TextView tree = text("mhgurecomp.ini\nMHGURecomp/save\nmods/   cheats/\ndata/   (exefs, romfs)\nlog.txt", 13, CREAM, false);
        tree.setTypeface(Typeface.MONOSPACE);
        tree.setPadding(0, dp(4), 0, 0);
        box.addView(tree);
        page.addView(box);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        pick = button(UserFolder.hasAccess(this) ? (es ? "Elegir carpeta" : "Choose folder")
                                                 : (es ? "Dar permiso y elegir carpeta" : "Give permission and choose folder"), true);
        pick.setOnClickListener(v -> choose());
        row.addView(pick, btnSize(LinearLayout.LayoutParams.WRAP_CONTENT));
        useDefault = button(first ? (es ? "Usar la de siempre" : "Keep the default") : (es ? "Volver a la de siempre" : "Use the default"), false);
        useDefault.setOnClickListener(v -> onDefault());
        row.addView(useDefault, btnSize(LinearLayout.LayoutParams.WRAP_CONTENT));
        if (!first) {
            leave = button(es ? "Cancelar" : "Cancel", false);
            leave.setOnClickListener(v -> finish());
            row.addView(leave, btnSize(dp(120)));
        }
        row.setPadding(0, 0, 0, dp(10));
        whole.addView(row);
        setContentView(whole);
        pick.requestFocus();  // (a console's pad: A presses it)
    }

    private void buildApply() {
        LinearLayout[] pg = new LinearLayout[1];
        LinearLayout whole = shell(es ? "MOVIENDO TUS ARCHIVOS" : "MOVING YOUR FILES",
                es ? "Se copian a la carpeta nueva. No cierres la app. Lo de la carpeta anterior no se borra."
                   : "They are being copied to the new folder. Don't close the app. The old folder is left as it is.", null, pg);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        retry = button(es ? "Reintentar" : "Retry", true);
        retry.setOnClickListener(v -> run());
        retry.setVisibility(android.view.View.GONE);
        row.addView(retry, btnSize(LinearLayout.LayoutParams.WRAP_CONTENT));
        leave = button(es ? "Dejar la carpeta anterior" : "Keep the old folder", false);
        leave.setOnClickListener(v -> { UserFolder.clearPending(this); goOn(); });
        leave.setVisibility(android.view.View.GONE);
        row.addView(leave, btnSize(LinearLayout.LayoutParams.WRAP_CONTENT));
        row.setPadding(0, 0, 0, dp(10));
        whole.addView(row);
        setContentView(whole);
    }

    // ---- choosing
    private void choose() {
        if (UserFolder.hasAccess(this)) { openPicker(); return; }
        if (Build.VERSION.SDK_INT < 30) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE}, PERM);
            return;
        }
        waitingAccess = true;
        status.setTextColor(GOLD);
        status.setText(es ? "Activa el acceso a todos los archivos y vuelve."
                          : "Turn on access to all files and come back.");
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (apply) return;
        if (pick != null) pick.setText(UserFolder.hasAccess(this) ? (es ? "Elegir carpeta" : "Choose folder")
                                                                  : (es ? "Dar permiso y elegir carpeta" : "Give permission and choose folder"));
        if (waitingAccess && UserFolder.hasAccess(this)) {
            waitingAccess = false;
            status.setText("");
            openPicker();
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        if (code == PERM && UserFolder.hasAccess(this)) openPicker();
    }

    private void openPicker() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK);
        } catch (ActivityNotFoundException e) {
            fail(es ? "Este dispositivo no tiene selector de carpetas." : "This device has no folder picker.");
        }
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        File dir = UserFolder.pathOf(data.getData());
        if (dir == null) {
            fail(es ? "No se puede usar esa ubicación. Busca la carpeta en la memoria del teléfono o en la tarjeta SD."
                    : "That location can't be used. Find the folder in the phone's storage or on the SD card.");
            return;
        }
        change(dir, false);
    }

    private void onDefault() {
        if (first) {  // (kept: not asked again; a lost folder is forgotten, or this start would come back here)
            if (lost) UserFolder.setChosen(this, null);
            UserFolder.markAsked(this);
            goOn();
            return;
        }
        change(UserFolder.defaultHome(this), true);
    }

    private void fail(String s) {
        status.setTextColor(ALERT);
        status.setText(s);
    }

    /** target: the new folder (toDefault: the app's own, which is kept as "no choice"). */
    private void change(File target, boolean toDefault) {
        File now = UserFolder.home(this);
        boolean same;
        try {
            same = target.getCanonicalPath().equals(now.getCanonicalPath());
        } catch (IOException e) {
            same = target.equals(now);
        }
        if (same) {
            if (toDefault && UserFolder.chosen(this) != null) {  // (the pointer names it too: just clear it)
                UserFolder.setChosen(this, null);
            }
            UserFolder.markAsked(this);
            status.setTextColor(GOLD);
            status.setText(es ? "Ya se usa esa carpeta." : "That folder is already in use.");
            if (first) goOn();
            return;
        }
        if (!UserFolder.canCreateIn(target)) {
            fail(es ? "No se puede escribir en esa carpeta. Elige otra (o revisa el permiso de todos los archivos)."
                    : "Files can't be written in that folder. Choose another (or check the all-files permission).");
            return;
        }
        if (UserFolder.nested(target, now)) {
            fail(es ? "Elige una carpeta que no esté dentro de la actual, ni la contenga."
                    : "Choose a folder that is not inside the current one, and does not contain it.");
            return;
        }
        if (UserFolder.hasUserFiles(target)) {  // (a folder from before: a reinstall, another phone: used as it is)
            queue(target, toDefault, UserFolder.EMPTY, es ? "Ya hay archivos tuyos en esa carpeta: se usarán tal cual."
                                                          : "Your files are already in that folder: they will be used as they are.");
            return;
        }
        pick.setEnabled(false);
        useDefault.setEnabled(false);
        status.setTextColor(GOLD);
        status.setText(es ? "Contando los archivos..." : "Counting the files...");
        new Thread(() -> {
            long all = size(now, false, true), data = all - size(now, true, true);
            runOnUiThread(() -> {
                pick.setEnabled(true);
                useDefault.setEnabled(true);
                status.setText("");
                ask(target, toDefault, all, data);
            });
        }).start();
    }

    private void ask(File target, boolean toDefault, long all, long data) {
        if (all == 0) {
            queue(target, toDefault, UserFolder.EMPTY, es ? "La carpeta nueva se usará vacía." : "The new folder will be used empty.");
            return;
        }
        AlertDialog.Builder b = new AlertDialog.Builder(this);
        b.setTitle(es ? "¿Copiar tus archivos?" : "Copy your files?");
        b.setMessage((es ? "En la carpeta actual hay " + fmt(all) + (data > 0 ? " (" + fmt(data) + " son los archivos del juego en data/)." : ".")
                         : "The current folder holds " + fmt(all) + (data > 0 ? " (" + fmt(data) + " of it the game files in data/)." : "."))
                + "\n\n" + (es ? "Lo de la carpeta anterior no se borra." : "The old folder is not deleted."));
        b.setPositiveButton(data > 0 ? (es ? "Copiar todo" : "Copy everything") : (es ? "Copiar" : "Copy"),
                (d, w) -> queue(target, toDefault, UserFolder.COPY, null));
        if (data > 0)
            b.setNeutralButton(es ? "Sin los archivos del juego" : "Leave the game files",
                    (d, w) -> queue(target, toDefault, UserFolder.KEEP_GAME, null));
        b.setNegativeButton(es ? "Empezar vacía" : "Start empty", (d, w) -> queue(target, toDefault, UserFolder.EMPTY, null));
        b.show();
    }

    /** The choice is kept; it takes effect at the next start, when the files are copied (runtime/main.c reads the folder then). */
    private void queue(File target, boolean toDefault, String how, String note) {
        UserFolder.setPending(this, toDefault ? null : target, how);
        if (first) {  // (nothing is running yet: apply it now)
            startActivity(new Intent(this, UserFolderActivity.class).putExtra(EXTRA_APPLY, true));
            finish();
            return;
        }
        status.setTextColor(GOLD);
        status.setText((note != null ? note + " " : "")
                + (es ? "Cierra el juego del todo y vuelve a abrirlo: ahí se copian los archivos y se empieza a usar la carpeta nueva."
                      : "Close the game completely and open it again: the files are copied then, and the new folder starts to be used."));
        pick.setEnabled(false);
        useDefault.setEnabled(false);
    }

    private static String fmt(long b) {
        return b >= 1L << 30 ? String.format(Locale.US, "%.1f GB", b / (double) (1L << 30)) : String.format(Locale.US, "%.0f MB", b / (double) (1L << 20));
    }

    // ---- applying (the start after a choice)
    private void run() {
        if (working) return;
        working = true;
        retry.setVisibility(android.view.View.GONE);
        leave.setVisibility(android.view.View.GONE);
        status.setTextColor(GOLD);
        String[] p = UserFolder.pending(this);
        File to = p[0].isEmpty() ? UserFolder.defaultHome(this) : new File(p[0]);
        File from = UserFolder.home(this);
        String how = p[1];
        new Thread(() -> {
            String wrong = null;
            try {
                if (!UserFolder.canCreateIn(to)) throw new IOException(es ? "No se puede escribir en " + to : "Can't write in " + to);
                if (!how.equals(UserFolder.EMPTY) && !UserFolder.nested(from, to)) {
                    boolean skipData = how.equals(UserFolder.KEEP_GAME);
                    total = size(from, skipData, true);
                    done = 0;
                    copyTree(from, to, skipData, true);
                    File data = new File(from, "data");
                    if (skipData && data.isDirectory() && !new File(to, "game_folder.txt").exists())  // (the game files are read where they are)
                        UserFolder.write(new File(to, "game_folder.txt"), data.getAbsolutePath());
                }
            } catch (IOException e) {
                wrong = String.valueOf(e.getMessage());
            }
            final String err = wrong;
            runOnUiThread(() -> {
                working = false;
                if (err != null) {
                    fail((es ? "No se pudo copiar: " : "Could not copy: ") + err);
                    retry.setVisibility(android.view.View.VISIBLE);
                    leave.setVisibility(android.view.View.VISIBLE);
                    return;
                }
                UserFolder.setChosen(this, p[0].isEmpty() ? null : to);
                UserFolder.clearPending(this);
                UserFolder.markAsked(this);
                goOn();
            });
        }).start();
    }

    private volatile long total, done;
    private volatile long shown;

    private void progress(String name) {
        long now = System.currentTimeMillis();
        if (now - shown < 400) return;
        shown = now;
        final String s = (es ? "Copiando " : "Copying ") + fmt(done) + " / " + fmt(total) + "\n" + name;
        runOnUiThread(() -> status.setText(s));
    }

    /** The bytes the copy will move (skipData: not data/ at the top). */
    private static long size(File dir, boolean skipData, boolean top) {
        long n = 0;
        File[] list = dir.listFiles();
        if (list == null) return 0;
        for (File f : list) {
            if (top && skipData && f.getName().equals("data")) continue;
            if (f.getName().equals(".mhgu_write_test")) continue;
            n += f.isDirectory() ? size(f, skipData, false) : f.length();
        }
        return n;
    }

    private void copyTree(File from, File to, boolean skipData, boolean top) throws IOException {
        File[] list = from.listFiles();
        if (list == null) return;
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to);
        for (File f : list) {
            if (top && skipData && f.getName().equals("data")) continue;
            if (f.getName().equals(".mhgu_write_test")) continue;
            File t = new File(to, f.getName());
            if (f.isDirectory()) { copyTree(f, t, skipData, false); continue; }
            if (t.isFile() && t.length() == f.length()) { done += f.length(); continue; }  // (a copy that was cut short goes on from here)
            File part = new File(to, f.getName() + ".part");
            try (FileInputStream in = new FileInputStream(f); FileOutputStream out = new FileOutputStream(part)) {
                byte[] buf = new byte[1 << 20];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    done += n;
                    progress(f.getName());
                }
            }
            t.delete();
            if (!part.renameTo(t)) throw new IOException("rename " + t);
        }
    }

    private void goOn() {
        startActivity(new Intent(this, GameFolderActivity.class));
        finish();
    }
}
