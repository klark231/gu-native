package com.mhgu.recomp;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Settings -> Save folder: where the save is kept. By default the game's own folder (MHGURecomp/save/save_0 in the
 * app's folder); or a folder the player chooses, for example one a sync app shares with their other devices and the PC
 * version. The same files and rules as MH4U for Android's and the PC's (runtime/savefolder.c reads the same files):
 *
 *   save_folder.txt          the chosen folder (the one holding system and system_backup); none: the game's own
 *   save_folder_pending.txt  folder=<path or nothing: the game's own>, keep=folder|device: a change chosen while the
 *                            game ran, applied before it starts again (applyPending), as an imported save is
 *   save_folder_state.txt    the folder's save as last seen (size, time): an empty folder that had one is a problem
 *   save_folder_missed.txt   (runtime/savefolder.c) the folder couldn't be reached at a save, so it went here
 *   device.txt               id=, name=: this device, for the in-use marker
 *   <folder>/.mhgu-in-use    id=, name=, time=: a device has the game open with that save (savefolder.c keeps it
 *                            fresh every few minutes and removes it at a clean exit)
 *
 * Nothing is overwritten without asking: when both places have a save the player chooses, and the save not chosen
 * goes to the app's own MHGURecomp/save_backups/<date>, never into the shared folder. Files in the chosen folder are
 * never deleted (but the conflict copies a sync app left, after they are backed up). Backups and bonus/ stay local.
 */
final class SaveFolder {
    private static final String TAG = "MHGU";
    static final String SUB = "MHGU save", MARKER = ".mhgu-in-use";
    // (the save's size is fixed; the same mark at +0x14 in saves from a console, the emulators and this port)
    static final int SIZE = 5159100, MARK_AT = 0x14;
    private static final byte[] MARK = {0x74, (byte) 0xee, (byte) 0xb2, 0x36};
    static final String[] FILES = {"system", "system_backup"};
    static final long FRESH_S = 15 * 60;  // (the marker is renewed every 5 minutes: older than this, nobody has it open)

    private SaveFolder() {}

    static File home(Context c) { return AppHome.home(c); }
    static File local(Context c) { return new File(home(c), "MHGURecomp/save/save_0"); }
    static File backups(Context c) { return new File(home(c), "MHGURecomp/save_backups"); }
    private static File file(Context c, String name) { return new File(home(c), name); }

    /** The chosen folder, or null: the game's own. */
    static File chosen(Context c) {
        String p = AppHome.read(file(c, "save_folder.txt"));
        return p.isEmpty() ? null : new File(p);
    }

    /** Where the save is now: the chosen folder, else the game's own. */
    static File active(Context c) {
        File f = chosen(c);
        return f != null ? f : local(c);
    }

    // ---- a save and what is in it
    static final class Info {
        boolean valid;
        long time;
        String names = "";
    }

    static boolean isSave(File f) {
        if (f.length() != SIZE) return false;
        byte[] head = new byte[MARK_AT + MARK.length];
        try (FileInputStream in = new FileInputStream(f)) {
            if (in.read(head) != head.length) return false;
        } catch (IOException e) {
            return false;
        }
        return Arrays.equals(Arrays.copyOfRange(head, MARK_AT, head.length), MARK);
    }

    /** The save in dir (its file system): valid, when it was saved, and the hunters' names. */
    static Info describe(File dir) { return describeFile(new File(dir, "system")); }

