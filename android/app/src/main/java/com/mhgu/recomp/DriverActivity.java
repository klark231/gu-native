package com.mhgu.recomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Ajustes -> Gráficos -> Driver de GPU: un driver Vulkan propio para una Adreno, a riesgo del jugador. Por defecto se usa
 * el del sistema y nada cambia. Acepta los .zip con meta.json y la librería, el formato de los paquetes de drivers para
 * emuladores. El juego lee la elección al arrancar (runtime/platform/android_gpu.c); si con ese driver no llega a dibujar,
 * en el arranque siguiente vuelve solo al del sistema y deja el archivo failed, que esta pantalla muestra.
 */
public class DriverActivity extends Activity {
    private static final String TAG = "MHGU";
    private static final int PICK = 1;
    private static final long MAX_BYTES = 256L * 1024 * 1024;  // (una librería de driver: unas decenas de MB)
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120), LIT = Color.rgb(96, 62, 24);

    private boolean es, changed;
    private TextView status;
    private LinearLayout list;
    private Button add;

    private File root() { return new File(getFilesDir(), "gpu_drivers"); }

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

    private GradientDrawable plate(int fill) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(2), EDGE);
        d.setCornerRadius(dp(6));
        return d;
    }

    private Button button(String s, boolean strong, int fill) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(strong ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        if (strong) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(fill));
        return b;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        es = "es".equals(Locale.getDefault().getLanguage());

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(14), dp(24), dp(16));
        box.setBackground(plate(WINE));
        TextView title = text(es ? "DRIVER DE GPU" : "GPU DRIVER", 20, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        TextView body = text(es
                ? "Solo para GPU Adreno y bajo tu propio riesgo: un driver que no sea para tu GPU puede cerrar el juego o verse mal. "
                  + "Si te falla al arrancar, la app vuelve sola al driver del sistema. Agrega un .zip con meta.json y la librería."
                : "For Adreno GPUs only, at your own risk: a driver that isn't right for your GPU can close the game or look wrong. "
                  + "If it fails at start, the app goes back to the system's driver by itself. Add a .zip with meta.json and the library.",
                14, CREAM, false);
        body.setPadding(0, dp(10), 0, dp(10));
        body.setLineSpacing(dp(1), 1);
        box.addView(body);
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
        add = button(es ? "Agregar driver" : "Add a driver", true, BRONZE);
        add.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            startActivityForResult(pick, PICK);
        });
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(190), dp(46));
        size.setMargins(dp(8), 0, dp(8), 0);
        row.addView(add, size);
        Button close = button(es ? "Cerrar" : "Close", false, BRONZE);
        close.setOnClickListener(v -> {
            if (changed) restart();
            else finish();
        });
        row.addView(close, new LinearLayout.LayoutParams(size));
        box.addView(row);
        closeButton = close;

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(PANEL);
        scroll.setFillViewport(true);
        LinearLayout page = new LinearLayout(this);
        page.setGravity(Gravity.CENTER);
        page.setPadding(dp(32), dp(10), dp(32), dp(10));
        int room = getResources().getDisplayMetrics().widthPixels - dp(64);  // (never wider than the screen)
        page.addView(box, new LinearLayout.LayoutParams(Math.min(dp(600), room), LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(page);
        setContentView(scroll);

        File failed = new File(root(), "failed");
        if (failed.isFile()) {
            say((es ? "No funcionó: " : "It didn't work: ") + firstLine(failed) + (es ? ". Se usa el del sistema." : ". The system's is in use."), true);
            failed.delete();
        }
        refresh();
    }

    private Button closeButton;

    private void say(String s, boolean bad) {
        status.setText(s);
        status.setTextColor(bad ? ALERT : GOLD);
    }

    private static String firstLine(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String l = r.readLine();
            return l != null ? l.trim() : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static List<String> lines(File f) {
        List<String> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String l; (l = r.readLine()) != null; ) out.add(l.trim());
        } catch (IOException e) {
            Log.w(TAG, "driver: " + e);
        }
        return out;
    }

    private void refresh() {
        list.removeAllViews();
        String picked = firstLine(new File(root(), "selected"));
        Button system = button((picked.isEmpty() ? "● " : "") + (es ? "Driver del sistema" : "The system's driver"), picked.isEmpty(), picked.isEmpty() ? LIT : BRONZE);
        system.setOnClickListener(v -> choose(null, null));
        LinearLayout.LayoutParams wide = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(46));
        wide.setMargins(0, dp(4), 0, dp(4));
        list.addView(system, wide);
        File[] all = root().listFiles(File::isDirectory);
        if (all == null) return;
        Arrays.sort(all);
        for (File d : all) {
            List<String> info = lines(new File(d, "driver.txt"));
            if (info.size() < 2) continue;
            boolean on = d.getName().equals(picked);
            LinearLayout row = new LinearLayout(this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            Button use = button((on ? "● " : "") + info.get(0), on, on ? LIT : BRONZE);
            use.setOnClickListener(v -> choose(d.getName(), info.get(1)));
            row.addView(use, new LinearLayout.LayoutParams(0, dp(46), 1));
            Button remove = button(es ? "Quitar" : "Remove", false, BRONZE);
            remove.setOnClickListener(v -> {
                if (on) choose(null, null);
                delete(d);
                refresh();
            });
            LinearLayout.LayoutParams small = new LinearLayout.LayoutParams(dp(92), dp(46));
            small.setMargins(dp(8), 0, 0, 0);
            row.addView(remove, small);
            list.addView(row, new LinearLayout.LayoutParams(wide));
        }
    }

    /**
     * A library named as the system's own driver (vulkan.adreno.so) is never loaded: Android's interface has that one open
     * already, and the system's runs in its place. Such a library gets a name of its own (one installed before too).
     */
    private static String ownName(File dir, String library) {
        boolean system = library.equals("vulkan.adreno.so") || library.equals("libvulkan.so")
                || new File("/vendor/lib64/hw/" + library).exists() || new File("/system/lib64/hw/" + library).exists();
        if (!system) return library;
        String renamed = "vulkan.player.so";
        File from = new File(dir, library), to = new File(dir, renamed);
        if (!from.isFile()) return to.isFile() ? renamed : library;
        to.delete();
        if (!from.renameTo(to)) return library;
        File txt = new File(dir, "driver.txt");
        List<String> info = lines(txt);
        if (info.size() >= 2) {
            try (FileWriter w = new FileWriter(txt)) {
                w.write(info.get(0) + "\n" + renamed + "\n");
            } catch (IOException e) {
                Log.w(TAG, "driver: " + e);
            }
        }
        return renamed;
    }

    private void choose(String folder, String library) {
        File selected = new File(root(), "selected");
        new File(root(), "trial").delete();
        if (folder == null) {
            selected.delete();
        } else {
            root().mkdirs();
            library = ownName(new File(root(), folder), library);
            try (FileWriter w = new FileWriter(selected)) {
                w.write(folder + "\n" + library + "\n");
            } catch (IOException e) {
                say((es ? "No se pudo guardar: " : "Could not save it: ") + e.getMessage(), true);
                return;
            }
        }
        changed = true;
        closeButton.setText(es ? "Reiniciar el juego" : "Restart the game");
        say(es ? "Se aplica al reiniciar el juego. Guarda antes tu partida." : "It applies when the game restarts. Save your game first.", false);
        refresh();
    }

    /** Back to the game through PhoenixActivity: a driver is only loaded at the start. */
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

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri zip = data.getData();
        add.setEnabled(false);
        say(es ? "Instalando…" : "Installing…", false);
        new Thread(() -> {
            String name = null, error = null;
            try {
                name = install(zip);
            } catch (Exception e) {
                Log.e(TAG, "driver install: " + e);
                error = e.getMessage();
            }
            final String done = name, why = error;
            runOnUiThread(() -> {
                add.setEnabled(true);
                if (done != null) say((es ? "Instalado: " : "Installed: ") + done + (es ? ". Tócalo para usarlo." : ". Tap it to use it."), false);
                else say((es ? "No se pudo instalar: " : "Could not install it: ") + why, true);
                refresh();
            });
        }).start();
    }

    /** The zip's meta.json and its 64-bit ARM libraries into a folder of their own. The name to show. */
    private String install(Uri zip) throws Exception {
        File tmp = new File(root(), ".new");
        delete(tmp);
        if (!tmp.mkdirs()) throw new IOException("no folder");
        JSONObject meta = null;
        List<String> libs = new ArrayList<>();
        long total = 0;
        try (InputStream in = getContentResolver().openInputStream(zip); ZipInputStream z = new ZipInputStream(in)) {
            byte[] buf = new byte[65536];
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory()) continue;
                String base = new File(e.getName()).getName();  // (nothing outside the folder, whatever the zip says)
                if (base.equalsIgnoreCase("meta.json")) {
                    ByteArrayOutputStream json = new ByteArrayOutputStream();
                    for (int n; (n = z.read(buf)) > 0; ) if (json.size() < 65536) json.write(buf, 0, n);
                    meta = new JSONObject(json.toString("UTF-8"));
                } else if (base.endsWith(".so")) {
                    try (OutputStream out = new FileOutputStream(new File(tmp, base))) {
                        for (int n; (n = z.read(buf)) > 0; ) {
                            total += n;
                            if (total > MAX_BYTES) throw new IOException(es ? "demasiado grande" : "too large");
                            out.write(buf, 0, n);
                        }
                    }
                    libs.add(base);
                }
            }
        }
        if (libs.isEmpty()) {
            delete(tmp);
            throw new IOException(es ? "el zip no trae ninguna librería .so" : "the zip has no .so library");
        }
        String library = meta != null ? meta.optString("libraryName", "") : "";
        if (library.isEmpty() || !libs.contains(library)) library = libs.get(0);
        if (!arm64(new File(tmp, library))) {
            delete(tmp);
            throw new IOException(es ? "no es una librería para ARM de 64 bits" : "not a 64-bit ARM library");
        }
        library = ownName(tmp, library);
        String shown = meta != null ? meta.optString("name", "") : "";
        String version = meta != null ? meta.optString("driverVersion", "") : "";
        if (shown.isEmpty()) shown = library;
        if (!version.isEmpty()) shown += " " + version;
        String folder = shown.replaceAll("[^A-Za-z0-9._-]+", "_");
        if (folder.startsWith(".") || folder.isEmpty()) folder = "driver" + folder;
        try (FileWriter w = new FileWriter(new File(tmp, "driver.txt"))) {
            w.write(shown.replace('\n', ' ') + "\n" + library + "\n");
        }
        File to = new File(root(), folder);
        delete(to);
        if (!tmp.renameTo(to)) throw new IOException("no folder");
        return shown;
    }

    private static boolean arm64(File so) throws IOException {
        byte[] h = new byte[20];
        try (FileInputStream in = new FileInputStream(so)) {
            if (in.read(h) != h.length) return false;
        }
        return h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 2 && h[18] == (byte) 0xb7 && h[19] == 0;
    }
}
