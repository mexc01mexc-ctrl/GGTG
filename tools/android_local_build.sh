#!/bin/bash
# Local Android APK build (arm64-v8a only), the same Gradle task as .github/workflows/android.yml.
#
#   tools/android_local_build.sh          incremental build -> APP/GGTG-Android.apk
#   tools/android_local_build.sh setup    only install what is missing (cmdline-tools, SDK 36, NDK, CMake)
#
# The compromise between speed and memory:
#   - arm64-v8a only (one native build instead of four: the biggest saving of time and disk)
#   - Gradle build cache + ccache for the NDK part: the second build takes minutes, not tens of minutes
#   - Gradle heap 5g (the R8 step needs it), parallel workers 4; GGTG_ANDROID_HEAP / GGTG_ANDROID_WORKERS override
#   - the Gradle and Kotlin daemons are stopped when the build is over, so nothing keeps holding RAM
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJ="$ROOT/android/Telegram-GGTG"
MODE="${1:-build}"
HEAP="${GGTG_ANDROID_HEAP:-5g}"
WORKERS="${GGTG_ANDROID_WORKERS:-4}"

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"

# --- JDK 17 (as in the CI): newer ones (Android Studio's bundled 21/25) are too new for this project's Gradle
if /usr/libexec/java_home -v 17 >/dev/null 2>&1; then
  export JAVA_HOME="$(/usr/libexec/java_home -v 17)"
else
  echo "Нужна JDK 17:  brew install --cask temurin@17" >&2
  exit 1
fi
export PATH="$JAVA_HOME/bin:$PATH"

# --- SDK pieces (once)
SDKM="$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager"
if [ ! -x "$SDKM" ]; then
  echo "=== Ставим Android cmdline-tools (около 150 МБ) ==="
  TMP="$(mktemp -d)"
  curl -fL -o "$TMP/cmdline.zip" "https://dl.google.com/android/repository/commandlinetools-mac-11076708_latest.zip"
  unzip -q "$TMP/cmdline.zip" -d "$TMP"
  mkdir -p "$ANDROID_HOME/cmdline-tools"
  rm -rf "$ANDROID_HOME/cmdline-tools/latest"
  mv "$TMP/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest"
  rm -rf "$TMP"
fi
yes | "$SDKM" --licenses >/dev/null 2>&1 || true
NEED=()
[ -d "$ANDROID_HOME/platforms/android-36" ]        || NEED+=("platforms;android-36")
[ -d "$ANDROID_HOME/build-tools/36.0.0" ]          || NEED+=("build-tools;36.0.0")
[ -d "$ANDROID_HOME/ndk/27.2.12479018" ]           || NEED+=("ndk;27.2.12479018")
[ -d "$ANDROID_HOME/cmake/3.22.1" ]                || NEED+=("cmake;3.22.1")
if [ ${#NEED[@]} -gt 0 ]; then
  echo "=== Ставим: ${NEED[*]} (NDK около 1.5 ГБ, один раз) ==="
  "$SDKM" "${NEED[@]}"
fi
[ "$MODE" = "setup" ] && { echo "Всё нужное на месте."; exit 0; }

# --- ccache for the native part, when installed
if command -v ccache >/dev/null 2>&1; then
  export NDK_CCACHE="$(command -v ccache)"
  export CMAKE_C_COMPILER_LAUNCHER="$NDK_CCACHE"
  export CMAKE_CXX_COMPILER_LAUNCHER="$NDK_CCACHE"
fi

cd "$PROJ"
chmod +x gradlew
trap './gradlew --stop >/dev/null 2>&1 || true' EXIT
./gradlew --build-cache --parallel \
  -Dorg.gradle.jvmargs="-Xmx$HEAP -XX:MaxMetaspaceSize=1g" \
  -Dkotlin.daemon.jvm.options="-Xmx2g" \
  -Dorg.gradle.caching=true \
  --max-workers="$WORKERS" \
  -PggtgAbis=arm64-v8a \
  -Pandroid.injected.build.abi=arm64-v8a \
  -Pandroid.injected.testOnly=false \
  :TMessagesProj_AppStandalone:assembleAfatStandalone

APK="$(find "$PROJ/TMessagesProj_AppStandalone/build" -name "*.apk" -print0 | xargs -0 ls -t | head -n 1)"
[ -f "$APK" ] || { echo "APK не найден" >&2; exit 1; }
mkdir -p "$ROOT/APP"
cp "$APK" "$ROOT/APP/GGTG-Android.apk"
ls -lh "$ROOT/APP/GGTG-Android.apk"
