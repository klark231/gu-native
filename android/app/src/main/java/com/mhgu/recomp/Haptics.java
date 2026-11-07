package com.mhgu.recomp;

import android.content.Context;
import android.media.AudioAttributes;
import android.os.Build;
import android.os.VibrationAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;

import org.libsdl.app.SDLActivity;

/**
 * La vibración del teléfono (runtime/haptics.c, por JNI desde el hilo principal de SDL): un clic al tocar los botones
 * táctiles y la vibración del juego (rugidos, golpes) como pulsos que el código nativo renueva mientras dura.
 *
 * Uso "multimedia" (VibrationAttributes.USAGE_MEDIA en API 33+, que la documentación da para juegos; antes
 * AudioAttributes.USAGE_GAME), no "táctil": apagar la vibración al tocar la pantalla (Samsung: "Interacciones
 * táctiles") no la apaga, y la intensidad que manda es la de multimedia. El ahorro de batería de Android sí la apaga.
 *
 * Todo es barato y nada lanza excepciones: si algo falla, no vibra.
 */
public final class Haptics {
    private static Vibrator vibrator;           // null: no hay (o todavía no se buscó)
    private static boolean looked, amplitude;   // amplitude: el motor acepta una intensidad (1..255)
    private static VibrationAttributes vattrs;  // API 33+
    private static AudioAttributes aattrs;      // API 29..32
    private static VibrationEffect click;

    private Haptics() {}

    /** 0 sin vibrador, 1 solo encendido / apagado, 2 con control de intensidad (la primera llamada lo busca) */
    public static int caps() {
        try {
            return vibrator() == null ? 0 : amplitude ? 2 : 1;
        } catch (Throwable t) {
            return 0;
        }
    }

    /** el clic de una tecla: corto y seco */
    public static void tap() {
        try {
            Vibrator v = vibrator();
            if (v != null && click != null) play(v, click);
        } catch (Throwable t) {
            // (sin vibración)
        }
    }

    /** un pulso de ms milisegundos a esa intensidad (1..255; sin control de intensidad, la del teléfono) */
    public static void rumble(int amp, int ms) {
        try {
            Vibrator v = vibrator();
            if (v == null || ms <= 0) return;
            int a = amplitude ? Math.max(1, Math.min(255, amp)) : VibrationEffect.DEFAULT_AMPLITUDE;
            play(v, VibrationEffect.createOneShot(ms, a));
        } catch (Throwable t) {
            // (sin vibración)
        }
    }

    /** corta lo que esté vibrando */
    public static void stop() {
        try {
            Vibrator v = vibrator();
            if (v != null) v.cancel();
        } catch (Throwable t) {
            // (nada que cortar)
        }
    }

    @SuppressWarnings("deprecation")  // (vibrate con AudioAttributes: lo único que hay antes de API 33)
    private static void play(Vibrator v, VibrationEffect e) {
        if (vattrs != null && Build.VERSION.SDK_INT >= 33) v.vibrate(e, vattrs);
        else v.vibrate(e, aattrs);
    }

    @SuppressWarnings("deprecation")  // (VIBRATOR_SERVICE: lo único que hay antes de API 31)
    private static Vibrator vibrator() {
        if (looked) return vibrator;
        Context ctx = SDLActivity.getContext();
        if (ctx == null) return null;  // (todavía sin actividad: se busca en la próxima llamada)
        looked = true;
        try {
            Context app = ctx.getApplicationContext();  // (no retiene la actividad)
            if (app == null) app = ctx;
            Vibrator v;
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager vm = (VibratorManager) app.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
                v = vm != null ? vm.getDefaultVibrator() : null;
            } else {
                v = (Vibrator) app.getSystemService(Context.VIBRATOR_SERVICE);
            }
            if (v == null || !v.hasVibrator()) return null;
            amplitude = v.hasAmplitudeControl();
            if (Build.VERSION.SDK_INT >= 33) {
                vattrs = VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA);
            } else {
                aattrs = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build();
            }
            click = makeClick(v);
            vibrator = v;
        } catch (Throwable t) {
            vibrator = null;
        }
        return vibrator;
    }

    // El clic predefinido del teléfono (el de su teclado). Si dice que no lo tiene (el sistema lo cambiaría por un
    // patrón largo), un pulso propio: 12 ms a media intensidad, o 20 ms si el motor no acepta intensidad (los motores
    // de masa excéntrica tardan en arrancar y uno más corto no se siente).
    private static VibrationEffect makeClick(Vibrator v) {
        boolean predefined = Build.VERSION.SDK_INT >= 29 && (Build.VERSION.SDK_INT < 30
                || v.areAllEffectsSupported(VibrationEffect.EFFECT_CLICK) != Vibrator.VIBRATION_EFFECT_SUPPORT_NO);  // (Android 9: the own pulse)
        if (predefined) {
            try {
                return VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK);
            } catch (Throwable t) {
                // (el pulso propio)
            }
        }
        return amplitude ? VibrationEffect.createOneShot(12, 120)
                         : VibrationEffect.createOneShot(20, VibrationEffect.DEFAULT_AMPLITUDE);
    }
}
