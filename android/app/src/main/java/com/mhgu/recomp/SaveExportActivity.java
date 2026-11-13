package com.mhgu.recomp;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Ajustes -> Exportar partida: copia la partida a una carpeta que elige el jugador, para llevarla a un emulador o a otro
 * dispositivo. Se copia lo último que el juego guardó en el archivo; la app no toca la partida. Los archivos tienen el
 * mismo formato que en la consola y en los emuladores, y son los que SaveImportActivity acepta de vuelta.
 */
public class SaveExportActivity extends Activity {
    private static final String TAG = "MHGU";
    private static final int PICK = 1;
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120);
    private static final String[] FILES = {"system", "system_backup"};

    private boolean es;
    private TextView status;
    private Button main;

    private File saves() { return SaveFolder.active(this); }  // (the save folder's, when one is chosen)

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

    private Button button(String s, boolean strong) {
        Button b = new Button(this);
        b.setText(Zh.t(s));
        b.setAllCaps(false);
        b.setTextColor(strong ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        if (strong) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(BRONZE));
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
        TextView title = text(es ? "EXPORTAR PARTIDA" : "EXPORT YOUR SAVE", 20, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        TextView body = text(es
                ? "Elige una carpeta y ahí se crea otra con tus archivos system y system_backup. Se copia lo último que guardaste en el juego.\n\n"
                  + "Para usarla en un emulador, pásala a tu PC y copia los dos archivos a la carpeta de partida del juego:\n"
                  + "• Ryujinx: clic derecho en el juego, Open User Save Directory.\n"
                  + "• yuzu: clic derecho en el juego, Open Save Data Location.\n"
                  + "Antes, guarda una copia de lo que haya ahí."
                : "Choose a folder and a new one is made there with your files system and system_backup. It copies what you last saved in the game.\n\n"
                  + "To use it on an emulator, move it to your PC and copy both files to the game's save folder:\n"
                  + "• Ryujinx: right-click the game, Open User Save Directory.\n"
                  + "• yuzu: right-click the game, Open Save Data Location.\n"
                  + "Keep a copy of what is there first.", 14, CREAM, false);
        body.setPadding(0, dp(10), 0, dp(10));
        body.setLineSpacing(dp(1), 1);
        box.addView(body);
        status = text("", 14, GOLD, true);
        status.setGravity(Gravity.CENTER);
        box.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(12), 0, 0);
        main = button(es ? "Elegir carpeta" : "Choose a folder", true);
        main.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK));
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(190), dp(46));
        size.setMargins(dp(8), 0, dp(8), 0);
        row.addView(main, size);
        Button close = button(es ? "Cerrar" : "Close", false);
        close.setOnClickListener(v -> finish());
        row.addView(close, new LinearLayout.LayoutParams(size));
        box.addView(row);

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

        if (!new File(saves(), FILES[0]).isFile()) {
            main.setEnabled(false);
            say(es ? "Todavía no hay ninguna partida guardada." : "There is no saved game yet.", true);
        }
    }

    private void say(String s, boolean bad) {
        status.setText(Zh.t(s));
        status.setTextColor(bad ? ALERT : GOLD);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        main.setEnabled(false);
        say(es ? "Copiando…" : "Copying…", false);
        new Thread(() -> {
            String folder = null, error = null;
            try {
                folder = export(tree);
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "save export: " + e);
                error = e.getMessage();
            }
            final String done = folder, why = error;
            runOnUiThread(() -> {
                main.setEnabled(true);
                if (done != null) say((es ? "Listo: carpeta " : "Done: folder ") + done, false);
                else say((es ? "No se pudo copiar: " : "Could not copy it: ") + why, true);
            });
        }).start();
    }

    /** A new folder inside the chosen one, with the save's files; their sizes checked. Its name. */
    private String export(Uri tree) throws IOException {
        ContentResolver cr = getContentResolver();
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        String name = "MHGU_save_" + new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(new Date());
        Uri dir = DocumentsContract.createDocument(cr, parent, DocumentsContract.Document.MIME_TYPE_DIR, name);
        if (dir == null) throw new IOException(es ? "esa carpeta no admite archivos nuevos" : "that folder takes no new files");
        for (String file : FILES) {
            File from = new File(saves(), file);
            if (!from.isFile()) continue;
            Uri to = DocumentsContract.createDocument(cr, dir, "application/octet-stream", file);
            if (to == null) throw new IOException(file);
            long copied = 0;
            try (FileInputStream in = new FileInputStream(from); OutputStream out = cr.openOutputStream(to)) {
                if (out == null) throw new IOException(file);
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; copied += n) out.write(buf, 0, n);
            }
            if (copied != from.length()) throw new IOException(file);
        }
        Log.i(TAG, "save export: " + name);
        return name;
    }
}
