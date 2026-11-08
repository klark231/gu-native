package com.mhgu.recomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Le entrega el registro armado por LogShareActivity a la app que el jugador eligió, y nada más: un solo archivo, de
 * solo lectura, sólo con el permiso que da el envío. En el proceso ":share", que sigue vivo aunque el juego se haya
 * cerrado por un error.
 */
public class LogProvider extends ContentProvider {
    private static final String NAME = "mhgu-log.txt";  // (the authority: the package's own + ".logs", as the manifest says)

    static File report(Context c) { return new File(c.getCacheDir(), NAME); }
    static Uri uri(Context c) { return Uri.parse("content://" + c.getPackageName() + ".logs/" + NAME); }

    @Override
    public boolean onCreate() { return true; }

    @Override
    public String getType(Uri uri) { return "text/plain"; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(report(getContext()), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        String[] columns = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++)
            row[i] = OpenableColumns.DISPLAY_NAME.equals(columns[i]) ? NAME
                   : OpenableColumns.SIZE.equals(columns[i]) ? (Object) report(getContext()).length() : null;
        MatrixCursor c = new MatrixCursor(columns, 1);
        c.addRow(row);
        return c;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) { return null; }

    @Override
    public int delete(Uri uri, String selection, String[] args) { return 0; }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
