package com.mhgu.recomp;

import android.app.Dialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * MHGU: la actividad de SDL que arranca el runtime (libmain.so, SDL_main en runtime/main.c).
 *
 * Los archivos del jugador van en la carpeta externa de la app (Android/data/com.mhgu.recomp/files):
 *   data/exefs, data/romfs  la copia propia del juego, si no se eligió otra carpeta (GameFolderActivity)
 *   mods/, cheats/          como en PC
 *   mhgurecomp.ini, log.txt, cache/, MHGURecomp/save/
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "MHGU";

    @Override
    protected String[] getLibraries() {
        return new String[] { "c++_shared", "SDL3", "mhguffmpeg", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        prepareFiles();
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        pickRefreshRate();
    }

    @Override
    protected void onResume() {
        super.onResume();
        pickRefreshRate();
    }

    /**
     * "Guardar y reiniciar" del menú de opciones (runtime/ui.c restart_application, por JNI desde el hilo de SDL):
     * PhoenixActivity, en su propio proceso, espera a que este termine (el código nativo hace exit(0) al volver de acá,
     * y su atexit guarda la caché de pipelines) y lanza la app de nuevo. Si no se puede, una excepción: el menú dice
     * entonces que el reinicio falló y el juego sigue.
     */
    public static void restartApp() {
        SDLActivity activity = mSingleton;
        if (activity == null) throw new IllegalStateException("restartApp: no activity");
        Intent intent = new Intent(activity, PhoenixActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(PhoenixActivity.EXTRA_PID, Process.myPid());
        activity.startActivity(intent);
        Log.i(TAG, "restart: PhoenixActivity, pid " + Process.myPid());
    }

    /** Los carteles del programa (SDL_ShowMessageBox) con los colores de las ventanas del juego. */
    @Override
    protected void messageboxCreateAndShow(Bundle args) {
        final int wine = Color.rgb(38, 18, 14), cream = Color.rgb(240, 228, 200), gold = Color.rgb(226, 182, 92);
        final int bronze = Color.rgb(62, 38, 22), edge = Color.rgb(196, 150, 70);
        final Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setCancelable(false);
        dialog.setOnDismissListener(d -> {
            synchronized (messageboxSelection) {
                messageboxSelection.notify();
            }
        });

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(16), dp(24), dp(18));
        GradientDrawable plate = new GradientDrawable();
        plate.setColor(wine);
        plate.setStroke(dp(2), edge);
        plate.setCornerRadius(dp(6));
        box.setBackground(plate);

        TextView title = new TextView(this);
        title.setText(args.getString("title"));
        title.setTextColor(gold);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 19);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        View rule = new View(this);
        rule.setBackgroundColor(edge);
        LinearLayout.LayoutParams ruleSize = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1));
        ruleSize.setMargins(0, dp(10), 0, dp(14));
        box.addView(rule, ruleSize);
        TextView message = new TextView(this);
        message.setText(args.getString("message"));
        message.setTextColor(cream);
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        message.setGravity(Gravity.CENTER);
        box.addView(message);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(16), 0, 0);
        int[] ids = args.getIntArray("buttonIds");
        String[] texts = args.getStringArray("buttonTexts");
        Button first = null;
        for (int i = 0; i < texts.length; i++) {
            final int id = ids[i];
            Button button = new Button(this);
            button.setText(texts[i]);
            button.setAllCaps(false);
            button.setTextColor(cream);
            GradientDrawable face = new GradientDrawable();
            face.setColor(bronze);
            face.setStroke(dp(2), edge);
            face.setCornerRadius(dp(6));
            button.setBackground(face);
            button.setOnClickListener(v -> {
                messageboxSelection[0] = id;
                dialog.dismiss();
            });
            LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(120), dp(44));
            size.setMargins(dp(6), 0, dp(6), 0);
            row.addView(button, size);
            if (first == null) first = button;
        }
        box.addView(row);
        final Button main = first;
        dialog.setOnKeyListener((d, key, event) -> {  // (un joystick: A o B)
            if (main == null || (key != KeyEvent.KEYCODE_BUTTON_A && key != KeyEvent.KEYCODE_BUTTON_B
                    && key != KeyEvent.KEYCODE_ENTER && key != KeyEvent.KEYCODE_ESCAPE)) return false;
            if (event.getAction() == KeyEvent.ACTION_UP) main.performClick();
            return true;
        });
        dialog.setContentView(box);
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setLayout((int) (getResources().getDisplayMetrics().widthPixels * .52f), WindowManager.LayoutParams.WRAP_CONTENT);
        }
        dialog.show();
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    /** Ajustes -> Licencias (runtime/ui.c show_licenses, por JNI desde el hilo de SDL). */
    public static void showLicenses() {
        SDLActivity activity = mSingleton;
        if (activity != null) activity.startActivity(new Intent(activity, LicensesActivity.class));
    }

    /** Ajustes -> Importar partida. */
    public static void showSaveImport() {
        SDLActivity activity = mSingleton;
        if (activity != null) activity.startActivity(new Intent(activity, SaveImportActivity.class));
    }

    /** Ajustes -> Exportar partida. */
    /** Ajustes -> Gráficos -> Driver de GPU. */
    public static void showDrivers() {
        SDLActivity activity = mSingleton;
        if (activity != null) activity.startActivity(new Intent(activity, DriverActivity.class));
    }

    public static void showSaveExport() {
        SDLActivity activity = mSingleton;
        if (activity != null) activity.startActivity(new Intent(activity, SaveExportActivity.class));
    }

    /** Ajustes -> Compartir registro. */
    public static void showShareLog() {
        SDLActivity activity = mSingleton;
        if (activity != null) activity.startActivity(new Intent(activity, LogShareActivity.class));
    }

    /** El juego se cierra por un error (runtime/platform/android.c): la pantalla para compartir el registro, en su proceso. */
    public static void crashReport() {
        SDLActivity activity = mSingleton;
        if (activity == null) throw new IllegalStateException("crashReport: no activity");
        Intent intent = new Intent(activity, LogShareActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(LogShareActivity.EXTRA_CRASH, true);
        activity.startActivity(intent);
    }

    /** LAN (runtime/net/ldn.c): sin este permiso, muchos teléfonos descartan por Wi-Fi los paquetes de búsqueda de sala. */
    public static void lanSearch(boolean on) {
        SDLActivity activity = mSingleton;
        if (activity == null) return;
        try {
            synchronized (GameActivity.class) {
                if (lanLock == null) {
                    android.net.wifi.WifiManager wifi = (android.net.wifi.WifiManager) activity.getApplicationContext().getSystemService(WIFI_SERVICE);
                    if (wifi == null) return;
                    lanLock = wifi.createMulticastLock("mhgu-lan");
                    lanLock.setReferenceCounted(false);
                }
                if (on) lanLock.acquire();
                else if (lanLock.isHeld()) lanLock.release();
                Log.i(TAG, "LAN: Wi-Fi broadcasts " + (on ? "on" : "off"));
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "LAN: " + e);
        }
    }
    private static android.net.wifi.WifiManager.MulticastLock lanLock;

    /** Ajustes -> Carpeta del juego. */
    public static void showGameFolder() {
        SDLActivity activity = mSingleton;
        if (activity != null) {
            Intent intent = new Intent(activity, GameFolderActivity.class);
            intent.putExtra(GameFolderActivity.EXTRA_CHANGE, true);
            activity.startActivity(intent);
        }
    }

    /** Ajustes -> Carpeta del usuario. */
    public static void showUserFolder() {
        SDLActivity activity = mSingleton;
        if (activity != null) activity.startActivity(new Intent(activity, UserFolderActivity.class));
    }

    private static volatile float frameCap;  // (el límite de FPS de los ajustes; 0: aún no se sabe, o sin límite)

    /** El límite de FPS cambió (runtime/ui.c): la pantalla lo sigue. */
    public static void setFrameCap(float fps) {
        frameCap = fps;
        SDLActivity activity = mSingleton;
        if (activity instanceof GameActivity) activity.runOnUiThread(((GameActivity) activity)::pickRefreshRate);
    }

    /**
     * La tasa de refresco más baja que alcanza para el límite de FPS, nunca menos de 60: una pantalla a 120 Hz con el juego
     * a 60 gasta batería y calienta el teléfono sin mostrar nada más. Sin límite conocido, la más alta.
     */
    private void pickRefreshRate() {
        try {
            Display display = Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
            if (display == null) return;
            Display.Mode current = display.getMode(), best = null, fastest = current;
            float want = frameCap > 0 ? Math.max(frameCap, 60) - .5f : Float.MAX_VALUE;
            for (Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
                if (m.getRefreshRate() > fastest.getRefreshRate()) fastest = m;
                if (m.getRefreshRate() >= want && (best == null || m.getRefreshRate() < best.getRefreshRate())) best = m;
            }
            if (best == null) best = fastest;
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            lp.preferredRefreshRate = best.getRefreshRate();
            getWindow().setAttributes(lp);
            Log.i(TAG, "display mode " + best.getPhysicalWidth() + "x" + best.getPhysicalHeight() + " @ " + best.getRefreshRate());
        } catch (Exception e) {
            Log.w(TAG, "refresh rate: " + e);
        }
    }

    /** Crea las carpetas del jugador y copia lo que el APK trae para ellas (la lista de pipelines). */
    private void prepareFiles() {
        SaveImportActivity.applyPending(this);  // (before the game: Ajustes -> Importar partida)
        File home = UserFolder.home(this);
        new File(home, "data").mkdirs();
        new File(home, "mods").mkdirs();
        new File(home, "cheats").mkdirs();
        copyAsset("pipelines.list", new File(home, "pipelines.list"));
        // los trucos opcionales del port (comer / beber más rápido): la primera vez, apagados (cheats/_off.txt: el
        // runtime enciende todo truco que encuentra salvo los listados ahí; la pestaña Trucos lo reescribe)
        // (una sola vez: si el jugador lo borra o lo renombra con '_', no vuelve)
        android.content.SharedPreferences prefs = getSharedPreferences("mhgu", MODE_PRIVATE);
        if (!prefs.getBoolean("cheats_eat_installed", false)) {
            File eat = new File(home, "cheats/comer_rapido.txt");
            if (copyAsset("cheats/comer_rapido.txt", eat)) {
                try (OutputStream o = new FileOutputStream(new File(home, "cheats/_off.txt"), true)) {
                    o.write(("\ncomer_rapido.txt\tComer rapido siempre (beber x1.7, carne x1.8)\n"
                            + "comer_rapido.txt\tBeber muy rapido (x2.5)\n").getBytes("UTF-8"));
                    prefs.edit().putBoolean("cheats_eat_installed", true).apply();
                } catch (Exception e) {
                    Log.w(TAG, "cheats/_off.txt: " + e);
                }
            }
        }
    }

    private boolean copyAsset(String name, File to) {  // (false: it could not be copied)
        try (InputStream in = getAssets().open(name)) {
            long size = in.available();
            if (to.exists() && to.length() == size) return true;
            try (OutputStream out = new FileOutputStream(to)) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            return true;
        } catch (Exception e) {
            Log.w(TAG, "asset " + name + ": " + e);
            return false;
        }
    }
}
