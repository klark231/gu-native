package com.mhgu.recomp;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;

import org.libsdl.app.SDLActivity;

/**
 * Temperatura del teléfono para el contador de rendimiento ([Performance] ShowStats, runtime/perfstats.cpp), que el
 * código nativo lee por JNI desde el hilo principal de SDL cada 2 s como mucho. No hace falta ningún permiso: la
 * batería sale del intent fijo ACTION_BATTERY_CHANGED y el estado térmico de PowerManager.
 */
public final class DeviceStats {
    private DeviceStats() {}

    /** Temperatura de la batería en décimas de °C (385 = 38,5 °C); Integer.MIN_VALUE si no se sabe. */
    public static int batteryTempTenths() {
        try {
            Context ctx = SDLActivity.getContext();
            if (ctx == null) return Integer.MIN_VALUE;
            // (receptor null: devuelve el último intent fijo sin registrar nada)
            Intent battery = ctx.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            return battery != null ? battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE)
                                   : Integer.MIN_VALUE;
        } catch (Exception e) {
            return Integer.MIN_VALUE;
        }
    }

    /** PowerManager.THERMAL_STATUS_*: 0 ninguno, 1 leve, 2 moderado, 3 grave, 4 crítico, 5 emergencia, 6 apagado; -1 si no se sabe. */
    public static int thermalStatus() {
        try {
            PowerManager pm = powerManager();
            return pm != null && Build.VERSION.SDK_INT >= 29 ? pm.getCurrentThermalStatus() : -1;  // (API 29)
        } catch (Exception e) {
            return -1;
        }
    }

    /**
     * Margen térmico previsto a 10 s (Android 11+): 1.0 es donde el sistema empieza a bajar el rendimiento; NaN si no
     * se sabe (versión anterior, o el sistema todavía no tiene muestras).
     */
    public static float thermalHeadroom() {
        if (Build.VERSION.SDK_INT < 30) return Float.NaN;
        try {
            PowerManager pm = powerManager();
            return pm != null ? pm.getThermalHeadroom(10) : Float.NaN;
        } catch (Exception e) {
            return Float.NaN;
        }
    }

    private static PowerManager powerManager() {
        Context ctx = SDLActivity.getContext();
        return ctx != null ? (PowerManager) ctx.getSystemService(Context.POWER_SERVICE) : null;
    }
}
