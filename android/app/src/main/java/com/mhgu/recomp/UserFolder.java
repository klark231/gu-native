package com.mhgu.recomp;

import android.Manifest;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;

/**
 * The user folder: where everything of the player's lives (mhgurecomp.ini, MHGURecomp/save, mods/, cheats/, data/, log.txt...),
 * like Azahar's user directory. By default Android/data/com.mhgu.recomp/files; the player can point it at any folder
 * (UserFolderActivity). The choice is kept in user_folder.txt in the app's private folder, which runtime/main.c reads
 * too (android_env): both sides must use the same file and the same rule (a folder that exists and can be written).
 */
final class UserFolder {
    private static final String POINTER = "user_folder.txt";   // the chosen folder (absent: the default)
    private static final String PENDING = "user_folder_next.txt";  // a change waiting for the next start: path, then how
    private static final String PREFS = "mhgu", ASKED = "user_folder_asked";

    private UserFolder() {}

    /** The default: the app's external files folder. */
    static File defaultHome(Context c) {
        File f = c.getExternalFilesDir(null);
        return f != null ? f : c.getFilesDir();
    }

    static boolean usable(File dir) {
        return dir != null && dir.isDirectory() && dir.canWrite();
    }

    /** The folder the player chose, or null for the default. It may be gone or unwritable: see lost(). */
    static File chosen(Context c) {
        String path = read(new File(c.getFilesDir(), POINTER)).trim();
        return path.isEmpty() ? null : new File(path);
    }

    /** Where the player's files are now (what the game uses; the same rule as runtime/main.c). */
    static File home(Context c) {
        File ch = chosen(c);
        return usable(ch) ? ch : defaultHome(c);
    }

    /** A folder was chosen but cannot be used now (an SD card that was taken out, the permission taken back). */
    static boolean lost(Context c) {
        File ch = chosen(c);
        return ch != null && !usable(ch);
    }

    /** null: back to the default. */
    static void setChosen(Context c, File dir) {
        File p = new File(c.getFilesDir(), POINTER);
        if (dir == null) p.delete();
        else write(p, dir.getAbsolutePath());
    }

    static boolean asked(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(ASKED, false);
    }

    static void markAsked(Context c) {
        SharedPreferences.Editor e = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit();
        e.putBoolean(ASKED, true).apply();
    }

    // ---- a change that is applied at the next start (the game that is running still writes to the old folder)
    static final String COPY = "copy", KEEP_GAME = "keepgame", EMPTY = "empty";

    static void setPending(Context c, File target /* null: the default */, String how) {
        write(new File(c.getFilesDir(), PENDING), (target == null ? "" : target.getAbsolutePath()) + "\n" + how);
    }

    static boolean hasPending(Context c) {
        return new File(c.getFilesDir(), PENDING).exists();
    }

    static void clearPending(Context c) {
        new File(c.getFilesDir(), PENDING).delete();
    }

    /** {path (empty: the default), how} */
    static String[] pending(Context c) {
        String[] lines = read(new File(c.getFilesDir(), PENDING)).split("\n", -1);
        return new String[] {lines[0].trim(), lines.length > 1 ? lines[1].trim() : EMPTY};
    }

    // ---- reading any folder: the plain file paths the game uses need this permission
    static boolean hasAccess(Context c) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return c.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && c.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** A picked folder as a plain path (null: a provider with no real folder behind it). */
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

    /** Can files be created in it? (the permission can be missing, or the volume read-only) */
    static boolean canCreateIn(File dir) {
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) return false;
            File t = new File(dir, ".mhgu_write_test");
            try (FileOutputStream out = new FileOutputStream(t)) {
                out.write(1);
            }
            return t.delete();
        } catch (Exception e) {
            return false;
        }
    }

    /** Is one folder the other, or inside it? (copying a folder into itself never ends) */
    static boolean nested(File a, File b) {
        try {
            String x = a.getCanonicalPath() + "/", y = b.getCanonicalPath() + "/";
            return x.startsWith(y) || y.startsWith(x);
        } catch (Exception e) {
            return true;
        }
    }

    /** Does the folder already hold the player's files (a folder from before: a reinstall, another phone)? */
    static boolean hasUserFiles(File dir) {
        return new File(dir, "mhgurecomp.ini").exists() || new File(dir, "MHGURecomp").isDirectory()
                || new File(dir, "data/exefs").isDirectory() || new File(dir, "game_folder.txt").exists();
    }

    static String read(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] b = new byte[4096];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, "UTF-8") : "";
        } catch (Exception e) {
            return "";
        }
    }

    static void write(File f, String s) {
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes("UTF-8"));
        } catch (Exception e) {
            android.util.Log.w("MHGU", f.getName() + ": " + e);
        }
    }
}
