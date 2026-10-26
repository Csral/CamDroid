# CamDroid
CamDroid lets you stream your Android camera as a webcam to your desktop.

## Documentation
Find the documentation [here](http://camdroid.csral.dev/docs).

## External dependencies
* FFMPEG: Provides decoder.
* Qt6: Provides the desktop GUI interface.

## Building from source

### Windows desktop

Install these prerequisites:

- [Git](https://git-scm.com/downloads).
- [CMake](https://cmake.org/download/).
- Visual Studio or Visual Studio Build Tools with **Desktop development with C++**,
  including the MSVC x64 compiler and Windows 11 SDK.
- A [.NET SDK](https://dotnet.microsoft.com/download) for installer generation.
- [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started),
  installed and bootstrapped.

Set `VCPKG_ROOT` to your vcpkg installation directory. For example, in PowerShell:

```powershell
$env:VCPKG_ROOT = 'C:\path\to\vcpkg'
```

From the CamDroid repository root, configure and build:

```powershell
cmake -S desktop -B desktop/build -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build desktop/build --config Release
```

CMake uses `desktop/vcpkg.json` to install FFmpeg, Qt6 and their dependencies.
The packaging script installs WiX 4.0.6 and its extensions automatically.
Initial setup requires internet access.

The Release build also generates the Windows installer and uninstaller.
Installer artifacts are written to `desktop/build/dist/Release/`.

To run the desktop and protocol tests:

```powershell
ctest --test-dir desktop/build -C Release --output-on-failure
```

The virtual webcam requires Windows 11, build 22000 or later.

### Android application

Install Android Studio and a **full JDK 26**, including `javac` and `jlink`.

Using Android Studio’s **SDK Manager**, install:

- Android SDK Platform **37**.
- Android SDK Platform-Tools and Command-line Tools.
- NDK (Side by side) **30.0.16248370**.
- CMake **4.1.2**.

Enable **Show Package Details** to select the required NDK and CMake versions.

Set these environment variables in PowerShell, adjusting the paths to your installations:

```powershell
$env:JAVA_HOME = 'C:\path\to\jdk-26'
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
```

The project requires a locally installed JDK 26 and disables automatic JDK downloads. When using Android Studio, ensure its Gradle JVM criteria also select version 26.

From the repository root, build a debug APK:

```powershell
.\gradlew.bat :app:assembleDebug
```

The APK is written to:

```text
app/build/outputs/apk/debug/app-debug.apk
```

For an unsigned release APK:

```powershell
.\gradlew.bat :app:assembleRelease
```

Release APKs are written to:

```text
app/build/outputs/apk/release/
```

The repository does not configure release signing. To generate an installable signed release, open the project in Android Studio and select **Build > Generate Signed App Bundle or APK**, choose **APK**, and use your own signing keystore.

Keep signing keys and passwords outside the repository. The Gradle wrapper downloads the required Gradle distribution; a separate Gradle installation is unnecessary.

The Android application requires Android 8.0 (API 26) or later.

## Contributing
You can find the guidelines [here](http://camdroid.csral.dev/docs/contributing/).

## License

CamDroid is free and open-source software.

- Android application: GPL-3.0-only
- Desktop application: GPL-3.0-only
- Protocol library: LGPL-3.0-only

See [LICENSE](LICENSE) for details.

## Links
* [Official website](http://camdroid.csral.dev/)
* [Documentation](http://camdroid.csral.dev/docs/)
* [Downloads](http://camdroid.csral.dev/downloads/)