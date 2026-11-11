#!/bin/sh
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
chmod +x package_sh.sh
./package_sh.sh
