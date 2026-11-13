package com.mhgu.recomp;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Two notices before the game starts, the same in MHGU and MH4U for Android (only REPO, OFFICIAL and the settings
 * file differ):
 *  - Unofficial build: the APK's signing certificate is not khorne-hub's (its SHA-256 is in the README's Official
 *    downloads). Once per version; Continue goes on to the game: it never stops it.
 *  - Update: at most once a day, in the background, one plain GET of GitHub's latest release of this repository (no
 *    other data is sent); a newer tag than this version is shown at the next start, at most once a day: View opens
 *    the release page, Later goes on. Settings -> Check for updates (mhgurecomp.ini [Application] Updates = Off)
 *    turns it off. Offline, or any error: nothing is said.
 */
final class Notices {
    private static final String TAG = "MHGU";
    static final String REPO = "gu-native";
    // the release key's certificate (android/mhgu.keystore), as the README's Official downloads lists it
    private static final String OFFICIAL = "D72F6FF155E0312C9C7486C530646038E42494C0AD7CD65F4B19FF5D0F32B297";
    private static final long DAY_MS = 24L * 60 * 60 * 1000;
    private static final Pattern TAG_NAME = Pattern.compile("\"tag_name\"\\s*:\\s*\"([^\"]+)\"");

    private Notices() {}

    private static SharedPreferences prefs(Context c) { return c.getSharedPreferences("notices", Context.MODE_PRIVATE); }

    static String version(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    /** The APK is signed with khorne-hub's key (any of its signers, as Android lists them). */
    @SuppressWarnings("deprecation")
    static boolean official(Context c) {
        try {
            PackageInfo info = c.getPackageManager().getPackageInfo(c.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            Signature[] signers = info.signingInfo == null ? null
                    : info.signingInfo.hasMultipleSigners() ? info.signingInfo.getApkContentsSigners()
                    : info.signingInfo.getSigningCertificateHistory();
            if (signers == null) return true;  // (nothing to compare: say nothing)
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            for (Signature s : signers) {
                StringBuilder hex = new StringBuilder();
                for (byte b : sha.digest(s.toByteArray())) hex.append(String.format(Locale.ROOT, "%02X", b));
                if (hex.toString().equals(OFFICIAL)) return true;
            }
            return false;
        } catch (Exception e) {
            Log.w(TAG, "notices: the signature: " + e);
            return true;
        }
    }

    /** Settings -> Check for updates: [Application] Updates in mhgurecomp.ini (Off / never: off; anything else on). */
    static boolean updatesOn(Context c) {
        File ini = new File(AppHome.home(c), "mhgurecomp.ini");
        String section = "";
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            for (String line; (line = r.readLine()) != null; ) {
                String t = line.trim();
                if (t.startsWith("[") && t.endsWith("]")) { section = t.substring(1, t.length() - 1).trim(); continue; }
                int eq = t.indexOf('=');
                if (eq <= 0 || !section.equalsIgnoreCase("Application")) continue;
                if (!t.substring(0, eq).trim().equalsIgnoreCase("Updates")) continue;
                String v = t.substring(eq + 1).trim().toLowerCase(Locale.ROOT);
                return !(v.equals("off") || v.equals("never"));
            }
        } catch (IOException ignored) {
        }
        return true;
    }

    /** "v0.1.9" / "0.1.9" against "0.1.8": the first is newer. Anything that isn't numbers and dots: not newer. */
    static boolean newer(String tag, String mine) {
        int[] a = parts(tag), b = parts(mine);
        if (a == null || b == null) return false;
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? a[i] : 0, y = i < b.length ? b[i] : 0;
            if (x != y) return x > y;
        }
        return false;
    }

    private static int[] parts(String v) {
        if (v == null) return null;
        v = v.trim();
        if (v.startsWith("v") || v.startsWith("V")) v = v.substring(1);
        int cut = 0;
        while (cut < v.length() && (Character.isDigit(v.charAt(cut)) || v.charAt(cut) == '.')) cut++;
        v = v.substring(0, cut);
        if (v.isEmpty()) return null;
        String[] s = v.split("\\.");
        int[] out = new int[s.length];
        try {
            for (int i = 0; i < s.length; i++) out[i] = Integer.parseInt(s[i]);
        } catch (NumberFormatException e) {
            return null;
        }
        return out;
    }

