package com.mhgu.recomp;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Settings -> Save folder (and the start gate's "Choose folder"): where the save is kept. The change is not made here:
 * the game holds the save in memory and could write it at any moment, so the choice waits in
 * save_folder_pending.txt and the app restarts; SaveFolder.applyPending copies the saves before the game runs again.
 * gate() is the start gate GameFolderActivity runs before the game: the folder gone, the permission lost, an empty
 * folder that had a save, two versions left by a sync app, a save that went to this device, the save open elsewhere.
 * The same screens and texts as MH4U for Android's.
 */
public class SaveFolderActivity extends Activity {
    static final String EXTRA_GATE = "gate";  // (opened by the start gate: the game isn't running, Cancel goes back to it)
    private static final int PICK = 1, ACCESS = 2;

    private boolean es, waitingAccess, committed, gate;
    private TextView status, now;
    private Button choose, own;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        es = Look.es();
        gate = getIntent().getBooleanExtra(EXTRA_GATE, false);
        int pad = Look.dp(this, 14);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad * 2, Look.dp(this, 8), pad * 2, Look.dp(this, 6));

        TextView title = Look.text(this, es ? "CARPETA DE PARTIDAS" : "SAVE FOLDER", 20, Look.GOLD, true);
        title.setGravity(Gravity.CENTER);
        page.addView(title);
        // (an SD card only from Android 11: before, Android lets no app write on a removable card outside its own folder,
        // and the folder browser offers a card only when the game can write in it)
        boolean card = Build.VERSION.SDK_INT >= 30;
        TextView intro = Look.text(this, es
                ? (card ? "Guarda tu partida en la carpeta que elijas: en este dispositivo, en su tarjeta SD o en una que una app de "
                          + "sincronización comparta con tus otros dispositivos."
                        : "Guarda tu partida en la carpeta que elijas: en este dispositivo o en una que una app de sincronización "
                          + "comparta con tus otros dispositivos.")
                : (card ? "Keep your save in a folder of your choice: on this device, on its SD card, or in one a sync app shares "
                          + "with your other devices."
                        : "Keep your save in a folder of your choice: on this device, or in one a sync app shares with your other "
                          + "devices."), 14, Look.CREAM, false);
        intro.setGravity(Gravity.CENTER);
        intro.setPadding(0, Look.dp(this, 2), 0, Look.dp(this, 4));
        page.addView(intro);
        now = Look.text(this, "", 14, Look.GOLD, true);
        now.setGravity(Gravity.CENTER);
        now.setPadding(0, 0, 0, Look.dp(this, 8));
        page.addView(now);

        LinearLayout columns = new LinearLayout(this);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        columns.addView(column(es ? "CÓMO FUNCIONA" : "HOW IT WORKS", es
                ? "• Tu partida se copia a la carpeta antes de cambiar nada.\n"
                  + "• Si la carpeta ya tiene una partida, eliges cuál se queda; la otra queda guardada en una copia, en save_backups.\n"
                  + "• El juego se reinicia para usar la carpeta: guarda antes."
                : "• Your save is copied to the folder before anything changes.\n"
                  + "• If the folder already has a save, you choose which one stays; the other is kept in a copy, in save_backups.\n"
                  + "• The game restarts to use the folder: save first.", true),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        columns.addView(column(es ? "EN DOS DISPOSITIVOS" : "ON TWO DEVICES", es
                ? "• Caza en uno a la vez.\n"
                  + "• Deja que la app de sincronización termine antes de abrir el juego en el otro.\n"
                  + "• Los dos comparten los mismos tres cazadores."
                : "• Hunt on one at a time.\n"
                  + "• Let the sync app finish before opening the game on the other.\n"
                  + "• Both share the same three hunters.", false),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        page.addView(columns);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        LinearLayout whole = new LinearLayout(this);
        whole.setOrientation(LinearLayout.VERTICAL);
        whole.setBackgroundColor(Look.WINE);
        whole.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        status = Look.text(this, "", 14, Look.GOLD, false);
        status.setGravity(Gravity.CENTER);
        status.setPadding(pad * 2, Look.dp(this, 4), pad * 2, Look.dp(this, 4));
        status.setVisibility(View.GONE);  // (no room to lose on a 4:3 screen until there is a message)
        whole.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, 0, 0, Look.dp(this, 10));
        choose = Look.button(this, "", true);
        choose.setOnClickListener(v -> choose());
        row.addView(choose, params(0));
        own = Look.button(this, es ? "Usar la carpeta propia del juego" : "Use the game's own folder", false);
        own.setOnClickListener(v -> backToOwn());
        row.addView(own, params(0));
        Button leave = Look.button(this, es ? "Cancelar" : "Cancel", false);
        leave.setOnClickListener(v -> { if (gate) backToGate(); else finish(); });
        row.addView(leave, params(120));
        whole.addView(row);
        whole.setOnApplyWindowInsetsListener((v, insets) -> {  // (the buttons stay above the system's bar)
            v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(),
                    insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(whole);
        refresh();
        choose.requestFocus();  // (a console's pad: A presses it)
    }

    private LinearLayout.LayoutParams params(int width) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(width > 0 ? Look.dp(this, width) : LinearLayout.LayoutParams.WRAP_CONTENT,
                Look.dp(this, 46));
        lp.setMargins(Look.dp(this, 6), 0, Look.dp(this, 6), 0);
        return lp;
    }

    private LinearLayout column(String title, String body, boolean framed) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        if (framed) {
            c.setBackground(Look.plate(this, Look.PANEL, 6));
            c.setPadding(Look.dp(this, 14), Look.dp(this, 10), Look.dp(this, 14), Look.dp(this, 10));
        } else {
            c.setPadding(Look.dp(this, 18), 0, 0, 0);
        }
        c.addView(Look.text(this, title, 13, Look.GOLD, true));
        TextView t = Look.text(this, body, 13, Look.CREAM, false);
        t.setPadding(0, Look.dp(this, 4), 0, 0);
        t.setLineSpacing(Look.dp(this, 1), 1);
        c.addView(t);
        return c;
    }

    private void refresh() {
        File f = SaveFolder.chosen(this);
        now.setText(Zh.t(f == null ? (es ? "Ahora: la carpeta propia del juego" : "Now: the game's own folder")
                              : (es ? "Ahora: " : "Now: ") + shown(f)));
        own.setVisibility(f == null ? View.GONE : View.VISIBLE);
        choose.setText(Zh.t(SaveFolder.hasAccess(this) ? (es ? "Elegir carpeta" : "Choose folder")
                                                  : (es ? "Dar permiso y elegir carpeta" : "Give permission and choose folder")));
    }

    /** A folder as the player knows it: from the storage's root. */
    private String shown(File f) {
        String p = f.getAbsolutePath(), root = android.os.Environment.getExternalStorageDirectory().getAbsolutePath();
        return p.startsWith(root + "/") ? p.substring(root.length() + 1) : p;
    }

    private void say(String s, boolean bad) {
        status.setTextColor(bad ? Look.ALERT : Look.GOLD);
        status.setText(Zh.t(s));
        status.setVisibility(View.VISIBLE);
    }

    // ---- the folder
    private void choose() {
        if (SaveFolder.hasAccess(this)) { browse(); return; }
        if (Build.VERSION.SDK_INT < 30) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE}, ACCESS);
            return;
        }
        waitingAccess = true;
        say(es ? "Activa el acceso a todos los archivos y vuelve. El juego solo escribe en la carpeta de partidas que elijas."
               : "Turn on access to all files and come back. The game only writes in the save folder you choose.", false);
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    /** From the start gate: the app opens again, and the gate looks at the folder again. */
    private void backToGate() {
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (launch != null) {
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(launch);
        }
        finish();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (gate) backToGate();
        else super.onBackPressed();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        // (the gate's "Give permission": once it is given and the folder is there, back to the game)
        File f = SaveFolder.chosen(this);
        if (gate && !committed && f != null && SaveFolder.hasAccess(this) && f.isDirectory() && f.canWrite()) {
            backToGate();
            return;
        }
        if (waitingAccess && SaveFolder.hasAccess(this)) {
            waitingAccess = false;
            status.setVisibility(View.GONE);
            browse();
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        refresh();
        if (code == ACCESS && SaveFolder.hasAccess(this)) browse();
    }

    private void browse() {
        File f = SaveFolder.chosen(this);
        File at = f != null ? (f.getName().equals(SaveFolder.SUB) ? f.getParentFile() : f) : null;
        FolderBrowser.open(this, at, es ? "ELIGE LA CARPETA DE PARTIDAS" : "CHOOSE THE SAVE FOLDER",
                SaveFolder::holdsSave,
                (dir, holds) -> holds ? (es ? "✓ Aquí hay una partida." : "✓ There is a save here.")
                                      : (es ? "Tu partida va en una carpeta llamada MHGU save, que se crea aquí."
                                            : "Your save goes in a folder named MHGU save, made here."),
                this::picked, this::systemPicker, true);
    }

    private void systemPicker() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK);
        } catch (ActivityNotFoundException e) {
            say(es ? "Este dispositivo no tiene selector de carpetas." : "This device has no folder picker.", true);
        }
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        File dir = FolderBrowser.pathOf(data.getData());
        if (dir == null) {
            say(es ? "Ese lugar no tiene una carpeta real detrás. Usa una carpeta que tu app de sincronización mantenga en el teléfono."
                   : "That place has no real folder behind it. Use a folder your sync app keeps on the phone.", true);
            return;
        }
        picked(dir);
    }

    private void picked(File dir) {
        String no = SaveFolder.refusal(this, dir, es);
        if (no != null) { say(no, true); return; }
        File to = SaveFolder.resolve(dir), from = SaveFolder.active(this);
        if (to.equals(SaveFolder.chosen(this))) {
            say(es ? "Esa ya es tu carpeta de partidas." : "That is your save folder already.", false);
            return;
        }
        SaveFolder.Info there = SaveFolder.describe(to), here = SaveFolder.describe(from);
        String where = shown(to);
        if (there.valid && here.valid) {
            Look.notice(this, es ? "ESTA CARPETA YA TIENE UNA PARTIDA" : "THIS FOLDER ALREADY HAS A SAVE",
                    (es ? "En la carpeta: " : "In the folder: ") + SaveFolder.line(there, es) + "\n"
                    + (es ? "En este dispositivo: " : "On this device: ") + SaveFolder.line(here, es) + "\n\n"
                    + (es ? "La que no elijas queda guardada en una copia." : "The one you don't choose is kept in a copy."),
                    new Look.Choice(es ? "Usar la de la carpeta" : "Use the folder's", true, () -> commit(to, true)),
                    new Look.Choice(es ? "Usar la de este dispositivo" : "Use this device's", false, () -> commit(to, false)),
                    new Look.Choice(es ? "Cancelar" : "Cancel", false, null));
            return;
        }
        String body = there.valid
                ? (es ? "Se usa la partida de " + where + ": " + SaveFolder.line(there, es) + "."
                      : "The save in " + where + " is used: " + SaveFolder.line(there, es) + ".")
                : here.valid
                ? (es ? "Tu partida (" + SaveFolder.line(here, es) + ") se copia a " + where + ", y el juego la usa desde ahí."
                      : "Your save (" + SaveFolder.line(here, es) + ") is copied to " + where + ", and the game uses it from there.")
                : (es ? "El juego guardará tu partida en " + where + "." : "The game will keep your save in " + where + ".");
        Look.notice(this, es ? "CARPETA DE PARTIDAS" : "SAVE FOLDER",
                body + "\n\n" + (es ? "El juego se reinicia: guarda antes lo que no quieras perder."
                                    : "The game restarts: save first if you have progress to keep."),
                new Look.Choice(es ? "Usar y reiniciar" : "Use it and restart", true, () -> commit(to, there.valid)),
                new Look.Choice(es ? "Cancelar" : "Cancel", false, null));
    }

    private void backToOwn() {
        File f = SaveFolder.chosen(this);
        if (f == null) return;
        SaveFolder.Info there = f.isDirectory() ? SaveFolder.describe(f) : new SaveFolder.Info();
        SaveFolder.Info here = SaveFolder.describe(SaveFolder.local(this));
        String title = es ? "USAR LA CARPETA PROPIA DEL JUEGO" : "USE THE GAME'S OWN FOLDER";
        String note = es ? "Los archivos de la carpeta de partidas quedan como están." : "The files in the save folder stay as they are.";
        if (there.valid && here.valid) {
            Look.notice(this, title,
                    (es ? "En la carpeta: " : "In the folder: ") + SaveFolder.line(there, es) + "\n"
                    + (es ? "En este dispositivo: " : "On this device: ") + SaveFolder.line(here, es) + "\n\n"
                    + (es ? "La que no elijas queda guardada en una copia. " : "The one you don't choose is kept in a copy. ") + note,
                    new Look.Choice(es ? "Usar la de la carpeta" : "Use the folder's", true, () -> commit(null, true)),
                    new Look.Choice(es ? "Usar la de este dispositivo" : "Use this device's", false, () -> commit(null, false)),
                    new Look.Choice(es ? "Cancelar" : "Cancel", false, null));
            return;
        }
        String body = there.valid
                ? (es ? "La partida de la carpeta (" + SaveFolder.line(there, es) + ") se copia a la carpeta propia del juego."
                      : "The folder's save (" + SaveFolder.line(there, es) + ") is copied to the game's own folder.")
                : (es ? "El juego vuelve a guardar en su propia carpeta." : "The game keeps its save in its own folder again.");
        Look.notice(this, title, body + " " + note + "\n\n" + (es ? "El juego se reinicia: guarda antes lo que no quieras perder."
                                                                 : "The game restarts: save first if you have progress to keep."),
                new Look.Choice(es ? "Usar y reiniciar" : "Use it and restart", true, () -> commit(null, there.valid)),
                new Look.Choice(es ? "Cancelar" : "Cancel", false, null));
    }

    /** The choice waits for the next start; the app restarts (the game must not save over what comes next). */
    private void commit(File folder, boolean keepFolder) {
        if (!SaveFolder.setPending(this, folder, keepFolder)) {
            say(es ? "No se pudo preparar el cambio. No se cambió nada." : "The change could not be prepared. Nothing was changed.", true);
            return;
        }
        committed = true;
        choose.setEnabled(false);
        own.setEnabled(false);
        say(es ? "Reiniciando..." : "Restarting...", false);
        restart(this);
    }

    /** The app opens again (PhoenixActivity waits for this process, which is killed: the game is paused behind it). */
    static void restart(Activity a) {
        Intent intent = new Intent(a, PhoenixActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(PhoenixActivity.EXTRA_PID, Process.myPid());
        a.startActivity(intent);
        new Handler(Looper.getMainLooper()).postDelayed(() -> Process.killProcess(Process.myPid()), 200);
    }

    // ================================================================== the start gate
    /**
     * Before the game starts (GameFolderActivity): a change chosen last time is applied, then the folder is checked;
     * each problem is shown in the game's style until there is none (or the player leaves), then `then` runs.
     */
    static void gate(Activity a, Runnable then) {
        boolean es = Look.es();
        String failed = SaveFolder.applyPending(a);
        if (failed != null) {
            Look.notice(a, es ? "NO SE CAMBIÓ LA CARPETA DE PARTIDAS" : "THE SAVE FOLDER WASN'T CHANGED",
                    (es ? "No se pudo cambiar: " : "It couldn't be changed: ") + failed + "\n\n"
                    + (es ? "No se cambió nada: tu partida sigue donde estaba." : "Nothing was changed: your save is where it was."),
                    new Look.Choice(es ? "Continuar" : "Continue", true, () -> gate(a, then)));
            return;
        }
        check(a, then, false);
    }

    private static void check(Activity a, Runnable then, boolean ignoreInUse) {
        boolean es = Look.es();
        SaveFolder.Problem p = SaveFolder.check(a, ignoreInUse);
        if (p == null) {
            SaveFolder.started(a);
            then.run();
            return;
        }
        Runnable again = () -> check(a, then, ignoreInUse);
        String path = p.folder.getAbsolutePath();
        String root = android.os.Environment.getExternalStorageDirectory().getAbsolutePath();
        if (path.startsWith(root + "/")) path = path.substring(root.length() + 1);
        SaveFolder.Info mine = SaveFolder.describe(SaveFolder.local(a));
        Look.Choice useMine = new Look.Choice(mine.valid ? (es ? "Usar la partida de este dispositivo (" + SaveFolder.when(mine.time) + ")"
                                                               : "Use this device's save (" + SaveFolder.when(mine.time) + ")")
                                                         : (es ? "Usar la carpeta propia del juego" : "Use the game's own folder"), false,
                () -> { SaveFolder.forget(a); check(a, then, ignoreInUse); });
        Look.Choice exit = new Look.Choice(es ? "Salir" : "Exit", false, a::finish);
        switch (p.kind) {
        case SaveFolder.NOT_FOUND:
            Look.notice(a, es ? "NO SE ENCUENTRA LA CARPETA DE PARTIDAS" : "SAVE FOLDER NOT FOUND",
                    es ? "El juego no llega a " + path + ": una tarjeta SD quitada, o una carpeta movida o renombrada. No se cambió nada."
                       : "The game can't reach " + path + ": an SD card taken out, or a folder moved or renamed. Nothing was changed.",
                    new Look.Choice(es ? "Reintentar" : "Try again", true, again),
                    new Look.Choice(es ? "Elegir carpeta" : "Choose folder", false, () -> {
                        a.startActivity(new Intent(a, SaveFolderActivity.class).putExtra(EXTRA_GATE, true));
                        a.finish();
                    }),
                    useMine, exit);
            break;
        case SaveFolder.NO_PERMISSION:
            Look.notice(a, es ? "EL JUEGO PERDIÓ EL PERMISO" : "THE GAME LOST ITS PERMISSION",
                    es ? "Para usar tu carpeta de partidas necesita de nuevo el acceso a todos los archivos."
                       : "To use your save folder it needs access to all files again.",
                    new Look.Choice(es ? "Dar permiso" : "Give permission", true, () -> {
                        a.startActivity(new Intent(a, SaveFolderActivity.class).putExtra(EXTRA_GATE, true));
                        a.finish();
                    }),
                    useMine, exit);
            break;
        case SaveFolder.EMPTY: {
            List<Look.Choice> c = new ArrayList<>();
            c.add(new Look.Choice(es ? "Reintentar" : "Try again", true, again));
            if (mine.valid)
                c.add(new Look.Choice(es ? "Poner ahí la partida de este dispositivo (" + SaveFolder.when(mine.time) + ")"
                                         : "Put this device's save there (" + SaveFolder.when(mine.time) + ")", false, () -> {
                    try {
                        SaveFolder.copySave(SaveFolder.local(a), p.folder);
                    } catch (IOException e) {
                        android.util.Log.e("MHGU", "save folder: " + e.getMessage());
                    }
                    again.run();
                }));
            c.add(exit);
            long had = 0;
            for (String l : AppHome.read(new File(SaveFolder.home(a), "save_folder_state.txt")).split("\n"))
                if (l.startsWith("time=")) try { had = Long.parseLong(l.substring(5).trim()); } catch (NumberFormatException ignored) {}
            String date = had > 0 ? SaveFolder.when(had) : "?";
            Look.notice(a, es ? "LA CARPETA DE PARTIDAS ESTÁ VACÍA" : "THE SAVE FOLDER IS EMPTY",
                    es ? "No hay ninguna partida en " + path + ", pero había una (" + date + "). Puede que tu app de sincronización "
                         + "todavía la esté trayendo: espera y vuelve a intentar."
                       : "There is no save in " + path + ", but there was one (" + date + "). Your sync app may still be bringing it: "
                         + "wait and try again.",
                    c.toArray(new Look.Choice[0]));
            break;
        }
        case SaveFolder.CONFLICT: {
            List<Look.Choice> c = new ArrayList<>();
            boolean first = true;
            for (File v : p.copies) {
                SaveFolder.Info i = SaveFolder.describeFile(v);
                String label = SaveFolder.line(i, es) + (v.getName().equals("system") ? "" : "  (" + v.getName() + ")");
                c.add(new Look.Choice(label, first, () -> {
                    try {
                        SaveFolder.keepVersion(a, p.folder, v);
                    } catch (IOException e) {
                        android.util.Log.e("MHGU", "save folder: " + e.getMessage());
                    }
                    again.run();
                }));
                first = false;
            }
            c.add(exit);
            Look.notice(a, es ? "DOS PARTIDAS EN LA CARPETA" : "TWO SAVES IN THE FOLDER",
                    es ? "Tu app de sincronización guardó dos versiones de la partida. Elige con cuál cazar; la otra queda guardada en una copia."
                       : "Your sync app kept two versions of the save. Choose the one to hunt with; the other is kept in a copy.",
                    c.toArray(new Look.Choice[0]));
            break;
        }
        case SaveFolder.MISSED: {
            String date = SaveFolder.when(SaveFolder.missedAt(a));
            SaveFolder.Info there = SaveFolder.describe(p.folder);
            Look.notice(a, es ? "GUARDADA EN ESTE DISPOSITIVO" : "SAVED ON THIS DEVICE",
                    es ? "La última vez no se pudo llegar a la carpeta de partidas, así que el juego guardó aquí (" + date + "). ¿Qué partida conservas?"
                       : "Last time the save folder couldn't be reached, so the game saved here (" + date + "). Which save do you keep?",
                    new Look.Choice((es ? "La de este dispositivo (" : "This device's (") + date + ")", true, () -> settle(a, true, again)),
                    new Look.Choice((es ? "La de la carpeta" : "The folder's") + (there.valid ? " (" + SaveFolder.when(there.time) + ")" : ""),
                            false, () -> settle(a, false, again)),
                    exit);
            break;
        }
        case SaveFolder.IN_USE:
            Look.notice(a, es ? "LA PARTIDA ESTÁ ABIERTA EN OTRO DISPOSITIVO" : "THE SAVE IS OPEN ON ANOTHER DEVICE",
                    es ? p.device + " la abrió a las " + SaveFolder.when(p.since).substring(11) + " y no la cerró. Cierra el juego ahí y "
                         + "deja que termine la sincronización, o se perderá una de las dos partidas."
                       : p.device + " opened it at " + SaveFolder.when(p.since).substring(11) + " and hasn't closed it. Close the game "
                         + "there and let the sync finish, or one of the two saves will be lost.",
                    new Look.Choice(es ? "Jugar igual" : "Play anyway", false, () -> check(a, then, true)),
                    exit);
            break;
        default:
            then.run();
        }
    }

    private static void settle(Activity a, boolean keepDevice, Runnable again) {
        try {
            SaveFolder.settleMissed(a, keepDevice);
        } catch (IOException e) {
            android.util.Log.e("MHGU", "save folder: " + e.getMessage());
        }
        again.run();
    }

}
