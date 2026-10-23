#!/bin/bash
# Builds GGTG/APP/GGTG-Android.apk the way that installs on a phone.
#
#   ./build_release.sh
#
# Why this script exists: the arm64-only build needs -Pandroid.injected.build.abi,
# and that flag makes Gradle treat the build as "run from Android Studio" and
# mark the APK android:testOnly="true". A phone refuses such an APK
# (INSTALL_FAILED_TEST_ONLY, "App not installed"). -Pandroid.injected.testOnly=false
# turns the mark off. The script builds with both flags, then CHECKS the result
# and refuses to put a broken APK in place.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# SDK: Homebrew location, or ~/Android/sdk (override with ANDROID_SDK_DIR)
sdk="${ANDROID_SDK_DIR:-/opt/homebrew/share/android-commandlinetools}"
[ -d "$sdk" ] || sdk="$HOME/Android/sdk"
tools="$sdk/build-tools/36.0.0"
export JAVA_HOME="$(ls -d "$here"/jdk/jdk-17*/Contents/Home | head -1)"
# The signature the installed copies on the phones were signed with
# (TMessagesProj/config/release.keystore); a different one would not update them.
expected_cert="a08d7dc323ddf71ef3201944397e0d3cce7d40847263e11f328b68bbe19229ab"

cd "$here/Telegram-GGTG"
./gradlew --no-daemon \
    -Pandroid.injected.build.abi=arm64-v8a \
    -Pandroid.injected.testOnly=false \
    :TMessagesProj_AppStandalone:assembleAfatStandalone

apk="$here/Telegram-GGTG/TMessagesProj_AppStandalone/build/intermediates/apk/afat/standalone/app.apk"
[ -f "$apk" ] || { echo "FAIL: no $apk"; exit 1; }

# 1. not test-only (the cause of "App not installed")
if "$tools/aapt2" dump xmltree --file AndroidManifest.xml "$apk" | grep -q testOnly; then
    echo "FAIL: the APK is marked testOnly - a phone will not install it"; exit 1
fi
# 2. not debuggable, right package
badging="$("$tools/aapt2" dump badging "$apk")"
echo "$badging" | grep -q "application-debuggable" && { echo "FAIL: debuggable"; exit 1; }
echo "$badging" | grep -q "name='org.telegram.messenger.web'" || { echo "FAIL: wrong package"; exit 1; }
# 3. signed with the usual key
cert="$("$tools/apksigner" verify --print-certs "$apk" | grep "SHA-256 digest" | head -1 | awk '{print $NF}')"
[ "$cert" = "$expected_cert" ] || { echo "FAIL: signed with another key ($cert)"; exit 1; }

# GGTG/APP (the old one goes to APP/previous/)
releases="$(cd "$here" && pwd -P)/../APP"
mkdir -p "$releases/previous"
out="$releases/GGTG-Android.apk"
[ -f "$out" ] && cp -f "$out" "$releases/previous/GGTG-Android-prev.apk"
cp -f "$apk" "$out"
echo "OK: $(du -h "$out" | cut -f1) $out"
echo "$badging" | grep -E "^package" | cut -c1-90
echo
echo "Check the install without a phone (emulator, Android 15):"
echo "  $sdk/emulator/emulator -avd nicegram_test -no-window -no-audio &"
echo "  $sdk/platform-tools/adb install -r $out"
