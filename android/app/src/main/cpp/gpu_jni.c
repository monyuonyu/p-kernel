/*
 *  android/app/src/main/cpp/gpu_jni.c
 *
 *  JNI bridge for the GPU-1/GPU-2 Vulkan compute backend's UI surface.
 *
 *  This file exposes ONLY the read/flag side of gpu_compute.h to Kotlin/Java
 *  so the Settings toggle and the engineer page can show an HONEST GPU status
 *  and flip the runtime enable flag:
 *
 *    PKernel.nativeGpuAvailable()    -> gpu_available()   (usable RIGHT NOW)
 *    PKernel.nativeGpuName()         -> gpu_name()        ("Adreno (TM) 840")
 *    PKernel.nativeGpuGetEnabled()   -> gpu_get_enabled() (the runtime flag)
 *    PKernel.nativeGpuSetEnabled(b)  -> gpu_set_enabled() (flip + persist)
 *
 *  It does NOT route any inference through the GPU — that is GPU-3. This wave
 *  only surfaces availability + the enable flag. The native functions in
 *  gpu_compute.h are default-visibility exports of the SAME libpkernel.so
 *  (gpu_vk.c); this shim merely adapts them to the JNI calling convention.
 *
 *  COMPATIBILITY: gpu_compute.h's functions are all safe to call at any time
 *  (before gpu_init, on a device with no Vulkan, etc.). gpu_available() lazily
 *  triggers gpu_init() the first time; every entry point here is therefore
 *  crash-free on a Vulkan-less device and simply reports "not available".
 *
 *  We do NOT add libvulkan as a link dependency: gpu_vk.c dlopen()s it at
 *  runtime, so libpkernel.so keeps NO libvulkan DT_NEEDED.
 */

#include <jni.h>

#include "gpu/gpu_compute.h"

JNIEXPORT jboolean JNICALL
Java_io_pkernel_PKernel_nativeGpuAvailable(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return gpu_available() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_io_pkernel_PKernel_nativeGpuName(JNIEnv *env, jclass cls)
{
    (void)cls;
    /* gpu_name() never returns NULL; "" before a successful init. */
    const char *name = gpu_name();
    return (*env)->NewStringUTF(env, name ? name : "");
}

JNIEXPORT jboolean JNICALL
Java_io_pkernel_PKernel_nativeGpuGetEnabled(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return gpu_get_enabled() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_io_pkernel_PKernel_nativeGpuSetEnabled(JNIEnv *env, jclass cls,
                                            jboolean enabled)
{
    (void)env; (void)cls;
    gpu_set_enabled(enabled == JNI_TRUE ? 1 : 0);
}
