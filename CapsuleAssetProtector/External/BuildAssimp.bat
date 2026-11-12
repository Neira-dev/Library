@echo off
rem =============================================
rem BuildAssimp.bat
rem =============================================
rem assimpをCMakeで静的ライブラリとしてビルドします。
rem CapsuleAssetProtectorのビルド前(PreBuildEvent)に自動で呼ばれ、ビルド済みなら何もしません。
rem assimpはサブモジュールとしてリポジトリに含まれているので、別PCでもこのスクリプトだけで再現できます。
rem 引数: %1 = 構成(Debug/Release)  %2 = プラットフォーム(x64/Win32)  %3 = Visual Studioのインストール先
rem =============================================
setlocal

set CONFIG=%~1
set PLATFORM=%~2
set VSROOT=%~3
if "%CONFIG%"=="" set CONFIG=Debug
if "%PLATFORM%"=="" set PLATFORM=x64

set ROOT=%~dp0
set SRC=%ROOT%assimp
set BUILD=%ROOT%build\assimp-%PLATFORM%

rem すでにビルド済みならスキップする
if exist "%BUILD%\lib\%CONFIG%\assimp*.lib" exit /b 0

if not exist "%SRC%\CMakeLists.txt" (
	echo [BuildAssimp] assimpのソースが見つかりません。git submodule update --init を実行してください。
	exit /b 1
)

rem Visual Studio同梱のCMakeを優先して使う(無ければPATH上のcmake)
set CMAKE_EXE=cmake
if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set CMAKE_EXE="%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

rem 外側のMSBuildから引き継いだ環境変数が、内側のビルドに干渉しないようにする
set Platform=
set Configuration=
set MSBUILDDISABLENODEREUSE=1

if /I "%PLATFORM%"=="Win32" (set ARCH=Win32) else (set ARCH=x64)

if not exist "%BUILD%\CMakeCache.txt" (
	echo [BuildAssimp] CMakeの構成を行います: %BUILD%
	%CMAKE_EXE% -S "%SRC%" -B "%BUILD%" -G "Visual Studio 17 2022" -A %ARCH% ^
		-DBUILD_SHARED_LIBS=OFF ^
		-DUSE_STATIC_CRT=OFF ^
		-DASSIMP_BUILD_ZLIB=ON ^
		-DASSIMP_NO_EXPORT=ON ^
		-DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF ^
		-DASSIMP_BUILD_FBX_IMPORTER=ON ^
		-DASSIMP_BUILD_OBJ_IMPORTER=ON ^
		-DASSIMP_BUILD_GLTF_IMPORTER=ON ^
		-DASSIMP_BUILD_ASSIMP_TOOLS=OFF ^
		-DASSIMP_BUILD_TESTS=OFF ^
		-DASSIMP_BUILD_SAMPLES=OFF ^
		-DASSIMP_BUILD_DOCS=OFF ^
		-DASSIMP_INSTALL=OFF ^
		-DASSIMP_INJECT_DEBUG_POSTFIX=ON ^
		-DASSIMP_WARNINGS_AS_ERRORS=OFF ^
		-DASSIMP_IGNORE_GIT_HASH=ON
	if errorlevel 1 exit /b 1
)

echo [BuildAssimp] assimp(%CONFIG%^|%PLATFORM%)をビルドします。初回は数分かかります。
%CMAKE_EXE% --build "%BUILD%" --config %CONFIG% --target assimp --parallel
if errorlevel 1 exit /b 1
exit /b 0
