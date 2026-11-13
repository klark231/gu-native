package com.mhgu.recomp;

import android.content.Context;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * The app's own folder: settings, log, caches, game_folder.txt and the saves (MHGURecomp/). It is the external files
 * folder, Android/data/com.mhgu.recomp/files, unless the app can't write there.
 *
 * On Android 11 and later a file or folder made in Android/data by another app (a file manager working through
 * Shizuku, or adb) keeps that app as its owner, and nothing in this app can write it again (gu-native #176: "open
 * failed: EACCES" saving game_folder.txt). ensure(), at every start before anything is written:
 *  - gives back to the app what it can: a file it can't write, in a folder it can, is copied and put in its place
 *    (now the app's); a folder it can't write is renamed aside (<name>.other-app-<date>) and made again with copies;
 *  - if the folder itself can't be written (EACCES / EPERM, never a full or read-only storage, never a missing mount),
 *    moves to the private folder (files/ of the app's internal storage) for good: home.txt there says so, readable
 *    files of the old folder are copied (copied, never moved), and the player is told once (movedNotice).
 * runtime/main.c reads home.txt too, so the game and these screens always use the same folder.
 */
final class AppHome {
    private static final String TAG = "MHGU";
    private static final String MARK = "home.txt", MOVED = "home_moved.txt";
    // (what is never copied or repaired: the game's own files, big and only read, and what is made again)
    private static final String[] SKIP = {"data", "cache", "shots", "mods", "dlc"};

    private AppHome() {}

    /** The folder: the private one once ensure() moved there, else the external one (the private one with no external storage). */
    static File home(Context c) {
        File moved = chosen(c);
        if (moved != null) return moved;
        File f = c.getExternalFilesDir(null);
        return f != null ? f : c.getFilesDir();
    }

    /** The external files folder, where game files may have been put by hand (data/): null when there is none. */
    static File external(Context c) { return c.getExternalFilesDir(null); }

    private static File chosen(Context c) {
        File mark = new File(c.getFilesDir(), MARK);
        if (!mark.isFile()) return null;
        String path = read(mark);
        return path.isEmpty() ? null : new File(path);
    }

    /** At the start, before anything is written (GameFolderActivity, and GameActivity for a start without it). */
    static synchronized void ensure(Context c) {
        File moved = chosen(c);
        if (moved != null) {
            keepGameFiles(c, moved);
            return;
        }
        File ext = c.getExternalFilesDir(null);
        if (ext == null) return;  // (no external storage at this moment: the private folder for this run, nothing decided)
        int err = probe(ext);
        if (err == 0) {
            repair(ext, 0, new int[] {0});
            return;
        }
        if (err != OsConstants.EACCES && err != OsConstants.EPERM) {
            Log.w(TAG, "home: " + ext + " can't be written (" + OsConstants.errnoName(err) + "): left as it is");
            return;
        }
        File own = c.getFilesDir();
        Log.w(TAG, "home: " + ext + " belongs to another app (" + OsConstants.errnoName(err) + "): the private folder " + own + " is used");
        int[] count = {0};
        File[] kids = ext.listFiles();
        if (kids != null)
            for (File k : kids) if (!skipped(k.getName())) copyTree(k, new File(own, k.getName()), count);
        writeText(new File(own, MOVED), ext.getAbsolutePath());
        writeText(new File(own, MARK), own.getAbsolutePath());
        Log.i(TAG, "home: " + count[0] + " file(s) copied from " + ext);
        keepGameFiles(c, own);
    }

    /**
     * Once the home is the private folder: game files put by hand in the external folder's data/ (where the game looks
     * when no folder was chosen, and GameFolderActivity accepts them) are still read there, so game_folder.txt in the
     * private folder points at them. Without it runtime/main.c would look in the private folder's data/, which nobody
     * can fill, and say the game files were not found. A folder chosen before (its game_folder.txt was copied) stays.
     */
    private static void keepGameFiles(Context c, File home) {
        File ext = c.getExternalFilesDir(null), saved = new File(home, "game_folder.txt");
        if (ext == null || home.equals(ext) || !read(saved).isEmpty()) return;
        File data = new File(ext, "data");
        if (!new File(data, "exefs/main").canRead()) return;
        if (writeText(saved, data.getAbsolutePath())) Log.i(TAG, "home: the game files stay in " + data + " (game_folder.txt)");
    }

    /** Once after a move: the old folder (shown to the player), else null. The note is gone afterwards. */
    static String movedNotice(Context c) {
        File f = new File(c.getFilesDir(), MOVED);
        if (!f.isFile()) return null;
        String old = read(f);
        f.delete();
        return old.isEmpty() ? "Android/data/" + c.getPackageName() : old;
    }

    private static boolean skipped(String name) {
        for (String s : SKIP) if (s.equals(name)) return true;
        return name.startsWith(".") || name.endsWith(".tmp");
    }

    /** 0: a new file can be made and deleted in dir; else the errno that stopped it. */
    static int probe(File dir) {
        File p = new File(dir, ".mhgu-probe-" + android.os.Process.myPid());
        try {
            FileDescriptor fd = Os.open(p.getAbsolutePath(), OsConstants.O_CREAT | OsConstants.O_EXCL | OsConstants.O_WRONLY, 0600);
            Os.close(fd);
            Os.remove(p.getAbsolutePath());
            return 0;
        } catch (ErrnoException e) {
            p.delete();
            return e.errno;
        }
    }

    // (the folders the app itself writes in: only these are repaired, with the files at the top. Anything else a player or
    // adb put there - a backup, a test folder - is never touched)
    private static final String[] OWNED = {"MHGURecomp", "save_import", "cheats", "cache", "shots"};

    private static boolean owned(String name) {
        for (String s : OWNED) if (s.equals(name)) return true;
        return false;
    }

    /** What another app made in the folder becomes the app's again (see above); a few levels, a few thousand files. */
    private static void repair(File dir, int depth, int[] seen) {
        File[] kids = dir.listFiles();
        if (kids == null || depth > 5) return;
        String stamp = null;
        for (File k : kids) {
            if (++seen[0] > 4000) return;
            String name = k.getName();
            if (name.endsWith(".other-app.tmp") || name.endsWith(".mine.tmp")) {
                // (a replacement cut short, the name left empty: a whole file back under it, the app's own copy first. The
                // original is moved aside only once the copy is whole; a copy with no original beside it was cut short)
                String base = name.substring(0, name.lastIndexOf('.', name.length() - 5));
                File was = new File(dir, base), mine = new File(dir, base + ".mine.tmp"), theirs = new File(dir, base + ".other-app.tmp");
                File back = mine.isFile() ? mine : theirs;
                if (!was.exists() && theirs.isFile() && back.renameTo(was)) Log.w(TAG, "home: " + was + " put back from " + back.getName());
                continue;
            }
            if (name.contains(".other-app-") || name.endsWith(".tmp")) continue;
            if (depth == 0 && k.isDirectory() && !owned(k.getName())) continue;
            if (k.isDirectory()) {
                int err = probe(k);
                if (err == OsConstants.EACCES || err == OsConstants.EPERM) {
                    if (stamp == null) stamp = new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(new Date());
                    File aside = new File(dir, k.getName() + ".other-app-" + stamp);
                    if (k.renameTo(aside)) {
                        int[] n = {0};
                        copyTree(aside, k, n);
                        Log.w(TAG, "home: " + k + " belonged to another app: made again (" + n[0] + " file(s)); the old one is " + aside.getName());
                    } else {
                        Log.w(TAG, "home: " + k + " belongs to another app and can't be moved aside");
                    }
                } else {
                    repair(k, depth + 1, seen);
                }
            } else if (!k.canWrite() && k.canRead()) {
                // (the copy made, the original moved aside, the copy put in its place, and only then the original deleted:
                // whatever fails, a whole file keeps the name, or the copy and the original both stay beside it for the next
                // start. Never the only copy deleted: such a file can be the save)
                File tmp = new File(dir, name + ".mine.tmp"), aside = new File(dir, name + ".other-app.tmp");
                boolean whole = true;  // (the name holds a whole file: the original, or the app's copy)
                try {
                    copy(k, tmp);
                    if (!k.renameTo(aside)) throw new IOException("can't move it aside");
                    whole = tmp.renameTo(k);
                    if (!whole) {
                        whole = aside.renameTo(k);
                        if (whole) throw new IOException("can't put the copy in its place (the original stays)");
                        // (neither takes the name back: the copy is copied under it, or this start's game finds no file there
                        // and may make a new one, which the next start would not replace)
                        String both = "the original is " + aside.getName() + ", the copy " + tmp.getName();
                        if (k.exists()) throw new IOException("can't put the copy in its place, something else took the name: " + both);
                        try {
                            copy(tmp, k);
                        } catch (IOException e) {
                            k.delete();  // (never a copy cut short under the name: the next start puts a whole one back)
                            throw new IOException("can't put the copy in its place (" + e.getMessage() + "): " + both);
                        }
                        whole = true;
                        Log.w(TAG, "home: " + k + ": the copy couldn't be renamed, so it was copied under the name");
                        if (!tmp.delete()) Log.w(TAG, "home: " + tmp + " couldn't be deleted");
                    }
                    if (!aside.delete()) Log.w(TAG, "home: " + aside + " (the other app's original) couldn't be deleted");
                    Log.w(TAG, "home: " + k + " belonged to another app: replaced by the app's own copy");
                } catch (IOException e) {
                    if (whole) tmp.delete();  // (only with a whole file under the name: never the only copy left)
                    Log.w(TAG, "home: " + k + " belongs to another app: " + e.getMessage());
                }
            }
        }
    }

    private static void copyTree(File from, File to, int[] count) {
        if (from.isDirectory()) {
            to.mkdirs();
            File[] list = from.listFiles();
            if (list != null) for (File f : list) copyTree(f, new File(to, f.getName()), count);
            return;
        }
        if (!from.canRead() || to.exists()) return;
        // (through a temporary file: a copy cut short - the app closed meanwhile - is never taken for a whole one by the
        // next start, which skips what is there)
        File tmp = new File(to.getParentFile(), to.getName() + ".copy.tmp");
        try {
            copy(from, tmp);
            if (!tmp.renameTo(to)) throw new IOException("can't rename " + tmp.getName());
            count[0]++;
        } catch (IOException e) {
            tmp.delete();
            Log.w(TAG, "home: copying " + from + ": " + e.getMessage());
        }
    }

    static void copy(File from, File to) throws IOException {
        try (FileInputStream in = new FileInputStream(from); FileOutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            out.getFD().sync();
        }
        if (to.length() != from.length()) throw new IOException("short copy");
    }

    /**
     * Writes a small text file of the app's: a temporary file renamed over it, and if the old file is another app's
     * (it can't be replaced), deleted first and written again. Throws when it can't be written at all.
     */
    static void write(File f, String text) throws IOException {
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        File tmp = new File(f.getParentFile(), f.getName() + ".tmp");
        try {
            try (FileOutputStream out = new FileOutputStream(tmp)) {
                out.write(bytes);
                out.getFD().sync();
            }
            if (tmp.renameTo(f)) return;
            if (f.delete() && tmp.renameTo(f)) return;
            throw new IOException("can't replace " + f.getName());
        } catch (IOException e) {
            tmp.delete();
            // (the folder can't take a new file: the old one written in place, if it can be)
            try (FileOutputStream out = new FileOutputStream(f)) {
                out.write(bytes);
            } catch (IOException e2) {
                throw e;
            }
        }
    }

    /** write(), with no exception: false when it couldn't. */
    static boolean writeText(File f, String text) {
        try {
            write(f, text);
            return true;
        } catch (IOException e) {
            Log.e(TAG, "writing " + f + ": " + e);
            return false;
        }
    }

    /** A small text file, trimmed: "" when it can't be read. */
    static String read(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] b = new byte[8192];
            int n = 0;
            for (int k; n < b.length && (k = in.read(b, n, b.length - n)) > 0; ) n += k;
            return new String(b, 0, n, StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return "";
        }
    }
}