    /** At most once a day, on a thread of its own: asks GitHub for the latest release's tag, and keeps it. */
    static void checkInBackground(Context context) {
        Context c = context.getApplicationContext();
        if (!updatesOn(c)) return;
        SharedPreferences p = prefs(c);
        long now = System.currentTimeMillis(), last = p.getLong("checked_at", 0);
        if (now - last < DAY_MS && now >= last) return;
        p.edit().putLong("checked_at", now).apply();
        new Thread(() -> {
            HttpURLConnection http = null;
            try {
                http = (HttpURLConnection) new URL("https://api.github.com/repos/khorne-hub/" + REPO + "/releases/latest").openConnection();
                http.setConnectTimeout(8000);
                http.setReadTimeout(8000);
                http.setRequestProperty("Accept", "application/vnd.github+json");
                // (GitHub wants a User-Agent, and Android's own names the phone's model, Android version and build: the
                // repository's name instead, the same for every phone, so the request says nothing about the device)
                http.setRequestProperty("User-Agent", REPO);
                if (http.getResponseCode() != 200) return;
                StringBuilder body = new StringBuilder();
                try (InputStream in = http.getInputStream()) {
                    byte[] buf = new byte[8192];
                    for (int n; (n = in.read(buf)) > 0 && body.length() < (256 << 10); )
                        body.append(new String(buf, 0, n, StandardCharsets.UTF_8));
                }
                Matcher m = TAG_NAME.matcher(body);
                if (m.find()) {
                    p.edit().putString("latest", m.group(1)).apply();
                    Log.i(TAG, "updates: the latest release is " + m.group(1));
                }
            } catch (Exception e) {  // (offline, a time-out, anything: nothing to say)
                Log.i(TAG, "updates: " + e.getClass().getSimpleName());
            } finally {
                if (http != null) http.disconnect();
            }
        }, "update-check").start();
    }

    /**
     * Before the game starts: the notices that are due, one after the other, then `then` (on this thread). With none,
     * `then` runs at once.
     */
    static void beforeGame(Activity a, Runnable then) {
        boolean es = Look.es();
        String mine = version(a);
        SharedPreferences p = prefs(a);
        if (!official(a) && !mine.equals(p.getString("unofficial_seen", null))) {
            p.edit().putString("unofficial_seen", mine).apply();
            Look.notice(a, es ? "ESTA NO ES LA VERSIÓN OFICIAL" : "THIS IS NOT THE OFFICIAL VERSION",
                    es ? "Esta copia no está firmada por khorne-hub, así que no es la versión oficial: alguien la cambió o la "
                         + "volvió a firmar. Las versiones oficiales están solo en github.com/khorne-hub/" + REPO + "."
                       : "This copy isn't signed by khorne-hub, so it isn't the official version: someone changed it or signed "
                         + "it again. The official versions are only at github.com/khorne-hub/" + REPO + ".",
                    new Look.Choice(es ? "Continuar" : "Continue", true, () -> updateThen(a, then)));
            return;
        }
        updateThen(a, then);
    }

    private static void updateThen(Activity a, Runnable then) {
        boolean es = Look.es();
        SharedPreferences p = prefs(a);
        String latest = p.getString("latest", null), mine = version(a);
        long now = System.currentTimeMillis(), shown = p.getLong("shown_at", 0);
        if (latest == null || !updatesOn(a) || !newer(latest, mine) || (now - shown < DAY_MS && now >= shown)) {
            then.run();
            return;
        }
        p.edit().putLong("shown_at", now).apply();
        String v = latest.startsWith("v") || latest.startsWith("V") ? latest.substring(1) : latest;
        // (a copy signed by someone else: Android won't install the official one over it, and uninstalling it deletes the
        // save in Android/data, so the save is exported first)
        boolean over = official(a);
        Look.notice(a, es ? "HAY UNA VERSIÓN NUEVA" : "A NEW VERSION IS OUT",
                es ? "La versión " + v + " ya está disponible (tienes la " + mine + "). "
                     + (over ? "Se instala encima de esta: tu partida y tus ajustes se quedan como están."
                             : "Esta copia no es la oficial, así que la oficial no se instala encima. Primero exporta tu partida "
                               + "(Opciones, Exportar partida); después desinstala esta copia, instala la oficial e importa tu partida.")
                   : "Version " + v + " is out (you have " + mine + "). "
                     + (over ? "It installs over this one: your save and your settings stay as they are."
                             : "This copy isn't the official one, so the official one won't install over it. First export your "
                               + "save (Settings, Export save); then uninstall this copy, install the official one and import your save."),
                new Look.Choice(es ? "Ver" : "View", true, () -> {
                    try {
                        a.startActivity(new Intent(Intent.ACTION_VIEW,
                                Uri.parse("https://github.com/khorne-hub/" + REPO + "/releases/latest")));
                        a.finish();
                    } catch (ActivityNotFoundException e) {
                        then.run();
                    }
                }),
                new Look.Choice(es ? "Más tarde" : "Later", false, then));
    }
}
