package com.mhgu.recomp;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
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
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * La entrada de la app: si los archivos del juego están a mano arranca el juego; si no, muestra qué hace falta y deja
 * elegir la carpeta donde están. El juego los lee desde ahí, sin copiarlos: la ruta queda en game_folder.txt (runtime/main.c).
 * También sirve data/ dentro de la carpeta de la app, como siempre. Ajustes -> Carpeta del juego la abre con EXTRA_CHANGE.
 * Mismas comprobaciones que runtime/main.c: si esa las rechaza deja game_folder_bad.txt y se vuelve a esta pantalla.
 */
public class GameFolderActivity extends Activity {
    static final String EXTRA_CHANGE = "change";
    private static final int PICK = 1, READ = 2;
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120);
    private static final int MIN_FILES = 35000;  // (1.4.0 has 35531; the margin as in runtime/main.c)
    private static final byte[] BUILD_ID = {(byte) 0xfb, 0x08, (byte) 0xf1, (byte) 0xd2, 0x0f, (byte) 0xd1, 0x20, 0x4f};

    private boolean es, change, waitingAccess;
    private TextView status;
    private Button pick;

    private File home() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }
    private File savedFile() { return new File(home(), "game_folder.txt"); }
    private File rejectedFile() { return new File(home(), "game_folder_bad.txt"); }
    private File ownLib() { return new File(getFilesDir(), "game/libmhgugame.so"); }

    /** The game code at hand: the player's, or in the APK (a build of one's own, -Pwithgame). */
    private boolean haveLib() {
        return ownLib().exists() || new File(getApplicationInfo().nativeLibraryDir, "libmhgugame.so").exists();
    }

    /** Which game code this version of the app runs with (native/build_native.sh). */
    private String wantedId() {
        try (java.io.InputStream in = getAssets().open("gamelib_id.txt")) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
        } catch (Exception e) {
            return "";
        }
    }

    private File saved() {
        try (FileInputStream in = new FileInputStream(savedFile())) {
            byte[] b = new byte[4096];
            int n = in.read(b);
            String path = n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
            return path.isEmpty() ? null : new File(path);
        } catch (Exception e) {
            return null;
        }
    }

    private static boolean looksRight(File dir) {
        return dir != null && new File(dir, "exefs/main").canRead() && new File(dir, "romfs").isDirectory();
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        declareGame();
        es = "es".equals(Locale.getDefault().getLanguage());
        change = getIntent().getBooleanExtra(EXTRA_CHANGE, false);
        boolean rejected = rejectedFile().exists();
        File chosen = saved(), own = new File(home(), "data");
        if (!change && !rejected && haveLib() && (looksRight(chosen) || (chosen == null && looksRight(own)))) {
            startGame();
            return;
        }
        build();
        if (rejected) {  // (runtime/main.c turned them down: say what is wrong with them)
            File bad = chosen != null ? chosen : own;
            verify(bad);
        } else if (chosen != null && !change) {
            status.setTextColor(ALERT);
            status.setText(hasAccess()
                    ? (es ? "Ya no se encuentra la carpeta " + chosen + ". Elige dónde está ahora." : "The folder " + chosen + " is gone. Choose where it is now.")
                    : (es ? "El juego perdió el permiso para leer la carpeta. Vuelve a darlo." : "The game lost its permission to read the folder. Please give it again."));
        }
    }

    /** Samsung ignora la categoría del manifiesto: cuenta la que fija quien instaló la app (Android solo se lo permite a él). */
    private void declareGame() {
        try {
            getPackageManager().setApplicationCategoryHint(getPackageName(), ApplicationInfo.CATEGORY_GAME);
            android.util.Log.i("MHGU", "category hint: game");
        } catch (RuntimeException e) {
            android.util.Log.i("MHGU", "category hint: " + e.getMessage());
        }
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        overridePendingTransition(0, 0);
        finish();
    }

    // ---- the screen
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

    /** The folder as it must look: folders in gold, what goes in them in cream. */
    private CharSequence tree() {
        String[] lines = {
            es ? "Tu carpeta" : "Your folder",
            "├─ exefs",
            "│    main",
            "│    main.npdm",
            "│    rtld, sdk, subsdk0",
            "├─ romfs",
            "│    nativeNX",
            es ? "│    ... 35.500 archivos" : "│    ... 35,500 files",
            "├─ libmhgugame.so",
            "└─ libmhgugame.id",
        };
        SpannableStringBuilder b = new SpannableStringBuilder();
        for (int i = 0; i < lines.length; i++) {
            int at = b.length();
            b.append(lines[i]).append(i + 1 < lines.length ? "\n" : "");
            boolean folder = i == 0 || i == 1 || i == 5 || i == 8;
            b.setSpan(new ForegroundColorSpan(folder ? GOLD : CREAM), at, b.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        return b;
    }

    private void build() {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        int pad = dp(14);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad * 2, pad, pad * 2, pad);

        TextView title = text(es ? "ARCHIVOS DEL JUEGO" : "GAME FILES", 20, GOLD, true);
        title.setGravity(Gravity.CENTER);
        page.addView(title);
        TextView intro = text(es ? "Elige la carpeta donde tienes tu copia del juego. Se lee desde ahí: no se copia nada."
                                 : "Choose the folder that holds your copy of the game. It is read from there: nothing is copied.",
                14, CREAM, false);
        intro.setGravity(Gravity.CENTER);
        intro.setPadding(0, dp(4), 0, dp(10));
        page.addView(intro);

        LinearLayout columns = new LinearLayout(this);
        columns.setOrientation(LinearLayout.HORIZONTAL);

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setBackground(plate(PANEL, 6));
        left.setPadding(pad, dp(10), pad, dp(10));
        left.addView(text(es ? "LA CARPETA DEBE TENER ESTO" : "THE FOLDER MUST LOOK LIKE THIS", 13, GOLD, true));
        TextView tree = new TextView(this);
        tree.setText(tree());
        tree.setTypeface(Typeface.MONOSPACE);
        tree.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        tree.setPadding(0, dp(6), 0, 0);
        left.addView(tree);
        columns.addView(left, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));

        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setPadding(dp(18), 0, 0, 0);
        right.addView(text(es ? "QUÉ TIENE QUE SER CADA UNA" : "WHAT EACH ONE MUST BE", 13, GOLD, true));
        TextView notes = text(es
                ? "• exefs: el de la actualización 1.4.0.\n"
                  + "• romfs: el del juego base, con la actualización 1.4.0 ya aplicada encima.\n"
                  + "• libmhgugame.so: se crea una sola vez en una PC con la herramienta de la guía.\n"
                  + "• Todo junto en la misma carpeta, unos 12 GB. Elige esa carpeta."
                : "• exefs: the one from the 1.4.0 update.\n"
                  + "• romfs: the base game's, with the 1.4.0 update already applied over it.\n"
                  + "• libmhgugame.so: made once on a PC with the guide's tool.\n"
                  + "• All in the same folder, about 12 GB. Choose that folder.", 14, CREAM, false);
        notes.setPadding(0, dp(6), 0, 0);
        notes.setLineSpacing(dp(2), 1);
        right.addView(notes);
        columns.addView(right, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        page.addView(columns);

        // (the page scrolls on a short or square screen; the message and the buttons stay in view)
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

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        pick = button("", true);
        pick.setOnClickListener(v -> choose());
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(46));
        size.setMargins(dp(8), 0, dp(8), 0);
        row.addView(pick, size);
        Button leave = button(change ? (es ? "Cancelar" : "Cancel") : (es ? "Salir" : "Exit"), false);
        leave.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams size2 = new LinearLayout.LayoutParams(dp(120), dp(46));
        size2.setMargins(dp(8), 0, dp(8), 0);
        row.addView(leave, size2);
        row.setPadding(0, 0, 0, dp(10));
        whole.addView(row);
        whole.setOnApplyWindowInsetsListener((v, insets) -> {  // (the buttons stay above the system's bar)
            v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(),
                    insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(whole);
        pick.requestFocus();  // (a console's pad: A presses it)
        refreshButton();
    }

    private void refreshButton() {
        if (pick != null) pick.setText(hasAccess() ? (es ? "Elegir carpeta" : "Choose folder")
                                                   : (es ? "Dar permiso y elegir carpeta" : "Give permission and choose folder"));
    }

    // ---- reading any folder: the plain file paths the game uses need this permission
    private boolean hasAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void choose() {
        if (hasAccess()) { openPicker(); return; }
        if (Build.VERSION.SDK_INT < 30) {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE}, READ);
            return;
        }
        waitingAccess = true;
        status.setTextColor(GOLD);
        status.setText(es ? "Activa el acceso a todos los archivos y vuelve. El juego solo lee esa carpeta: no cambia nada."
                          : "Turn on access to all files and come back. The game only reads that folder: it changes nothing.");
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshButton();
        if (waitingAccess && hasAccess()) {
            waitingAccess = false;
            status.setText("");
            openPicker();
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        if (code == READ && hasAccess()) openPicker();
        refreshButton();
    }

    private void openPicker() {
        openBrowser(null);
    }

    /** Android's own folder picker: the browser's second way (some show nothing at the root, or refuse it). */
    private void systemPicker() {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK);
        } catch (ActivityNotFoundException e) {
            status.setTextColor(ALERT);
            status.setText(es ? "Este dispositivo no tiene selector de carpetas." : "This device has no folder picker.");
        }
    }

    // ---- the app's own folder browser. Android's picker (HyperOS on a POCO F6, seen twice) showed the storage's root
    // empty and refused it; with access to all files the folders can be listed here, in the game's style
    private android.app.Dialog browser;

    /** The storage roots: the phone's own and each SD card's (from the app's folders on them). */
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

    private boolean holdsGame(File d) {
        return new File(d, "exefs").isDirectory() && new File(d, "romfs").isDirectory();
    }

    private void openBrowser(File at) {
        if (browser != null) browser.dismiss();
        List<File> roots = roots();
        int pad = dp(12);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(plate(PANEL, 8));
        box.setPadding(pad, pad, pad, pad);
        TextView title = text(es ? "ELIGE LA CARPETA DEL JUEGO" : "CHOOSE THE GAME'S FOLDER", 17, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        // the roots, as tabs
        LinearLayout tabs = new LinearLayout(this);
        tabs.setGravity(Gravity.CENTER);
        for (int i = 0; i < roots.size(); i++) {
            File r = roots.get(i);
            boolean on = at != null ? at.getAbsolutePath().startsWith(r.getAbsolutePath()) : i == 0;
            Button t = button(i == 0 ? (es ? "Memoria interna" : "Phone storage")
                                     : (es ? "Tarjeta SD" : "SD card") + (roots.size() > 2 ? " " + i : ""), on);
            t.setOnClickListener(v -> openBrowser(r));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(40));
            lp.setMargins(dp(4), dp(6), dp(4), dp(4));
            tabs.addView(t, lp);
        }
        box.addView(tabs);
        File dir = at != null ? at : roots.get(0);
        File root = roots.get(0);
        for (File r : roots) if (dir.getAbsolutePath().startsWith(r.getAbsolutePath())) root = r;
        String shown = dir.getAbsolutePath().substring(root.getAbsolutePath().length());
        TextView where = text((shown.isEmpty() ? "/" : shown), 13, CREAM, false);
        where.setPadding(dp(4), dp(2), dp(4), dp(4));
        box.addView(where);
        boolean game = holdsGame(dir);
        TextView hint = text(game ? (es ? "✓ Aquí está el juego: exefs y romfs." : "✓ The game is here: exefs and romfs.")
                                  : (es ? "Entra a la carpeta que tiene exefs y romfs." : "Go into the folder that holds exefs and romfs."),
                13, game ? GOLD : CREAM, game);
        hint.setPadding(dp(4), 0, dp(4), dp(6));
        box.addView(hint);
        // the folders here
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        final File here = dir, top = root;
        if (!dir.equals(root)) {
            Button up = button("⬑  " + (es ? "Subir" : "Up"), false);
            up.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            up.setOnClickListener(v -> openBrowser(here.getParentFile()));
            list.addView(up, rowParams());
        }
        File[] kids = dir.listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (kids != null) {
            Arrays.sort(kids, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File k : kids) {
                boolean has = holdsGame(k);
                Button b = button((has ? "✓  " : "▸  ") + k.getName(), has);
                b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                b.setOnClickListener(v -> openBrowser(k));
                list.addView(b, rowParams());
            }
        }
        if (kids == null || kids.length == 0) {
            TextView none = text(es ? "(no hay carpetas)" : "(no folders)", 13, CREAM, false);
            none.setPadding(dp(6), dp(8), dp(6), dp(8));
            list.addView(none);
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(list);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        // use it, Android's picker, cancel
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(8), 0, 0);
        Button use = button(es ? "Usar esta carpeta" : "Use this folder", true);
        use.setOnClickListener(v -> { browser.dismiss(); verify(here); });
        Button other = button(es ? "Selector de Android" : "Android's picker", false);
        other.setOnClickListener(v -> { browser.dismiss(); systemPicker(); });
        Button cancel = button(es ? "Cancelar" : "Cancel", false);
        cancel.setOnClickListener(v -> browser.dismiss());
        for (Button b : new Button[] {use, other, cancel}) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
            lp.setMargins(dp(5), 0, dp(5), 0);
            row.addView(b, lp);
        }
        box.addView(row);
        browser = new android.app.Dialog(this);
        browser.requestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
        browser.setContentView(box);
        android.view.Window w = browser.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
            android.util.DisplayMetrics m = getResources().getDisplayMetrics();
            w.setLayout(Math.min(dp(640), (int) (m.widthPixels * .92f)), (int) (m.heightPixels * .92f));
        }
        browser.show();
        use.requestFocus();
    }

    private LinearLayout.LayoutParams rowParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(42));
        lp.setMargins(0, dp(3), 0, dp(3));
        return lp;
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        File dir = pathOf(data.getData());
        if (dir == null) {
            status.setTextColor(ALERT);
            status.setText(es ? "No se puede usar esa ubicación. Busca la carpeta en la memoria del teléfono o en la tarjeta SD."
                              : "That location can't be used. Find the folder in the phone's storage or on the SD card.");
            return;
        }
        verify(dir);
    }

    /** A picked folder as a plain path (null: a provider with no real folder behind it). */
    private File pathOf(Uri tree) {
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

    // ---- is it the right thing? (a background thread: the walk over romfs takes seconds)
    private void verify(File picked) {
        pick.setEnabled(false);
        status.setTextColor(GOLD);
        status.setText(es ? "Comprobando los archivos..." : "Checking the files...");
        new Thread(() -> {
            File[] ways = {picked, new File(picked, "data"), picked.getParentFile()};  // (or one level off)
            // (the game code changed folders or versions: what is kept goes with the new choice)
            File dir = picked;
            for (File w : ways)
                if (w != null && new File(w, "exefs").isDirectory() && new File(w, "romfs").isDirectory()) { dir = w; break; }
            final File found = dir;
            String wrong = problem(found);
            if (wrong == null) {
                try {
                    keepLib(found);
                } catch (Exception e) {
                    wrong = String.valueOf(e);
                }
            }
            final String problem = wrong;
            runOnUiThread(() -> {
                pick.setEnabled(true);
                if (problem != null) {
                    status.setTextColor(ALERT);
                    status.setText(problem);
                } else {
                    accept(found);
                }
            });
        }).start();
    }

    private String problem(File dir) {
        File exefs = new File(dir, "exefs"), romfs = new File(dir, "romfs"), main = new File(exefs, "main");
        if (!exefs.isDirectory() && !romfs.isDirectory())
            return es ? "En " + dir.getName() + " no están exefs ni romfs. Elige la carpeta que las contiene."
                      : "exefs and romfs are not in " + dir.getName() + ". Choose the folder that holds them.";
        if (!exefs.isDirectory()) return es ? "Falta la carpeta exefs, al lado de romfs." : "The exefs folder is missing, beside romfs.";
        if (!romfs.isDirectory()) return es ? "Falta la carpeta romfs, al lado de exefs." : "The romfs folder is missing, beside exefs.";
        if (!main.canRead()) return es ? "En exefs falta el archivo main." : "The file main is missing from exefs.";
        byte[] head = new byte[0x48];
        try (FileInputStream in = new FileInputStream(main)) {
            if (in.read(head) != head.length) head = new byte[0x48];
        } catch (Exception e) {
            return es ? "No se pudo leer exefs/main." : "exefs/main could not be read.";
        }
        boolean nso = head[0] == 'N' && head[1] == 'S' && head[2] == 'O' && head[3] == '0';
        if (!nso || !Arrays.equals(Arrays.copyOfRange(head, 0x40, 0x48), BUILD_ID))
            return es ? "Este exefs no es de la versión 1.4.0. Usa el de la actualización 1.4.0."
                      : "This exefs is not version 1.4.0. Use the one from the 1.4.0 update.";
        int files = countFiles(romfs);
        if (files < MIN_FILES)
            return es ? "romfs está incompleto: tiene " + files + " archivos y deberían ser 35.531. Usa el juego base con la actualización 1.4.0 ya aplicada."
                      : "romfs is incomplete: it has " + files + " files where there should be 35,531. Use the base game with the 1.4.0 update applied.";
        boolean bundled = new File(getApplicationInfo().nativeLibraryDir, "libmhgugame.so").exists();
        File lib = new File(dir, "libmhgugame.so");
        if (!lib.canRead() && !bundled)
            return es ? "Falta libmhgugame.so en esa carpeta. Créalo en una PC con la herramienta de la guía."
                      : "libmhgugame.so is missing from that folder. Make it on a PC with the guide's tool.";
        if (!bundled && !wantedId().equals(idBeside(dir)))
            return es ? "Este libmhgugame.so es de otra versión. Créalo de nuevo con la herramienta de esta versión."
                      : "This libmhgugame.so is from another version. Make it again with this version's tool.";
        if (!new File(romfs, "nativeNX/system/app_shader/AppShaderPackage.mfx").exists())
            return es ? "En romfs falta nativeNX/system/app_shader/AppShaderPackage.mfx. Usa el juego base con la actualización 1.4.0 ya aplicada."
                      : "nativeNX/system/app_shader/AppShaderPackage.mfx is missing from romfs. Use the base game with the 1.4.0 update applied.";
        return null;
    }

    private String idBeside(File dir) {
        try (FileInputStream in = new FileInputStream(new File(dir, "libmhgugame.id"))) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
        } catch (Exception e) {
            return "";
        }
    }

    /** The game code into the app's private folder, read-only: Android runs nothing from shared storage. */
    private void keepLib(File dir) throws java.io.IOException {
        File from = new File(dir, "libmhgugame.so");
        if (!from.canRead()) return;  // (a build with it in the APK)
        File to = ownLib(), tmp = new File(to.getParentFile(), "libmhgugame.tmp");
        to.getParentFile().mkdirs();
        tmp.delete();
        try (FileInputStream in = new FileInputStream(from); FileOutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        to.delete();
        if (!tmp.renameTo(to)) throw new java.io.IOException("libmhgugame.so");
        to.setReadOnly();
    }

    private int countFiles(File top) {
        int n = 0;
        ArrayDeque<File> dirs = new ArrayDeque<>();
        dirs.add(top);
        while (!dirs.isEmpty()) {
            File[] list = dirs.poll().listFiles();
            if (list == null) continue;
            for (File f : list) {
                if (f.isDirectory()) dirs.add(f);
                else n++;
            }
        }
        return n;
    }

    private void accept(File dir) {
        try (FileOutputStream out = new FileOutputStream(savedFile())) {
            out.write(dir.getAbsolutePath().getBytes("UTF-8"));
        } catch (Exception e) {
            status.setTextColor(ALERT);
            status.setText(String.valueOf(e));
            return;
        }
        rejectedFile().delete();
        if (change) {
            status.setTextColor(GOLD);
            status.setText(es ? "Listo: se usará la próxima vez que abras el juego." : "Done: it will be used the next time you open the game.");
            pick.setEnabled(false);
            return;
        }
        startGame();
    }
}