    /** The same for one save file (a sync app's other version of system has another name). */
    static Info describeFile(File f) {
        Info i = new Info();
        if (!isSave(f)) return i;
        i.valid = true;
        i.time = f.lastModified();
        // (the three character slots: u32 at +0x34/+0x38/+0x3c, from 0x24; a hunter's name is the slot's first 32 bytes)
        StringBuilder names = new StringBuilder();
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            for (int at = 0x34; at <= 0x3c; at += 4) {
                r.seek(at);
                byte[] b4 = new byte[4];
                r.readFully(b4);
                long slot = ((b4[0] & 0xffL) | (b4[1] & 0xffL) << 8 | (b4[2] & 0xffL) << 16 | (b4[3] & 0xffL) << 24) + 0x24;
                if (slot + 32 > SIZE) continue;
                byte[] name = new byte[32];
                r.seek(slot);
                r.readFully(name);
                int n = 0;
                while (n < name.length && name[n] != 0) n++;
                String s = new String(name, 0, n, StandardCharsets.UTF_8).trim();
                if (!s.isEmpty()) names.append(names.length() > 0 ? ", " : "").append(s);
            }
        } catch (IOException ignored) {
        }
        i.names = names.toString();
        return i;
    }

    static String when(long time) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(new Date(time));
    }

    /** "KHORNE, Anbernic · saved 2026-11-09 18:30", or "no save". */
    static String line(Info i, boolean es) {
        if (!i.valid) return es ? "sin partida" : "no save";
        String who = i.names.isEmpty() ? (es ? "una partida" : "a save") : i.names;
        return who + (es ? " · guardada el " : " · saved ") + when(i.time);
    }

    // ---- choosing a folder
    /**
     * The folder the save goes in, for a folder the player picked (lenient, as the game folder's "one level off"):
     * one with a save in it (an Export folder, another device's) as it is; else its "MHGU save" folder, made if it
     * isn't there, so one shared folder can hold MHGU's and MH4U's saves and nothing loose lands in Download.
     */
    static File resolve(File picked) {
        if (isSave(new File(picked, "system"))) return picked;
        File sub = new File(picked, SUB);
        if (sub.isDirectory()) return sub;
        if (picked.getName().equals(SUB)) return picked;
        return sub;
    }

    /** The browser's ✓: a folder that holds a save, itself or in its MHGU save folder. */
    static boolean holdsSave(File dir) {
        return isSave(new File(dir, "system")) || isSave(new File(dir, SUB + "/system"));
    }

    /** Why the picked folder can't be the save folder, or null when it can. */
    static String refusal(Context c, File picked, boolean es) {
        String p = picked.getAbsolutePath();
        if (p.contains("/Android/data/") || p.endsWith("/Android/data") || p.contains("/Android/obb"))
            return es ? "Esa carpeta es de una app y Android la mantiene cerrada. Elige otra."
                      : "That folder belongs to an app, and Android keeps it closed. Choose another one.";
        String own = home(c).getAbsolutePath();
        if (p.equals(own) || p.startsWith(own + "/"))
            return es ? "Esa es la carpeta propia del juego. Elige otra, o usa \"Usar la carpeta propia del juego\"."
                      : "That is the game's own folder. Choose another one, or use \"Use the game's own folder\".";
        if (!picked.isDirectory() || AppHome.probe(picked) != 0)
            return es ? "El juego no puede escribir en esa carpeta. Elige una en la memoria del teléfono."
                      : "The game can't write in that folder. Choose one in the phone's storage.";
        return null;
    }

    /** The all-files access the save folder needs (Android 9 and 10: reading and writing the storage). */
    static boolean hasAccess(Context c) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return c.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
            && c.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    /** A change, for the next start (null folder: back to the game's own). keepFolder: the folder's save stays. */
    static boolean setPending(Context c, File folder, boolean keepFolder) {
        return AppHome.writeText(file(c, "save_folder_pending.txt"),
                "folder=" + (folder == null ? "" : folder.getAbsolutePath()) + "\nkeep=" + (keepFolder ? "folder" : "device") + "\n");
    }

    static boolean hasPending(Context c) { return file(c, "save_folder_pending.txt").isFile(); }

    private static Map<String, String> keys(File f) {
        Map<String, String> m = new LinkedHashMap<>();
        for (String l : AppHome.read(f).split("\n")) {
            int eq = l.indexOf('=');
            if (eq > 0) m.put(l.substring(0, eq).trim(), l.substring(eq + 1).trim());
        }
        return m;
    }

    /**
     * Before the game starts (GameFolderActivity, and GameActivity for a start without it): the change chosen last time.
     * The saves are copied first (checked), save_folder.txt is written last. null: done, or nothing to do; else why it
     * couldn't be (nothing was changed; the change is dropped, so the game starts as before).
     */
    static synchronized String applyPending(Context c) {
        File pend = file(c, "save_folder_pending.txt");
        if (!pend.isFile()) return null;
        boolean es = Look.es();
        Map<String, String> k = keys(pend);
        String path = k.containsKey("folder") ? k.get("folder") : "";
        boolean keepFolder = "folder".equals(k.get("keep"));
        File now = chosen(c), from = active(c);
        String error = null;
        try {
            if (!path.isEmpty()) {
                File to = new File(path);
                if (!to.isDirectory() && !to.mkdirs()) throw new IOException(es ? "no se pudo crear " + to : "couldn't make " + to);
                if (AppHome.probe(to) != 0) throw new IOException(es ? "no se puede escribir en " + to : "can't write in " + to);
                Info there = describe(to), here = describe(from);
                if (!to.equals(from)) {
                    if (!keepFolder || !there.valid) {
                        if (there.valid) backup(c, to, "folder");
                        if (here.valid) copySave(from, to);
                    } else if (here.valid) {
                        backup(c, from, "device");
                    }
                }
                AppHome.write(file(c, "save_folder.txt"), to.getAbsolutePath());
                if (now != null && !now.equals(to)) unmark(c, now);
                Log.i(TAG, "save folder: " + to + (keepFolder ? " (its save kept)" : " (this device's save copied there)"));
            } else if (now != null) {
                File own = local(c);
                Info there = describe(now), here = describe(own);
                if (keepFolder && there.valid) {
                    if (!own.isDirectory() && !own.mkdirs()) throw new IOException(es ? "no se pudo crear " + own : "couldn't make " + own);
                    if (here.valid) backup(c, own, "device");
                    copySave(now, own);
                }
                if (!file(c, "save_folder.txt").delete() && file(c, "save_folder.txt").exists())
                    throw new IOException("save_folder.txt");
                unmark(c, now);
                Log.i(TAG, "save folder: back to the game's own" + (keepFolder ? " (the folder's save copied)" : ""));
            }
            file(c, "save_folder_state.txt").delete();
            file(c, "save_folder_missed.txt").delete();
        } catch (IOException e) {
            Log.e(TAG, "save folder: " + e.getMessage());
            error = e.getMessage();
        }
        pend.delete();
        return error;
    }

    /** The save's files from one folder into another: each copied beside its place, checked, and renamed over it. */
    static void copySave(File from, File to) throws IOException {
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("couldn't make " + to);
        File system = new File(from, "system");
        for (String name : FILES) {
            File src = new File(from, name);
            if (!src.isFile()) src = system;  // (no system_backup: the game keeps one, so system takes its place)
            copyChecked(src, new File(to, name));
        }
    }

    static void copyChecked(File src, File dst) throws IOException {
        File parent = dst.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("couldn't make " + parent);
        File tmp = new File(dst.getParentFile(), dst.getName() + ".mhgu-copy");
        tmp.delete();
        AppHome.copy(src, tmp);
        if (src.length() == SIZE && !isSave(tmp)) { tmp.delete(); throw new IOException("the copy of " + src + " is not right"); }
        if (!tmp.renameTo(dst) && !(dst.delete() && tmp.renameTo(dst))) {
            tmp.delete();
            throw new IOException("couldn't replace " + dst);
        }
        dst.setLastModified(src.lastModified());
    }

    /** A copy of the save in dir in the app's own save_backups/<date>-<what>. */
    static File backup(Context c, File dir, String what) throws IOException {
        String stamp = new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(new Date());
        File keep = new File(backups(c), stamp + "-" + what);
        for (int n = 2; keep.exists(); n++) keep = new File(backups(c), stamp + "-" + what + "-" + n);
        if (!keep.mkdirs()) throw new IOException("couldn't make " + keep);
        for (String name : FILES) {
            File f = new File(dir, name);
            if (f.isFile()) AppHome.copy(f, new File(keep, name));
        }
        Log.i(TAG, "save folder: the save of " + dir + " kept in " + keep);
        return keep;
    }

    // ---- the start gate
    static final int NOT_FOUND = 1, NO_PERMISSION = 2, EMPTY = 3, CONFLICT = 4, MISSED = 5, IN_USE = 6;

    static final class Problem {
        int kind;
        File folder;
        List<File> copies = new ArrayList<>();  // CONFLICT: system and the sync app's other versions
        String device;                          // IN_USE: which one has it open, and since when
        long since;
    }

    /** Is something wrong with the save folder before the game starts? null: nothing (or no folder chosen). */
    static Problem check(Context c, boolean ignoreInUse) {
        File f = chosen(c);
        if (f == null) return null;
        Problem p = new Problem();
        p.folder = f;
        if (!f.isDirectory() || !f.canWrite()) {
            p.kind = hasAccess(c) ? NOT_FOUND : NO_PERMISSION;
            return p;
        }
        if (file(c, "save_folder_missed.txt").isFile()) {
            p.kind = MISSED;
            return p;
        }
        p.copies = conflicts(f);
        if (p.copies.size() > 1 || (p.copies.size() == 1 && !p.copies.get(0).getName().equals("system"))) {
            p.kind = CONFLICT;
            return p;
        }
        Info i = describe(f);
        if (!i.valid) {
            if (!AppHome.read(file(c, "save_folder_state.txt")).isEmpty()) {
                p.kind = EMPTY;
                return p;
            }
            return null;  // (a new folder that never had a save: the game makes the first one there)
        }
        if (!ignoreInUse) {
            Map<String, String> m = keys(new File(f, MARKER));
            long time = parse(m.get("time"));
            long now = System.currentTimeMillis() / 1000;
            if (time > 0 && now - time < FRESH_S && now >= time - 60 && m.containsKey("id") && !m.get("id").equals(deviceId(c))) {
                p.kind = IN_USE;
                p.device = m.containsKey("name") ? m.get("name") : "?";
                p.since = time * 1000;
                return p;
            }
        }
        return null;
    }

    private static long parse(String s) {
        try {
            return s == null ? 0 : Long.parseLong(s.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** system and the other versions of it a sync app left beside it ("system.sync-conflict-...", "system (1)"...). */
    static List<File> conflicts(File dir) {
        List<File> out = new ArrayList<>();
        File main = new File(dir, "system");
        if (isSave(main)) out.add(main);
        File[] kids = dir.listFiles(File::isFile);
        if (kids == null) return out;
        Arrays.sort(kids, (a, b) -> a.getName().compareTo(b.getName()));
        for (File k : kids) {
            String n = k.getName();
            if (!n.startsWith("system") || n.equals("system") || n.startsWith("system_backup")) continue;
            if (n.endsWith(".mhgu-tmp") || n.endsWith(".mhgu-commit") || n.endsWith(".mhgu-copy")) continue;
            if (isSave(k)) out.add(k);
        }
        return out;
    }

    /** The version chosen becomes system (and system_backup); the others are backed up here, then removed there. */
    static void keepVersion(Context c, File dir, File chosen) throws IOException {
        String stamp = new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(new Date());
        File keep = new File(backups(c), stamp + "-conflict");
        if (!keep.isDirectory() && !keep.mkdirs()) throw new IOException("couldn't make " + keep);
        File[] kids = dir.listFiles(File::isFile);
        List<File> others = new ArrayList<>();
        if (kids != null)
            for (File k : kids) {
                String n = k.getName();
                if (!n.startsWith("system") || k.equals(chosen)) continue;
                if (n.endsWith(".mhgu-tmp") || n.endsWith(".mhgu-commit") || n.endsWith(".mhgu-copy")) continue;
                AppHome.copy(k, new File(keep, n));
                if (!n.equals("system") && !n.equals("system_backup")) others.add(k);
            }
        if (!chosen.getName().equals("system")) {
            File staged = new File(keep, "chosen-" + chosen.getName());
            AppHome.copy(chosen, staged);
            for (String name : FILES) {
                copyChecked(staged, new File(dir, name));
                new File(dir, name).setLastModified(chosen.lastModified());
            }
            others.add(chosen);
        }
        for (File o : others) if (!o.delete()) Log.w(TAG, "save folder: couldn't remove " + o);
        Log.i(TAG, "save folder: kept " + chosen.getName() + ", the other versions in " + keep);
    }

    /** This device's in-use mark in a folder it no longer uses (another device's stays). */
    static void unmark(Context c, File folder) {
        File m = new File(folder, MARKER);
        if (deviceId(c).equals(keys(m).get("id"))) m.delete();
    }

    /** After the gate: what was seen of the folder's save, and this device's mark that the save is open here. */
    static void started(Context c) {
        File f = chosen(c);
        if (f == null) return;
        Info i = describe(f);
        if (i.valid) AppHome.writeText(file(c, "save_folder_state.txt"), "size=" + SIZE + "\ntime=" + i.time + "\n");
        AppHome.writeText(new File(f, MARKER), "id=" + deviceId(c) + "\nname=" + deviceName(c) + "\ntime="
                + System.currentTimeMillis() / 1000 + "\n");
    }

    /** MISSED: the save went to this device last time; keepDevice: it goes to the folder (the folder's backed up). */
    static void settleMissed(Context c, boolean keepDevice) throws IOException {
        File f = chosen(c), own = local(c);
        if (f != null) {
            if (keepDevice) {
                if (describe(f).valid) backup(c, f, "folder");
                copySave(own, f);
            } else if (describe(own).valid) {
                backup(c, own, "device");
            }
        }
        file(c, "save_folder_missed.txt").delete();
    }

    /** When the save went to this device instead (runtime/savefolder.c writes time=, in seconds). */
    static long missedAt(Context c) {
        long t = parse(keys(file(c, "save_folder_missed.txt")).get("time"));
        return t > 0 ? t * 1000 : describe(local(c)).time;
    }

    /** Stops using the folder (the game's own folder again; nothing copied, the folder left as it is). */
    static void forget(Context c) {
        file(c, "save_folder.txt").delete();
        file(c, "save_folder_state.txt").delete();
        file(c, "save_folder_missed.txt").delete();
    }

    // ---- this device, for the marker (device.txt, shared with runtime/savefolder.c)
    private static Map<String, String> device(Context c) {
        File f = file(c, "device.txt");
        Map<String, String> m = keys(f);
        if (!m.containsKey("id") || m.get("id").isEmpty()) {
            byte[] b = new byte[8];
            new SecureRandom().nextBytes(b);
            StringBuilder hex = new StringBuilder();
            for (byte x : b) hex.append(String.format(Locale.ROOT, "%02x", x));
            m.put("id", hex.toString());
            m.put("name", Build.MODEL == null || Build.MODEL.isEmpty() ? "Android" : Build.MODEL);
            AppHome.writeText(f, "id=" + m.get("id") + "\nname=" + m.get("name") + "\n");
        }
        return m;
    }
    static String deviceId(Context c) { return device(c).get("id"); }
    static String deviceName(Context c) {
        String n = device(c).get("name");
        return n == null || n.isEmpty() ? Build.MODEL : n;
    }
}
