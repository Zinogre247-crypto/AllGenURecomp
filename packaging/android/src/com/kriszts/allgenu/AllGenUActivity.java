package com.kriszts.allgenu;

import android.os.Bundle;
import android.util.Log;
import android.app.AlertDialog;
import android.content.DialogInterface;

import org.libsdl.app.SDLActivity;

/**
 * Activity del port. Hereda de SDLActivity (el glue de SDL3) y solo anade:
 *   - showMessage(): dialogos de error que llama el runtime por JNI
 *     (plat_error_box en runtime/platform/android.c)
 *   - el aviso de donde poner los archivos del juego
 *
 * SDLActivity.c  ->  getLibraries()  ->  "main"  ->  libmain.so
 * libmain.so exporta SDL_main (runtime/platform/android_jni.c), que llama a
 * allgenu_main (runtime/main.c compilado con -Dmain=allgenu_main).
 */
public class AllGenUActivity extends SDLActivity {

    private static final String TAG = "AllGenU";

    @Override
    protected String[] getLibraries() {
        // Solo libmain.so: todo el runtime y el codigo del juego recompilado
        // van enlazados dentro (SDL3, Vulkan y shaderc son .so del sistema o
        // bibliotecas que tambien se empaquetan; si se empaquetan aparte, se
        // anaden aqui en orden de dependencia).
        return new String[] {
                "SDL3",
                "main"
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "AllGenUActivity creada");
    }

    /** Llamado desde C (android_show_message) cuando plat_error_box() salta. */
    public void showMessage(final String msg) {
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
