package com.kriszts.allgenu;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.app.AlertDialog;
import android.content.DialogInterface;

import org.libsdl.app.SDLActivity;

/**
 * Activity del port. Hereda de SDLActivity (el glue de SDL3) y anade:
 *
 *   - Un TEXTO DE ESTADO en la esquina, encima del juego. En el movil no hay
 *     consola ni PC, asi que lo que pasa se ve aqui: si algo falla, el error
 *     se queda escrito en pantalla en vez de dejar una pantalla negra muda.
 *   - showMessage(): dialogos de error que llama el runtime por JNI
 *     (plat_error_box en runtime/platform/android.c)
 *
 * SDLActivity  ->  getLibraries()  ->  "SDL3", "main"  ->  dlsym("SDL_main")
 * libmain.so exporta SDL_main (runtime/platform/android_jni.c), que llama a
 * allgenu_main (runtime/main.c compilado con -Dmain=allgenu_main).
 */
public class AllGenUActivity extends SDLActivity {

    private static final String TAG = "AllGenU";

    private TextView mStatus;
    private volatile boolean mNativeStarted = false;

    @Override
    protected String[] getLibraries() {
        // Solo libmain.so: todo el runtime y el codigo del juego recompilado
        // van enlazados dentro. SDL3 se empaqueta aparte (libSDL3.so).
        return new String[] {
                "SDL3",
                "main"
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // ---- texto de estado (encima del juego) ----
        try {
            RelativeLayout root = (RelativeLayout) findViewById(android.R.id.content);
            if (root == null) {
                View content = findViewById(android.R.id.content);
                root = (RelativeLayout) ((ViewGroup) content).getChildAt(0);
            }
            mStatus = new TextView(this);
            mStatus.setTextSize(13f);
            mStatus.setTextColor(0xFFFFE066);          // amarillo
            mStatus.setBackgroundColor(0x99000000);    // fondo negro semi
            mStatus.setPadding(20, 8, 20, 8);
            mStatus.setText("AllGenU: iniciando...");
            RelativeLayout.LayoutParams lp = new RelativeLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
            lp.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
            mStatus.setLayoutParams(lp);
            root.addView(mStatus);
            Log.i(TAG, "texto de estado anadido");
        } catch (Throwable t) {
            Log.e(TAG, "no pude crear el texto de estado: " + t);
            mStatus = null;
        }

        appendStatus("Java: actividad creada, cargando libmain.so");

        // Permiso para leer Download/ (Android <= 12). Sin esto, el dump que el
        // jugador deja en Descargas no se puede abrir.
        askStoragePermission();

        // Si a los 8 s la parte nativa no ha dicho nada, se avisa en pantalla:
        // eso significa que el fallo es de carga/arranque, no del juego.
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!mNativeStarted) {
                    appendStatus("La parte nativa NO arranco. Mira el final del log "
                            + "(files/diagnostico.txt).");
                }
            }
        }, 8000);
    }

    private void askStoragePermission() {
        try {
            if (Build.VERSION.SDK_INT > 32) return;   // 13+: no aplica (ver README)
            if (Build.VERSION.SDK_INT < 23) return;
            if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED) {
                appendStatus("Permiso de archivos: ya concedido");
                return;
            }
            requestPermissions(new String[] {
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE }, 1001);
            appendStatus("Pidiendo permiso para leer tus archivos (Aceptar)");
        } catch (Throwable t) {
            Log.e(TAG, "permiso: " + t);
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        super.onRequestPermissionsResult(code, perms, res);
        if (code != 1001) return;
        boolean ok = res != null && res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED;
        appendStatus(ok ? "Permiso de archivos: CONCEDIDO"
                        : "Permiso de archivos: DENEGADO (el dump debe ir a Android/data/...)");
    }

    private void appendStatus(final String msg) {
        Log.i(TAG, "[estado] " + msg);
        if (mStatus == null) return;
        // se esconde solo a los 20 s (si vuelve a pasar algo, reaparece)
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                if (mStatus != null) mStatus.setVisibility(View.GONE);
            }
        }, 20000);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                CharSequence prev = mStatus.getText();
                String txt = (prev == null ? "" : prev.toString());
                // se guardan solo las 8 ultimas lineas para no tapar el juego
                String[] lines = txt.split("\n");
                StringBuilder sb = new StringBuilder();
                int from = Math.max(0, lines.length - 7);
                for (int i = from; i < lines.length; i++) {
                    if (lines[i].length() == 0) continue;
                    if (sb.length() > 0) sb.append('\n');
                    sb.append(lines[i]);
                }
                if (sb.length() > 0) sb.append('\n');
                sb.append(msg);
                mStatus.setText(sb.toString());
                mStatus.setVisibility(View.VISIBLE);
            }
        });
    }

    /** Llamado desde C (SDL_main) nada mas entrar: la parte nativa existe. */
    public void onNativeStart() {
        mNativeStarted = true;
        appendStatus("C: parte nativa arrancando (libmain.so cargada)");
    }

    /** Llamado desde C: linea de estado / error, se queda en pantalla. */
    public void showStatus(final String msg) {
        appendStatus(msg);
    }

    /** Llamado desde C (plat_error_box) cuando algo falla. */
    public void showMessage(final String msg) {
        appendStatus("ERROR: " + msg);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    new AlertDialog.Builder(AllGenUActivity.this)
                            .setTitle("AllGenURecomp")
                            .setMessage(msg)
                            .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                                public void onClick(DialogInterface d, int w) { d.dismiss(); }
                            })
                            .show();
                } catch (Throwable t) {
                    Log.e(TAG, "no pude mostrar el dialogo: " + t);
                }
            }
        });
    }
}
