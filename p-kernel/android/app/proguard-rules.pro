# UMP proguard rules — release builds only.
#
# Keep the JNI bridge surface so the native side can find Java methods.
-keep class io.pkernel.PKernel { *; }
